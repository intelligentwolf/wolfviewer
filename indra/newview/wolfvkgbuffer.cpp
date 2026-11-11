/**
 * @file wolfvkgbuffer.cpp
 * @brief The G-buffer shared between OpenGL and Vulkan (see wolfvkgbuffer.h).
 *
 * [VULKAN G-BUFFER 2026-11-08]
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
#include "wolfvkgbuffer.h"
#include "wolfvkbridge.h"   // [AMD PERF 2026-11-09] timingOn()
#include "llgl.h"
#include "llglheaders.h"
#include <fstream>
#if LL_SDL
#include "SDL2/SDL_video.h"   // SDL_GL_GetProcAddress
#elif LL_WINDOWS
#include <windows.h>
#endif

namespace
{
    // GL/glext.h tokens (lines in wolfvkshare.h)
    const GLenum WOLF_GL_LAYOUT_COLOR_ATTACHMENT_EXT = 0x958E;
    const GLenum WOLF_GL_LAYOUT_DEPTH_STENCIL_ATTACHMENT_EXT = 0x958F;
    const GLenum WOLF_GL_HANDLE_TYPE_OPAQUE_FD_EXT = 0x9586;
    const GLenum WOLF_GL_HANDLE_TYPE_OPAQUE_WIN32_EXT = 0x9587;

    // [VINTAGE G-BUFFER 2026-11-10] The attachments as shared, by what the viewer asks for (pipeline.cpp
    // allocateScreenBufferInternal / addDeferredAttachments:394-420): the normal is GL_RGBA16 with HDR on and GL_RGB10_A2
    // without (RenderHDREnabled off, which graphics quality Low/LowMid/Mid set through RenderDisableVintageMode 0,
    // featuretable.txt:140/185/411-459 -> llviewercontrol.cpp handleDisableVintageMode); the emissive attachment is
    // GL_RGB16F with HDR on, GL_RGB without, and is absent altogether when RenderEnableEmissiveBuffer is off (the same
    // levels) - three colour attachments then. Before this, index 2 was only ever GL_RGBA16: a vintage-mode G-buffer
    // got attachments 0, 1 and the depth shared and the normal refused - half shared, which the init comment below
    // calls worse than none, and no Vulkan pass ever ran (prepare() wanted four). Found in the W102 Windows binary under
    // Wine (gpu benchmark -> Class1 -> level 1 -> vintage): "G-buffer attachment 2 asked for GL format 0x8059".
    struct Slot { uint32_t mGlInternal; uint32_t mGlPixelType; };
    const Slot sSlotRGBA8   = { GL_RGBA8,    GL_UNSIGNED_BYTE };
    const Slot sSlotRGBA16  = { GL_RGBA16,   GL_UNSIGNED_SHORT };
    const Slot sSlotRGB10A2 = { GL_RGB10_A2, GL_UNSIGNED_INT_2_10_10_10_REV };
    const Slot sSlotRGBA16F = { GL_RGBA16F,  GL_HALF_FLOAT };
    // the slot for a request, or null when this provider does not share that format at that index
    const Slot* slotFor(uint32_t index, uint32_t color_fmt)
    {
        switch (index)
        {
        case 0: case 1:   // diffuse (allocate GL_RGBA), orm (GL_RGBA)
            return (color_fmt == GL_RGBA || color_fmt == GL_RGBA8) ? &sSlotRGBA8 : nullptr;
        case 2:           // normal: GL_RGBA16 (HDR) or GL_RGB10_A2 (vintage)
            if (color_fmt == GL_RGBA16) return &sSlotRGBA16;
            if (color_fmt == GL_RGB10_A2) return &sSlotRGB10A2;
            return nullptr;
        case 3:           // emissive: GL_RGB16F (HDR, widened to four channels) or GL_RGB (vintage, widened to RGBA8)
            if (color_fmt == GL_RGB16F || color_fmt == GL_RGBA16F) return &sSlotRGBA16F;
            if (color_fmt == GL_RGB || color_fmt == GL_RGB8 || color_fmt == GL_RGBA || color_fmt == GL_RGBA8) return &sSlotRGBA8;
            return nullptr;
        default:
            return nullptr;
        }
    }
    const Slot* const sAllSlots[4] = { &sSlotRGBA8, &sSlotRGBA16, &sSlotRGB10A2, &sSlotRGBA16F };

    // the GL_EXT_semaphore entry points, fetched by name (the share class keeps its own table private)
    PFNGLGENSEMAPHORESEXTPROC sGenSemaphoresEXT = nullptr;
    PFNGLDELETESEMAPHORESEXTPROC sDeleteSemaphoresEXT = nullptr;
    PFNGLWAITSEMAPHOREEXTPROC sWaitSemaphoreEXT = nullptr;
    PFNGLSIGNALSEMAPHOREEXTPROC sSignalSemaphoreEXT = nullptr;
#if LL_WINDOWS
    PFNGLIMPORTSEMAPHOREWIN32HANDLEEXTPROC sImportSemaphoreWin32HandleEXT = nullptr;
#else
    PFNGLIMPORTSEMAPHOREFDEXTPROC sImportSemaphoreFdEXT = nullptr;
#endif
    bool sSemaphoreFunctionsLoaded = false;

    void* glProc(const char* name)
    {
#if LL_SDL
        return SDL_GL_GetProcAddress(name);
#elif LL_WINDOWS
        return reinterpret_cast<void*>(wglGetProcAddress(name));
#else
        (void)name;
        return nullptr;
#endif
    }

    bool loadSemaphoreFunctions()
    {
        if (sSemaphoreFunctionsLoaded) return sGenSemaphoresEXT && sWaitSemaphoreEXT && sSignalSemaphoreEXT && sDeleteSemaphoresEXT;
        sSemaphoreFunctionsLoaded = true;
        sGenSemaphoresEXT = reinterpret_cast<PFNGLGENSEMAPHORESEXTPROC>(glProc("glGenSemaphoresEXT"));
        sDeleteSemaphoresEXT = reinterpret_cast<PFNGLDELETESEMAPHORESEXTPROC>(glProc("glDeleteSemaphoresEXT"));
        sWaitSemaphoreEXT = reinterpret_cast<PFNGLWAITSEMAPHOREEXTPROC>(glProc("glWaitSemaphoreEXT"));
        sSignalSemaphoreEXT = reinterpret_cast<PFNGLSIGNALSEMAPHOREEXTPROC>(glProc("glSignalSemaphoreEXT"));
#if LL_WINDOWS
        sImportSemaphoreWin32HandleEXT = reinterpret_cast<PFNGLIMPORTSEMAPHOREWIN32HANDLEEXTPROC>(glProc("glImportSemaphoreWin32HandleEXT"));
        return sGenSemaphoresEXT && sDeleteSemaphoresEXT && sWaitSemaphoreEXT && sSignalSemaphoreEXT && sImportSemaphoreWin32HandleEXT;
#else
        sImportSemaphoreFdEXT = reinterpret_cast<PFNGLIMPORTSEMAPHOREFDEXTPROC>(glProc("glImportSemaphoreFdEXT"));
        return sGenSemaphoresEXT && sDeleteSemaphoresEXT && sWaitSemaphoreEXT && sSignalSemaphoreEXT && sImportSemaphoreFdEXT;
#endif
    }

    std::string glErrorsHere()
    {
        std::string s;
        for (GLenum e = glGetError(); e != GL_NO_ERROR; e = glGetError())
        {
            if (!s.empty()) s += ", ";
            s += llformat("0x%x", e);
        }
        return s;
    }
}


WolfVkGBuffer::~WolfVkGBuffer()
{
    destroy();
}

bool WolfVkGBuffer::init(WolfVkDevice& device, bool self_test, std::string& error)
{
    destroy();
    mDevice = &device;
    mSelfTest = self_test;
    std::string why;
    if (!WolfVkShare::available(device, why)) { error = why; mDevice = nullptr; return false; }
    if (!loadSemaphoreFunctions()) { error = "the OpenGL semaphore entry points are missing"; mDevice = nullptr; return false; }
    // every attachment format must be exportable on this device before a single one is handed to LLRenderTarget: a
    // half-shared G-buffer would be worse than none
    bool dedicated_only = false;
    const VkImageUsageFlags colour_usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    const VkImageUsageFlags depth_usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    for (const Slot* slot : sAllSlots)
    {
        if (!WolfVkImage::externalFormatSupported(device, WolfVkImage::formatFor(slot->mGlInternal, slot->mGlPixelType), colour_usage, dedicated_only, error))
        {
            mDevice = nullptr;
            return false;
        }
    }
    if (!WolfVkImage::externalFormatSupported(device, WolfVkImage::formatFor(GL_DEPTH_COMPONENT32F, GL_FLOAT), depth_usage, dedicated_only, error)) { mDevice = nullptr; return false; }

    // the two semaphores of the pass hand-over
    WolfVkDevice::ExternalHandle h1, h2;
    if (!device.createExportedSemaphore(mGlPassDone, h1, error)) { destroy(); return false; }
    if (!device.createExportedSemaphore(mVkPassDone, h2, error)) { device.closeExternalHandle(h1); destroy(); return false; }
    glErrorsHere();
    GLuint sems[2] = { 0, 0 };
    sGenSemaphoresEXT(2, sems);
    mGlSemGlPassDone = sems[0];
    mGlSemVkPassDone = sems[1];
    // [G3 2026-11-11] as WolfVkShare::createSemaphores: Linux fds pass to GL on a successful import (EXT_external_objects_fd
    // rev 7, 4.2.1); Windows NT handles never do ("Importing a Windows handle does not transfer ownership of the handle to
    // the GL implementation", EXT_external_objects_win32 rev 10, 4.2.1) and are closed after the import, success or not
#if LL_WINDOWS
    sImportSemaphoreWin32HandleEXT(mGlSemGlPassDone, WOLF_GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, h1.mWin32);
    sImportSemaphoreWin32HandleEXT(mGlSemVkPassDone, WOLF_GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, h2.mWin32);
    const std::string e = glErrorsHere();
    device.closeExternalHandle(h1);
    device.closeExternalHandle(h2);
    if (!e.empty())
    {
        error = "glImportSemaphoreWin32HandleEXT (G-buffer): " + e;
        destroy();
        return false;
    }
#else
    sImportSemaphoreFdEXT(mGlSemGlPassDone, WOLF_GL_HANDLE_TYPE_OPAQUE_FD_EXT, h1.mFd);
    sImportSemaphoreFdEXT(mGlSemVkPassDone, WOLF_GL_HANDLE_TYPE_OPAQUE_FD_EXT, h2.mFd);
    const std::string e = glErrorsHere();
    if (!e.empty())
    {
        error = "glImportSemaphoreFdEXT (G-buffer): " + e;
        device.closeExternalHandle(h1);
        device.closeExternalHandle(h2);
        destroy();
        return false;
    }
#endif
    mAvailable = true;
    LL_INFOS("Vulkan") << "G-buffer sharing ready: RGBA8, RGBA16, RGB10_A2, RGBA16F + D32_SFLOAT exportable; pass semaphores made" << LL_ENDL;
    return true;
}

void WolfVkGBuffer::destroy()
{
    // [HANG GUARD 2026-11-08 8.2k] "hung" is decided by the waits themselves (review: it was read before waitIdle, so a
    // wait that timed out right here still let the semaphores below be destroyed under the stuck submit). waitIdle is
    // false on a device already hung; glFinishBounded is not started then. Images go through the frame-slot deferral
    // either way (never run on a hung device); the semaphores, GL's and Vulkan's, stay with a hung device.
    bool idle = !mDevice || !mDevice->hasDevice() || mDevice->waitIdle();
    if (idle && gGLManager.mInited && mDevice) idle = WolfVkShare::glFinishBounded(mDevice, "G-buffer destroy");   // nothing shared yet without a device
    const bool hung = !idle;
    mTarget.reset();
    destroyImages();
    if (gGLManager.mInited && sDeleteSemaphoresEXT && !hung)
    {
        GLuint sems[2] = { mGlSemGlPassDone, mGlSemVkPassDone };
        if (sems[0] || sems[1]) sDeleteSemaphoresEXT(2, sems);
    }
    mGlSemGlPassDone = mGlSemVkPassDone = 0;
    if (mDevice && mDevice->hasDevice() && !hung)
    {
        if (mGlPassDone) gVk.vkDestroySemaphore(mDevice->device(), mGlPassDone, nullptr);
        if (mVkPassDone) gVk.vkDestroySemaphore(mDevice->device(), mVkPassDone, nullptr);
    }
    mGlPassDone = mVkPassDone = VK_NULL_HANDLE;
    mGlSignalled = mVkSignalled = false;
    mBrokenReason.clear();
    if (gGLManager.mInited)
    {
        if (mRewriteTex) glDeleteTextures(1, &mRewriteTex);
        if (mRewriteProgram) glDeleteProgram(mRewriteProgram);
        if (mRewriteVao) glDeleteVertexArrays(1, &mRewriteVao);
        if (mRewriteQueryIds[0]) glDeleteQueries(2, mRewriteQueryIds);
    }
    mRewriteQueryIds[0] = mRewriteQueryIds[1] = 0;
    mRewriteQueries[0] = mRewriteQueries[1] = 0;
    mRewriteQueryNext = 0;
    mRewriteMsSum = 0.0;
    mRewriteMsCount = 0;
    mRewriteTex = mRewriteProgram = mRewriteVao = 0;
    mRewriteTexLoc = -1;   // the program went with it (and keeps this field in use where glRewriteDepth is not built, macOS)
    mRewriteW = mRewriteH = 0;
    mAvailable = false;
    mDisabledReason.clear();
    mSelfTestResult.clear();
    mWidth = mHeight = 0;
    mDevice = nullptr;
}

bool WolfVkGBuffer::idleBothAPIs(const char* what)
{
    // [HANG GUARD 2026-11-08 8.2k] the resize paths waited for Vulkan only (vkDeviceWaitIdle, unbounded) and not for GL at
    // all, while GL may still have this frame's draws into the old attachments queued; WolfVkShare::resize already did
    // both. Both bounded now; on a timeout the old images stay (they may still be in use) and the device is hung.
    if (!mDevice || !mDevice->hasDevice()) return true;
    if (mDevice->waitIdle() && (!gGLManager.mInited || WolfVkShare::glFinishBounded(mDevice, what))) return true;
    // the old images are left to the abandoned device and nothing is shared from now on: no attachment of a new size
    // may be mixed with the old ones (ready() stays false), and the next frame falls back on the hung device
    mAvailable = false;
    return false;
}

void WolfVkGBuffer::destroyImages()
{
    for (WolfVkSharedImage& c : mColor) c.destroy();
    mDepth.destroy();
    mTargetDirty = true;
}

bool WolfVkGBuffer::createImage(WolfVkSharedImage& out, uint32_t width, uint32_t height, const WolfVkImage::Format& format, uint32_t gl_internal_format, bool depth, std::string& error)
{
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                                  | (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (!out.create(*mDevice, format, width, height, usage, gl_internal_format, error)) return false;
    // fresh image: the layout GL expects to find it in, released to the external queue family, before GL touches it
    VkCommandBuffer cmd = mDevice->beginOneTime();
    if (cmd == VK_NULL_HANDLE) { error = "no one-time command buffer"; return false; }
    WolfVkImage* img = out.image();
    VkImageMemoryBarrier b{};   // Source: vulkan_core.h:4025
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = 0;
    b.dstAccessMask = depth ? (VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)
                            : (VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.srcQueueFamilyIndex = mDevice->queueFamily();
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;   // :5477
    b.image = img->image();
    b.subresourceRange = { img->aspect(), 0, 1, 0, 1 };
    gVk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    img->noteLayout(b.newLayout, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, b.dstAccessMask);
    if (!mDevice->endOneTimeAndWait(cmd, error)) return false;
    mTargetDirty = true;
    // [IMPORT SELF-TEST 2026-11-10] GL writes it, Vulkan reads it back, through the pass semaphores (nothing pending on
    // either: the pass protocol only starts with the first glHandToVulkan). A failure ends the sharing: this image goes
    // (never handed out), the ones already handed to LLRenderTarget stay until it releases them, and no Vulkan pass runs
    // (prepare() needs all five).
    if (mSelfTest)
    {
        std::string line;
        const bool ok = out.roundTrip(mGlSemGlPassDone, mGlSemVkPassDone, mGlPassDone, mVkPassDone, false, line);
        mGlSignalled = mVkSignalled = false;
        if (!mSelfTestResult.empty()) mSelfTestResult += "; ";
        mSelfTestResult += line;
        if (ok)
        {
            LL_INFOS("Vulkan") << "G-buffer self-test: " << line << LL_ENDL;
        }
        else
        {
            LL_WARNS("Vulkan") << "G-buffer self-test FAILED: " << line << " - the G-buffer is not shared with Vulkan from here; OpenGL draws every pass" << LL_ENDL;
            mDisabledReason = "self-test failed: " + line;
            mAvailable = false;
            out.destroy();
            error = mDisabledReason;
            return false;
        }
    }
    return true;
}

// ---- LLRenderTarget::WolfVkProvider --------------------------------------------------------------------------------

bool WolfVkGBuffer::colorTexture(LLRenderTarget& target, U32 index, U32& color_fmt, U32& tex)
{
    if (!mAvailable || !mDevice || index >= 4) return false;
    const U32 w = target.getWidth(), h = target.getHeight();
    if (w == 0 || h == 0) return false;
    // the slot's format as shared, by the viewer's request (slotFor above). A format this provider does not share ends
    // the sharing for the session (never half a G-buffer: the attachments already handed out stay until the target
    // releases them, the rest and every later allocation are OpenGL's own, no Vulkan pass runs)
    const Slot* slot_ptr = slotFor(index, color_fmt);
    if (!slot_ptr)
    {
        LL_WARNS("Vulkan") << "G-buffer attachment " << index << " asked for GL format 0x" << std::hex << color_fmt << std::dec
                           << ", which this provider does not share: G-buffer sharing ends, OpenGL draws every pass" << LL_ENDL;
        mDisabledReason = llformat("attachment %u asked for GL format 0x%x, not shared", index, color_fmt);
        mAvailable = false;
        return false;
    }
    const Slot& slot = *slot_ptr;
    if (mWidth != w || mHeight != h)
    {
        // a new size: everything is remade as the target asks for each attachment
        if (!idleBothAPIs("G-buffer resize")) return false;   // hung: GL gets its own texture (LLRenderTarget falls back)
        destroyImages();
        mWidth = w;
        mHeight = h;
    }
    std::string error;
    if (!createImage(mColor[index], w, h, WolfVkImage::formatFor(slot.mGlInternal, slot.mGlPixelType), slot.mGlInternal, false, error))
    {
        LL_WARNS("Vulkan") << "G-buffer attachment " << index << " could not be shared: " << error << LL_ENDL;
        return false;
    }
    color_fmt = slot.mGlInternal;
    tex = mColor[index].glTexture();
    LL_INFOS("Vulkan") << "G-buffer attachment " << index << " shared: " << w << "x" << h << " GL 0x" << std::hex << slot.mGlInternal << std::dec << LL_ENDL;
    return true;
}

bool WolfVkGBuffer::depthTexture(LLRenderTarget& target, U32& tex)
{
    if (!mAvailable || !mDevice) return false;
    const U32 w = target.getWidth(), h = target.getHeight();
    if (w == 0 || h == 0) return false;
    if (mWidth != w || mHeight != h)
    {
        if (!idleBothAPIs("G-buffer resize")) return false;
        destroyImages();
        mWidth = w;
        mHeight = h;
    }
    std::string error;
    if (!createImage(mDepth, w, h, WolfVkImage::formatFor(GL_DEPTH_COMPONENT32F, GL_FLOAT), GL_DEPTH_COMPONENT32F, true, error))
    {
        LL_WARNS("Vulkan") << "G-buffer depth could not be shared: " << error << LL_ENDL;
        return false;
    }
    tex = mDepth.glTexture();
    LL_INFOS("Vulkan") << "G-buffer depth shared: " << w << "x" << h << " GL_DEPTH_COMPONENT32F" << LL_ENDL;
    return true;
}

void WolfVkGBuffer::released(LLRenderTarget& /*target*/)
{
    // the target gives its textures back (resize or shutdown): gone on both sides, remade at the next allocate
    mTarget.reset();   // deferred by its own destroy (frame slot), like the images
    if (idleBothAPIs("G-buffer release")) destroyImages();   // hung: the images stay with the abandoned device
    mWidth = mHeight = 0;
}

uint32_t WolfVkGBuffer::colorCount() const
{
    // [VINTAGE G-BUFFER 2026-11-10] the leading valid colour attachments: 4 with the emissive buffer, 3 without
    // (pipeline.cpp addDeferredAttachments: diffuse, orm, normal always; emissive with RenderEnableEmissiveBuffer)
    uint32_t n = 0;
    while (n < 4 && mColor[n].isValid()) ++n;
    return n;
}

bool WolfVkGBuffer::ready() const
{
    if (!mAvailable || !mDepth.isValid()) return false;
    if (colorCount() < 3) return false;
    return mTarget && !mTargetDirty;
}

bool WolfVkGBuffer::prepare()
{
    if (!mAvailable || !mDepth.isValid()) return false;
    if (colorCount() < 3) return false;
    if (!mTarget || mTargetDirty)
    {
        std::string error;
        if (!buildTarget(error))
        {
            LL_WARNS_ONCE("Vulkan") << "G-buffer Vulkan target: " << error << LL_ENDL;
            return false;
        }
        LL_INFOS("Vulkan") << "G-buffer shared and ready for Vulkan passes: " << mWidth << "x" << mHeight << ", " << colorCount() << " colour attachments + D32_SFLOAT depth" << LL_ENDL;
    }
    return true;
}

bool WolfVkGBuffer::buildTarget(std::string& error)
{
    mTarget.reset(new WolfVkRenderTarget());
    mTarget->init(*mDevice);
    const uint32_t count = colorCount();
    for (uint32_t i = 0; i < count; ++i)
    {
        if (!mTarget->addExternalColorAttachment(mColor[i].image(), error)) { mTarget.reset(); return false; }
    }
    mTarget->setExternalDepth(mDepth.image());
    mTarget->setColorLayout(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);   // handed to and from GL as attachments
    mTargetDirty = false;
    return true;
}

bool WolfVkGBuffer::recordDump(VkCommandBuffer cmd, std::string& error)
{
    if (!prepare()) { error = "G-buffer not shared"; return false; }
    mDumpBuffers.clear();
    WolfVkImage* images[5] = { mColor[0].image(), mColor[1].image(), mColor[2].image(), mColor[3].image(), mDepth.image() };
    const uint32_t count = colorCount();
    acquire(cmd);
    for (uint32_t i = 0; i < 5; ++i)
    {
        if (i < 4 && i >= count) continue;   // [VINTAGE G-BUFFER] no emissive attachment
        WolfVkImage* img = images[i];
        const bool depth = img->format().mDepth;
        const uint32_t bpt = depth ? 4u : img->format().mTexelBytes;
        std::unique_ptr<WolfVkBuffer> host(new WolfVkBuffer());
        if (!host->create(*mDevice, static_cast<VkDeviceSize>(img->width()) * img->height() * bpt, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, error))
        {
            // [HAND-OVER 2026-11-08 8.2k] the images acquired above go back to GL all the same (the earlier ones are
            // already back in their attachment layouts): the submit then still matches GL's signal with a release
            mDumpBuffers.clear();
            release(cmd);
            return false;
        }
        // attachment layout -> transfer source, copy, back (the GL hand layouts must hold again at release)
        img->transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy region{};   // Source: vulkan_core.h:3996
        region.imageSubresource = { depth ? static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_DEPTH_BIT) : static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_COLOR_BIT), 0, 0, 1 };
        region.imageExtent = { img->width(), img->height(), 1 };
        gVk.vkCmdCopyImageToBuffer(cmd, img->image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, host->buffer(), 1, &region);   // :4593
        img->transition(cmd, depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        depth ? (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT) : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                        depth ? (VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT) : (VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT));
        mDumpBuffers.push_back(std::move(host));
    }
    // host reads after the copies: :2936 HOST_BIT, :3117 HOST_READ
    VkMemoryBarrier mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    gVk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    release(cmd);
    return true;
}

bool WolfVkGBuffer::writeDump(const std::string& dir, std::string& error)
{
    const uint32_t count = colorCount();
    if (mDumpBuffers.size() != static_cast<size_t>(count) + 1) { error = "no dump recorded"; return false; }
    WolfVkImage* images[5] = { mColor[0].image(), mColor[1].image(), mColor[2].image(), mColor[3].image(), mDepth.image() };
    size_t buffer = 0;
    for (size_t i = 0; i < 5; ++i)   // size_t: indexes mDumpBuffers (-Wsign-conversion)
    {
        if (i < 4 && i >= count) continue;   // [VINTAGE G-BUFFER] the file numbers keep the slot: 4 is always the depth
        const uint32_t bpt = images[i]->format().mDepth ? 4u : images[i]->format().mTexelBytes;
        const std::string path = dir + llformat("/gbuf_%zu_%ux%u_%u.raw", i, images[i]->width(), images[i]->height(), bpt);
        std::ofstream out(path, std::ios::binary);
        if (!out) { error = "cannot write " + path; mDumpBuffers.clear(); return false; }
        mDumpBuffers[buffer]->invalidateForRead();   // [V14 2026-11-11]
        out.write(reinterpret_cast<const char*>(mDumpBuffers[buffer]->hostData()), static_cast<std::streamsize>(mDumpBuffers[buffer]->size()));
        buffer++;
    }
    mDumpBuffers.clear();
    return true;
}

bool WolfVkGBuffer::writeGLViewDump(const std::string& dir, const std::string& prefix, std::string& error)
{
    if (!ready()) { error = "G-buffer not shared"; return false; }
    // GL formats as shared (sColorSlots above / wolfvkgbuffer.h): RGBA8, RGBA8, RGBA16, RGBA16F, DEPTH_COMPONENT32F;
    // read back in their own type so the bytes compare one to one with the Vulkan copy (OpenGL 4.6 8.11.4 glGetTexImage)
    // [VINTAGE G-BUFFER 2026-11-10] each colour attachment read in its own type: RGBA8 as bytes, RGBA16 as shorts,
    // RGB10_A2 as the packed word, RGBA16F as halves (the formats slotFor shares)
    struct Read { uint32_t tex; GLenum format; GLenum type; uint32_t bpt; };
    auto read_of = [&](const WolfVkSharedImage& c) -> Read
    {
        const VkFormat f = c.image() ? c.image()->format().mFormat : VK_FORMAT_UNDEFINED;
        if (f == VK_FORMAT_R16G16B16A16_UNORM) return { c.glTexture(), GL_RGBA, GL_UNSIGNED_SHORT, 8 };
        if (f == VK_FORMAT_R16G16B16A16_SFLOAT) return { c.glTexture(), GL_RGBA, GL_HALF_FLOAT, 8 };
        if (f == VK_FORMAT_A2B10G10R10_UNORM_PACK32) return { c.glTexture(), GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, 4 };
        return { c.glTexture(), GL_RGBA, GL_UNSIGNED_BYTE, 4 };
    };
    const uint32_t count = colorCount();
    const Read reads[5] = { read_of(mColor[0]), read_of(mColor[1]), read_of(mColor[2]), read_of(mColor[3]),
                            { mDepth.glTexture(), GL_DEPTH_COMPONENT, GL_FLOAT, 4 } };
    GLint saved_tex = 0, saved_pack = 0, saved_align = 4;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &saved_tex);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &saved_pack);
    glGetIntegerv(GL_PACK_ALIGNMENT, &saved_align);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);   // rows of 4- and 8-byte texels: no padding either way
    std::vector<uint8_t> data;
    bool ok = true;
    for (uint32_t i = 0; i < 5 && ok; ++i)
    {
        if (i < 4 && i >= count) continue;
        data.resize(static_cast<size_t>(mWidth) * mHeight * reads[i].bpt);
        glBindTexture(GL_TEXTURE_2D, reads[i].tex);
        glGetTexImage(GL_TEXTURE_2D, 0, reads[i].format, reads[i].type, data.data());
        const std::string e = glErrorsHere();
        if (!e.empty()) { error = llformat("glGetTexImage attachment %d: GL error %s", i, e.c_str()); ok = false; break; }
        const std::string path = dir + llformat("/gbufglview_%s_%u_%ux%u_%u.raw", prefix.c_str(), i, mWidth, mHeight, reads[i].bpt);
        std::ofstream out(path, std::ios::binary);
        if (!out) { error = "cannot write " + path; ok = false; break; }
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(saved_tex));
    glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(saved_pack));
    glPixelStorei(GL_PACK_ALIGNMENT, saved_align);
    return ok;
}

void WolfVkGBuffer::glRewriteDepth()
{
#if LL_DARWIN
    // [CI 2026-11-09] Not built for macOS: Apple's GL is 4.1 and has no glCopyImageSubData (GL 4.3; macOS CI run
    // 42614243966 failed at link on it), and nothing reaches this there - the G-buffer is only shared with Vulkan through
    // GL_EXT_memory_object, which Apple's GL lacks (WolfVkShare::available is false on LL_DARWIN, wolfvkshare.cpp).
    return;
#else
    if (!ready()) return;
    if (!mRewriteProgram)
    {
        // a full-screen triangle from gl_VertexID writing the copied depth at its own texel (the same program shape as
        // WolfVkShare's depth copy, wolfvkshare.cpp sDepthCopyVert/Frag)
        const char* vs_src = "#version 330 core\nvoid main(){ vec2 p = vec2((gl_VertexID == 1) ? 3.0 : -1.0, (gl_VertexID == 2) ? 3.0 : -1.0); gl_Position = vec4(p, 0.0, 1.0); }\n";
        const char* fs_src = "#version 330 core\nuniform sampler2D depthTex;\nvoid main(){ gl_FragDepth = texelFetch(depthTex, ivec2(gl_FragCoord.xy), 0).r; }\n";
        GLuint vs = glCreateShader(GL_VERTEX_SHADER); glShaderSource(vs, 1, &vs_src, nullptr); glCompileShader(vs);
        GLuint fs = glCreateShader(GL_FRAGMENT_SHADER); glShaderSource(fs, 1, &fs_src, nullptr); glCompileShader(fs);
        mRewriteProgram = glCreateProgram();
        glAttachShader(mRewriteProgram, vs); glAttachShader(mRewriteProgram, fs); glLinkProgram(mRewriteProgram);
        glDeleteShader(vs); glDeleteShader(fs);
        GLint linked = GL_FALSE;
        glGetProgramiv(mRewriteProgram, GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE)
        {
            LL_WARNS_ONCE("Vulkan") << "GL depth rewrite program did not link" << LL_ENDL;
            glDeleteProgram(mRewriteProgram);
            mRewriteProgram = 0;
            return;
        }
        mRewriteTexLoc = glGetUniformLocation(mRewriteProgram, "depthTex");
        glGenVertexArrays(1, &mRewriteVao);   // core profile: a vertex array must be bound to draw
    }
    GLint saved_tex = 0, saved_prog = 0, saved_vao = 0, saved_active = 0, saved_func = GL_LESS, vp[4] = { 0, 0, 0, 0 };
    GLboolean saved_mask = GL_TRUE, saved_test = GL_FALSE, cm[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
    glGetIntegerv(GL_ACTIVE_TEXTURE, &saved_active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &saved_tex);
    glGetIntegerv(GL_CURRENT_PROGRAM, &saved_prog);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &saved_vao);
    glGetIntegerv(GL_DEPTH_FUNC, &saved_func);
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &saved_mask);
    glGetBooleanv(GL_COLOR_WRITEMASK, cm);
    saved_test = glIsEnabled(GL_DEPTH_TEST);
    // every texel must be written: no scissor rectangle or face culling may cut the full-screen triangle short
    const GLboolean saved_scissor = glIsEnabled(GL_SCISSOR_TEST), saved_cull = glIsEnabled(GL_CULL_FACE);
    if (!mRewriteTex || mRewriteW != mWidth || mRewriteH != mHeight)
    {
        if (mRewriteTex) glDeleteTextures(1, &mRewriteTex);
        glGenTextures(1, &mRewriteTex);
        glBindTexture(GL_TEXTURE_2D, mRewriteTex);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH_COMPONENT32F, static_cast<GLsizei>(mWidth), static_cast<GLsizei>(mHeight));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
        mRewriteW = mWidth; mRewriteH = mHeight;
    }
    glErrorsHere();
    // cost: a GL_TIME_ELAPSED query around the copy and the pass (OpenGL 4.6 4.3 "Time Queries"); two queries in turn,
    // each read the next time round only when GL_QUERY_RESULT_AVAILABLE says so - never a wait.
    // [AMD PERF 2026-11-09] only while the timing instruments are on (WolfVkBridge::timingOn): a query poll every frame is
    // a thread sync in Mesa's threaded context unless the query was flushed (8.2o), and nothing needs the figure otherwise
    const bool want_timing = WolfVkBridge::timingOn();
    if (want_timing && !mRewriteQueryIds[0]) glGenQueries(2, mRewriteQueryIds);
    const int qi = mRewriteQueryNext;
    if (want_timing && mRewriteQueries[qi])
    {
        GLuint available = 0;
        glGetQueryObjectuiv(mRewriteQueryIds[qi], GL_QUERY_RESULT_AVAILABLE, &available);
        if (available)
        {
            GLuint64 ns = 0;
            glGetQueryObjectui64v(mRewriteQueryIds[qi], GL_QUERY_RESULT, &ns);
            mRewriteMsSum += static_cast<double>(ns) / 1.0e6;
            mRewriteMsCount++;
            mRewriteQueries[qi] = 0;
        }
    }
    const bool timing = want_timing && mRewriteQueries[qi] == 0;   // asked for, and the slot is free (its last result was read)
    if (timing) glBeginQuery(GL_TIME_ELAPSED, mRewriteQueryIds[qi]);
    glCopyImageSubData(mDepth.glTexture(), GL_TEXTURE_2D, 0, 0, 0, 0, mRewriteTex, GL_TEXTURE_2D, 0, 0, 0, 0,
                       static_cast<GLsizei>(mWidth), static_cast<GLsizei>(mHeight), 1);
    glUseProgram(mRewriteProgram);
    glBindTexture(GL_TEXTURE_2D, mRewriteTex);
    glUniform1i(mRewriteTexLoc, 0);
    glBindVertexArray(mRewriteVao);
    glViewport(0, 0, static_cast<GLsizei>(mWidth), static_cast<GLsizei>(mHeight));
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_ALWAYS);
    glDepthMask(GL_TRUE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    if (timing)
    {
        glEndQuery(GL_TIME_ELAPSED);
        mRewriteQueries[qi] = 1;
        mRewriteQueryNext = 1 - qi;
    }
    const std::string e = glErrorsHere();
    if (!e.empty()) LL_WARNS_ONCE("Vulkan") << "GL depth rewrite: GL error " << e << LL_ENDL;
    glColorMask(cm[0], cm[1], cm[2], cm[3]);
    glDepthMask(saved_mask);
    glDepthFunc(static_cast<GLenum>(saved_func));
    if (!saved_test) glDisable(GL_DEPTH_TEST);
    if (saved_scissor) glEnable(GL_SCISSOR_TEST);
    if (saved_cull) glEnable(GL_CULL_FACE);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glBindVertexArray(static_cast<GLuint>(saved_vao));
    glUseProgram(static_cast<GLuint>(saved_prog));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(saved_tex));
    glActiveTexture(static_cast<GLenum>(saved_active));
#endif
}

double WolfVkGBuffer::rewriteMsAndReset()
{
    if (mRewriteMsCount == 0) return -1.0;
    const double ms = mRewriteMsSum / mRewriteMsCount;
    mRewriteMsSum = 0.0;
    mRewriteMsCount = 0;
    return ms;
}

// ---- the pass hand-over --------------------------------------------------------------------------------------------

void WolfVkGBuffer::glLayouts(uint32_t textures[5], uint32_t layouts[5], uint32_t& count) const
{
    count = 0;
    for (const WolfVkSharedImage& c : mColor)
    {
        if (!c.isValid()) continue;   // [VINTAGE G-BUFFER] three colour attachments without the emissive buffer
        textures[count] = c.glTexture();
        layouts[count] = WOLF_GL_LAYOUT_COLOR_ATTACHMENT_EXT;
        count++;
    }
    textures[count] = mDepth.glTexture();
    layouts[count] = WOLF_GL_LAYOUT_DEPTH_STENCIL_ATTACHMENT_EXT;
    count++;
}

bool WolfVkGBuffer::glHandToVulkan()
{
    // [HAND-OVER 2026-11-08 8.2k] Every early return below leaves gl_pass_done unsignalled, and says so (false): the caller
    // then records nothing and submits nothing for the pass. Before, these returns were silent and the Vulkan side
    // carried on - it acquired the five images from VK_QUEUE_FAMILY_EXTERNAL with no GL release
    // (VUID-vkQueueSubmit-pSubmits-02207) and drew into them while GL could still be writing them.
    if (!mAvailable || broken() || mGlSignalled) return false;   // mGlSignalled: a second signal of a signalled binary semaphore
    if (!prepare()) return false;   // the five images and the Vulkan target over them must all be there
    GLuint textures[5]; GLenum layouts[5]; GLuint n = 0;
    glLayouts(textures, layouts, n);
    glErrorsHere();   // the check below must see this call's errors only
    // GL_EXT_semaphore: SignalSemaphoreEXT transitions the textures to the layouts and flushes "as if Flush"
    sSignalSemaphoreEXT(mGlSemGlPassDone, 0, nullptr, n, textures, layouts);
    const std::string e = glErrorsHere();
    if (!e.empty())
    {
        // a signal GL refused must not be waited on by Vulkan (VUID-vkQueueSubmit-pWaitSemaphores-03238): the wait would
        // never complete and GL's later wait on vk_pass_done would stall behind it (8.2j)
        mBrokenReason = "glSignalSemaphoreEXT (gl_pass_done): GL error " + e;
        LL_WARNS("Vulkan") << "G-buffer hand-over: " << mBrokenReason << LL_ENDL;
        return false;
    }
    mGlSignalled = true;
    return true;
}

bool WolfVkGBuffer::glTakeBack()
{
    if (!mAvailable || !mVkSignalled) return true;   // nothing Vulkan signalled is waiting to be taken
    GLuint textures[5]; GLenum layouts[5]; GLuint n = 0;
    glLayouts(textures, layouts, n);
    glErrorsHere();
    sWaitSemaphoreEXT(mGlSemVkPassDone, 0, nullptr, n, textures, layouts);
    mVkSignalled = false;
    const std::string e = glErrorsHere();
    if (!e.empty())
    {
        // the wait did not happen: vk_pass_done stays signalled and the next pass's Vulkan signal of it would be invalid
        // (VUID-vkQueueSubmit-pSignalSemaphores-00067), and GL would draw over images Vulkan may still be writing
        mBrokenReason = "glWaitSemaphoreEXT (vk_pass_done): GL error " + e;
        LL_WARNS("Vulkan") << "G-buffer hand-back: " << mBrokenReason << LL_ENDL;
        return false;
    }
    return true;
}

WolfVkDevice::ExtraSync WolfVkGBuffer::sync()
{
    WolfVkDevice::ExtraSync s;
    if (mGlSignalled)
    {
        s.mWaits.push_back(mGlPassDone);
        // the pass writes the attachments and tests depth
        s.mWaitStages.push_back(VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        mGlSignalled = false;
    }
    if (!mVkSignalled)
    {
        s.mSignals.push_back(mVkPassDone);
        mVkSignalled = true;
    }
    return s;
}

void WolfVkGBuffer::acquire(VkCommandBuffer cmd)
{
    const uint32_t ours = mDevice->queueFamily();
    auto barrier = [&](WolfVkImage* img, bool depth)
    {
        if (!img || !img->isValid()) return;
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = 0;
        b.dstAccessMask = depth ? (VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)
                                : (VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        b.oldLayout = b.newLayout = depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        b.dstQueueFamilyIndex = ours;
        b.image = img->image();
        b.subresourceRange = { img->aspect(), 0, 1, 0, 1 };
        const VkPipelineStageFlags dst = depth ? (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT) : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        gVk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, dst, 0, 0, nullptr, 0, nullptr, 1, &b);
        img->noteLayout(b.newLayout, dst, b.dstAccessMask);
    };
    for (WolfVkSharedImage& c : mColor) barrier(c.image(), false);
    barrier(mDepth.image(), true);
}

void WolfVkGBuffer::release(VkCommandBuffer cmd)
{
    const uint32_t ours = mDevice->queueFamily();
    auto barrier = [&](WolfVkImage* img, bool depth)
    {
        if (!img || !img->isValid()) return;
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = depth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        b.dstAccessMask = 0;
        b.oldLayout = b.newLayout = depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b.srcQueueFamilyIndex = ours;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        b.image = img->image();
        b.subresourceRange = { img->aspect(), 0, 1, 0, 1 };
        // [BLUE DASHES 2026-11-08] depth/stencil attachment writes happen in BOTH fragment-test stages (vk.xml syncaccess
        // VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT: syncsupport EARLY_FRAGMENT_TESTS, LATE_FRAGMENT_TESTS). With LATE
        // alone the release's first scope missed every depth write made by early tests, so nothing made them available
        // before the hand-back to GL.
        const VkPipelineStageFlags src = depth ? (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT)
                                               : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        gVk.vkCmdPipelineBarrier(cmd, src, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        img->noteLayout(b.newLayout, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
    };
    for (WolfVkSharedImage& c : mColor) barrier(c.image(), false);
    barrier(mDepth.image(), true);
}
