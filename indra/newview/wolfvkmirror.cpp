/**
 * @file wolfvkmirror.cpp
 * @brief Vulkan copies of the OpenGL vertex buffers and textures the Vulkan pass draws with (see wolfvkmirror.h).
 *
 * [VULKAN BACKEND 2026-11-08]
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer Source Code
 * Copyright (C) 2026, IntelligentWolf Ltd
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"
#include "wolfvkmirror.h"
#include "vk/wolfvkdevice.h"
#include "llvertexbuffer.h"
#include "llimagegl.h"
#include "llgl.h"
#include "llthread.h"   // on_main_thread (llthread.h:162)
#include <algorithm>   // std::max in the mip-chain count
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    WolfVkDevice* sDevice = nullptr;
    WolfVkSamplerCache* sSamplers = nullptr;

    struct BufferEntry
    {
        WolfVkVertexBuffer* mBuffer = nullptr;
    };
    std::unordered_map<LLVertexBuffer*, BufferEntry> sBuffers;
    std::unordered_set<LLVertexBuffer*> sDirtyBuffers;

    struct TextureEntry
    {
        WolfVkImage* mImage = nullptr;
        VkSampler mSampler = VK_NULL_HANDLE;
        bool mStale = false;
        uint64_t mBytes = 0;
    };
    std::unordered_map<LLGLuint, TextureEntry> sTextures;
    uint64_t sTextureBytes = 0;
    std::unordered_set<LLGLuint> sWarnedTextures;
    WolfVkMirror::Stats sStats;
    WolfVkStagingRing* sStagingRing = nullptr;

    // [TEXTURE PURGE 2026-11-08 8.2k] LLImageGL's hooks can fire on the texture threads too: createGLTexture/setImage run
    // there when RenderGLMultiThreadedTextures / RenderGLMultiThreadedMedia are on (llimagegl.cpp:1667-1745 "not on main
    // thread" path; both settings default to 0 in settings.xml, so the test sessions never took it). sTextures is the
    // main thread's alone, so a hook off the main thread only queues the name; texture() applies the queue before any
    // lookup, i.e. before the next Vulkan pass can bind a copy of that texture.
    std::mutex sOffThreadMutex;
    std::vector<LLGLuint> sOffThreadStale, sOffThreadDeleted;
    std::atomic<bool> sOffThreadPending{ false };
    void dropTexture(LLGLuint name);
    // [DEVICE LOST 2026-11-08 review] a texture whose copy failed, with the device frame it failed in: not tried again in
    // that frame (each try is a GL read-back of every level plus one-time submits, and every batch using the texture
    // asked again). Entries go when the GL name is deleted or the copy succeeds.
    std::unordered_map<LLGLuint, uint64_t> sFailedInFrame;
    // [POSE CHECK 2026-11-08] buffers the Vulkan pass drew this frame, and GL flushes into them afterwards (same frame)
    std::unordered_set<const LLVertexBuffer*> sPoseDrawn;
    uint64_t sPoseLateFlushes = 0;
    uint32_t sPoseLateLogged = 0;

    // ---- hooks from llrender ------------------------------------------------------------------------------------
    void onBufferFlush(LLVertexBuffer* buffer, bool indices, U32 start, U32 end, const U8* data)
    {
        if (!sPoseDrawn.empty() && sPoseDrawn.count(buffer))
        {
            // a buffer the Vulkan pass has already drawn from this frame gets new bytes now: GL's later passes of this
            // frame draw them, Vulkan drew the old ones (the evidence the pose check is after)
            sPoseLateFlushes++;
            if (sPoseLateLogged < 10)
            {
                sPoseLateLogged++;
                LL_WARNS("WolfVulkanPose") << "GL flush into vertex buffer " << (const void*)buffer << (indices ? " (indices)" : " (vertices)")
                                           << " bytes " << start << "-" << end << " after the Vulkan pass drew from it this frame" << LL_ENDL;
            }
        }
        auto it = sBuffers.find(buffer);
        if (it == sBuffers.end() || !it->second.mBuffer) return;
        WolfVkVertexBuffer* vb = it->second.mBuffer;
        WolfVkBuffer& target = indices ? vb->indices() : vb->vertices();
        if (!target.isValid()) return;
        // flush_vbo's end is inclusive (llvertexbuffer.cpp:1391-1395); the Apple path passes the whole buffer with end 0
        // (its glBufferData of mSize), so "end 0" means the lot.
        const VkDeviceSize size = (end >= start && end != 0) ? static_cast<VkDeviceSize>(end - start + 1) : target.size() - start;
        if (static_cast<VkDeviceSize>(start) + size > target.size()) return;
        target.write(start, data, size);
        sStats.mBufferBytesWritten += size;
        sDirtyBuffers.insert(buffer);
    }

    void onBufferDestroy(LLVertexBuffer* buffer)
    {
        sPoseDrawn.erase(buffer);
        auto it = sBuffers.find(buffer);
        if (it == sBuffers.end()) return;
        delete it->second.mBuffer;   // its destroy() defers the Vulkan release
        sBuffers.erase(it);
        sDirtyBuffers.erase(buffer);
    }

    void onTexelsChanged(LLGLuint name)
    {
        if (!on_main_thread())
        {
            std::lock_guard<std::mutex> lock(sOffThreadMutex);
            sOffThreadStale.push_back(name);
            sOffThreadPending = true;
            return;
        }
        auto it = sTextures.find(name);
        if (it != sTextures.end()) it->second.mStale = true;
    }

    // The Vulkan copy of a GL texture whose name is being deleted (LLImageGL::deleteTextures, llimagegl.cpp:1313-1324,
    // before the name joins sFreeList; glDeleteTextures follows at a later LLImageGL::updateClass, llimagegl.cpp:1296-1310,
    // so the name cannot be handed out again by glGenTextures while this entry exists). WolfVkImage::destroy (wolfvkimage.cpp:395-415)
    // files the view, image and memory with WolfVkDevice::deferDestroy, which runs them when the slot of the last submitted
    // frame - or of the frame being recorded - comes round again, after its fence/timeline wait; every descriptor set that
    // named the view lives in that slot's pools, which WolfVkBridge::ensureFrameBegun resets (sScene->beginFrame) BEFORE
    // flushDeferred. So no descriptor can name freed memory, whatever the texture purge deletes mid-frame.
    void dropTexture(LLGLuint name)
    {
        sFailedInFrame.erase(name);
        auto it = sTextures.find(name);
        if (it == sTextures.end()) return;
        sTextureBytes -= it->second.mBytes;
        delete it->second.mImage;
        sTextures.erase(it);
        sStats.mTexturesDropped++;
    }

    void onTextureDelete(LLGLuint name)
    {
        if (!on_main_thread())
        {
            std::lock_guard<std::mutex> lock(sOffThreadMutex);
            sOffThreadDeleted.push_back(name);
            sOffThreadPending = true;
            return;
        }
        dropTexture(name);
    }

    void applyOffThreadHooks()
    {
        if (!sOffThreadPending) return;
        std::vector<LLGLuint> stale, deleted;
        {
            std::lock_guard<std::mutex> lock(sOffThreadMutex);
            stale.swap(sOffThreadStale);
            deleted.swap(sOffThreadDeleted);
            sOffThreadPending = false;
        }
        for (LLGLuint name : stale)
        {
            auto it = sTextures.find(name);
            if (it != sTextures.end()) it->second.mStale = true;
        }
        for (LLGLuint name : deleted) dropTexture(name);
    }

    // ---- GL state save/restore for the read-backs (as wolfvrscreen.cpp captureInto saves the bindings it touches)
    struct GLBufferBindings
    {
        GLint mArray = 0, mElement = 0;
        GLBufferBindings()
        {
            glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &mArray);
            glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &mElement);
        }
        ~GLBufferBindings()
        {
            glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(mArray));
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLuint>(mElement));
        }
    };

    bool mirrorBuffer(LLVertexBuffer* buffer, WolfVkVertexBuffer*& out)
    {
        out = nullptr;
        const U32 gl_vbo = buffer->getGLBuffer();
        const U32 gl_ibo = buffer->getGLIndices();
        if (gl_vbo == 0 && gl_ibo == 0) return false;
        std::string error;
        WolfVkVertexBuffer* vb = new WolfVkVertexBuffer();
        vb->init(*sDevice, buffer->getTypeMask());
        if (!vb->allocateBuffer(buffer->getNumVerts(), buffer->getNumIndices(), error))
        {
            LL_WARNS_ONCE("Vulkan") << "vertex buffer mirror: " << error << LL_ENDL;
            delete vb;
            return false;
        }
        // The GL buffer holds what GL draws: copy it (glGetBufferSubData, OpenGL 4.6 spec 6.3 "Buffer Object Queries") into the
        // Vulkan copy's host bytes and mark the whole range for upload. LLVertexBuffer's own bind bookkeeping (sGLRenderBuffer)
        // is left as it was: the previous bindings are restored on return.
        GLBufferBindings saved;
        // the copy must have exactly GL's shape, or its contents would be guesses: a mismatch is a failure, not a copy
        if ((gl_vbo != 0 && buffer->getSize() != vb->getSize()) || (gl_ibo != 0 && buffer->getIndicesSize() != vb->getIndicesSize()))
        {
            LL_WARNS_ONCE("Vulkan") << "vertex buffer mirror: size differs from GL's (" << buffer->getSize() << "/" << vb->getSize() << " bytes, indices "
                                    << buffer->getIndicesSize() << "/" << vb->getIndicesSize() << "); not mirrored" << LL_ENDL;
            delete vb;
            return false;
        }
        if (gl_vbo != 0 && vb->vertices().isValid())
        {
            glBindBuffer(GL_ARRAY_BUFFER, gl_vbo);
            glGetBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(vb->getSize()), vb->vertices().hostData());
            vb->vertices().markDirty(0, vb->getSize());
        }
        if (gl_ibo != 0 && vb->indices().isValid())
        {
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl_ibo);
            glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(vb->getIndicesSize()), vb->indices().hostData());
            vb->indices().markDirty(0, vb->getIndicesSize());
        }
        if (glGetError() != GL_NO_ERROR)
        {
            LL_WARNS_ONCE("Vulkan") << "vertex buffer mirror: glGetBufferSubData failed" << LL_ENDL;
        }
        out = vb;
        return true;
    }

    uint32_t glPixelFormatForChannels(uint32_t channels)
    {
        switch (channels)
        {
        case 1: return GL_RED;
        case 2: return GL_RG;
        case 3: return GL_RGB;
        default: return GL_RGBA;
        }
    }

    // [T766 2026-11-10] GL's texture swizzle, carried into the Vulkan copy's view.
    // Source: llimagegl.cpp:1340-1360 setManualImage() - on the core profile a 1-channel image (GL_LUMINANCE) is stored as
    // GL_R8 with GL_TEXTURE_SWIZZLE_RGBA (R,R,R,ONE), GL_ALPHA as GL_R8 with (ZERO,ZERO,ZERO,R), GL_LUMINANCE_ALPHA as GL_RG8
    // with (R,R,R,G). GL_TEXTURE_INTERNAL_FORMAT then reads back as plain GL_R8 / GL_RG8, and formatFor() gives those an
    // identity view (wolfvkimage.cpp:106-112), so a white greyscale texture sampled as (1,0,0,1): the grid's "Blank Texture"
    // 5748decc-f629-461c-9a36-a35a221fe21f is a 1-component 512x512 J2C (Csiz 1 in its SIZ marker), and a box set to Blank
    // showed red in the Vulkan-drawn deferred diffuse pass (ticket 766, W103 on Windows).
    // The swizzle is a texture parameter (OpenGL 4.6 core spec 8.10 "Texture Parameters", TEXTURE_SWIZZLE_RGBA, read back
    // with glGetTexParameteriv): each output component names a component of the texture's own RGBA (RED/GREEN/BLUE/ALPHA)
    // or a constant (ZERO/ONE). In the Vulkan copy the texture's own RGBA is what format.mSwizzle already makes of the stored
    // channels, so GL's swizzle is composed on top of it.
    void applyGLSwizzle(WolfVkImage::Format& format)
    {
        GLint gl[4] = { GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA };   // GL's default swizzle
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, gl);
        if (gl[0] == GL_RED && gl[1] == GL_GREEN && gl[2] == GL_BLUE && gl[3] == GL_ALPHA) return;
        // the texture's own component i in the copy: format.mSwizzle's entry, IDENTITY meaning channel i itself
        // (Vulkan spec VkComponentMapping)
        const VkComponentSwizzle base[4] = {
            format.mSwizzle.r == VK_COMPONENT_SWIZZLE_IDENTITY ? VK_COMPONENT_SWIZZLE_R : format.mSwizzle.r,
            format.mSwizzle.g == VK_COMPONENT_SWIZZLE_IDENTITY ? VK_COMPONENT_SWIZZLE_G : format.mSwizzle.g,
            format.mSwizzle.b == VK_COMPONENT_SWIZZLE_IDENTITY ? VK_COMPONENT_SWIZZLE_B : format.mSwizzle.b,
            format.mSwizzle.a == VK_COMPONENT_SWIZZLE_IDENTITY ? VK_COMPONENT_SWIZZLE_A : format.mSwizzle.a };
        VkComponentSwizzle out[4];
        for (int c = 0; c < 4; ++c)
        {
            switch (gl[c])
            {
            case GL_RED:   out[c] = base[0]; break;
            case GL_GREEN: out[c] = base[1]; break;
            case GL_BLUE:  out[c] = base[2]; break;
            case GL_ALPHA: out[c] = base[3]; break;
            case GL_ZERO:  out[c] = VK_COMPONENT_SWIZZLE_ZERO; break;
            case GL_ONE:   out[c] = VK_COMPONENT_SWIZZLE_ONE; break;
            default:       out[c] = base[c]; break;   // GL accepts no other value; keep the component as it was
            }
        }
        format.mSwizzle = { out[0], out[1], out[2], out[3] };
    }

    bool mirrorTexture(LLImageGL* image, TextureEntry& entry)
    {
        const LLGLuint name = image->getTexName();
        if (name == 0) return false;
        // [DEVICE LOST 2026-11-08] nothing is copied for a hung or lost device: its submits are refused anyway, and the GL
        // read-backs before them would be wasted (nv-hang1 kept mirroring after the loss until the layer crashed)
        if (sDevice->hung()) return false;
        {
            auto f = sFailedInFrame.find(name);
            if (f != sFailedInFrame.end() && f->second == sDevice->frameCounter()) return false;
        }
        // the copy is attempted now; it counts as failed in this frame unless it completes (erased at the end)
        sFailedInFrame[name] = sDevice->frameCounter();
        if (image->getTarget() != LLTexUnit::TT_TEXTURE)
        {
            if (!sWarnedTextures.count(name))
            {
                LL_WARNS("Vulkan") << "texture mirror: only 2D textures are mirrored in this cut (texture " << name << " has type " << image->getTarget() << ")" << LL_ENDL;
                sWarnedTextures.insert(name);
            }
            return false;
        }
        GLint saved_binding = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &saved_binding);
        glBindTexture(GL_TEXTURE_2D, name);

        // level 0 shape and format: OpenGL 4.6 spec 8.11.3 "Texture Level Parameter Queries" (glGetTexLevelParameteriv)
        GLint width = 0, height = 0, internal = 0, compressed = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &internal);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_COMPRESSED, &compressed);
        GLint max_level = 1000;
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, &max_level);
        if (width <= 0 || height <= 0)
        {
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(saved_binding));
            return false;
        }
        // the data type GL will hand back, from the internal format's precision
        uint32_t pixel_type = GL_UNSIGNED_BYTE;
        switch (static_cast<uint32_t>(internal))
        {
        case GL_RGBA16F: case GL_RGB16F: case GL_R16F: case GL_RG16F: pixel_type = GL_HALF_FLOAT; break;
        case GL_RGBA32F: case GL_R32F: pixel_type = GL_FLOAT; break;
        case GL_DEPTH_COMPONENT: case GL_DEPTH_COMPONENT24: case GL_DEPTH_COMPONENT32F: pixel_type = GL_FLOAT; break;
        default: break;
        }
        WolfVkImage::Format format = WolfVkImage::formatFor(static_cast<uint32_t>(internal), pixel_type);
        if (!format.valid())
        {
            if (!sWarnedTextures.count(name))
            {
                LL_WARNS("Vulkan") << "texture mirror: no Vulkan format for GL internal format 0x" << std::hex << internal << std::dec << " (texture " << name << ")" << LL_ENDL;
                sWarnedTextures.insert(name);
            }
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(saved_binding));
            return false;
        }
        // [T766 2026-11-10] sample what GL samples: a greyscale (GL_R8 + swizzle) texture as grey, not red
        if (!format.mDepth) applyGLSwizzle(format);
        // count the levels of GL's mipmap chain that follow level 0. "width > 0" alone is not enough: when the texture
        // purge (llviewertexture.cpp:618 TextureDiscardBackgroundedTime) re-specifies level 0 smaller, GL keeps the older,
        // larger levels above it (run nv-hang1 19:09:09Z: level 0 8x8, level 4 still 16 wide). Counting those made a
        // VkImage with mipLevels 6 for 8x8 (VUID-VkImageCreateInfo-mipLevels-00958) and a 16x16 copy into its 1x1 level
        // (VUID-vkCmdCopyBufferToImage-imageSubresource-07971) -> out-of-bounds GPU write -> VK_ERROR_DEVICE_LOST 1 s later.
        // Source: OpenGL 4.6 core spec 8.14.4 "Mipmapping" (level i is max(1, floor(w0 / 2^i)) x max(1, floor(h0 / 2^i));
        // a level that breaks that is outside the complete chain); Vulkan spec VkImageCreateInfo mipLevels
        // <= floor(log2(max(width, height))) + 1, which this loop cannot exceed since it stops at 1x1.
        uint32_t levels = 0;
        for (GLint level = 0; level <= max_level && level < 16; ++level)
        {
            GLint w = 0, h = 0;
            glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_WIDTH, &w);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_HEIGHT, &h);
            if (w != std::max(1, width >> level) || h != std::max(1, height >> level)) break;
            levels++;
            if (w == 1 && h == 1) break;
        }
        if (levels == 0) levels = 1;

        std::string error;
        WolfVkImage* img = new WolfVkImage();
        // TRANSFER_SRC as well: the debug probe (WolfVulkanDebugProbe) reads mirrored textures back
        if (!img->create(*sDevice, WolfVkImage::Kind::TEXTURE_2D, format, static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1, levels,
                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, false, error))
        {
            LL_WARNS_ONCE("Vulkan") << "texture mirror: " << error << LL_ENDL;
            delete img;
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(saved_binding));
            return false;
        }
        uint64_t bytes = 0;
        std::vector<uint8_t> data;
        GLint saved_pack = 0;
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &saved_pack);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);   // read into client memory
        glPixelStorei(GL_PACK_ALIGNMENT, 4);     // the default; upload() expects rows aligned to 4 as glTexImage2D's unpack does
        // [TEXTURE PURGE 2026-11-08] every counted level must arrive; a copy with a level missing (GL read or upload
        // failed) is not kept - its contents in that level would be undefined
        uint32_t uploaded = 0;
        for (uint32_t level = 0; level < levels; ++level)
        {
            GLint w = 0, h = 0;
            glGetTexLevelParameteriv(GL_TEXTURE_2D, static_cast<GLint>(level), GL_TEXTURE_WIDTH, &w);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, static_cast<GLint>(level), GL_TEXTURE_HEIGHT, &h);
            if (w <= 0 || h <= 0) break;
            if (compressed)
            {
                GLint size = 0;
                glGetTexLevelParameteriv(GL_TEXTURE_2D, static_cast<GLint>(level), GL_TEXTURE_COMPRESSED_IMAGE_SIZE, &size);
                if (size <= 0) break;
                data.resize(static_cast<size_t>(size));
                // OpenGL 4.6 spec 8.11.4 "Texture Image Queries": glGetCompressedTexImage returns the compressed blocks
                glGetCompressedTexImage(GL_TEXTURE_2D, static_cast<GLint>(level), data.data());
            }
            else
            {
                const uint32_t channels = format.mSourceChannels;
                const uint32_t bpc = format.mSourceBytesPerChannel;
                const uint32_t row = (static_cast<uint32_t>(w) * channels * bpc + 3u) & ~3u;
                data.resize(static_cast<size_t>(row) * static_cast<uint32_t>(h));
                glGetTexImage(GL_TEXTURE_2D, static_cast<GLint>(level), format.mDepth ? GL_DEPTH_COMPONENT : glPixelFormatForChannels(channels), pixel_type, data.data());
            }
            if (glGetError() != GL_NO_ERROR)
            {
                LL_WARNS_ONCE("Vulkan") << "texture mirror: glGetTexImage failed for texture " << name << " level " << level << LL_ENDL;
                break;
            }
            if (!img->upload(VK_NULL_HANDLE, level, 0, static_cast<uint32_t>(w), static_cast<uint32_t>(h), data.data(), pixel_type, false, error))
            {
                LL_WARNS_ONCE("Vulkan") << "texture mirror: " << error << LL_ENDL;
                break;
            }
            bytes += data.size();
            uploaded++;
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(saved_pack));
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(saved_binding));
        if (uploaded != levels)
        {
            delete img;   // its destroy is deferred by frame slot like any other
            return false;
        }

        // readable by the fragment shaders from now on.
        // [TEXTURE PURGE 2026-11-08 8.2k] transition() notes SHADER_READ_ONLY when it records; if that one-time submit
        // then failed or timed out (a hung device), the copy would be sampled in a layout it never reached. It is
        // dropped instead (its destroy is deferred like any other), and the batch samples the white fallback.
        VkCommandBuffer cmd = sDevice->beginOneTime();
        if (cmd == VK_NULL_HANDLE)
        {
            delete img;
            return false;
        }
        img->transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        if (!sDevice->endOneTimeAndWait(cmd, error))
        {
            LL_WARNS_ONCE("Vulkan") << "texture mirror: " << error << LL_ENDL;
            delete img;
            return false;
        }
        // the sampler LLTexUnit would set for this image (llrender.cpp:474-553): its filter option and address mode
        entry.mImage = img;
        entry.mSampler = sSamplers->get(static_cast<WolfVkSamplerCache::Filter>(image->getFilteringOption()),
                                        static_cast<WolfVkSamplerCache::Address>(image->getAddressMode()), levels > 1, false);
        entry.mBytes = bytes;   // the bytes read back and uploaded for every level (texture() adds them to sTextureBytes)
        entry.mStale = false;
        sFailedInFrame.erase(name);
        return true;
    }
}

namespace WolfVkMirror
{

void init(WolfVkDevice& device, WolfVkSamplerCache& samplers)
{
    sDevice = &device;
    sSamplers = &samplers;
    LLVertexBuffer::sWolfVkOnFlush = onBufferFlush;
    LLVertexBuffer::sWolfVkOnDestroy = onBufferDestroy;
    LLImageGL::sWolfVkOnTexelsChanged = onTexelsChanged;
    LLImageGL::sWolfVkOnDelete = onTextureDelete;
}

void shutdown()
{
    LLVertexBuffer::sWolfVkOnFlush = nullptr;
    LLVertexBuffer::sWolfVkOnDestroy = nullptr;
    LLImageGL::sWolfVkOnTexelsChanged = nullptr;
    LLImageGL::sWolfVkOnDelete = nullptr;
    for (auto& kv : sBuffers) delete kv.second.mBuffer;
    sBuffers.clear();
    sDirtyBuffers.clear();
    for (auto& kv : sTextures) delete kv.second.mImage;
    sTextures.clear();
    sTextureBytes = 0;
    {
        std::lock_guard<std::mutex> lock(sOffThreadMutex);
        sOffThreadStale.clear();
        sOffThreadDeleted.clear();
        sOffThreadPending = false;
    }
    sFailedInFrame.clear();
    sPoseDrawn.clear();
    sWarnedTextures.clear();
    sDevice = nullptr;
    sSamplers = nullptr;
}

WolfVkVertexBuffer* vertexBuffer(LLVertexBuffer* buffer)
{
    if (!sDevice || !buffer) return nullptr;
    auto it = sBuffers.find(buffer);
    if (it != sBuffers.end())
    {
        // [DEVICE LOST 2026-11-08 16:27Z] LLVertexBuffer::allocateBuffer re-sizes a buffer in place (new GL buffer object,
        // new vertex/index counts, same LLVertexBuffer); a copy made for the old size then has fewer vertices and
        // indices than the draws ask for, and an out-of-range fetch on the card is a device loss. A copy whose
        // shape no longer matches is remade here; the old one goes when the frames using it are done.
        WolfVkVertexBuffer* vb = it->second.mBuffer;
        if (vb && (vb->getNumVerts() != buffer->getNumVerts() || vb->getNumIndices() != buffer->getNumIndices()
                   || vb->getSize() != buffer->getSize() || vb->getIndicesSize() != buffer->getIndicesSize()))
        {
            delete vb;
            sBuffers.erase(it);
            sDirtyBuffers.erase(buffer);
            sStats.mBuffersRemade++;
        }
        else
        {
            return vb;
        }
    }
    WolfVkVertexBuffer* vb = nullptr;
    if (!mirrorBuffer(buffer, vb))
    {
        return nullptr;
    }
    sBuffers[buffer].mBuffer = vb;
    sDirtyBuffers.insert(buffer);
    sStats.mBuffersMade++;
    return vb;
}

WolfVkVertexBuffer* existingVertexBuffer(LLVertexBuffer* buffer)
{
    auto it = sBuffers.find(buffer);
    return it == sBuffers.end() ? nullptr : it->second.mBuffer;
}

void poseNoteDrawn(LLVertexBuffer* buffer) { sPoseDrawn.insert(buffer); }
void poseFrameEnd() { sPoseDrawn.clear(); }
uint64_t poseLateFlushes() { return sPoseLateFlushes; }

const Stats& stats()
{
    return sStats;
}

void recordUploads(VkCommandBuffer cmd)
{
    for (LLVertexBuffer* b : sDirtyBuffers)
    {
        auto it = sBuffers.find(b);
        if (it != sBuffers.end() && it->second.mBuffer)
        {
            it->second.mBuffer->recordUploads(cmd, sStagingRing);
        }
    }
    sDirtyBuffers.clear();
}

void setStagingRing(WolfVkStagingRing* ring)
{
    sStagingRing = ring;
}

Texture texture(LLImageGL* image, VkCommandBuffer /*cmd*/)
{
    Texture t;
    if (!sDevice || !image || image->getTexName() == 0) return t;
    applyOffThreadHooks();
    const LLGLuint name = image->getTexName();
    auto it = sTextures.find(name);
    if (it != sTextures.end() && !it->second.mStale)
    {
        t.mImage = it->second.mImage;
        t.mSampler = it->second.mSampler;
        return t;
    }
    const bool remade = it != sTextures.end();
    if (remade)
    {
        sTextureBytes -= it->second.mBytes;
        delete it->second.mImage;
        sTextures.erase(it);
    }
    TextureEntry entry;
    if (!mirrorTexture(image, entry))
    {
        return t;
    }
    sStats.mTexturesMade++;
    if (remade) sStats.mTexturesRemade++;
    sTextures[name] = entry;
    sTextureBytes += entry.mBytes;
    t.mImage = entry.mImage;
    t.mSampler = entry.mSampler;
    return t;
}

uint32_t vertexBufferCount() { return static_cast<uint32_t>(sBuffers.size()); }
uint32_t textureCount() { return static_cast<uint32_t>(sTextures.size()); }
uint64_t textureBytes() { return sTextureBytes; }

} // namespace WolfVkMirror
