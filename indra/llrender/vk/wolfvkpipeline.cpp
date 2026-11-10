/**
 * @file wolfvkpipeline.cpp
 * @brief Shader programs from SPIR-V and the graphics pipeline cache (see wolfvkpipeline.h for the state mapping).
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
#include "wolfvkpipeline.h"
#include "wolfvkdevice.h"
#include "wolfvkbuffer.h"
#include <cstring>

// ---------------------------------------------------------------------------------------------------------------
// WolfVkProgram

WolfVkProgram::~WolfVkProgram()
{
    destroy();
}

bool WolfVkProgram::buildStage(WolfVkDevice& device, Stage& stage, const std::vector<uint32_t>& words, const char* block_name, std::string& error)
{
    if (!WolfVkSpirv::reflect(words.data(), words.size(), stage.mReflection, error))
    {
        return false;
    }
    // Source: vulkan_core.h:4075 VkShaderModuleCreateInfo { sType, pNext, flags, codeSize (bytes), pCode }
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;   // :223
    info.codeSize = words.size() * sizeof(uint32_t);
    info.pCode = words.data();
    VkResult res = gVk.vkCreateShaderModule(device.device(), &info, nullptr, &stage.mModule);
    if (res != VK_SUCCESS)
    {
        error = std::string("vkCreateShaderModule: ") + WolfVkLoader::resultString(res);
        stage.mModule = VK_NULL_HANDLE;
        return false;
    }
    stage.mUniforms = nullptr;
    for (const WolfVkSpirv::UniformBlock& b : stage.mReflection.mBlocks)
    {
        if (b.mName == block_name && b.mSet == SET_UNIFORMS)
        {
            stage.mUniforms = &b;
        }
    }
    return true;
}

bool WolfVkProgram::create(WolfVkDevice& device, const std::string& name, const std::vector<uint32_t>& vert, const std::vector<uint32_t>& frag, std::string& error)
{
    destroy();
    mDevice = &device;
    mName = name;
    if (!buildStage(device, mVertex, vert, "WolfUniformsV", error) || !buildStage(device, mFragment, frag, "WolfUniformsF", error))
    {
        error = name + ": " + error;
        destroy();
        return false;
    }
    if (mVertex.mReflection.mExecutionModel != 0 || mFragment.mReflection.mExecutionModel != 4)   // spirv.hpp:66 Vertex = 0, :70 Fragment = 4
    {
        error = name + ": the SPIR-V stages are not a vertex and a fragment shader";
        destroy();
        return false;
    }

    // Vertex inputs -> attribute locations and the attribute mask
    for (const WolfVkSpirv::Input& in : mVertex.mReflection.mInputs)
    {
        mInputLocations[in.mName] = in.mLocation;
        for (uint32_t t = 0; t < WolfVkVertexBuffer::TYPE_MAX; ++t)
        {
            if (in.mName == WolfVkVertexBuffer::sAttribName[t])
            {
                mAttributeMask |= 1u << t;
            }
        }
    }

    // Samplers (set 1), merged across stages; a name in both stages must have the same binding (the compiler assigns
    // bindings per program, so it does; checked anyway)
    for (const Stage* s : { &mVertex, &mFragment })
    {
        for (const WolfVkSpirv::Sampler& smp : s->mReflection.mSamplers)
        {
            if (smp.mSet != SET_SAMPLERS)
            {
                error = name + ": sampler " + smp.mName + " is in descriptor set " + std::to_string(smp.mSet) + ", expected " + std::to_string(SET_SAMPLERS);
                destroy();
                return false;
            }
            auto it = mSamplerBindings.find(smp.mName);
            if (it != mSamplerBindings.end())
            {
                if (it->second != smp.mBinding)
                {
                    error = name + ": sampler " + smp.mName + " has two bindings";
                    destroy();
                    return false;
                }
                continue;
            }
            mSamplerBindings[smp.mName] = smp.mBinding;
            mSamplers.push_back(smp);
        }
    }

    VkDevice dev = device.device();
    // Set 0: the two uniform blocks. Source: vulkan_core.h:4207 VkDescriptorSetLayoutBinding { binding, descriptorType, descriptorCount,
    // stageFlags, pImmutableSamplers }; :2418 UNIFORM_BUFFER_DYNAMIC (one ring buffer, the offset per draw at bind time)
    std::vector<VkDescriptorSetLayoutBinding> uniform_bindings;
    if (mVertex.mUniforms)
    {
        uniform_bindings.push_back({ BINDING_UNIFORMS_VERT, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr });   // :2883
    }
    if (mFragment.mUniforms)
    {
        uniform_bindings.push_back({ BINDING_UNIFORMS_FRAG, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr });   // :2887
    }
    // Source: vulkan_core.h:4215 VkDescriptorSetLayoutCreateInfo { sType, pNext, flags, bindingCount, pBindings }
    VkDescriptorSetLayoutCreateInfo set0{};
    set0.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;   // :239
    set0.bindingCount = static_cast<uint32_t>(uniform_bindings.size());
    set0.pBindings = uniform_bindings.empty() ? nullptr : uniform_bindings.data();
    VkResult res = gVk.vkCreateDescriptorSetLayout(dev, &set0, nullptr, &mUniformSetLayout);
    if (res != VK_SUCCESS)
    {
        error = name + ": vkCreateDescriptorSetLayout (uniforms): " + WolfVkLoader::resultString(res);
        destroy();
        return false;
    }
    // Set 1: samplers, each visible to both stages (a sampler used by one stage only is still declared for both;
    // harmless, and the layout stays independent of which stage reads it)
    std::vector<VkDescriptorSetLayoutBinding> sampler_bindings;
    for (const WolfVkSpirv::Sampler& smp : mSamplers)
    {
        sampler_bindings.push_back({ smp.mBinding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, smp.mArraySize, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr });   // :2411
    }
    VkDescriptorSetLayoutCreateInfo set1{};
    set1.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set1.bindingCount = static_cast<uint32_t>(sampler_bindings.size());
    set1.pBindings = sampler_bindings.empty() ? nullptr : sampler_bindings.data();
    res = gVk.vkCreateDescriptorSetLayout(dev, &set1, nullptr, &mSamplerSetLayout);
    if (res != VK_SUCCESS)
    {
        error = name + ": vkCreateDescriptorSetLayout (samplers): " + WolfVkLoader::resultString(res);
        destroy();
        return false;
    }
    // Source: vulkan_core.h:4130 VkPipelineLayoutCreateInfo { sType, pNext, flags, setLayoutCount, pSetLayouts, pushConstantRangeCount, pPushConstantRanges }
    VkDescriptorSetLayout layouts[2] = { mUniformSetLayout, mSamplerSetLayout };
    VkPipelineLayoutCreateInfo pl{};
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;   // :237
    pl.setLayoutCount = 2;
    pl.pSetLayouts = layouts;
    res = gVk.vkCreatePipelineLayout(dev, &pl, nullptr, &mPipelineLayout);
    if (res != VK_SUCCESS)
    {
        error = name + ": vkCreatePipelineLayout: " + WolfVkLoader::resultString(res);
        destroy();
        return false;
    }
    LL_INFOS("Vulkan") << "Program " << name << ": vertex uniforms " << (mVertex.mUniforms ? mVertex.mUniforms->mSize : 0u) << " bytes ("
                       << (mVertex.mUniforms ? mVertex.mUniforms->mMembers.size() : 0u) << " members), fragment uniforms "
                       << (mFragment.mUniforms ? mFragment.mUniforms->mSize : 0u) << " bytes (" << (mFragment.mUniforms ? mFragment.mUniforms->mMembers.size() : 0u)
                       << " members), " << mSamplers.size() << " samplers, " << mInputLocations.size() << " vertex inputs, attribute mask 0x" << std::hex << mAttributeMask << std::dec << LL_ENDL;
    return true;
}

void WolfVkProgram::destroy()
{
    if (mDevice && mDevice->hasDevice())
    {
        WolfVkDevice* device = mDevice;
        VkShaderModule v = mVertex.mModule, f = mFragment.mModule;
        VkDescriptorSetLayout s0 = mUniformSetLayout, s1 = mSamplerSetLayout;
        VkPipelineLayout pl = mPipelineLayout;
        device->deferDestroy([device, v, f, s0, s1, pl]()
        {
            if (pl) gVk.vkDestroyPipelineLayout(device->device(), pl, nullptr);
            if (s0) gVk.vkDestroyDescriptorSetLayout(device->device(), s0, nullptr);
            if (s1) gVk.vkDestroyDescriptorSetLayout(device->device(), s1, nullptr);
            if (v) gVk.vkDestroyShaderModule(device->device(), v, nullptr);
            if (f) gVk.vkDestroyShaderModule(device->device(), f, nullptr);
        });
    }
    mVertex = Stage();
    mFragment = Stage();
    mSamplers.clear();
    mSamplerBindings.clear();
    mInputLocations.clear();
    mAttributeMask = 0;
    mUniformSetLayout = mSamplerSetLayout = VK_NULL_HANDLE;
    mPipelineLayout = VK_NULL_HANDLE;
    mDevice = nullptr;
}

int32_t WolfVkProgram::inputLocation(const char* attrib_name) const
{
    auto it = mInputLocations.find(attrib_name);
    return it == mInputLocations.end() ? -1 : static_cast<int32_t>(it->second);
}

WolfVkProgram::UniformRef WolfVkProgram::findUniform(const std::string& name) const
{
    for (const Stage* s : { &mVertex, &mFragment })
    {
        if (!s->mUniforms) continue;
        for (const WolfVkSpirv::BlockMember& m : s->mUniforms->mMembers)
        {
            if (m.mName == name) return { s, &m };
        }
    }
    return { nullptr, nullptr };
}

int32_t WolfVkProgram::samplerBinding(const std::string& name) const
{
    auto it = mSamplerBindings.find(name);
    return it == mSamplerBindings.end() ? -1 : static_cast<int32_t>(it->second);
}

// ---------------------------------------------------------------------------------------------------------------
// WolfVkPipelineState

uint64_t WolfVkPipelineState::hash() const
{
    // FNV-1a over the bytes that matter (no padding: fields are written one by one)
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ull; };
    mix(mBlendSrcColor); mix(mBlendDstColor); mix(mBlendSrcAlpha); mix(mBlendDstAlpha); mix(mBlendEnable ? 1u : 0u);
    mix((mColorMask[0] ? 1u : 0u) | (mColorMask[1] ? 2u : 0u) | (mColorMask[2] ? 4u : 0u) | (mColorMask[3] ? 8u : 0u));
    mix(mDepthTest ? 1u : 0u); mix(mDepthWrite ? 1u : 0u); mix(mDepthFunc); mix(mCullFace ? 1u : 0u); mix(mWireframe ? 1u : 0u);
    mix(mTopology); mix(mAttributeMask); mix(mColorAttachments); mix(mCoverageAttachment ? 1u : 0u);
    uint32_t bc = 0, bs = 0; memcpy(&bc, &mDepthBiasConstant, 4); memcpy(&bs, &mDepthBiasSlope, 4); mix(bc); mix(bs);
    return h;
}

bool WolfVkPipelineState::operator==(const WolfVkPipelineState& o) const
{
    return mBlendSrcColor == o.mBlendSrcColor && mBlendDstColor == o.mBlendDstColor && mBlendSrcAlpha == o.mBlendSrcAlpha
        && mBlendDstAlpha == o.mBlendDstAlpha && mBlendEnable == o.mBlendEnable
        && mColorMask[0] == o.mColorMask[0] && mColorMask[1] == o.mColorMask[1] && mColorMask[2] == o.mColorMask[2] && mColorMask[3] == o.mColorMask[3]
        && mDepthTest == o.mDepthTest && mDepthWrite == o.mDepthWrite && mDepthFunc == o.mDepthFunc && mCullFace == o.mCullFace
        && mWireframe == o.mWireframe && mTopology == o.mTopology && mAttributeMask == o.mAttributeMask && mColorAttachments == o.mColorAttachments
        && mCoverageAttachment == o.mCoverageAttachment && mDepthBiasConstant == o.mDepthBiasConstant && mDepthBiasSlope == o.mDepthBiasSlope;
}

// ---------------------------------------------------------------------------------------------------------------
// WolfVkPipelineCache

size_t WolfVkPipelineCache::KeyHash::operator()(const Key& k) const
{
    uint64_t h = k.mState.hash();
    h ^= reinterpret_cast<uintptr_t>(k.mProgram) * 0x9E3779B97F4A7C15ull;
    h ^= reinterpret_cast<uintptr_t>(k.mPass) * 0xC2B2AE3D27D4EB4Full;
    return static_cast<size_t>(h);
}

bool WolfVkPipelineCache::KeyEq::operator()(const Key& a, const Key& b) const
{
    return a.mProgram == b.mProgram && a.mPass == b.mPass && a.mState == b.mState;
}

void WolfVkPipelineCache::init(WolfVkDevice& device)
{
    mDevice = &device;
    // Source: vulkan_core.h:4083 VkPipelineCacheCreateInfo { sType, pNext, flags, initialDataSize, pInitialData }
    VkPipelineCacheCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;   // :224
    VkResult res = gVk.vkCreatePipelineCache(device.device(), &info, nullptr, &mCache);
    if (res != VK_SUCCESS)
    {
        LL_WARNS("Vulkan") << "vkCreatePipelineCache: " << WolfVkLoader::resultString(res) << " (pipelines are built without a cache)" << LL_ENDL;
        mCache = VK_NULL_HANDLE;
    }
}

void WolfVkPipelineCache::destroy()
{
    // [HANG GUARD 2026-11-08 8.2k] bounded idle (WolfVkDevice::waitIdle) instead of vkDeviceWaitIdle; a hung device keeps its objects
    if (mDevice && mDevice->hasDevice() && mDevice->waitIdle())
    {
        for (auto& kv : mPipelines)
        {
            gVk.vkDestroyPipeline(mDevice->device(), kv.second, nullptr);
        }
        if (mCache != VK_NULL_HANDLE)
        {
            gVk.vkDestroyPipelineCache(mDevice->device(), mCache, nullptr);
        }
    }
    mPipelines.clear();
    mFailed.clear();
    mCache = VK_NULL_HANDLE;
    mDevice = nullptr;
}

VkPipeline WolfVkPipelineCache::get(const WolfVkProgram& program, const WolfVkPipelineState& state, VkRenderPass render_pass)
{
    Key key{ &program, render_pass, state };
    auto it = mPipelines.find(key);
    if (it != mPipelines.end())
    {
        return it->second;
    }
    if (mFailed.count(key))
    {
        return VK_NULL_HANDLE;
    }
    std::string error;
    VkPipeline p = build(program, state, render_pass, error);
    if (p == VK_NULL_HANDLE)
    {
        LL_WARNS("Vulkan") << "Pipeline for " << program.name() << ": " << error << LL_ENDL;
        mFailed[key] = true;
        return VK_NULL_HANDLE;
    }
    mPipelines[key] = p;
    return p;
}

VkPipeline WolfVkPipelineCache::build(const WolfVkProgram& program, const WolfVkPipelineState& state, VkRenderPass render_pass, std::string& error)
{
    // Source: vulkan_core.h:4104 VkPipelineShaderStageCreateInfo { sType, pNext, flags, stage, module, pName, pSpecializationInfo }
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;   // :225
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = program.vertex().mModule;
    stages[0].pName = program.vertex().mReflection.mEntryPoint.c_str();
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = program.fragment().mModule;
    stages[1].pName = program.fragment().mReflection.mEntryPoint.c_str();

    // Vertex input: binding b for the b-th set bit of the attribute mask (WolfVkVertexBuffer::bind), stride sTypeSize,
    // the attribute's location from the shader. Source: vulkan_core.h:4274 VkVertexInputBindingDescription { binding, stride, inputRate };
    // :4267 VkVertexInputAttributeDescription { location, binding, format, offset }
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
    uint32_t b = 0;
    for (uint32_t t = 0; t < WolfVkVertexBuffer::TYPE_MAX; ++t)
    {
        if (!(state.mAttributeMask & (1u << t))) continue;
        const uint32_t binding = b++;
        // the colour binding points at the emissive array when both are bound (setupVertexBuffer :1810-1814): same stride (4)
        bindings.push_back({ binding, WolfVkVertexBuffer::sTypeSize[t], VK_VERTEX_INPUT_RATE_VERTEX });   // :2658
        const int32_t loc = program.inputLocation(WolfVkVertexBuffer::sAttribName[t]);
        if (loc < 0) continue;   // the array is bound but this shader does not read it
        const WolfVkVertexBuffer::AttribFormat f = WolfVkVertexBuffer::attribFormat(static_cast<WolfVkVertexBuffer::AttributeType>(t));
        attributes.push_back({ static_cast<uint32_t>(loc), binding, f.mFormat, 0 });
    }
    // every shader input must be fed, or the pipeline is invalid
    for (const WolfVkSpirv::Input& in : program.vertex().mReflection.mInputs)
    {
        bool fed = false;
        for (const VkVertexInputAttributeDescription& a : attributes) if (a.location == in.mLocation) fed = true;
        if (!fed)
        {
            error = "vertex input '" + in.mName + "' is not in the bound attribute mask 0x" + llformat("%x", state.mAttributeMask);
            return VK_NULL_HANDLE;
        }
    }
    // Source: vulkan_core.h:4377 VkPipelineVertexInputStateCreateInfo { sType, pNext, flags, vertexBindingDescriptionCount, pVertexBindingDescriptions,
    // vertexAttributeDescriptionCount, pVertexAttributeDescriptions }
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;   // :226
    vertex_input.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size());
    vertex_input.pVertexBindingDescriptions = bindings.empty() ? nullptr : bindings.data();
    vertex_input.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
    vertex_input.pVertexAttributeDescriptions = attributes.empty() ? nullptr : attributes.data();

    // Source: vulkan_core.h:4334 VkPipelineInputAssemblyStateCreateInfo { sType, pNext, flags, topology, primitiveRestartEnable };
    // topology per wolfvkpipeline.h (LLRender::eGeomModes llrender.h:315-324 in order)
    static const VkPrimitiveTopology topologies[7] =
    {
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN,
        VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP,
        VK_PRIMITIVE_TOPOLOGY_LINE_STRIP /* LINE_LOOP: no Vulkan equivalent, closing segment missing */,
    };
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;   // :227
    assembly.topology = topologies[state.mTopology < 7 ? state.mTopology : 0];
    assembly.primitiveRestartEnable = VK_FALSE;

    // Source: vulkan_core.h:4387 VkPipelineViewportStateCreateInfo; counts only, the values are dynamic
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;   // :229
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    // [GL DEPTH 2026-11-08] GL's clip-space z range [-w, w] taken as is, so the stored depth comes from the viewport
    // transform as GL's is (z_w = ((f - n)/2) z_d + (n + f)/2, OpenGL 4.6 13.8.1) instead of from a matrix
    // pre-multiplied by z' = 0.5 z + 0.5 w (wolfvkshader.cpp applyProjectionFix). The pre-multiplied path rounded
    // differently: the same surface stored a depth 1 ulp off GL's in ~80% of texels (G-buffer dumps nv-hang2/nv-hang3,
    // docs 8.2n), so GL passes drawn after the Vulkan G-buffer pass against that depth z-fought with it (Paul's blue
    // dashes). Source: vulkan_core.h:21215 VkPipelineViewportDepthClipControlCreateInfoEXT { sType, pNext,
    // negativeOneToOne }; :1000 sType.
    VkPipelineViewportDepthClipControlCreateInfoEXT depth_clip{};
    depth_clip.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT;
    depth_clip.negativeOneToOne = VK_TRUE;
    if (mDevice->glDepthRange()) viewport.pNext = &depth_clip;

    // Source: vulkan_core.h:4354 VkPipelineRasterizationStateCreateInfo { sType, pNext, flags, depthClampEnable, rasterizerDiscardEnable, polygonMode,
    // cullMode, frontFace, depthBiasEnable, depthBiasConstantFactor, depthBiasClamp, depthBiasSlopeFactor, lineWidth }
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;   // :230
    raster.depthClampEnable = VK_FALSE;
    raster.rasterizerDiscardEnable = VK_FALSE;
    raster.polygonMode = state.mWireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;   // :2680, :2679
    raster.cullMode = state.mCullFace ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;          // :3326, :3324 (glCullFace(GL_BACK) llrender.cpp:880)
    // GL's default front face is GL_CCW (gl.h); the targets are written in GL's row order with a positive-height viewport
    // (wolfvkrendertarget.cpp begin), which mirrors y against Vulkan's y-down window space, so GL's CCW is CW here. :2621
    raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
    raster.depthBiasEnable = (state.mDepthBiasConstant != 0.f || state.mDepthBiasSlope != 0.f) ? VK_TRUE : VK_FALSE;
    raster.depthBiasConstantFactor = state.mDepthBiasConstant;
    raster.depthBiasSlopeFactor = state.mDepthBiasSlope;
    raster.depthBiasClamp = 0.f;
    raster.lineWidth = 1.0f;

    // Source: vulkan_core.h:4342 VkPipelineMultisampleStateCreateInfo
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;   // :231
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Source: vulkan_core.h:4311 VkPipelineDepthStencilStateCreateInfo { sType, pNext, flags, depthTestEnable, depthWriteEnable, depthCompareOp,
    // depthBoundsTestEnable, stencilTestEnable, front, back, minDepthBounds, maxDepthBounds }
    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;   // :232
    depth.depthTestEnable = state.mDepthTest ? VK_TRUE : VK_FALSE;
    // LLGLDepthTest (llgl.cpp:2829-2834) forces writes off when the test is off
    depth.depthWriteEnable = (state.mDepthTest && state.mDepthWrite) ? VK_TRUE : VK_FALSE;
    // GL_NEVER..GL_ALWAYS = 0x200..0x207 (gl.h:259-266) -> VK_COMPARE_OP_NEVER..ALWAYS = 0..7 (vulkan_core.h:2392-2399), same order
    if (state.mDepthFunc < 0x200 || state.mDepthFunc > 0x207)
    {
        error = llformat("depth function 0x%x is not a GL compare function", state.mDepthFunc);
        return VK_NULL_HANDLE;
    }
    depth.depthCompareOp = static_cast<VkCompareOp>(state.mDepthFunc - 0x200);
    depth.depthBoundsTestEnable = VK_FALSE;
    depth.stencilTestEnable = VK_FALSE;

    // Source: vulkan_core.h:4289 VkPipelineColorBlendAttachmentState { blendEnable, srcColorBlendFactor, dstColorBlendFactor, colorBlendOp,
    // srcAlphaBlendFactor, dstAlphaBlendFactor, alphaBlendOp, colorWriteMask }; the factor table is in the header
    static const VkBlendFactor factors[10] =
    {
        VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_SRC_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_DST_ALPHA,
        VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
    };
    auto factor = [&](uint8_t f) -> VkBlendFactor { return f < 10 ? factors[f] : VK_BLEND_FACTOR_ZERO; };
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.blendEnable = state.mBlendEnable ? VK_TRUE : VK_FALSE;
    attachment.srcColorBlendFactor = factor(state.mBlendSrcColor);
    attachment.dstColorBlendFactor = factor(state.mBlendDstColor);
    attachment.colorBlendOp = VK_BLEND_OP_ADD;
    attachment.srcAlphaBlendFactor = factor(state.mBlendSrcAlpha);
    attachment.dstAlphaBlendFactor = factor(state.mBlendDstAlpha);
    attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    attachment.colorWriteMask = (state.mColorMask[0] ? VK_COLOR_COMPONENT_R_BIT : 0u) | (state.mColorMask[1] ? VK_COLOR_COMPONENT_G_BIT : 0u)
                              | (state.mColorMask[2] ? VK_COLOR_COMPONENT_B_BIT : 0u) | (state.mColorMask[3] ? VK_COLOR_COMPONENT_A_BIT : 0u);   // :3315-3318
    std::vector<VkPipelineColorBlendAttachmentState> attachments(std::max<uint32_t>(1, state.mColorAttachments), attachment);
    if (state.mCoverageAttachment && attachments.size() >= 2)
    {
        VkPipelineColorBlendAttachmentState& cov = attachments.back();
        cov.blendEnable = VK_FALSE;
        cov.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    }
    // Source: vulkan_core.h:4300 VkPipelineColorBlendStateCreateInfo { sType, pNext, flags, logicOpEnable, logicOp, attachmentCount, pAttachments, blendConstants }
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;   // :233
    blend.attachmentCount = static_cast<uint32_t>(attachments.size());
    blend.pAttachments = attachments.data();

    // Source: vulkan_core.h:4326 VkPipelineDynamicStateCreateInfo; :2526-2527 VIEWPORT, SCISSOR
    const VkDynamicState dynamics[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;   // :234
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamics;

    // Source: vulkan_core.h:4397 VkGraphicsPipelineCreateInfo { sType, pNext, flags, stageCount, pStages, pVertexInputState, pInputAssemblyState,
    // pTessellationState, pViewportState, pRasterizationState, pMultisampleState, pDepthStencilState, pColorBlendState, pDynamicState, layout,
    // renderPass, subpass, basePipelineHandle, basePipelineIndex }
    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;   // :235
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = program.pipelineLayout();
    info.renderPass = render_pass;
    info.subpass = 0;
    info.basePipelineIndex = -1;
    VkPipeline pipeline = VK_NULL_HANDLE;
    // Source: vulkan_core.h:4639 PFN_vkCreateGraphicsPipelines(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines)
    VkResult res = gVk.vkCreateGraphicsPipelines(mDevice->device(), mCache, 1, &info, nullptr, &pipeline);
    if (res != VK_SUCCESS)
    {
        error = std::string("vkCreateGraphicsPipelines: ") + WolfVkLoader::resultString(res);
        return VK_NULL_HANDLE;
    }
    return pipeline;
}
