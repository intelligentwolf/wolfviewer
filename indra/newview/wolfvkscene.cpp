/**
 * @file wolfvkscene.cpp
 * @brief The Vulkan backend's per-frame drawing (see wolfvkscene.h).
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
#include "wolfvkscene.h"
#include "wolfvkmirror.h"
#include "vk/wolfvkdevice.h"
#include "vk/wolfvkswapchain.h"
#include "vk/wolfvkbuffer.h"

#include "llgl.h"
#include "llglheaders.h"
#include "llglslshader.h"
#include "llrendertarget.h"
#include "llvertexbuffer.h"
#include "llviewershadermgr.h"
#include "llviewerwindow.h"
#include "llviewercontrol.h"
#include "llviewertexture.h"
#include "llspatialpartition.h"
#include "lldrawpool.h"
#include "pipeline.h"
#include "lldir.h"
#include "llmatrix4a.h"
#include "llappviewer.h"   // gUseWireframe
#include "llfile.h"        // [BLACK WORLD 2026-11-10] LLFile::stat of manifest.json

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <fstream>
#include <sstream>
#include <algorithm>

namespace
{
    constexpr VkDeviceSize UNIFORM_RING_BYTES = 64u * 1024u * 1024u;   // per frame: ~3 KB per draw -> ~20,000 draws (the G-buffer pass draws thousands)
    constexpr VkDeviceSize STAGING_RING_BYTES = 64u * 1024u * 1024u;   // per frame: the vertex bytes rewritten by GL in one frame (streaming bursts)

    bool readSpvFile(const std::string& path, std::vector<uint32_t>& words)
    {
        llifstream in(path, std::ios::binary | std::ios::ate);
        if (!in.is_open()) return false;
        const std::streamsize size = in.tellg();
        if (size <= 0 || (size % 4) != 0) return false;
        words.resize(static_cast<size_t>(size) / 4);
        in.seekg(0);
        in.read(reinterpret_cast<char*>(words.data()), size);
        return in.good() || in.eof();
    }

    // Saves and restores the GL bindings the read-backs touch (wolfvrscreen.cpp captureInto does the same for its blits).
    struct GLReadbackScope
    {
        GLint mReadFB = 0, mPack = 0, mTexture = 0, mReadBuffer = 0;
        GLReadbackScope()
        {
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &mReadFB);
            glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &mPack);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &mTexture);
            glGetIntegerv(GL_READ_BUFFER, &mReadBuffer);
        }
        ~GLReadbackScope()
        {
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(mTexture));
            glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(mPack));
            glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(mReadFB));
            glReadBuffer(static_cast<GLenum>(mReadBuffer));
        }
    };
}

WolfVkScene::~WolfVkScene()
{
    destroy();
}

bool WolfVkScene::init(WolfVkDevice& device, WolfVkSwapchain& swapchain, std::string& error)
{
    mDevice = &device;
    mSpvDir = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "shaders", "spv");
    mPipelines.init(device);
    if (!mUniforms.init(device, UNIFORM_RING_BYTES, error))
    {
        return false;
    }
    mSamplers.init(device, gGLManager.mMaxAnisotropy);
    if (!mStaging.init(device, STAGING_RING_BYTES, error))
    {
        return false;
    }
    WolfVkMirror::init(device, mSamplers);
    WolfVkMirror::setStagingRing(&mStaging);

    // 1x1 white: what a missing texture samples (LLTexUnit::sWhiteTexture plays that part in GL); 1x1 transparent black:
    // a layer that contributes nothing to the composite
    auto make_pixel = [&](std::unique_ptr<WolfVkImage>& out, uint8_t r, uint8_t g, uint8_t b, uint8_t a) -> bool
    {
        out.reset(new WolfVkImage());
        WolfVkImage::Format f = WolfVkImage::formatFor(GL_RGBA8, GL_UNSIGNED_BYTE);
        if (!out->create(device, WolfVkImage::Kind::TEXTURE_2D, f, 1, 1, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false, error))
        {
            return false;
        }
        const uint8_t px[4] = { r, g, b, a };
        if (!out->upload(VK_NULL_HANDLE, 0, 0, 1, 1, px, GL_UNSIGNED_BYTE, false, error))
        {
            return false;
        }
        VkCommandBuffer cmd = device.beginOneTime();
        if (cmd == VK_NULL_HANDLE) { error = "no one-time command buffer"; return false; }   // [8.2k] beginOneTime refuses on a hung device
        out->transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        return device.endOneTimeAndWait(cmd, error);
    };
    if (!make_pixel(mWhite, 255, 255, 255, 255) || !make_pixel(mTransparent, 0, 0, 0, 0))
    {
        return false;
    }

    // the composite program from the build's SPIR-V
    {
        std::vector<uint32_t> vert, frag;
        const std::string delim = gDirUtilp->getDirDelimiter();
        if (!readSpvFile(mSpvDir + delim + "wolfvk_composite.vert.spv", vert) || !readSpvFile(mSpvDir + delim + "wolfvk_composite.frag.spv", frag))
        {
            error = "the composite shaders (shaders/spv/wolfvk_composite.*.spv) are missing: the build did not run the Vulkan shader step (indra/cmake/VulkanShaders.cmake)";
            return false;
        }
        mComposite.reset(new WolfVkProgram());
        if (!mComposite->create(device, "WolfVk composite", vert, frag, error))
        {
            return false;
        }
    }

    // GL pixel pack buffers for the read-backs (OpenGL 4.6 spec 18.2 "Reading Pixels": a bound PIXEL_PACK_BUFFER receives them)
    GLuint pbos[3] = { 0, 0, 0 };
    glGenBuffers(3, pbos);
    mPboWorld = pbos[0];
    mPboUI = pbos[1];
    mPboDepth = pbos[2];

    // [SHARED IMAGES 2026-11-08] the layers shared with GL where the drivers allow it; otherwise the read-backs stay
    {
        std::string why;
        static LLCachedControl<bool> no_share(gSavedSettings, "WolfVulkanDebugNoShare", false);
        if (no_share)
        {
            why = "WolfVulkanDebugNoShare is set";
        }
        else if (WolfVkShare::available(device, why))
        {
            WolfVkShare::logExtensions(device);   // [BLACK WORLD 2026-11-10]
            mShare.reset(new WolfVkShare());
            const uint32_t w = std::max<uint32_t>(1, swapchain.extent().width), h = std::max<uint32_t>(1, swapchain.extent().height);
            // [IMPORT SELF-TEST 2026-11-10] WolfVulkanShareSelfTest: GL's writes must come back through the shared memory
            // before the first frame; a failed round trip means the read-back hand-over for this session
            if (!mShare->init(device, w, h, gSavedSettings.getBOOL("WolfVulkanShareSelfTest"), why))
            {
                LL_WARNS("Vulkan") << "Shared GL/Vulkan layers unavailable, using read-back: " << why << LL_ENDL;
                mShare.reset();
            }
        }
        else
        {
            LL_INFOS("Vulkan") << "Shared GL/Vulkan layers unavailable, using read-back: " << why << LL_ENDL;
        }
        mHandOver = mShare ? "through shared images (nothing is copied)" : "by copying it through the processor (" + why + ")";
    }
    // [BLACK WORLD 2026-11-10] what the package carries: the next log says at once whether the SPIR-V store is there
    {
        const std::string delim = gDirUtilp->getDirDelimiter();
        const U32 spv_files = gDirUtilp->countFilesInDir(mSpvDir + delim, "*.spv");
        const std::string manifest = mSpvDir + delim + "manifest.json";
        llstat st{};
        const S64 manifest_bytes = (LLFile::stat(manifest, &st) == 0) ? static_cast<S64>(st.st_size) : -1;
        LL_INFOS("Vulkan") << "SPIR-V store: " << spv_files << " .spv files in " << mSpvDir << ", manifest.json "
                           << (manifest_bytes < 0 ? std::string("MISSING") : llformat("%lld bytes", static_cast<long long>(manifest_bytes))) << LL_ENDL;
    }

    onSwapchainChanged(swapchain);
    return createCompositePass(swapchain, error);
}

void WolfVkScene::destroy()
{
    if (!mDevice) return;
    if (!mDevice->waitIdle())
    {
        // [HANG GUARD 2026-11-08 8.2k] the bounded wait ran out (or the device is lost): a stuck submit may still use the
        // descriptor pools, samplers and layers, so nothing here is reset or destroyed (review: the result was ignored and
        // the pools reset anyway). The objects stay with the abandoned device; only the GL hooks come off. The members'
        // own destructors only file deferred destroys, which a hung device never runs.
        WolfVkMirror::shutdown();
        (void)mShare.release();
        (void)mScene.release();
        (void)mComposite.release();
        (void)mProbeVkImage.release();   // [BLACK WORLD 2026-11-10] left with the hung device, like the rest
        for (auto& b : mProbeVkHost) (void)b.release();
        mProbeVkHost.clear();
        mCompositeFramebuffers.clear();
        mCompositePass = VK_NULL_HANDLE;
        mDevice = nullptr;
        return;
    }
    mUniforms.resetPools();   // the descriptor sets name images that are about to go (also the deferred ones)
    mDevice->waitIdleAndFlushDeferred();
    probeDestroy();           // [BLACK WORLD 2026-11-10]
    mScene.reset();           // may hold the shared depth as its external attachment
    mShare.reset();
    destroyCompositePass();
    mComposite.reset();
    mScene.reset();
    mWorldImage.reset();
    mUIImage.reset();
    mWhite.reset();
    mTransparent.reset();
    WolfVkMirror::setStagingRing(nullptr);
    WolfVkMirror::shutdown();
    mSamplers.destroy();
    mUniforms.destroy();
    mStaging.destroy();
    mPipelines.destroy();
    if (gGLManager.mInited)
    {
        GLuint pbos[3] = { mPboWorld, mPboUI, mPboDepth };
        glDeleteBuffers(3, pbos);
    }
    mPboWorld = mPboUI = mPboDepth = 0;
    mDevice->waitIdleAndFlushDeferred();
    mDevice = nullptr;
}

bool WolfVkScene::createLayers(uint32_t width, uint32_t height, std::string& error)
{
    WolfVkImage::Format f = WolfVkImage::formatFor(GL_RGBA8, GL_UNSIGNED_BYTE);
    mWorldImage.reset(new WolfVkImage());
    mUIImage.reset(new WolfVkImage());
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!mWorldImage->create(*mDevice, WolfVkImage::Kind::TEXTURE_2D, f, width, height, 1, 1, usage, false, error)) return false;
    if (!mUIImage->create(*mDevice, WolfVkImage::Kind::TEXTURE_2D, f, width, height, 1, 1, usage, false, error)) return false;
    mHaveWorld = mHaveUI = false;
    return true;
}

void WolfVkScene::onSwapchainChanged(WolfVkSwapchain& swapchain)
{
    const uint32_t w = swapchain.extent().width, h = swapchain.extent().height;
    if (w != mWidth || h != mHeight || !mWorldImage)
    {
        mWidth = w;
        mHeight = h;
        std::string error;
        if (!createLayers(w, h, error))
        {
            LL_WARNS("Vulkan") << "layer images: " << error << LL_ENDL;
        }
        if (mShare)
        {
            mScene.reset();   // its depth attachment is the shared depth image, which is about to be remade
            if (!mShare->resize(w, h, error))
            {
                LL_WARNS("Vulkan") << "shared layers could not be resized, using read-back from now on: " << error << LL_ENDL;
                mShare.reset();
                mHandOver = "by copying it through the processor (the shared images could not be resized)";
            }
        }
    }
    if (mComposite && (mCompositeFramebuffers.size() != swapchain.imageCount() || mCompositeFormat != swapchain.format() || mCompositePass == VK_NULL_HANDLE))
    {
        std::string error;
        if (!createCompositePass(swapchain, error))
        {
            LL_WARNS("Vulkan") << "composite pass: " << error << LL_ENDL;
        }
    }
    else if (mComposite && mCompositePass != VK_NULL_HANDLE)
    {
        // same count and format but new images: new framebuffers
        std::string error;
        createCompositePass(swapchain, error);
    }
}

bool WolfVkScene::createCompositePass(WolfVkSwapchain& swapchain, std::string& error)
{
    destroyCompositePass();
    VkDevice dev = mDevice->device();
    // Source: vulkan_core.h:4419 VkAttachmentDescription: the swapchain image, cleared, stored, ending in PRESENT_SRC (:2288)
    VkAttachmentDescription a{};
    a.format = swapchain.format();
    a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    a.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    // the acquire semaphore is waited at COLOR_ATTACHMENT_OUTPUT (wolfvkdevice.cpp endFrame); this dependency makes the
    // layout transition wait for it too (spec 8.1, the usual swapchain external dependency)
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 1;
    info.pAttachments = &a;
    info.subpassCount = 1;
    info.pSubpasses = &sub;
    info.dependencyCount = 1;
    info.pDependencies = &dep;
    VkResult res = gVk.vkCreateRenderPass(dev, &info, nullptr, &mCompositePass);
    if (res != VK_SUCCESS)
    {
        error = std::string("vkCreateRenderPass (composite): ") + WolfVkLoader::resultString(res);
        mCompositePass = VK_NULL_HANDLE;
        return false;
    }
    mCompositeFormat = swapchain.format();
    mCompositeFramebuffers.assign(swapchain.imageCount(), VK_NULL_HANDLE);
    for (uint32_t i = 0; i < swapchain.imageCount(); ++i)
    {
        VkImageView view = swapchain.view(i);
        VkFramebufferCreateInfo fb{};
        fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass = mCompositePass;
        fb.attachmentCount = 1;
        fb.pAttachments = &view;
        fb.width = swapchain.extent().width;
        fb.height = swapchain.extent().height;
        fb.layers = 1;
        res = gVk.vkCreateFramebuffer(dev, &fb, nullptr, &mCompositeFramebuffers[i]);
        if (res != VK_SUCCESS)
        {
            error = std::string("vkCreateFramebuffer (composite): ") + WolfVkLoader::resultString(res);
            destroyCompositePass();
            return false;
        }
    }
    return true;
}

void WolfVkScene::destroyCompositePass()
{
    if (mDevice && mDevice->hasDevice())
    {
        WolfVkDevice* device = mDevice;
        std::vector<VkFramebuffer> fbs = mCompositeFramebuffers;
        VkRenderPass pass = mCompositePass;
        device->deferDestroy([device, fbs, pass]()
        {
            for (VkFramebuffer fb : fbs) if (fb) gVk.vkDestroyFramebuffer(device->device(), fb, nullptr);
            if (pass) gVk.vkDestroyRenderPass(device->device(), pass, nullptr);
        });
    }
    mCompositeFramebuffers.clear();
    mCompositePass = VK_NULL_HANDLE;
}

bool WolfVkScene::ensureSceneTarget(uint32_t width, uint32_t height, std::string& error)
{
    if (mScene && mScene->getWidth() == width && mScene->getHeight() == height) return true;
    mScene.reset(new WolfVkRenderTarget());
    mScene->init(*mDevice);
    // RGBA16F like the viewer's post-deferred screen target (pipeline.cpp:985 mRT->screen GL_RGBA16F); the depth is the
    // shared GL depth when the layers are shared, the target's own (uploaded from the read-back) otherwise
    if (!mScene->allocate(width, height, GL_RGBA16F, mShare == nullptr, false, error)) return false;
    // [COVERAGE 2026-11-08] attachment 1: wolf_coverage (shader_vulkan.py), R8: 1 where the pass drew
    if (!mScene->addColorAttachment(GL_R8, error)) return false;
    if (mShare)
    {
        mScene->setExternalDepth(mShare->depth());
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// GL read-backs

void WolfVkScene::captureWorld()
{
    if (!mDevice || !gViewerWindow) return;
    const S32 ww = gViewerWindow->getWindowWidthRaw();
    const S32 wh = gViewerWindow->getWindowHeightRaw();
    if (ww <= 0 || wh <= 0) return;
    if (mShare)
    {
        // the window changed before the swapchain did: the layers follow the window now, the swapchain catches up at present
        if (mShare->width() != static_cast<uint32_t>(ww) || mShare->height() != static_cast<uint32_t>(wh))
        {
            std::string error;
            mScene.reset();   // its depth attachment is the shared depth image, which is about to be remade
            if (!mShare->resize(static_cast<uint32_t>(ww), static_cast<uint32_t>(wh), error))
            {
                // [HAND-OVER 2026-11-08 8.2k] as onSwapchainChanged does: a share whose layers could not be remade is
                // dropped and this very frame goes the read-back way below. Before, it was kept with invalid layers -
                // GL then neither waited nor signalled while the Vulkan side still acquired, sampled and released them.
                LL_WARNS("Vulkan") << "shared layers could not be resized, using read-back from now on: " << error << LL_ENDL;
                mShare.reset();
                mHandOver = "by copying it through the processor (the shared images could not be resized)";
            }
        }
    }
    if (mShare)
    {
        // [SHARED IMAGES 2026-11-08] GL copies the finished world and the scene depth straight into the shared layers
        const LLRect world_rect = gViewerWindow->getWorldViewRectRaw();
        uint32_t depth_tex = 0, dw = 0, dh = 0;
        if (gPipeline.mRT && gPipeline.mRT->deferredScreen.getDepth())
        {
            LLRenderTarget& ds = gPipeline.mRT->deferredScreen;
            depth_tex = ds.getDepth();
            dw = ds.getWidth();
            dh = ds.getHeight();
        }
        if (!mShare->glCaptureWorld(depth_tex, dw, dh, world_rect))
        {
            // [HAND-OVER 2026-11-08 8.2k] broken (a GL semaphore call failed): WolfVkBridge::present sees handOverBroken()
            // and falls back before any Vulkan submit waits on this frame's hand-over
            mWorldCaptured = false;
            mHaveWorld = false;
            return;
        }
        mWorldRect[0] = world_rect.mLeft;
        mWorldRect[1] = world_rect.mBottom;
        mWorldRect[2] = world_rect.getWidth();
        mWorldRect[3] = world_rect.getHeight();
        mWidth = static_cast<uint32_t>(ww);
        mHeight = static_cast<uint32_t>(wh);
        mDepthWidth = static_cast<uint32_t>(std::max(0, world_rect.getWidth()));
        mDepthHeight = static_cast<uint32_t>(std::max(0, world_rect.getHeight()));
        mHaveDepth = depth_tex != 0;
        mHaveWorld = true;
        // [BLACK WORLD 2026-11-10] the probe's GL sample of the finished world, before the clear below
        mProbeDueThisFrame = false;
        if (mProbeArmed)
        {
            const F64 now = LLTimer::getTotalSeconds();
            if (now - mProbeLastTime >= 1.0 && world_rect.getWidth() >= 32 && world_rect.getHeight() >= 32)
            {
                mProbeLastTime = now;
                mProbe.mSeq++;
                mProbeDueThisFrame = true;
                probeGLSample();
            }
        }
        // the UI goes onto a clear layer, as below
        GLfloat cc[4];
        glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
        GLboolean mask[4];
        glGetBooleanv(GL_COLOR_WRITEMASK, mask);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0.f, 0.f, 0.f, 0.f);
        glClear(GL_COLOR_BUFFER_BIT);
        glClearColor(cc[0], cc[1], cc[2], cc[3]);
        glColorMask(mask[0], mask[1], mask[2], mask[3]);
        mWorldCaptured = true;
        return;
    }
    GLReadbackScope scope;
    // the finished world is in the default framebuffer's back buffer (renderFinalize draws its last pass there)
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, mPboWorld);
    const GLsizeiptr bytes = static_cast<GLsizeiptr>(ww) * wh * 4;
    glBufferData(GL_PIXEL_PACK_BUFFER, bytes, nullptr, GL_STREAM_READ);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, ww, wh, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    mWorldPixels.resize(static_cast<size_t>(bytes));
    // mapped straight away: the read must finish first, so this is where the GPU wait of the hand-over lands (measured
    // in WolfVkBridge::presentMillis; shared images remove it in the next stage)
    void* p = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, bytes, GL_MAP_READ_BIT);
    if (p)
    {
        memcpy(mWorldPixels.data(), p, static_cast<size_t>(bytes));
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        mHaveWorld = true;
    }
    mWidth = static_cast<uint32_t>(ww);   // the layer images follow the window; onSwapchainChanged keeps them matched
    mHeight = static_cast<uint32_t>(wh);
    // [BLACK WORLD 2026-11-10] the probe's sample from the CPU copy (nothing of Vulkan's to compare with on this path)
    mProbeDueThisFrame = false;
    if (mProbeArmed && mHaveWorld)
    {
        const F64 now = LLTimer::getTotalSeconds();
        if (now - mProbeLastTime >= 1.0)
        {
            mProbeLastTime = now;
            mProbe.mSeq++;
            probeGLSampleFromPixels();
        }
    }

    // the scene depth: the deferred screen target's depth texture (GL_DEPTH_COMPONENT24, llrendertarget.cpp:307), read as float
    if (gPipeline.mRT && gPipeline.mRT->deferredScreen.getDepth())
    {
        LLRenderTarget& ds = gPipeline.mRT->deferredScreen;
        mDepthWidth = ds.getWidth();
        mDepthHeight = ds.getHeight();
        const GLsizeiptr dbytes = static_cast<GLsizeiptr>(mDepthWidth) * mDepthHeight * 4;
        glBindTexture(GL_TEXTURE_2D, ds.getDepth());
        glBindBuffer(GL_PIXEL_PACK_BUFFER, mPboDepth);
        glBufferData(GL_PIXEL_PACK_BUFFER, dbytes, nullptr, GL_STREAM_READ);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        mDepthPixels.resize(static_cast<size_t>(dbytes));
        void* d = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, dbytes, GL_MAP_READ_BIT);
        if (d)
        {
            memcpy(mDepthPixels.data(), d, static_cast<size_t>(dbytes));
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            mHaveDepth = true;
        }
    }
    else
    {
        mHaveDepth = false;
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    // the UI goes onto a clear layer: colour (0,0,0,0) so alpha blending leaves premultiplied colour and coverage
    GLfloat cc[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
    GLboolean mask[4];
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(cc[0], cc[1], cc[2], cc[3]);
    glColorMask(mask[0], mask[1], mask[2], mask[3]);
    mWorldCaptured = true;
    if (glGetError() != GL_NO_ERROR)
    {
        LL_WARNS_ONCE("Vulkan") << "captureWorld: a GL error during the read-back" << LL_ENDL;
    }
}

bool WolfVkScene::readBackUI()
{
    const S32 ww = gViewerWindow ? gViewerWindow->getWindowWidthRaw() : 0;
    const S32 wh = gViewerWindow ? gViewerWindow->getWindowHeightRaw() : 0;
    if (ww <= 0 || wh <= 0) return false;
    if (mShare)
    {
        // [SHARED IMAGES 2026-11-08] the UI layer is copied GPU-side and the layers handed to Vulkan with gl_done
        if (!mShare->glCaptureUIAndSignal())
        {
            // [HAND-OVER 2026-11-08 8.2k] nothing was signalled: uploadLayers/recordFrame then neither acquire nor
            // release the layers, frameSync() does not wait on gl_done, and WolfVkBridge::present falls back on
            // handOverBroken() before the submit
            mWorldCaptured = false;
            mHaveWorld = false;
            mHaveUI = false;
            return false;
        }
        if (!mWorldCaptured)
        {
            mHaveWorld = false;   // no world this frame (login screen): the GL frame is all "UI", composited over black
        }
        mWorldCaptured = false;
        mHaveUI = true;
        return true;
    }
    GLReadbackScope scope;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, mPboUI);
    const GLsizeiptr bytes = static_cast<GLsizeiptr>(ww) * wh * 4;
    glBufferData(GL_PIXEL_PACK_BUFFER, bytes, nullptr, GL_STREAM_READ);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, ww, wh, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    mUIPixels.resize(static_cast<size_t>(bytes));
    void* p = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, bytes, GL_MAP_READ_BIT);
    bool ok = false;
    if (p)
    {
        memcpy(mUIPixels.data(), p, static_cast<size_t>(bytes));
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        ok = true;
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    if (!mWorldCaptured)
    {
        // no world capture this frame (login screen, a frame that returned early): the whole GL frame is the "UI" and
        // the world layer stays black; the UI blend (premultiplied over) then shows the frame as GL drew it
        mHaveWorld = false;
    }
    mWorldCaptured = false;
    mHaveUI = ok;
    return ok;
}

void WolfVkScene::uploadLayers(VkCommandBuffer cmd)
{
    std::string error;
    if (mShare)
    {
        // [HAND-OVER 2026-11-08 8.2k] acquired only after GL's release (its glSignalSemaphoreEXT): an acquire with no
        // matching release is invalid (VUID-vkQueueSubmit-pSubmits-02207) and races GL's own writes
        mLayersAcquired = mShare->glSignalledPending();
        if (mLayersAcquired) mShare->acquire(cmd);   // EXTERNAL -> graphics family; world/ui readable, depth an attachment
        return;
    }
    if (mWorldImage && (mWorldImage->width() != mWidth || mWorldImage->height() != mHeight))
    {
        createLayers(mWidth, mHeight, error);
    }
    if (mHaveWorld && mWorldImage && mWorldPixels.size() == static_cast<size_t>(mWidth) * mHeight * 4)
    {
        mWorldImage->upload(cmd, 0, 0, mWidth, mHeight, mWorldPixels.data(), GL_UNSIGNED_BYTE, false, error);
    }
    if (mHaveUI && mUIImage && mUIPixels.size() == static_cast<size_t>(mWidth) * mHeight * 4)
    {
        mUIImage->upload(cmd, 0, 0, mWidth, mHeight, mUIPixels.data(), GL_UNSIGNED_BYTE, false, error);
    }
    // whatever was uploaded (or nothing) is sampled by the composite: make both readable
    for (WolfVkImage* img : { mWorldImage.get(), mUIImage.get() })
    {
        if (!img) continue;
        if (img->layout() == VK_IMAGE_LAYOUT_UNDEFINED)
        {
            // never written: clear to transparent black so it samples as "nothing"
            img->transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            VkClearColorValue zero{};
            VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            gVk.vkCmdClearColorImage(cmd, img->image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
        }
        img->transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// The scene pass: PASS_FULLBRIGHT with the deferred fullbright program

WolfVkShaderMirror* WolfVkScene::mirrorFor(LLGLSLShader* shader)
{
    if (!shader || !shader->isComplete()) return nullptr;
    if (shader->mWolfVk) return shader->mWolfVk;
    // [SPIR-V MISSING 2026-11-08] a program variant without SPIR-V is not tried again until the GL program is remade (a
    // shader reload gives it a new program object and possibly a variant that has SPIR-V). This was one flag for the whole
    // scene: run nv-rl1 changed RenderScreenSpaceReflections, the reloaded Deferred Diffuse Shader had no SPIR-V (key
    // 5a3d2113...) and from then on no shader got a Vulkan mirror again, not even the variant that had one before - the
    // Vulkan G-buffer pass drew nothing for the rest of the session ("gbuffer draws 0"), which is why every grab after the
    // first reload looked clean in nv-bis1 and nv-rl1.
    {
        auto missing = mNoSpirv.find(shader);
        if (missing != mNoSpirv.end() && missing->second == shader->mProgramObject) return nullptr;
    }
    std::string error;
    shader->mWolfVk = WolfVkShaderMirror::create(*mDevice, shader, mSpvDir, error);
    if (!shader->mWolfVk)
    {
        LL_WARNS("Vulkan") << error << LL_ENDL;
        mShaderStatus = "no SPIR-V for \"" + shader->mName + "\" (see the log for its recipe)";
        mNoSpirv[shader] = shader->mProgramObject;
        return nullptr;
    }
    // the GL side sends every uniform again at its next bind, so the Vulkan copy gets them all (llglslshader.cpp bind())
    shader->mUniformsDirty = true;
    mMirrorsMade++;   // [BLACK WORLD 2026-11-10]
    mShaderStatus = "\"" + shader->mName + "\" running on Vulkan (key " + shader->mWolfVk->key().substr(0, 8) + ")";
    return shader->mWolfVk;
}

uint32_t WolfVkScene::drawPass(VkCommandBuffer cmd, uint32_t pass_type, LLGLSLShader* program, WolfVkPipelineState state, WolfVkRenderTarget& target)
{
    WolfVkShaderMirror* mirror = mirrorFor(program);
    if (!mirror) return 0;
    uint32_t draws = 0;
    const glm::mat4 view = glm::make_mat4(gGLModelView);
    const glm::mat4 projection = glm::make_mat4(gGLProjection);
    const glm::mat4 identity(1.f);
    const uint32_t program_mask = mirror->program().attributeMask();
    VkPipeline bound = VK_NULL_HANDLE;

    auto* begin = gPipeline.beginRenderMap(pass_type);
    auto* end = gPipeline.endRenderMap(pass_type);
    for (LLCullResult::drawinfo_iterator i = begin; i != end; )
    {
        LLDrawInfo* params = *i;
        LLCullResult::increment_iterator(i, end);
        if (!params || !params->mCount || params->mVertexBuffer.isNull()) continue;
        LLVertexBuffer* glvb = params->mVertexBuffer.get();
        // every attribute the shader reads must be in the buffer (LLVertexBuffer::setBuffer asserts the same, :1724-1727)
        if ((program_mask & glvb->getTypeMask()) != program_mask) { mSkippedLastFrame++; continue; }
        WolfVkVertexBuffer* vb = WolfVkMirror::vertexBuffer(glvb);
        if (!vb || vb->hasPendingUploads()) { mSkippedLastFrame++; continue; }   // a copy made this frame waits for the next (its upload is after this pass)
        // a draw must stay inside the copy (an out-of-range fetch on the card is a device loss): LLDrawInfo's index range and
        // vertex range against the copy's counts (both set from the GL buffer's when it was made)
        if (static_cast<uint64_t>(params->mOffset) + params->mCount > vb->getNumIndices() || params->mEnd >= vb->getNumVerts())
        {
            mSkippedLastFrame++;
            continue;
        }

        state.mAttributeMask = program_mask;
        VkPipeline pipeline = mPipelines.get(mirror->program(), state, target.renderPass());
        if (pipeline == VK_NULL_HANDLE) { mSkippedLastFrame++; continue; }
        if (pipeline != bound)
        {
            gVk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            bound = pipeline;
        }

        // LLRenderPass::applyModelMatrix (lldrawpool.cpp:558-571): modelview = gGLModelView * model; the shader's
        // modelview_projection_matrix = projection * modelview (LLRender::syncMatrices); texture_matrix0 from the batch
        const glm::mat4 modelview = params->mModelMatrix ? view * glm::make_mat4(&params->mModelMatrix->mMatrix[0][0]) : view;
        const glm::mat4 mvp = projection * modelview;
        const glm::mat4 texmat = params->mTextureMatrix ? glm::make_mat4(&params->mTextureMatrix->mMatrix[0][0]) : identity;
        mirror->set("modelview_matrix", WolfVkShaderMirror::Kind::M4, 1, glm::value_ptr(modelview));
        mirror->set("modelview_projection_matrix", WolfVkShaderMirror::Kind::M4, 1, glm::value_ptr(mvp));
        // LLRender::syncMatrices (llrender.cpp:1052-1077): inv_modelview = inverse(modelview); normal_matrix = the upper-left 3x3 of
        // transpose(inv_modelview) (norm[0..2], norm[4..6], norm[8..10]); the deferred shaders light with them
        const glm::mat4 inv_modelview = glm::inverse(modelview);
        // [NORMALS 2026-11-08] packed by hand, never glm::value_ptr of a glm::mat3: the viewer builds GLM with
        // GLM_FORCE_DEFAULT_ALIGNED_GENTYPES (cmake/00-Common.cmake:37), so a glm::mat3 is three 16-byte columns and its
        // value_ptr floats run c0.xyz pad c1.xyz pad c2.xyz pad; read as 9 contiguous floats the shader got the columns
        // (pad, c1.x, c1.y) and (c1.z, pad, c2.x): garbage normals, black hulls with stray speculars (Paul's report 11-08)
        const glm::mat4 norm_mat4 = glm::transpose(inv_modelview);   // named: value_ptr of a temporary would dangle
        const float* norm = glm::value_ptr(norm_mat4);                // a mat4's columns are 16 bytes either way
        const float normal_matrix[9] = { norm[0], norm[1], norm[2], norm[4], norm[5], norm[6], norm[8], norm[9], norm[10] };
        mirror->set("normal_matrix", WolfVkShaderMirror::Kind::M3, 1, normal_matrix);
        mirror->set("inv_modelview", WolfVkShaderMirror::Kind::M4, 1, glm::value_ptr(inv_modelview));
        mirror->set("texture_matrix0", WolfVkShaderMirror::Kind::M4, 1, glm::value_ptr(texmat));
        if (mProbeThisFrame && draws == 0 && pass_type == LLRenderPass::PASS_FULLBRIGHT) probeBatch(params, glvb, vb, mirror, mvp);
        if (!mirror->bindUniforms(cmd, mUniforms)) { mSkippedLastFrame++; break; }

        // textures: LLRenderPass::pushBatch (lldrawpool.cpp:573-633) binds mTextureList[i] to unit i when batching, else
        // mTexture to unit 0; the indexed shaders sample tex0..texN by the vertex's texture_index
        std::vector<VkDescriptorImageInfo> images;
        VkDescriptorImageInfo fallback{ mSamplers.get(WolfVkSamplerCache::TFO_BILINEAR, WolfVkSamplerCache::TAM_WRAP, false), mWhite->view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        auto add = [&](LLViewerTexture* tex)
        {
            VkDescriptorImageInfo info = fallback;
            if (tex && tex->getGLTexture())
            {
                WolfVkMirror::Texture t = WolfVkMirror::texture(tex->getGLTexture(), VK_NULL_HANDLE);
                if (t.mImage && t.mImage->layout() == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                {
                    info = { t.mSampler, t.mImage->view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                }
            }
            images.push_back(info);
        };
        if (params->mTextureList.size() > 1)
        {
            for (const auto& t : params->mTextureList) add(t.get());
        }
        else
        {
            add(params->mTexture.get());
        }
        // a batch with one texture: every texN samples it (the vertices' texture_index is 0, but a stray index must not sample nothing)
        while (images.size() < 4) images.push_back(images.front());
        if (!mirror->bindSamplers(cmd, mUniforms, images, fallback)) { mSkippedLastFrame++; break; }

        vb->bind(cmd, program_mask);
        vb->drawRange(cmd, params->mCount, params->mOffset);
        mDrawnByVulkan.insert(params);
        draws++;
        // [POSE CHECK 2026-11-08] what this draw used: the model matrix's values (by the pointer GL dereferences again in
        // LLRenderPass::applyModelMatrix, lldrawpool.cpp:559-574), the view, and the vertex/index range of the copy
        static LLCachedControl<bool> pose_check(gSavedSettings, "WolfVulkanDebugPoseCheck", false);
        if (pose_check)
        {
            if (params->mModelMatrix)
            {
                std::array<float, 16> m;
                memcpy(m.data(), &params->mModelMatrix->mMatrix[0][0], sizeof(float) * 16);
                mPoseMatrices[params->mModelMatrix] = m;
            }
            if (!mPoseViewSet) { memcpy(mPoseView, gGLModelView, sizeof(mPoseView)); mPoseViewSet = true; }
            if (mPoseRecords.size() < 8192) mPoseRecords.push_back({ params->mVertexBuffer, params->mStart, params->mEnd, params->mOffset, params->mCount, params->mModelMatrix });
            WolfVkMirror::poseNoteDrawn(glvb);
            mPoseBatches++;
        }
    }
    return draws;
}

void WolfVkScene::preparePass(VkCommandBuffer cmd, uint32_t pass_type, LLGLSLShader* program)
{
    WolfVkShaderMirror* mirror = mirrorFor(program);
    if (!mirror) return;
    const uint32_t program_mask = mirror->program().attributeMask();
    auto* begin = gPipeline.beginRenderMap(pass_type);
    auto* end = gPipeline.endRenderMap(pass_type);
    for (LLCullResult::drawinfo_iterator i = begin; i != end; )
    {
        LLDrawInfo* params = *i;
        LLCullResult::increment_iterator(i, end);
        if (!params || !params->mCount || params->mVertexBuffer.isNull()) continue;
        LLVertexBuffer* glvb = params->mVertexBuffer.get();
        if ((program_mask & glvb->getTypeMask()) != program_mask) continue;
        WolfVkMirror::vertexBuffer(glvb);   // made (copied from the GL buffer) if new; its upload is recorded below
        if (params->mTextureList.size() > 1)
        {
            for (const auto& t : params->mTextureList) if (t.notNull() && t->getGLTexture()) WolfVkMirror::texture(t->getGLTexture(), VK_NULL_HANDLE);
        }
        else if (params->mTexture.notNull() && params->mTexture->getGLTexture())
        {
            WolfVkMirror::texture(params->mTexture->getGLTexture(), VK_NULL_HANDLE);
        }
    }
    WolfVkMirror::recordUploads(cmd);   // outside the render pass: vkCmdCopyBuffer is not allowed inside one
}

uint32_t WolfVkScene::drawGBufferSimple(VkCommandBuffer cmd, WolfVkRenderTarget& gbuffer)
{
    // LLDrawPoolSimple::renderDeferred (lldrawpoolsimple.cpp:99-111): LLGLDisable blend, gDeferredDiffuseProgram, the static
    // PASS_SIMPLE batches; the deferred loop's state around it (pipeline.cpp:4328 LLGLEnable cull, :4352 setColorMask(true, true));
    // depth: LLGLDepthTest default LEQUAL with writes (the G-buffer pass writes depth)
    mGBufferDraws = 0;
    preparePass(cmd, LLRenderPass::PASS_SIMPLE, &gDeferredDiffuseProgram);
    WolfVkPipelineState state;
    state.mBlendEnable = false;
    state.mDepthTest = true;
    state.mDepthWrite = true;
    state.mDepthFunc = GL_LEQUAL;
    state.mCullFace = true;
    state.mWireframe = gUseWireframe;
    state.mTopology = static_cast<uint8_t>(LLRender::TRIANGLES);
    state.mColorAttachments = gbuffer.getNumTextures();   // the four G-buffer attachments; wolf_coverage (location 4) has none and is discarded
    state.mCoverageAttachment = false;
    const float clear[4] = { 0.f, 0.f, 0.f, 0.f };
    const uint32_t skipped_before = mSkippedLastFrame;
    gbuffer.begin(cmd, false, false, clear, 1.0f);   // LOAD: GL cleared it and drew the pools before this one
    mGBufferDraws = drawPass(cmd, LLRenderPass::PASS_SIMPLE, &gDeferredDiffuseProgram, state, gbuffer);
    gbuffer.end(cmd);
    mGBufferComplete = (mSkippedLastFrame == skipped_before);
    return mGBufferDraws;
}

void WolfVkScene::drawFullbright(VkCommandBuffer cmd)
{
    mDrawsLastFrame = 0;
    mSkippedLastFrame = 0;
    static LLCachedControl<bool> no_pass(gSavedSettings, "WolfVulkanDebugNoScenePass", false);
    if (no_pass) return;   // debugging: the hand-over and composite alone
    // [ONE RENDERER PER BATCH 2026-11-08] the fullbright OVERLAY (Vulkan drawing the unlit surfaces again, after GL's
    // post-processing, onto its own layer) was the proof stage; it is off unless asked for (WolfVulkanOverlayFullbright)
    // because the surfaces come out without the viewer's tone map, and when on, GL skips the batches it draws
    static LLCachedControl<bool> overlay(gSavedSettings, "WolfVulkanOverlayFullbright", false);
    if (!overlay) return;
    if (!mHaveDepth || mDepthWidth == 0 || mDepthHeight == 0) return;
    std::string error;
    // shared layers: the target is window-sized (its depth IS the shared depth) and the pass draws into the world view
    // rect, as GL's gGLViewport does (pipeline.cpp renderFinalize); read-back: the target is the deferred screen's size
    const bool shared = mShare != nullptr;
    if (!ensureSceneTarget(shared ? mWidth : mDepthWidth, shared ? mHeight : mDepthHeight, error))
    {
        LL_WARNS_ONCE("Vulkan") << "scene target: " << error << LL_ENDL;
        return;
    }
    WolfVkShaderMirror* mirror = mirrorFor(&gDeferredFullbrightProgram);

    if (!shared)
    {
        // the GL depth into the scene target's depth image, then the depth attachment loaded (begin with clear_depth = false)
        WolfVkImage* depth = mScene->depthImage();
        if (depth && mDepthPixels.size() == static_cast<size_t>(mDepthWidth) * mDepthHeight * 4)
        {
            depth->upload(cmd, 0, 0, mDepthWidth, mDepthHeight, mDepthPixels.data(), GL_FLOAT, false, error);
            depth->transition(cmd, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        }
    }
    preparePass(cmd, LLRenderPass::PASS_FULLBRIGHT, &gDeferredFullbrightProgram);

    const float clear[4] = { 0.f, 0.f, 0.f, 0.f };
    VkRect2D viewport{ { mWorldRect[0], mWorldRect[1] }, { static_cast<uint32_t>(std::max(0, mWorldRect[2])), static_cast<uint32_t>(std::max(0, mWorldRect[3])) } };
    mScene->begin(cmd, true, false, clear, 1.0f, shared ? &viewport : nullptr);

    if (mirror)
    {
        // state: wolfvkscene.h (renderGeomPostDeferred's: blend off, depth LEQUAL + write, cull back). One difference from
        // the GL pass's setColorMask(true, false): this pass draws into its own layer that the composite lays over the GL
        // world by coverage (attachment 1), so every channel is written here.
        // debugging knobs (settings.xml WolfVulkanDebugNoDepth / WolfVulkanDebugNoCull): the two tests that can hide
        // everything, switchable without a build so a run tells which one does
        static LLCachedControl<bool> no_depth(gSavedSettings, "WolfVulkanDebugNoDepth", false);
        static LLCachedControl<bool> no_cull(gSavedSettings, "WolfVulkanDebugNoCull", false);
        WolfVkPipelineState state;
        state.mBlendEnable = false;
        state.mColorMask[3] = true;
        state.mDepthTest = !no_depth;
        state.mDepthWrite = !no_depth;
        state.mDepthFunc = GL_LEQUAL;
        state.mCullFace = !no_cull;
        state.mWireframe = gUseWireframe;
        state.mTopology = static_cast<uint8_t>(LLRender::TRIANGLES);
        state.mColorAttachments = 2;          // colour + wolf_coverage
        state.mCoverageAttachment = true;
        // this pass re-draws surfaces GL has already put in the depth buffer: a small offset towards the camera stops the
        // z-fight between GL's and Vulkan's depth of the same triangle (hatched signs, Sandbox pictures 14:49Z)
        state.mDepthBiasConstant = -2.f;
        state.mDepthBiasSlope = -1.f;
        mDrawsLastFrame = drawPass(cmd, LLRenderPass::PASS_FULLBRIGHT, &gDeferredFullbrightProgram, state, *mScene);
    }
    mScene->end(cmd);
}

// ---------------------------------------------------------------------------------------------------------------
// WolfVulkanDebugProbe

void WolfVkScene::probeCoverage()
{
    if (!mScene || !mScene->image(0)) return;
    std::vector<uint8_t> px;
    std::string error;
    if (!mScene->readPixels(0, px, error))
    {
        LL_WARNS("Vulkan") << "probe: scene layer read-back failed: " << error << LL_ENDL;
        return;
    }
    // the layer is RGBA16F (createLayers): 8 bytes a pixel, alpha = half-float at +6. A half is > 0 when its sign bit is
    // clear and any other bit is set (IEEE 754-2008 binary16).
    const size_t count = px.size() / 8;
    size_t covered = 0, rgb_any = 0, first = count;
    auto positive = [](uint16_t h) { return (h & 0x8000u) == 0 && (h & 0x7fffu) != 0; };
    for (size_t i = 0; i < count; ++i)
    {
        const uint16_t r = static_cast<uint16_t>(px[i * 8 + 0] | (px[i * 8 + 1] << 8));
        const uint16_t g = static_cast<uint16_t>(px[i * 8 + 2] | (px[i * 8 + 3] << 8));
        const uint16_t b = static_cast<uint16_t>(px[i * 8 + 4] | (px[i * 8 + 5] << 8));
        const uint16_t a = static_cast<uint16_t>(px[i * 8 + 6] | (px[i * 8 + 7] << 8));
        if (positive(r) || positive(g) || positive(b)) ++rgb_any;
        if (positive(a))
        {
            if (first == count) first = i;
            ++covered;
        }
    }
    const uint32_t w = mScene->getWidth();
    LL_INFOS("Vulkan") << "probe: scene layer " << w << "x" << mScene->getHeight() << ", alpha>0 at " << covered << ", rgb>0 at " << rgb_any << " of " << count
                       << " pixels after the last frame's " << mDrawsLastFrame << " draws (" << mSkippedLastFrame
                       << " skipped)" << (first < count && w ? llformat(", first at (%u, %u)", static_cast<unsigned>(first % w), static_cast<unsigned>(first / w)) : std::string(""))
                       << LL_ENDL;
}

void WolfVkScene::probeBatch(const LLDrawInfo* params, const LLVertexBuffer* glvb, const WolfVkVertexBuffer* vb, const WolfVkShaderMirror* mirror, const glm::mat4& mvp)
{
    // what the Vulkan draw really gets: the matrix bytes in the uniform image, the mirror's copy of vertex 0 and of the
    // first three indices (host staging bytes = what was uploaded), the mirror's layout against GL's
    {
        std::ostringstream u;
        std::vector<float> f; std::string where;
        if (mirror && mirror->debugMember("modelview_projection_matrix", f, where))
        {
            u << "probe: uniform image modelview_projection_matrix at " << where << "=";
            for (float v : f) u << " " << v;
        }
        else u << "probe: modelview_projection_matrix is NOT a block member of the mirror";
        u << "; mvp given col0 (" << mvp[0][0] << ", " << mvp[0][1] << ", " << mvp[0][2] << ", " << mvp[0][3] << ") col3 (" << mvp[3][0] << ", " << mvp[3][1] << ", " << mvp[3][2] << ", " << mvp[3][3] << ")";
        LL_INFOS("Vulkan") << u.str() << LL_ENDL;
    }
    if (vb)
    {
        std::ostringstream m;
        m << "probe: mirror buffer size " << vb->getSize() << " (GL " << glvb->getSize() << ") indices " << vb->getIndicesSize() << " (GL " << glvb->getIndicesSize()
          << ") pending " << (vb->hasPendingUploads() ? "yes" : "no") << " offsets vertex " << vb->getOffset(WolfVkVertexBuffer::TYPE_VERTEX) << "/" << glvb->getOffset(LLVertexBuffer::TYPE_VERTEX)
          << " texcoord0 " << vb->getOffset(WolfVkVertexBuffer::TYPE_TEXCOORD0) << "/" << glvb->getOffset(LLVertexBuffer::TYPE_TEXCOORD0)
          << " color " << vb->getOffset(WolfVkVertexBuffer::TYPE_COLOR) << "/" << glvb->getOffset(LLVertexBuffer::TYPE_COLOR);
        const uint8_t* host = vb->getMappedData();
        const uint8_t* hidx = vb->getMappedIndices();
        if (host && vb->getSize() >= vb->getOffset(WolfVkVertexBuffer::TYPE_VERTEX) + 16)
        {
            float p[4]; memcpy(p, host + vb->getOffset(WolfVkVertexBuffer::TYPE_VERTEX) + static_cast<size_t>(params->mStart) * 16, 16);
            m << "; mirror vertex " << params->mStart << " (" << p[0] << ", " << p[1] << ", " << p[2] << ", w-bits " << p[3] << ")";
        }
        if (hidx && vb->getIndicesSize() >= (static_cast<size_t>(params->mOffset) + 3) * 2)
        {
            uint16_t i3[3]; memcpy(i3, hidx + static_cast<size_t>(params->mOffset) * 2, 6);
            m << "; mirror indices [" << i3[0] << ", " << i3[1] << ", " << i3[2] << "]";
            if (host && vb->getSize() >= vb->getOffset(WolfVkVertexBuffer::TYPE_VERTEX) + (static_cast<size_t>(i3[0]) + 1) * 16)
            {
                float q[3]; memcpy(q, host + vb->getOffset(WolfVkVertexBuffer::TYPE_VERTEX) + static_cast<size_t>(i3[0]) * 16, 12);
                const glm::vec4 c = mvp * glm::vec4(q[0], q[1], q[2], 1.f);
                m << " first indexed vertex (" << q[0] << ", " << q[1] << ", " << q[2] << ") -> NDC (" << (c.w != 0.f ? c.x / c.w : 0.f) << ", " << (c.w != 0.f ? c.y / c.w : 0.f) << ", " << (c.w != 0.f ? c.z / c.w : 0.f) << ")";
            }
        }
        if (host && hidx && vb->getIndicesSize() >= (static_cast<size_t>(params->mOffset) + 1) * 2)
        {
            uint16_t i0 = 0; memcpy(&i0, hidx + static_cast<size_t>(params->mOffset) * 2, 2);
            const size_t cat = vb->getOffset(WolfVkVertexBuffer::TYPE_COLOR) + static_cast<size_t>(i0) * 4;
            if (cat + 4 <= vb->getSize())
            {
                m << "; mirror colour of that vertex rgba (" << int(host[cat]) << ", " << int(host[cat + 1]) << ", " << int(host[cat + 2]) << ", " << int(host[cat + 3]) << ")";
            }
        }
        LL_INFOS("Vulkan") << m.str() << LL_ENDL;
    }
    // the batch's texture as the Vulkan draw samples it: the mirrored image and its level-0 alpha
    {
        LLViewerTexture* tex = params->mTextureList.size() > 1 ? params->mTextureList[0].get() : params->mTexture.get();
        std::ostringstream t;
        if (!tex || !tex->getGLTexture()) t << "probe: batch texture: none (white fallback)";
        else
        {
            LLImageGL* gl = tex->getGLTexture();
            t << "probe: batch texture GL " << gl->getTexName() << " " << gl->getWidth() << "x" << gl->getHeight() << " internal 0x" << std::hex << gl->getPrimaryFormat() << std::dec
              << " components " << int(gl->getComponents());
            WolfVkMirror::Texture mt = WolfVkMirror::texture(gl, VK_NULL_HANDLE);
            if (!mt.mImage) t << "; NOT mirrored (white fallback)";
            else
            {
                t << "; mirror " << WolfVkImage::formatName(mt.mImage->format().mFormat) << " " << mt.mImage->width() << "x" << mt.mImage->height() << " levels " << mt.mImage->mipLevels()
                  << " layout " << mt.mImage->layout() << " texel bytes " << mt.mImage->format().mTexelBytes << " block " << mt.mImage->format().mBlockBytes;
                std::vector<uint8_t> px; std::string error;
                if (mt.mImage->format().mBlockBytes == 0 && mt.mImage->format().mTexelBytes == 4 && mt.mImage->readBack(px, error))
                {
                    size_t n = px.size() / 4, a_nonzero = 0, rgb_nonzero = 0; uint64_t a_sum = 0;
                    for (size_t i = 0; i < n; ++i) { a_sum += px[i * 4 + 3]; if (px[i * 4 + 3]) ++a_nonzero; if (px[i * 4] | px[i * 4 + 1] | px[i * 4 + 2]) ++rgb_nonzero; }
                    t << "; level 0 read back: alpha>0 at " << a_nonzero << " of " << n << " texels (mean alpha " << (n ? a_sum / n : 0) << "), rgb>0 at " << rgb_nonzero;
                }
                else if (!error.empty()) t << "; read-back: " << error;
            }
        }
        LL_INFOS("Vulkan") << t.str() << LL_ENDL;
    }
    // the first vertex of the batch, straight from the GL buffer (llvertexbuffer.h getOffset: byte offset of the position
    // array; positions are 3 floats, LLVertexBuffer::sTypeSize[TYPE_VERTEX] = 12), pushed through the matrix the Vulkan
    // draw uses, so the log shows where it lands: clip w, GL NDC, the window pixel and GL's depth there
    // [CI 2026-11-09] read through GL_COPY_READ_BUFFER (GL 3.1) and glGetBufferSubData (GL 1.5), not the GL 4.5
    // glGetNamedBufferSubData: Apple's GL is 4.1 and has no such symbol (macOS CI run 42614243966: undefined
    // _glGetNamedBufferSubData at link). The copy-read binding is put back, so LLVertexBuffer's own bindings are untouched.
    if (!glvb->getGLBuffer()) return;
    float pos[3] = { 0.f, 0.f, 0.f };
    const GLintptr offset = static_cast<GLintptr>(glvb->getOffset(LLVertexBuffer::TYPE_VERTEX)) + static_cast<GLintptr>(params->mStart) * 12;
    GLint saved_copy_read = 0;
    glGetIntegerv(GL_COPY_READ_BUFFER_BINDING, &saved_copy_read);
    glBindBuffer(GL_COPY_READ_BUFFER, glvb->getGLBuffer());
    glGetBufferSubData(GL_COPY_READ_BUFFER, offset, 12, pos);
    glBindBuffer(GL_COPY_READ_BUFFER, static_cast<GLuint>(saved_copy_read));
    const glm::vec4 clip = mvp * glm::vec4(pos[0], pos[1], pos[2], 1.f);
    std::ostringstream s;
    s << "probe: batch count " << params->mCount << " offset " << params->mOffset << " start " << params->mStart << " end " << params->mEnd
      << " types 0x" << std::hex << glvb->getTypeMask() << std::dec << " gl buffer " << glvb->getGLBuffer()
      << " model " << (params->mModelMatrix ? "yes" : "no") << " textures " << params->mTextureList.size()
      << "; vertex " << params->mStart << " at (" << pos[0] << ", " << pos[1] << ", " << pos[2] << ") -> clip (" << clip.x << ", " << clip.y << ", " << clip.z << ", w " << clip.w << ")";
    if (clip.w > 0.f)
    {
        const float nx = clip.x / clip.w, ny = clip.y / clip.w, nz = clip.z / clip.w;
        const int px = static_cast<int>((nx * 0.5f + 0.5f) * static_cast<float>(mDepthWidth));
        const int py = static_cast<int>((ny * 0.5f + 0.5f) * static_cast<float>(mDepthHeight));
        s << " NDC (" << nx << ", " << ny << ", " << nz << ") window (" << px << ", " << py << ") GL depth would be " << (nz * 0.5f + 0.5f);
        if (px >= 0 && py >= 0 && static_cast<uint32_t>(px) < mDepthWidth && static_cast<uint32_t>(py) < mDepthHeight
            && mDepthPixels.size() >= static_cast<size_t>(mDepthWidth) * mDepthHeight * 4)
        {
            float d = 0.f;
            memcpy(&d, mDepthPixels.data() + (static_cast<size_t>(py) * mDepthWidth + static_cast<size_t>(px)) * 4, 4);
            s << ", GL depth buffer there " << d;
        }
    }
    LL_INFOS("Vulkan") << s.str() << LL_ENDL;
}

// ---------------------------------------------------------------------------------------------------------------
// The composite into the swapchain image

void WolfVkScene::drawComposite(VkCommandBuffer cmd, WolfVkSwapchain& swapchain, uint32_t image_index)
{
    if (!mComposite || mCompositePass == VK_NULL_HANDLE || image_index >= mCompositeFramebuffers.size()) return;

    VkClearValue clear{};
    clear.color.float32[3] = 1.f;
    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass = mCompositePass;
    begin.framebuffer = mCompositeFramebuffers[image_index];
    begin.renderArea = { { 0, 0 }, swapchain.extent() };
    begin.clearValueCount = 1;
    begin.pClearValues = &clear;
    gVk.vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);

    // the composite triangle is written for Vulkan's own y (down), so the viewport is the plain one here
    VkViewport vp{ 0.f, 0.f, static_cast<float>(swapchain.extent().width), static_cast<float>(swapchain.extent().height), 0.f, 1.f };
    gVk.vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D scissor{ { 0, 0 }, swapchain.extent() };
    gVk.vkCmdSetScissor(cmd, 0, 1, &scissor);

    WolfVkPipelineState state;
    state.mBlendEnable = false;
    state.mDepthTest = false;
    state.mDepthWrite = false;
    state.mCullFace = false;
    state.mTopology = static_cast<uint8_t>(LLRender::TRIANGLES);
    state.mAttributeMask = 0;
    state.mColorAttachments = 1;
    VkPipeline pipeline = mPipelines.get(*mComposite, state, mCompositePass);
    if (pipeline != VK_NULL_HANDLE)
    {
        gVk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

        // set 0: the fragment block (wolf_tint, wolf_flags) by reflected offsets
        const WolfVkProgram::Stage& frag = mComposite->fragment();
        // [VR 3D POOL 2026-11-09] the draw needs every set its layout declares: a failed push or descriptor allocation
        // (VK_ERROR_OUT_OF_POOL_MEMORY: WolfVkFrameUniforms::allocate returns VK_NULL_HANDLE) skips the draw this frame
        // instead of writing to and binding a null set (the NVIDIA SIGSEGV of the VR 3D Mode crash, 2026-11-09)
        bool uniforms_bound = !frag.mUniforms;
        if (frag.mUniforms)
        {
            std::vector<uint8_t> block(frag.mUniforms->mSize, 0);
            static LLCachedControl<bool> tint(gSavedSettings, "WolfVulkanDebugTint", false);
            static LLCachedControl<bool> only(gSavedSettings, "WolfVulkanShowSceneOnly", false);
            for (const WolfVkSpirv::BlockMember& m : frag.mUniforms->mMembers)
            {
                float v[4] = { 0.f, 0.f, 0.f, 0.f };
                if (m.mName == "wolf_tint") { v[0] = 1.f; v[1] = 0.35f; v[2] = 1.f; v[3] = tint ? 1.f : 0.f; }
                if (m.mName == "wolf_flags") { v[0] = swapchain.isSrgb() ? 1.f : 0.f; v[1] = only ? 1.f : 0.f; }
                if (m.mOffset + 16 <= block.size()) memcpy(block.data() + m.mOffset, v, 16);
            }
            VkDeviceSize offset = 0;
            VkDescriptorSet set = VK_NULL_HANDLE;
            if (mUniforms.push(block.data(), block.size(), offset)
                && (set = mUniforms.allocate(mComposite->uniformSetLayout())) != VK_NULL_HANDLE)
            {
                VkDescriptorBufferInfo bi{ mUniforms.buffer(), 0, block.size() };
                VkWriteDescriptorSet w{};
                w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w.dstSet = set;
                w.dstBinding = WolfVkProgram::BINDING_UNIFORMS_FRAG;
                w.descriptorCount = 1;
                w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
                w.pBufferInfo = &bi;
                gVk.vkUpdateDescriptorSets(mDevice->device(), 1, &w, 0, nullptr);
                const uint32_t dyn = static_cast<uint32_t>(offset);
                gVk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mComposite->pipelineLayout(), WolfVkProgram::SET_UNIFORMS, 1, &set, 1, &dyn);
                uniforms_bound = true;
            }
        }
        // set 1: worldMap 0, sceneMap 1, uiMap 2 (compositeF.glsl), linear clamp samplers (the layers are screen-sized)
        VkSampler linear = mSamplers.get(WolfVkSamplerCache::TFO_BILINEAR, WolfVkSamplerCache::TAM_CLAMP, false);
        VkDescriptorImageInfo fallback{ linear, mWhite->view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        std::vector<VkDescriptorImageInfo> by_binding(4, fallback);
        WolfVkImage* world = mShare ? mShare->world() : mWorldImage.get();
        WolfVkImage* ui = mShare ? mShare->ui() : mUIImage.get();
        // a shared world layer with no world this frame (login screen) holds whatever GL left; the GL frame is then all "UI"
        if (mShare && !mHaveWorld) world = nullptr;
        by_binding[0] = { linear, (world && world->layout() == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) ? world->view() : mTransparent->view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        WolfVkImage* scene = mScene ? mScene->image(0) : nullptr;
        WolfVkImage* coverage = mScene ? mScene->image(1) : nullptr;
        // without a scene target this frame (login screen, no depth yet) the scene layer contributes nothing: coverage 0
        by_binding[1] = { linear, (scene && scene->layout() == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) ? scene->view() : mTransparent->view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        if (ui && ui->layout() == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) by_binding[2] = { linear, ui->view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        else by_binding[2] = { linear, mTransparent->view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        by_binding[3] = { linear, (coverage && coverage->layout() == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && mDrawsLastFrame > 0) ? coverage->view() : mTransparent->view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorSet set = uniforms_bound ? mUniforms.allocate(mComposite->samplerSetLayout()) : VK_NULL_HANDLE;
        if (set != VK_NULL_HANDLE)
        {
            VkWriteDescriptorSet writes[4]{};
            for (uint32_t b = 0; b < 4; ++b)
            {
                writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[b].dstSet = set;
                writes[b].dstBinding = b;
                writes[b].descriptorCount = 1;
                writes[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[b].pImageInfo = &by_binding[b];
            }
            gVk.vkUpdateDescriptorSets(mDevice->device(), 4, writes, 0, nullptr);
            gVk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mComposite->pipelineLayout(), WolfVkProgram::SET_SAMPLERS, 1, &set, 0, nullptr);
            gVk.vkCmdDraw(cmd, 3, 1, 0, 0);   // the full-screen triangle (compositeV.glsl)
        }
    }
    gVk.vkCmdEndRenderPass(cmd);
}

void WolfVkScene::beginFrame()
{
    mUniforms.beginFrame(static_cast<uint32_t>(mDevice->frameCounter()));
    mStaging.beginFrame(static_cast<uint32_t>(mDevice->frameCounter()));
    probeVkCollect();   // [BLACK WORLD 2026-11-10] this slot's fence has been waited: its probe buffer is final
}

// ---------------------------------------------------------------------------------------------------------------
// [BLACK WORLD 2026-11-10] the world probe (wolfvkscene.h WorldProbe)

void WolfVkScene::setProbeArmed(bool armed)
{
    if (armed == mProbeArmed) return;
    mProbeArmed = armed;
    mProbeLastTime = 0.0;
    mProbeDueThisFrame = false;
    if (!armed) probeDestroy();
}

namespace
{
    // the largest R/G/B over count RGBA8 pixels, and how many have any of R/G/B > 0
    void maxRGB(const uint8_t* px, size_t count, uint32_t& max, uint32_t& lit)
    {
        max = 0;
        lit = 0;
        for (size_t i = 0; i < count; ++i)
        {
            const uint8_t m = std::max(px[i * 4], std::max(px[i * 4 + 1], px[i * 4 + 2]));
            if (m > max) max = m;
            if (m) ++lit;
        }
    }
}

void WolfVkScene::probeGLSample()
{
    // every probe: the sample issued two probes ago (into this PBO) is read - two seconds old, so the map never waits
    // for the GPU - then this probe's blit + read are issued into the same PBO
    const int k = mProbeGlNext;
    mProbeGlNext = 1 - mProbeGlNext;
    GLint saved_read = 0, saved_draw = 0, saved_pack = 0, saved_tex = 0, saved_read_buffer = 0, saved_pack_align = 4;
    GLboolean saved_mask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &saved_read);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &saved_draw);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &saved_pack);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &saved_tex);
    glGetIntegerv(GL_READ_BUFFER, &saved_read_buffer);
    glGetIntegerv(GL_PACK_ALIGNMENT, &saved_pack_align);
    glGetBooleanv(GL_COLOR_WRITEMASK, saved_mask);
    const GLboolean saved_scissor = glIsEnabled(GL_SCISSOR_TEST);
    while (glGetError() != GL_NO_ERROR) {}
    if (!mProbeGlTex)
    {
        glGenTextures(1, &mProbeGlTex);
        glBindTexture(GL_TEXTURE_2D, mProbeGlTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(PROBE_SIZE), static_cast<GLsizei>(PROBE_SIZE), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glGenFramebuffers(1, &mProbeGlFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, mProbeGlFbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mProbeGlTex, 0);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        const GLenum status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
        glGenBuffers(2, mProbeGlPbo);
        if (status != GL_FRAMEBUFFER_COMPLETE)
        {
            LL_WARNS("WolfVulkanCheck") << "world probe: the 32x32 GL framebuffer is incomplete (0x" << std::hex << status << std::dec << "); no GL samples" << LL_ENDL;
            probeDestroy();
            mProbeArmed = false;
            return;
        }
    }
    // collect the sample issued into this PBO two probes ago
    if (mProbeGlPending[k])
    {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, mProbeGlPbo[k]);
        const void* p = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, static_cast<GLsizeiptr>(PROBE_SIZE * PROBE_SIZE * 4), GL_MAP_READ_BIT);
        if (p)
        {
            maxRGB(static_cast<const uint8_t*>(p), PROBE_SIZE * PROBE_SIZE, mProbe.mGlMax, mProbe.mGlLit);
            mProbe.mGlSeq = mProbeGlPending[k];
            mProbe.mGlValid = true;
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        }
        mProbeGlPending[k] = 0;
    }
    // this probe: the world view rect of the default framebuffer -> 32x32 by a linear blit (OpenGL 4.6 18.3.2: one
    // 2x2 bilinear tap per destination pixel, so 1024 points spread over the rect), then read into the PBO
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, mProbeGlFbo);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBlitFramebuffer(mWorldRect[0], mWorldRect[1], mWorldRect[0] + mWorldRect[2], mWorldRect[1] + mWorldRect[3],
                      0, 0, static_cast<GLint>(PROBE_SIZE), static_cast<GLint>(PROBE_SIZE), GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, mProbeGlFbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, mProbeGlPbo[k]);
    glBufferData(GL_PIXEL_PACK_BUFFER, static_cast<GLsizeiptr>(PROBE_SIZE * PROBE_SIZE * 4), nullptr, GL_STREAM_READ);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, static_cast<GLsizei>(PROBE_SIZE), static_cast<GLsizei>(PROBE_SIZE), GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    mProbeGlPending[k] = mProbe.mSeq;
    const GLenum e = glGetError();
    if (e != GL_NO_ERROR) LL_WARNS_ONCE("WolfVulkanCheck") << "world probe: GL error 0x" << std::hex << e << std::dec << " in the GL sample" << LL_ENDL;
    // state back
    glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(saved_pack));
    glPixelStorei(GL_PACK_ALIGNMENT, saved_pack_align);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(saved_read));
    glReadBuffer(static_cast<GLenum>(saved_read_buffer));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(saved_draw));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(saved_tex));
    glColorMask(saved_mask[0], saved_mask[1], saved_mask[2], saved_mask[3]);
    if (saved_scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
}

void WolfVkScene::probeGLSampleFromPixels()
{
    // read-back hand-over: mWorldPixels is the whole window as GL drew it; 1024 points of the world view rect
    if (!gViewerWindow || mWorldPixels.size() < static_cast<size_t>(mWidth) * mHeight * 4) return;
    const LLRect rect = gViewerWindow->getWorldViewRectRaw();
    const int32_t rw = rect.getWidth(), rh = rect.getHeight();
    if (rw < 32 || rh < 32) return;
    uint32_t max = 0, lit = 0;
    for (uint32_t j = 0; j < PROBE_SIZE; ++j)
    {
        for (uint32_t i = 0; i < PROBE_SIZE; ++i)
        {
            const int32_t x = rect.mLeft + static_cast<int32_t>((static_cast<int64_t>(i) * rw) / PROBE_SIZE) + rw / (2 * static_cast<int32_t>(PROBE_SIZE));
            const int32_t y = rect.mBottom + static_cast<int32_t>((static_cast<int64_t>(j) * rh) / PROBE_SIZE) + rh / (2 * static_cast<int32_t>(PROBE_SIZE));
            if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= mWidth || static_cast<uint32_t>(y) >= mHeight) continue;
            const uint8_t* px = mWorldPixels.data() + (static_cast<size_t>(y) * mWidth + static_cast<size_t>(x)) * 4;
            const uint8_t m = std::max(px[0], std::max(px[1], px[2]));
            if (m > max) max = m;
            if (m) ++lit;
        }
    }
    mProbe.mGlMax = max;
    mProbe.mGlLit = lit;
    mProbe.mGlSeq = mProbe.mSeq;
    mProbe.mGlValid = true;
}

void WolfVkScene::probeVkRecord(VkCommandBuffer cmd)
{
    if (!mDevice || !mShare || !mShare->world()) return;
    WolfVkImage* world = mShare->world();
    if (!mProbeVkChecked)
    {
        mProbeVkChecked = true;
        // the blit needs BLIT_SRC on the layer's format and BLIT_DST on the 32x32 target's (vulkan_core.h:2725-2726), both
        // optimal tiling; the same check WolfVkImage makes for its mip blits
        VkFormatProperties src_props{}, dst_props{};
        gVk.vkGetPhysicalDeviceFormatProperties(mDevice->physical(), world->format().mFormat, &src_props);
        gVk.vkGetPhysicalDeviceFormatProperties(mDevice->physical(), VK_FORMAT_R8G8B8A8_UNORM, &dst_props);
        const bool features = (src_props.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) && (dst_props.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)
                              && (world->usage() & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        std::string error;
        bool ok = features;
        if (ok)
        {
            mProbeVkImage.reset(new WolfVkImage());
            ok = mProbeVkImage->create(*mDevice, WolfVkImage::Kind::TEXTURE_2D, WolfVkImage::formatFor(GL_RGBA8, GL_UNSIGNED_BYTE), PROBE_SIZE, PROBE_SIZE, 1, 1,
                                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false, error);
        }
        if (ok)
        {
            const uint32_t slots = mDevice->framesInFlight();
            mProbeVkHost.clear();
            mProbeVkPending.assign(slots, 0);
            for (uint32_t i = 0; i < slots && ok; ++i)
            {
                std::unique_ptr<WolfVkBuffer> b(new WolfVkBuffer());
                ok = b->create(*mDevice, static_cast<VkDeviceSize>(PROBE_SIZE) * PROBE_SIZE * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, error);
                mProbeVkHost.push_back(std::move(b));
            }
        }
        mProbe.mVkPossible = ok;
        if (!ok)
        {
            LL_WARNS("WolfVulkanCheck") << "world probe: Vulkan's view of the shared world layer cannot be sampled ("
                                        << (features ? error : std::string("the formats cannot be blitted")) << "); GL samples only" << LL_ENDL;
            mProbeVkImage.reset();
            mProbeVkHost.clear();
            mProbeVkPending.clear();
        }
    }
    if (!mProbe.mVkPossible) return;
    const uint32_t slot = mDevice->frameIndex();
    if (slot >= mProbeVkPending.size() || mProbeVkPending[slot] != 0) return;
    // the world layer (SHADER_READ_ONLY after the acquire) to TRANSFER_SRC, the world view rect blitted to 32x32, copied
    // out to this slot's host buffer, the layer back to SHADER_READ_ONLY for the composite (release() takes layout())
    world->transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    mProbeVkImage->transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    // Source: vulkan_core.h:4111 VkImageBlit { srcSubresource, srcOffsets[2], dstSubresource, dstOffsets[2] }; :4660 PFN_vkCmdBlitImage
    VkImageBlit blit{};
    blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    const int32_t x0 = std::max(0, mWorldRect[0]), y0 = std::max(0, mWorldRect[1]);
    const int32_t x1 = std::min(static_cast<int32_t>(world->width()), mWorldRect[0] + mWorldRect[2]);
    const int32_t y1 = std::min(static_cast<int32_t>(world->height()), mWorldRect[1] + mWorldRect[3]);
    if (x1 <= x0 || y1 <= y0) return;
    blit.srcOffsets[0] = { x0, y0, 0 };
    blit.srcOffsets[1] = { x1, y1, 1 };
    blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.dstOffsets[0] = { 0, 0, 0 };
    blit.dstOffsets[1] = { static_cast<int32_t>(PROBE_SIZE), static_cast<int32_t>(PROBE_SIZE), 1 };
    gVk.vkCmdBlitImage(cmd, world->image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mProbeVkImage->image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    mProbeVkImage->transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy region{};   // Source: vulkan_core.h:3996
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { PROBE_SIZE, PROBE_SIZE, 1 };
    gVk.vkCmdCopyImageToBuffer(cmd, mProbeVkImage->image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mProbeVkHost[slot]->buffer(), 1, &region);   // :4593
    VkBufferMemoryBarrier hb{};   // host read after the copy (:2936 HOST_BIT, :3117 HOST_READ)
    hb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    hb.srcQueueFamilyIndex = hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hb.buffer = mProbeVkHost[slot]->buffer();
    hb.size = VK_WHOLE_SIZE;
    gVk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hb, 0, nullptr);
    world->transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    mProbeVkPending[slot] = mProbe.mSeq;
}

void WolfVkScene::probeVkCollect()
{
    if (!mDevice || mProbeVkPending.empty()) return;
    const uint32_t slot = mDevice->frameIndex();
    if (slot >= mProbeVkPending.size() || mProbeVkPending[slot] == 0 || slot >= mProbeVkHost.size()) return;
    const uint8_t* data = mProbeVkHost[slot]->hostData();
    if (data)
    {
        maxRGB(data, PROBE_SIZE * PROBE_SIZE, mProbe.mVkMax, mProbe.mVkLit);
        mProbe.mVkSeq = mProbeVkPending[slot];
        mProbe.mVkValid = true;
    }
    mProbeVkPending[slot] = 0;
}

void WolfVkScene::probeDestroy()
{
    if (gGLManager.mInited)
    {
        if (mProbeGlFbo) glDeleteFramebuffers(1, &mProbeGlFbo);
        if (mProbeGlTex) glDeleteTextures(1, &mProbeGlTex);
        if (mProbeGlPbo[0] || mProbeGlPbo[1]) glDeleteBuffers(2, mProbeGlPbo);
    }
    mProbeGlFbo = mProbeGlTex = 0;
    mProbeGlPbo[0] = mProbeGlPbo[1] = 0;
    mProbeGlPending[0] = mProbeGlPending[1] = 0;
    mProbeGlNext = 0;
    mProbeVkImage.reset();      // deferred Vulkan destroys, as every image
    mProbeVkHost.clear();
    mProbeVkPending.clear();
    mProbeVkChecked = false;
    mProbe.mVkPossible = false;
}

std::string WolfVkScene::spirvSummary() const
{
    std::string missing;
    for (const auto& kv : mNoSpirv)
    {
        if (!missing.empty()) missing += ", ";
        missing += kv.first ? kv.first->mName : std::string("?");
    }
    return llformat("%u program mirrors made, %u programs without SPIR-V", mMirrorsMade, static_cast<unsigned>(mNoSpirv.size()))
         + (missing.empty() ? std::string() : " (" + missing + ")") + "; last: " + mShaderStatus;
}

bool WolfVkScene::recordFrame(VkCommandBuffer cmd, WolfVkSwapchain& swapchain, uint32_t image_index)
{
    static LLCachedControl<bool> probe(gSavedSettings, "WolfVulkanDebugProbe", false);
    mProbeThisFrame = false;
    if (probe)
    {
        const F64 now = LLTimer::getTotalSeconds();
        if (now - mProbeLast >= 1.0)
        {
            mProbeLast = now;
            mProbeThisFrame = true;
            probeCoverage();
        }
    }
    mLayersAcquired = false;
    readBackUI();
    uploadLayers(cmd);
    // [HAND-OVER 2026-11-08 8.2k] Shared layers not acquired from GL this frame (GL did not release them): the scene pass
    // and the composite would read them without ownership (VUID-vkQueueSubmit-pSubmits-02808), so nothing more is
    // recorded and the caller falls back without submitting (review item 7; today this coincides with handOverBroken()).
    if (handOverBroken() || (mShare && !mLayersAcquired)) return false;
    // [BLACK WORLD 2026-11-10] Vulkan's view of the shared world layer, this very frame, after the acquire
    if (mProbeDueThisFrame && mShare && mLayersAcquired && mHaveWorld) probeVkRecord(cmd);
    mProbeDueThisFrame = false;
    drawFullbright(cmd);
    drawComposite(cmd, swapchain, image_index);
    if (mShare && mLayersAcquired)
    {
        mShare->release(cmd);   // back to GL for the next frame's writes
    }
    return true;
}

// [POSE CHECK 2026-11-08] see wolfvkscene.h. GL side: a model matrix applied after the Vulkan pass, in the main G-buffer phase.
void WolfVkScene::poseCheckMatrix(const LLMatrix4* model_matrix)
{
    if (!model_matrix) return;
    auto it = mPoseMatrices.find(model_matrix);
    if (it == mPoseMatrices.end()) return;
    mPoseGlMatrixChecks++;
    const float* now = &model_matrix->mMatrix[0][0];
    float diff = 0.f;
    for (int k = 0; k < 16; ++k) diff = std::max(diff, std::fabs(now[k] - it->second[static_cast<size_t>(k)]));
    if (diff > 0.f)
    {
        mPoseMatrixMismatch++;
        mPoseMatrixMaxDiff = std::max(mPoseMatrixMaxDiff, diff);
        if (mPoseFirstMismatch.empty())
        {
            mPoseFirstMismatch = llformat("model matrix %p changed after the Vulkan pass by %g (translation then %g %g %g, now %g %g %g)",
                                          (const void*)model_matrix, diff, it->second[12], it->second[13], it->second[14], now[12], now[13], now[14]);
        }
    }
    if (mPoseViewSet && memcmp(mPoseView, gGLModelView, sizeof(mPoseView)) != 0) mPoseViewMismatch++;
}

// Everything of the frame is drawn (called at captureWorld): the copies' positions and indices against the GL buffers.
void WolfVkScene::poseCheckVertices()
{
    static LLCachedControl<bool> pose_check(gSavedSettings, "WolfVulkanDebugPoseCheck", false);
    if (!pose_check) return;
    const F64 now = LLTimer::getTotalSeconds();
    if (now - mPoseLast < 1.0) return;   // once a second: glGetBufferSubData waits for the GPU
    mPoseLast = now;
    GLint saved_array = 0, saved_element = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &saved_array);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &saved_element);
    std::vector<uint8_t> gl_bytes;
    // [POSE CHECK round 2, nv-ab3/nv-bis1] the batches with a model matrix (rocked boats, every active drawable) first - the
    // dashes sit on them - then the rest, up to 1024 ranges; every attribute array of the range is compared, not only the
    // positions (the colour's alpha is the env intensity frag_data[1] and encodeNormal write, diffuseIndexedF.glsl)
    std::vector<const PoseRecord*> order;
    for (const PoseRecord& r : mPoseRecords) if (r.mMatrix) order.push_back(&r);
    const size_t moving = order.size();
    for (const PoseRecord& r : mPoseRecords) if (!r.mMatrix) order.push_back(&r);
    size_t sampled = 0;
    for (const PoseRecord* rp : order)
    {
        const PoseRecord& r = *rp;
        if (sampled >= 1024) break;
        LLVertexBuffer* glvb = r.mBuffer.get();
        if (!glvb || glvb->getGLBuffer() == 0) continue;
        WolfVkVertexBuffer* vb = WolfVkMirror::existingVertexBuffer(glvb);
        if (!vb || !vb->vertices().hostData()) continue;
        sampled++;
        mPoseVertexSampled++;
        if (r.mMatrix) mPoseMovingSampled++;
        glBindBuffer(GL_ARRAY_BUFFER, glvb->getGLBuffer());
        // every attribute array the copy holds (WolfVkVertexBuffer::calcOffsets = LLVertexBuffer::calcOffsets,
        // llvertexbuffer.cpp:1062-1082; TYPE_TEXTURE_INDEX lives inside the positions, so it is covered by TYPE_VERTEX)
        for (uint32_t t = 0; t < WolfVkVertexBuffer::TYPE_TEXTURE_INDEX; ++t)
        {
            if (!(vb->getTypeMask() & (1u << t)) || WolfVkVertexBuffer::sTypeSize[t] == 0) continue;
            const size_t stride = WolfVkVertexBuffer::sTypeSize[t];
            const size_t first = static_cast<size_t>(vb->getOffset(static_cast<WolfVkVertexBuffer::AttributeType>(t))) + static_cast<size_t>(r.mStart) * stride;
            const size_t bytes = static_cast<size_t>(r.mEnd - r.mStart + 1) * stride;
            if (first + bytes > vb->vertices().size()) continue;
            gl_bytes.resize(bytes);
            glGetBufferSubData(GL_ARRAY_BUFFER, static_cast<GLintptr>(first), static_cast<GLsizeiptr>(bytes), gl_bytes.data());
            if (memcmp(gl_bytes.data(), vb->vertices().hostData() + first, bytes) != 0)
            {
                mPoseVertexMismatch++;
                if (r.mMatrix) mPoseMovingMismatch++;
                if (mPoseFirstMismatch.empty())
                {
                    size_t at = 0;
                    while (at < bytes && gl_bytes[at] == vb->vertices().hostData()[first + at]) ++at;
                    mPoseFirstMismatch = llformat("vertex buffer %p attribute %u (%s) verts %u-%u: the Vulkan copy differs from GL's from byte %zu of %zu (%s)",
                                                  (const void*)glvb, t, WolfVkVertexBuffer::sAttribName[t], r.mStart, r.mEnd, at, bytes,
                                                  r.mMatrix ? "a batch with a model matrix" : "a static batch");
                }
                break;   // one count per range
            }
        }
        if (glvb->getGLIndices() != 0 && vb->indices().hostData())
        {
            // the index size as the copy was made (16- or 32-bit: LLVertexBuffer::setIndexData has both, llvertexbuffer.cpp:1918-1931)
            const size_t isz = vb->getNumIndices() ? static_cast<size_t>(vb->getIndicesSize() / vb->getNumIndices()) : 0;
            const size_t ifirst = static_cast<size_t>(r.mOffset) * isz;
            const size_t ibytes = static_cast<size_t>(r.mCount) * isz;
            if (isz && ifirst + ibytes <= vb->indices().size())
            {
                gl_bytes.resize(ibytes);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, glvb->getGLIndices());
                glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLintptr>(ifirst), static_cast<GLsizeiptr>(ibytes), gl_bytes.data());
                if (memcmp(gl_bytes.data(), vb->indices().hostData() + ifirst, ibytes) != 0) mPoseIndexMismatch++;
            }
        }
    }
    mPoseMovingBatches += moving;
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(saved_array));
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLuint>(saved_element));
    LL_INFOS("WolfVulkanPose") << "batches " << mPoseBatches << ", GL matrix checks " << mPoseGlMatrixChecks << ", matrix changed after the Vulkan pass "
                               << mPoseMatrixMismatch << " (max " << mPoseMatrixMaxDiff << "), view changed " << mPoseViewMismatch
                               << "; vertex ranges sampled " << mPoseVertexSampled << " (with a model matrix " << mPoseMovingSampled
                               << ", of " << mPoseMovingBatches << " such batches seen at the checks), attribute data differ " << mPoseVertexMismatch
                               << " (with a model matrix " << mPoseMovingMismatch << "), indices differ " << mPoseIndexMismatch
                               << "; late GL flushes into drawn buffers " << WolfVkMirror::poseLateFlushes()
                               << (mPoseFirstMismatch.empty() ? std::string() : "; first: " + mPoseFirstMismatch) << " (totals)" << LL_ENDL;
    mPoseFirstMismatch.clear();
    // the Vulkan side's copy of the simple pass's uniforms (nv-bis1: the dashes vanish after the first shader reload, so
    // this line before and after one is the comparison)
    if (gDeferredDiffuseProgram.mWolfVk)
    {
        LL_INFOS("WolfVulkanPose") << "diffuse uniforms (key " << gDeferredDiffuseProgram.mWolfVk->key().substr(0, 8) << "): "
                                   << gDeferredDiffuseProgram.mWolfVk->describeUniforms() << LL_ENDL;
    }
    else
    {
        // after a shader reload the mirror is made again at the next Vulkan draw; none means that variant has no SPIR-V
        LL_INFOS("WolfVulkanPose") << "diffuse uniforms: no Vulkan mirror for the current Deferred Diffuse Shader" << LL_ENDL;
    }
}

void WolfVkScene::poseCheckFrameEnd()
{
    mPoseMatrices.clear();
    mPoseRecords.clear();
    mPoseViewSet = false;
    WolfVkMirror::poseFrameEnd();
}

bool WolfVkScene::handOverBroken() const
{
    // [HAND-OVER 2026-11-08 8.2k] a shared hand-over is all-or-nothing per frame: GL signalled exactly what Vulkan waits
    // for. A failed GL semaphore call (broken) or layers that are not valid leave that unknown, so the frame must not be
    // submitted with the hand-over's waits.
    return mShare && (mShare->broken() || !mShare->isValid());
}

std::string WolfVkScene::handOverBrokenReason() const
{
    if (!mShare) return std::string();
    if (mShare->broken()) return mShare->brokenReason();
    return mShare->isValid() ? std::string("OpenGL did not hand the shared layers over this frame") : std::string("the shared layers are not valid");
}

WolfVkDevice::ExtraSync WolfVkScene::frameSync()
{
    return mShare ? mShare->sync() : WolfVkDevice::ExtraSync();
}
