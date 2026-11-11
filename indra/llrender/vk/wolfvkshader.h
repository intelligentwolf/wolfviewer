/**
 * @file wolfvkshader.h
 * @brief LLGLSLShader's Vulkan side: the SPIR-V program for a GL program, and the uniform store that LLGLSLShader's
 *        uniform API writes into so the viewer's shader-setting code works unchanged.
 *
 * [VULKAN BACKEND 2026-11-08]
 *
 * How a GL program finds its SPIR-V: the build compiled every program variant with scripts/shader_check.py
 * --vulkan-out (shader_vulkan.py), naming each by the MD5 of its "recipe" (the stitching inputs, per unit:
 * stage | file | class found | #version line | defines | indexed channels | amd | oldselect, in attach order). The
 * viewer builds the same string from the same inputs (LLShaderMgr::wolfVkUnitRecipe, recorded in loadShaderFile for
 * the shared feature objects and computed for the program's own files) and loads
 * app_settings/shaders/spv/<key>.vert.spv and .frag.spv. A variant the build did not produce is reported once
 * ("no SPIR-V for ...", with the recipe) and that program is not drawn by Vulkan; nothing crashes.
 *
 * Uniforms: the off-line transform put every loose uniform of a stage into one std140 block (WolfUniformsV /
 * WolfUniformsF, set 0, bindings 0/1). The store keeps a byte image of each block, filled from reflection
 * (wolfvkspirv.h: offsets, array strides, matrix strides) and written by LLGLSLShader::uniform*() through
 * WolfVkShaderMirror::set(name, ...) with std140 rules (GLSL 4.60 section 7.6.2.2 "Standard Uniform Block Layout":
 * vec3 in 16-byte slots, array elements and matrix columns at their decorated strides, column-major unless RowMajor).
 * At draw time the two images are copied into the frame's ring buffer (WolfVkFrameUniforms) and bound with dynamic
 * offsets (VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC): one vkCmdBindDescriptorSets per draw, no per-uniform calls.
 *
 * Samplers: uniform1i(name, channel) on a sampler records the GL texture unit the viewer meant; the draw code binds
 * images to the program's set-1 bindings by sampler name (WolfVkProgram::samplerBinding).
 *
 * Projection: Vulkan clip space has z in [0, 1] and y downwards; OpenGL has z in [-1, 1] and y upwards. The viewer's
 * matrices are OpenGL's, so modelview_projection_matrix and projection_matrix are pre-multiplied here by
 *   [1 0 0 0; 0 1 0 0; 0 0 0.5 0.5; 0 0 0 1]   (z' = 0.5 z + 0.5 w)
 * which maps GL's [-w, w] onto Vulkan's [0, w] and leaves the stored depth value (z/w mapped to [0,1]) equal to what
 * GL would store with its default glDepthRange(0, 1) (OpenGL 4.6 spec 13.8.1 "Controlling the Viewport":
 * z_w = ((f - n)/2) z_d + (n + f)/2). The y flip is done by the negative-height viewport (wolfvkrendertarget.cpp), so
 * y stays. inv_proj and inv_modelview are left as GL's: a shader that reconstructs eye positions from depth with them
 * would need the inverse of the correction too - not in this cut (fullbright does not use them); declared in
 * docs/VULKAN_BACKEND.md.
 * [GL DEPTH 2026-11-08] On a device with VK_EXT_depth_clip_control (WolfVkDevice::glDepthRange) the correction is not
 * applied: the pipelines take GL's z range directly (negativeOneToOne), so the depth is made by the same viewport transform as GL's.
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

#ifndef WOLF_VK_SHADER_H
#define WOLF_VK_SHADER_H

#include "wolfvkpipeline.h"
#include "wolfvkbuffer.h"
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class LLGLSLShader;
class WolfVkDevice;

// Per-frame uniform ring buffer and descriptor pool: beginFrame() resets them; push() copies a block image in and
// returns its offset; allocate() gives a descriptor set from this frame's pool.
class WolfVkFrameUniforms
{
public:
    bool init(WolfVkDevice& device, VkDeviceSize bytes_per_frame, std::string& error);
    void destroy();
    void beginFrame(uint32_t frame_index);
    // Resets every frame's descriptor pool (device idle): before the images the sets name are destroyed.
    void resetPools();
    // Copies size bytes into the ring; false when the frame's ring is full (the draw is skipped, logged once a frame).
    // [V6 2026-11-11] essential: may use the reserve kept at the end of the ring (the frame's composite)
    bool push(const void* data, VkDeviceSize size, VkDeviceSize& offset, bool essential = false);
    VkBuffer buffer() const { return mBuffers[mFrame] ? mBuffers[mFrame]->buffer() : VK_NULL_HANDLE; }
    VkDescriptorSet allocate(VkDescriptorSetLayout layout);
    VkDeviceSize used() const { return mUsed; }
    VkDeviceSize capacity() const { return mBytes; }

private:
    WolfVkDevice* mDevice = nullptr;
    std::vector<std::unique_ptr<WolfVkBuffer>> mBuffers;   // one host-visible buffer per frame in flight
    // [V6 2026-11-11] per frame slot, a chain of descriptor pools: the first made at init, more when a frame needs them
    std::vector<std::vector<VkDescriptorPool>> mPools;
    uint32_t mPoolInUse = 0;
    bool mWarnedPools = false;
    static constexpr size_t MAX_POOLS_PER_FRAME = 16;            // 16 x 8192 sets: ~65,000 draws in one frame
    static constexpr VkDeviceSize ESSENTIAL_RESERVE = 64 * 1024; // ring bytes only the composite may use
    VkDescriptorPool createPool(std::string& error);
    VkDeviceSize mBytes = 0;
    VkDeviceSize mUsed = 0;
    VkDeviceSize mAlignment = 256;   // VkPhysicalDeviceLimits::minUniformBufferOffsetAlignment (vulkan_core.h:3653)
    uint32_t mFrame = 0;
    bool mWarnedFull = false;
};

class WolfVkShaderMirror
{
public:
    enum class Kind : uint8_t { F1, F2, F3, F4, I1, I2, I4, U4, M2, M3, M3x4, M4 };

    // Builds the mirror for shader: recipe key, .spv files, WolfVkProgram, uniform images. Returns null (and logs
    // once per program name) when the SPIR-V is missing or does not load. spv_dir: the shaders/spv directory.
    static WolfVkShaderMirror* create(WolfVkDevice& device, LLGLSLShader* shader, const std::string& spv_dir, std::string& error);
    ~WolfVkShaderMirror();

    const WolfVkProgram& program() const { return mProgram; }
    const std::string& key() const { return mKey; }

    // LLGLSLShader::uniform*() land here with the uniform's name (reserved index resolved by the caller).
    void set(const std::string& name, Kind kind, uint32_t count, const void* data, bool transpose = false);
    // uniform1i on a sampler: the GL texture unit the viewer binds for it.
    int32_t samplerChannel(const std::string& sampler_name) const;
    // Debugging (WolfVulkanDebugProbe): the bytes of a uniform as they sit in the stage image right now, as floats;
    // "stage:offset" of each copy in out_where. False when the name is not a block member.
    bool debugMember(const std::string& name, std::vector<float>& out_floats, std::string& out_where) const;
    // [POSE CHECK 2026-11-08] every uniform this mirror holds as "name=v0,v1,..;" (names sorted), the matrices left out
    // (they are set per draw), for comparing the Vulkan side's values before and after a shader reload
    std::string describeUniforms() const;

    // Binds set 0 (both uniform blocks, pushed into the frame ring) on cmd for the next draw. False when the ring is full.
    bool bindUniforms(VkCommandBuffer cmd, WolfVkFrameUniforms& ring);
    // Binds set 1 with the given image/sampler pairs by binding index (missing bindings get fallback).
    bool bindSamplers(VkCommandBuffer cmd, WolfVkFrameUniforms& ring, const std::vector<VkDescriptorImageInfo>& by_binding, const VkDescriptorImageInfo& fallback);

    // The viewer's recipe for one unit, as shader_vulkan.unit_recipe builds it (also used by LLShaderMgr::loadShaderFile).
    static std::string unitRecipe(const std::string& stage, const std::string& filename, int found_level, const std::string& version_line,
                                  const std::map<std::string, std::string>* defines, int channels, bool amd, bool old_select);
    static std::string programKey(const std::vector<std::string>& unit_recipes);
    // "#version 420\n" etc. exactly as LLShaderMgr::loadShaderFile chooses it (llshadermgr.cpp:657-711) for the GL in use.
    static std::string versionLine(bool geometry_shader);

private:
    WolfVkShaderMirror() = default;
    struct Member
    {
        const WolfVkSpirv::BlockMember* mMember;
        std::vector<uint8_t>* mImage;
    };
    void writeMember(const Member& m, Kind kind, uint32_t count, const void* data, bool transpose);
    void applyProjectionFix(const std::string& name, std::vector<float>& columns) const;

    WolfVkDevice* mDevice = nullptr;
    std::string mKey;
    std::string mName;
    WolfVkProgram mProgram;
    std::vector<uint8_t> mVertImage;
    std::vector<uint8_t> mFragImage;
    std::unordered_map<std::string, std::vector<Member>> mMembers;   // a name may be in both stages
    std::unordered_map<std::string, int32_t> mSamplerChannels;
    std::unordered_map<std::string, bool> mUnknownWarned;
};

#endif // WOLF_VK_SHADER_H
