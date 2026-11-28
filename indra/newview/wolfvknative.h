/**
 * @file wolfvknative.h
 * @brief Native Vulkan renderer (no OpenGL context, no GL/Vulkan sharing or readback).
 *
 * [NATIVE VULKAN 2026-11-26]
 * Implements the architecture in docs/NATIVE_VULKAN_RENDERER_DESIGN.md:
 *   - Owns VkInstance, VkSurfaceKHR, WolfVkDevice, WolfVkSwapchain, staging/uniform rings,
 *     pipeline cache, sampler cache, native vertex/index buffer store, native texture store,
 *     and native render target owner.
 *   - Populates gGLManager capabilities from VkPhysicalDevice with zero OpenGL calls.
 *   - Records every LLVertexBuffer draw call directly into Vulkan command buffers using
 *     reflected SPIR-V pipelines (WolfVkShaderMirror / WolfVkPipelineCache).
 *   - Presents the window target directly to the Vulkan swapchain.
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

#ifndef WOLF_VK_NATIVE_H
#define WOLF_VK_NATIVE_H

#include "llsd.h"
#include <string>
#include <vector>

class LLWindow;
class LLGLSLShader;

namespace WolfVkNative
{
    // Initialize native Vulkan on the SDL Vulkan window (before LLViewerShaderMgr::setShaders and gGL.init).
    // Populates gGLManager capabilities from VkPhysicalDevice and installs all native Vulkan hooks.
    bool init(LLWindow* window, std::string& error);

    // True once init() succeeded and until shutdown().
    bool isActive();

    // True when the active Native Vulkan device is an integrated (unified-memory) GPU.
    bool isIntegratedGPU();

    // True when the Vulkan device is hung or lost (e.g. a frame timed out or VK_ERROR_DEVICE_LOST).
    bool isHung();
    std::string hungReason();

    // Hardware-derived memory bandwidth estimate (GB/s) for LLFeatureManager GPU class selection in Native Vulkan
    // (replaces the OpenGL ARB_timer_query benchmark, which produces 0/0 NaN without an OpenGL context).
    F32 estimatedMemoryBandwidthGBps();

    // Called in place of swapBuffers() / WolfVkBridge::present(). Ends any active pass, blits window target
    // to swapchain image, submits, and presents.
    bool present();

    // Called at end of main-loop frame when display did not call present() (e.g. minimized / unshown frame).
    void endUnshownFrame();

    // Submit any open snapshot frame and read back the bound LLRenderTarget (or window target if none is bound).
    // Color readbacks return tightly packed 4-byte RGBA/BGRA pixels in OpenGL bottom-up row order (out_bgra true
    // when BGRA); depth readbacks return tightly packed 32-bit floats in OpenGL bottom-up row order.
    bool readSnapshotTarget(bool depth, std::vector<U8>& out_bytes, U32& out_width, U32& out_height, bool& out_bgra);

    // Window resize notification from LLViewerWindow::reshape().
    void onResize();

    // Copy the currently bound LLRenderTarget's color attachment 0 into a 3D / array / cubemap-array image layer+mip.
    // Replaces glCopyTexSubImage3D in native Vulkan mode (e.g. reflection probe radiance/irradiance mip chains).
    void copyBoundTargetToImage3D(U32 dst_tex, S32 mip, S32 layer, S32 width, S32 height);

    // Upload RGBA8 pixels into a single layer of a GL_TEXTURE_2D_ARRAY storage texture and optionally regenerate mips.
    bool uploadTexture2DArrayLayer(U32 dst_tex, S32 layer, S32 width, S32 height, const void* rgba_pixels, bool generate_mips);

    // Wolf Environment (Takram atmosphere LUTs, 3D/array cloud passes, zero-clears, and async/sync readbacks).
    struct LayerAttachment
    {
        U32 mTex = 0;
        S32 mLayer = 0;
    };
    bool bindLayerTarget(const LayerAttachment* atts, U32 count);
    void unbindLayerTarget();
    void clearTextureZero(U32 tex_name);
    bool readBackTextureFloatSync(U32 tex_name, S32 mip, U32 out_channels, F32* out_floats, size_t max_floats);
    bool queueAsyncTextureRead(S32 channel_id, U32 tex_name, S32 mip, F32 tag = 0.f);
    bool pollAsyncTextureRead(S32 channel_id, void* out_bytes, size_t byte_count, F32* out_tag = nullptr);
    void resetAsyncTextureRead(S32 channel_id);

    // [W114-VULKAN 2026-11-27] GPU time of named sky sections (diagnostic): averaged per frame over the window since the last take.
    constexpr U32 GPU_SECTION_MAX = 8;
    void gpuSectionBegin(U32 section);
    void gpuSectionEnd(U32 section);
    bool takeGpuSectionStats(F64* ms_per_frame, F64* calls_per_frame, U32 count);

    // Hardware occlusion queries (VK_QUERY_TYPE_OCCLUSION + hostQueryReset, non-blocking poll).
    bool occlusionSupported();
    U32 genOcclusionQuery();
    void deleteOcclusionQuery(U32 id);
    bool beginOcclusionQuery(U32 id);
    void endOcclusionQuery();
    bool getOcclusionQueryResult(U32 id, bool& out_available, U32& out_passed);

    // Static shadow caster multi-draw indirect (vkCmdDrawIndexedIndirect + per-instance mat4 stream).
    bool shadowBatchSupported();
    bool shadowBatchProgramReady(LLGLSLShader* shader);
    void shadowBatchAdjustProjection(F32* proj16);
    S32 createShadowBatchPage(U32 vertex_bytes, U32 index_bytes);
    void writeShadowBatchPage(S32 page_id, U32 vertex_byte_offset, const void* vertices, U32 vertex_bytes,
                              U32 index_byte_offset, const void* indices, U32 index_bytes);
    void destroyShadowBatchPage(S32 page_id);
    void flushShadowBatchPages(const S32* page_ids, U32 page_count);
    bool uploadShadowBatchMatrices(const void* matrices, U32 matrix_count, U64& out_matrix_token);
    U32 drawShadowBatchPage(LLGLSLShader* batch_shader, S32 page_id, U64 matrix_token,
                            const void* commands, U32 cull_on_count, U32 cull_off_count);

    // GPU timestamp marker after sun shadow cascades finish.
    void markPhaseShadowsDone();

    // Cleanly waits for GPU idle, destroys every Vulkan child object (buffers, textures, targets, mirrors,
    // pipelines, samplers, swapchain, surface), then destroys the device.
    void shutdown();

    // Diagnostics / Help > About / status strings.
    std::string rendererLine();
    std::string vulkanLine();
    std::string statusLine();
    LLSD reportFields();
    U32 drawsLastFrame();
    U32 validationMessageCount();
}

#endif // WOLF_VK_NATIVE_H
