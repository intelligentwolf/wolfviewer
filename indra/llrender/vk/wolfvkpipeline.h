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
 *              uses are debug drawing; MoltenVK refuses TRIANGLE_FAN pipelines, Mac must convert fans - not done here, declared)
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
#include <vector>

class WolfVkDevice;

class WolfVkProgram
{
public:
    // Descriptor set numbers the off-line compiler assigns (scripts/shader_check.py, Vulkan transform):
    static constexpr uint32_t SET_UNIFORMS = 0;    // binding 0 = WolfUniformsV, binding 1 = WolfUniformsF
    static constexpr uint32_t SET_SAMPLERS = 1;    // one binding per sampler, in the compiler's order
    static constexpr uint32_t BINDING_UNIFORMS_VERT = 0;
    static constexpr uint32_t BINDING_UNIFORMS_FRAG = 1;

    struct Stage
    {
        VkShaderModule mModule = VK_NULL_HANDLE;
        WolfVkSpirv::Module mReflection;
        const WolfVkSpirv::UniformBlock* mUniforms = nullptr;   // the stage's WolfUniforms block, or null when it has none
    };

    WolfVkProgram() = default;
    ~WolfVkProgram();

    // vert/frag: SPIR-V words (from the .spv files). name: for the log.
    bool create(WolfVkDevice& device, const std::string& name, const std::vector<uint32_t>& vert, const std::vector<uint32_t>& frag, std::string& error);
    void destroy();
    bool isValid() const { return mPipelineLayout != VK_NULL_HANDLE; }

    const std::string& name() const { return mName; }
    const Stage& vertex() const { return mVertex; }
    const Stage& fragment() const { return mFragment; }
    VkPipelineLayout pipelineLayout() const { return mPipelineLayout; }
    VkDescriptorSetLayout uniformSetLayout() const { return mUniformSetLayout; }
    VkDescriptorSetLayout samplerSetLayout() const { return mSamplerSetLayout; }   // VK_NULL_HANDLE when the program samples nothing
    // Samplers by binding (set 1), merged from both stages.
    const std::vector<WolfVkSpirv::Sampler>& samplers() const { return mSamplers; }
    // Vertex input location of an LLVertexBuffer attribute name ("position", "texcoord0", ...), or -1 when the shader lacks it.
    int32_t inputLocation(const char* attrib_name) const;
    // The attribute mask the vertex shader needs (bit = WolfVkVertexBuffer::AttributeType), from its inputs.
    uint32_t attributeMask() const { return mAttributeMask; }

    // Uniform lookup by name, either stage: (stage pointer, member) or (nullptr, nullptr).
    struct UniformRef { const Stage* mStage; const WolfVkSpirv::BlockMember* mMember; };
    UniformRef findUniform(const std::string& name) const;
    // Sampler binding (set 1) by name, or -1.
    int32_t samplerBinding(const std::string& name) const;

private:
    bool buildStage(WolfVkDevice& device, Stage& stage, const std::vector<uint32_t>& words, const char* block_name, std::string& error);

    WolfVkDevice* mDevice = nullptr;
    std::string mName;
    Stage mVertex;
    Stage mFragment;
    std::vector<WolfVkSpirv::Sampler> mSamplers;
    std::map<std::string, uint32_t> mSamplerBindings;
    std::map<std::string, uint32_t> mInputLocations;
    uint32_t mAttributeMask = 0;
    VkDescriptorSetLayout mUniformSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout mSamplerSetLayout = VK_NULL_HANDLE;
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

    uint64_t hash() const;
    bool operator==(const WolfVkPipelineState& o) const;
};

class WolfVkPipelineCache
{
public:
    void init(WolfVkDevice& device);
    void destroy();
    // The pipeline for program + state in render_pass (subpass 0); created on first use. VK_NULL_HANDLE on failure (logged once).
    VkPipeline get(const WolfVkProgram& program, const WolfVkPipelineState& state, VkRenderPass render_pass);
    uint32_t pipelineCount() const { return static_cast<uint32_t>(mPipelines.size()); }

private:
    struct Key
    {
        const WolfVkProgram* mProgram;
        VkRenderPass mPass;
        WolfVkPipelineState mState;
    };
    struct KeyHash { size_t operator()(const Key& k) const; };
    struct KeyEq { bool operator()(const Key& a, const Key& b) const; };

    VkPipeline build(const WolfVkProgram& program, const WolfVkPipelineState& state, VkRenderPass render_pass, std::string& error);

    WolfVkDevice* mDevice = nullptr;
    VkPipelineCache mCache = VK_NULL_HANDLE;
    std::unordered_map<Key, VkPipeline, KeyHash, KeyEq> mPipelines;
    std::unordered_map<Key, bool, KeyHash, KeyEq> mFailed;
};

#endif // WOLF_VK_PIPELINE_H
