/**
 * @file wolfvkgbuffer.h
 * @brief The viewer's G-buffer (mRT->deferredScreen) shared between OpenGL and Vulkan, so Vulkan can draw geometry passes
 *        into it in the middle of the GL frame.
 *
 * [VULKAN G-BUFFER 2026-11-08] Stage 4 proper (docs/VULKAN_BACKEND.md 8.5). LLRenderTarget asks this provider for its
 * textures when the target is marked shared (LLRenderTarget::sWolfVkProvider / setWolfVkShared): each attachment is a
 * Vulkan image with exported memory, imported into GL as the texture LLRenderTarget attaches to its FBO. Formats
 * (pipeline.cpp allocateScreenBufferInternal + addDeferredAttachments), as shared:
 *   0 GL_RGBA (diffuse)   -> GL_RGBA8   / VK_FORMAT_R8G8B8A8_UNORM
 *   1 GL_RGBA (orm)       -> GL_RGBA8   / VK_FORMAT_R8G8B8A8_UNORM
 *   2 GL_RGBA16 (normal)  -> GL_RGBA16  / VK_FORMAT_R16G16B16A16_UNORM
 *   3 GL_RGB16F (emissive)-> GL_RGBA16F / VK_FORMAT_R16G16B16A16_SFLOAT (three-channel 16-bit images are not exportable; the
 *                            alpha is written 0 by the shaders and read by nobody)
 *   depth GL_DEPTH_COMPONENT24 -> GL_DEPTH_COMPONENT32F / VK_FORMAT_D32_SFLOAT (RADV has no D24; vkglshare probe)
 * mRT->screen shares the depth texture as before (LLRenderTarget::shareDepthBuffer).
 *
 * A pass: GL has bound deferredScreen and drawn the pools before this one. glSignalSemaphoreEXT(gl_pass_done, the five
 * textures, COLOR_ATTACHMENT / DEPTH_STENCIL_ATTACHMENT layouts) hands them over; Vulkan records a mid-frame command
 * buffer (WolfVkDevice::beginMidFrame): acquire barriers from VK_QUEUE_FAMILY_EXTERNAL, the pass into a WolfVkRenderTarget
 * built over the five images (LOAD everything, layouts kept at COLOR_ATTACHMENT_OPTIMAL), release barriers back, and
 * submits it waiting gl_pass_done and signalling vk_pass_done; GL then glWaitSemaphoreEXT(vk_pass_done, same textures and
 * layouts) and carries on. One signal, one wait, each direction, per pass; nothing waits on the CPU.
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

#ifndef WOLF_VK_GBUFFER_H
#define WOLF_VK_GBUFFER_H

#include "llrendertarget.h"
#include "wolfvkshare.h"
#include "vk/wolfvkrendertarget.h"
#include "vk/wolfvkbuffer.h"
#include <memory>
#include <string>

class WolfVkGBuffer : public LLRenderTarget::WolfVkProvider
{
public:
    WolfVkGBuffer() = default;
    ~WolfVkGBuffer() override;

    // Checks the five formats can be exported and makes the two semaphores. The target is only filled when LLRenderTarget
    // asks (allocate after setWolfVkShared). False with error when this device/GL pair cannot share them.
    bool init(WolfVkDevice& device, std::string& error);
    void destroy();

    // LLRenderTarget::WolfVkProvider
    bool colorTexture(LLRenderTarget& target, U32 index, U32& color_fmt, U32& tex) override;
    bool depthTexture(LLRenderTarget& target, U32& tex) override;
    void released(LLRenderTarget& target) override;

    // The shared G-buffer is complete (four colours + depth, same size) and the Vulkan target over it is built.
    bool ready() const;
    // Builds the Vulkan target over the five images when they are all there and it is not built yet; true = ready().
    bool prepare();
    uint32_t width() const { return mWidth; }
    uint32_t height() const { return mHeight; }
    WolfVkRenderTarget* target() { return ready() ? mTarget.get() : nullptr; }
    // [G-BUFFER COMPARE 2026-11-08] The five attachments as raw files <dir>/gbuf_<n>_<w>x<h>_<bytes-per-texel>.raw (0-3
    // colour, 4 depth as 32-bit floats), for the GL-vs-Vulkan compare (docs 8.5 item 4): recordDump puts the copy commands
    // on one mid-frame command buffer (acquire from GL, copy the five images into host buffers kept until writeDump, hand
    // back - the hand-back is recorded on failure too, so the buffer always ends with the images released to GL);
    // writeDump() writes the files once the submit has completed. Called between passes, never inside a render pass.
    bool recordDump(VkCommandBuffer cmd, std::string& error);
    bool writeDump(const std::string& dir, std::string& error);
    // [BLUE DASHES 2026-11-08] the same five attachments as OPENGL sees them (glGetTexImage on the imported textures, after
    // glTakeBack): <dir>/gbufglview_<prefix>_<n>_<w>x<h>_<bpt>.raw. The Vulkan dump and this one, taken in the same frame
    // from the same memory, must be identical; a difference means GL reads something other than what Vulkan wrote.
    bool writeGLViewDump(const std::string& dir, const std::string& prefix, std::string& error);

    // ---- one pass -------------------------------------------------------------------------------------------------
    // GL side, before the Vulkan submit: hands the five images to Vulkan (signals gl_pass_done).
    // [HAND-OVER 2026-11-08 8.2k] True only when gl_pass_done was really signalled. On false nothing was handed over: the
    // caller must neither record the acquire/pass/release nor submit sync() (WolfVkDevice::discardMidFrame), and GL draws
    // the pass itself. False after a GL error in the signal also sets broken().
    bool glHandToVulkan();
    // GL side, after the submit: takes them back (waits vk_pass_done on GL's queue). False (and broken()) when the wait
    // raised a GL error.
    bool glTakeBack();
    // [BLUE DASHES 2026-11-08] WolfVulkanGLDepthRewrite (on by default, 8.2n): OpenGL copies the shared depth (glCopyImageSubData,
    // OpenGL 4.6 18.3.3) and draws it back into the bound target's depth through a full-screen pass (depth test ALWAYS,
    // gl_FragDepth, colour writes off). The values stay the same; what changes is that OpenGL's depth unit has written
    // every texel itself. Call with the main G-buffer bound, after glTakeBack.
    void glRewriteDepth();
    // [BLUE DASHES 2026-11-08] the GPU time of glRewriteDepth (GL_TIME_ELAPSED queries, read a frame later without
    // waiting): mean ms over the calls since the last read, -1 when none finished
    double rewriteMsAndReset();
    // [HAND-OVER 2026-11-08 8.2k] a GL semaphore call failed: the pass semaphores' state is unknown, the owner falls back
    bool broken() const { return !mBrokenReason.empty(); }
    const std::string& brokenReason() const { return mBrokenReason; }
    // Vulkan side: the submit's waits/signals (exactly once per pass), and the barriers around the pass.
    WolfVkDevice::ExtraSync sync();
    void acquire(VkCommandBuffer cmd);
    void release(VkCommandBuffer cmd);

private:
    bool createImage(WolfVkSharedImage& out, uint32_t width, uint32_t height, const WolfVkImage::Format& format, uint32_t gl_internal_format, bool depth, std::string& error);
    void destroyImages();
    bool buildTarget(std::string& error);
    void glLayouts(uint32_t textures[5], uint32_t layouts[5], uint32_t& count) const;
    // [HANG GUARD 2026-11-08 8.2k] before shared images are destroyed or remade: Vulkan and GL both finished with them
    // (bounded waits; false = the device is now hung and the images are left alone)
    bool idleBothAPIs(const char* what);

    WolfVkDevice* mDevice = nullptr;
    bool mAvailable = false;
    uint32_t mWidth = 0, mHeight = 0;
    WolfVkSharedImage mColor[4];
    WolfVkSharedImage mDepth;
    std::unique_ptr<WolfVkRenderTarget> mTarget;
    bool mTargetDirty = true;
    VkSemaphore mGlPassDone = VK_NULL_HANDLE;   // GL signals, Vulkan waits
    VkSemaphore mVkPassDone = VK_NULL_HANDLE;   // Vulkan signals, GL waits
    uint32_t mGlSemGlPassDone = 0, mGlSemVkPassDone = 0;
    bool mGlSignalled = false;
    bool mVkSignalled = false;
    std::string mBrokenReason;
    uint64_t mRewriteQueries[2] = { 0, 0 };
    uint32_t mRewriteQueryIds[2] = { 0, 0 };
    int mRewriteQueryNext = 0;
    double mRewriteMsSum = 0.0;
    uint32_t mRewriteMsCount = 0;
    uint32_t mRewriteTex = 0, mRewriteW = 0, mRewriteH = 0, mRewriteProgram = 0, mRewriteVao = 0;
    int32_t mRewriteTexLoc = -1;
    std::vector<std::unique_ptr<WolfVkBuffer>> mDumpBuffers;   // host buffers of a recorded dump
};

#endif // WOLF_VK_GBUFFER_H
