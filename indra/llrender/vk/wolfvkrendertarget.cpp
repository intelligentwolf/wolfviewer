/**
 * @file wolfvkrendertarget.cpp
 * @brief Off-screen colour + depth targets (see wolfvkrendertarget.h).
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

#include "linden_common.h"
#include "wolfvkrendertarget.h"
#include "wolfvkdevice.h"
#include "llglheaders.h"
#include <algorithm>

WolfVkRenderTarget::~WolfVkRenderTarget()
{
    release();
}

bool WolfVkRenderTarget::allocate(uint32_t resx, uint32_t resy, uint32_t gl_color_fmt, bool depth, bool shared, std::string& error)
{
    release();
    if (!mDevice)
    {
        error = "WolfVkRenderTarget not initialised";
        return false;
    }
    mResX = resx;
    mResY = resy;
    mShared = shared;
    if (!addColorAttachment(gl_color_fmt, error))
    {
        return false;
    }
    if (depth && !allocateDepth(error))
    {
        return false;
    }
    return rebuildPass(error);
}

bool WolfVkRenderTarget::addColorAttachment(uint32_t gl_color_fmt, std::string& error)
{
    if (mColorRefs.size() >= MAX_COLOR_ATTACHMENTS)
    {
        error = "a render target has at most four colour attachments";
        return false;
    }
    // LLRenderTarget::addColorAttachment :238 hands setManualImage GL_RGBA / GL_UNSIGNED_BYTE as the client format; the
    // internal format decides the storage, so the pixel type here is what the internal format implies.
    uint32_t pixel_type = GL_UNSIGNED_BYTE;
    if (gl_color_fmt == GL_RGBA16F || gl_color_fmt == GL_RGB16F || gl_color_fmt == GL_R16F || gl_color_fmt == GL_RG16F) pixel_type = GL_HALF_FLOAT;
    if (gl_color_fmt == GL_RGBA32F || gl_color_fmt == GL_R32F) pixel_type = GL_FLOAT;
    WolfVkImage::Format f = WolfVkImage::formatFor(gl_color_fmt, pixel_type);
    if (!f.valid() || f.mDepth)
    {
        error = llformat("no Vulkan colour format for GL internal format 0x%x", gl_color_fmt);
        return false;
    }
    std::unique_ptr<WolfVkImage> img(new WolfVkImage());
    // colour attachment, sampled afterwards, transfer src for read-back and transfer dst for clears/uploads (vulkan_core.h:2808-2812)
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!img->create(*mDevice, WolfVkImage::Kind::TEXTURE_2D, f, mResX, mResY, 1, 1, usage, mShared, error))
    {
        return false;
    }
    mColorRefs.push_back(img.get());
    mColor.push_back(std::move(img));
    mGLColorFormats.push_back(gl_color_fmt);
    if (mColorRefs.size() > 1 || mDepth)
    {
        destroyPass();   // the pass is rebuilt by the caller's allocate(), or lazily by begin()
    }
    return true;
}

bool WolfVkRenderTarget::allocateDepth(std::string& error)
{
    // llrendertarget.cpp:307 GL_DEPTH_COMPONENT24 -> VK_FORMAT_D32_SFLOAT (wolfvkimage.h table)
    WolfVkImage::Format f = WolfVkImage::formatFor(GL_DEPTH_COMPONENT24, GL_UNSIGNED_INT);
    std::unique_ptr<WolfVkImage> img(new WolfVkImage());
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;   // :2813
    if (!img->create(*mDevice, WolfVkImage::Kind::TEXTURE_2D, f, mResX, mResY, 1, 1, usage, mShared, error))
    {
        return false;
    }
    mDepth = std::move(img);
    destroyPass();
    return true;
}

bool WolfVkRenderTarget::addExternalColorAttachment(WolfVkImage* image, std::string& error)
{
    if (!image || !image->isValid()) { error = "external colour attachment: no image"; return false; }
    if (mColorRefs.size() >= MAX_COLOR_ATTACHMENTS) { error = "a render target has at most four colour attachments"; return false; }
    if (mResX == 0) { mResX = image->width(); mResY = image->height(); }
    if (image->width() != mResX || image->height() != mResY) { error = "external colour attachment size differs from the target"; return false; }
    mColorRefs.push_back(image);
    mGLColorFormats.push_back(0);
    destroyPass();
    return true;
}

void WolfVkRenderTarget::setExternalDepth(WolfVkImage* depth)
{
    if (mExternalDepth == depth) return;
    mExternalDepth = depth;
    mDepth.reset();
    destroyPass();
}

void WolfVkRenderTarget::destroyPass()
{
    if (mDevice && mDevice->hasDevice())
    {
        WolfVkDevice* device = mDevice;
        std::vector<VkRenderPass> passes(mPasses, mPasses + 4);
        std::vector<VkFramebuffer> fbs(mFramebuffers, mFramebuffers + 4);
        device->deferDestroy([device, passes, fbs]()
        {
            for (VkFramebuffer fb : fbs) if (fb) gVk.vkDestroyFramebuffer(device->device(), fb, nullptr);
            for (VkRenderPass rp : passes) if (rp) gVk.vkDestroyRenderPass(device->device(), rp, nullptr);
        });
    }
    for (int i = 0; i < 4; ++i) { mPasses[i] = VK_NULL_HANDLE; mFramebuffers[i] = VK_NULL_HANDLE; }
}

void WolfVkRenderTarget::release()
{
    destroyPass();
    mColor.clear();
    mColorRefs.clear();
    mGLColorFormats.clear();
    mDepth.reset();
    mResX = mResY = 0;
    mInPass = false;
}

bool WolfVkRenderTarget::resize(uint32_t resx, uint32_t resy, std::string& error)
{
    const std::vector<uint32_t> formats = mGLColorFormats;
    const bool depth = mDepth != nullptr;
    const bool shared = mShared;
    release();
    mResX = resx;
    mResY = resy;
    mShared = shared;
    for (uint32_t f : formats)
    {
        if (!addColorAttachment(f, error)) return false;
    }
    if (depth && !allocateDepth(error)) return false;
    return rebuildPass(error);
}

bool WolfVkRenderTarget::rebuildPass(std::string& error)
{
    destroyPass();
    WolfVkImage* depth = depthImage();
    if (mColor.empty() && !depth)
    {
        error = "render target has no attachments";
        return false;
    }
    VkDevice dev = mDevice->device();

    auto build = [&](bool clear_color, bool clear_depth, VkRenderPass& out_pass, VkFramebuffer& out_fb) -> bool
    {
        std::vector<VkAttachmentDescription> attachments;
        std::vector<VkAttachmentReference> color_refs;
        std::vector<VkImageView> views;
        for (WolfVkImage* img : mColorRefs)
        {
            // Source: vulkan_core.h:4419 VkAttachmentDescription { flags, format, samples, loadOp, storeOp, stencilLoadOp, stencilStoreOp, initialLayout, finalLayout }
            VkAttachmentDescription a{};
            a.format = img->format().mFormat;
            a.samples = VK_SAMPLE_COUNT_1_BIT;
            a.loadOp = clear_color ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;   // :2688, :2687
            a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;                                            // :2697
            a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;                                   // :2689
            a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;                                 // :2698
            // LOAD keeps what is there, so the old contents' layout must be the one they were left in (SHADER_READ_ONLY
            // after end()); a CLEAR pass does not care
            a.initialLayout = clear_color ? VK_IMAGE_LAYOUT_UNDEFINED : mColorLayout;   // :2270; mColorLayout: :2275 sampled next, or :2272 an attachment for GL
            a.finalLayout = mColorLayout;
            attachments.push_back(a);
            // Source: vulkan_core.h:4431 VkAttachmentReference { attachment, layout }
            color_refs.push_back({ static_cast<uint32_t>(attachments.size() - 1), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL });   // :2272
            views.push_back(img->view());
        }
        VkAttachmentReference depth_ref{ VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };   // :137, :2273
        if (depth)
        {
            VkAttachmentDescription a{};
            a.format = depth->format().mFormat;
            a.samples = VK_SAMPLE_COUNT_1_BIT;
            a.loadOp = clear_depth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            // The pass writes depth, so the attachment stays in DEPTH_STENCIL_ATTACHMENT_OPTIMAL (:2273) in and out; a
            // loaded pass expects the image in that layout (end() notes it; a sampler of the depth transitions it to
            // read-only itself). A read-only layout with depth writes enabled is invalid (VUID-vkCmdDraw-None-06886).
            a.initialLayout = clear_depth ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            a.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            attachments.push_back(a);
            depth_ref.attachment = static_cast<uint32_t>(attachments.size() - 1);
            views.push_back(depth->view());
        }

        // Source: vulkan_core.h:4458 VkSubpassDescription { flags, pipelineBindPoint, inputAttachmentCount, pInputAttachments, colorAttachmentCount,
        // pColorAttachments, pResolveAttachments, pDepthStencilAttachment, preserveAttachmentCount, pPreserveAttachments }
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;   // :2435
        subpass.colorAttachmentCount = static_cast<uint32_t>(color_refs.size());
        subpass.pColorAttachments = color_refs.empty() ? nullptr : color_refs.data();
        subpass.pDepthStencilAttachment = depth ? &depth_ref : nullptr;

        // Source: vulkan_core.h:4448 VkSubpassDependency { srcSubpass, dstSubpass, srcStageMask, dstStageMask, srcAccessMask, dstAccessMask, dependencyFlags }.
        // In: whatever sampled these images before (fragment shader reads) must finish before they are written.
        // Out: the attachment writes must be visible to later fragment shader reads (spec 8.1 "Render Pass Creation",
        // implicit dependencies cover layout transitions but not memory visibility to a following sampling pass).
        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;   // :138
        deps[0].dstSubpass = 0;
        deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;   // :2929, :2934
        deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;   // :2932, :2930, :2931
        deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;   // :3109, :3115, :3116
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;   // :3112, :3111, :3114, :3113
        deps[0].dependencyFlags = 0;
        deps[1].srcSubpass = 0;
        deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        deps[1].dependencyFlags = 0;

        // Source: vulkan_core.h:4471 VkRenderPassCreateInfo { sType, pNext, flags, attachmentCount, pAttachments, subpassCount, pSubpasses, dependencyCount, pDependencies }
        VkRenderPassCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;   // :245
        info.attachmentCount = static_cast<uint32_t>(attachments.size());
        info.pAttachments = attachments.data();
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        info.dependencyCount = 2;
        info.pDependencies = deps;
        VkResult res = gVk.vkCreateRenderPass(dev, &info, nullptr, &out_pass);
        if (res != VK_SUCCESS)
        {
            error = std::string("vkCreateRenderPass: ") + WolfVkLoader::resultString(res);
            return false;
        }
        // Source: vulkan_core.h:4436 VkFramebufferCreateInfo { sType, pNext, flags, renderPass, attachmentCount, pAttachments, width, height, layers }
        VkFramebufferCreateInfo fb{};
        fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;   // :244
        fb.renderPass = out_pass;
        fb.attachmentCount = static_cast<uint32_t>(views.size());
        fb.pAttachments = views.data();
        fb.width = mResX;
        fb.height = mResY;
        fb.layers = 1;
        res = gVk.vkCreateFramebuffer(dev, &fb, nullptr, &out_fb);
        if (res != VK_SUCCESS)
        {
            error = std::string("vkCreateFramebuffer: ") + WolfVkLoader::resultString(res);
            return false;
        }
        return true;
    };
    for (int i = 0; i < 4; ++i)
    {
        if (!build((i & 1) != 0, (i & 2) != 0, mPasses[i], mFramebuffers[i])) { destroyPass(); return false; }
    }
    return true;
}

void WolfVkRenderTarget::begin(VkCommandBuffer cmd, bool clear_color, bool clear_depth, const float clear_rgba[4], float depth_value, const VkRect2D* viewport)
{
    if (mPasses[3] == VK_NULL_HANDLE)
    {
        std::string error;
        if (!rebuildPass(error))
        {
            LL_WARNS_ONCE("Vulkan") << "render target pass: " << error << LL_ENDL;
            return;
        }
    }
    // A never-drawn attachment is in VK_IMAGE_LAYOUT_UNDEFINED and the LOAD passes expect the post-end() layouts: clear it then.
    bool color_fresh = false;
    for (WolfVkImage* img : mColorRefs) if (img->layout() == VK_IMAGE_LAYOUT_UNDEFINED) color_fresh = true;
    WolfVkImage* depth = depthImage();
    const bool depth_fresh = depth && depth->layout() == VK_IMAGE_LAYOUT_UNDEFINED;
    if (depth && !depth_fresh && !clear_depth && depth->layout() != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
    {
        // a loaded depth must arrive in the attachment layout the pass declares (rebuildPass)
        depth->transition(cmd, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    }
    const int variant = ((clear_color || color_fresh) ? 1 : 0) | ((clear_depth || depth_fresh) ? 2 : 0);
    VkRenderPass pass = mPasses[variant];
    VkFramebuffer fb = mFramebuffers[variant];

    // Source: vulkan_core.h:4494 VkClearValue { color | depthStencil }; :4483 VkClearDepthStencilValue { depth, stencil }
    std::vector<VkClearValue> clears;
    for (size_t i = 0; i < mColorRefs.size(); ++i)
    {
        VkClearValue v{};
        v.color.float32[0] = clear_rgba ? clear_rgba[0] : 0.f;
        v.color.float32[1] = clear_rgba ? clear_rgba[1] : 0.f;
        v.color.float32[2] = clear_rgba ? clear_rgba[2] : 0.f;
        v.color.float32[3] = clear_rgba ? clear_rgba[3] : 0.f;
        clears.push_back(v);
    }
    if (depth)
    {
        VkClearValue v{};
        v.depthStencil = { depth_value, 0 };
        clears.push_back(v);
    }
    // Source: vulkan_core.h:4520 VkRenderPassBeginInfo { sType, pNext, renderPass, framebuffer, renderArea, clearValueCount, pClearValues }
    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;   // :250
    begin.renderPass = pass;
    begin.framebuffer = fb;
    begin.renderArea = { { 0, 0 }, { mResX, mResY } };
    begin.clearValueCount = static_cast<uint32_t>(clears.size());
    begin.pClearValues = clears.data();
    // Source: vulkan_core.h:4664 PFN_vkCmdBeginRenderPass(cmd, pRenderPassBegin, contents); :2707 VK_SUBPASS_CONTENTS_INLINE
    gVk.vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
    mInPass = true;

    // LLRenderTarget::bindTarget, llrendertarget.cpp:447 glViewport(0, 0, mResX, mResY). ORIENTATION: every image this
    // backend shares with or copies from OpenGL has GL's memory order, row 0 = the BOTTOM of the picture (GL textures,
    // the layers GL writes, the read-back depth). A Vulkan viewport with a positive height puts clip-space y = -1 at
    // framebuffer row 0, so rendering with the ordinary positive viewport writes the image in exactly that order: the
    // depth test then compares against the right rows of GL's depth, and one layer composites over another with the
    // same texture coordinates (the composite flips once for the swapchain, compositeV.glsl). The price is the winding:
    // GL's counter-clockwise front faces appear clockwise in Vulkan's y-down window space, so the pipelines use
    // VK_FRONT_FACE_CLOCKWISE (wolfvkpipeline.cpp). (The first cut used a negative-height viewport instead - the Vulkan
    // layer came out upside down against GL's depth and layers: nothing passed the depth test, 2026-11-08.)
    // A sub-rectangle is GL's glViewport(x, y, w, h) with y from the bottom = framebuffer row y.
    const uint32_t rx = viewport ? static_cast<uint32_t>(std::max(0, viewport->offset.x)) : 0u;
    const uint32_t ry = viewport ? static_cast<uint32_t>(std::max(0, viewport->offset.y)) : 0u;
    const uint32_t rw = viewport ? std::min(viewport->extent.width, mResX - std::min(rx, mResX)) : mResX;
    const uint32_t rh = viewport ? std::min(viewport->extent.height, mResY - std::min(ry, mResY)) : mResY;
    VkViewport vp{};
    vp.x = static_cast<float>(rx);
    vp.y = static_cast<float>(ry);
    vp.width = static_cast<float>(rw);
    vp.height = static_cast<float>(rh);
    vp.minDepth = 0.f;
    vp.maxDepth = 1.f;
    gVk.vkCmdSetViewport(cmd, 0, 1, &vp);   // :4645
    VkRect2D scissor{ { static_cast<int32_t>(rx), static_cast<int32_t>(ry) }, { rw, rh } };
    gVk.vkCmdSetScissor(cmd, 0, 1, &scissor);   // :4646
}

void WolfVkRenderTarget::end(VkCommandBuffer cmd)
{
    if (!mInPass) return;
    gVk.vkCmdEndRenderPass(cmd);   // :4666
    mInPass = false;
    // the pass's finalLayout left every attachment readable; tell the images so later transitions start from the truth
    for (WolfVkImage* img : mColorRefs)
    {
        if (mColorLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
            img->noteLayout(mColorLayout, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        else
            img->noteLayout(mColorLayout, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
    if (WolfVkImage* depth = depthImage())
    {
        depth->noteLayout(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    }
}

bool WolfVkRenderTarget::readPixels(uint32_t attachment, std::vector<uint8_t>& out, std::string& error)
{
    if (attachment >= mColorRefs.size())
    {
        error = "no such colour attachment";
        return false;
    }
    return mColorRefs[attachment]->readBack(out, error);
}
