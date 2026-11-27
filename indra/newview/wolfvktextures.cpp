/**
 * @file wolfvktextures.cpp
 * @brief The Vulkan-owned memory of the textures OpenGL code holds by a raw name (see wolfvktextures.h).
 *
 * [VK S6 2026-11-26]
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
#include "wolfvktextures.h"
#include "wolfvkshare.h"
#include "vk/wolfvkdevice.h"
#include "vk/wolfvkimage.h"
#include "llimagegl.h"
#include "llgl.h"
#include "llglheaders.h"
#include <memory>
#include <set>
#include <unordered_map>
#include <utility>

namespace
{
    WolfVkDevice* sDevice = nullptr;
    // name -> image. Entries outlive shutdown(): the OpenGL texture and memory object of each are still deleted here.
    // Never destroyed (made on first use, left to the process's end): a texture still alive when the viewer exits is
    // not deleted from a static destructor, after the OpenGL context and wolfvkshare.cpp's own tables are gone.
    typedef std::unordered_map<U32, std::unique_ptr<WolfVkSharedImage>> TextureMap;
    TextureMap& textures() { static TextureMap* map = new TextureMap(); return *map; }
    std::unordered_map<U32, uint64_t> sBytes;
    struct Shape { U32 mFormat; S32 mWidth, mHeight, mDepth, mLevels; };   // as asked for in createStorage
    std::unordered_map<U32, Shape> sShapes;
    std::set<std::pair<U32, U32>> sRefusedLogged;   // (target, internal format) already named in the log
    WolfVkTextures::Stats sStats;

    // The sized formats shared as they are: OpenGL's storage format and the Vulkan format are the same bits
    // (WolfVkImage::formatFor; probed both ways 2026-11-26: RGBA8, SRGB8_ALPHA8, R8, RGBA16F, RGBA32F, R11F_G11F_B10F,
    // DEPTH_COMPONENT32F - the others are the same table's one- and two-channel rows and are accepted or refused by
    // the driver's own answer below).
    bool sharedAsIs(U32 internal_format)
    {
        switch (internal_format)
        {
        case GL_RGBA8: case GL_SRGB8_ALPHA8: case GL_R8: case GL_RG8: case GL_R16F: case GL_RG16F: case GL_RGBA16F:
        case GL_RGBA16: case GL_R32F: case GL_RG32F: case GL_RGBA32F: case GL_R11F_G11F_B10F: case GL_DEPTH_COMPONENT32F:
            return true;
        default:
            return false;
        }
    }

    void refuse(U32 target, U32 internal_format, const std::string& why)
    {
        sStats.mRefused++;
        if (sRefusedLogged.insert({ target, internal_format }).second)
        {
            LL_WARNS("Vulkan") << llformat("Texture owner: target 0x%x format 0x%x stays an OpenGL texture: ", target, internal_format) << why << LL_ENDL;
        }
    }

    // GL_TEXTURE_SWIZZLE_A = GL_ONE on a texture just made (glcorearb.h:1601 GL_TEXTURE_SWIZZLE_A 0x8E45): the
    // four-channel storage then samples as the three-channel format asked for (alpha one). The binding of the active
    // unit is put back, so LLTexUnit's own record of it still holds.
    void setAlphaOne(U32 target, U32 name)
    {
        GLenum binding_query = GL_TEXTURE_BINDING_2D;
        switch (target)
        {
        case GL_TEXTURE_CUBE_MAP:       binding_query = GL_TEXTURE_BINDING_CUBE_MAP; break;
        case GL_TEXTURE_2D_ARRAY:       binding_query = GL_TEXTURE_BINDING_2D_ARRAY; break;
        case GL_TEXTURE_CUBE_MAP_ARRAY: binding_query = GL_TEXTURE_BINDING_CUBE_MAP_ARRAY; break;
        case GL_TEXTURE_3D:             binding_query = GL_TEXTURE_BINDING_3D; break;
        default: break;
        }
        GLint saved = 0;
        glGetIntegerv(binding_query, &saved);
        glBindTexture(target, name);
        glTexParameteri(target, GL_TEXTURE_SWIZZLE_A, GL_ONE);
        glBindTexture(target, static_cast<GLuint>(saved));
    }

    class Owner final : public LLImageGL::WolfVkTextureOwner
    {
    public:
        U32 createStorage(U32 target, S32 levels, U32 internal_format, S32 width, S32 height, S32 depth, bool gl_renders) override
        {
            if (!sDevice || !sDevice->hasDevice() || sDevice->hung()) return 0;
            if (levels < 1 || width < 1 || height < 1) return 0;
            WolfVkImage::Kind kind = WolfVkImage::Kind::TEXTURE_2D;
            uint32_t layers = 1;
            switch (target)
            {
            case GL_TEXTURE_2D:             kind = WolfVkImage::Kind::TEXTURE_2D; layers = 1; break;
            case GL_TEXTURE_CUBE_MAP:       kind = WolfVkImage::Kind::TEXTURE_CUBE; layers = 6; break;
            case GL_TEXTURE_2D_ARRAY:       kind = WolfVkImage::Kind::TEXTURE_2D_ARRAY; layers = static_cast<uint32_t>(llmax(depth, 0)); break;
            case GL_TEXTURE_CUBE_MAP_ARRAY: kind = WolfVkImage::Kind::TEXTURE_CUBE_ARRAY; layers = static_cast<uint32_t>(llmax(depth, 0)); break;
            case GL_TEXTURE_3D:             kind = WolfVkImage::Kind::TEXTURE_3D; layers = static_cast<uint32_t>(llmax(depth, 0)); break;
            default:
                refuse(target, internal_format, "not a target the owner knows");
                return 0;
            }
            if (layers == 0 || (kind == WolfVkImage::Kind::TEXTURE_CUBE_ARRAY && (layers % 6u) != 0))
            {
                refuse(target, internal_format, "no layers (a cube map array needs a multiple of six layer-faces)");
                return 0;
            }
            // GL_RGB8 / GL_RGB16F: Vulkan's three-channel formats are not exportable (VK_FORMAT_R16G16B16_SFLOAT:
            // VK_ERROR_FORMAT_NOT_SUPPORTED on NVIDIA 615.78, probe 2026-11-26 case c), so the storage has four
            // channels and alpha is sampled as one on both sides (llimagegl.h WolfVkTextureOwner)
            U32 storage_format = internal_format;
            bool alpha_one = false;
            if (internal_format == GL_RGB8) { storage_format = GL_RGBA8; alpha_one = true; }
            else if (internal_format == GL_RGB16F) { storage_format = GL_RGBA16F; alpha_one = true; }
            if (!sharedAsIs(storage_format))
            {
                refuse(target, internal_format, "not a format with one exact Vulkan equivalent");
                return 0;
            }
            // the pixel type only chooses between the rows of an UNSIZED format; these are all sized
            WolfVkImage::Format format = WolfVkImage::formatFor(storage_format, GL_UNSIGNED_BYTE);
            if (!format.valid() || format.mExpandRGB || format.mBlockBytes != 0)
            {
                refuse(target, internal_format, "no Vulkan format");
                return 0;
            }
            if (alpha_one) format.mSwizzle.a = VK_COMPONENT_SWIZZLE_ONE;   // vulkan_core.h VkComponentSwizzle
            // sampled by both; copied into and out of (uploads, mip generation, read-backs); rendered into when OpenGL
            // does that today, so that the Vulkan pass replacing it can as well (vulkan_core.h VkImageUsageFlagBits)
            VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            if (gl_renders) usage |= format.mDepth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            std::unique_ptr<WolfVkSharedImage> shared(new WolfVkSharedImage());
            std::string error;
            if (!shared->createKind(*sDevice, kind, format, static_cast<uint32_t>(width), static_cast<uint32_t>(height), layers,
                                    static_cast<uint32_t>(levels), usage, storage_format, error))
            {
                refuse(target, internal_format, error);
                return 0;
            }
            // OpenGL uses it from here on: the layout of its main use (EXT_external_objects table 4.4)
            const VkImageLayout hand = format.mDepth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                     : gl_renders ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            if (!shared->handToGL(hand, error))
            {
                refuse(target, internal_format, "first hand-over: " + error);
                return 0;   // shared goes: its texture, memory object and image with it
            }
            const U32 name = shared->glTexture();
            if (alpha_one) setAlphaOne(target, name);
            const uint64_t bytes = shared->image() ? shared->image()->allocationSize() : 0;
            sBytes[name] = bytes;
            sShapes[name] = { internal_format, width, height, depth, levels };
            sStats.mBytes += bytes;
            sStats.mMade++;
            textures()[name] = std::move(shared);
            return name;
        }

        bool destroyStorage(U32 name) override
        {
            auto it = textures().find(name);
            if (it == textures().end()) return false;
            auto bytes = sBytes.find(name);
            if (bytes != sBytes.end())
            {
                sStats.mBytes -= llmin(sStats.mBytes, bytes->second);
                sBytes.erase(bytes);
            }
            sShapes.erase(name);
            textures().erase(it);   // ~WolfVkSharedImage: the OpenGL texture and memory object now, the Vulkan image deferred
            return true;
        }

        bool describe(U32 name, U32& internal_format, S32& width, S32& height, S32& depth, S32& levels) override
        {
            auto it = sShapes.find(name);
            if (it == sShapes.end()) return false;
            internal_format = it->second.mFormat;
            width = it->second.mWidth;
            height = it->second.mHeight;
            depth = it->second.mDepth;
            levels = it->second.mLevels;
            return true;
        }
    };

    Owner sOwner;
}

namespace WolfVkTextures
{

bool init(WolfVkDevice& device, std::string& why)
{
#if LL_DARWIN
    (void)device;
    why = "Apple's OpenGL has no GL_EXT_memory_object: raw-name textures stay OpenGL's until the whole frame is drawn by Vulkan";
    return false;
#else
    if (!WolfVkShare::available(device, why)) return false;
    sDevice = &device;
    sStats.mMade = 0;
    sStats.mRefused = 0;
    sRefusedLogged.clear();
    LLImageGL::sWolfVkTextureOwner = &sOwner;
    LL_INFOS("Vulkan") << "Texture owner started: textures held by an OpenGL name are made in Vulkan memory that OpenGL imports" << LL_ENDL;
    return true;
#endif
}

void shutdown()
{
    if (!sDevice) return;
    // the owner stays installed: it makes nothing without a device, and the names it holds are still deleted through it
    for (auto& entry : textures()) entry.second->releaseVulkanHalf();
    sDevice = nullptr;
}

bool active() { return sDevice != nullptr; }

WolfVkSharedImage* find(uint32_t gl_name)
{
    if (!sDevice) return nullptr;
    auto it = textures().find(gl_name);
    return (it != textures().end() && it->second->isValid()) ? it->second.get() : nullptr;
}

Stats stats()
{
    Stats s = sStats;
    s.mTextures = static_cast<uint32_t>(textures().size());
    return s;
}

} // namespace WolfVkTextures
