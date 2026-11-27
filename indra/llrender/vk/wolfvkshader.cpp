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
#include "wolfcoreloader.h"
#include "wolfvkdevice.h"
#include "llglslshader.h"
#include "llshadermgr.h"
#include <algorithm>   // std::sort (describeUniforms)
#include "llgl.h"
#include "llmd5.h"
#include "llfile.h"
#include "lldir.h"   // gDirUtilp
#include <cstring>
#include <locale>    // std::locale::classic (spec constants)
#include <sstream>
#include <fstream>
#include <set>
#include <filesystem>   // [STORE STAMP 2026-11-26] shaderTreeMD5
#include "fsyspath.h"

// [STORE STAMP 2026-11-26] indra/llrender/CMakeLists.txt gives this file the MD5 of the transform scripts
// (scripts/shader_vulkan.py --transform-md5). A build without it (no Python at configure time) has no stamp to compare:
// every store is then reported as not checkable and refused, never silently trusted.
#ifndef WOLFVK_TRANSFORM_MD5
#define WOLFVK_TRANSFORM_MD5 ""
#endif

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
        VkDescriptorPool p = createPool(error);
        if (p == VK_NULL_HANDLE)
        {
            destroy();
            return false;
        }
        mPools.push_back(std::vector<VkDescriptorPool>(1, p));
    }
    return true;
}

VkDescriptorPool WolfVkFrameUniforms::createPool(std::string& error)
{
    // Source: vulkan_core.h:4185 VkDescriptorPoolSize { type, descriptorCount }; :4190 VkDescriptorPoolCreateInfo { sType, pNext, flags, maxSets, poolSizeCount, pPoolSizes }
    // Enough for a few thousand draws: two dynamic uniform descriptors and up to eight samplers each. [V6 2026-11-11] a
    // frame that needs more chains another pool of the same size (allocate)
    const VkDescriptorPoolSize sizes[2] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 8192 }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32768 } };
    VkDescriptorPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;   // :240
    pool.maxSets = 8192;
    pool.poolSizeCount = 2;
    pool.pPoolSizes = sizes;
    VkDescriptorPool p = VK_NULL_HANDLE;
    VkResult res = gVk.vkCreateDescriptorPool(mDevice->device(), &pool, nullptr, &p);
    if (res != VK_SUCCESS)
    {
        error = std::string("vkCreateDescriptorPool: ") + WolfVkLoader::resultString(res);
        return VK_NULL_HANDLE;
    }
    return p;
}

void WolfVkFrameUniforms::destroy()
{
    // [HANG GUARD 2026-11-08 8.2k] bounded idle (WolfVkDevice::waitIdle) instead of vkDeviceWaitIdle; a hung device keeps its objects
    if (mDevice && mDevice->hasDevice() && mDevice->waitIdle())
    {
        for (const std::vector<VkDescriptorPool>& chain : mPools)
            for (VkDescriptorPool p : chain) gVk.vkDestroyDescriptorPool(mDevice->device(), p, nullptr);
    }
    mPools.clear();
    mBuffers.clear();
    mDevice = nullptr;
    mUsed = 0;
}

void WolfVkFrameUniforms::beginFrame(uint32_t frame_index)
{
    mFrame = frame_index % static_cast<uint32_t>(mBuffers.size());
    mEpoch++;   // [S2 2026-11-26] every set and ring offset of the slot's last use goes with the reset below
    mUsed = 0;
    mWarnedFull = false;
    mWarnedPools = false;
    mPoolInUse = 0;
    // Source: vulkan_core.h:4626 PFN_vkResetDescriptorPool(device, pool, flags): every set of the previous use of this frame slot goes
    for (VkDescriptorPool p : mPools[mFrame]) gVk.vkResetDescriptorPool(mDevice->device(), p, 0);
}

void WolfVkFrameUniforms::resetPools()
{
    if (!mDevice || !mDevice->hasDevice()) return;
    for (const std::vector<VkDescriptorPool>& chain : mPools)
    {
        // Source: vulkan_core.h:4626 PFN_vkResetDescriptorPool
        for (VkDescriptorPool p : chain) gVk.vkResetDescriptorPool(mDevice->device(), p, 0);
    }
}

bool WolfVkFrameUniforms::needBind(VkCommandBuffer cmd, uint32_t set_index, VkPipelineLayout layout, VkDescriptorSet set, const uint32_t* offsets, uint32_t offset_count)
{
    if (set_index >= 3 || offset_count > 8) return true;   // not tracked: always bind
    if (cmd != mBoundCmd || mEpoch != mBoundEpoch || layout != mBoundLayout)
    {
        // another command buffer, frame or pipeline layout: nothing is known to be bound
        mBoundCmd = cmd;
        mBoundEpoch = mEpoch;
        mBoundLayout = layout;
        mPushLayout = VK_NULL_HANDLE;
        for (BoundSlot& b : mBound) b = BoundSlot();
    }
    BoundSlot& b = mBound[set_index];
    if (b.mSet == set && b.mCount == offset_count && (offset_count == 0 || memcmp(b.mOffsets, offsets, offset_count * sizeof(uint32_t)) == 0))
    {
        return false;
    }
    b.mSet = set;
    b.mCount = offset_count;
    if (offset_count) memcpy(b.mOffsets, offsets, offset_count * sizeof(uint32_t));
    return true;
}

bool WolfVkFrameUniforms::push(const void* data, VkDeviceSize size, VkDeviceSize& offset, bool essential)
{
    const VkDeviceSize start = (mUsed + mAlignment - 1) / mAlignment * mAlignment;
    // [V6 2026-11-11] the last ESSENTIAL_RESERVE bytes are kept for the frame's composite (essential), so a frame whose
    // scene draws filled the ring still shows its picture instead of a black frame
    const VkDeviceSize limit = essential ? mBytes : (mBytes > ESSENTIAL_RESERVE ? mBytes - ESSENTIAL_RESERVE : 0);
    if (start + size > limit)
    {
        if (!mWarnedFull)
        {
            // once per frame that overflows, at WARN: the G-buffer pass reports itself incomplete and OpenGL draws it
            // whole (WolfVkScene::gbufferPassComplete), so nothing goes missing from the picture
            LL_WARNS("Vulkan") << "Frame uniform ring full (" << mBytes << " bytes" << (essential ? "" : ", the composite's reserve kept")
                               << "); later Vulkan draws this frame are skipped and drawn by OpenGL" << LL_ENDL;
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
    // [V6 2026-11-11] Was one pool of 8192 sets per frame slot (about 4096 draws): past that every allocation failed with
    // VK_ERROR_OUT_OF_POOL_MEMORY, logged once a session, and the frame's composite could find no set at all (a black
    // frame). Now a full pool chains the next one of the slot (made on first need, kept and reset with the slot), up to
    // MAX_POOLS_PER_FRAME; only past that cap does an allocation fail, logged at WARN once per frame.
    // vkAllocateDescriptorSets "VK_ERROR_OUT_OF_POOL_MEMORY" / "VK_ERROR_FRAGMENTED_POOL": the pool cannot hold the set
    // (vulkan_core.h:161 / :158); any other error is not a full pool and is not retried.
    std::vector<VkDescriptorPool>& chain = mPools[mFrame];
    while (true)
    {
        // Source: vulkan_core.h:4199 VkDescriptorSetAllocateInfo { sType, pNext, descriptorPool, descriptorSetCount, pSetLayouts }
        VkDescriptorSetAllocateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;   // :241
        info.descriptorPool = chain[mPoolInUse];
        info.descriptorSetCount = 1;
        info.pSetLayouts = &layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkResult res = gVk.vkAllocateDescriptorSets(mDevice->device(), &info, &set);
        if (res == VK_SUCCESS) return set;
        const bool full = res == VK_ERROR_OUT_OF_POOL_MEMORY || res == VK_ERROR_FRAGMENTED_POOL;
        if (full && mPoolInUse + 1 < chain.size())
        {
            mPoolInUse++;
            continue;
        }
        std::string error;
        if (full && chain.size() < MAX_POOLS_PER_FRAME)
        {
            VkDescriptorPool p = createPool(error);
            if (p != VK_NULL_HANDLE)
            {
                chain.push_back(p);
                mPoolInUse = static_cast<uint32_t>(chain.size() - 1);
                LL_INFOS("Vulkan") << "Descriptor pool " << chain.size() << " added to frame slot " << mFrame << " (a busy frame)" << LL_ENDL;
                continue;
            }
        }
        if (!mWarnedPools)
        {
            LL_WARNS("Vulkan") << "vkAllocateDescriptorSets: " << WolfVkLoader::resultString(res) << " with " << chain.size() << " pool(s) in this frame slot"
                               << (error.empty() ? std::string() : " (" + error + ")") << "; later Vulkan draws this frame are skipped and drawn by OpenGL" << LL_ENDL;
            mWarnedPools = true;
        }
        return VK_NULL_HANDLE;
    }
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

std::map<std::string, std::string> WolfVkShaderMirror::usedDefines(const std::set<std::string>& words, const std::map<std::string, std::string>& defines)
{
    // Source: scripts/shader_check.py used_defines()
    std::map<std::string, std::string> used;
    for (const auto& kv : defines)
    {
        if (words.count(kv.first)) used.insert(kv);
    }
    while (true)
    {
        std::set<std::string> value_words;
        for (const auto& kv : used)
        {
            const std::string& v = kv.second;
            size_t i = 0;
            while (i < v.size())
            {
                size_t j = i;
                while (j < v.size() && isWordChar(v[j])) ++j;
                if (j > i)
                {
                    value_words.insert(v.substr(i, j - i));
                    i = j;
                }
                else
                {
                    ++i;
                }
            }
        }
        bool more = false;
        for (const auto& kv : defines)
        {
            if (!used.count(kv.first) && value_words.count(kv.first))
            {
                used.insert(kv);
                more = true;
            }
        }
        if (!more) return used;
    }
}

bool WolfVkShaderMirror::specConstant(const std::string& define_name, uint32_t& id)
{
    // Source: scripts/shader_vulkan.py SPEC_CONSTANTS. TERRAIN_TRIPLANAR_BLEND_FACTOR: llviewershadermgr.cpp:1192-1193,
    // llformat("%.2f") of the F32 setting RenderTerrainPBRTriplanarBlendFactor - any value, so no build can list them.
    if (define_name == "TERRAIN_TRIPLANAR_BLEND_FACTOR")
    {
        id = 0;
        return true;
    }
    return false;
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
            uint32_t spec_id = 0;
            if (kv.first == "OLD_SELECT" || specConstant(kv.first, spec_id)) continue;
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

// [STORE STAMP 2026-11-26] wolfvkshader.h shaderTreeMD5
std::string WolfVkShaderMirror::shaderTreeMD5(const std::string& shaders_dir)
{
    namespace fs = std::filesystem;
    std::vector<std::pair<std::string, fs::path>> files;   // (relative path with '/', file)
    std::error_code ec;
    for (fs::directory_iterator top(fsyspath(shaders_dir), ec), end; !ec && top != end; top.increment(ec))
    {
        // shader_vulkan.tree_md5: only the directories named class<digits> (the store itself, spv/, is not part of it)
        const std::string class_name = fsyspath(top->path().filename()).string();
        bool is_class = class_name.size() > 5 && class_name.compare(0, 5, "class") == 0;
        for (size_t i = 5; is_class && i < class_name.size(); ++i) is_class = class_name[i] >= '0' && class_name[i] <= '9';
        std::error_code dir_ec;
        if (!is_class || !top->is_directory(dir_ec) || dir_ec) continue;
        std::error_code walk_ec;
        for (fs::recursive_directory_iterator it(top->path(), walk_ec), walk_end; !walk_ec && it != walk_end; it.increment(walk_ec))
        {
            std::error_code file_ec;
            if (!it->is_regular_file(file_ec) || file_ec) continue;
            const std::string name = fsyspath(it->path().filename()).string();
            if (name.size() < 5 || name.compare(name.size() - 5, 5, ".glsl") != 0) continue;
            // generic_u8string: '/' separators on every platform (C++20 [fs.path.generic.obs])
            const auto rel = it->path().lexically_relative(top->path()).generic_u8string();
            files.emplace_back(class_name + "/" + std::string(rel.begin(), rel.end()), it->path());
        }
        if (walk_ec) return std::string();
    }
    if (ec || files.empty()) return std::string();
    std::sort(files.begin(), files.end());   // byte order = Python's sorted() on these ASCII paths
    LLMD5 md5;
    const uint8_t nul = 0;
    std::vector<char> bytes;
    for (const auto& f : files)
    {
        md5.update(reinterpret_cast<const uint8_t*>(f.first.data()), f.first.size());
        md5.update(&nul, 1);
        std::ifstream in(f.second, std::ios::binary | std::ios::ate);
        if (!in.is_open()) return std::string();
        const std::streamsize size = in.tellg();
        bytes.resize(size > 0 ? static_cast<size_t>(size) : 0u);
        in.seekg(0);
        if (size > 0 && !in.read(bytes.data(), size)) return std::string();
        if (!bytes.empty()) md5.update(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
        md5.update(&nul, 1);
    }
    md5.finalize();
    char hex[MD5HEX_STR_SIZE];
    md5.hex_digest(hex);
    return std::string(hex);
}

// [STORE STAMP 2026-11-26] wolfvkshader.h checkStore
bool WolfVkShaderMirror::checkStore(const std::string& spv_dir, std::string& line)
{
    static std::map<std::string, std::pair<bool, std::string>> sChecked;   // spv_dir -> (current, line); main thread only
    auto known = sChecked.find(spv_dir);
    if (known != sChecked.end())
    {
        line = known->second.second;
        return known->second.first;
    }
    // stamp.json is Python's json.dump(..., indent=1, sort_keys=True) of flat string values: "name": "value"
    std::string text;
    {
        llifstream in(spv_dir + gDirUtilp->getDirDelimiter() + "stamp.json", std::ios::binary);
        if (in.is_open())
        {
            std::stringstream ss;
            ss << in.rdbuf();
            text = ss.str();
        }
    }
    auto value_of = [&text](const std::string& name) -> std::string
    {
        const std::string tag = "\"" + name + "\": \"";
        const size_t at = text.find(tag);
        if (at == std::string::npos) return std::string();
        const size_t end = text.find('"', at + tag.size());
        return end == std::string::npos ? std::string() : text.substr(at + tag.size(), end - at - tag.size());
    };
    const std::string want_transform = WOLFVK_TRANSFORM_MD5;
    // the store sits in app_settings/shaders/spv: its parent is the shader tree the viewer loads
    const std::string shaders_dir = fsyspath(fsyspath(spv_dir).parent_path()).string();
    const std::string want_shaders = shaderTreeMD5(shaders_dir);
    bool current = false;
    if (text.empty())
    {
        line = "SPIR-V store " + spv_dir + " has NO stamp.json: it was not made by this build's shader step, and no program is loaded from it";
    }
    else if (want_transform.empty() || want_shaders.empty())
    {
        line = "SPIR-V store " + spv_dir + " cannot be checked (" + (want_transform.empty() ? "this viewer was built without the transform stamp" : "the shader tree " + shaders_dir + " could not be read")
             + "), and no program is loaded from it";
    }
    else if (value_of("transform") != want_transform || value_of("shaders") != want_shaders)
    {
        line = "SPIR-V store " + spv_dir + " is STALE: it was made by transform " + value_of("transform") + " from shader tree " + value_of("shaders")
             + ", this viewer needs transform " + want_transform + " and shader tree " + want_shaders + "; no program is loaded from it (rebuild: the wolfvk_shaders target)";
    }
    else
    {
        current = true;
        line = "SPIR-V store " + spv_dir + " is this viewer's (transform " + want_transform + ", shader tree " + want_shaders + ")";
    }
    sChecked[spv_dir] = std::make_pair(current, line);
    return current;
}

WolfVkShaderMirror* WolfVkShaderMirror::create(WolfVkDevice& device, LLGLSLShader* shader, const std::string& spv_dir, std::string& error)
{
    if (!shader)
    {
        error = "no shader";
        return nullptr;
    }
    {
        // [STORE STAMP 2026-11-26] never a program from a store of another commit (its keys or its SPIR-V are not ours)
        std::string store_line;
        if (!checkStore(spv_dir, store_line))
        {
            error = shader->mName + ": " + store_line;
            return nullptr;
        }
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
    bool has_geometry = false;
    for (const auto& file : shader->mShaderFiles)
    {
        const bool wolf_unit = file.first.compare(0, 7, "wolftk/") == 0;   // llglslshader.cpp createShader: wolftk units get 0 channels
        const int channels = wolf_unit ? 0 : shader->mFeatures.mIndexedTextureChannels;
        // [GEOMETRY 2026-11-26] the stage names of shader_check.py (VERT/GEOM/FRAG) and LLShaderMgr::loadShaderFile's
        // mWolfVkUnitRecipes (llshadermgr.cpp); a geometry file was named "frag" here before, so the key of the normal
        // debug programs (llviewershadermgr.cpp:4078) never matched the build's
        const bool geometry = file.second == GL_GEOMETRY_SHADER;
        has_geometry = has_geometry || geometry;
        const std::string stage = (file.second == GL_VERTEX_SHADER) ? "vert" : (geometry ? "geom" : "frag");
        int found = 0;
        if (!mgr->wolfVkFindShaderClass(file.first, shader->mShaderLevel, found))
        {
            error = shader->mName + ": shader file " + file.first + " not found below class " + std::to_string(shader->mShaderLevel);
            return nullptr;
        }
        // [SPV KEY 2026-11-26] the defines the file uses (the build's shader_check.py recipe_defines)
        const std::set<std::string>* words = mgr->wolfVkFileWords(file.first, found);
        if (!words)
        {
            error = shader->mName + ": shader file " + file.first + " (class " + std::to_string(found) + ") could not be read for its recipe";
            return nullptr;
        }
        const std::map<std::string, std::string> used = usedDefines(*words, shader->mDefines);
        recipes.push_back(unitRecipe(stage, file.first, found, versionLine(geometry), &used, channels, amd, old_select));
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
    const std::string geom_path = spv_dir + gDirUtilp->getDirDelimiter() + key + ".geom.spv";
    std::vector<uint32_t> vert, geom, frag;
    if (!readSpv(vert_path, vert) || !readSpv(frag_path, frag) || (has_geometry && !readSpv(geom_path, geom)))
    {
        error = "no SPIR-V for \"" + shader->mName + "\" (key " + key + ", " + (has_geometry ? geom_path : vert_path) + "); recipe:";
        for (const std::string& r : recipes) error += "\n    " + r;
        return nullptr;
    }
    WolfVkShaderMirror* m = new WolfVkShaderMirror();
    m->mDevice = &device;
    m->mKey = key;
    m->mName = shader->mName;
    if (!m->mProgram.create(device, shader->mName, vert, geom, frag, error))
    {
        delete m;
        return nullptr;
    }
    // [SPEC CONSTANTS 2026-11-26] the value GL's text has for each specialization-constant define: the program's own
    // define, else the shared objects' (LLGLSLShader::sGlobalDefines = the attribs loadBasicShaders stitched them with,
    // llviewershadermgr.cpp:1209). The text ("%.2f", so '.' decimal) is read as a double in the classic locale (the
    // viewer's locale may use a decimal comma; strtod would follow it) and narrowed to float, as glslang reads the literal
    // the build puts in as the default.
    for (const auto* table : { &shader->mDefines, &LLGLSLShader::sGlobalDefines })
    {
        for (const auto& kv : *table)
        {
            uint32_t spec_id = 0;
            if (!specConstant(kv.first, spec_id) || m->mProgram.hasSpecConstant(spec_id)) continue;
            std::istringstream in(kv.second);
            in.imbue(std::locale::classic());
            double value = 0.0;
            in >> value;
            if (in.fail())
            {
                error = shader->mName + ": specialization-constant define " + kv.first + " = \"" + kv.second + "\" is not a number";
                delete m;
                return nullptr;
            }
            m->mProgram.setSpecConstant(spec_id, static_cast<float>(value));
        }
    }
    // the uniform images and the name table
    const WolfVkProgram::Stage* stages[2] = { &m->mProgram.vertex(), &m->mProgram.fragment() };
    std::vector<uint8_t>* images[2] = { &m->mVertImage, &m->mFragImage };
    for (int s = 0; s < 2; ++s)
    {
        const WolfVkSpirv::UniformBlock* u = stages[s]->mUniforms ? stages[s]->mUniforms : stages[s]->mPushConstants;
        if (!u) continue;
        images[s]->assign(u->mSize, 0);
        for (const WolfVkSpirv::BlockMember& member : u->mMembers)
        {
            m->mMembers[member.mName].push_back({ &member, images[s] });
        }
    }
    {
        // [POINT SIZE 2026-11-26] glPointSize's initial value (wolfvkshader.h POINT_SIZE_UNIFORM); the images start at zero
        const float one = 1.f;
        m->set(POINT_SIZE_UNIFORM, Kind::F1, 1, &one);
    }
    m->rebuildResolvedSamplers();
    LL_INFOS("Vulkan") << "Shader mirror for \"" << shader->mName << "\": key " << key << ", " << m->mMembers.size() << " uniforms, "
                       << m->mProgram.samplers().size() << " samplers declared, " << m->mProgram.samplerSlots().size() << " used" << LL_ENDL;
    return m;
}

WolfVkShaderMirror::~WolfVkShaderMirror()
{
    mProgram.destroy();
}

void WolfVkShaderMirror::syncFromGL(uint32_t gl_program)
{
    if (!gl_program) return;
    GLint active = 0, max_len = 0;
    glGetProgramiv(gl_program, GL_ACTIVE_UNIFORMS, &active);
    glGetProgramiv(gl_program, GL_ACTIVE_UNIFORM_MAX_LENGTH, &max_len);
    if (active <= 0 || max_len <= 0) return;
    std::vector<char> buf(static_cast<size_t>(max_len) + 1, 0);
    std::vector<uint32_t> words;
    for (GLint i = 0; i < active; ++i)
    {
        GLint size = 0;
        GLenum type = 0;
        GLsizei len = 0;
        glGetActiveUniform(gl_program, static_cast<GLuint>(i), max_len, &len, &size, &type, buf.data());
        if (len <= 0 || size <= 0) continue;
        std::string name(buf.data(), static_cast<size_t>(len));
        if (name.compare(0, 3, "gl_") == 0) continue;
        // an array is reported as "name[0]" (OpenGL 4.6 7.3.1.1 "Naming Active Resources"); the block member is "name"
        const bool array = name.size() > 3 && name.compare(name.size() - 3, 3, "[0]") == 0;
        const std::string base = array ? name.substr(0, name.size() - 3) : name;
        Kind kind = Kind::F1;
        uint32_t comps = 1;
        bool integer = false;
        switch (type)
        {
        case GL_FLOAT: kind = Kind::F1; comps = 1; break;
        case GL_FLOAT_VEC2: kind = Kind::F2; comps = 2; break;
        case GL_FLOAT_VEC3: kind = Kind::F3; comps = 3; break;
        case GL_FLOAT_VEC4: kind = Kind::F4; comps = 4; break;
        case GL_INT: case GL_BOOL: kind = Kind::I1; comps = 1; integer = true; break;
        case GL_INT_VEC2: case GL_BOOL_VEC2: kind = Kind::I2; comps = 2; integer = true; break;
        case GL_INT_VEC4: case GL_BOOL_VEC4: kind = Kind::I4; comps = 4; integer = true; break;
        case GL_UNSIGNED_INT_VEC4: kind = Kind::U4; comps = 4; integer = true; break;
        case GL_FLOAT_MAT2: kind = Kind::M2; comps = 4; break;
        case GL_FLOAT_MAT3: kind = Kind::M3; comps = 9; break;
        case GL_FLOAT_MAT3x4: kind = Kind::M3x4; comps = 12; break;
        case GL_FLOAT_MAT4: kind = Kind::M4; comps = 16; break;
        case GL_SAMPLER_1D: case GL_SAMPLER_2D: case GL_SAMPLER_3D: case GL_SAMPLER_CUBE: case GL_SAMPLER_1D_SHADOW: case GL_SAMPLER_2D_SHADOW:
        case GL_SAMPLER_2D_RECT: case GL_SAMPLER_2D_RECT_SHADOW: case GL_SAMPLER_2D_ARRAY: case GL_SAMPLER_2D_ARRAY_SHADOW:
        case GL_SAMPLER_CUBE_MAP_ARRAY: case GL_SAMPLER_2D_MULTISAMPLE:
            kind = Kind::I1; comps = 1; integer = true; break;   // a sampler's value is its texture unit
        default:
            continue;   // a type no LLGLSLShader setter writes either
        }
        words.assign(static_cast<size_t>(size) * comps, 0u);
        bool any = false;
        for (GLint e = 0; e < size; ++e)
        {
            const std::string element = array ? base + "[" + std::to_string(e) + "]" : name;
            const GLint loc = glGetUniformLocation(gl_program, element.c_str());
            if (loc < 0) continue;   // a member of a uniform block (no location), or an element the compiler dropped
            if (integer) glGetUniformiv(gl_program, loc, reinterpret_cast<GLint*>(words.data() + static_cast<size_t>(e) * comps));
            else glGetUniformfv(gl_program, loc, reinterpret_cast<GLfloat*>(words.data() + static_cast<size_t>(e) * comps));
            any = true;
        }
        if (any) set(base, kind, static_cast<uint32_t>(size), words.data(), false);
    }
}

int32_t WolfVkShaderMirror::samplerChannel(const std::string& sampler_name) const
{
    auto it = mSamplerChannels.find(sampler_name);
    return it == mSamplerChannels.end() ? -1 : it->second;
}

void WolfVkShaderMirror::rebuildResolvedSamplers()
{
    mResolvedSamplers.clear();
    mMaxSamplerBinding = 0;
    for (const WolfVkProgram::SamplerSlot& slot : mProgram.samplerSlots())
    {
        mMaxSamplerBinding = std::max(mMaxSamplerBinding, slot.mBinding + slot.mCount);
    }
    mResolvedSamplers.reserve(mProgram.samplerSlots().size());
    for (const WolfVkSpirv::Sampler& samp : mProgram.samplers())
    {
        if (!mProgram.samplerInLayout(samp.mBinding)) continue;
        ResolvedSampler rs{};
        rs.mBinding = samp.mBinding;
        rs.mArraySize = std::max<uint32_t>(1u, samp.mArraySize);
        rs.mDim = samp.mDim;
        rs.mArrayed = samp.mArrayed;
        rs.mShadow = samp.mShadow;
        auto it = mSamplerChannels.find(samp.mName);
        rs.mChannel = (it != mSamplerChannels.end() && it->second >= 0) ? it->second : 0;
        mResolvedSamplers.push_back(rs);
    }
}

// [W114-VULKAN S4 2026-11-26] wolfvkshader.h vertexMember
const WolfVkSpirv::BlockMember* WolfVkShaderMirror::vertexMember(const std::string& name) const
{
    auto it = mMembers.find(name);
    if (it == mMembers.end()) return nullptr;
    for (const Member& m : it->second)
    {
        if (m.mImage == &mVertImage) return m.mMember;
    }
    return nullptr;
}

void WolfVkShaderMirror::applyProjectionFix(const std::string& name, float* columns) const
{
    // modelview_projection_matrix and projection_matrix (llshadermgr.cpp mReservedUniforms "projection_matrix",
    // "modelview_projection_matrix"): M' = C * M with C = [1 0 0 0; 0 1 0 0; 0 0 .5 .5; 0 0 0 1] (wolfvkshader.h).
    // Column-major: element (row r, column c) is columns[c*4 + r]. Only row 2 changes: r2' = 0.5*r2 + 0.5*r3.
    if (name != "modelview_projection_matrix" && name != "projection_matrix") return;
    // [GL DEPTH 2026-11-08] with VK_EXT_depth_clip_control every pipeline takes GL's z range (wolfvkpipeline.cpp), so GL's
    // matrices go through unchanged and the depth is made by the viewport transform as in GL (docs 8.2n)
    if (mDevice && mDevice->glDepthRange()) return;
    for (int c = 0; c < 4; ++c)
    {
        columns[c * 4 + 2] = 0.5f * columns[c * 4 + 2] + 0.5f * columns[c * 4 + 3];
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
    if (const WolfCoreApi* wc = WolfCoreLoader::api())
    {
        uint32_t wc_kind = WC_UNIFORM_F1;
        switch (kind)
        {
        case Kind::F1: wc_kind = WC_UNIFORM_F1; break;
        case Kind::F2: wc_kind = WC_UNIFORM_F2; break;
        case Kind::F3: wc_kind = WC_UNIFORM_F3; break;
        case Kind::F4: wc_kind = WC_UNIFORM_F4; break;
        case Kind::I1: wc_kind = WC_UNIFORM_I1; break;
        case Kind::I2: wc_kind = WC_UNIFORM_I2; break;
        case Kind::I4: wc_kind = WC_UNIFORM_I4; break;
        case Kind::U4: wc_kind = WC_UNIFORM_U4; break;
        case Kind::M2: wc_kind = WC_UNIFORM_M2; break;
        case Kind::M3: wc_kind = WC_UNIFORM_M3; break;
        case Kind::M3x4: wc_kind = WC_UNIFORM_M3X4; break;
        case Kind::M4: wc_kind = WC_UNIFORM_M4; break;
        }
        WolfCoreMemberLayout layout{};
        layout.columns       = bm.mColumns;
        layout.rows          = bm.mRows;
        layout.array_size    = bm.mArraySize;
        layout.offset        = bm.mOffset;
        layout.array_stride  = bm.mArrayStride;
        layout.matrix_stride = bm.mMatrixStride;
        layout.row_major     = bm.mRowMajor ? 1u : 0u;
        const int32_t changed = wc->pack_uniform_member(
            image.data(), image.size(), &layout, wc_kind, count, data, transpose ? 1u : 0u);
        if (changed > 0)
        {
            mDirty[m.mImage == &mVertImage ? 0 : 1] = true;
        }
        return;
    }
    // Open-source LGPL fallback when WolfCore is not installed.
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
            // [S2 2026-11-26] only a change makes the stage image dirty (it is pushed into the ring again at the next draw)
            if (memcmp(image.data() + base, elem, rows * scalar) != 0)
            {
                memcpy(image.data() + base, elem, rows * scalar);
                mDirty[m.mImage == &mVertImage ? 0 : 1] = true;
            }
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
                    if (memcmp(image.data() + dst, elem + src_index * scalar, scalar) != 0)   // [S2] as above
                    {
                        memcpy(image.data() + dst, elem + src_index * scalar, scalar);
                        mDirty[m.mImage == &mVertImage ? 0 : 1] = true;
                    }
                }
            }
        }
    }
}

void WolfVkShaderMirror::setMembers(const std::string& name, const std::vector<Member>& members, Kind kind, uint32_t count, const void* data, bool transpose, bool projection)
{
    if (projection && kind == Kind::M4 && (!mDevice || !mDevice->glDepthRange() || transpose))
    {
        // modelview_projection_matrix / projection_matrix: corrected for Vulkan's depth range when VK_EXT_depth_clip_control is absent
        float fixed[16];
        const float* in = static_cast<const float*>(data);
        for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) fixed[c * 4 + r] = transpose ? in[r * 4 + c] : in[c * 4 + r];
        applyProjectionFix(name, fixed);
        for (const Member& m : members) writeMember(m, kind, 1, fixed, false);
        return;
    }
    for (const Member& m : members)
    {
        writeMember(m, kind, count, data, transpose);
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
            const int32_t ch = *static_cast<const int32_t*>(data);
            auto cit = mSamplerChannels.find(name);
            if (cit == mSamplerChannels.end() || cit->second != ch)
            {
                mSamplerChannels[name] = ch;
                rebuildResolvedSamplers();
            }
        }
        return;
    }
    setMembers(name, it->second, kind, count, data, transpose, name == "modelview_projection_matrix" || name == "projection_matrix");
}

void WolfVkShaderMirror::setReserved(uint32_t index, const std::string& name, Kind kind, uint32_t count, const void* data, bool transpose)
{
    if (!data || count == 0) return;
    if (index >= mReserved.size()) mReserved.resize(index + 1);
    Reserved& r = mReserved[index];
    if (!r.mResolved)
    {
        // once per index: the same lookups set() makes by name
        auto it = mMembers.find(name);
        r.mMembers = it == mMembers.end() ? nullptr : &it->second;
        r.mSampler = mProgram.samplerBinding(name) >= 0;
        r.mProjection = name == "modelview_projection_matrix" || name == "projection_matrix";
        r.mResolved = true;
    }
    if (!r.mMembers)
    {
        if (kind == Kind::I1 && r.mSampler)
        {
            const int32_t ch = *static_cast<const int32_t*>(data);
            auto cit = mSamplerChannels.find(name);
            if (cit == mSamplerChannels.end() || cit->second != ch)
            {
                mSamplerChannels[name] = ch;
                rebuildResolvedSamplers();
            }
        }
        return;
    }
    setMembers(name, *r.mMembers, kind, count, data, transpose, r.mProjection);
}

bool WolfVkShaderMirror::prepareUniforms(WolfVkFrameUniforms& ring, const uint8_t*& out_push_data, uint32_t& out_push_size,
                                         VkDescriptorSet& out_set0, uint32_t out_offsets[2], uint32_t& out_offset_count)
{
    out_push_data = nullptr;
    out_push_size = 0;
    out_set0 = VK_NULL_HANDLE;
    out_offsets[0] = out_offsets[1] = 0;
    out_offset_count = 0;

    const WolfVkProgram::Stage* stages[2] = { &mProgram.vertex(), &mProgram.fragment() };
    const std::vector<uint8_t>* images[2] = { &mVertImage, &mFragImage };
    const uint32_t bindings[2] = { WolfVkProgram::BINDING_UNIFORMS_VERT, WolfVkProgram::BINDING_UNIFORMS_FRAG };
    const uint64_t epoch = ring.epoch();

    if (mPushedEpoch != epoch)
    {
        mDirty[0] = mDirty[1] = true;   // last frame's ring offsets mean nothing in this one
        mPushedEpoch = epoch;
    }

    if (stages[0]->mPushConstants && !mVertImage.empty())
    {
        out_push_data = mVertImage.data();
        out_push_size = static_cast<uint32_t>(mVertImage.size());
    }

    if (!stages[0]->mUniforms && !stages[1]->mUniforms)
    {
        return true;
    }

    if (mUniformSetEpoch != epoch)
    {
        mUniformSet = ring.allocate(mProgram.uniformSetLayout());
        if (mUniformSet == VK_NULL_HANDLE) return false;
        VkDescriptorBufferInfo infos[2]{};
        VkWriteDescriptorSet writes[2]{};
        uint32_t n = 0;
        for (int s = 0; s < 2; ++s)
        {
            if (!stages[s]->mUniforms || images[s]->empty()) continue;
            infos[n] = { ring.buffer(), 0, images[s]->size() };
            writes[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[n].dstSet = mUniformSet;
            writes[n].dstBinding = bindings[s];
            writes[n].descriptorCount = 1;
            writes[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            writes[n].pBufferInfo = &infos[n];
            n++;
        }
        if (n) gVk.vkUpdateDescriptorSets(mDevice->device(), n, writes, 0, nullptr);
        mUniformSetEpoch = epoch;
    }

    uint32_t n = 0;
    for (int s = 0; s < 2; ++s)
    {
        if (!stages[s]->mUniforms || images[s]->empty()) continue;
        if (mDirty[s])
        {
            VkDeviceSize offset = 0;
            if (!ring.push(images[s]->data(), images[s]->size(), offset)) return false;
            mPushedOffset[s] = offset;
            mDirty[s] = false;
        }
        out_offsets[n++] = static_cast<uint32_t>(mPushedOffset[s]);
    }
    out_set0 = mUniformSet;
    out_offset_count = n;
    return true;
}

bool WolfVkShaderMirror::bindUniforms(VkCommandBuffer cmd, WolfVkFrameUniforms& ring)
{
    const uint8_t* push_data = nullptr;
    uint32_t push_size = 0;
    VkDescriptorSet set0 = VK_NULL_HANDLE;
    uint32_t dynamic_offsets[2] = { 0, 0 };
    uint32_t n = 0;
    if (!prepareUniforms(ring, push_data, push_size, set0, dynamic_offsets, n))
    {
        return false;
    }
    if (push_data && push_size > 0)
    {
        const bool layout_changed = ring.needPushConstants(cmd, mProgram.pipelineLayout());
        if (mDirty[0] || layout_changed)
        {
            gVk.vkCmdPushConstants(cmd, mProgram.pipelineLayout(), VK_SHADER_STAGE_VERTEX_BIT, 0, push_size, push_data);
            mDirty[0] = false;
        }
    }
    if (set0 != VK_NULL_HANDLE && ring.needBind(cmd, WolfVkProgram::SET_UNIFORMS, mProgram.pipelineLayout(), set0, dynamic_offsets, n))
    {
        gVk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mProgram.pipelineLayout(), WolfVkProgram::SET_UNIFORMS, 1, &set0, n, dynamic_offsets);
    }
    return true;
}

bool WolfVkShaderMirror::prepareSamplers(WolfVkFrameUniforms& ring, const VkDescriptorImageInfo* by_binding, uint32_t binding_count,
                                         const VkDescriptorImageInfo& fallback, VkDescriptorSet& out_set1)
{
    out_set1 = VK_NULL_HANDLE;
    const std::vector<WolfVkProgram::SamplerSlot>& slots = mProgram.samplerSlots();
    if (slots.empty()) return true;   // nothing in set 1 is read by any stage: no set needs to be bound for it
    const uint64_t epoch = ring.epoch();
    if (mSamplerSetsEpoch != epoch)
    {
        mSamplerSets.clear();   // the pool they came from was reset with the frame slot
        mSamplerSetsEpoch = epoch;
        mLastSamplerEpoch = UINT64_MAX;
    }

    VkDescriptorImageInfo stack_infos[64];
    uint32_t total = 0;
    for (const WolfVkProgram::SamplerSlot& smp : slots) total += smp.mCount;
    VkDescriptorImageInfo* info_ptr = stack_infos;
    if (total > 64)
    {
        mSamplerInfos.resize(total);
        info_ptr = mSamplerInfos.data();
    }
    uint32_t idx = 0;
    for (const WolfVkProgram::SamplerSlot& smp : slots)
    {
        for (uint32_t i = 0; i < smp.mCount; ++i)
        {
            const uint32_t b = smp.mBinding + i;
            info_ptr[idx++] = (by_binding && b < binding_count && by_binding[b].imageView != VK_NULL_HANDLE) ? by_binding[b] : fallback;
        }
    }

    VkDescriptorSet set = VK_NULL_HANDLE;
    if (total <= 64 && mLastSamplerEpoch == epoch && mLastSamplerCount == total &&
        memcmp(info_ptr, mLastSamplerInfos, total * sizeof(VkDescriptorImageInfo)) == 0)
    {
        set = mLastSamplerSet;
    }
    else
    {
        mSamplerKey.assign(reinterpret_cast<const char*>(info_ptr), total * sizeof(VkDescriptorImageInfo));
        auto found = mSamplerSets.find(mSamplerKey);
        if (found != mSamplerSets.end())
        {
            set = found->second;
        }
        else
        {
            set = ring.allocate(mProgram.samplerSetLayout());
            if (set == VK_NULL_HANDLE) return false;
            VkWriteDescriptorSet stack_writes[32];
            std::vector<VkWriteDescriptorSet> heap_writes;
            VkWriteDescriptorSet* write_ptr = stack_writes;
            if (slots.size() > 32)
            {
                heap_writes.resize(slots.size());
                write_ptr = heap_writes.data();
            }
            size_t at = 0;
            for (size_t s = 0; s < slots.size(); ++s)
            {
                const WolfVkProgram::SamplerSlot& smp = slots[s];
                VkWriteDescriptorSet w{};
                w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w.dstSet = set;
                w.dstBinding = smp.mBinding;
                w.descriptorCount = smp.mCount;
                w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w.pImageInfo = info_ptr + at;
                at += smp.mCount;
                write_ptr[s] = w;
            }
            gVk.vkUpdateDescriptorSets(mDevice->device(), static_cast<uint32_t>(slots.size()), write_ptr, 0, nullptr);
            mSamplerSets.emplace(mSamplerKey, set);
        }
        if (total <= 64)
        {
            memcpy(mLastSamplerInfos, info_ptr, total * sizeof(VkDescriptorImageInfo));
            mLastSamplerCount = total;
            mLastSamplerEpoch = epoch;
            mLastSamplerSet = set;
        }
    }
    out_set1 = set;
    return true;
}

bool WolfVkShaderMirror::bindSamplers(VkCommandBuffer cmd, WolfVkFrameUniforms& ring, const std::vector<VkDescriptorImageInfo>& by_binding, const VkDescriptorImageInfo& fallback)
{
    return bindSamplers(cmd, ring, by_binding.data(), static_cast<uint32_t>(by_binding.size()), fallback);
}

bool WolfVkShaderMirror::bindSamplers(VkCommandBuffer cmd, WolfVkFrameUniforms& ring, const VkDescriptorImageInfo* by_binding, uint32_t binding_count, const VkDescriptorImageInfo& fallback)
{
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (!prepareSamplers(ring, by_binding, binding_count, fallback, set))
    {
        return false;
    }
    if (set != VK_NULL_HANDLE && ring.needBind(cmd, WolfVkProgram::SET_SAMPLERS, mProgram.pipelineLayout(), set, nullptr, 0))
    {
        gVk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mProgram.pipelineLayout(), WolfVkProgram::SET_SAMPLERS, 1, &set, 0, nullptr);
    }
    return true;
}

bool WolfVkShaderMirror::prepareBlocks(WolfVkFrameUniforms& ring, const uint32_t* offsets, uint32_t count,
                                       VkDescriptorSet& out_set2, uint32_t out_sorted[8], uint32_t& out_sorted_count)
{
    out_set2 = VK_NULL_HANDLE;
    out_sorted_count = 0;
    const std::vector<WolfVkProgram::BlockBinding>& blocks = mProgram.blocks();
    if (blocks.empty()) return true;
    if (!offsets || count != blocks.size() || blocks.size() > 8) return false;
    const uint64_t epoch = ring.epoch();
    if (mBlockSetEpoch != epoch)
    {
        mBlockSet = ring.allocate(mProgram.blockSetLayout());
        if (mBlockSet == VK_NULL_HANDLE) return false;
        VkDescriptorBufferInfo infos[8]{};
        VkWriteDescriptorSet writes[8]{};
        for (size_t i = 0; i < blocks.size(); ++i)
        {
            infos[i] = { ring.buffer(), 0, blocks[i].mSize };
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = mBlockSet;
            writes[i].dstBinding = blocks[i].mBinding;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            writes[i].pBufferInfo = &infos[i];
        }
        gVk.vkUpdateDescriptorSets(mDevice->device(), static_cast<uint32_t>(blocks.size()), writes, 0, nullptr);
        mBlockSetEpoch = epoch;
    }
    uint32_t order[8];
    const uint32_t n = static_cast<uint32_t>(blocks.size());
    for (uint32_t i = 0; i < n; ++i) order[i] = i;
    if (n > 1)
    {
        std::sort(order, order + n, [&](uint32_t a, uint32_t b) { return blocks[a].mBinding < blocks[b].mBinding; });
    }
    for (uint32_t i = 0; i < n; ++i) out_sorted[i] = offsets[order[i]];
    out_set2 = mBlockSet;
    out_sorted_count = n;
    return true;
}

bool WolfVkShaderMirror::bindBlocks(VkCommandBuffer cmd, WolfVkFrameUniforms& ring, const std::vector<uint32_t>& offsets)
{
    return bindBlocks(cmd, ring, offsets.data(), static_cast<uint32_t>(offsets.size()));
}

bool WolfVkShaderMirror::bindBlocks(VkCommandBuffer cmd, WolfVkFrameUniforms& ring, const uint32_t* offsets, uint32_t count)
{
    VkDescriptorSet set2 = VK_NULL_HANDLE;
    uint32_t sorted[8] = {};
    uint32_t n = 0;
    if (!prepareBlocks(ring, offsets, count, set2, sorted, n))
    {
        return false;
    }
    if (set2 != VK_NULL_HANDLE && ring.needBind(cmd, WolfVkProgram::SET_BLOCKS, mProgram.pipelineLayout(), set2, sorted, n))
    {
        gVk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mProgram.pipelineLayout(), WolfVkProgram::SET_BLOCKS, 1, &set2, n, sorted);
    }
    return true;
}
