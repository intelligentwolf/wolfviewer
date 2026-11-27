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
#include <set>
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
    // [VULKAN G-BUFFER S2 2026-11-26] The lean draw path. epoch(): changes at every beginFrame, so anything made from this
    // frame's ring or pools (a descriptor set, a ring offset) is reused only while the epoch is the same. needBind():
    // false when exactly this set with these dynamic offsets is already bound at set_index on cmd with this pipeline
    // layout (then the vkCmdBindDescriptorSets is skipped); otherwise it records them as bound and returns true. A bind
    // with another pipeline layout forgets every slot (sets bound with an incompatible layout are disturbed, Vulkan spec
    // 14.2.2 "Pipeline Layouts", "Pipeline Layout Compatibility").
    uint64_t epoch() const { return mEpoch; }
    bool needBind(VkCommandBuffer cmd, uint32_t set_index, VkPipelineLayout layout, VkDescriptorSet set, const uint32_t* offsets, uint32_t offset_count);
    bool needPushConstants(VkCommandBuffer cmd, VkPipelineLayout layout)
    {
        if (cmd != mPushCmd || mEpoch != mPushEpoch || layout != mPushLayout)
        {
            mPushCmd = cmd;
            mPushEpoch = mEpoch;
            mPushLayout = layout;
            return true;
        }
        return false;
    }
    // [S2] a caller that binds descriptor sets itself (the composite) calls this first: nothing is assumed bound after it
    void forgetBound()
    {
        mBoundCmd = VK_NULL_HANDLE;
        mBoundLayout = VK_NULL_HANDLE;
        mPushCmd = VK_NULL_HANDLE;
        mPushLayout = VK_NULL_HANDLE;
        for (BoundSlot& b : mBound) b = BoundSlot();
    }

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
    uint64_t mEpoch = 0;   // [S2]
    struct BoundSlot { VkDescriptorSet mSet = VK_NULL_HANDLE; uint32_t mCount = 0; uint32_t mOffsets[8] = {}; };
    VkCommandBuffer mBoundCmd = VK_NULL_HANDLE;
    uint64_t mBoundEpoch = 0;
    VkPipelineLayout mBoundLayout = VK_NULL_HANDLE;
    BoundSlot mBound[3];
    VkCommandBuffer mPushCmd = VK_NULL_HANDLE;
    uint64_t mPushEpoch = 0;
    VkPipelineLayout mPushLayout = VK_NULL_HANDLE;
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
    // [VULKAN G-BUFFER S2 2026-11-26] Reads every active loose uniform's current value from the GL program (glGetActiveUniform,
    // glGetUniformfv / glGetUniformiv: OpenGL 4.6 7.3.1 "Program Interfaces", 7.13 "Shader, Program, and Program Pipeline
    // Queries") into the images, samplers' texture units included. A mirror made after the viewer has set uniforms
    // (programs linked before Vulkan started, or set once after linking) then starts from exactly what OpenGL has.
    // Needs the GL context (main thread). Uniforms of a type no setter writes (ivec3, uvec2, ...) are left as they are.
    void syncFromGL(uint32_t gl_program);
    const std::string& key() const { return mKey; }

    // LLGLSLShader::uniform*() land here with the uniform's name (reserved index resolved by the caller).
    void set(const std::string& name, Kind kind, uint32_t count, const void* data, bool transpose = false);
    // [VULKAN G-BUFFER S2 2026-11-26] the same by LLShaderMgr reserved index: the name is looked up once per index and
    // program (the matrices LLRender::syncMatrices sends before every draw come this way), name = mReservedUniforms[index]
    void setReserved(uint32_t index, const std::string& name, Kind kind, uint32_t count, const void* data, bool transpose = false);
    // uniform1i on a sampler: the GL texture unit the viewer binds for it.
    int32_t samplerChannel(const std::string& sampler_name) const;
    // Pre-resolved active sampler bindings & channels for zero-hash hot-path draw loops.
    struct ResolvedSampler
    {
        uint32_t mBinding = 0;
        uint32_t mArraySize = 1;
        uint32_t mDim = 1;
        bool mArrayed = false;
        bool mShadow = false;
        int32_t mChannel = 0;
    };
    const std::vector<ResolvedSampler>& resolvedSamplers() const { return mResolvedSamplers; }
    uint32_t maxSamplerBinding() const { return mMaxSamplerBinding; }
    // [W114-VULKAN S4 2026-11-26] The reflected member of the VERTEX stage's uniform block that set(name ...) writes, or null
    // when the vertex stage has no such uniform (wolfvkavatar.cpp checks the joint palette's shape with it).
    const WolfVkSpirv::BlockMember* vertexMember(const std::string& name) const;
    // Debugging (WolfVulkanDebugProbe): the bytes of a uniform as they sit in the stage image right now, as floats;
    // "stage:offset" of each copy in out_where. False when the name is not a block member.
    bool debugMember(const std::string& name, std::vector<float>& out_floats, std::string& out_where) const;
    // [POSE CHECK 2026-11-08] every uniform this mirror holds as "name=v0,v1,..;" (names sorted), the matrices left out
    // (they are set per draw), for comparing the Vulkan side's values before and after a shader reload
    std::string describeUniforms() const;

    // Binds set 0 (both uniform blocks, pushed into the frame ring) on cmd for the next draw. False when the ring is full.
    // [S2 2026-11-26] lean: the set is allocated and written once per program per frame (its descriptors name the ring
    // at offset 0; the draw's place in the ring is the dynamic offset); a stage image is pushed again only after a value in
    // it changed (set/setReserved compare before they write); the bind is skipped when nothing differs (needBind).
    bool bindUniforms(VkCommandBuffer cmd, WolfVkFrameUniforms& ring);
    // Binds set 1 with the given image/sampler pairs by binding index (missing bindings get fallback).
    // [S2] a set is written once per frame per distinct list of image/sampler pairs and reused for later draws with it.
    bool bindSamplers(VkCommandBuffer cmd, WolfVkFrameUniforms& ring, const std::vector<VkDescriptorImageInfo>& by_binding, const VkDescriptorImageInfo& fallback);
    bool bindSamplers(VkCommandBuffer cmd, WolfVkFrameUniforms& ring, const VkDescriptorImageInfo* by_binding, uint32_t binding_count, const VkDescriptorImageInfo& fallback);
    // [S2] Binds set 2 (WolfVkProgram::blocks()): offsets[i] = the ring offset of blocks()[i]'s bytes this frame (the caller
    // pushes them, once per frame per change). No-op for a program without set 2.
    bool bindBlocks(VkCommandBuffer cmd, WolfVkFrameUniforms& ring, const std::vector<uint32_t>& offsets);
    bool bindBlocks(VkCommandBuffer cmd, WolfVkFrameUniforms& ring, const uint32_t* offsets, uint32_t count);

    // Command-buffer-free descriptor & push-constant resolution for multithreaded secondary command buffer recording:
    bool prepareUniforms(WolfVkFrameUniforms& ring, const uint8_t*& out_push_data, uint32_t& out_push_size,
                         VkDescriptorSet& out_set0, uint32_t out_offsets[2], uint32_t& out_offset_count);
    bool prepareSamplers(WolfVkFrameUniforms& ring, const VkDescriptorImageInfo* by_binding, uint32_t binding_count,
                         const VkDescriptorImageInfo& fallback, VkDescriptorSet& out_set1);
    bool prepareBlocks(WolfVkFrameUniforms& ring, const uint32_t* offsets, uint32_t count,
                       VkDescriptorSet& out_set2, uint32_t out_sorted[8], uint32_t& out_sorted_count);

    // Per-shader pipeline lookup fast-cache (remembers last state + renderPass per shader across shader switches)
    VkPipeline cachedPipeline(const WolfVkPipelineState& state, VkRenderPass pass, WolfVkPipelineCache& cache)
    {
        if (mLastPipeline != VK_NULL_HANDLE && mLastRenderPass == pass && state == mLastState)
        {
            return mLastPipeline;
        }
        VkPipeline p = cache.get(mProgram, state, pass);
        if (p != VK_NULL_HANDLE)
        {
            mLastState = state;
            mLastRenderPass = pass;
            mLastPipeline = p;
        }
        return p;
    }
    void invalidateRenderPass(VkRenderPass pass)
    {
        if (mLastRenderPass == pass)
        {
            mLastRenderPass = VK_NULL_HANDLE;
            mLastPipeline = VK_NULL_HANDLE;
        }
    }

    // [STORE STAMP 2026-11-26] Is the SPIR-V store in spv_dir the one made for THIS viewer? The build writes
    // spv_dir/stamp.json (scripts/shader_check.py --vulkan-out: shader_vulkan.store_stamp) with "transform" = the MD5 of
    // the scripts that decide keys and SPIR-V (compiled in here as WOLFVK_TRANSFORM_MD5, indra/llrender/CMakeLists.txt) and
    // "shaders" = the MD5 of the shader tree (shaderTreeMD5 of spv_dir's parent, app_settings/shaders). A store from
    // another commit differs in one of them: its keys are not this viewer's keys, or its SPIR-V was made from other
    // shader text. checkStore reads and compares once per spv_dir and returns the answer; line = one plain sentence for
    // the log either way. create() refuses to load a program from a store that is not current.
    static bool checkStore(const std::string& spv_dir, std::string& line);
    // scripts/shader_vulkan.py tree_md5: every *.glsl under <shaders_dir>/class<N>/, sorted by its path relative to
    // shaders_dir with '/' separators, each as  path NUL bytes NUL. Empty when the tree cannot be read.
    static std::string shaderTreeMD5(const std::string& shaders_dir);
    // [POINT SIZE 2026-11-26] the float the transform adds to every vertex stage's uniform block and writes to
    // gl_PointSize (scripts/shader_vulkan.py POINT_SIZE_UNIFORM). create() starts it at 1.0, glPointSize's initial value
    // (OpenGL 4.6 core 14.4 "Points"); a recorder of a POINTS draw sets GL's current point size with
    // set(POINT_SIZE_UNIFORM, Kind::F1, 1, &size). Sizes other than 1.0 need the largePoints feature (vulkan_core.h
    // VkPhysicalDeviceFeatures::largePoints; the range is VkPhysicalDeviceLimits::pointSizeRange).
    static constexpr const char* POINT_SIZE_UNIFORM = "wolf_point_size";

    // [SPV KEY 2026-11-26] The defines of a unit's recipe: those whose name is one of the file's words (LLShaderMgr::
    // wolfVkFileWords), and those named in the value of a used one, repeated until nothing is added (shader_check.py
    // used_defines). A define the file never names cannot change its text, so it does not split the key.
    static std::map<std::string, std::string> usedDefines(const std::set<std::string>& words, const std::map<std::string, std::string>& defines);
    static bool isWordChar(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; }
    // [SPEC CONSTANTS 2026-11-26] a define the Vulkan side takes as a specialization constant (scripts/shader_vulkan.py
    // SPEC_CONSTANTS, the same table): left out of the recipe, its value given at pipeline creation. id: its constant_id.
    static bool specConstant(const std::string& define_name, uint32_t& id);
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
    void applyProjectionFix(const std::string& name, float* columns) const;   // columns: 16 floats, column-major

    WolfVkDevice* mDevice = nullptr;
    std::string mKey;
    std::string mName;
    WolfVkProgram mProgram;
    std::vector<uint8_t> mVertImage;
    std::vector<uint8_t> mFragImage;
    std::unordered_map<std::string, std::vector<Member>> mMembers;   // a name may be in both stages
    std::unordered_map<std::string, int32_t> mSamplerChannels;
    std::unordered_map<std::string, bool> mUnknownWarned;
    // [S2 2026-11-26] the lean path's state (wolfvkshader.h bindUniforms / setReserved)
    struct Reserved { bool mResolved = false; const std::vector<Member>* mMembers = nullptr; bool mSampler = false; bool mProjection = false; };
    std::vector<Reserved> mReserved;
    void setMembers(const std::string& name, const std::vector<Member>& members, Kind kind, uint32_t count, const void* data, bool transpose, bool projection);
    bool mDirty[2] = { true, true };            // vertex, fragment image changed since its last push
    VkDeviceSize mPushedOffset[2] = { 0, 0 };   // its place in the ring (valid while mPushedEpoch is the ring's epoch)
    uint64_t mPushedEpoch = UINT64_MAX;
    VkDescriptorSet mUniformSet = VK_NULL_HANDLE;
    uint64_t mUniformSetEpoch = UINT64_MAX;
    VkDescriptorSet mBlockSet = VK_NULL_HANDLE;
    uint64_t mBlockSetEpoch = UINT64_MAX;
    std::unordered_map<std::string, VkDescriptorSet> mSamplerSets;   // key: the image/sampler handles in binding order
    uint64_t mSamplerSetsEpoch = UINT64_MAX;
    std::string mSamplerKey;                                          // scratch, kept to avoid an allocation per draw
    std::vector<VkDescriptorImageInfo> mSamplerInfos;                 // scratch
    std::vector<ResolvedSampler> mResolvedSamplers;
    uint32_t mMaxSamplerBinding = 0;
    void rebuildResolvedSamplers();
    VkDescriptorImageInfo mLastSamplerInfos[64] = {};
    uint32_t mLastSamplerCount = 0;
    uint64_t mLastSamplerEpoch = UINT64_MAX;
    VkDescriptorSet mLastSamplerSet = VK_NULL_HANDLE;
    WolfVkPipelineState mLastState{};
    VkRenderPass mLastRenderPass = VK_NULL_HANDLE;
    VkPipeline mLastPipeline = VK_NULL_HANDLE;
};

#endif // WOLF_VK_SHADER_H
