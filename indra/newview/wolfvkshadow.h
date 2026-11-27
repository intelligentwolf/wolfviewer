/**
 * @file wolfvkshadow.h
 * @brief The shadow maps as Vulkan-owned depth images that Vulkan draws into: the four sun cascades, the two projector
 *        (spot) maps and the reflection-probe / mirror render packs' cascades.
 *
 * [W114-VULKAN S5 2026-11-26] Stage S5 (docs/VULKAN_BACKEND.md 8.7).
 *
 * What this file owns:
 *   - WolfVkShadow: the LLRenderTarget::WolfVkProvider of every shadow map (LLRenderTarget::wolfVkRegister, the S6
 *     registry) and the bracket target of the one that is bound. Each map is LLRenderTarget::allocate(w, h, 0, true)
 *     (pipeline.cpp allocateShadowBuffer): depth only, so the provider is asked for depthTexture() alone.
 *
 * The frame: LLPipeline::generateSunShadow / the spot loop bind a map, clear it (OpenGL) and call
 * LLPipeline::renderShadow, which turns the interceptor's phase on (WolfVkBridge::setGBufferPhase) for the whole pass.
 * Every LLVertexBuffer draw call of the pass - the static and rigged casters, WolfFarProxy's casters, the pools' shadow
 * passes (terrain, trees, avatars), the alpha-mask, alpha and GLTF casters - is offered to Vulkan exactly as the G-buffer
 * draws are (wolfvkscene.h recordGBufferDraw: the bound program's mirror, the bound textures, LLGLState, LLGLDepthTest),
 * now into the bound map's Vulkan target. LLRenderTarget::flush() at the end of the map closes the bracket.
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

#ifndef WOLF_VK_SHADOW_H
#define WOLF_VK_SHADOW_H

#include "llrendertarget.h"
#include "wolfvkbrackettarget.h"
#include "wolfvkshare.h"
#include "vk/wolfvkrendertarget.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

class WolfVkShadow : public LLRenderTarget::WolfVkProvider, public WolfVkBracketTarget
{
public:
    WolfVkShadow() = default;
    ~WolfVkShadow() override;

    // Checks the depth format can be exported and makes the two pass semaphores. False with error when this device / GL
    // pair cannot share a shadow map. self_test: every map makes one round trip when it is created (as the G-buffer's
    // attachments do, wolfvkgbuffer.h init).
    bool init(WolfVkDevice& device, bool self_test, std::string& error);
    // Registers this provider for every shadow map of the pipeline and releases the maps, so that their next allocation
    // (LLPipeline::allocateShadowBuffer; the caller sets gResizeScreenTexture) asks here.
    void registerTargets();
    // Gives every map back to OpenGL's own textures (releases them and unregisters) and destroys the images and the
    // semaphores. The caller sets gResizeScreenTexture so the pipeline allocates them again.
    void destroy();
    bool available() const { return mAvailable; }
    const std::string& disabledReason() const { return mDisabledReason; }
    const std::string& selfTestResult() const { return mSelfTestResult; }
    // "D32_SFLOAT" or "X8_D24_UNORM_PACK32": what the maps are made as on this device
    const char* formatName() const;

    // LLRenderTarget::WolfVkProvider (a shadow map has no colour attachment: colorTexture is never a yes)
    bool colorTexture(LLRenderTarget& target, U32 index, U32& color_fmt, U32& tex) override;
    bool depthTexture(LLRenderTarget& target, U32& tex) override;
    void released(LLRenderTarget& target) override;

    // The bracket's target for the draws OpenGL is about to make: the map that is OpenGL's bound render target, when it
    // is one of ours with its image. False: the bound target is not a shared shadow map (OpenGL draws).
    bool selectBound();
    // The depth image of a map for a Vulkan pass that samples it (the lighting passes, stage S7), null when the map is
    // not shared. It is in VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL and OpenGL's between brackets: a sampling
    // pass acquires it (WolfVkSharedImage::acquireFromGL) as the bracket does. The sampler state OpenGL gives the maps is
    // pipeline.cpp allocateShadowBuffer's: TFO_ANISOTROPIC, TAM_CLAMP, compare GL_LEQUAL
    // (WolfVkSamplerCache::get(TFO_ANISOTROPIC, TAM_CLAMP, false, true)).
    WolfVkSharedImage* shared(const LLRenderTarget* target);

    // WolfVkBracketTarget, for the map chosen by selectBound()
    const char* bracketName() const override { return "shadow map"; }
    bool prepare() override;
    bool glHandToVulkan() override;
    void acquire(VkCommandBuffer cmd) override;
    WolfVkRenderTarget* target() override;
    void release(VkCommandBuffer cmd) override;
    WolfVkDevice::ExtraSync sync() override;
    bool glTakeBack() override;
    void glAfterTakeBack() override;
    bool boundInGL() override { return selectBound(); }
    void wantSampled(uint32_t gl_texture) override { mExtras.want(gl_texture); }
    WolfVkImage* sampledImage(uint32_t gl_texture) override { return mExtras.inBracket(gl_texture); }
    bool broken() const override { return !mBrokenReason.empty(); }
    const std::string& brokenReason() const override { return mBrokenReason; }

private:
    struct Map
    {
        LLRenderTarget* mTarget = nullptr;                // the LLRenderTarget this map belongs to
        WolfVkSharedImage mDepth;
        std::unique_ptr<WolfVkRenderTarget> mVkTarget;     // depth only, over mDepth
        uint32_t mWidth = 0, mHeight = 0;
    };
    Map* find(const LLRenderTarget* target);
    void forEachTarget(const std::function<void(LLRenderTarget&)>& fn);
    bool idleBothAPIs(const char* what);
    void destroyRewrite();

    WolfVkDevice* mDevice = nullptr;
    bool mAvailable = false;
    bool mSelfTest = true;
    bool mRegistered = false;
    bool mD24 = false;               // the maps are VK_FORMAT_X8_D24_UNORM_PACK32 / GL_DEPTH_COMPONENT24 (else D32_SFLOAT / 32F)
    std::string mDisabledReason;
    std::string mSelfTestResult;
    std::string mBrokenReason;
    std::vector<std::unique_ptr<Map>> mMaps;
    Map* mCurrent = nullptr;         // selectBound()'s map: the bracket's target
    WolfVkBracketExtras mExtras;     // the images the recorded draws sample (wolfvkbrackettarget.h)
    VkSemaphore mGlPassDone = VK_NULL_HANDLE;   // GL signals, Vulkan waits
    VkSemaphore mVkPassDone = VK_NULL_HANDLE;   // Vulkan signals, GL waits
    uint32_t mGlSemGlPassDone = 0, mGlSemVkPassDone = 0;
    bool mGlSignalled = false;
    bool mVkSignalled = false;
    // glAfterTakeBack's depth rewrite (wolfvkgbuffer.h glRewriteDepth, for the bound map)
    uint32_t mRewriteTex = 0, mRewriteW = 0, mRewriteH = 0, mRewriteProgram = 0, mRewriteVao = 0;
    int32_t mRewriteTexLoc = -1;
    bool mRewriteD24 = false;        // the copy texture's format follows the maps'
};

#endif // WOLF_VK_SHADOW_H
