/**
 * @file wolfvkrendertarget.h
 * @brief Off-screen colour + depth targets (the LLRenderTarget equivalent): images, render pass, framebuffer, read-back.
 *
 * [VULKAN BACKEND 2026-11-08]
 *
 * Mirrors LLRenderTarget (llrendertarget.h): allocate(resx, resy, color_fmt, depth) makes the first colour attachment
 * and, with depth, a depth image; addColorAttachment adds up to four colour attachments (:205-296); allocateDepth makes
 * the depth image (GL_DEPTH_COMPONENT24 there :307, VK_FORMAT_D32_SFLOAT here: RADV has no D24, vkglshare probe);
 * bindTarget/flush become begin(cmd, clear)/end(cmd) around the draws; getTexture(i) becomes image(i).
 *
 * One VkRenderPass per target (attachments: the colour images with LOAD_OP_CLEAR or LOAD, STORE_OP_STORE; the depth
 * image likewise; one subpass; external dependencies so a later sampling pass sees the results), and one
 * VkFramebuffer. Read-back (readPixels) copies a colour attachment to a host buffer and waits, for the VR stream and
 * the light meter paths that today use glReadPixels / glGetTexImage.
 *
 * Images meant to be shared with OpenGL in the staged hand-over are created dedicated (vkglshare probe: RADV
 * requires it) by passing shared = true to allocate(); the export itself (VK_KHR_external_memory_fd / win32) is the
 * next stage's work and is not here.
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

#ifndef WOLF_VK_RENDER_TARGET_H
#define WOLF_VK_RENDER_TARGET_H

#include "wolfvkimage.h"
#include <memory>
#include <string>
#include <vector>

class WolfVkDevice;

class WolfVkRenderTarget
{
public:
    static constexpr uint32_t MAX_COLOR_ATTACHMENTS = 4;   // llrendertarget.h:92 "limit of 4 color attachments per render target"

    WolfVkRenderTarget() = default;
    ~WolfVkRenderTarget();

    void init(WolfVkDevice& device) { mDevice = &device; }

    // LLRenderTarget::allocate :105: gl_color_fmt as the viewer gives it (GL_RGBA, GL_RGBA16F, ...); depth adds a depth image.
    // shared: images that OpenGL will import later (dedicated allocations).
    bool allocate(uint32_t resx, uint32_t resy, uint32_t gl_color_fmt, bool depth, bool shared, std::string& error);
    // LLRenderTarget::addColorAttachment :205
    bool addColorAttachment(uint32_t gl_color_fmt, std::string& error);
    // LLRenderTarget::allocateDepth :298
    bool allocateDepth(std::string& error);
    // [SHARED IMAGES 2026-11-08] Use a depth image owned elsewhere (the layer shared with OpenGL, wolfvkshare.cpp) as this
    // target's depth attachment; the target does not destroy it. Null returns to no depth attachment.
    void setExternalDepth(WolfVkImage* depth);
    // LLRenderTarget::release :354
    void release();
    // LLRenderTarget::resize :77: same formats, new size
    bool resize(uint32_t resx, uint32_t resy, std::string& error);

    bool isComplete() const { return mPasses[3] != VK_NULL_HANDLE && mFramebuffers[3] != VK_NULL_HANDLE; }
    uint32_t getWidth() const { return mResX; }
    uint32_t getHeight() const { return mResY; }
    uint32_t getNumTextures() const { return static_cast<uint32_t>(mColorRefs.size()); }
    WolfVkImage* image(uint32_t i) { return i < mColorRefs.size() ? mColorRefs[i] : nullptr; }
    // [VULKAN G-BUFFER 2026-11-08] A colour attachment owned elsewhere (a G-buffer image shared with OpenGL); not destroyed here.
    bool addExternalColorAttachment(WolfVkImage* image, std::string& error);
    // The layout the colour attachments are in when a LOAD pass begins and are left in by end(): SHADER_READ_ONLY_OPTIMAL
    // (sampled by the next pass, the default) or COLOR_ATTACHMENT_OPTIMAL (handed to/from OpenGL as attachments).
    void setColorLayout(VkImageLayout layout) { if (layout != mColorLayout) { mColorLayout = layout; destroyPass(); } }
    WolfVkImage* depthImage() { return mExternalDepth ? mExternalDepth : mDepth.get(); }
    VkRenderPass renderPass() const { return mPasses[3]; }
    VkFramebuffer framebuffer() const { return mFramebuffers[3]; }
    VkFormat colorFormat(uint32_t i) const { return i < mColorRefs.size() ? mColorRefs[i]->format().mFormat : VK_FORMAT_UNDEFINED; }
    bool hasDepth() const { return mExternalDepth != nullptr || mDepth != nullptr; }

    // LLRenderTarget::bindTarget + clear: begins the render pass on cmd. clear_color/clear_depth: as LLRenderTarget::clear
    // (GL_COLOR_BUFFER_BIT / GL_DEPTH_BUFFER_BIT); without a clear the attachment keeps its contents (LOAD_OP_LOAD).
    // Sets the viewport and scissor to the whole target (LLRenderTarget::bindTarget sets glViewport, llrendertarget.cpp:447).
    // viewport: when given, the viewport and scissor are this rectangle (x, y from the BOTTOM-left as glViewport takes
    // them) instead of the whole target - the world view rect inside the window (pipeline.cpp renderFinalize gGLViewport).
    void begin(VkCommandBuffer cmd, bool clear_color, bool clear_depth, const float clear_rgba[4], float depth_value, const VkRect2D* viewport = nullptr);
    // LLRenderTarget::flush: ends the render pass; the attachments are then readable by shaders.
    void end(VkCommandBuffer cmd);

    // glReadPixels / glGetTexImage replacement: colour attachment i, tightly packed in its VkFormat, waiting for the GPU.
    bool readPixels(uint32_t attachment, std::vector<uint8_t>& out, std::string& error);

private:
    bool rebuildPass(std::string& error);
    void destroyPass();

    WolfVkDevice* mDevice = nullptr;
    uint32_t mResX = 0, mResY = 0;
    bool mShared = false;
    std::vector<std::unique_ptr<WolfVkImage>> mColor;   // the attachments this target owns
    std::vector<WolfVkImage*> mColorRefs;                // every colour attachment in order (owned or external)
    VkImageLayout mColorLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    std::vector<uint32_t> mGLColorFormats;
    std::unique_ptr<WolfVkImage> mDepth;
    WolfVkImage* mExternalDepth = nullptr;
    // four passes: load ops are baked into a render pass, so colour clear/load x depth clear/load each get their own;
    // index = (clear_color ? 1 : 0) | (clear_depth ? 2 : 0). renderPass() returns the clear-both one (the pipeline
    // cache keys on it; the four are compatible for pipelines: same attachments and formats, spec 8.2 "Render Pass Compatibility").
    VkRenderPass mPasses[4] = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkFramebuffer mFramebuffers[4] = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };
    bool mInPass = false;
};

#endif // WOLF_VK_RENDER_TARGET_H
