/**
 * @file wolfvkshader.cpp
 * @brief LLGLSLShader's Vulkan side (see wolfvkshader.h).
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
#include "wolfvkshader.h"
#include "wolfvkdevice.h"
#include "llglslshader.h"
#include "llshadermgr.h"
#include <algorithm>   // std::sort (describeUniforms)
#include "llgl.h"
#include "llmd5.h"
#include "llfile.h"
#include "lldir.h"   // gDirUtilp
#include <cstring>
#include <fstream>
#include <set>

// ---------------------------------------------------------------------------------------------------------------
// WolfVkFrameUniforms

bool WolfVkFrameUniforms::init(WolfVkDevice& device, VkDeviceSize bytes_per_frame, std::string& error)
{
    destroy();
    mDevice = &device;
    mBytes = bytes_per_frame;
    mAlignment = std::max<VkDeviceSize>(16, device.limits().minUniformBufferOffsetAlignment);   // vulkan_core.h:3653
    const uint32_t frames = device.framesInFlight();
    for (uint32_t i = 0; i < frames; ++i)
    {
        std::unique_ptr<WolfVkBuffer> b(new WolfVkBuffer());
        if (!b->create(device, bytes_per_frame, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true /*host only*/, error))   // :3062
        {
            destroy();
            return false;
        }
        mBuffers.push_back(std::move(b));
        // Source: vulkan_core.h:4185 VkDescriptorPoolSize { type, descriptorCount }; :4190 VkDescriptorPoolCreateInfo { sType, pNext, flags, maxSets, poolSizeCount, pPoolSizes }
        // Enough for a few thousand draws per frame: two dynamic uniform descriptors and up to eight samplers each.
        const VkDescriptorPoolSize sizes[2] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 8192 }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32768 } };
        VkDescriptorPoolCreateInfo pool{};
        pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;   // :240
        pool.maxSets = 8192;
        pool.poolSizeCount = 2;
        pool.pPoolSizes = sizes;
        VkDescriptorPool p = VK_NULL_HANDLE;
        VkResult res = gVk.vkCreateDescriptorPool(device.device(), &pool, nullptr, &p);
        if (res != VK_SUCCESS)
        {
            error = std::string("vkCreateDescriptorPool: ") + WolfVkLoader::resultString(res);
            destroy();
            return false;
        }
        mPools.push_back(p);
    }
    return true;
}

void WolfVkFrameUniforms::destroy()
{
    // [HANG GUARD 2026-11-08 8.2k] bounded idle (WolfVkDevice::waitIdle) instead of vkDeviceWaitIdle; a hung device keeps its objects
    if (mDevice && mDevice->hasDevice() && mDevice->waitIdle())
    {
        for (VkDescriptorPool p : mPools) gVk.vkDestroyDescriptorPool(mDevice->device(), p, nullptr);
    }
    mPools.clear();
    mBuffers.clear();
    mDevice = nullptr;
    mUsed = 0;
}

void WolfVkFrameUniforms::beginFrame(uint32_t frame_index)
{
    mFrame = frame_index % static_cast<uint32_t>(mBuffers.size());
    mUsed = 0;
    mWarnedFull = false;
    // Source: vulkan_core.h:4626 PFN_vkResetDescriptorPool(device, pool, flags): every set of the previous use of this frame slot goes
    gVk.vkResetDescriptorPool(mDevice->device(), mPools[mFrame], 0);
}

void WolfVkFrameUniforms::resetPools()
{
    if (!mDevice || !mDevice->hasDevice()) return;
    for (VkDescriptorPool p : mPools)
    {
        // Source: vulkan_core.h:4626 PFN_vkResetDescriptorPool
        gVk.vkResetDescriptorPool(mDevice->device(), p, 0);
    }
}

bool WolfVkFrameUniforms::push(const void* data, VkDeviceSize size, VkDeviceSize& offset)
{
    const VkDeviceSize start = (mUsed + mAlignment - 1) / mAlignment * mAlignment;
    if (start + size > mBytes)
    {
        if (!mWarnedFull)
        {
            LL_WARNS("Vulkan") << "Frame uniform ring full (" << mBytes << " bytes); later draws this frame are skipped" << LL_ENDL;
            mWarnedFull = true;
        }
        return false;
    }
    mBuffers[mFrame]->write(start, data, size);
    offset = start;
    mUsed = start + size;
    return true;
}

VkDescriptorSet WolfVkFrameUniforms::allocate(VkDescriptorSetLayout layout)
{
    // Source: vulkan_core.h:4199 VkDescriptorSetAllocateInfo { sType, pNext, descriptorPool, descriptorSetCount, pSetLayouts }
    VkDescriptorSetAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;   // :241
    info.descriptorPool = mPools[mFrame];
    info.descriptorSetCount = 1;
    info.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkResult res = gVk.vkAllocateDescriptorSets(mDevice->device(), &info, &set);
    if (res != VK_SUCCESS)
    {
        LL_WARNS_ONCE("Vulkan") << "vkAllocateDescriptorSets: " << WolfVkLoader::resultString(res) << LL_ENDL;
        return VK_NULL_HANDLE;
    }
    return set;
}

// ---------------------------------------------------------------------------------------------------------------
// Recipe keys

std::string WolfVkShaderMirror::versionLine(bool geometry_shader)
{
    // Source: llshadermgr.cpp:657-711, the same branches in the same order.
    const S32 major_version = gGLManager.mGLSLVersionMajor;
    const S32 minor_version = gGLManager.mGLSLVersionMinor;
    if (major_version >= 4)
    {
        return minor_version >= 20 ? "#version 420\n" : "#version 400\n";
    }
    if (major_version == 3)
    {
        return minor_version <= 29 ? "#version 150\n" : "#version 330\n";
    }
    return (geometry_shader || minor_version >= 50) ? "#version 150\n" : "#version 140\n";
}

std::string WolfVkShaderMirror::unitRecipe(const std::string& stage, const std::string& filename, int found_level, const std::string& version_line,
                                           const std::map<std::string, std::string>* defines, int channels, bool amd, bool old_select)
{
    // shader_vulkan.py unit_recipe: "|".join([stage, filename, str(found_level), version_line.strip(), defs, str(channels), amd, oldselect])
    std::string defs;
    if (defines)
    {
        for (const auto& kv : *defines)   // std::map: sorted by key, as Python's sorted()
        {
            if (kv.first == "OLD_SELECT") continue;
            if (!defs.empty()) defs += ";";
            defs += kv.first + "=" + kv.second;
        }
    }
    std::string version = version_line;
    while (!version.empty() && (version.back() == '\n' || version.back() == '\r' || version.back() == ' ')) version.pop_back();
    return stage + "|" + filename + "|" + std::to_string(found_level) + "|" + version + "|" + defs + "|" + std::to_string(channels) + "|"
         + (amd ? "amd" : "") + "|" + (old_select ? "oldselect" : "");
}

std::string WolfVkShaderMirror::programKey(const std::vector<std::string>& unit_recipes)
{
    std::string joined;
    for (size_t i = 0; i < unit_recipes.size(); ++i)
    {
        if (i) joined += "\n";
        joined += unit_recipes[i];
    }
    LLMD5 md5;
    md5.update(reinterpret_cast<const uint8_t*>(joined.data()), joined.size());
    md5.finalize();
    char hex[MD5HEX_STR_SIZE];
    md5.hex_digest(hex);
    return std::string(hex);
}

// ---------------------------------------------------------------------------------------------------------------
// WolfVkShaderMirror

namespace
{
    bool readSpv(const std::string& path, std::vector<uint32_t>& words)
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
}

WolfVkShaderMirror* WolfVkShaderMirror::create(WolfVkDevice& device, LLGLSLShader* shader, const std::string& spv_dir, std::string& error)
{
    if (!shader)
    {
        error = "no shader";
        return nullptr;
    }
    LLShaderMgr* mgr = LLShaderMgr::instance();
    const bool amd = gGLManager.mIsAMD;
#if LL_DARWIN
    const bool old_select = !gGLManager.mIsApple;   // llglslshader.cpp createShader: mDefines["OLD_SELECT"] on LL_DARWIN && !mIsApple
#else
    const bool old_select = false;
#endif
    // Attach order = LLGLSLShader::createShader: the program's own files, then attachShaderFeatures' vertex objects, then its fragment objects
    std::vector<std::string> recipes;
    for (const auto& file : shader->mShaderFiles)
    {
        const bool wolf_unit = file.first.compare(0, 7, "wolftk/") == 0;   // llglslshader.cpp createShader: wolftk units get 0 channels
        const int channels = wolf_unit ? 0 : shader->mFeatures.mIndexedTextureChannels;
        const std::string stage = (file.second == GL_VERTEX_SHADER) ? "vert" : "frag";
        int found = 0;
        if (!mgr->wolfVkFindShaderClass(file.first, shader->mShaderLevel, found))
        {
            error = shader->mName + ": shader file " + file.first + " not found below class " + std::to_string(shader->mShaderLevel);
            return nullptr;
        }
        recipes.push_back(unitRecipe(stage, file.first, found, versionLine(false), &shader->mDefines, channels, amd, old_select));
    }
    for (const auto& feature : shader->mWolfVkFeatureObjects)
    {
        const std::string stage = (feature.second == GL_VERTEX_SHADER) ? "vert" : "frag";
        auto it = mgr->mWolfVkUnitRecipes.find(stage + "|" + feature.first);
        if (it == mgr->mWolfVkUnitRecipes.end())
        {
            error = shader->mName + ": no recipe recorded for the shared object " + feature.first + " (" + stage + ")";
            return nullptr;
        }
        recipes.push_back(it->second);
    }
    const std::string key = programKey(recipes);
    const std::string vert_path = spv_dir + gDirUtilp->getDirDelimiter() + key + ".vert.spv";
    const std::string frag_path = spv_dir + gDirUtilp->getDirDelimiter() + key + ".frag.spv";
    std::vector<uint32_t> vert, frag;
    if (!readSpv(vert_path, vert) || !readSpv(frag_path, frag))
    {
        error = "no SPIR-V for \"" + shader->mName + "\" (key " + key + ", " + vert_path + "); recipe:";
        for (const std::string& r : recipes) error += "\n    " + r;
        return nullptr;
    }
    WolfVkShaderMirror* m = new WolfVkShaderMirror();
    m->mDevice = &device;
    m->mKey = key;
    m->mName = shader->mName;
    if (!m->mProgram.create(device, shader->mName, vert, frag, error))
    {
        delete m;
        return nullptr;
    }
    // the uniform images and the name table
    const WolfVkProgram::Stage* stages[2] = { &m->mProgram.vertex(), &m->mProgram.fragment() };
    std::vector<uint8_t>* images[2] = { &m->mVertImage, &m->mFragImage };
    for (int s = 0; s < 2; ++s)
    {
        if (!stages[s]->mUniforms) continue;
        images[s]->assign(stages[s]->mUniforms->mSize, 0);
        for (const WolfVkSpirv::BlockMember& member : stages[s]->mUniforms->mMembers)
        {
            m->mMembers[member.mName].push_back({ &member, images[s] });
        }
    }
    LL_INFOS("Vulkan") << "Shader mirror for \"" << shader->mName << "\": key " << key << ", " << m->mMembers.size() << " uniforms, "
                       << m->mProgram.samplers().size() << " samplers" << LL_ENDL;
    return m;
}

WolfVkShaderMirror::~WolfVkShaderMirror()
{
    mProgram.destroy();
}

int32_t WolfVkShaderMirror::samplerChannel(const std::string& sampler_name) const
{
    auto it = mSamplerChannels.find(sampler_name);
    return it == mSamplerChannels.end() ? -1 : it->second;
}

void WolfVkShaderMirror::applyProjectionFix(const std::string& name, std::vector<float>& columns) const
{
    // modelview_projection_matrix and projection_matrix (llshadermgr.cpp mReservedUniforms "projection_matrix",
    // "modelview_projection_matrix"): M' = C * M with C = [1 0 0 0; 0 1 0 0; 0 0 .5 .5; 0 0 0 1] (wolfvkshader.h).
    // Column-major: element (row r, column c) is columns[c*4 + r]. Only row 2 changes: r2' = 0.5*r2 + 0.5*r3.
    if (columns.size() != 16 || (name != "modelview_projection_matrix" && name != "projection_matrix")) return;
    // [GL DEPTH 2026-11-08] with VK_EXT_depth_clip_control every pipeline takes GL's z range (wolfvkpipeline.cpp), so GL's
    // matrices go through unchanged and the depth is made by the viewport transform as in GL (docs 8.2n)
    if (mDevice && mDevice->glDepthRange()) return;
    for (int c = 0; c < 4; ++c)
    {
        columns[static_cast<size_t>(c * 4 + 2)] = 0.5f * columns[static_cast<size_t>(c * 4 + 2)] + 0.5f * columns[static_cast<size_t>(c * 4 + 3)];
    }
}

std::string WolfVkShaderMirror::describeUniforms() const
{
    std::vector<std::string> names;
    for (const auto& kv : mMembers) names.push_back(kv.first);
    std::sort(names.begin(), names.end());
    std::string out;
    std::vector<float> v;
    std::string where;
    for (const std::string& n : names)
    {
        if (!debugMember(n, v, where) || v.size() > 4) continue;   // matrices and arrays: set per draw / too long for a line
        out += n + "=";
        for (size_t i = 0; i < v.size(); ++i) out += llformat(i ? ",%g" : "%g", v[i]);
        out += "; ";
    }
    return out;
}

bool WolfVkShaderMirror::debugMember(const std::string& name, std::vector<float>& out_floats, std::string& out_where) const
{
    auto it = mMembers.find(name);
    if (it == mMembers.end()) return false;
    out_floats.clear();
    out_where.clear();
    for (const Member& m : it->second)
    {
        const WolfVkSpirv::BlockMember& bm = *m.mMember;
        out_where += (m.mImage == &mVertImage ? "vert:" : "frag:") + std::to_string(bm.mOffset) + " ";
        if (out_floats.empty())
        {
            const uint32_t floats = std::max<uint32_t>(1, bm.mColumns) * std::max<uint32_t>(1, bm.mRows);
            const uint32_t stride = (bm.mColumns > 1 && bm.mMatrixStride) ? bm.mMatrixStride : bm.mRows * 4;
            for (uint32_t c = 0; c < std::max<uint32_t>(1, bm.mColumns); ++c)
            {
                for (uint32_t r = 0; r < std::max<uint32_t>(1, bm.mRows); ++r)
                {
                    const size_t at = bm.mOffset + (bm.mColumns > 1 ? c * stride : 0) + r * 4;
                    float f = 0.f;
                    if (at + 4 <= m.mImage->size()) memcpy(&f, m.mImage->data() + at, 4);
                    out_floats.push_back(f);
                }
            }
            if (out_floats.size() != floats) out_floats.resize(floats);
        }
    }
    return true;
}

void WolfVkShaderMirror::writeMember(const Member& m, Kind kind, uint32_t count, const void* data, bool transpose)
{
    const WolfVkSpirv::BlockMember& bm = *m.mMember;
    std::vector<uint8_t>& image = *m.mImage;
    // element shape from the kind (what the viewer passed), element layout from the reflection (what the shader has)
    uint32_t cols = 1, rows = 1, scalar = 4;
    switch (kind)
    {
    case Kind::F1: case Kind::I1: rows = 1; break;
    case Kind::F2: case Kind::I2: rows = 2; break;
    case Kind::F3: rows = 3; break;
    case Kind::F4: case Kind::I4: case Kind::U4: rows = 4; break;
    case Kind::M2: cols = 2; rows = 2; break;
    case Kind::M3: cols = 3; rows = 3; break;
    case Kind::M3x4: cols = 3; rows = 4; break;   // GL mat3x4: 3 columns of 4
    case Kind::M4: cols = 4; rows = 4; break;
    }
    if (bm.mRows != rows || bm.mColumns != cols)
    {
        // GL would reject a mismatched glUniform too (GL_INVALID_OPERATION); say so once and keep the image as it was
        if (!mUnknownWarned[bm.mName])
        {
            LL_WARNS("Vulkan") << mName << ": uniform " << bm.mName << " set as " << cols << "x" << rows << " but the shader has " << bm.mColumns << "x" << bm.mRows << LL_ENDL;
            mUnknownWarned[bm.mName] = true;
        }
        return;
    }
    const uint32_t n = std::min<uint32_t>(count, std::max<uint32_t>(1, bm.mArraySize));
    const uint8_t* src = static_cast<const uint8_t*>(data);
    const uint32_t element_floats = cols * rows;
    for (uint32_t e = 0; e < n; ++e)
    {
        const uint32_t base = bm.mOffset + e * (bm.mArraySize > 1 ? bm.mArrayStride : 0);
        const uint8_t* elem = src + static_cast<size_t>(e) * element_floats * scalar;
        if (cols == 1)
        {
            if (base + rows * scalar > image.size()) return;
            memcpy(image.data() + base, elem, rows * scalar);
        }
        else
        {
            // column-major in GL (transpose == false): column c = floats [c*rows, c*rows+rows); std140 ColMajor: column c at base + c*matrixStride
            const uint32_t stride = bm.mMatrixStride ? bm.mMatrixStride : rows * scalar;
            for (uint32_t c = 0; c < cols; ++c)
            {
                for (uint32_t r = 0; r < rows; ++r)
                {
                    const uint32_t src_index = transpose ? (r * cols + c) : (c * rows + r);
                    const uint32_t dst = bm.mRowMajor ? base + r * stride + c * scalar : base + c * stride + r * scalar;
                    if (dst + scalar > image.size()) return;
                    memcpy(image.data() + dst, elem + src_index * scalar, scalar);
                }
            }
        }
    }
}

void WolfVkShaderMirror::set(const std::string& name, Kind kind, uint32_t count, const void* data, bool transpose)
{
    if (!data || count == 0) return;
    auto it = mMembers.find(name);
    if (it == mMembers.end())
    {
        // a sampler's texture unit, or a uniform the compiler removed as unused, or one this stage pair lacks
        if (kind == Kind::I1 && mProgram.samplerBinding(name) >= 0)
        {
            mSamplerChannels[name] = *static_cast<const int32_t*>(data);
        }
        return;
    }
    if ((kind == Kind::M4) && (name == "modelview_projection_matrix" || name == "projection_matrix"))
    {
        std::vector<float> fixed(static_cast<const float*>(data), static_cast<const float*>(data) + 16);
        if (transpose)
        {
            // make it column-major first, then correct, then write untransposed
            std::vector<float> cm(16);
            for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) cm[static_cast<size_t>(c * 4 + r)] = fixed[static_cast<size_t>(r * 4 + c)];
            fixed.swap(cm);
        }
        applyProjectionFix(name, fixed);
        for (const Member& m : it->second) writeMember(m, kind, 1, fixed.data(), false);
        return;
    }
    for (const Member& m : it->second)
    {
        writeMember(m, kind, count, data, transpose);
    }
}

bool WolfVkShaderMirror::bindUniforms(VkCommandBuffer cmd, WolfVkFrameUniforms& ring)
{
    VkDescriptorSet set = ring.allocate(mProgram.uniformSetLayout());
    if (set == VK_NULL_HANDLE) return false;
    VkDescriptorBufferInfo infos[2]{};
    VkWriteDescriptorSet writes[2]{};
    uint32_t dynamic_offsets[2] = { 0, 0 };
    uint32_t n = 0;
    const WolfVkProgram::Stage* stages[2] = { &mProgram.vertex(), &mProgram.fragment() };
    const std::vector<uint8_t>* images[2] = { &mVertImage, &mFragImage };
    const uint32_t bindings[2] = { WolfVkProgram::BINDING_UNIFORMS_VERT, WolfVkProgram::BINDING_UNIFORMS_FRAG };
    for (int s = 0; s < 2; ++s)
    {
        if (!stages[s]->mUniforms || images[s]->empty()) continue;
        VkDeviceSize offset = 0;
        if (!ring.push(images[s]->data(), images[s]->size(), offset)) return false;
        // Source: vulkan_core.h:4173 VkDescriptorBufferInfo { buffer, offset, range }: the dynamic descriptor points at offset 0, the per-draw
        // offset comes through pDynamicOffsets (spec 14.2.4 "Descriptor Set Binding")
        infos[n] = { ring.buffer(), 0, images[s]->size() };
        // Source: vulkan_core.h:4223 VkWriteDescriptorSet { sType, pNext, dstSet, dstBinding, dstArrayElement, descriptorCount, descriptorType, pImageInfo, pBufferInfo, pTexelBufferView }
        writes[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;   // :242
        writes[n].dstSet = set;
        writes[n].dstBinding = bindings[s];
        writes[n].descriptorCount = 1;
        writes[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        writes[n].pBufferInfo = &infos[n];
        dynamic_offsets[n] = static_cast<uint32_t>(offset);
        n++;
    }
    if (n)
    {
        gVk.vkUpdateDescriptorSets(mDevice->device(), n, writes, 0, nullptr);   // :4629
    }
    // Source: vulkan_core.h:4631 PFN_vkCmdBindDescriptorSets(cmd, bindPoint, layout, firstSet, setCount, pSets, dynamicOffsetCount, pDynamicOffsets)
    gVk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mProgram.pipelineLayout(), WolfVkProgram::SET_UNIFORMS, 1, &set, n, dynamic_offsets);
    return true;
}

bool WolfVkShaderMirror::bindSamplers(VkCommandBuffer cmd, WolfVkFrameUniforms& ring, const std::vector<VkDescriptorImageInfo>& by_binding, const VkDescriptorImageInfo& fallback)
{
    if (mProgram.samplers().empty()) return true;
    VkDescriptorSet set = ring.allocate(mProgram.samplerSetLayout());
    if (set == VK_NULL_HANDLE) return false;
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<std::vector<VkDescriptorImageInfo>> infos;
    writes.reserve(mProgram.samplers().size());
    infos.reserve(mProgram.samplers().size());
    for (const WolfVkSpirv::Sampler& smp : mProgram.samplers())
    {
        infos.emplace_back();
        std::vector<VkDescriptorImageInfo>& arr = infos.back();
        for (uint32_t i = 0; i < smp.mArraySize; ++i)
        {
            const uint32_t b = smp.mBinding + i;
            // Source: vulkan_core.h:4179 VkDescriptorImageInfo { sampler, imageView, imageLayout }
            arr.push_back((b < by_binding.size() && by_binding[b].imageView != VK_NULL_HANDLE) ? by_binding[b] : fallback);
        }
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = set;
        w.dstBinding = smp.mBinding;
        w.descriptorCount = smp.mArraySize;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = arr.data();
        writes.push_back(w);
    }
    gVk.vkUpdateDescriptorSets(mDevice->device(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    gVk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mProgram.pipelineLayout(), WolfVkProgram::SET_SAMPLERS, 1, &set, 0, nullptr);
    return true;
}
