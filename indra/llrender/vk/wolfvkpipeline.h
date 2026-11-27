/**
 * @file wolfvkpipeline.h
 * @brief Shader programs from SPIR-V with reflected layouts, and the graphics pipeline cache keyed on render state.
 *
 * [VULKAN BACKEND 2026-11-08]
 *
 * WolfVkProgram: a vertex + fragment SPIR-V pair (compiled off-line by scripts/shader_check.py --vulkan-out, see
 * docs/VULKAN_BACKEND.md) with its reflection (wolfvkspirv.h): the per-stage uniform blocks WolfUniformsV /
 * WolfUniformsF (set 0, bindings 0 and 1), the samplers (set 1, one binding each), the vertex input locations by
 * attribute name. It owns the descriptor set layouts and the pipeline layout built from that reflection.
 *
 * WolfVkPipelineState: the fixed-function state the viewer sets through LLRender / LLGLState, read from those
 * classes and mapped one to one:
 *   blend      LLRender::blendFunc (llrender.cpp:1449-1486, sGLBlendFactor :103-116) -> VkPipelineColorBlendAttachmentState
 *              (vulkan_core.h:4289), blend enabled by LLGLEnable(GL_BLEND); the factors (eBlendFactor llrender.h:353-366):
 *              BF_ONE -> VK_BLEND_FACTOR_ONE (:2449), BF_ZERO -> ZERO (:2448), BF_DEST_COLOR -> DST_COLOR (:2452),
 *              BF_SOURCE_COLOR -> SRC_COLOR (:2450), BF_ONE_MINUS_DEST_COLOR -> ONE_MINUS_DST_COLOR (:2453),
 *              BF_ONE_MINUS_SOURCE_COLOR -> ONE_MINUS_SRC_COLOR (:2451), BF_DEST_ALPHA -> DST_ALPHA (:2456),
 *              BF_SOURCE_ALPHA -> SRC_ALPHA (:2454), BF_ONE_MINUS_DEST_ALPHA -> ONE_MINUS_DST_ALPHA (:2457),
 *              BF_ONE_MINUS_SOURCE_ALPHA -> ONE_MINUS_SRC_ALPHA (:2455); the equation is GL's default GL_FUNC_ADD -> VK_BLEND_OP_ADD (:2471)
 *   colour mask LLRender::setColorMask (llrender.cpp:1397-1415 glColorMask) -> colorWriteMask bits (:3315-3318)
 *   depth      LLGLDepthTest (llgl.cpp:2822-2855: glEnable(GL_DEPTH_TEST), glDepthFunc, glDepthMask; writes forced off when
 *              the test is off) -> VkPipelineDepthStencilStateCreateInfo (:4311); GL_NEVER..GL_ALWAYS (gl.h:259-266, 0x200..0x207)
 *              -> VK_COMPARE_OP_NEVER..ALWAYS (:2392-2399), the same order in both enumerations
 *   cull       LLGLEnable(GL_CULL_FACE) with glCullFace(GL_BACK) (llrender.cpp:880, the only glCullFace) and GL's default
 *              front face GL_CCW -> VK_CULL_MODE_BACK_BIT (:3326) / VK_CULL_MODE_NONE (:3324), VK_FRONT_FACE_COUNTER_CLOCKWISE
 *              (:2620); correct together with the negative-height viewport (wolfvkrendertarget.cpp begin)
 *   polygon    gUseWireframe glPolygonMode(GL_LINE) (pipeline.cpp:4301 and others) -> VK_POLYGON_MODE_LINE (:2680), else FILL (:2679)
 *   topology   LLRender::eGeomModes (llrender.h:315-324) through LLVertexBuffer::sGLMode (llvertexbuffer.cpp:740-748):
 *              TRIANGLES -> TRIANGLE_LIST (:2667), TRIANGLE_STRIP -> TRIANGLE_STRIP (:2668), TRIANGLE_FAN -> TRIANGLE_FAN (:2669),
 *              POINTS -> POINT_LIST (:2664), LINES -> LINE_LIST (:2665), LINE_STRIP -> LINE_STRIP (:2666), LINE_LOOP -> LINE_STRIP
 *              (Vulkan has no loop; the closing segment is missing: a known limitation of this cut, the viewer's 21 LINE_LOOP
 *              uses are debug drawing). [W114-VULKAN MAC 2026-11-26] TRIANGLE_FAN on a VK_KHR_portability_subset device needs
 *              its triangleFans feature (VUID-VkPipelineInputAssemblyStateCreateInfo-triangleFans-04452); MoltenVK 1.4.2
 *              offers it (MVKDevice.mm:610) and draws fans as indexed triangles itself (MVKCmdDraw.mm:182 "Metal doesn't
 *              support triangle fans, so encode it as triangles"), and WolfVkDevice enables it. A device without it gets
 *              no fan pipeline (build() refuses with a reason); a recorder then draws the fan as a TRIANGLE_LIST through
 *              WolfVkTopology::fanToTriangleList below, which keeps Vulkan's own fan triangles and their order.
 *   vertex input  the attribute mask (which LLVertexBuffer arrays the draw binds) + the program's input locations
 * Viewport and scissor are dynamic (:2526-2527), set per render target.
 *
 * WolfVkPipelineCache: pipelines by (program, state, render pass, attachment count) in a hash map, created on first
 * use with a VkPipelineCache (:4083) so the driver reuses its own compiled state across runs.
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

#ifndef WOLF_VK_PIPELINE_H
#define WOLF_VK_PIPELINE_H

#include "wolfvkspirv.h"
#include "wolfvkloader.h"
#include <map>
#include <string>
#include <unordered_map>
#include <functional>   // [V5] forget()
#include <vector>

class WolfVkDevice;

// [W114-VULKAN MAC 2026-11-26] Triangle fan as a triangle list, for a device whose VK_KHR_portability_subset lacks
// triangleFans (WolfVkDevice::triangleFans()). Vulkan's fan (Vulkan spec "Triangle Fans", chapters/drawing.adoc):
// p_i = {v_(i+1), v_(i+2), v_0}, max(0, vertexCount - 2) primitives, provoking vertex v_(i+1). Each p_i is appended in
// that same vertex order, so winding (cull) and the first-vertex provoking vertex of a TRIANGLE_LIST
// (VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX: v_(3i)) are those of the fan, and the triangles come in the fan's order
// (blending order unchanged). The fan's vertices are first .. first+count-1, or indices[first .. first+count-1] when
// indices is not null (an indexed fan); out receives the list's vertex indices (cleared first).
namespace WolfVkTopology
{
    void fanToTriangleList(uint32_t first, uint32_t count, const uint16_t* indices, std::vector<uint32_t>& out);
    void fanToTriangleList(uint32_t first, uint32_t count, const uint32_t* indices, std::vector<uint32_t>& out);
}

class WolfVkProgram
{
public:
    // Descriptor set numbers the off-line compiler assigns (scripts/shader_check.py, Vulkan transform):
    static constexpr uint32_t SET_UNIFORMS = 0;    // binding 0 = WolfUniformsV, binding 1 = WolfUniformsF
    static constexpr uint32_t SET_SAMPLERS = 1;    // one binding per sampler, in the compiler's order
    static constexpr uint32_t BINDING_UNIFORMS_VERT = 0;
    static constexpr uint32_t BINDING_UNIFORMS_FRAG = 1;
    // [VULKAN G-BUFFER S2 2026-11-26] the viewer's own std140 blocks: shader_vulkan.py gives them set 2 with
    // LLGLSLShader::UniformBlock's order as the binding (llglslshader.h UB_REFLECTION_PROBES 0 .. UB_WOLF_EEP_GAINS 4)
    static constexpr uint32_t SET_BLOCKS = 2;
    struct BlockBinding
    {
        std::string mName;      // the block's name (ReflectionProbes, ...)
        uint32_t mBinding = 0;  // = the GL binding point (llglslshader.cpp mapUniforms glUniformBlockBinding(..., i))
        uint32_t mSize = 0;     // std140 bytes the shader reads
        bool mUsed = false;     // statically used by either stage (WolfVkSpirv::UniformBlock::mUsed)
    };

    struct Stage
    {
        VkShaderModule mModule = VK_NULL_HANDLE;
        WolfVkSpirv::Module mReflection;
        const WolfVkSpirv::UniformBlock* mUniforms = nullptr;      // the stage's WolfUniforms UBO block (set 0), or null
        const WolfVkSpirv::UniformBlock* mPushConstants = nullptr; // WolfUniformsV promoted to PushConstant, or null
    };

    WolfVkProgram() = default;
    ~WolfVkProgram();

    // vert/frag: SPIR-V words (from the .spv files). geom: the geometry stage's words, empty for a program without one
    // ([GEOMETRY 2026-11-26] scripts/shader_vulkan.py STAGES; the stage has no uniform block). name: for the log.
    bool create(WolfVkDevice& device, const std::string& name, const std::vector<uint32_t>& vert, const std::vector<uint32_t>& geom,
                const std::vector<uint32_t>& frag, std::string& error);
    void destroy();
    bool isValid() const { return mPipelineLayout != VK_NULL_HANDLE; }

    const std::string& name() const { return mName; }
    const Stage& vertex() const { return mVertex; }
    const Stage& fragment() const { return mFragment; }
    // [GEOMETRY 2026-11-26] the geometry stage; its mModule is VK_NULL_HANDLE when the program has none
    const Stage& geometry() const { return mGeometry; }
    bool hasGeometry() const { return mGeometry.mModule != VK_NULL_HANDLE; }
    // [SPEC CONSTANTS 2026-11-26] a 32-bit float specialization constant every stage of this program's pipelines gets
    // (constant_id, scripts/shader_vulkan.py SPEC_CONSTANTS); set after create, before the first pipeline is built.
    // An id a stage does not declare is ignored by Vulkan (VkSpecializationInfo, spec 10.11 "Specialization Constants").
    void setSpecConstant(uint32_t id, float value);
    bool hasSpecConstant(uint32_t id) const;
    // Null when the program has none.
    const VkSpecializationInfo* specialization() const { return mSpecEntries.empty() ? nullptr : &mSpecInfo; }
    VkPipelineLayout pipelineLayout() const { return mPipelineLayout; }
    VkDescriptorSetLayout uniformSetLayout() const { return mUniformSetLayout; }
    VkDescriptorSetLayout samplerSetLayout() const { return mSamplerSetLayout; }   // VK_NULL_HANDLE when the program samples nothing
    // Every sampler the stages DECLARE (set 1), merged; mUsed = some stage statically uses it. Names and binding numbers
    // for the uniform setters; NOT the layout (samplerSlots()).
    const std::vector<WolfVkSpirv::Sampler>& samplers() const { return mSamplers; }
    // [W114-VULKAN S7 2026-11-26] The set-1 layout: one slot per statically used sampler, the SPIR-V binding number
    // kept, visible to exactly the stages that use it (Mac gap (a): a device's per-stage sampler limit counts what the
    // layout holds, not what the shaders declare; wolfvkpipeline.cpp create()). A descriptor write or a descriptor count
    // for set 1 must follow this list and nothing else (VUID-VkWriteDescriptorSet-dstBinding-00315).
    struct SamplerSlot
    {
        uint32_t mBinding = 0;
        uint32_t mCount = 1;              // descriptors (the sampler's array size)
        VkShaderStageFlags mStages = 0;   // the stages that statically use it
    };
    const std::vector<SamplerSlot>& samplerSlots() const { return mSamplerSlots; }
    // binding (or an element of an arrayed slot: slot binding + element, as bindSamplers numbers them) is in the layout
    bool samplerInLayout(uint32_t binding) const;
    // Descriptors of the layout visible to a stage (0 vertex, 1 geometry, 2 fragment): samplers + uniform buffers, for
    // VkPhysicalDeviceLimits::maxPerStageResources at pipeline creation (the colour attachments are added there).
    uint32_t stageResources(uint32_t stage) const { return stage < 3 ? mStageResources[stage] : 0u; }
    // Vertex input location of an LLVertexBuffer attribute name ("position", "texcoord0", ...), or -1 when the shader lacks it.
    int32_t inputLocation(const char* attrib_name) const;
    // The attribute mask the vertex shader needs (bit = WolfVkVertexBuffer::AttributeType), from its inputs.
    uint32_t attributeMask() const { return mAttributeMask; }

    // Uniform lookup by name, either stage: (stage pointer, member) or (nullptr, nullptr).
    struct UniformRef { const Stage* mStage; const WolfVkSpirv::BlockMember* mMember; };
    UniformRef findUniform(const std::string& name) const;
    // Sampler binding (set 1) by name, or -1.
    int32_t samplerBinding(const std::string& name) const;
    // [S2] set 2 (empty when the program has none) and its layout (VK_NULL_HANDLE then; the pipeline layout has 2 sets)
    const std::vector<BlockBinding>& blocks() const { return mBlocks; }
    VkDescriptorSetLayout blockSetLayout() const { return mBlockSetLayout; }

private:
    bool buildStage(WolfVkDevice& device, Stage& stage, const std::vector<uint32_t>& words, const char* block_name, std::string& error);

    WolfVkDevice* mDevice = nullptr;
    std::string mName;
    Stage mVertex;
    Stage mFragment;
    Stage mGeometry;
    std::vector<VkSpecializationMapEntry> mSpecEntries;
    std::vector<float> mSpecData;
    VkSpecializationInfo mSpecInfo{};
    std::vector<WolfVkSpirv::Sampler> mSamplers;
    std::vector<VkShaderStageFlags> mSamplerStages;   // [S7] parallel to mSamplers: the stages that statically use each
    std::vector<SamplerSlot> mSamplerSlots;           // [S7] the set-1 layout
    uint32_t mStageResources[3] = { 0, 0, 0 };        // [S7]
    std::map<std::string, uint32_t> mSamplerBindings;
    std::map<std::string, uint32_t> mInputLocations;
    uint32_t mAttributeMask = 0;
    VkDescriptorSetLayout mUniformSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout mSamplerSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout mBlockSetLayout = VK_NULL_HANDLE;   // [S2]
    std::vector<BlockBinding> mBlocks;                      // [S2]
    VkPipelineLayout mPipelineLayout = VK_NULL_HANDLE;
};

struct WolfVkPipelineState
{
    // LLRender::eBlendFactor values (llrender.h:353-366), BF_UNDEF = 10 means "blend disabled"
    uint8_t mBlendSrcColor = 7;   // BF_SOURCE_ALPHA
    uint8_t mBlendDstColor = 9;   // BF_ONE_MINUS_SOURCE_ALPHA
    uint8_t mBlendSrcAlpha = 7;
    uint8_t mBlendDstAlpha = 9;
    bool mBlendEnable = false;
    bool mColorMask[4] = { true, true, true, true };
    bool mDepthTest = true;
    bool mDepthWrite = true;
    uint32_t mDepthFunc = 0x0203;   // GL_LEQUAL (gl.h:262), LLGLDepthTest's default (llglstates.h:41)
    bool mCullFace = true;          // LLGLEnable(GL_CULL_FACE)
    // [W114-VULKAN S4 2026-11-26] with mCullFace: front faces are culled instead of back faces (glCullFace(GL_FRONT):
    // llviewerjoint.cpp:122, the inside pass of small avatar hair and skirts). Ignored when mCullFace is false.
    bool mCullFront = false;
    bool mWireframe = false;
    uint8_t mTopology = 0;          // LLRender::eGeomModes: 0 TRIANGLES .. 6 LINE_LOOP
    uint32_t mAttributeMask = 0;    // the LLVertexBuffer arrays bound (WolfVkVertexBuffer::bind order)
    uint32_t mColorAttachments = 1;
    // [COVERAGE 2026-11-08] the last colour attachment is the wolf_coverage output (shader_vulkan.py): written as it is,
    // no blending and no colour mask, whatever the state says for the real attachments
    bool mCoverageAttachment = false;
    // [DEPTH BIAS 2026-11-08] polygon offset for a pass that re-draws surfaces OpenGL has already written to the depth
    // buffer (the fullbright overlay): without it the two computations of the same depth z-fight (hatched signs in the
    // Sandbox pictures). glPolygonOffset(factor, units) semantics: VkPipelineRasterizationStateCreateInfo depthBiasSlopeFactor
    // = factor, depthBiasConstantFactor = units (Vulkan spec 27.7.3 "Depth Bias"). 0/0 = off.
    float mDepthBiasConstant = 0.f;
    float mDepthBiasSlope = 0.f;
    // [W114-VULKAN S5 2026-11-26] GL_DEPTH_CLAMP (the sun shadow cascades, pipeline.cpp renderShadow LLGLEnable
    // clamp_depth): "If depth clamping is enabled ... clipping against the near and far planes is disabled" and the
    // window z is clamped to the depth range (OpenGL 4.6 13.7 "Primitive Clipping", 15.1 "Depth Clamping").
    // VkPipelineRasterizationStateCreateInfo::depthClampEnable is the same pair: clamp to the viewport's minDepth /
    // maxDepth (Vulkan spec 29.9 "Depth Test") and, without VkPipelineRasterizationDepthClipStateCreateInfoEXT,
    // "depth clipping is disabled when depthClampEnable is VK_TRUE" (spec 27.2 "Primitive Clipping"). Needs
    // WolfVkDevice::depthClamp(); a pipeline asked for it on a device without the feature is refused, never drawn unclamped.
    bool mDepthClamp = false;

    uint64_t hash() const;
    bool operator==(const WolfVkPipelineState& o) const;
};

class WolfVkPipelineCache
{
public:
    WolfVkPipelineCache() = default;
    // [B2 2026-11-11] the cache is registered by address (sLiveCaches): it unregisters itself whatever ends it, and is
    // never copied or moved
    ~WolfVkPipelineCache();
    WolfVkPipelineCache(const WolfVkPipelineCache&) = delete;
    WolfVkPipelineCache& operator=(const WolfVkPipelineCache&) = delete;
    // [W114-VULKAN S7 2026-11-26] disk_file (Mac gap (i): the driver's compiled pipelines kept between runs, so a start
    // does not compile every pipeline again - on MoltenVK each one is a Metal shader translation): the full path of the
    // file the VkPipelineCache is loaded from here and written to by saveIfDue() / destroy(); empty = memory only.
    // What is loaded must carry this file's own envelope (length and checksum of the payload) and the header of THIS
    // device and driver (VkPipelineCacheHeaderVersionOne: vendorID, deviceID, pipelineCacheUUID); anything else is
    // ignored and replaced at the next save.
    void init(WolfVkDevice& device, const std::string& disk_file = std::string());
    // Writes the cache to the disk file when pipelines were built since the last write and at least SAVE_INTERVAL_SECONDS
    // have passed since then (now_seconds: any monotonic clock). One vkGetPipelineCacheData and one file write; called
    // once a frame by the owner. destroy() writes unconditionally when anything is unsaved.
    void saveIfDue(double now_seconds);
    void destroy();
    // [B2 2026-11-11] the device is hung: the pipelines are left to it (no Vulkan call) and the cache unregisters
    void abandon();
    // The pipeline for program + state in render_pass (subpass 0); created on first use. VK_NULL_HANDLE on failure (logged once).
    VkPipeline get(const WolfVkProgram& program, const WolfVkPipelineState& state, VkRenderPass render_pass);
    uint32_t pipelineCount() const { return static_cast<uint32_t>(mPipelines.size()); }
    // [V5 2026-11-11] A render pass or program is going away: every live cache drops (and destroys, deferred) the
    // pipelines made for it. Called by their owners before the object's own destruction is filed.
    static void forgetRenderPass(VkRenderPass pass);
    static void forgetProgram(const WolfVkProgram* program);

private:
    struct Key
    {
        const WolfVkProgram* mProgram;
        VkRenderPass mPass;
        WolfVkPipelineState mState;
    };
    struct KeyHash { size_t operator()(const Key& k) const; };
    struct KeyEq { bool operator()(const Key& a, const Key& b) const; };
    void forget(const std::function<bool(const Key&)>& match);

    VkPipeline build(const WolfVkProgram& program, const WolfVkPipelineState& state, VkRenderPass render_pass, std::string& error);
    // [W114-VULKAN S7 2026-11-26] the disk file (wolfvkpipeline.cpp "pipeline cache on disk")
    static constexpr double SAVE_INTERVAL_SECONDS = 60.0;
    // No eternal stores: a cache file larger than this is neither read nor written (the old file is removed).
    static constexpr size_t DISK_LIMIT_BYTES = 256u * 1024u * 1024u;
    bool loadDisk(std::vector<uint8_t>& payload) const;
    void saveDisk();
    std::string mDiskFile;
    uint32_t mBuiltSinceSave = 0;
    double mLastSave = -1.0;   // < 0: the clock has not been seen yet

    WolfVkDevice* mDevice = nullptr;
    VkPipelineCache mCache = VK_NULL_HANDLE;
    std::unordered_map<Key, VkPipeline, KeyHash, KeyEq> mPipelines;
    std::unordered_map<Key, bool, KeyHash, KeyEq> mFailed;
};

#endif // WOLF_VK_PIPELINE_H
