/**
 * @file wolfvkscene.h
 * @brief What the Vulkan backend draws each frame in this first cut: the OpenGL frame's layers composited in Vulkan,
 *        and the scene's fullbright batches drawn by Vulkan with the viewer's real fullbright shader.
 *
 * [VULKAN BACKEND 2026-11-08] Stage 4's first visible step (docs/VULKAN_BACKEND.md).
 *
 * Frame, in Vulkan mode (WolfVkBridge drives it from the viewer's swap points):
 *   1. captureWorld()   in render_ui() after renderFinalize(): the finished world is in GL's default framebuffer. It is
 *                       read back (glReadPixels into a pixel pack buffer), the deferred depth texture too
 *                       (glGetTexImage), and GL's colour is cleared to (0,0,0,0) so the HUD and UI that follow land on
 *                       a layer of their own with coverage in alpha.
 *   2. recordFrame()    at the swap: the UI layer is read back; the three read-backs are uploaded; the Vulkan scene
 *                       pass draws every LLDrawInfo of LLRenderPass::PASS_FULLBRIGHT (the batches LLDrawPoolFullbright
 *                       draws, lldrawpoolsimple.cpp:161-182) into an RGBA16F target whose depth was loaded from the GL
 *                       depth, with the deferred fullbright program's SPIR-V, the batch's vertex buffer and textures
 *                       (Vulkan copies, wolfvkmirror.h) and the uniforms the GL pass set (WolfVkShaderMirror); then the
 *                       composite pass (class1/wolfvk/composite*.glsl) lays world, scene and UI into the swapchain image.
 * The fullbright pass's GL state is the one renderGeomPostDeferred leaves (pipeline.cpp: LLGLDisable blend in
 * renderDeferredLighting's post-deferred block, cull on, setColorMask(true, false); display()'s
 * LLGLDepthTest(GL_TRUE, GL_TRUE, GL_LEQUAL)): blending off, depth LEQUAL with writes, no alpha write, back-face cull.
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

#ifndef WOLF_VK_SCENE_H
#define WOLF_VK_SCENE_H

#include "vk/wolfvkloader.h"
#include "vk/wolfvkimage.h"
#include "vk/wolfvkrendertarget.h"
#include "vk/wolfvkpipeline.h"
#include "vk/wolfvkshader.h"
#include "wolfvkshare.h"
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <array>
#include "llpointer.h"
class LLMatrix4;

class WolfVkDevice;
class WolfVkSwapchain;
class LLGLSLShader;
class LLDrawInfo;       // llspatialpartition.h
class LLVertexBuffer;

class WolfVkScene
{
public:
    WolfVkScene() = default;
    ~WolfVkScene();

    bool init(WolfVkDevice& device, WolfVkSwapchain& swapchain, std::string& error);
    void destroy();
    void onSwapchainChanged(WolfVkSwapchain& swapchain);

    void captureWorld();
    // Resets this frame's descriptor pools and uniform ring (after WolfVkDevice::beginFrame, before its flushDeferred).
    void beginFrame();
    // false: the frame must not be submitted (the shared hand-over did not happen; handOverBrokenReason() says why)
    bool recordFrame(VkCommandBuffer cmd, WolfVkSwapchain& swapchain, uint32_t image_index);

    // [SHARED IMAGES 2026-11-08] The semaphores this frame's submit must wait on / signal for the GL hand-over (empty
    // without shared layers); called exactly once per submit by WolfVkBridge::present. handOverLine(): plain words on
    // how the picture is handed over, for the floater and the reports.
    WolfVkDevice::ExtraSync frameSync();
    // [HAND-OVER 2026-11-08 8.2k] true when the shared hand-over can no longer be trusted (a GL semaphore call failed, or
    // the layers are not valid): the owner falls back to OpenGL instead of submitting a frame that waits on it
    bool handOverBroken() const;
    std::string handOverBrokenReason() const;
    bool sharedHandOver() const { return mShare != nullptr; }
    const std::string& handOverLine() const { return mHandOver; }

    // Statistics for the floater and the log (last frame).
    uint32_t drawsLastFrame() const { return mDrawsLastFrame; }
    uint32_t skippedLastFrame() const { return mSkippedLastFrame; }
    uint32_t pipelineCount() const { return mPipelines.pipelineCount(); }
    const std::string& shaderStatus() const { return mShaderStatus; }

private:
    bool createLayers(uint32_t width, uint32_t height, std::string& error);
    bool createCompositePass(WolfVkSwapchain& swapchain, std::string& error);
    void destroyCompositePass();
    bool ensureSceneTarget(uint32_t width, uint32_t height, std::string& error);
    bool readBackUI();
    void uploadLayers(VkCommandBuffer cmd);
    void drawFullbright(VkCommandBuffer cmd);
    // [VULKAN G-BUFFER 2026-11-08] The draw infos of one render pass type with one program and one pipeline state, into
    // the target already begun on cmd (the batch loop shared by every pass Vulkan draws). Returns the draws made.
    uint32_t drawPass(VkCommandBuffer cmd, uint32_t pass_type, LLGLSLShader* program, WolfVkPipelineState state, WolfVkRenderTarget& target);
    // Before a pass (outside any render pass): makes/updates the Vulkan copies of every buffer and texture the pass's
    // draw infos need and records their uploads on cmd, so the pass itself skips nothing for "copy still uploading".
    void preparePass(VkCommandBuffer cmd, uint32_t pass_type, LLGLSLShader* program);
public:
    // The deferred simple pass (LLDrawPoolSimple::renderDeferred: gDeferredDiffuseProgram, PASS_SIMPLE) into the shared
    // G-buffer target, recorded on a mid-frame command buffer. Returns the draws made.
    uint32_t drawGBufferSimple(VkCommandBuffer cmd, WolfVkRenderTarget& gbuffer);
    uint32_t gbufferDrawsLastFrame() const { return mGBufferDraws; }
    // [ONE RENDERER PER BATCH 2026-11-08] the draw infos Vulkan drew this frame (GL skips them in LLRenderPass::pushBatches)
    bool drawnByVulkan(const LLDrawInfo* params) const { return mDrawnByVulkan.count(params) != 0; }
    void clearDrawnByVulkan() { mDrawnByVulkan.clear(); }
    // [POSE CHECK 2026-11-08] WolfVulkanDebugPoseCheck (Paul: the boats have blue tearing and bits missing, the houses are
    // fine). Evidence for one question: does the Vulkan G-buffer pass draw a batch with the same model matrix, view and
    // vertex data that OpenGL's later passes of the same frame use for the same object? drawPass records what it used;
    // poseCheckMatrix compares a model matrix OpenGL applies afterwards in the main G-buffer phase
    // (LLRenderPass::applyModelMatrix); poseCheckVertices (at captureWorld, everything drawn) compares the Vulkan copies'
    // positions and indices with the GL buffers' current bytes; a summary line a second (log tag WolfVulkanPose).
    void poseCheckMatrix(const LLMatrix4* model_matrix);
    void poseCheckVertices();
    void poseCheckFrameEnd();
    uint32_t skippedLastFrameTotal() const { return mSkippedLastFrame; }
    // false when the last G-buffer pass could not draw every batch (uniform ring full, a buffer still uploading): the
    // caller then lets GL draw the pass as well, so nothing is missing from the picture
    bool gbufferPassComplete() const { return mGBufferComplete; }
private:
    struct PoseRecord { LLPointer<LLVertexBuffer> mBuffer; uint32_t mStart, mEnd, mOffset, mCount; const LLMatrix4* mMatrix; };
    std::unordered_map<const LLMatrix4*, std::array<float, 16>> mPoseMatrices;   // model matrices the pass used this frame
    std::vector<PoseRecord> mPoseRecords;
    float mPoseView[16] = {};
    bool mPoseViewSet = false;
    uint64_t mPoseBatches = 0, mPoseGlMatrixChecks = 0, mPoseMatrixMismatch = 0, mPoseViewMismatch = 0;
    uint64_t mPoseVertexSampled = 0, mPoseVertexMismatch = 0, mPoseIndexMismatch = 0;
    uint64_t mPoseMovingSampled = 0, mPoseMovingMismatch = 0, mPoseMovingBatches = 0;
    float mPoseMatrixMaxDiff = 0.f;
    std::string mPoseFirstMismatch;
    F64 mPoseLast = 0.0;
    uint32_t mGBufferDraws = 0;
    bool mGBufferComplete = true;
    std::unordered_set<const LLDrawInfo*> mDrawnByVulkan;
    void drawComposite(VkCommandBuffer cmd, WolfVkSwapchain& swapchain, uint32_t image_index);
    WolfVkShaderMirror* mirrorFor(LLGLSLShader* shader);

    WolfVkDevice* mDevice = nullptr;
    uint32_t mWidth = 0;
    uint32_t mHeight = 0;
    std::string mSpvDir;

    WolfVkPipelineCache mPipelines;
    WolfVkFrameUniforms mUniforms;
    WolfVkSamplerCache mSamplers;
    WolfVkStagingRing mStaging;   // [STAGING RACE 2026-11-08] per-frame transient staging for the vertex buffer copies

    // GL read-backs: pixel pack buffers (world, UI, depth) and the client copies handed to the uploads
    uint32_t mPboWorld = 0, mPboUI = 0, mPboDepth = 0;
    uint32_t mDepthWidth = 0, mDepthHeight = 0;
    bool mWorldCaptured = false;
    std::vector<uint8_t> mWorldPixels, mUIPixels, mDepthPixels;
    bool mHaveWorld = false, mHaveUI = false, mHaveDepth = false;

    // Vulkan images of the layers and the scene target
    std::unique_ptr<WolfVkImage> mWorldImage, mUIImage, mWhite, mTransparent;
    std::unique_ptr<WolfVkRenderTarget> mScene;

    // composite pass on the swapchain
    std::unique_ptr<WolfVkProgram> mComposite;
    VkRenderPass mCompositePass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> mCompositeFramebuffers;
    VkFormat mCompositeFormat = VK_FORMAT_UNDEFINED;

    uint32_t mDrawsLastFrame = 0;
    uint32_t mSkippedLastFrame = 0;
    std::string mShaderStatus;
    std::unordered_map<const LLGLSLShader*, uint32_t> mNoSpirv;   // shader -> GL program object that had no SPIR-V

    // [SHARED IMAGES 2026-11-08] the GL/Vulkan shared layers (null: the read-back hand-over above is used) and the
    // world view rect of the last captured frame (the scene pass's viewport inside the window-sized layers)
    std::unique_ptr<WolfVkShare> mShare;
    bool mLayersAcquired = false;   // [HAND-OVER 8.2k] this frame's command buffer acquired the shared layers from GL
    std::string mHandOver;
    int32_t mWorldRect[4] = { 0, 0, 0, 0 };   // x, y (bottom-left), width, height

    // [VULKAN BACKEND 2026-11-08] WolfVulkanDebugProbe: once a second, hard numbers instead of looking at pictures - how
    // many pixels of the Vulkan scene layer were written (its alpha), and where the first batch's first vertex lands.
    void probeCoverage();
    void probeBatch(const LLDrawInfo* params, const LLVertexBuffer* glvb, const WolfVkVertexBuffer* vb, const WolfVkShaderMirror* mirror, const glm::mat4& mvp);
    F64 mProbeLast = 0.0;
    bool mProbeThisFrame = false;
};

#endif // WOLF_VK_SCENE_H
