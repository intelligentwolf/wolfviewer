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
#include <algorithm>   // [V5] std::find / std::remove
#include <functional>
#include "llfile.h"   // [W114-VULKAN S7 2026-11-26] the pipeline cache file
#include "llapp.h"    // LLApp::getPid
#include <cerrno>
#include <cstdio>

// ---------------------------------------------------------------------------------------------------------------
// WolfVkProgram

WolfVkProgram::~WolfVkProgram()
{
    destroy();
}

namespace
{
    // Promote single WolfUniformsV std140 UBO block (set 0, binding 0) into a hardware Vulkan PushConstant block when
    // WolfUniformsV is the sole ScUniform variable in the vertex SPIR-V module and fits within maxPushConstantsSize.
    bool tryPromoteVertexUniformsToPushConstants(const std::vector<uint32_t>& in_words, uint32_t max_pc_bytes, std::vector<uint32_t>& out_words)
    {
        if (in_words.size() < 6 || max_pc_bytes == 0) return false;
        WolfVkSpirv::Module refl;
        std::string err;
        if (!WolfVkSpirv::reflect(in_words.data(), in_words.size(), refl, err)) return false;
        if (refl.mExecutionModel != 0 || refl.mBlocks.size() != 1) return false;
        const WolfVkSpirv::UniformBlock& blk = refl.mBlocks[0];
        if (blk.mName != "WolfUniformsV" || blk.mPushConstant || blk.mSet != 0 || blk.mBinding != 0) return false;
        if (blk.mSize == 0 || blk.mSize > max_pc_bytes) return false;

        constexpr uint32_t OpTypePointer = 32;
        constexpr uint32_t OpVariable = 59;
        constexpr uint32_t OpDecorate = 71;
        constexpr uint32_t DecBinding = 33;
        constexpr uint32_t DecDescriptorSet = 34;
        constexpr uint32_t ScUniform = 2;
        constexpr uint32_t ScPushConstant = 9;
        uint32_t uniform_var_id = 0;
        size_t i = 5;
        while (i < in_words.size())
        {
            const uint32_t count = in_words[i] >> 16;
            const uint32_t op = in_words[i] & 0xFFFFu;
            if (count == 0 || i + count > in_words.size()) return false;
            if (op == OpTypePointer && count >= 4 && in_words[i + 2] == ScPushConstant)
            {
                return false; // already has push-constant pointer types
            }
            if (op == OpVariable && count >= 4)
            {
                const uint32_t sc = in_words[i + 3];
                if (sc == ScPushConstant) return false;
                if (sc == ScUniform)
                {
                    if (uniform_var_id != 0) return false; // more than one ScUniform variable
                    uniform_var_id = in_words[i + 2];
                }
            }
            i += count;
        }
        if (uniform_var_id == 0) return false;

        out_words.clear();
        out_words.reserve(in_words.size());
        out_words.insert(out_words.end(), in_words.begin(), in_words.begin() + 5);
        i = 5;
        while (i < in_words.size())
        {
            const uint32_t count = in_words[i] >> 16;
            const uint32_t op = in_words[i] & 0xFFFFu;
            if (op == OpDecorate && count >= 4 && in_words[i + 1] == uniform_var_id &&
                (in_words[i + 2] == DecDescriptorSet || in_words[i + 2] == DecBinding))
            {
                i += count;
                continue; // strip DescriptorSet/Binding decorations cleanly (OpNop is illegal outside basic blocks)
            }
            const size_t base = out_words.size();
            out_words.insert(out_words.end(), in_words.begin() + i, in_words.begin() + i + count);
            if (op == OpTypePointer && count >= 4 && out_words[base + 2] == ScUniform)
            {
                out_words[base + 2] = ScPushConstant;
            }
            else if (op == OpVariable && count >= 4 && out_words[base + 2] == uniform_var_id && out_words[base + 3] == ScUniform)
            {
                out_words[base + 3] = ScPushConstant;
            }
            i += count;
        }
        return true;
    }
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
    stage.mPushConstants = nullptr;
    for (const WolfVkSpirv::UniformBlock& b : stage.mReflection.mBlocks)
    {
        if (b.mName == block_name)
        {
            if (b.mPushConstant)
            {
                stage.mPushConstants = &b;
            }
            else if (b.mSet == SET_UNIFORMS)
            {
                stage.mUniforms = &b;
            }
        }
    }
    return true;
}

bool WolfVkProgram::create(WolfVkDevice& device, const std::string& name, const std::vector<uint32_t>& vert, const std::vector<uint32_t>& geom,
                           const std::vector<uint32_t>& frag, std::string& error)
{
    destroy();
    mDevice = &device;
    mName = name;
    if (!geom.empty() && !device.geometryShaders())
    {
        // VUID-VkPipelineShaderStageCreateInfo-stage-00704: a geometry stage needs the geometryShader feature
        error = name + ": the program has a geometry shader and this device does not offer the geometryShader feature";
        destroy();
        return false;
    }
    std::vector<uint32_t> promoted_vert;
    const bool use_promoted_vert = geom.empty() &&
        tryPromoteVertexUniformsToPushConstants(vert, device.limits().maxPushConstantsSize, promoted_vert);
    const std::vector<uint32_t>& eff_vert = use_promoted_vert ? promoted_vert : vert;
    if (!buildStage(device, mVertex, eff_vert, "WolfUniformsV", error) || !buildStage(device, mFragment, frag, "WolfUniformsF", error)
        || (!geom.empty() && !buildStage(device, mGeometry, geom, "", error)))
    {
        error = name + ": " + error;
        destroy();
        return false;
    }
    // Source: spirv.hpp:66 ExecutionModelVertex = 0, :69 ExecutionModelGeometry = 3, :70 ExecutionModelFragment = 4
    if (mVertex.mReflection.mExecutionModel != 0 || mFragment.mReflection.mExecutionModel != 4
        || (hasGeometry() && mGeometry.mReflection.mExecutionModel != 3))
    {
        error = name + ": the SPIR-V stages are not a vertex, " + (hasGeometry() ? "a geometry " : "") + "and a fragment shader";
        destroy();
        return false;
    }
    // [GEOMETRY 2026-11-26] the transform gives the geometry stage no uniform block (scripts/shader_vulkan.py STAGES): set 0
    // has bindings for the vertex and fragment blocks only, so a geometry stage that reads one cannot be bound
    for (const WolfVkSpirv::UniformBlock& b : mGeometry.mReflection.mBlocks)
    {
        if (b.mSet == SET_UNIFORMS)
        {
            error = name + ": the geometry stage reads uniform block " + b.mName + " in set " + std::to_string(SET_UNIFORMS) + ", which has no binding for it";
            destroy();
            return false;
        }
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
    // bindings per program, so it does; checked anyway).
    // [W114-VULKAN S7 2026-11-26] Mac gap (a). mSamplers keeps every DECLARED sampler (names and bindings: the uniform
    // setters and samplerBinding() need them), with mUsed = some stage statically uses it. mSamplerSlots = the set-1
    // layout: only the statically used ones, each visible to exactly the stages that use it, binding numbers unchanged.
    // Source: Vulkan spec 1.4.363 chapters/interfaces.adoc:1675-1691 ("A pipeline layout may include descriptor sets and
    // bindings which are not referenced by any variables statically used ... However, if a variable assigned to a given
    // DescriptorSet and Binding is statically used by the entry point for a shader stage, the pipeline layout must
    // contain a descriptor set layout binding in that descriptor set layout and for that binding number, and that
    // binding's stageFlags must include the appropriate VkShaderStageFlagBits for that stage") and
    // chapters/descriptorsets.adoc:1945-1949 (pipeline layout consistency: "All resource variables statically used in
    // all shaders in a pipeline must be declared with a (set, binding, arrayElement) that exists in the corresponding
    // descriptor set layout ... and includes the set of shader stages it is used by in stageFlags"). Static use:
    // chapters/shaders.adoc:3166-3178; WolfVkSpirv marks a variable used when ANY instruction of the module names it or
    // the entry point's interface lists it, which is that set or a superset of it (never fewer).
    // NOTE the valid-usage line VUID-VkGraphicsPipelineCreateInfo-layout-07988 (validusage.json 1.4.363) says "declared"
    // where the chapters say "statically used"; the chapter text above is what this relies on.
    for (const Stage* s : { &mVertex, &mGeometry, &mFragment })
    {
        // Source: vulkan_core.h:2883 VK_SHADER_STAGE_VERTEX_BIT, :2886 GEOMETRY_BIT, :2887 FRAGMENT_BIT
        const VkShaderStageFlags flag = (s == &mVertex) ? VK_SHADER_STAGE_VERTEX_BIT
                                      : (s == &mGeometry) ? VK_SHADER_STAGE_GEOMETRY_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
        for (const WolfVkSpirv::Sampler& smp : s->mReflection.mSamplers)
        {
            if (smp.mSet != SET_SAMPLERS)
            {
                error = name + ": sampler " + smp.mName + " is in descriptor set " + std::to_string(smp.mSet) + ", expected " + std::to_string(SET_SAMPLERS);
                destroy();
                return false;
            }
            size_t at = mSamplers.size();
            auto it = mSamplerBindings.find(smp.mName);
            if (it != mSamplerBindings.end())
            {
                if (it->second != smp.mBinding)
                {
                    error = name + ": sampler " + smp.mName + " has two bindings";
                    destroy();
                    return false;
                }
                for (size_t k = 0; k < mSamplers.size(); ++k)
                {
                    if (mSamplers[k].mName == smp.mName) at = k;
                }
            }
            if (at == mSamplers.size())
            {
                for (const WolfVkSpirv::Sampler& other : mSamplers)
                {
                    // two names on one binding cannot share one descriptor here (bindSamplers resolves one texture per binding)
                    if (other.mBinding == smp.mBinding)
                    {
                        error = name + ": samplers " + other.mName + " and " + smp.mName + " share binding " + std::to_string(smp.mBinding);
                        destroy();
                        return false;
                    }
                }
                mSamplerBindings[smp.mName] = smp.mBinding;
                mSamplers.push_back(smp);
                mSamplerStages.push_back(0u);
            }
            else
            {
                // the same sampler in a second stage: one descriptor serves both, so the declarations must agree
                WolfVkSpirv::Sampler& have = mSamplers[at];
                if (have.mDim != smp.mDim || have.mArrayed != smp.mArrayed || have.mShadow != smp.mShadow
                    || have.mMultisampled != smp.mMultisampled || have.mArraySize != smp.mArraySize)
                {
                    error = name + ": sampler " + smp.mName + " is declared with two different types";
                    destroy();
                    return false;
                }
                have.mUsed = have.mUsed || smp.mUsed;
            }
            if (smp.mUsed) mSamplerStages[at] |= flag;
        }
    }
    for (size_t k = 0; k < mSamplers.size(); ++k)
    {
        if (mSamplerStages[k] != 0u) mSamplerSlots.push_back({ mSamplers[k].mBinding, mSamplers[k].mArraySize, mSamplerStages[k] });
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
    // Set 1: the statically used samplers, each visible to the stages that use it (mSamplerSlots above). An empty layout
    // (bindingCount 0) is valid and keeps the pipeline layout's set numbering.
    std::vector<VkDescriptorSetLayoutBinding> sampler_bindings;
    for (const SamplerSlot& slot : mSamplerSlots)
    {
        sampler_bindings.push_back({ slot.mBinding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, slot.mCount, slot.mStages, nullptr });   // :2411
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
    // [VULKAN G-BUFFER S2 2026-11-26] Set 2: the viewer's own std140 blocks (wolfvkpipeline.h SET_BLOCKS), one dynamic
    // uniform buffer each (the bytes go through the frame ring like set 0's), visible to the stages that declare them.
    // Before this a program with such a block got a pipeline layout without set 2 (VUID-VkGraphicsPipelineCreateInfo-layout-07988).
    std::vector<VkDescriptorSetLayoutBinding> block_bindings;
    // [W114-VULKAN merge 2026-11-26] the geometry stage (spv) too: a block it declares must be in the layout with its stage
    // flag. Source: vulkan_core.h:2886 VK_SHADER_STAGE_GEOMETRY_BIT. A program without a geometry stage has an empty mGeometry.
    for (const Stage* st : { &mVertex, &mGeometry, &mFragment })
    {
        const VkShaderStageFlags flag = (st == &mVertex) ? VK_SHADER_STAGE_VERTEX_BIT
                                      : (st == &mGeometry) ? VK_SHADER_STAGE_GEOMETRY_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
        for (const WolfVkSpirv::UniformBlock& b : st->mReflection.mBlocks)
        {
            if (b.mPushConstant || b.mSet == SET_UNIFORMS) continue;
            if (b.mSet != SET_BLOCKS)
            {
                error = name + ": uniform block " + b.mName + " is in descriptor set " + std::to_string(b.mSet) + ", expected " + std::to_string(SET_BLOCKS);
                destroy();
                return false;
            }
            bool merged = false;
            for (size_t k = 0; k < mBlocks.size(); ++k)
            {
                if (mBlocks[k].mBinding != b.mBinding) continue;
                if (mBlocks[k].mName != b.mName)
                {
                    error = name + ": blocks " + mBlocks[k].mName + " and " + b.mName + " share binding " + std::to_string(b.mBinding);
                    destroy();
                    return false;
                }
                mBlocks[k].mSize = std::max(mBlocks[k].mSize, b.mSize);
                mBlocks[k].mUsed = mBlocks[k].mUsed || b.mUsed;
                block_bindings[k].stageFlags |= flag;
                merged = true;
            }
            if (merged) continue;
            BlockBinding bb;
            bb.mName = b.mName;
            bb.mBinding = b.mBinding;
            bb.mSize = b.mSize;
            bb.mUsed = b.mUsed;
            mBlocks.push_back(bb);
            // Source: vulkan_core.h:4207 VkDescriptorSetLayoutBinding { binding, descriptorType, descriptorCount, stageFlags, pImmutableSamplers }
            block_bindings.push_back({ b.mBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, flag, nullptr });
        }
    }
    if (!block_bindings.empty())
    {
        VkDescriptorSetLayoutCreateInfo set2{};
        set2.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        set2.bindingCount = static_cast<uint32_t>(block_bindings.size());
        set2.pBindings = block_bindings.data();
        res = gVk.vkCreateDescriptorSetLayout(dev, &set2, nullptr, &mBlockSetLayout);
        if (res != VK_SUCCESS)
        {
            error = name + ": vkCreateDescriptorSetLayout (blocks): " + WolfVkLoader::resultString(res);
            destroy();
            return false;
        }
    }
    // [W114-VULKAN S7 2026-11-26] The device's limits on what one pipeline layout may hold, checked before the layout is
    // made: a program over a limit is refused here in plain words and OpenGL keeps drawing it (the only programs known to
    // need more than 16 used samplers are Water variants: MoltenVK's 16 is the lowest limit met so far). Counted exactly
    // as the layout is built: uniform_bindings (set 0), sampler_bindings (set 1), block_bindings (set 2).
    // Source: vulkan_core.h:3599 maxPerStageDescriptorSamplers, :3600 maxPerStageDescriptorUniformBuffers, :3602
    // maxPerStageDescriptorSampledImages, :3605 maxPerStageResources, :3606 maxDescriptorSetSamplers, :3608
    // maxDescriptorSetUniformBuffersDynamic, :3611 maxDescriptorSetSampledImages (VkPhysicalDeviceLimits). Rules
    // (validusage.json 1.4.363): VUID-VkPipelineLayoutCreateInfo-descriptorType-03016 (samplers per stage; a
    // COMBINED_IMAGE_SAMPLER counts), -06939 (sampled images per stage; it counts again), -03017 (uniform buffers per
    // stage), -03028 (samplers, all stages), -03033 (sampled images, all stages), -03030 (dynamic uniform buffers, all
    // stages); "accessible" to a stage = the binding's stageFlags has its bit (chapters/limits.adoc:151-165).
    {
        const VkPhysicalDeviceLimits& lim = device.limits();
        const VkShaderStageFlags stage_bits[3] = { VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_GEOMETRY_BIT, VK_SHADER_STAGE_FRAGMENT_BIT };
        const char* stage_names[3] = { "vertex", "geometry", "fragment" };
        uint32_t samplers_all = 0, dynamic_all = 0;
        for (const VkDescriptorSetLayoutBinding& lb : sampler_bindings) samplers_all += lb.descriptorCount;
        for (const VkDescriptorSetLayoutBinding& lb : uniform_bindings) dynamic_all += lb.descriptorCount;
        for (const VkDescriptorSetLayoutBinding& lb : block_bindings) dynamic_all += lb.descriptorCount;
        std::string over;
        for (int st = 0; st < 3 && over.empty(); ++st)
        {
            uint32_t samplers_stage = 0, buffers_stage = 0;
            for (const VkDescriptorSetLayoutBinding& lb : sampler_bindings) if (lb.stageFlags & stage_bits[st]) samplers_stage += lb.descriptorCount;
            for (const VkDescriptorSetLayoutBinding& lb : uniform_bindings) if (lb.stageFlags & stage_bits[st]) buffers_stage += lb.descriptorCount;
            for (const VkDescriptorSetLayoutBinding& lb : block_bindings) if (lb.stageFlags & stage_bits[st]) buffers_stage += lb.descriptorCount;
            mStageResources[st] = samplers_stage + buffers_stage;
            if (samplers_stage > lim.maxPerStageDescriptorSamplers)
            {
                over = std::string("its ") + stage_names[st] + " stage uses " + std::to_string(samplers_stage) + " samplers and this device allows "
                     + std::to_string(lim.maxPerStageDescriptorSamplers) + " per stage (maxPerStageDescriptorSamplers)";
            }
            else if (samplers_stage > lim.maxPerStageDescriptorSampledImages)
            {
                over = std::string("its ") + stage_names[st] + " stage uses " + std::to_string(samplers_stage) + " sampled images and this device allows "
                     + std::to_string(lim.maxPerStageDescriptorSampledImages) + " per stage (maxPerStageDescriptorSampledImages)";
            }
            else if (buffers_stage > lim.maxPerStageDescriptorUniformBuffers)
            {
                over = std::string("its ") + stage_names[st] + " stage uses " + std::to_string(buffers_stage) + " uniform buffers and this device allows "
                     + std::to_string(lim.maxPerStageDescriptorUniformBuffers) + " per stage (maxPerStageDescriptorUniformBuffers)";
            }
        }
        if (over.empty() && samplers_all > lim.maxDescriptorSetSamplers)
        {
            over = "it uses " + std::to_string(samplers_all) + " samplers and this device allows " + std::to_string(lim.maxDescriptorSetSamplers)
                 + " in a pipeline layout (maxDescriptorSetSamplers)";
        }
        if (over.empty() && samplers_all > lim.maxDescriptorSetSampledImages)
        {
            over = "it uses " + std::to_string(samplers_all) + " sampled images and this device allows " + std::to_string(lim.maxDescriptorSetSampledImages)
                 + " in a pipeline layout (maxDescriptorSetSampledImages)";
        }
        if (over.empty() && dynamic_all > lim.maxDescriptorSetUniformBuffersDynamic)
        {
            over = "it uses " + std::to_string(dynamic_all) + " dynamic uniform buffers and this device allows "
                 + std::to_string(lim.maxDescriptorSetUniformBuffersDynamic) + " in a pipeline layout (maxDescriptorSetUniformBuffersDynamic)";
        }
        if (!over.empty())
        {
            error = name + ": " + over + "; Vulkan cannot draw this program on this device";
            destroy();
            return false;
        }
    }
    // Source: vulkan_core.h:4130 VkPipelineLayoutCreateInfo { sType, pNext, flags, setLayoutCount, pSetLayouts, pushConstantRangeCount, pPushConstantRanges }
    VkDescriptorSetLayout layouts[3] = { mUniformSetLayout, mSamplerSetLayout, mBlockSetLayout };
    VkPushConstantRange pc_range{};
    VkPipelineLayoutCreateInfo pl{};
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;   // :237
    pl.setLayoutCount = mBlockSetLayout != VK_NULL_HANDLE ? 3u : 2u;
    pl.pSetLayouts = layouts;
    if (mVertex.mPushConstants)
    {
        pc_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pc_range.offset = 0;
        pc_range.size = mVertex.mPushConstants->mSize;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &pc_range;
    }
    res = gVk.vkCreatePipelineLayout(dev, &pl, nullptr, &mPipelineLayout);
    if (res != VK_SUCCESS)
    {
        error = name + ": vkCreatePipelineLayout: " + WolfVkLoader::resultString(res);
        destroy();
        return false;
    }
    const WolfVkSpirv::UniformBlock* vu = mVertex.mPushConstants ? mVertex.mPushConstants : mVertex.mUniforms;
    LL_INFOS("Vulkan") << "Program " << name << ": vertex " << (mVertex.mPushConstants ? "push-constants " : "uniforms ")
                       << (vu ? vu->mSize : 0u) << " bytes (" << (vu ? vu->mMembers.size() : 0u)
                       << " members), fragment uniforms " << (mFragment.mUniforms ? mFragment.mUniforms->mSize : 0u) << " bytes ("
                       << (mFragment.mUniforms ? mFragment.mUniforms->mMembers.size() : 0u)
                       << " members), " << mSamplers.size() << " samplers declared (" << mSamplerSlots.size() << " used, in the layout), "
                       << mInputLocations.size() << " vertex inputs, attribute mask 0x" << std::hex << mAttributeMask << std::dec
                       << (hasGeometry() ? ", geometry stage" : "") << LL_ENDL;
    return true;
}

void WolfVkProgram::destroy()
{
    WolfVkPipelineCache::forgetProgram(this);   // [V5 2026-11-11] its pipelines go with it (deferred)
    if (mDevice && mDevice->hasDevice())
    {
        WolfVkDevice* device = mDevice;
        VkShaderModule v = mVertex.mModule, f = mFragment.mModule, g = mGeometry.mModule;
        VkDescriptorSetLayout s0 = mUniformSetLayout, s1 = mSamplerSetLayout, s2 = mBlockSetLayout;
        VkPipelineLayout pl = mPipelineLayout;
        device->deferDestroy([device, v, f, g, s0, s1, s2, pl]()
        {
            if (g) gVk.vkDestroyShaderModule(device->device(), g, nullptr);
            if (pl) gVk.vkDestroyPipelineLayout(device->device(), pl, nullptr);
            if (s0) gVk.vkDestroyDescriptorSetLayout(device->device(), s0, nullptr);
            if (s1) gVk.vkDestroyDescriptorSetLayout(device->device(), s1, nullptr);
            if (s2) gVk.vkDestroyDescriptorSetLayout(device->device(), s2, nullptr);   // [S2]
            if (v) gVk.vkDestroyShaderModule(device->device(), v, nullptr);
            if (f) gVk.vkDestroyShaderModule(device->device(), f, nullptr);
        });
    }
    mVertex = Stage();
    mFragment = Stage();
    mGeometry = Stage();
    mSpecEntries.clear();
    mSpecData.clear();
    mSpecInfo = VkSpecializationInfo{};
    mSamplers.clear();
    mSamplerStages.clear();    // [S7]
    mSamplerSlots.clear();     // [S7]
    mStageResources[0] = mStageResources[1] = mStageResources[2] = 0;
    mSamplerBindings.clear();
    mInputLocations.clear();
    mAttributeMask = 0;
    mUniformSetLayout = mSamplerSetLayout = mBlockSetLayout = VK_NULL_HANDLE;
    mBlocks.clear();   // [S2]
    mPipelineLayout = VK_NULL_HANDLE;
    mDevice = nullptr;
}

void WolfVkProgram::setSpecConstant(uint32_t id, float value)
{
    for (size_t i = 0; i < mSpecEntries.size(); ++i)
    {
        if (mSpecEntries[i].constantID == id)
        {
            mSpecData[i] = value;
            return;
        }
    }
    // Source: vulkan_core.h:4091 VkSpecializationMapEntry { constantID, offset, size }; :4097 VkSpecializationInfo
    // { mapEntryCount, pMapEntries, dataSize, pData }
    mSpecEntries.push_back({ id, static_cast<uint32_t>(mSpecData.size() * sizeof(float)), sizeof(float) });
    mSpecData.push_back(value);
    mSpecInfo.mapEntryCount = static_cast<uint32_t>(mSpecEntries.size());
    mSpecInfo.pMapEntries = mSpecEntries.data();
    mSpecInfo.dataSize = mSpecData.size() * sizeof(float);
    mSpecInfo.pData = mSpecData.data();
}

bool WolfVkProgram::hasSpecConstant(uint32_t id) const
{
    for (const VkSpecializationMapEntry& e : mSpecEntries)
    {
        if (e.constantID == id) return true;
    }
    return false;
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
        const WolfVkSpirv::UniformBlock* blk = s->mUniforms ? s->mUniforms : s->mPushConstants;
        if (!blk) continue;
        for (const WolfVkSpirv::BlockMember& m : blk->mMembers)
        {
            if (m.mName == name) return { s, &m };
        }
    }
    return { nullptr, nullptr };
}

bool WolfVkProgram::samplerInLayout(uint32_t binding) const
{
    for (const SamplerSlot& slot : mSamplerSlots)
    {
        if (binding >= slot.mBinding && binding - slot.mBinding < slot.mCount) return true;
    }
    return false;
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
    mix((mCullFace && mCullFront) ? 1u : 0u);   // [W114-VULKAN S4 2026-11-26/]
    mix(mDepthClamp ? 1u : 0u);   // [W114-VULKAN S5 2026-11-26/]
    return h;
}

bool WolfVkPipelineState::operator==(const WolfVkPipelineState& o) const
{
    return mBlendSrcColor == o.mBlendSrcColor && mBlendDstColor == o.mBlendDstColor && mBlendSrcAlpha == o.mBlendSrcAlpha
        && mBlendDstAlpha == o.mBlendDstAlpha && mBlendEnable == o.mBlendEnable
        && mColorMask[0] == o.mColorMask[0] && mColorMask[1] == o.mColorMask[1] && mColorMask[2] == o.mColorMask[2] && mColorMask[3] == o.mColorMask[3]
        && mDepthTest == o.mDepthTest && mDepthWrite == o.mDepthWrite && mDepthFunc == o.mDepthFunc && mCullFace == o.mCullFace
        && (mCullFace && mCullFront) == (o.mCullFace && o.mCullFront)   // [W114-VULKAN S4 2026-11-26/]
        && mWireframe == o.mWireframe && mTopology == o.mTopology && mAttributeMask == o.mAttributeMask && mColorAttachments == o.mColorAttachments
        && mCoverageAttachment == o.mCoverageAttachment && mDepthBiasConstant == o.mDepthBiasConstant && mDepthBiasSlope == o.mDepthBiasSlope
        && mDepthClamp == o.mDepthClamp;   // [W114-VULKAN S5 2026-11-26/]
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

namespace
{
    // [V5 2026-11-11] every initialised cache, so a render pass or program going away can take its pipelines with it
    // (forgetRenderPass / forgetProgram). Main thread only, as every Vulkan object here.
    // never destroyed (a pointer to a heap vector): ~WolfVkProgram of the viewer's global shader objects can run during
    // static destruction, after a plain static vector would already be gone [B2 2026-11-11]
    std::vector<WolfVkPipelineCache*>& liveCaches()
    {
        static std::vector<WolfVkPipelineCache*>* caches = new std::vector<WolfVkPipelineCache*>();
        return *caches;
    }
}

void WolfVkPipelineCache::forgetRenderPass(VkRenderPass pass)
{
    if (pass == VK_NULL_HANDLE) return;
    for (WolfVkPipelineCache* cache : liveCaches()) cache->forget([pass](const Key& k) { return k.mPass == pass; });
}

void WolfVkPipelineCache::forgetProgram(const WolfVkProgram* program)
{
    if (!program) return;
    for (WolfVkPipelineCache* cache : liveCaches()) cache->forget([program](const Key& k) { return k.mProgram == program; });
}

void WolfVkPipelineCache::forget(const std::function<bool(const Key&)>& match)
{
    // [V5 2026-11-11] The cache is keyed by the program's address and the render pass handle. Neither lived as long as
    // the cache: every window resize remade the composite pass and the render targets' passes (and a remade mirror its
    // program), so the old pipelines leaked, and an address or handle the driver handed out again found a pipeline made
    // for an object that no longer existed. The entries go when their pass or program goes (called before that object's
    // own deferred destruction is filed); the pipelines themselves are destroyed deferred (WolfVkDevice::deferDestroy, as
    // every object here), since a frame in flight may still have them bound. Failed entries are forgotten too, so a new
    // object at the same address gets its own attempt.
    std::vector<VkPipeline> gone;
    for (auto it = mPipelines.begin(); it != mPipelines.end();)
    {
        if (match(it->first)) { gone.push_back(it->second); it = mPipelines.erase(it); }
        else ++it;
    }
    for (auto it = mFailed.begin(); it != mFailed.end();)
    {
        if (match(it->first)) it = mFailed.erase(it);
        else ++it;
    }
    if (gone.empty() || !mDevice || !mDevice->hasDevice() || mDevice->hung()) return;   // a hung device keeps its objects
    WolfVkDevice* device = mDevice;
    device->deferDestroy([device, gone]()
    {
        for (VkPipeline p : gone) if (p) gVk.vkDestroyPipeline(device->device(), p, nullptr);
    });
}

WolfVkPipelineCache::~WolfVkPipelineCache()
{
    // [B2 2026-11-11] Before, only destroy() unregistered: the hung branch of WolfVkScene::destroy skipped it, the scene was
    // deleted, and a later ~WolfVkProgram (shader reload, quit) walked the freed cache in forgetProgram - a use-after-free.
    liveCaches().erase(std::remove(liveCaches().begin(), liveCaches().end(), this), liveCaches().end());
}

void WolfVkPipelineCache::abandon()
{
    mPipelines.clear();
    mFailed.clear();
    mCache = VK_NULL_HANDLE;
    mDevice = nullptr;
    mDiskFile.clear();      // [W114-VULKAN S7 2026-11-26] a hung device is not asked for its cache data
    mBuiltSinceSave = 0;
    liveCaches().erase(std::remove(liveCaches().begin(), liveCaches().end(), this), liveCaches().end());
}

// ---------------------------------------------------------------------------------------------------------------
// [W114-VULKAN S7 2026-11-26] pipeline cache on disk (Mac gap (i); the same path on Windows and Linux)
//
// File = envelope + payload. The payload is exactly what vkGetPipelineCacheData returned. The envelope is this
// viewer's own (it is not a Vulkan format): 8 bytes magic "WOLFVKPC", 4 bytes envelope version (1), 4 bytes zero,
// 8 bytes payload length, 8 bytes FNV-1a 64 of the payload, all little-endian. It catches a truncated or damaged
// file before the bytes reach a driver.
// The payload's own header is checked against the device as the specification asks of applications
// (VkPipelineCacheHeaderVersionOne, vulkan_core.h:4051-4057: headerSize, headerVersion, vendorID, deviceID,
// pipelineCacheUUID[VK_UUID_SIZE]. Its reference page, docs.vulkan.org refpages VkPipelineCacheHeaderVersionOne, read
// 2026-11-26: VUID-VkPipelineCacheHeaderVersionOne-headerSize-04967 "headerSize must be 32", -headerVersion-04968
// "headerVersion must be VK_PIPELINE_CACHE_HEADER_VERSION_ONE" (= 1, vulkan_core.h:2355), and "all fields of this
// structure are written with the least significant byte first, regardless of host byte-order").
namespace
{
    const uint8_t DISK_MAGIC[8] = { 'W', 'O', 'L', 'F', 'V', 'K', 'P', 'C' };
    const uint32_t DISK_ENVELOPE_VERSION = 1;
    const size_t DISK_ENVELOPE_BYTES = 32;
    const size_t CACHE_HEADER_BYTES = 32;   // 4 + 4 + 4 + 4 + VK_UUID_SIZE (16, vulkan_core.h:133)

    uint32_t readLE32(const uint8_t* p)
    {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    }
    uint64_t readLE64(const uint8_t* p)
    {
        return static_cast<uint64_t>(readLE32(p)) | (static_cast<uint64_t>(readLE32(p + 4)) << 32);
    }
    void writeLE32(uint8_t* p, uint32_t v)
    {
        for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFFu);
    }
    void writeLE64(uint8_t* p, uint64_t v)
    {
        for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFFu);
    }
    // FNV-1a, 64 bits: offset basis 14695981039346656037, prime 1099511628211
    // (Fowler, Noll, Vo: draft-eastlake-fnv, section 2 "FNV Basics" and 2.2 "FNV Offset Bases").
    uint64_t fnv1a64(const uint8_t* data, size_t size)
    {
        uint64_t h = 14695981039346656037ULL;
        for (size_t i = 0; i < size; ++i)
        {
            h ^= data[i];
            h *= 1099511628211ULL;
        }
        return h;
    }
    // true when payload begins with a version-one header that names this physical device and driver build
    bool cacheHeaderMatches(const uint8_t* payload, size_t size, const VkPhysicalDeviceProperties& props)
    {
        if (size < CACHE_HEADER_BYTES) return false;
        if (readLE32(payload) != CACHE_HEADER_BYTES) return false;
        if (readLE32(payload + 4) != static_cast<uint32_t>(VK_PIPELINE_CACHE_HEADER_VERSION_ONE)) return false;
        if (readLE32(payload + 8) != props.vendorID) return false;     // vulkan_core.h:3711-3712 vendorID, deviceID
        if (readLE32(payload + 12) != props.deviceID) return false;
        return memcmp(payload + 16, props.pipelineCacheUUID, VK_UUID_SIZE) == 0;   // :3715
    }
}

bool WolfVkPipelineCache::loadDisk(std::vector<uint8_t>& payload) const
{
    payload.clear();
    llstat st;
    if (LLFile::stat(mDiskFile, &st) != 0) return false;   // no file yet: the first run on this computer
    if (st.st_size < static_cast<decltype(st.st_size)>(DISK_ENVELOPE_BYTES + CACHE_HEADER_BYTES)
        || static_cast<uint64_t>(st.st_size) > static_cast<uint64_t>(DISK_LIMIT_BYTES))
    {
        LL_INFOS("Vulkan") << "Pipeline cache file ignored (size " << static_cast<uint64_t>(st.st_size) << " bytes): " << mDiskFile << LL_ENDL;
        return false;
    }
    const size_t file_size = static_cast<size_t>(st.st_size);
    LLFILE* f = LLFile::fopen(mDiskFile, "rb");
    if (!f) return false;
    std::vector<uint8_t> bytes(file_size);
    const size_t got = fread(bytes.data(), 1, file_size, f);
    fclose(f);
    if (got != file_size)
    {
        LL_INFOS("Vulkan") << "Pipeline cache file ignored (short read): " << mDiskFile << LL_ENDL;
        return false;
    }
    const uint8_t* e = bytes.data();
    const size_t payload_size = file_size - DISK_ENVELOPE_BYTES;
    if (memcmp(e, DISK_MAGIC, sizeof(DISK_MAGIC)) != 0 || readLE32(e + 8) != DISK_ENVELOPE_VERSION
        || readLE64(e + 16) != static_cast<uint64_t>(payload_size)
        || readLE64(e + 24) != fnv1a64(e + DISK_ENVELOPE_BYTES, payload_size))
    {
        LL_INFOS("Vulkan") << "Pipeline cache file ignored (not written whole by this viewer, or damaged): " << mDiskFile << LL_ENDL;
        return false;
    }
    if (!cacheHeaderMatches(e + DISK_ENVELOPE_BYTES, payload_size, mDevice->chosen().mProperties))
    {
        // another card, or a driver update: the driver would refuse the data itself; it is replaced at the next save
        LL_INFOS("Vulkan") << "Pipeline cache file is for another graphics card or driver version; starting an empty one" << LL_ENDL;
        return false;
    }
    payload.assign(e + DISK_ENVELOPE_BYTES, e + file_size);
    return true;
}

void WolfVkPipelineCache::saveDisk()
{
    if (mDiskFile.empty() || mCache == VK_NULL_HANDLE || !mDevice || !mDevice->hasDevice() || mDevice->hung()) return;
    mBuiltSinceSave = 0;   // whatever happens below, the same failure is not retried every frame
    // Source: vulkan_core.h:4614 PFN_vkGetPipelineCacheData(device, pipelineCache, pDataSize, pData): with pData null
    // the size comes back; with pData at most *pDataSize bytes are written, *pDataSize is set to the bytes written, and
    // VK_INCOMPLETE (:146) is returned when that was less than the whole cache
    size_t size = 0;
    VkResult res = gVk.vkGetPipelineCacheData(mDevice->device(), mCache, &size, nullptr);
    if (res != VK_SUCCESS || size < CACHE_HEADER_BYTES)
    {
        LL_WARNS("Vulkan") << "vkGetPipelineCacheData (size): " << WolfVkLoader::resultString(res) << ", " << size << " bytes; the pipeline cache was not saved" << LL_ENDL;
        return;
    }
    if (size > DISK_LIMIT_BYTES - DISK_ENVELOPE_BYTES)
    {
        LL_WARNS("Vulkan") << "The pipeline cache has grown to " << size << " bytes, above the " << DISK_LIMIT_BYTES
                           << " byte limit of its file: not saved, and the old file is removed" << LL_ENDL;
        LLFile::remove(mDiskFile, ENOENT);
        return;
    }
    std::vector<uint8_t> bytes(DISK_ENVELOPE_BYTES + size);
    res = gVk.vkGetPipelineCacheData(mDevice->device(), mCache, &size, bytes.data() + DISK_ENVELOPE_BYTES);
    if (res != VK_SUCCESS || size < CACHE_HEADER_BYTES)
    {
        // VK_INCOMPLETE: only the single-threaded owner builds pipelines, so the cache cannot grow between the two
        // calls; a driver that says so anyway has given a cut-off blob, which is not kept
        LL_WARNS("Vulkan") << "vkGetPipelineCacheData: " << WolfVkLoader::resultString(res) << "; the pipeline cache was not saved" << LL_ENDL;
        return;
    }
    bytes.resize(DISK_ENVELOPE_BYTES + size);   // a driver may write fewer bytes than it announced
    uint8_t* e = bytes.data();
    memcpy(e, DISK_MAGIC, sizeof(DISK_MAGIC));
    writeLE32(e + 8, DISK_ENVELOPE_VERSION);
    writeLE32(e + 12, 0);
    writeLE64(e + 16, static_cast<uint64_t>(size));
    writeLE64(e + 24, fnv1a64(e + DISK_ENVELOPE_BYTES, size));
    // written beside the file under a name of this process's own (two viewers may run), then renamed over it:
    // LLFile::rename replaces an existing file on every platform (llfile.cpp: MoveFileExW MOVEFILE_REPLACE_EXISTING,
    // POSIX rename), so a reader sees the old file or the new one, never a part of one
    const std::string tmp = mDiskFile + "." + std::to_string(LLApp::getPid()) + ".tmp";
    LLFILE* f = LLFile::fopen(tmp, "wb");
    if (!f)
    {
        LL_WARNS("Vulkan") << "The pipeline cache file could not be written: " << tmp << LL_ENDL;
        return;
    }
    const size_t put = fwrite(bytes.data(), 1, bytes.size(), f);
    const bool closed = fclose(f) == 0;
    if (put != bytes.size() || !closed || LLFile::rename(tmp, mDiskFile) != 0)
    {
        LL_WARNS("Vulkan") << "The pipeline cache file could not be written whole (disk full?): " << mDiskFile << LL_ENDL;
        LLFile::remove(tmp, ENOENT);
        return;
    }
    LL_INFOS("Vulkan") << "Pipeline cache saved: " << size << " bytes, " << mPipelines.size() << " pipelines this session" << LL_ENDL;
}

void WolfVkPipelineCache::saveIfDue(double now_seconds)
{
    if (mLastSave < 0.0) mLastSave = now_seconds;
    if (mBuiltSinceSave == 0 || now_seconds - mLastSave < SAVE_INTERVAL_SECONDS) return;
    mLastSave = now_seconds;
    saveDisk();
}

void WolfVkPipelineCache::init(WolfVkDevice& device, const std::string& disk_file)
{
    if (std::find(liveCaches().begin(), liveCaches().end(), this) == liveCaches().end()) liveCaches().push_back(this);
    mDevice = &device;
    mDiskFile = disk_file;
    mBuiltSinceSave = 0;
    mLastSave = -1.0;
    std::vector<uint8_t> initial;
    const bool loaded = !mDiskFile.empty() && loadDisk(initial);
    // Source: vulkan_core.h:4083 VkPipelineCacheCreateInfo { sType, pNext, flags, initialDataSize, pInitialData }
    VkPipelineCacheCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;   // :224
    info.initialDataSize = loaded ? initial.size() : 0;
    info.pInitialData = loaded ? initial.data() : nullptr;
    VkResult res = gVk.vkCreatePipelineCache(device.device(), &info, nullptr, &mCache);
    if (res != VK_SUCCESS && loaded)
    {
        // the saved data was not taken: an empty cache, as on a first run
        LL_WARNS("Vulkan") << "vkCreatePipelineCache with the saved data: " << WolfVkLoader::resultString(res) << "; starting an empty one" << LL_ENDL;
        info.initialDataSize = 0;
        info.pInitialData = nullptr;
        mCache = VK_NULL_HANDLE;
        res = gVk.vkCreatePipelineCache(device.device(), &info, nullptr, &mCache);
    }
    if (res != VK_SUCCESS)
    {
        LL_WARNS("Vulkan") << "vkCreatePipelineCache: " << WolfVkLoader::resultString(res) << " (pipelines are built without a cache)" << LL_ENDL;
        mCache = VK_NULL_HANDLE;
    }
    else if (loaded)
    {
        LL_INFOS("Vulkan") << "Pipeline cache loaded: " << initial.size() << " bytes from " << mDiskFile << LL_ENDL;
    }
}

void WolfVkPipelineCache::destroy()
{
    // [HANG GUARD 2026-11-08 8.2k] bounded idle (WolfVkDevice::waitIdle) instead of vkDeviceWaitIdle; a hung device keeps its objects
    if (mDevice && mDevice->hasDevice() && mDevice->waitIdle())
    {
        if (mBuiltSinceSave != 0) saveDisk();   // [W114-VULKAN S7 2026-11-26] what this session compiled, for the next start
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
    mDiskFile.clear();
    mBuiltSinceSave = 0;
    liveCaches().erase(std::remove(liveCaches().begin(), liveCaches().end(), this), liveCaches().end());   // [V5]
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
    mBuiltSinceSave++;   // [W114-VULKAN S7 2026-11-26] saveIfDue
    return p;
}

VkPipeline WolfVkPipelineCache::build(const WolfVkProgram& program, const WolfVkPipelineState& state, VkRenderPass render_pass, std::string& error)
{
    // Source: vulkan_core.h:4104 VkPipelineShaderStageCreateInfo { sType, pNext, flags, stage, module, pName, pSpecializationInfo }
    VkPipelineShaderStageCreateInfo stages[3]{};
    uint32_t stage_count = 0;
    stages[stage_count].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;   // :225
    stages[stage_count].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[stage_count].module = program.vertex().mModule;
    stages[stage_count].pName = program.vertex().mReflection.mEntryPoint.c_str();
    stages[stage_count].pSpecializationInfo = program.specialization();   // [SPEC CONSTANTS 2026-11-26]
    stage_count++;
    if (program.hasGeometry())   // [GEOMETRY 2026-11-26]
    {
        stages[stage_count].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[stage_count].stage = VK_SHADER_STAGE_GEOMETRY_BIT;   // :2886
        stages[stage_count].module = program.geometry().mModule;
        stages[stage_count].pName = program.geometry().mReflection.mEntryPoint.c_str();
        stages[stage_count].pSpecializationInfo = program.specialization();
        stage_count++;
    }
    stages[stage_count].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[stage_count].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[stage_count].module = program.fragment().mModule;
    stages[stage_count].pName = program.fragment().mReflection.mEntryPoint.c_str();
    stages[stage_count].pSpecializationInfo = program.specialization();
    stage_count++;

    // [W114-VULKAN S7 2026-11-26] VUID-VkGraphicsPipelineCreateInfo-layout-01688: "The number of resources in layout
    // accessible to each shader stage that is used by the pipeline must be less than or equal to
    // VkPhysicalDeviceLimits::maxPerStageResources" (vulkan_core.h:3605); "For the fragment shader stage the framebuffer
    // color attachments also count against this limit" (chapters/limits.adoc:289-290), known only here.
    {
        const uint32_t limit = mDevice->limits().maxPerStageResources;
        const uint32_t need[3] = { program.stageResources(0), program.hasGeometry() ? program.stageResources(1) : 0u,
                                   program.stageResources(2) + state.mColorAttachments };
        const char* stage_names[3] = { "vertex", "geometry", "fragment" };
        for (int st = 0; st < 3; ++st)
        {
            if (need[st] > limit)
            {
                error = std::string("the ") + stage_names[st] + " stage needs " + std::to_string(need[st]) + " resources (samplers, uniform buffers"
                      + (st == 2 ? ", colour attachments" : "") + ") and this device allows " + std::to_string(limit) + " per stage (maxPerStageResources)";
                return VK_NULL_HANDLE;
            }
        }
    }

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
    // [W114-VULKAN MAC 2026-11-26] VUID-VkVertexInputBindingDescription-stride-04456 (VK_KHR_portability_subset): each
    // stride a multiple of, and at least, minVertexInputBindingStrideAlignment (4 on MoltenVK's non-Apple5 GPUs, 1 on
    // Apple5+, MVKDevice.mm:2433/:2568). The viewer's strides (LLVertexBuffer::sTypeSize, llvertexbuffer.cpp:704-720) are
    // 4, 8 and 16; a device asking for more gets no pipeline rather than an invalid one.
    const uint32_t stride_align = mDevice->minVertexStrideAlignment();
    for (const VkVertexInputBindingDescription& bd : bindings)
    {
        if (bd.stride < stride_align || bd.stride % stride_align != 0)
        {
            error = llformat("vertex binding %u stride %u is not a multiple of the device's minVertexInputBindingStrideAlignment %u",
                             bd.binding, bd.stride, stride_align);
            return VK_NULL_HANDLE;
        }
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
    // [W114-VULKAN MAC 2026-11-26] VUID-VkPipelineInputAssemblyStateCreateInfo-triangleFans-04452 (see the header)
    if (assembly.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN && !mDevice->triangleFans())
    {
        error = "the device's VK_KHR_portability_subset has no triangleFans: draw this fan as a TRIANGLE_LIST "
                "(WolfVkTopology::fanToTriangleList)";
        return VK_NULL_HANDLE;
    }
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
    // [W114-VULKAN S5 2026-11-26] wolfvkpipeline.h mDepthClamp; vulkan_core.h VkPipelineRasterizationStateCreateInfo::depthClampEnable
    if (state.mDepthClamp && !mDevice->depthClamp())
    {
        error = "depth clamp asked for on a device without the depthClamp feature";
        return VK_NULL_HANDLE;
    }
    raster.depthClampEnable = state.mDepthClamp ? VK_TRUE : VK_FALSE;
    raster.rasterizerDiscardEnable = VK_FALSE;
    raster.polygonMode = state.mWireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;   // :2680, :2679
    raster.cullMode = state.mCullFace ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;          // :3326, :3324 (glCullFace(GL_BACK) llrender.cpp:880)
    // [W114-VULKAN S4 2026-11-26/] glCullFace(GL_FRONT): VK_CULL_MODE_FRONT_BIT (vulkan_core.h:3325)
    if (state.mCullFace && state.mCullFront) raster.cullMode = VK_CULL_MODE_FRONT_BIT;
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
    // [W114-VULKAN S5 2026-11-26] exactly the subpass's colour attachments, none for a depth-only target (the shadow
    // maps): "attachmentCount ... must equal the colorAttachmentCount used to create subpass"
    // (VUID-VkGraphicsPipelineCreateInfo-renderPass-07609). It was max(1, n), one too many for a depth-only pass.
    std::vector<VkPipelineColorBlendAttachmentState> attachments(state.mColorAttachments, attachment);
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
    blend.pAttachments = attachments.empty() ? nullptr : attachments.data();

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
    info.stageCount = stage_count;
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

namespace WolfVkTopology
{
namespace
{
    // Source: Vulkan spec "Triangle Fans" (chapters/drawing.adoc): p_i = {v_(i+1), v_(i+2), v_0}, max(0, n - 2) primitives
    template <typename T>
    void fanToList(uint32_t first, uint32_t count, const T* indices, std::vector<uint32_t>& out)
    {
        out.clear();
        if (count < 3) return;
        auto vertex = [&](uint32_t k) -> uint32_t { return indices ? static_cast<uint32_t>(indices[first + k]) : first + k; };
        out.reserve(static_cast<size_t>(count - 2) * 3);
        for (uint32_t i = 0; i + 2 < count; ++i)
        {
            out.push_back(vertex(i + 1));
            out.push_back(vertex(i + 2));
            out.push_back(vertex(0));
        }
    }
}

void fanToTriangleList(uint32_t first, uint32_t count, const uint16_t* indices, std::vector<uint32_t>& out)
{
    fanToList(first, count, indices, out);
}

void fanToTriangleList(uint32_t first, uint32_t count, const uint32_t* indices, std::vector<uint32_t>& out)
{
    fanToList(first, count, indices, out);
}
} // namespace WolfVkTopology
