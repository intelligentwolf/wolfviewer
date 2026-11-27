/**
 * @file wolfvknative.cpp
 * @brief Native Vulkan renderer implementation (Milestone 1: GL-free window, shaders, buffers, textures, targets, UI/login).
 *
 * [NATIVE VULKAN 2026-11-26]
 * See docs/NATIVE_VULKAN_RENDERER_DESIGN.md.
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

#include "llviewerprecompiledheaders.h"
#include "wolfvknative.h"

#include "llcontrol.h"
#include "lldir.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llimagegl.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llshadermgr.h"
#include "llvertexbuffer.h"
#include "llviewercontrol.h"
#include "llwindow.h"

#include "vk/wolfvkbuffer.h"
#include "vk/wolfvkdevice.h"
#include "vk/wolfvkimage.h"
#include "vk/wolfvkloader.h"
#include "vk/wolfvkpipeline.h"
#include "vk/wolfvkrendertarget.h"
#include "vk/wolfvkshader.h"
#include "vk/wolfvkstate.h"
#include "vk/wolfvksurface.h"
#include "vk/wolfvkswapchain.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern LLControlGroup gSavedSettings;
extern S32 gGLViewport[4];

namespace
{
    // -------------------------------------------------------------------------
    // Native Vulkan state
    // -------------------------------------------------------------------------
    bool sActive = false;
    LLWindow* sWindow = nullptr;
    WolfVkDevice sDevice;
    VkSurfaceKHR sSurface = VK_NULL_HANDLE;
    WolfVkSwapchain sSwapchain;
    WolfVkFrameUniforms sUniforms;
    WolfVkStagingRing sStagingRing;
    WolfVkPipelineCache sPipelines;
    WolfVkSamplerCache sSamplers;
    WolfVkImage sFallbackWhite;          // 2D
    WolfVkImage sFallbackCube;           // Cube (6 layers)
    WolfVkImage sFallback2DArray;        // 2D Array (1 layer)
    WolfVkImage sFallback3D;             // 3D (1x1x1)
    WolfVkImage sFallbackCubeArray;      // Cube Array (6 layers)
    WolfVkRenderTarget sWindowTarget;

    std::string sSpvDir;
    std::string sVulkanLine;

    bool sResizePending = false;
    bool sFrameOpen = false;
    uint32_t sSwapImageIndex = 0;
    VkCommandBuffer sFrameCmd = VK_NULL_HANDLE;

    // Currently active render pass target (either sWindowTarget or a bound LLRenderTarget's WolfVkRenderTarget)
    WolfVkRenderTarget* sActiveTarget = nullptr;
    bool sPendingClearColor = true;
    bool sPendingClearDepth = true;
    float sPendingClearRGBA[4] = { 0.f, 0.f, 0.f, 1.f };
    float sPendingClearDepthVal = 1.0f;

    // Per-render-pass command buffer state deduplication
    struct PassCmdState
    {
        VkPipeline mPipeline = VK_NULL_HANDLE;
        VkViewport mViewport{};
        bool mHasViewport = false;
        VkRect2D mScissor{};
        bool mHasScissor = false;
        VkBuffer mVBufs[LLVertexBuffer::TYPE_MAX] = {};
        VkDeviceSize mVOffs[LLVertexBuffer::TYPE_MAX] = {};
        uint32_t mNumVBufs = 0;
        VkBuffer mIBuf = VK_NULL_HANDLE;
        VkIndexType mIType = VK_INDEX_TYPE_UINT16;
    };
    PassCmdState sPassState;
    void resetPassState()
    {
        sPassState = PassCmdState{};
    }

    U32 sDrawsThisFrame = 0;
    U32 sDrawsLastFrame = 0;
    std::unordered_set<std::string> sWarnedOnce;

    // Set-2 UBO shadow bytes with per-frame ring offset caching (so large blocks like ReflectionProbes push ONCE/frame!)
    struct UboEntry
    {
        std::vector<uint8_t> mBytes;
        uint64_t mPushedEpoch = UINT64_MAX;
        uint32_t mPushedSize = 0;
        VkDeviceSize mPushedOffset = 0;
    };
    std::unordered_map<GLuint, UboEntry> sUboBytes;
    GLuint sBoundUbo[LLGLSLShader::NUM_UNIFORM_BLOCKS] = {};
    // Cached zero-block per frame for unbound UBOs (64 KiB static zero buffer avoids per-draw heap alloc)
    alignas(16) const uint8_t sZeroUboBytes[65536] = {};
    uint64_t sZeroUboEpoch = UINT64_MAX;
    uint32_t sZeroUboSize = 0;
    VkDeviceSize sZeroUboOffset = 0;

    void onUniformBufferData(GLuint buffer, U32 offset, U32 size, const void* data, bool respecify)
    {
        if (buffer == 0) return;
        auto& entry = sUboBytes[buffer];
        auto& vec = entry.mBytes;
        if (respecify)
        {
            vec.assign(offset + size, 0);
        }
        else if (vec.size() < offset + size)
        {
            vec.resize(offset + size, 0);
        }
        if (data && size > 0)
        {
            memcpy(vec.data() + offset, data, size);
        }
        entry.mPushedEpoch = UINT64_MAX; // dirty: re-push once on next draw
    }

    void onUniformBufferBind(GLuint point, GLuint buffer)
    {
        if (point < LLGLSLShader::NUM_UNIFORM_BLOCKS)
        {
            sBoundUbo[point] = buffer;
        }
    }

    WolfVkImage::Kind expectedImageKind(uint32_t dim, bool arrayed)
    {
        if (dim == 2) return WolfVkImage::Kind::TEXTURE_3D;
        if (dim == 3) return arrayed ? WolfVkImage::Kind::TEXTURE_CUBE_ARRAY : WolfVkImage::Kind::TEXTURE_CUBE;
        return arrayed ? WolfVkImage::Kind::TEXTURE_2D_ARRAY : WolfVkImage::Kind::TEXTURE_2D;
    }

    WolfVkImage* fallbackForKind(WolfVkImage::Kind kind)
    {
        switch (kind)
        {
        case WolfVkImage::Kind::TEXTURE_CUBE:       return &sFallbackCube;
        case WolfVkImage::Kind::TEXTURE_2D_ARRAY:   return &sFallback2DArray;
        case WolfVkImage::Kind::TEXTURE_3D:         return &sFallback3D;
        case WolfVkImage::Kind::TEXTURE_CUBE_ARRAY: return sFallbackCubeArray.isValid() ? &sFallbackCubeArray : &sFallbackCube;
        default:                                    return &sFallbackWhite;
        }
    }

    void warnOnce(const std::string& key, const std::string& msg)
    {
        if (sWarnedOnce.insert(key).second)
        {
            LL_WARNS("Vulkan") << "[Native] " << msg << LL_ENDL;
        }
    }

    // -------------------------------------------------------------------------
    // 1. Pure-Vulkan Vertex/Index Buffer Store with Recycled Pool + GDDR7 VRAM Promotion
    // -------------------------------------------------------------------------
    struct NativeBufferEntry
    {
        std::unique_ptr<WolfVkBuffer> mBuf;
        VkBuffer mDeviceBuf = VK_NULL_HANDLE;
        WolfVkAllocation mDeviceAlloc{};
        VkDeviceSize mDeviceSize = 0;
        std::vector<uint8_t> mShadow;
        size_t mValidBytes = 0;
        uint64_t mAllocGen = 0;
        uint64_t mLastDrawnEpoch = UINT64_MAX;
        uint64_t mLastWriteEpoch = UINT64_MAX;
        uint32_t mWriteCount = 0;
        bool mIsIndex = false;
        bool mQueuedPromote = false;
    };

    class NativeBufferStore : public LLVertexBuffer::WolfVkStore
    {
    public:
        static VkDeviceSize bucketSize(VkDeviceSize want)
        {
            VkDeviceSize b = 256;
            while (b < want && b < (16ull * 1024ull * 1024ull))
            {
                b <<= 1;
            }
            return std::max<VkDeviceSize>(want, b);
        }

        std::unique_ptr<WolfVkBuffer> acquireBuffer(bool is_index, VkDeviceSize min_size)
        {
            const VkDeviceSize alloc_size = bucketSize(std::max<VkDeviceSize>(16, min_size));
            const uint64_t pool_key = (alloc_size << 1) | (is_index ? 1ull : 0ull);
            {
                std::lock_guard<std::mutex> lock(mMutex);
                auto pit = mRecyclePool.find(pool_key);
                if (pit != mRecyclePool.end() && !pit->second.empty())
                {
                    auto reused = std::move(pit->second.back());
                    pit->second.pop_back();
                    if (mPooledBytes >= reused->size()) mPooledBytes -= reused->size();
                    mBytesUsed += reused->size();
                    return reused;
                }
            }
            auto buf = std::make_unique<WolfVkBuffer>();
            // Include TRANSFER_SRC_BIT so settled geometry can DMA directly to DEVICE_LOCAL VRAM with zero CPU memcpy!
            const VkBufferUsageFlags usage = (is_index ? VK_BUFFER_USAGE_INDEX_BUFFER_BIT : VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)
                                           | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            std::string err;
            if (!buf->create(sDevice, alloc_size, usage, /*host_only=*/true, err))
            {
                LL_WARNS("Vulkan") << "[Native] buffer allocate (" << min_size << " B) failed: " << err << LL_ENDL;
                return nullptr;
            }
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mBytesUsed += alloc_size;
            }
            return buf;
        }

        void retireBuffer(std::unique_ptr<WolfVkBuffer> buf, bool is_index)
        {
            if (!buf || !buf->isValid()) return;
            const VkDeviceSize sz = buf->size();
            {
                std::lock_guard<std::mutex> lock(mMutex);
                if (mBytesUsed >= sz) mBytesUsed -= sz;
            }
            // Pool buffers up to 4 MiB once in-flight GPU frames finish reading them (zero vkCreateBuffer churn on dynamic VBOs!)
            static constexpr VkDeviceSize MAX_POOLED_TOTAL = 64ull * 1024ull * 1024ull;
            if (sActive && sDevice.hasDevice() && !sDevice.hung() && sz <= (4ull * 1024ull * 1024ull))
            {
                WolfVkBuffer* raw = buf.release();
                const uint64_t pool_key = (sz << 1) | (is_index ? 1ull : 0ull);
                sDevice.deferDestroy([this, raw, pool_key, sz]()
                {
                    std::unique_ptr<WolfVkBuffer> owned(raw);
                    if (!sActive || !sDevice.hasDevice() || sDevice.hung())
                    {
                        owned->destroy();
                        return;
                    }
                    std::lock_guard<std::mutex> lock(mMutex);
                    if (mPooledBytes + sz <= MAX_POOLED_TOTAL && mRecyclePool[pool_key].size() < 64)
                    {
                        mPooledBytes += sz;
                        mRecyclePool[pool_key].push_back(std::move(owned));
                        return;
                    }
                    owned->destroy();
                });
                return;
            }
            buf->destroy();
        }

        void retireDeviceBuffer(VkBuffer dev_buf, WolfVkAllocation dev_alloc, VkDeviceSize dev_sz)
        {
            if (dev_buf == VK_NULL_HANDLE) return;
            {
                std::lock_guard<std::mutex> lock(mMutex);
                if (mVramBytes >= dev_sz) mVramBytes -= dev_sz;
                if (mPromotedCount > 0) --mPromotedCount;
            }
            if (sDevice.hasDevice() && !sDevice.hung())
            {
                WolfVkDevice* dev = &sDevice;
                sDevice.deferDestroy([dev, dev_buf, dev_alloc]() mutable
                {
                    if (dev->hasDevice() && !dev->hung())
                    {
                        gVk.vkDestroyBuffer(dev->device(), dev_buf, nullptr);
                        dev->allocator().free(dev_alloc);
                    }
                });
            }
        }

        bool createDeviceLocalBuffer(bool is_index, VkDeviceSize size, VkBuffer& out_buf, WolfVkAllocation& out_alloc)
        {
            out_buf = VK_NULL_HANDLE;
            out_alloc = WolfVkAllocation{};
            if (!sDevice.hasDevice() || sDevice.hung() || size == 0) return false;

            VkBufferCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            info.size = size;
            info.usage = (is_index ? VK_BUFFER_USAGE_INDEX_BUFFER_BIT : VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)
                       | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (gVk.vkCreateBuffer(sDevice.device(), &info, nullptr, &out_buf) != VK_SUCCESS)
            {
                out_buf = VK_NULL_HANDLE;
                return false;
            }
            VkMemoryRequirements req{};
            gVk.vkGetBufferMemoryRequirements(sDevice.device(), out_buf, &req);
            std::string err;
            if (!sDevice.allocator().allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0,
                                              /*linear=*/true, /*dedicated=*/false,
                                              out_buf, VK_NULL_HANDLE, out_alloc, err))
            {
                gVk.vkDestroyBuffer(sDevice.device(), out_buf, nullptr);
                out_buf = VK_NULL_HANDLE;
                return false;
            }
            if (gVk.vkBindBufferMemory(sDevice.device(), out_buf, out_alloc.mMemory, out_alloc.mOffset) != VK_SUCCESS)
            {
                sDevice.allocator().free(out_alloc);
                gVk.vkDestroyBuffer(sDevice.device(), out_buf, nullptr);
                out_buf = VK_NULL_HANDLE;
                out_alloc = WolfVkAllocation{};
                return false;
            }
            return true;
        }

        bool allocate(bool is_index, U32 size, U32& gl_name, void*& handle) override
        {
            gl_name = 0;
            handle = nullptr;
            if (!sDevice.hasDevice() || size == 0) return false;

            auto entry = std::make_unique<NativeBufferEntry>();
            entry->mIsIndex = is_index;
            entry->mBuf = acquireBuffer(is_index, size);
            if (!entry->mBuf) return false;
            if (uint8_t* h = entry->mBuf->hostData())
            {
                memset(h, 0, size);
                entry->mBuf->markDirty(0, size);
            }
            entry->mShadow.assign(size, 0);
            entry->mValidBytes = 0;
            NativeBufferEntry* raw = entry.get();
            {
                std::lock_guard<std::mutex> lock(mMutex);
                raw->mAllocGen = ++mNextAllocGen;
                mLive[raw] = std::move(entry);
            }
            handle = raw;
            return true;
        }

        void write(void* handle, U32 offset, U32 size, const U8* data) override
        {
            if (!handle || !data || size == 0) return;
            auto* entry = static_cast<NativeBufferEntry*>(handle);
            const size_t end_off = static_cast<size_t>(offset) + static_cast<size_t>(size);
            if (entry->mShadow.size() < end_off)
            {
                entry->mShadow.resize(end_off, 0);
            }
            memcpy(entry->mShadow.data() + offset, data, size);
            entry->mValidBytes = std::max(entry->mValidBytes, end_off);

            const uint64_t cur_epoch = sUniforms.epoch();
            if (cur_epoch != entry->mLastWriteEpoch)
            {
                entry->mLastWriteEpoch = cur_epoch;
                if (entry->mWriteCount < 1000u) ++entry->mWriteCount;
            }

            // If this buffer was previously promoted to DEVICE_LOCAL VRAM and got modified on CPU, retire VRAM copy cleanly
            if (entry->mDeviceBuf != VK_NULL_HANDLE)
            {
                retireDeviceBuffer(entry->mDeviceBuf, entry->mDeviceAlloc, entry->mDeviceSize);
                entry->mDeviceBuf = VK_NULL_HANDLE;
                entry->mDeviceAlloc = WolfVkAllocation{};
                entry->mDeviceSize = 0;
            }

            auto maybeQueuePromote = [&]()
            {
                if (!entry->mQueuedPromote && entry->mValidBytes >= 256 && entry->mWriteCount <= 8)
                {
                    std::lock_guard<std::mutex> lock(mMutex);
                    if (!entry->mQueuedPromote)
                    {
                        entry->mQueuedPromote = true;
                        mPromoteQueue.emplace_back(handle, entry->mAllocGen);
                    }
                }
            };

            // Copy-on-write when this buffer was drawn in the current command buffer or an in-flight frame:
            // acquires a pre-warmed buffer from mRecyclePool in O(1) with zero vkCreateBuffer calls.
            const bool need_grow = !entry->mBuf || !entry->mBuf->isValid() || entry->mBuf->size() < end_off;
            const uint64_t fif = std::max<uint64_t>(2ull, sDevice.framesInFlight());
            const bool in_flight = (entry->mLastDrawnEpoch != UINT64_MAX && cur_epoch <= entry->mLastDrawnEpoch + fif);
            if (need_grow || in_flight)
            {
                const VkDeviceSize want_sz = std::max<VkDeviceSize>(end_off, entry->mShadow.size());
                auto new_buf = acquireBuffer(entry->mIsIndex, want_sz);
                if (new_buf)
                {
                    if (!entry->mShadow.empty())
                    {
                        new_buf->write(0, entry->mShadow.data(), entry->mShadow.size());
                    }
                    retireBuffer(std::move(entry->mBuf), entry->mIsIndex);
                    entry->mBuf = std::move(new_buf);
                    entry->mLastDrawnEpoch = UINT64_MAX;
                    maybeQueuePromote();
                    return;
                }
            }

            if (entry->mBuf && entry->mBuf->isValid())
            {
                entry->mBuf->write(offset, data, size);
                entry->mLastDrawnEpoch = UINT64_MAX;
            }
            maybeQueuePromote();
        }

        // Promote settled world/PBR/terrain/avatar buffers (>=256 B, untouched >=3 frames, <=8 writes) into GDDR7 VRAM
        // at the top of the frame command buffer (zero render-pass breaks, zero CPU memcpy).
        void promoteSettledToVRAM(VkCommandBuffer cmd, uint64_t cur_epoch)
        {
            if (cmd == VK_NULL_HANDLE || !sDevice.hasDevice() || sDevice.hung()) return;
            static constexpr VkDeviceSize MAX_PROMOTE_PER_FRAME = 32ull * 1024ull * 1024ull;
            VkDeviceSize promoted_bytes = 0;
            bool any_copied = false;

            size_t inspect_budget = 0;
            {
                std::lock_guard<std::mutex> lock(mMutex);
                inspect_budget = mPromoteQueue.size();
            }
            if (inspect_budget == 0) return;

            for (size_t i = 0; i < inspect_budget && promoted_bytes < MAX_PROMOTE_PER_FRAME; ++i)
            {
                void* handle = nullptr;
                uint64_t qgen = 0;
                NativeBufferEntry* entry = nullptr;
                {
                    std::lock_guard<std::mutex> lock(mMutex);
                    if (mPromoteQueue.empty()) break;
                    handle = mPromoteQueue.front().first;
                    qgen = mPromoteQueue.front().second;
                    mPromoteQueue.pop_front();
                    auto it = mLive.find(handle);
                    if (it == mLive.end() || !it->second || it->second->mAllocGen != qgen) continue;
                    entry = it->second.get();
                }

                if (entry->mDeviceBuf != VK_NULL_HANDLE || !entry->mBuf || !entry->mBuf->isValid() ||
                    entry->mValidBytes < 256 || entry->mWriteCount > 8)
                {
                    entry->mQueuedPromote = false;
                    continue;
                }

                const uint64_t settle_frames = std::max<uint64_t>(3ull, sDevice.framesInFlight() + 1ull);
                if (entry->mLastWriteEpoch != UINT64_MAX && cur_epoch < entry->mLastWriteEpoch + settle_frames)
                {
                    std::lock_guard<std::mutex> lock(mMutex);
                    mPromoteQueue.emplace_back(handle, qgen);
                    continue;
                }

                const VkDeviceSize span_sz = std::max<VkDeviceSize>(static_cast<VkDeviceSize>(entry->mValidBytes),
                                                                    static_cast<VkDeviceSize>(entry->mShadow.size()));
                const VkDeviceSize dev_sz = std::max<VkDeviceSize>(16, span_sz);
                const VkDeviceSize copy_sz = std::min<VkDeviceSize>(entry->mBuf->size(), dev_sz);
                VkBuffer dev_buf = VK_NULL_HANDLE;
                WolfVkAllocation dev_alloc{};
                if (!createDeviceLocalBuffer(entry->mIsIndex, dev_sz, dev_buf, dev_alloc))
                {
                    entry->mQueuedPromote = false;
                    break;
                }

                VkBufferCopy copy{};
                copy.srcOffset = 0;
                copy.dstOffset = 0;
                copy.size = copy_sz;
                gVk.vkCmdCopyBuffer(cmd, entry->mBuf->buffer(), dev_buf, 1, &copy);

                retireBuffer(std::move(entry->mBuf), entry->mIsIndex);
                entry->mDeviceBuf = dev_buf;
                entry->mDeviceAlloc = dev_alloc;
                entry->mDeviceSize = dev_sz;
                entry->mQueuedPromote = false;
                {
                    std::lock_guard<std::mutex> lock(mMutex);
                    mVramBytes += dev_sz;
                    ++mPromotedCount;
                }
                promoted_bytes += dev_sz;
                any_copied = true;
            }

            if (any_copied)
            {
                VkMemoryBarrier mb{};
                mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                mb.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
                gVk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                                         0, 1, &mb, 0, nullptr, 0, nullptr);
            }
        }

        void markDrawn(void* handle, uint64_t epoch)
        {
            if (!handle) return;
            auto* entry = static_cast<NativeBufferEntry*>(handle);
            entry->mLastDrawnEpoch = epoch;
        }

        void batchEnd() override
        {
        }

        void free(void* handle) override
        {
            if (!handle) return;
            std::unique_ptr<NativeBufferEntry> owned;
            {
                std::lock_guard<std::mutex> lock(mMutex);
                auto it = mLive.find(handle);
                if (it != mLive.end())
                {
                    owned = std::move(it->second);
                    mLive.erase(it);
                }
            }
            if (owned)
            {
                if (owned->mBuf)
                {
                    retireBuffer(std::move(owned->mBuf), owned->mIsIndex);
                }
                if (owned->mDeviceBuf != VK_NULL_HANDLE)
                {
                    retireDeviceBuffer(owned->mDeviceBuf, owned->mDeviceAlloc, owned->mDeviceSize);
                    owned->mDeviceBuf = VK_NULL_HANDLE;
                }
            }
        }

        U64 bytesUsed() override
        {
            std::lock_guard<std::mutex> lock(mMutex);
            return mBytesUsed + mVramBytes;
        }

        U64 vramBytesUsed()
        {
            std::lock_guard<std::mutex> lock(mMutex);
            return mVramBytes;
        }

        U64 hostBytesUsed()
        {
            std::lock_guard<std::mutex> lock(mMutex);
            return mBytesUsed;
        }

        U64 promotedCount()
        {
            std::lock_guard<std::mutex> lock(mMutex);
            return mPromotedCount;
        }

        VkBuffer vkBuffer(void* handle) const
        {
            if (!handle) return VK_NULL_HANDLE;
            const auto* entry = static_cast<const NativeBufferEntry*>(handle);
            if (entry->mDeviceBuf != VK_NULL_HANDLE) return entry->mDeviceBuf;
            return (entry->mBuf && entry->mBuf->isValid()) ? entry->mBuf->buffer() : VK_NULL_HANDLE;
        }

        void shutdown()
        {
            std::vector<std::unique_ptr<WolfVkBuffer>> all_bufs;
            std::vector<std::pair<VkBuffer, WolfVkAllocation>> dev_bufs;
            {
                std::lock_guard<std::mutex> lock(mMutex);
                for (auto& kv : mLive)
                {
                    if (!kv.second) continue;
                    if (kv.second->mBuf)
                    {
                        all_bufs.push_back(std::move(kv.second->mBuf));
                    }
                    if (kv.second->mDeviceBuf != VK_NULL_HANDLE)
                    {
                        dev_bufs.emplace_back(kv.second->mDeviceBuf, kv.second->mDeviceAlloc);
                        kv.second->mDeviceBuf = VK_NULL_HANDLE;
                    }
                }
                mLive.clear();
                mPromoteQueue.clear();
                for (auto& pkv : mRecyclePool)
                {
                    for (auto& b : pkv.second)
                    {
                        if (b) all_bufs.push_back(std::move(b));
                    }
                }
                mRecyclePool.clear();
                mBytesUsed = 0;
                mVramBytes = 0;
                mPooledBytes = 0;
                mPromotedCount = 0;
            }
            for (auto& b : all_bufs)
            {
                if (b) b->destroy();
            }
            if (sDevice.hasDevice() && !sDevice.hung())
            {
                for (auto& db : dev_bufs)
                {
                    if (db.first != VK_NULL_HANDLE)
                    {
                        gVk.vkDestroyBuffer(sDevice.device(), db.first, nullptr);
                        sDevice.allocator().free(db.second);
                    }
                }
            }
        }

    private:
        std::mutex mMutex;
        U64 mBytesUsed = 0;
        U64 mVramBytes = 0;
        U64 mPooledBytes = 0;
        U64 mPromotedCount = 0;
        U64 mNextAllocGen = 0;
        std::unordered_map<void*, std::unique_ptr<NativeBufferEntry>> mLive;
        std::deque<std::pair<void*, uint64_t>> mPromoteQueue;
        std::unordered_map<uint64_t, std::vector<std::unique_ptr<WolfVkBuffer>>> mRecyclePool;
    };

    NativeBufferStore sBufferStore;

    bool ensureFrameOpen();
    void endActivePass();
    VkCommandBuffer pickTransferCmdForImage(WolfVkImage* img);
    void finishActiveOcclusionQuery();
    void invalidateShadowBatchRenderPass(VkRenderPass pass);
    void invalidateShadowBatchProgram(const WolfVkProgram* prog);
    void onTextureImageReplaced(U32 name);
    std::unordered_set<VkImage> sAttachmentImages; // images ever attached to a WolfVkRenderTarget
    std::unordered_set<VkImage> sFrameCmdImages;   // images written on sFrameCmd during the current frame

    // -------------------------------------------------------------------------
    // 2. Pure-Vulkan Paged Lock-Free Texture Store (O(1) find & sampler lookup)
    // -------------------------------------------------------------------------
    class NativeTextureStore : public LLImageGL::WolfVkNativeTextureStore, public LLImageGL::WolfVkTextureOwner
    {
    public:
        struct StorageDesc
        {
            U32 mInternalFormat = 0;
            S32 mWidth = 0;
            S32 mHeight = 0;
            S32 mDepth = 1;
            S32 mLevels = 1;
            bool mValid = false;
        };

        struct SamplerOpts
        {
            S32 mFilter = LLTexUnit::TFO_BILINEAR;
            S32 mAddress = LLTexUnit::TAM_WRAP;
            bool mValid = false;
        };

        static constexpr U32 CHUNK_BITS = 12;
        static constexpr U32 CHUNK_SIZE = 1u << CHUNK_BITS; // 4096 slots/chunk
        static constexpr U32 MAX_CHUNKS = 512;              // up to 2,097,152 texture IDs

        struct InFlightMip0
        {
            std::vector<uint8_t> mBytes;
            uint32_t mIntFormat = 0;
            uint16_t mW = 0;
            uint16_t mH = 0;
            uint8_t mCh = 0;
        };

        struct Slot
        {
            std::atomic<WolfVkImage*> mRawImage{ nullptr };
            std::unique_ptr<WolfVkImage> mOwnedImage;
            StorageDesc mStorage;
            std::atomic<uint64_t> mPackedSampler{ 0 }; // bit 32 = valid, bits 16..31 = addr+1, bits 0..15 = filter+1
            std::vector<uint8_t> mCpuMip0;             // zero-stall CPU mip-0 cache for <=256x256 textures (sculpt maps, LUTs)
            uint64_t mLastUploadEpoch = UINT64_MAX;
            uint32_t mCpuIntFormat = 0;
            uint16_t mCpuW = 0;
            uint16_t mCpuH = 0;
            uint8_t mCpuCh = 0;
        };

        struct Chunk
        {
            Slot mSlots[CHUNK_SIZE];
        };

        NativeTextureStore()
        {
            for (U32 i = 0; i < MAX_CHUNKS; ++i)
            {
                mChunks[i].store(nullptr, std::memory_order_relaxed);
            }
        }

        ~NativeTextureStore()
        {
            for (U32 i = 0; i < MAX_CHUNKS; ++i)
            {
                delete mChunks[i].exchange(nullptr, std::memory_order_relaxed);
            }
        }

        Slot* ensureSlot(U32 name)
        {
            if (name == 0) return nullptr;
            const U32 ci = name >> CHUNK_BITS;
            if (ci >= MAX_CHUNKS) return nullptr;
            Chunk* c = mChunks[ci].load(std::memory_order_acquire);
            if (!c)
            {
                std::lock_guard<std::mutex> lock(mMutex);
                c = mChunks[ci].load(std::memory_order_relaxed);
                if (!c)
                {
                    c = new Chunk();
                    mChunks[ci].store(c, std::memory_order_release);
                }
            }
            return &c->mSlots[name & (CHUNK_SIZE - 1u)];
        }

        const Slot* findSlot(U32 name) const
        {
            if (name == 0) return nullptr;
            const U32 ci = name >> CHUNK_BITS;
            if (ci >= MAX_CHUNKS) return nullptr;
            const Chunk* c = mChunks[ci].load(std::memory_order_acquire);
            return c ? &c->mSlots[name & (CHUNK_SIZE - 1u)] : nullptr;
        }

        static uint64_t packSampler(S32 filter, S32 address)
        {
            const uint64_t f = static_cast<uint64_t>(static_cast<uint16_t>(filter));
            const uint64_t a = static_cast<uint64_t>(static_cast<uint16_t>(address));
            return (1ull << 32) | (a << 16) | f;
        }

        U32 createStorage(U32 target, S32 levels, U32 internal_format, S32 width, S32 height, S32 depth, bool gl_renders) override
        {
            (void)gl_renders;
            if (!sDevice.hasDevice() || width <= 0 || height <= 0 || depth <= 0) return 0;

            WolfVkImage::Kind kind = WolfVkImage::Kind::TEXTURE_2D;
            uint32_t layers = 1;
            switch (target)
            {
            case GL_TEXTURE_CUBE_MAP_ARRAY:
                if (!sDevice.cubeArrays() || (depth % 6) != 0) return 0;
                kind = WolfVkImage::Kind::TEXTURE_CUBE_ARRAY;
                layers = static_cast<uint32_t>(depth);
                break;
            case GL_TEXTURE_CUBE_MAP:
                if (depth != 6) return 0;
                kind = WolfVkImage::Kind::TEXTURE_CUBE;
                layers = 6;
                break;
            case GL_TEXTURE_2D_ARRAY:
                kind = WolfVkImage::Kind::TEXTURE_2D_ARRAY;
                layers = static_cast<uint32_t>(depth);
                break;
            case GL_TEXTURE_3D:
                kind = WolfVkImage::Kind::TEXTURE_3D;
                layers = static_cast<uint32_t>(depth);
                break;
            case GL_TEXTURE_2D:
                kind = WolfVkImage::Kind::TEXTURE_2D;
                layers = 1;
                break;
            default:
                return 0;
            }

            U32 eff_fmt = internal_format;
            bool force_alpha_one = false;
            if (eff_fmt == GL_RGB || eff_fmt == GL_RGB8)
            {
                eff_fmt = GL_RGBA8;
                force_alpha_one = true;
            }
            else if (eff_fmt == GL_RGB16F)
            {
                eff_fmt = GL_RGBA16F;
                force_alpha_one = true;
            }
            WolfVkImage::Format fmt = WolfVkImage::formatFor(eff_fmt, GL_UNSIGNED_BYTE);
            if (!fmt.valid()) return 0;

            VkFormatProperties props{};
            gVk.vkGetPhysicalDeviceFormatProperties(sDevice.physical(), fmt.mFormat, &props);
            const VkFormatFeatureFlags need_feat = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                                   VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                                                   VK_FORMAT_FEATURE_BLIT_DST_BIT;
            if ((props.optimalTilingFeatures & need_feat) != need_feat && eff_fmt == GL_R11F_G11F_B10F)
            {
                eff_fmt = GL_RGBA16F;
                fmt = WolfVkImage::formatFor(eff_fmt, GL_HALF_FLOAT);
            }
            if (force_alpha_one)
            {
                fmt.mSwizzle.a = VK_COMPONENT_SWIZZLE_ONE;
            }

            const uint32_t max_dim = std::max<uint32_t>(static_cast<uint32_t>(width),
                                     std::max<uint32_t>(static_cast<uint32_t>(height),
                                                        kind == WolfVkImage::Kind::TEXTURE_3D ? layers : 1u));
            uint32_t max_mips = 1;
            for (uint32_t d = max_dim; d > 1; d >>= 1) ++max_mips;
            const uint32_t mips = std::clamp<uint32_t>(static_cast<uint32_t>(std::max(1, levels)), 1u, max_mips);

            VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            if (!fmt.mDepth && !fmt.mBlockBytes && kind != WolfVkImage::Kind::TEXTURE_CUBE_ARRAY && kind != WolfVkImage::Kind::TEXTURE_CUBE)
            {
                usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            }

            auto created = std::make_unique<WolfVkImage>();
            std::string err;
            if (!created->create(sDevice, kind, fmt, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                                 layers, mips, usage, false, err))
            {
                warnOnce("storage_create_" + std::to_string(internal_format), "createStorage failed: " + err);
                return 0;
            }

            auto initImage = [&](VkCommandBuffer cmd)
            {
                created->transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
                if (!fmt.mDepth && !fmt.mBlockBytes)
                {
                    VkClearColorValue clear_val{};
                    clear_val.float32[3] = 1.0f;
                    VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0,
                                                   (kind == WolfVkImage::Kind::TEXTURE_3D) ? 1u : layers };
                    gVk.vkCmdClearColorImage(cmd, created->image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_val, 1, &range);
                }
                created->transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
            };
            if (sActive) ensureFrameOpen();
            VkCommandBuffer xfer_cmd = pickTransferCmdForImage(created.get());
            if (xfer_cmd != VK_NULL_HANDLE)
            {
                initImage(xfer_cmd);
            }
            else
            {
                VkCommandBuffer cmd = sDevice.beginOneTime();
                if (cmd != VK_NULL_HANDLE)
                {
                    initImage(cmd);
                    sDevice.endOneTimeAndWait(cmd, err);
                }
            }

            const U32 name = generateName();
            S32 def_filter = LLTexUnit::TFO_BILINEAR;
            S32 def_addr = LLTexUnit::TAM_CLAMP;
            if (kind == WolfVkImage::Kind::TEXTURE_CUBE_ARRAY || kind == WolfVkImage::Kind::TEXTURE_CUBE)
            {
                def_filter = (mips > 1) ? LLTexUnit::TFO_ANISOTROPIC : LLTexUnit::TFO_BILINEAR;
                def_addr = LLTexUnit::TAM_CLAMP;
            }
            else if (kind == WolfVkImage::Kind::TEXTURE_3D)
            {
                def_filter = (mips > 1) ? LLTexUnit::TFO_TRILINEAR : LLTexUnit::TFO_BILINEAR;
                def_addr = LLTexUnit::TAM_CLAMP;
            }
            else if (kind == WolfVkImage::Kind::TEXTURE_2D_ARRAY)
            {
                def_filter = (mips > 1) ? LLTexUnit::TFO_ANISOTROPIC : LLTexUnit::TFO_BILINEAR;
                def_addr = LLTexUnit::TAM_WRAP;
            }
            else
            {
                def_filter = (mips > 1) ? LLTexUnit::TFO_TRILINEAR : LLTexUnit::TFO_BILINEAR;
                def_addr = LLTexUnit::TAM_CLAMP;
            }
            Slot* slot = ensureSlot(name);
            if (!slot) return 0;
            {
                std::lock_guard<std::mutex> lock(mMutex);
                slot->mStorage = { internal_format, width, height, depth, static_cast<S32>(mips), true };
                slot->mPackedSampler.store(packSampler(def_filter, def_addr), std::memory_order_release);
                WolfVkImage* raw = created.get();
                slot->mOwnedImage = std::move(created);
                slot->mRawImage.store(raw, std::memory_order_release);
            }
            return name;
        }

        bool destroyStorage(U32 name) override
        {
            if (name == 0) return false;
            Slot* slot = ensureSlot(name);
            if (!slot) return false;
            onTextureImageReplaced(name);
            std::unique_ptr<WolfVkImage> img;
            {
                std::lock_guard<std::mutex> lock(mMutex);
                if (!slot->mStorage.mValid) return false;
                slot->mStorage = StorageDesc{};
                slot->mPackedSampler.store(0, std::memory_order_release);
                slot->mRawImage.store(nullptr, std::memory_order_release);
                slot->mCpuMip0.clear();
                slot->mCpuMip0.shrink_to_fit();
                slot->mCpuW = slot->mCpuH = 0;
                slot->mCpuCh = 0;
                mInFlightFrameMip0.erase(name);
                img = std::move(slot->mOwnedImage);
            }
            if (img) img->destroy();
            return true;
        }

        bool describe(U32 name, U32& internal_format, S32& width, S32& height, S32& depth, S32& levels) override
        {
            const Slot* slot = findSlot(name);
            if (!slot) return false;
            std::lock_guard<std::mutex> lock(mMutex);
            if (!slot->mStorage.mValid) return false;
            internal_format = slot->mStorage.mInternalFormat;
            width = slot->mStorage.mWidth;
            height = slot->mStorage.mHeight;
            depth = slot->mStorage.mDepth;
            levels = slot->mStorage.mLevels;
            return true;
        }

        U32 generateName() override
        {
            return mNextName.fetch_add(1, std::memory_order_relaxed);
        }

        void destroyNames(S32 count, const U32* names) override
        {
            if (!names || count <= 0) return;
            for (S32 i = 0; i < count; ++i)
            {
                if (names[i] != 0) onTextureImageReplaced(names[i]);
            }
            std::vector<std::unique_ptr<WolfVkImage>> to_destroy;
            {
                std::lock_guard<std::mutex> lock(mMutex);
                for (S32 i = 0; i < count; ++i)
                {
                    Slot* slot = ensureSlot(names[i]);
                    if (!slot) continue;
                    slot->mStorage = StorageDesc{};
                    slot->mPackedSampler.store(0, std::memory_order_release);
                    slot->mRawImage.store(nullptr, std::memory_order_release);
                    slot->mCpuMip0.clear();
                    slot->mCpuMip0.shrink_to_fit();
                    slot->mCpuW = slot->mCpuH = 0;
                    slot->mCpuCh = 0;
                    mInFlightFrameMip0.erase(names[i]);
                    if (slot->mOwnedImage)
                    {
                        to_destroy.push_back(std::move(slot->mOwnedImage));
                    }
                }
            }
            for (auto& img : to_destroy)
            {
                if (img) img->destroy();
            }
        }

        void setSamplerOptions(U32 name, S32 filter, S32 address) override
        {
            Slot* slot = ensureSlot(name);
            if (!slot) return;
            slot->mPackedSampler.store(packSampler(filter, address), std::memory_order_release);
        }

        bool getSamplerOptions(U32 name, S32& filter, S32& address) override
        {
            const Slot* slot = findSlot(name);
            if (!slot) return false;
            const uint64_t packed = slot->mPackedSampler.load(std::memory_order_acquire);
            if ((packed & (1ull << 32)) == 0) return false;
            filter = static_cast<S32>(static_cast<int16_t>(packed & 0xFFFFull));
            address = static_cast<S32>(static_cast<int16_t>((packed >> 16) & 0xFFFFull));
            return true;
        }

        bool uploadImage2D(U32 name, S32 miplevel, U32 intformat, S32 width, S32 height,
                           U32 pixformat, U32 pixtype, const void* pixels, bool swap_bytes,
                           bool generate_mips, S32 exact_mips = 0) override
        {
            if (!sDevice.hasDevice() || name == 0 || width <= 0 || height <= 0) return false;
            Slot* slot = ensureSlot(name);
            if (!slot) return false;

            U32 eff_intformat = intformat;
            bool force_alpha_one = false;
            if (pixformat == GL_BGRA && (intformat == GL_RGB || intformat == GL_RGB8 || intformat == GL_RGBA || intformat == GL_RGBA8))
            {
                eff_intformat = GL_BGRA;
                force_alpha_one = (intformat == GL_RGB || intformat == GL_RGB8);
            }
            else if (pixformat == GL_RGBA && (intformat == GL_RGB || intformat == GL_RGB8))
            {
                eff_intformat = GL_RGBA8;
                force_alpha_one = true;
            }
            else if (pixels && pixtype == GL_FLOAT)
            {
                if (intformat == GL_RGBA16F) eff_intformat = GL_RGBA32F;
                else if (intformat == GL_RGB16F) eff_intformat = GL_RGB;
                else if (intformat == GL_RG16F) eff_intformat = GL_RG32F;
                else if (intformat == GL_R16F) eff_intformat = GL_R32F;
            }
            WolfVkImage::Format fmt = WolfVkImage::formatFor(eff_intformat, pixtype);
            if (!fmt.valid())
            {
                fmt = WolfVkImage::formatFor(GL_RGBA8, GL_UNSIGNED_BYTE);
            }
            if (force_alpha_one)
            {
                fmt.mSwizzle.a = VK_COMPONENT_SWIZZLE_ONE;
            }

            const uint32_t full_mips = 1u + static_cast<uint32_t>(std::floor(std::log2(static_cast<double>(std::max(width, height)))));
            const uint32_t want_mips = (exact_mips > 0)
                ? std::clamp<uint32_t>(static_cast<uint32_t>(exact_mips), 1u, full_mips)
                : (generate_mips ? full_mips : 1u);

            WolfVkImage* img = slot->mRawImage.load(std::memory_order_acquire);
            std::unique_ptr<WolfVkImage> old_img;
            bool created_new = false;
            const bool need_recreate = !img || !img->isValid() ||
                (miplevel == 0 && (img->width() != static_cast<uint32_t>(width) ||
                                   img->height() != static_cast<uint32_t>(height) ||
                                   img->format().mFormat != fmt.mFormat ||
                                   img->mipLevels() != want_mips));
            if (need_recreate)
            {
                auto created = std::make_unique<WolfVkImage>();
                VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
                if (fmt.mDepth)
                {
                    usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
                }
                else if (!fmt.mBlockBytes)
                {
                    usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
                }
                std::string err;
                if (!created->create(sDevice, WolfVkImage::Kind::TEXTURE_2D, fmt,
                                     static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                                     1, want_mips, usage, false, err))
                {
                    warnOnce("tex_create_" + std::to_string(intformat), "texture create failed: " + err);
                    return false;
                }
                {
                    std::lock_guard<std::mutex> lock(mMutex);
                    old_img = std::move(slot->mOwnedImage);
                    img = created.get();
                    slot->mOwnedImage = std::move(created);
                    slot->mRawImage.store(img, std::memory_order_release);
                }
                created_new = true;
            }
            if (old_img)
            {
                onTextureImageReplaced(name);
                old_img->destroy();
            }

            if (sActive) ensureFrameOpen();

            if (created_new && !fmt.mDepth && (want_mips > 1 || !pixels))
            {
                auto clearAndTransition = [&](VkCommandBuffer cmd)
                {
                    if (!fmt.mBlockBytes)
                    {
                        img->transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
                        VkClearColorValue clear_val{};
                        clear_val.float32[3] = 1.0f;
                        VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, want_mips, 0, 1 };
                        gVk.vkCmdClearColorImage(cmd, img->image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_val, 1, &range);
                    }
                    img->transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
                };
                VkCommandBuffer xfer_cmd = pickTransferCmdForImage(img);
                if (xfer_cmd != VK_NULL_HANDLE)
                {
                    clearAndTransition(xfer_cmd);
                }
                else
                {
                    VkCommandBuffer cmd = sDevice.beginOneTime();
                    if (cmd != VK_NULL_HANDLE)
                    {
                        clearAndTransition(cmd);
                        std::string err;
                        sDevice.endOneTimeAndWait(cmd, err);
                    }
                }
            }

            if (pixels)
            {
                const uint32_t u_mip = static_cast<uint32_t>(std::max(0, miplevel));
                if (u_mip >= img->mipLevels())
                {
                    return true;
                }
                if (u_mip == 0)
                {
                    const uint64_t up_epoch = sUniforms.epoch();
                    const uint32_t src_ch = fmt.mSourceChannels;
                    if (!fmt.mBlockBytes && !fmt.mDepth && pixtype == GL_UNSIGNED_BYTE && src_ch >= 1u && src_ch <= 4u)
                    {
                        const size_t byte_cnt = static_cast<size_t>(width) * static_cast<size_t>(height) * src_ch;
                        auto copyNorm = [&](std::vector<uint8_t>& dst)
                        {
                            dst.resize(byte_cnt);
                            const uint8_t* s = static_cast<const uint8_t*>(pixels);
                            if (pixformat == GL_BGRA && src_ch == 4u)
                            {
                                const size_t npx = static_cast<size_t>(width) * static_cast<size_t>(height);
                                for (size_t p = 0; p < npx; ++p)
                                {
                                    dst[p * 4u + 0u] = s[p * 4u + 2u];
                                    dst[p * 4u + 1u] = s[p * 4u + 1u];
                                    dst[p * 4u + 2u] = s[p * 4u + 0u];
                                    dst[p * 4u + 3u] = s[p * 4u + 3u];
                                }
                            }
                            else
                            {
                                memcpy(dst.data(), s, byte_cnt);
                            }
                        };
                        std::lock_guard<std::mutex> lock(mMutex);
                        slot->mLastUploadEpoch = up_epoch;
                        slot->mCpuIntFormat = intformat;
                        if (width <= 256 && height <= 256)
                        {
                            copyNorm(slot->mCpuMip0);
                            slot->mCpuW = static_cast<uint16_t>(width);
                            slot->mCpuH = static_cast<uint16_t>(height);
                            slot->mCpuCh = static_cast<uint8_t>(src_ch);
                            mInFlightFrameMip0.erase(name);
                        }
                        else
                        {
                            slot->mCpuMip0.clear();
                            slot->mCpuMip0.shrink_to_fit();
                            slot->mCpuW = slot->mCpuH = 0;
                            slot->mCpuCh = 0;
                            if (sFrameOpen && width <= 2048 && height <= 2048)
                            {
                                InFlightMip0& inf = mInFlightFrameMip0[name];
                                copyNorm(inf.mBytes);
                                inf.mIntFormat = intformat;
                                inf.mW = static_cast<uint16_t>(width);
                                inf.mH = static_cast<uint16_t>(height);
                                inf.mCh = static_cast<uint8_t>(src_ch);
                            }
                        }
                    }
                    else
                    {
                        std::lock_guard<std::mutex> lock(mMutex);
                        slot->mLastUploadEpoch = up_epoch;
                        slot->mCpuIntFormat = intformat;
                        slot->mCpuMip0.clear();
                        slot->mCpuMip0.shrink_to_fit();
                        slot->mCpuW = slot->mCpuH = 0;
                        slot->mCpuCh = 0;
                        mInFlightFrameMip0.erase(name);
                    }
                }
                std::string err;
                VkCommandBuffer xfer_cmd = pickTransferCmdForImage(img);
                if (xfer_cmd != VK_NULL_HANDLE)
                {
                    if (!img->uploadSubRect(xfer_cmd, u_mip, 0,
                                            0, 0, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                                            0, pixels, pixformat, pixtype, swap_bytes, err))
                    {
                        warnOnce("tex_upload_" + std::to_string(intformat), "texture upload failed: " + err);
                        return false;
                    }
                    img->transition(xfer_cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
                }
                else
                {
                    if (!img->uploadSubRect(VK_NULL_HANDLE, u_mip, 0,
                                            0, 0, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                                            0, pixels, pixformat, pixtype, swap_bytes, err))
                    {
                        warnOnce("tex_upload_" + std::to_string(intformat), "texture upload failed: " + err);
                        return false;
                    }
                }
            }
            return true;
        }

        bool uploadSubImage2D(U32 name, S32 miplevel, S32 x_pos, S32 y_pos, S32 width, S32 height,
                              S32 row_pixels, U32 intformat, U32 pixformat, U32 pixtype,
                              const void* pixels, bool swap_bytes) override
        {
            if (!sDevice.hasDevice() || name == 0 || !pixels || width <= 0 || height <= 0) return false;
            WolfVkImage* img = find(name);
            if (!img || !img->isValid())
            {
                if (!uploadImage2D(name, miplevel, intformat, x_pos + width, y_pos + height, pixformat, pixtype, nullptr, swap_bytes, false))
                {
                    return false;
                }
                img = find(name);
            }
            if (!img || !img->isValid()) return false;
            if (sActive) ensureFrameOpen();
            if (miplevel <= 0)
            {
                if (Slot* slot = ensureSlot(name))
                {
                    std::lock_guard<std::mutex> lock(mMutex);
                    slot->mLastUploadEpoch = sUniforms.epoch();
                    slot->mCpuMip0.clear();
                    slot->mCpuW = slot->mCpuH = 0;
                    slot->mCpuCh = 0;
                    mInFlightFrameMip0.erase(name);
                }
            }
            std::string err;
            VkCommandBuffer xfer_cmd = pickTransferCmdForImage(img);
            if (xfer_cmd != VK_NULL_HANDLE)
            {
                if (!img->uploadSubRect(xfer_cmd, static_cast<uint32_t>(std::max(0, miplevel)), 0,
                                        static_cast<uint32_t>(std::max(0, x_pos)), static_cast<uint32_t>(std::max(0, y_pos)),
                                        static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                                        static_cast<uint32_t>(std::max(0, row_pixels)),
                                        pixels, pixformat, pixtype, swap_bytes, err))
                {
                    warnOnce("tex_subupload_" + std::to_string(intformat), "texture sub-upload failed: " + err);
                    return false;
                }
                img->transition(xfer_cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
                return true;
            }
            if (!img->uploadSubRect(VK_NULL_HANDLE, static_cast<uint32_t>(std::max(0, miplevel)), 0,
                                    static_cast<uint32_t>(std::max(0, x_pos)), static_cast<uint32_t>(std::max(0, y_pos)),
                                    static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                                    static_cast<uint32_t>(std::max(0, row_pixels)),
                                    pixels, pixformat, pixtype, swap_bytes, err))
            {
                warnOnce("tex_subupload_" + std::to_string(intformat), "texture sub-upload failed: " + err);
                return false;
            }
            return true;
        }

        bool generateMipmaps(U32 name) override
        {
            WolfVkImage* img = find(name);
            if (!img || !img->isValid() || img->mipLevels() < 2) return true;
            if (sActive) ensureFrameOpen();
            VkCommandBuffer xfer_cmd = pickTransferCmdForImage(img);
            if (xfer_cmd != VK_NULL_HANDLE)
            {
                img->generateMipmaps(xfer_cmd);
                img->transition(xfer_cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
                return true;
            }
            VkCommandBuffer cmd = sDevice.beginOneTime();
            if (cmd == VK_NULL_HANDLE) return false;
            img->generateMipmaps(cmd);
            img->transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
            std::string err;
            return sDevice.endOneTimeAndWait(cmd, err);
        }

        WolfVkImage* find(U32 name) const
        {
            const Slot* slot = findSlot(name);
            return slot ? slot->mRawImage.load(std::memory_order_acquire) : nullptr;
        }

        void clearInFlightFrameMip0()
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mInFlightFrameMip0.clear();
        }

        static void unpackAndResample2D(const uint8_t* src, uint32_t src_w, uint32_t src_h, uint32_t src_ch,
                                        uint32_t orig_intformat, uint32_t want_w, uint32_t want_h,
                                        uint32_t ncomp, uint8_t* out_data)
        {
            const bool alpha_only_tex = (orig_intformat == GL_ALPHA || orig_intformat == GL_ALPHA8 || orig_intformat == GL_COMPRESSED_ALPHA);
            if (src_w == want_w && src_h == want_h && src_ch == ncomp && !(src_ch == 1u && ncomp != 1u))
            {
                memcpy(out_data, src, static_cast<size_t>(want_w) * static_cast<size_t>(want_h) * ncomp);
                return;
            }
            for (uint32_t oy = 0; oy < want_h; ++oy)
            {
                const uint32_t sy0 = (oy * src_h) / want_h;
                const uint32_t sy1 = std::max<uint32_t>(sy0 + 1u, ((oy + 1u) * src_h) / want_h);
                for (uint32_t ox = 0; ox < want_w; ++ox)
                {
                    const uint32_t sx0 = (ox * src_w) / want_w;
                    const uint32_t sx1 = std::max<uint32_t>(sx0 + 1u, ((ox + 1u) * src_w) / want_w);
                    uint32_t sr = 0, sg = 0, sb = 0, sa = 0, cnt = 0;
                    for (uint32_t sy = sy0; sy < sy1 && sy < src_h; ++sy)
                    {
                        for (uint32_t sx = sx0; sx < sx1 && sx < src_w; ++sx)
                        {
                            const uint8_t* p = src + (static_cast<size_t>(sy) * src_w + sx) * src_ch;
                            uint32_t r = 0, g = 0, b = 0, a = 255;
                            if (src_ch == 4u) { r = p[0]; g = p[1]; b = p[2]; a = p[3]; }
                            else if (src_ch == 3u) { r = p[0]; g = p[1]; b = p[2]; a = 255; }
                            else if (src_ch == 2u) { r = p[0]; g = p[0]; b = p[0]; a = p[1]; }
                            else if (alpha_only_tex) { r = 0; g = 0; b = 0; a = p[0]; }
                            else { r = p[0]; g = p[0]; b = p[0]; a = 255; }
                            sr += r; sg += g; sb += b; sa += a;
                            ++cnt;
                        }
                    }
                    if (cnt == 0) cnt = 1;
                    const uint8_t r = static_cast<uint8_t>((sr + (cnt >> 1)) / cnt);
                    const uint8_t g = static_cast<uint8_t>((sg + (cnt >> 1)) / cnt);
                    const uint8_t b = static_cast<uint8_t>((sb + (cnt >> 1)) / cnt);
                    const uint8_t a = static_cast<uint8_t>((sa + (cnt >> 1)) / cnt);
                    uint8_t* dst = out_data + (static_cast<size_t>(oy) * want_w + ox) * ncomp;
                    if (ncomp == 4u) { dst[0] = r; dst[1] = g; dst[2] = b; dst[3] = a; }
                    else if (ncomp == 3u) { dst[0] = r; dst[1] = g; dst[2] = b; }
                    else if (ncomp == 2u) { dst[0] = r; dst[1] = a; }
                    else { dst[0] = alpha_only_tex ? a : r; }
                }
            }
        }

        bool readBackImage2D(U32 name, S32 gl_discard, S32 want_w, S32 want_h, S32 ncomponents, U8* out_data) override
        {
            (void)gl_discard;
            if (!sDevice.hasDevice() || name == 0 || !out_data || want_w <= 0 || want_h <= 0 || ncomponents < 1 || ncomponents > 4)
            {
                return false;
            }
            const Slot* slot = findSlot(name);
            if (!slot) return false;

            const uint32_t uw = static_cast<uint32_t>(want_w);
            const uint32_t uh = static_cast<uint32_t>(want_h);
            const uint32_t nc = static_cast<uint32_t>(ncomponents);
            uint64_t upload_epoch = UINT64_MAX;
            uint32_t orig_intformat = 0;
            {
                std::lock_guard<std::mutex> lock(mMutex);
                upload_epoch = slot->mLastUploadEpoch;
                orig_intformat = slot->mCpuIntFormat ? slot->mCpuIntFormat : slot->mStorage.mInternalFormat;
                if (!slot->mCpuMip0.empty() && slot->mCpuW > 0 && slot->mCpuH > 0 && slot->mCpuCh >= 1u && slot->mCpuCh <= 4u)
                {
                    unpackAndResample2D(slot->mCpuMip0.data(), slot->mCpuW, slot->mCpuH, slot->mCpuCh,
                                        orig_intformat, uw, uh, nc, out_data);
                    return true;
                }
                auto inf_it = mInFlightFrameMip0.find(name);
                if (inf_it != mInFlightFrameMip0.end() && !inf_it->second.mBytes.empty() && inf_it->second.mW > 0 && inf_it->second.mH > 0)
                {
                    unpackAndResample2D(inf_it->second.mBytes.data(), inf_it->second.mW, inf_it->second.mH, inf_it->second.mCh,
                                        inf_it->second.mIntFormat, uw, uh, nc, out_data);
                    return true;
                }
            }

            WolfVkImage* img = slot->mRawImage.load(std::memory_order_acquire);
            if (!img || !img->isValid() || img->format().mBlockBytes || img->format().mDepth ||
                img->format().mSourceBytesPerChannel != 1u || img->layout() == VK_IMAGE_LAYOUT_UNDEFINED)
            {
                return false;
            }
            if (sFrameOpen && (upload_epoch == sUniforms.epoch() || sFrameCmdImages.find(img->image()) != sFrameCmdImages.end()))
            {
                return false;
            }
            const uint32_t texel_bytes = img->format().mTexelBytes;
            if (texel_bytes < 1u || texel_bytes > 4u) return false;

            std::vector<uint8_t> gpu_bytes;
            std::string err;
            if (!img->readBack(gpu_bytes, err))
            {
                return false;
            }
            const uint32_t gw = img->width();
            const uint32_t gh = img->height();
            if (gpu_bytes.size() < static_cast<size_t>(gw) * static_cast<size_t>(gh) * texel_bytes)
            {
                return false;
            }
            if (img->format().mFormat == VK_FORMAT_B8G8R8A8_UNORM && texel_bytes == 4u)
            {
                const size_t npx = static_cast<size_t>(gw) * static_cast<size_t>(gh);
                for (size_t p = 0; p < npx; ++p)
                {
                    std::swap(gpu_bytes[p * 4u + 0u], gpu_bytes[p * 4u + 2u]);
                }
            }
            unpackAndResample2D(gpu_bytes.data(), gw, gh, texel_bytes, orig_intformat, uw, uh, nc, out_data);
            return true;
        }

        void shutdown()
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mInFlightFrameMip0.clear();
            const U32 hi = mNextName.load(std::memory_order_relaxed);
            const U32 max_ci = std::min<U32>(MAX_CHUNKS, (hi >> CHUNK_BITS) + 1u);
            for (U32 ci = 0; ci < max_ci; ++ci)
            {
                Chunk* c = mChunks[ci].exchange(nullptr, std::memory_order_acq_rel);
                if (!c) continue;
                for (U32 s = 0; s < CHUNK_SIZE; ++s)
                {
                    c->mSlots[s].mRawImage.store(nullptr, std::memory_order_relaxed);
                    if (c->mSlots[s].mOwnedImage)
                    {
                        c->mSlots[s].mOwnedImage->destroy();
                        c->mSlots[s].mOwnedImage.reset();
                    }
                }
                delete c;
            }
        }

    private:
        std::atomic<U32> mNextName{ 1 };
        mutable std::mutex mMutex;
        std::atomic<Chunk*> mChunks[MAX_CHUNKS];
        std::unordered_map<U32, InFlightMip0> mInFlightFrameMip0;
    };

    NativeTextureStore sTextureStore;

    // -------------------------------------------------------------------------
    // 3. Pure-Vulkan Render Target Owner + Pre-Pass Transfer Routing
    // -------------------------------------------------------------------------
    void flushDeferredPass();

    void endActivePass()
    {
        if (sActiveTarget && sFrameCmd != VK_NULL_HANDLE)
        {
            finishActiveOcclusionQuery();
            flushDeferredPass();
            sActiveTarget = nullptr;
            resetPassState();
        }
    }

    VkCommandBuffer pickTransferCmdForImage(WolfVkImage* img)
    {
        if (!sFrameOpen || sFrameCmd == VK_NULL_HANDLE) return VK_NULL_HANDLE;
        if (img && img->isValid() &&
            sAttachmentImages.find(img->image()) == sAttachmentImages.end() &&
            sFrameCmdImages.find(img->image()) == sFrameCmdImages.end())
        {
            VkCommandBuffer pre = sDevice.prePassTransferCmd();
            if (pre != VK_NULL_HANDLE)
            {
                return pre;
            }
        }
        endActivePass();
        if (img && img->isValid()) sFrameCmdImages.insert(img->image());
        return sFrameCmd;
    }

    WolfVkRenderTarget* sBoundLayerTarget = nullptr;

    class NativeRenderTargetOwner : public LLRenderTarget::WolfVkNativeOwner
    {
    public:
        WolfVkRenderTarget* getOrBuild(const LLRenderTarget& rt)
        {
            auto it = mTargets.find(&rt);
            if (it != mTargets.end() && it->second) return it->second.get();
            rebuild(rt);
            it = mTargets.find(&rt);
            return (it != mTargets.end()) ? it->second.get() : nullptr;
        }

        void rebuild(const LLRenderTarget& rt)
        {
            auto prev_it = mTargets.find(&rt);
            if (prev_it != mTargets.end() && prev_it->second)
            {
                if (prev_it->second.get() == sActiveTarget) endActivePass();
                invalidateShadowBatchRenderPass(prev_it->second->renderPass());
            }
            auto target = std::make_unique<WolfVkRenderTarget>();
            target->init(sDevice);
            std::string err;
            for (U32 i = 0; i < rt.getNumTextures(); ++i)
            {
                WolfVkImage* img = sTextureStore.find(rt.getTexture(i));
                if (img && img->isValid())
                {
                    sAttachmentImages.insert(img->image());
                    target->addExternalColorAttachment(img, err);
                }
            }
            U32 depth_name = rt.getDepth();
            if (depth_name == 0)
            {
                auto sit = mSharedDepth.find(&rt);
                if (sit != mSharedDepth.end()) depth_name = sit->second;
            }
            if (depth_name)
            {
                WolfVkImage* dimg = sTextureStore.find(depth_name);
                if (dimg && dimg->isValid())
                {
                    sAttachmentImages.insert(dimg->image());
                    target->setExternalDepth(dimg);
                }
            }
            target->ensurePass(err);
            mTargets[&rt] = std::move(target);
        }

        void onTargetChanged(LLRenderTarget& target) override
        {
            rebuild(target);
        }

        void onShareDepth(LLRenderTarget& src, LLRenderTarget& dst) override
        {
            if (src.getDepth())
            {
                mSharedDepth[&dst] = src.getDepth();
                rebuild(dst);
            }
        }

        void onBindTarget(LLRenderTarget& target) override
        {
            endActivePass();
            sBoundLayerTarget = nullptr;
            sPendingClearColor = false;
            sPendingClearDepth = false;
            WolfVkState::clearViewportOverride();
            if (!mTargets.count(&target))
            {
                rebuild(target);
            }
        }

        void onFlushTarget(LLRenderTarget& target, LLRenderTarget* previous) override
        {
            if ((sPendingClearColor || sPendingClearDepth) && sFrameOpen && !sActiveTarget)
            {
                WolfVkRenderTarget* rt = getOrBuild(target);
                std::string err;
                if (rt && rt->ensurePass(err))
                {
                    rt->begin(sFrameCmd, sPendingClearColor, sPendingClearDepth, sPendingClearRGBA, sPendingClearDepthVal, nullptr);
                    rt->end(sFrameCmd);
                }
            }
            sPendingClearColor = false;
            sPendingClearDepth = false;
            WolfVkState::clearViewportOverride();
            endActivePass();
        }

        void onReleaseTarget(LLRenderTarget& target) override
        {
            auto it = mTargets.find(&target);
            if (it != mTargets.end())
            {
                if (it->second)
                {
                    if (it->second.get() == sActiveTarget) endActivePass();
                    invalidateShadowBatchRenderPass(it->second->renderPass());
                }
                mTargets.erase(it);
            }
            mSharedDepth.erase(&target);
        }

        void invalidateUsingTexture(U32 tex_name)
        {
            if (tex_name == 0) return;
            std::vector<const LLRenderTarget*> to_drop;
            for (const auto& kv : mTargets)
            {
                const LLRenderTarget* rt = kv.first;
                if (!rt) continue;
                bool match = (rt->getDepth() == tex_name);
                if (!match)
                {
                    auto sit = mSharedDepth.find(rt);
                    if (sit != mSharedDepth.end() && sit->second == tex_name) match = true;
                }
                for (U32 i = 0; !match && i < rt->getNumTextures(); ++i)
                {
                    if (rt->getTexture(i) == tex_name) match = true;
                }
                if (match) to_drop.push_back(rt);
            }
            for (const LLRenderTarget* rt : to_drop)
            {
                auto it = mTargets.find(rt);
                if (it != mTargets.end())
                {
                    if (it->second)
                    {
                        if (it->second.get() == sActiveTarget) endActivePass();
                        invalidateShadowBatchRenderPass(it->second->renderPass());
                    }
                    mTargets.erase(it);
                }
            }
        }

        void shutdown()
        {
            endActivePass();
            mTargets.clear();
            mSharedDepth.clear();
        }

    private:
        std::unordered_map<const LLRenderTarget*, std::unique_ptr<WolfVkRenderTarget>> mTargets;
        std::unordered_map<const LLRenderTarget*, U32> mSharedDepth;
    };

    NativeRenderTargetOwner sRenderTargetOwner;

    struct LayerTargetKey
    {
        uint32_t mCount = 0;
        U32 mTex[WolfVkRenderTarget::MAX_COLOR_ATTACHMENTS] = {};
        S32 mLayer[WolfVkRenderTarget::MAX_COLOR_ATTACHMENTS] = {};

        bool operator==(const LayerTargetKey& o) const
        {
            if (mCount != o.mCount) return false;
            for (uint32_t i = 0; i < mCount; ++i)
            {
                if (mTex[i] != o.mTex[i] || mLayer[i] != o.mLayer[i]) return false;
            }
            return true;
        }
    };

    struct LayerTargetKeyHash
    {
        size_t operator()(const LayerTargetKey& k) const
        {
            uint64_t h = 1469598103934665603ull;
            h = (h ^ k.mCount) * 1099511628211ull;
            for (uint32_t i = 0; i < k.mCount; ++i)
            {
                h = (h ^ k.mTex[i]) * 1099511628211ull;
                h = (h ^ static_cast<uint32_t>(k.mLayer[i])) * 1099511628211ull;
            }
            return static_cast<size_t>(h);
        }
    };

    std::unordered_map<LayerTargetKey, std::unique_ptr<WolfVkRenderTarget>, LayerTargetKeyHash> sLayerTargets;
    uint64_t sSubmittedCmdCounter = 0;

    struct AsyncReadChannel
    {
        struct Slot
        {
            WolfVkBuffer mBuf;
            size_t mCapBytes = 0;
            size_t mReadBytes = 0;
            uint64_t mReadySubmit = 0;
            F32 mTag = 0.f;
            bool mPending = false;
        };
        static constexpr uint32_t RING = 4;
        Slot mSlots[RING];
        uint32_t mWrite = 0;
    };
    constexpr S32 MAX_ASYNC_READ_CHANNELS = 16;
    AsyncReadChannel sAsyncReadChannels[MAX_ASYNC_READ_CHANNELS];

    void shutdownAsyncReadChannels()
    {
        for (S32 c = 0; c < MAX_ASYNC_READ_CHANNELS; ++c)
        {
            for (uint32_t s = 0; s < AsyncReadChannel::RING; ++s)
            {
                AsyncReadChannel::Slot& sl = sAsyncReadChannels[c].mSlots[s];
                sl.mBuf.destroy();
                sl.mCapBytes = sl.mReadBytes = 0;
                sl.mReadySubmit = 0;
                sl.mPending = false;
            }
            sAsyncReadChannels[c].mWrite = 0;
        }
    }

    void invalidateLayerTargetsUsingTexture(U32 tex_name)
    {
        if (tex_name == 0) return;
        for (auto it = sLayerTargets.begin(); it != sLayerTargets.end();)
        {
            bool match = false;
            for (uint32_t i = 0; i < it->first.mCount; ++i)
            {
                if (it->first.mTex[i] == tex_name) { match = true; break; }
            }
            if (match)
            {
                if (it->second)
                {
                    if (it->second.get() == sActiveTarget) endActivePass();
                    if (it->second.get() == sBoundLayerTarget) sBoundLayerTarget = nullptr;
                    invalidateShadowBatchRenderPass(it->second->renderPass());
                }
                it = sLayerTargets.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    void onTextureImageReplaced(U32 name)
    {
        sRenderTargetOwner.invalidateUsingTexture(name);
        invalidateLayerTargetsUsingTexture(name);
    }

    // -------------------------------------------------------------------------
    // 4. Window target & swapchain helpers
    // -------------------------------------------------------------------------
    bool buildWindowTarget(uint32_t width, uint32_t height, std::string& error)
    {
        invalidateShadowBatchRenderPass(sWindowTarget.renderPass());
        sWindowTarget.release();
        sWindowTarget.init(sDevice);
        if (!sWindowTarget.allocate(width, height, GL_RGBA8, /*depth=*/true, /*shared=*/false, error))
        {
            return false;
        }
        return sWindowTarget.ensurePass(error);
    }

    bool recreateSwapchain(std::string& error)
    {
        if (!sWindow) return false;
        LLCoordWindow sz;
        sWindow->getSize(&sz);
        const uint32_t w = static_cast<uint32_t>(std::max<S32>(1, sz.mX));
        const uint32_t h = static_cast<uint32_t>(std::max<S32>(1, sz.mY));
        const bool vsync = gSavedSettings.getBOOL("RenderVSyncEnable");
        if (!sDevice.waitIdle())
        {
            error = "waitIdle failed before swapchain recreate";
            return false;
        }
        if (!sSwapchain.recreate(w, h, vsync, error))
        {
            return false;
        }
        if (!buildWindowTarget(sSwapchain.extent().width, sSwapchain.extent().height, error))
        {
            return false;
        }
        sResizePending = false;
        return true;
    }

    // -------------------------------------------------------------------------
    // 4b. Hardware Occlusion Queries & Multi-Draw Indirect Ring / Shadow Batch
    // -------------------------------------------------------------------------
    class IndirectStreamRing
    {
    public:
        bool init(WolfVkDevice& device, VkDeviceSize bytes_per_frame, std::string& error)
        {
            destroy();
            mBytes = bytes_per_frame;
            for (uint32_t i = 0; i < device.framesInFlight(); ++i)
            {
                auto b = std::make_unique<WolfVkBuffer>();
                if (!b->create(device, bytes_per_frame,
                               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                               /*host_only=*/true, error))
                {
                    destroy();
                    return false;
                }
                mBuffers.push_back(std::move(b));
            }
            return true;
        }

        void destroy()
        {
            for (auto& b : mBuffers)
            {
                if (b) b->destroy();
            }
            mBuffers.clear();
            mUsed = 0;
        }

        void beginFrame(uint32_t frame_index)
        {
            if (mBuffers.empty()) return;
            mFrame = frame_index % static_cast<uint32_t>(mBuffers.size());
            mUsed = 0;
        }

        bool push(const void* data, VkDeviceSize size, VkBuffer& out_buf, VkDeviceSize& out_offset)
        {
            if (mBuffers.empty() || !mBuffers[mFrame] || !mBuffers[mFrame]->isValid() || size == 0) return false;
            const VkDeviceSize start = (mUsed + 15ull) & ~15ull; // 16-byte aligned (satisfies VBO mat4 & 4-byte indirect command alignment)
            if (start + size > mBytes) return false;
            mBuffers[mFrame]->write(start, data, size);
            out_buf = mBuffers[mFrame]->buffer();
            out_offset = start;
            mUsed = start + size;
            return true;
        }

        VkBuffer currentBuffer() const
        {
            return (!mBuffers.empty() && mBuffers[mFrame]) ? mBuffers[mFrame]->buffer() : VK_NULL_HANDLE;
        }

    private:
        std::vector<std::unique_ptr<WolfVkBuffer>> mBuffers;
        VkDeviceSize mBytes = 0;
        VkDeviceSize mUsed = 0;
        uint32_t mFrame = 0;
    };

    IndirectStreamRing sIndirectRing;

    // Hardware Occlusion Query Pool (VK_QUERY_TYPE_OCCLUSION + hostQueryReset)
    static constexpr uint32_t OCCLUSION_POOL_CAPACITY = 32768;
    VkQueryPool sOcclusionPool = VK_NULL_HANDLE;

    enum class OcclusionQueryState : uint8_t
    {
        Idle = 0,
        Recording,
        RecordedInCurrentFrame,
        Submitted
    };

    struct LogicalOcclusionQuery
    {
        int32_t mPhysSlot = -1;
        OcclusionQueryState mState = OcclusionQueryState::Idle;
    };

    struct RetiredPhysSlot
    {
        int32_t mSlot = -1;
        uint64_t mRetiredFrame = 0;
    };

    std::vector<LogicalOcclusionQuery> sLogicalQueries; // index 0 unused
    std::vector<U32> sFreeLogicalIds;
    std::vector<int32_t> sFreePhysSlots;
    std::deque<RetiredPhysSlot> sRetiredPhysSlots;
    std::vector<U32> sRecordedQueriesThisFrame;
    U32 sActiveOcclusionId = 0;

    void initOcclusionPool()
    {
        sOcclusionPool = VK_NULL_HANDLE;
        sLogicalQueries.clear();
        sFreeLogicalIds.clear();
        sFreePhysSlots.clear();
        sRetiredPhysSlots.clear();
        sRecordedQueriesThisFrame.clear();
        sActiveOcclusionId = 0;

        if (!sDevice.hasDevice() || !sDevice.hostQueryReset()) return;

        VkQueryPoolCreateInfo qinfo{};
        qinfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qinfo.queryType = VK_QUERY_TYPE_OCCLUSION;
        qinfo.queryCount = OCCLUSION_POOL_CAPACITY;
        if (gVk.vkCreateQueryPool(sDevice.device(), &qinfo, nullptr, &sOcclusionPool) != VK_SUCCESS)
        {
            sOcclusionPool = VK_NULL_HANDLE;
            return;
        }
        sLogicalQueries.resize(1); // 0 reserved
        sFreePhysSlots.reserve(OCCLUSION_POOL_CAPACITY);
        for (int32_t i = static_cast<int32_t>(OCCLUSION_POOL_CAPACITY) - 1; i >= 0; --i)
        {
            sFreePhysSlots.push_back(i);
        }
        LL_INFOS("Vulkan") << "[Native] Hardware occlusion query pool active (" << OCCLUSION_POOL_CAPACITY << " slots, hostQueryReset)" << LL_ENDL;
    }

    void shutdownOcclusionPool()
    {
        if (sOcclusionPool != VK_NULL_HANDLE && sDevice.hasDevice() && !sDevice.hung())
        {
            gVk.vkDestroyQueryPool(sDevice.device(), sOcclusionPool, nullptr);
        }
        sOcclusionPool = VK_NULL_HANDLE;
        sLogicalQueries.clear();
        sFreeLogicalIds.clear();
        sFreePhysSlots.clear();
        sRetiredPhysSlots.clear();
        sRecordedQueriesThisFrame.clear();
        sActiveOcclusionId = 0;
    }

    void reclaimRetiredOcclusionSlots()
    {
        const uint64_t done_cutoff = sDevice.frameCounter();
        const uint64_t safety = static_cast<uint64_t>(sDevice.framesInFlight()) + 1ull;
        while (!sRetiredPhysSlots.empty())
        {
            const RetiredPhysSlot& front = sRetiredPhysSlots.front();
            if (done_cutoff >= front.mRetiredFrame + safety)
            {
                sFreePhysSlots.push_back(front.mSlot);
                sRetiredPhysSlots.pop_front();
            }
            else
            {
                break;
            }
        }
    }

    // -------------------------------------------------------------------------
    // 4d. Deferred Pass Queue + Aurora-Style 4-Thread Secondary Command Buffer Recorder
    // -------------------------------------------------------------------------
    enum class PassOpKind : uint8_t
    {
        Draw = 0,
        DrawIndexed,
        DrawIndexedIndirect,
        ClearAttachments,
        OcclusionBegin,
        OcclusionEnd
    };

    struct PassOp
    {
        PassOpKind mKind = PassOpKind::Draw;
        uint8_t mNumVBufs = 0;
        uint8_t mSet0OffCount = 0;
        uint8_t mSet2OffCount = 0;
        uint16_t mPushSize = 0;
        VkIndexType mIType = VK_INDEX_TYPE_UINT16;

        VkPipeline mPipeline = VK_NULL_HANDLE;
        VkPipelineLayout mLayout = VK_NULL_HANDLE;
        VkDescriptorSet mSet0 = VK_NULL_HANDLE;
        VkDescriptorSet mSet1 = VK_NULL_HANDLE;
        VkDescriptorSet mSet2 = VK_NULL_HANDLE;
        uint32_t mSet0Offsets[2] = {};
        uint32_t mSet2Offsets[8] = {};
        uint8_t mPushBytes[256] = {};

        VkViewport mViewport{};
        VkRect2D mScissor{};

        VkBuffer mVBufs[LLVertexBuffer::TYPE_MAX] = {};
        VkDeviceSize mVOffs[LLVertexBuffer::TYPE_MAX] = {};
        VkBuffer mIBuf = VK_NULL_HANDLE;
        VkBuffer mIndirectBuf = VK_NULL_HANDLE;
        VkDeviceSize mIndirectOff = 0;
        uint32_t mCount = 0;
        uint32_t mFirst = 0;

        uint32_t mClearAttCount = 0;
        VkClearAttachment mClearAtts[5] = {};
        VkClearRect mClearRect{};
        uint32_t mQuerySlot = 0;
    };

    std::vector<PassOp> sPassOps;
    bool sOpenPassClearColor = false;
    bool sOpenPassClearDepth = false;
    float sOpenPassClearRGBA[4] = { 0.f, 0.f, 0.f, 1.f };
    float sOpenPassClearDepthVal = 1.0f;
    uint32_t sPassDrawCount = 0;
    bool sPassForceInline = false;
    bool sPassHasOcclusion = false;

    void replayOpsSlice(VkCommandBuffer cmd, const PassOp* ops, size_t begin_idx, size_t end_idx)
    {
        VkPipeline cur_pipe = VK_NULL_HANDLE;
        VkPipelineLayout cur_desc_layout = VK_NULL_HANDLE;
        VkPipelineLayout cur_push_layout = VK_NULL_HANDLE;
        uint16_t cur_push_size = 0;
        uint8_t cur_push[256] = {};
        VkDescriptorSet cur_set[3] = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };
        uint8_t cur_off_cnt[3] = { 0, 0, 0 };
        uint32_t cur_offs[3][8] = {};
        bool has_vp = false;
        VkViewport cur_vp{};
        bool has_sc = false;
        VkRect2D cur_sc{};
        uint8_t cur_num_vb = 0;
        VkBuffer cur_vb[LLVertexBuffer::TYPE_MAX] = {};
        VkDeviceSize cur_vo[LLVertexBuffer::TYPE_MAX] = {};
        VkBuffer cur_ib = VK_NULL_HANDLE;
        VkIndexType cur_it = VK_INDEX_TYPE_UINT16;

        for (size_t i = begin_idx; i < end_idx; ++i)
        {
            const PassOp& op = ops[i];
            switch (op.mKind)
            {
            case PassOpKind::ClearAttachments:
                if (op.mClearAttCount > 0)
                {
                    gVk.vkCmdClearAttachments(cmd, op.mClearAttCount, op.mClearAtts, 1, &op.mClearRect);
                }
                continue;
            case PassOpKind::OcclusionBegin:
                if (sOcclusionPool != VK_NULL_HANDLE)
                {
                    gVk.vkCmdBeginQuery(cmd, sOcclusionPool, op.mQuerySlot, 0);
                }
                continue;
            case PassOpKind::OcclusionEnd:
                if (sOcclusionPool != VK_NULL_HANDLE)
                {
                    gVk.vkCmdEndQuery(cmd, sOcclusionPool, op.mQuerySlot);
                }
                continue;
            default:
                break;
            }

            if (op.mPipeline != cur_pipe)
            {
                gVk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, op.mPipeline);
                cur_pipe = op.mPipeline;
            }
            if (!has_vp || memcmp(&op.mViewport, &cur_vp, sizeof(VkViewport)) != 0)
            {
                gVk.vkCmdSetViewport(cmd, 0, 1, &op.mViewport);
                cur_vp = op.mViewport;
                has_vp = true;
            }
            if (!has_sc || memcmp(&op.mScissor, &cur_sc, sizeof(VkRect2D)) != 0)
            {
                gVk.vkCmdSetScissor(cmd, 0, 1, &op.mScissor);
                cur_sc = op.mScissor;
                has_sc = true;
            }

            if (op.mLayout != cur_desc_layout)
            {
                cur_desc_layout = op.mLayout;
                cur_set[0] = cur_set[1] = cur_set[2] = VK_NULL_HANDLE;
                cur_push_layout = VK_NULL_HANDLE;
                cur_push_size = 0;
            }

            if (op.mPushSize > 0)
            {
                if (op.mLayout != cur_push_layout || op.mPushSize != cur_push_size ||
                    memcmp(op.mPushBytes, cur_push, op.mPushSize) != 0)
                {
                    gVk.vkCmdPushConstants(cmd, op.mLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, op.mPushSize, op.mPushBytes);
                    cur_push_layout = op.mLayout;
                    cur_push_size = op.mPushSize;
                    memcpy(cur_push, op.mPushBytes, op.mPushSize);
                }
            }

            if (op.mSet0 != VK_NULL_HANDLE)
            {
                if (op.mSet0 != cur_set[0] || op.mSet0OffCount != cur_off_cnt[0] ||
                    (op.mSet0OffCount > 0 && memcmp(op.mSet0Offsets, cur_offs[0], op.mSet0OffCount * sizeof(uint32_t)) != 0))
                {
                    gVk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, op.mLayout,
                                                WolfVkProgram::SET_UNIFORMS, 1, &op.mSet0, op.mSet0OffCount, op.mSet0Offsets);
                    cur_set[0] = op.mSet0;
                    cur_off_cnt[0] = op.mSet0OffCount;
                    if (op.mSet0OffCount > 0) memcpy(cur_offs[0], op.mSet0Offsets, op.mSet0OffCount * sizeof(uint32_t));
                }
            }
            if (op.mSet1 != VK_NULL_HANDLE && op.mSet1 != cur_set[1])
            {
                gVk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, op.mLayout,
                                            WolfVkProgram::SET_SAMPLERS, 1, &op.mSet1, 0, nullptr);
                cur_set[1] = op.mSet1;
            }
            if (op.mSet2 != VK_NULL_HANDLE)
            {
                if (op.mSet2 != cur_set[2] || op.mSet2OffCount != cur_off_cnt[2] ||
                    (op.mSet2OffCount > 0 && memcmp(op.mSet2Offsets, cur_offs[2], op.mSet2OffCount * sizeof(uint32_t)) != 0))
                {
                    gVk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, op.mLayout,
                                                WolfVkProgram::SET_BLOCKS, 1, &op.mSet2, op.mSet2OffCount, op.mSet2Offsets);
                    cur_set[2] = op.mSet2;
                    cur_off_cnt[2] = op.mSet2OffCount;
                    if (op.mSet2OffCount > 0) memcpy(cur_offs[2], op.mSet2Offsets, op.mSet2OffCount * sizeof(uint32_t));
                }
            }

            if (op.mNumVBufs > 0)
            {
                const bool same_vb = (op.mNumVBufs == cur_num_vb) &&
                                     memcmp(op.mVBufs, cur_vb, op.mNumVBufs * sizeof(VkBuffer)) == 0 &&
                                     memcmp(op.mVOffs, cur_vo, op.mNumVBufs * sizeof(VkDeviceSize)) == 0;
                if (!same_vb)
                {
                    gVk.vkCmdBindVertexBuffers(cmd, 0, op.mNumVBufs, op.mVBufs, op.mVOffs);
                    memcpy(cur_vb, op.mVBufs, op.mNumVBufs * sizeof(VkBuffer));
                    memcpy(cur_vo, op.mVOffs, op.mNumVBufs * sizeof(VkDeviceSize));
                    cur_num_vb = op.mNumVBufs;
                }
            }

            if (op.mKind == PassOpKind::Draw)
            {
                gVk.vkCmdDraw(cmd, op.mCount, 1, op.mFirst, 0);
            }
            else
            {
                if (op.mIBuf != cur_ib || op.mIType != cur_it)
                {
                    gVk.vkCmdBindIndexBuffer(cmd, op.mIBuf, 0, op.mIType);
                    cur_ib = op.mIBuf;
                    cur_it = op.mIType;
                }
                if (op.mKind == PassOpKind::DrawIndexed)
                {
                    gVk.vkCmdDrawIndexed(cmd, op.mCount, 1, op.mFirst, 0, 0);
                }
                else
                {
                    gVk.vkCmdDrawIndexedIndirect(cmd, op.mIndirectBuf, op.mIndirectOff,
                                                 op.mCount, sizeof(VkDrawIndexedIndirectCommand));
                }
            }
        }
    }

    class ParallelPassRecorder
    {
    public:
        static constexpr uint32_t MAX_LANES = 4;      // Lane 0 = main thread, Lanes 1..3 = persistent worker threads
        static constexpr uint32_t MAX_SLOTS = 4;

        bool init()
        {
            shutdown();
            if (!sDevice.hasDevice()) return false;
            mSlotCount = std::min<uint32_t>(MAX_SLOTS, std::max<uint32_t>(1u, sDevice.framesInFlight()));
            for (uint32_t lane = 0; lane < MAX_LANES; ++lane)
            {
                for (uint32_t s = 0; s < mSlotCount; ++s)
                {
                    VkCommandPoolCreateInfo pinfo{};
                    pinfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
                    pinfo.queueFamilyIndex = sDevice.queueFamily();
                    pinfo.flags = 0;
                    if (gVk.vkCreateCommandPool(sDevice.device(), &pinfo, nullptr, &mLanes[lane].mPools[s]) != VK_SUCCESS)
                    {
                        shutdown();
                        return false;
                    }
                    mLanes[lane].mUsed[s] = 0;
                }
            }
            mStop = false;
            mGeneration = 0;
            mPendingWorkers = 0;
            mActiveWorkers = 0;
            for (uint32_t w = 1; w < MAX_LANES; ++w)
            {
                mWorkers.emplace_back([this, w]() { workerMain(w); });
            }
            mReady = true;
            LL_INFOS("Vulkan") << "[Native] Multithreaded secondary command buffer recorder active ("
                               << MAX_LANES << " lanes, " << mSlotCount << " frames in flight)" << LL_ENDL;
            return true;
        }

        void shutdown()
        {
            if (!mWorkers.empty())
            {
                {
                    std::lock_guard<std::mutex> lock(mMutex);
                    mStop = true;
                }
                mWorkCv.notify_all();
                for (std::thread& t : mWorkers)
                {
                    if (t.joinable()) t.join();
                }
                mWorkers.clear();
            }
            mReady = false;
            if (sDevice.hasDevice() && !sDevice.hung())
            {
                for (uint32_t lane = 0; lane < MAX_LANES; ++lane)
                {
                    for (uint32_t s = 0; s < MAX_SLOTS; ++s)
                    {
                        if (mLanes[lane].mPools[s] != VK_NULL_HANDLE)
                        {
                            gVk.vkDestroyCommandPool(sDevice.device(), mLanes[lane].mPools[s], nullptr);
                        }
                    }
                }
            }
            for (uint32_t lane = 0; lane < MAX_LANES; ++lane)
            {
                for (uint32_t s = 0; s < MAX_SLOTS; ++s)
                {
                    mLanes[lane].mPools[s] = VK_NULL_HANDLE;
                    mLanes[lane].mCmds[s].clear();
                    mLanes[lane].mUsed[s] = 0;
                }
            }
        }

        bool isReady() const { return mReady; }

        void resetSlot(uint32_t slot)
        {
            if (!mReady || slot >= mSlotCount || !sDevice.hasDevice() || sDevice.hung()) return;
            for (uint32_t lane = 0; lane < MAX_LANES; ++lane)
            {
                gVk.vkResetCommandPool(sDevice.device(), mLanes[lane].mPools[slot], 0);
                mLanes[lane].mUsed[slot] = 0;
            }
        }

        bool recordAndExecute(VkCommandBuffer primary_cmd, uint32_t slot, VkRenderPass pass, VkFramebuffer fb,
                              const std::vector<PassOp>& ops)
        {
            if (!mReady || slot >= mSlotCount || ops.empty()) return false;
            const size_t total = ops.size();
            const uint32_t lane_count = std::min<uint32_t>(MAX_LANES, std::max<uint32_t>(1u, static_cast<uint32_t>(total / 32u)));
            if (lane_count <= 1) return false;

            const size_t chunk = (total + lane_count - 1u) / lane_count;
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mJobSlot = slot;
                mJobPass = pass;
                mJobFb = fb;
                mJobOps = ops.data();
                uint32_t actual_lanes = 0;
                for (uint32_t l = 0; l < lane_count; ++l)
                {
                    const size_t b = static_cast<size_t>(l) * chunk;
                    if (b >= total) break;
                    const size_t e = std::min<size_t>(total, b + chunk);
                    mJobBegin[actual_lanes] = b;
                    mJobEnd[actual_lanes] = e;
                    mJobOutCmd[actual_lanes] = VK_NULL_HANDLE;
                    ++actual_lanes;
                }
                if (actual_lanes <= 1) return false;
                mActiveWorkers = actual_lanes - 1u;
                mPendingWorkers = mActiveWorkers;
                ++mGeneration;
            }
            mWorkCv.notify_all();

            // Lane 0 encodes concurrently on the main thread (zero idle wait!)
            mJobOutCmd[0] = encodeLaneSlice(0, slot, pass, fb, ops.data(), mJobBegin[0], mJobEnd[0]);

            {
                std::unique_lock<std::mutex> lock(mMutex);
                mDoneCv.wait(lock, [this]() { return mPendingWorkers == 0; });
            }

            VkCommandBuffer valid_cmds[MAX_LANES];
            uint32_t valid_count = 0;
            const uint32_t total_lanes = 1u + mActiveWorkers;
            for (uint32_t l = 0; l < total_lanes; ++l)
            {
                if (mJobOutCmd[l] != VK_NULL_HANDLE)
                {
                    valid_cmds[valid_count++] = mJobOutCmd[l];
                }
            }
            if (valid_count > 0)
            {
                gVk.vkCmdExecuteCommands(primary_cmd, valid_count, valid_cmds);
            }
            return valid_count == total_lanes;
        }

    private:
        struct LanePool
        {
            VkCommandPool mPools[MAX_SLOTS] = {};
            std::vector<VkCommandBuffer> mCmds[MAX_SLOTS];
            uint32_t mUsed[MAX_SLOTS] = {};
        };

        VkCommandBuffer encodeLaneSlice(uint32_t lane, uint32_t slot, VkRenderPass pass, VkFramebuffer fb,
                                        const PassOp* ops, size_t b, size_t e)
        {
            LanePool& lp = mLanes[lane];
            if (lp.mUsed[slot] >= lp.mCmds[slot].size())
            {
                VkCommandBufferAllocateInfo alloc{};
                alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                alloc.commandPool = lp.mPools[slot];
                alloc.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
                alloc.commandBufferCount = 1;
                VkCommandBuffer c = VK_NULL_HANDLE;
                if (gVk.vkAllocateCommandBuffers(sDevice.device(), &alloc, &c) != VK_SUCCESS)
                {
                    return VK_NULL_HANDLE;
                }
                lp.mCmds[slot].push_back(c);
            }
            VkCommandBuffer sec = lp.mCmds[slot][lp.mUsed[slot]++];
            VkCommandBufferInheritanceInfo inh{};
            inh.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
            inh.renderPass = pass;
            inh.subpass = 0;
            inh.framebuffer = fb;
            inh.occlusionQueryEnable = VK_FALSE;

            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
            begin.pInheritanceInfo = &inh;
            if (gVk.vkBeginCommandBuffer(sec, &begin) != VK_SUCCESS)
            {
                return VK_NULL_HANDLE;
            }
            replayOpsSlice(sec, ops, b, e);
            if (gVk.vkEndCommandBuffer(sec) != VK_SUCCESS)
            {
                return VK_NULL_HANDLE;
            }
            return sec;
        }

        void workerMain(uint32_t lane)
        {
            uint64_t seen_gen = 0;
            for (;;)
            {
                uint32_t slot = 0;
                VkRenderPass pass = VK_NULL_HANDLE;
                VkFramebuffer fb = VK_NULL_HANDLE;
                const PassOp* ops = nullptr;
                size_t b = 0, e = 0;
                bool active = false;
                {
                    std::unique_lock<std::mutex> lock(mMutex);
                    mWorkCv.wait(lock, [&]() { return mStop || mGeneration != seen_gen; });
                    if (mStop) return;
                    seen_gen = mGeneration;
                    if (lane <= mActiveWorkers)
                    {
                        active = true;
                        slot = mJobSlot;
                        pass = mJobPass;
                        fb = mJobFb;
                        ops = mJobOps;
                        b = mJobBegin[lane];
                        e = mJobEnd[lane];
                    }
                }
                if (active)
                {
                    VkCommandBuffer out = encodeLaneSlice(lane, slot, pass, fb, ops, b, e);
                    std::lock_guard<std::mutex> lock(mMutex);
                    mJobOutCmd[lane] = out;
                    if (--mPendingWorkers == 0)
                    {
                        mDoneCv.notify_one();
                    }
                }
            }
        }

        bool mReady = false;
        uint32_t mSlotCount = 0;
        LanePool mLanes[MAX_LANES];
        std::vector<std::thread> mWorkers;
        std::mutex mMutex;
        std::condition_variable mWorkCv;
        std::condition_variable mDoneCv;
        bool mStop = false;
        uint64_t mGeneration = 0;
        uint32_t mActiveWorkers = 0;
        uint32_t mPendingWorkers = 0;

        uint32_t mJobSlot = 0;
        VkRenderPass mJobPass = VK_NULL_HANDLE;
        VkFramebuffer mJobFb = VK_NULL_HANDLE;
        const PassOp* mJobOps = nullptr;
        size_t mJobBegin[MAX_LANES] = {};
        size_t mJobEnd[MAX_LANES] = {};
        VkCommandBuffer mJobOutCmd[MAX_LANES] = {};
    };

    ParallelPassRecorder sParallelRecorder;

    void finishActiveOcclusionQuery()
    {
        if (sActiveOcclusionId != 0 && sActiveOcclusionId < sLogicalQueries.size())
        {
            LogicalOcclusionQuery& lq = sLogicalQueries[sActiveOcclusionId];
            if (lq.mState == OcclusionQueryState::Recording && lq.mPhysSlot >= 0 &&
                sActiveTarget != nullptr && sOcclusionPool != VK_NULL_HANDLE)
            {
                PassOp op{};
                op.mKind = PassOpKind::OcclusionEnd;
                op.mQuerySlot = static_cast<uint32_t>(lq.mPhysSlot);
                sPassOps.push_back(op);
                lq.mState = OcclusionQueryState::RecordedInCurrentFrame;
            }
            else
            {
                lq.mState = OcclusionQueryState::Idle;
            }
        }
        sActiveOcclusionId = 0;
    }

    void onFrameQueriesSubmitted(bool submitted_ok)
    {
        finishActiveOcclusionQuery();
        for (U32 id : sRecordedQueriesThisFrame)
        {
            if (id > 0 && id < sLogicalQueries.size())
            {
                LogicalOcclusionQuery& lq = sLogicalQueries[id];
                if (lq.mState == OcclusionQueryState::RecordedInCurrentFrame)
                {
                    if (submitted_ok)
                    {
                        lq.mState = OcclusionQueryState::Submitted;
                    }
                    else
                    {
                        if (lq.mPhysSlot >= 0)
                        {
                            sFreePhysSlots.push_back(lq.mPhysSlot);
                            lq.mPhysSlot = -1;
                        }
                        lq.mState = OcclusionQueryState::Idle;
                    }
                }
            }
        }
        sRecordedQueriesThisFrame.clear();
    }

    // Static Shadow Batch Pages (DEVICE_LOCAL VRAM) & Custom Instanced Pipeline Cache
    struct ShadowBatchPage
    {
        WolfVkBuffer mVertexBuf;
        WolfVkBuffer mIndexBuf;
        bool mAlive = false;
    };
    std::vector<std::unique_ptr<ShadowBatchPage>> sShadowBatchPages;

    struct ShadowBatchPipeKey
    {
        const WolfVkProgram* mProgram = nullptr;
        VkRenderPass mPass = VK_NULL_HANDLE;
        uint32_t mColorAttachments = 0;
        uint8_t mColorMaskBits = 0;
        bool mCullFace = true;
        bool mDepthTest = true;
        bool mDepthWrite = true;
        uint32_t mDepthFunc = 0x0201;
        bool mDepthClamp = false;
        float mBiasSlope = 0.f;
        float mBiasConst = 0.f;

        bool operator==(const ShadowBatchPipeKey& o) const
        {
            return mProgram == o.mProgram && mPass == o.mPass &&
                   mColorAttachments == o.mColorAttachments && mColorMaskBits == o.mColorMaskBits &&
                   mCullFace == o.mCullFace && mDepthTest == o.mDepthTest &&
                   mDepthWrite == o.mDepthWrite && mDepthFunc == o.mDepthFunc &&
                   mDepthClamp == o.mDepthClamp &&
                   mBiasSlope == o.mBiasSlope && mBiasConst == o.mBiasConst;
        }
    };

    struct ShadowBatchPipeKeyHash
    {
        size_t operator()(const ShadowBatchPipeKey& k) const
        {
            uint64_t h = 1469598103934665603ull;
            auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ull; };
            mix(reinterpret_cast<uintptr_t>(k.mProgram));
            mix(reinterpret_cast<uintptr_t>(k.mPass));
            mix(k.mColorAttachments);
            mix(k.mColorMaskBits);
            mix(k.mCullFace ? 1u : 0u);
            mix(k.mDepthTest ? 1u : 0u);
            mix(k.mDepthWrite ? 1u : 0u);
            mix(k.mDepthFunc);
            mix(k.mDepthClamp ? 1u : 0u);
            uint32_t bs = 0, bc = 0;
            memcpy(&bs, &k.mBiasSlope, 4);
            memcpy(&bc, &k.mBiasConst, 4);
            mix(bs);
            mix(bc);
            return static_cast<size_t>(h);
        }
    };

    std::unordered_map<ShadowBatchPipeKey, VkPipeline, ShadowBatchPipeKeyHash> sShadowBatchPipelines;

    // Fast 1-entry pipeline lookup cache + per-shader cache invalidation
    const WolfVkProgram* sLastPipeProg = nullptr;
    VkRenderPass sLastPipePass = VK_NULL_HANDLE;
    WolfVkPipelineState sLastPipeState{};
    VkPipeline sLastPipeHandle = VK_NULL_HANDLE;

    void clearLastPipeCache()
    {
        sLastPipeProg = nullptr;
        sLastPipePass = VK_NULL_HANDLE;
        sLastPipeHandle = VK_NULL_HANDLE;
    }

    // -------------------------------------------------------------------------
    // 4c. Per-Pass GPU Hardware Timestamp Queries + CPU Stage Telemetry (#VulkanPerf#)
    // -------------------------------------------------------------------------
    enum TimestampSlotPoint : uint32_t
    {
        TS_FRAME_START  = 0,
        TS_UPLOADS_DONE = 1,
        TS_SHADOWS_DONE = 2,
        TS_WORLD_DONE   = 3,
        TS_FRAME_END    = 4,
        TS_PER_FRAME    = 5
    };
    static constexpr uint32_t MAX_TS_FRAME_SLOTS = 4;
    VkQueryPool sTimestampPool = VK_NULL_HANDLE;
    double sTimestampPeriodMs = 0.0; // nanoseconds_per_tick * 1e-6
    uint8_t sTsRecordedMask[MAX_TS_FRAME_SLOTS] = {};
    uint8_t sTsSubmittedMask[MAX_TS_FRAME_SLOTS] = {};

    struct PerfTelemetry
    {
        std::chrono::steady_clock::time_point mLastPresentEnd{};
        std::chrono::steady_clock::time_point mLastLogTime{};
        bool mHasLastPresent = false;

        // Current frame CPU & counter accumulators
        double mFenceWaitMsThisFrame = 0.0;
        double mVkDrawMsThisFrame = 0.0;
        uint32_t mPushConstDrawsThisFrame = 0;
        uint32_t mIndirectBatchesThisFrame = 0;
        uint32_t mPassesThisFrame = 0;
        uint32_t mMtPassesThisFrame = 0;
        uint32_t mMtDrawsThisFrame = 0;
        uint32_t mOccIssuedThisFrame = 0;
        uint32_t mOccReadThisFrame = 0;
        uint32_t mOccCulledThisFrame = 0;
        bool mRecordedUploadsDone = false;
        bool mRecordedWorldDone = false;

        // 10-second window accumulators
        uint64_t mWinFrames = 0;
        uint64_t mWinGpuSamples = 0;
        double mWinCpuFrameMs = 0.0;
        double mWinCpuFenceMs = 0.0;
        double mWinCpuDrawMs = 0.0;
        double mWinCpuPresentMs = 0.0;
        double mWinGpuTotalMs = 0.0;
        double mWinGpuUploadMs = 0.0;
        double mWinGpuShadowMs = 0.0;
        double mWinGpuWorldMs = 0.0;
        double mWinGpuUiMs = 0.0;
        uint64_t mWinDraws = 0;
        uint64_t mWinPushConstDraws = 0;
        uint64_t mWinIndirectBatches = 0;
        uint64_t mWinPasses = 0;
        uint64_t mWinMtPasses = 0;
        uint64_t mWinMtDraws = 0;
        uint64_t mWinOccIssued = 0;
        uint64_t mWinOccRead = 0;
        uint64_t mWinOccCulled = 0;
        double mWinUniformKb = 0.0;
        double mWinStagingKb = 0.0;

        void resetFrame()
        {
            mFenceWaitMsThisFrame = 0.0;
            mVkDrawMsThisFrame = 0.0;
            mPushConstDrawsThisFrame = 0;
            mIndirectBatchesThisFrame = 0;
            mPassesThisFrame = 0;
            mMtPassesThisFrame = 0;
            mMtDrawsThisFrame = 0;
            mOccIssuedThisFrame = 0;
            mOccReadThisFrame = 0;
            mOccCulledThisFrame = 0;
            mRecordedUploadsDone = false;
            mRecordedWorldDone = false;
        }
    };
    PerfTelemetry sPerf;

    // [W114-VULKAN 2026-11-27] GPU time of named Wolf sky sections (gpuSectionBegin / gpuSectionEnd): a pair of timestamps per
    // call, written between render passes (the open pass is ended first: ops inside a pass are encoded later, at its end).
    static constexpr uint32_t SEC_PAIRS = 16;   // calls of one section a frame (reflection-probe faces repeat a section)
    static constexpr uint32_t SEC_QUERIES_PER_SLOT = WolfVkNative::GPU_SECTION_MAX * SEC_PAIRS * 2;
    VkQueryPool sSecPool = VK_NULL_HANDLE;
    uint8_t sSecPairs[MAX_TS_FRAME_SLOTS][WolfVkNative::GPU_SECTION_MAX] = {};   // pairs written this slot
    bool sSecOpen[WolfVkNative::GPU_SECTION_MAX] = {};
    double sSecWinMs[WolfVkNative::GPU_SECTION_MAX] = {};
    uint32_t sSecWinCalls[WolfVkNative::GPU_SECTION_MAX] = {};
    uint64_t sSecWinFrames = 0;

    void harvestAndResetSections(VkCommandBuffer cmd, uint32_t slot)
    {
        if (sSecPool == VK_NULL_HANDLE || cmd == VK_NULL_HANDLE || slot >= MAX_TS_FRAME_SLOTS) return;
        bool any = false;
        for (uint32_t s = 0; s < WolfVkNative::GPU_SECTION_MAX; ++s)
        {
            const uint32_t pairs = sSecPairs[slot][s];
            for (uint32_t p = 0; p < pairs; ++p)
            {
                uint64_t t[2] = { 0, 0 };
                const uint32_t q = slot * SEC_QUERIES_PER_SLOT + (s * SEC_PAIRS + p) * 2;
                if (gVk.vkGetQueryPoolResults(sDevice.device(), sSecPool, q, 2, sizeof(t), t, sizeof(uint64_t),
                                              VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] >= t[0])
                {
                    sSecWinMs[s] += static_cast<double>(t[1] - t[0]) * sTimestampPeriodMs;
                    ++sSecWinCalls[s];
                    any = true;
                }
            }
            sSecPairs[slot][s] = 0;
        }
        if (any) ++sSecWinFrames;
        gVk.vkCmdResetQueryPool(cmd, sSecPool, slot * SEC_QUERIES_PER_SLOT, SEC_QUERIES_PER_SLOT);
    }

    void initTimestampPool()
    {
        sTimestampPool = VK_NULL_HANDLE;
        sTimestampPeriodMs = 0.0;
        memset(sTsRecordedMask, 0, sizeof(sTsRecordedMask));
        memset(sTsSubmittedMask, 0, sizeof(sTsSubmittedMask));
        sPerf = PerfTelemetry{};
        sPerf.mLastLogTime = std::chrono::steady_clock::now();

        if (!sDevice.hasDevice()) return;
        const uint32_t qf = sDevice.queueFamily();
        const auto& qfs = sDevice.chosen().mQueueFamilies;
        const float period_ns = sDevice.limits().timestampPeriod;
        if (qf >= qfs.size() || qfs[qf].timestampValidBits == 0 || period_ns <= 0.0f)
        {
            return;
        }
        const uint32_t slots = std::min<uint32_t>(MAX_TS_FRAME_SLOTS, std::max<uint32_t>(1u, sDevice.framesInFlight()));
        VkQueryPoolCreateInfo qinfo{};
        qinfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qinfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qinfo.queryCount = slots * TS_PER_FRAME;
        if (gVk.vkCreateQueryPool(sDevice.device(), &qinfo, nullptr, &sTimestampPool) != VK_SUCCESS)
        {
            sTimestampPool = VK_NULL_HANDLE;
            return;
        }
        sTimestampPeriodMs = static_cast<double>(period_ns) * 1.0e-6;
        qinfo.queryCount = slots * SEC_QUERIES_PER_SLOT;
        if (gVk.vkCreateQueryPool(sDevice.device(), &qinfo, nullptr, &sSecPool) != VK_SUCCESS) sSecPool = VK_NULL_HANDLE;
        memset(sSecPairs, 0, sizeof(sSecPairs));
        memset(sSecOpen, 0, sizeof(sSecOpen));
    }

    void shutdownTimestampPool()
    {
        if (sTimestampPool != VK_NULL_HANDLE && sDevice.hasDevice() && !sDevice.hung())
        {
            gVk.vkDestroyQueryPool(sDevice.device(), sTimestampPool, nullptr);
        }
        if (sSecPool != VK_NULL_HANDLE && sDevice.hasDevice() && !sDevice.hung())
        {
            gVk.vkDestroyQueryPool(sDevice.device(), sSecPool, nullptr);
        }
        sSecPool = VK_NULL_HANDLE;
        sTimestampPool = VK_NULL_HANDLE;
        sTimestampPeriodMs = 0.0;
        memset(sTsRecordedMask, 0, sizeof(sTsRecordedMask));
        memset(sTsSubmittedMask, 0, sizeof(sTsSubmittedMask));
    }

    void writeTimestamp(VkCommandBuffer cmd, uint32_t slot, TimestampSlotPoint pt, VkPipelineStageFlagBits stage)
    {
        if (sTimestampPool == VK_NULL_HANDLE || cmd == VK_NULL_HANDLE || slot >= MAX_TS_FRAME_SLOTS) return;
        const uint8_t bit = static_cast<uint8_t>(1u << static_cast<uint32_t>(pt));
        if (sTsRecordedMask[slot] & bit) return;
        gVk.vkCmdWriteTimestamp(cmd, stage, sTimestampPool, slot * TS_PER_FRAME + static_cast<uint32_t>(pt));
        sTsRecordedMask[slot] |= bit;
    }

    void harvestAndResetSlotTimestamps(VkCommandBuffer cmd, uint32_t slot)
    {
        if (sTimestampPool == VK_NULL_HANDLE || cmd == VK_NULL_HANDLE || slot >= MAX_TS_FRAME_SLOTS) return;
        const uint8_t mask = sTsSubmittedMask[slot];
        if ((mask & (1u << TS_FRAME_START)) && (mask & (1u << TS_FRAME_END)))
        {
            uint64_t ticks[TS_PER_FRAME] = {};
            bool ok = true;
            for (uint32_t q = 0; q < TS_PER_FRAME; ++q)
            {
                if (mask & (1u << q))
                {
                    const VkResult r = gVk.vkGetQueryPoolResults(
                        sDevice.device(), sTimestampPool, slot * TS_PER_FRAME + q, 1,
                        sizeof(uint64_t), &ticks[q], sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
                    if (r != VK_SUCCESS)
                    {
                        ok = false;
                        break;
                    }
                }
            }
            if (ok)
            {
                const uint64_t t0 = ticks[TS_FRAME_START];
                const uint64_t t1 = (mask & (1u << TS_UPLOADS_DONE)) ? std::max(t0, ticks[TS_UPLOADS_DONE]) : t0;
                const uint64_t t2 = (mask & (1u << TS_SHADOWS_DONE)) ? std::max(t1, ticks[TS_SHADOWS_DONE]) : t1;
                const uint64_t t3 = (mask & (1u << TS_WORLD_DONE))   ? std::max(t2, ticks[TS_WORLD_DONE])   : t2;
                const uint64_t t4 = std::max(t3, ticks[TS_FRAME_END]);
                sPerf.mWinGpuTotalMs  += static_cast<double>(t4 - t0) * sTimestampPeriodMs;
                sPerf.mWinGpuUploadMs += static_cast<double>(t1 - t0) * sTimestampPeriodMs;
                sPerf.mWinGpuShadowMs += static_cast<double>(t2 - t1) * sTimestampPeriodMs;
                sPerf.mWinGpuWorldMs  += static_cast<double>(t3 - t2) * sTimestampPeriodMs;
                sPerf.mWinGpuUiMs     += static_cast<double>(t4 - t3) * sTimestampPeriodMs;
                ++sPerf.mWinGpuSamples;
            }
        }
        sTsSubmittedMask[slot] = 0;
        sTsRecordedMask[slot] = 0;
        gVk.vkCmdResetQueryPool(cmd, sTimestampPool, slot * TS_PER_FRAME, TS_PER_FRAME);
        writeTimestamp(cmd, slot, TS_FRAME_START, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
    }

    void flushDeferredPass()
    {
        if (!sActiveTarget || sFrameCmd == VK_NULL_HANDLE) return;
        WolfVkRenderTarget* target = sActiveTarget;
        if (sPassOps.empty() && !sOpenPassClearColor && !sOpenPassClearDepth)
        {
            return;
        }
        for (uint32_t i = 0; i < target->getNumTextures(); ++i)
        {
            if (WolfVkImage* cimg = target->image(i)) sFrameCmdImages.insert(cimg->image());
        }
        if (WolfVkImage* dimg = target->depthImage()) sFrameCmdImages.insert(dimg->image());

        const uint32_t slot = sDevice.frameIndex();
        if (target == &sWindowTarget && !sPerf.mRecordedWorldDone)
        {
            sPerf.mRecordedWorldDone = true;
            writeTimestamp(sFrameCmd, slot, TS_WORLD_DONE, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        }

        const auto t_enc0 = std::chrono::steady_clock::now();
        const bool want_mt = !sPassForceInline && sPassDrawCount >= 96 && sParallelRecorder.isReady();
        if (want_mt)
        {
            VkRenderPass active_pass = VK_NULL_HANDLE;
            VkFramebuffer active_fb = VK_NULL_HANDLE;
            target->begin(sFrameCmd, sOpenPassClearColor, sOpenPassClearDepth,
                          sOpenPassClearRGBA, sOpenPassClearDepthVal, nullptr,
                          /*secondary_contents=*/true, &active_pass, &active_fb);
            sParallelRecorder.recordAndExecute(sFrameCmd, slot, active_pass, active_fb, sPassOps);
            target->end(sFrameCmd);
            ++sPerf.mMtPassesThisFrame;
            sPerf.mMtDrawsThisFrame += sPassDrawCount;
        }
        else
        {
            target->begin(sFrameCmd, sOpenPassClearColor, sOpenPassClearDepth,
                          sOpenPassClearRGBA, sOpenPassClearDepthVal, nullptr,
                          /*secondary_contents=*/false);
            if (!sPassOps.empty())
            {
                replayOpsSlice(sFrameCmd, sPassOps.data(), 0, sPassOps.size());
            }
            target->end(sFrameCmd);
        }
        const auto t_enc1 = std::chrono::steady_clock::now();
        sPerf.mVkDrawMsThisFrame += std::chrono::duration<double, std::milli>(t_enc1 - t_enc0).count();
        ++sPerf.mPassesThisFrame;

        sPassOps.clear();
        sPassDrawCount = 0;
        sPassForceInline = false;
        sPassHasOcclusion = false;
        sOpenPassClearColor = false;
        sOpenPassClearDepth = false;
    }

    void invalidateShadowBatchRenderPass(VkRenderPass pass)
    {
        if (pass == VK_NULL_HANDLE) return;
        if (sLastPipePass == pass) clearLastPipeCache();
        for (LLGLSLShader* sh : LLGLSLShader::sInstances)
        {
            if (sh && sh->mWolfVk) sh->mWolfVk->invalidateRenderPass(pass);
        }
        std::vector<VkPipeline> gone;
        for (auto it = sShadowBatchPipelines.begin(); it != sShadowBatchPipelines.end();)
        {
            if (it->first.mPass == pass)
            {
                gone.push_back(it->second);
                it = sShadowBatchPipelines.erase(it);
            }
            else ++it;
        }
        if (!gone.empty() && sDevice.hasDevice() && !sDevice.hung())
        {
            sDevice.deferDestroy([gone]()
            {
                for (VkPipeline p : gone)
                {
                    if (p != VK_NULL_HANDLE) gVk.vkDestroyPipeline(sDevice.device(), p, nullptr);
                }
            });
        }
    }

    void invalidateShadowBatchProgram(const WolfVkProgram* prog)
    {
        if (!prog) return;
        if (sLastPipeProg == prog) clearLastPipeCache();
        std::vector<VkPipeline> gone;
        for (auto it = sShadowBatchPipelines.begin(); it != sShadowBatchPipelines.end();)
        {
            if (it->first.mProgram == prog)
            {
                gone.push_back(it->second);
                it = sShadowBatchPipelines.erase(it);
            }
            else ++it;
        }
        if (!gone.empty() && sDevice.hasDevice() && !sDevice.hung())
        {
            sDevice.deferDestroy([gone]()
            {
                for (VkPipeline p : gone)
                {
                    if (p != VK_NULL_HANDLE) gVk.vkDestroyPipeline(sDevice.device(), p, nullptr);
                }
            });
        }
    }

    VkPipeline getOrBuildShadowBatchPipeline(const ShadowBatchPipeKey& key)
    {
        auto it = sShadowBatchPipelines.find(key);
        if (it != sShadowBatchPipelines.end()) return it->second;

        const WolfVkProgram& program = *key.mProgram;
        const int32_t pos_loc = program.inputLocation("position");
        const int32_t mvp_loc = program.inputLocation("wolf_mvp");
        if (pos_loc < 0 || mvp_loc < 0) return VK_NULL_HANDLE;

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = program.vertex().mModule;
        stages[0].pName = program.vertex().mReflection.mEntryPoint.c_str();
        stages[0].pSpecializationInfo = program.specialization();
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = program.fragment().mModule;
        stages[1].pName = program.fragment().mReflection.mEntryPoint.c_str();
        stages[1].pSpecializationInfo = program.specialization();

        VkVertexInputBindingDescription bindings[2]{};
        bindings[0] = { 0u, 3u * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX };
        bindings[1] = { 1u, 16u * sizeof(float), VK_VERTEX_INPUT_RATE_INSTANCE };

        VkVertexInputAttributeDescription attrs[5]{};
        attrs[0] = { static_cast<uint32_t>(pos_loc), 0u, VK_FORMAT_R32G32B32_SFLOAT, 0u };
        for (uint32_t col = 0; col < 4u; ++col)
        {
            attrs[1u + col] = { static_cast<uint32_t>(mvp_loc) + col, 1u, VK_FORMAT_R32G32B32A32_SFLOAT, col * 16u };
        }

        VkPipelineVertexInputStateCreateInfo vertex_input{};
        vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertex_input.vertexBindingDescriptionCount = 2;
        vertex_input.pVertexBindingDescriptions = bindings;
        vertex_input.vertexAttributeDescriptionCount = 5;
        vertex_input.pVertexAttributeDescriptions = attrs;

        VkPipelineInputAssemblyStateCreateInfo assembly{};
        assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        assembly.primitiveRestartEnable = VK_FALSE;

        VkPipelineViewportDepthClipControlCreateInfoEXT depth_clip{};
        depth_clip.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT;
        depth_clip.negativeOneToOne = VK_TRUE;

        VkPipelineViewportStateCreateInfo viewport{};
        viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        if (sDevice.glDepthRange()) viewport.pNext = &depth_clip;

        VkPipelineRasterizationStateCreateInfo raster{};
        raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.depthClampEnable = (key.mDepthClamp && sDevice.depthClamp()) ? VK_TRUE : VK_FALSE;
        raster.rasterizerDiscardEnable = VK_FALSE;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = key.mCullFace ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
        raster.depthBiasEnable = (key.mBiasConst != 0.f || key.mBiasSlope != 0.f) ? VK_TRUE : VK_FALSE;
        raster.depthBiasConstantFactor = key.mBiasConst;
        raster.depthBiasSlopeFactor = key.mBiasSlope;
        raster.depthBiasClamp = 0.f;
        raster.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depth{};
        depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth.depthTestEnable = key.mDepthTest ? VK_TRUE : VK_FALSE;
        depth.depthWriteEnable = (key.mDepthTest && key.mDepthWrite) ? VK_TRUE : VK_FALSE;
        depth.depthCompareOp = (key.mDepthFunc >= 0x200 && key.mDepthFunc <= 0x207)
                             ? static_cast<VkCompareOp>(key.mDepthFunc - 0x200)
                             : VK_COMPARE_OP_LESS;

        VkPipelineColorBlendAttachmentState att{};
        att.blendEnable = VK_FALSE;
        att.colorWriteMask = ((key.mColorMaskBits & 1u) ? VK_COLOR_COMPONENT_R_BIT : 0u) |
                             ((key.mColorMaskBits & 2u) ? VK_COLOR_COMPONENT_G_BIT : 0u) |
                             ((key.mColorMaskBits & 4u) ? VK_COLOR_COMPONENT_B_BIT : 0u) |
                             ((key.mColorMaskBits & 8u) ? VK_COLOR_COMPONENT_A_BIT : 0u);
        std::vector<VkPipelineColorBlendAttachmentState> atts(key.mColorAttachments, att);

        VkPipelineColorBlendStateCreateInfo blend{};
        blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.attachmentCount = static_cast<uint32_t>(atts.size());
        blend.pAttachments = atts.empty() ? nullptr : atts.data();

        const VkDynamicState dynamics[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamics;

        VkGraphicsPipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
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
        info.renderPass = key.mPass;
        info.subpass = 0;
        info.basePipelineIndex = -1;

        VkPipeline pipe = VK_NULL_HANDLE;
        if (gVk.vkCreateGraphicsPipelines(sDevice.device(), VK_NULL_HANDLE, 1, &info, nullptr, &pipe) != VK_SUCCESS)
        {
            return VK_NULL_HANDLE;
        }
        sShadowBatchPipelines[key] = pipe;
        return pipe;
    }

    bool ensureFrameOpen()
    {
        if (sFrameOpen) return true;
        if (!sActive || !sDevice.hasDevice() || sDevice.hung()) return false;

        std::string err;
        if (sResizePending)
        {
            if (!recreateSwapchain(err))
            {
                warnOnce("resize_fail", "swapchain resize failed: " + err);
                return false;
            }
        }

        sPerf.resetFrame();
        const auto t_fence0 = std::chrono::steady_clock::now();
        WolfVkDevice::Frame& frame = sDevice.beginFrame();
        const auto t_fence1 = std::chrono::steady_clock::now();
        sPerf.mFenceWaitMsThisFrame = std::chrono::duration<double, std::milli>(t_fence1 - t_fence0).count();
        if (sDevice.hung() || frame.mCmd == VK_NULL_HANDLE)
        {
            return false;
        }
        const uint32_t slot = sDevice.frameIndex();
        sUniforms.beginFrame(slot);
        sStagingRing.beginFrame(slot);
        sIndirectRing.beginFrame(slot);
        sParallelRecorder.resetSlot(slot);
        sDevice.flushDeferred();
        reclaimRetiredOcclusionSlots();

        sFrameCmd = frame.mCmd;
        sFrameOpen = true;
        sActiveTarget = nullptr;
        sFrameCmdImages.clear();
        sTextureStore.clearInFlightFrameMip0();
        sPassOps.clear();
        if (sPassOps.capacity() < 8192) sPassOps.reserve(8192);
        sPassDrawCount = 0;
        sPassForceInline = false;
        sPassHasOcclusion = false;
        sOpenPassClearColor = false;
        sOpenPassClearDepth = false;
        resetPassState();
        sPendingClearColor = true;
        sPendingClearDepth = true;

        VkCommandBuffer pre_cmd = sDevice.prePassTransferCmd();
        VkCommandBuffer xfer_cmd = (pre_cmd != VK_NULL_HANDLE) ? pre_cmd : sFrameCmd;
        harvestAndResetSlotTimestamps(xfer_cmd, slot);
        harvestAndResetSections(xfer_cmd, slot);
        sBufferStore.promoteSettledToVRAM(xfer_cmd, sUniforms.epoch());
        return true;
    }

    WolfVkRenderTarget* currentTarget()
    {
        if (sBoundLayerTarget && sBoundLayerTarget->isComplete())
        {
            return sBoundLayerTarget;
        }
        if (LLRenderTarget::sBoundTarget)
        {
            WolfVkRenderTarget* rt = sRenderTargetOwner.getOrBuild(*LLRenderTarget::sBoundTarget);
            if (rt && rt->isComplete()) return rt;
        }
        return &sWindowTarget;
    }

    bool ensureRenderPassOpen()
    {
        if (!ensureFrameOpen()) return false;
        WolfVkRenderTarget* target = currentTarget();
        if (!target) return false;
        if (sActiveTarget == target) return true;

        endActivePass();
        std::string err;
        if (!target->ensurePass(err))
        {
            warnOnce("pass_ensure", "render pass ensure failed: " + err);
            return false;
        }
        sOpenPassClearColor = sPendingClearColor;
        sOpenPassClearDepth = sPendingClearDepth;
        memcpy(sOpenPassClearRGBA, sPendingClearRGBA, sizeof(sOpenPassClearRGBA));
        sOpenPassClearDepthVal = sPendingClearDepthVal;
        sPendingClearColor = false;
        sPendingClearDepth = false;
        sPassOps.clear();
        sPassDrawCount = 0;
        sPassForceInline = false;
        sPassHasOcclusion = false;
        sActiveTarget = target;
        resetPassState();
        return true;
    }

    void computeTargetViewportAndScissor(VkViewport& vp, VkRect2D& sc)
    {
        const uint32_t tw = sActiveTarget->getWidth();
        const uint32_t th = sActiveTarget->getHeight();
        if (sActiveTarget == &sWindowTarget && gGLViewport[2] > 0 && gGLViewport[3] > 0)
        {
            vp.x = static_cast<float>(gGLViewport[0]);
            vp.y = static_cast<float>(gGLViewport[1]);
            vp.width = static_cast<float>(gGLViewport[2]);
            vp.height = static_cast<float>(gGLViewport[3]);
        }
        else if (sActiveTarget != &sWindowTarget && WolfVkState::current().mViewportOverride &&
                 WolfVkState::current().mViewport[2] > 0 && WolfVkState::current().mViewport[3] > 0)
        {
            vp.x = static_cast<float>(WolfVkState::current().mViewport[0]);
            vp.y = static_cast<float>(WolfVkState::current().mViewport[1]);
            vp.width = static_cast<float>(WolfVkState::current().mViewport[2]);
            vp.height = static_cast<float>(WolfVkState::current().mViewport[3]);
        }
        else
        {
            vp.x = 0.f;
            vp.y = 0.f;
            vp.width = static_cast<float>(tw);
            vp.height = static_cast<float>(th);
        }
        vp.minDepth = 0.f;
        vp.maxDepth = 1.f;

        if (LLGLState::wolfVkIsEnabled(GL_SCISSOR_TEST))
        {
            const S32 gx = LLGLState::sWolfScissor[0];
            const S32 gy = LLGLState::sWolfScissor[1];
            const S32 gw = LLGLState::sWolfScissor[2];
            const S32 gh = LLGLState::sWolfScissor[3];
            const S32 vx0 = llclamp<S32>(gx, 0, static_cast<S32>(tw));
            const S32 vy0 = llclamp<S32>(gy, 0, static_cast<S32>(th));
            const S32 vx1 = llclamp<S32>(gx + gw, vx0, static_cast<S32>(tw));
            const S32 vy1 = llclamp<S32>(gy + gh, vy0, static_cast<S32>(th));
            sc.offset = { vx0, vy0 };
            sc.extent = { static_cast<uint32_t>(vx1 - vx0), static_cast<uint32_t>(vy1 - vy0) };
        }
        else
        {
            sc.offset = { 0, 0 };
            sc.extent = { tw, th };
        }
    }

    // -------------------------------------------------------------------------
    // 5. Clear & Draw callbacks (installed on LLGLState & LLVertexBuffer)
    // -------------------------------------------------------------------------
    void onNativeClear(U32 mask, const F32 rgba[4], F32 depth)
    {
        const bool clr_color = (mask & GL_COLOR_BUFFER_BIT) != 0;
        const bool clr_depth = (mask & GL_DEPTH_BUFFER_BIT) != 0;
        if (!clr_color && !clr_depth) return;

        // Point 4: Avoid breaking an active render pass on clear!
        // 1) If the active pass has 0 recorded ops so far and is non-scissored, merge directly into LOAD_OP_CLEAR!
        // 2) If draws have already been recorded in the active pass (e.g. clearing depth before HUD/UI), emit
        //    vkCmdClearAttachments inside the open render pass with ZERO pass breaks!
        if (sActiveTarget && sActiveTarget == currentTarget())
        {
            const bool scissored = LLGLState::wolfVkIsEnabled(GL_SCISSOR_TEST);
            if (sPassOps.empty() && !scissored)
            {
                if (clr_color)
                {
                    sOpenPassClearColor = true;
                    memcpy(sOpenPassClearRGBA, rgba, 4 * sizeof(float));
                }
                if (clr_depth)
                {
                    sOpenPassClearDepth = true;
                    sOpenPassClearDepthVal = depth;
                }
                return;
            }

            PassOp cop{};
            cop.mKind = PassOpKind::ClearAttachments;
            uint32_t ac = 0;
            if (clr_color)
            {
                for (uint32_t i = 0; i < sActiveTarget->getNumTextures() && ac < 4u; ++i)
                {
                    cop.mClearAtts[ac].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    cop.mClearAtts[ac].colorAttachment = i;
                    memcpy(cop.mClearAtts[ac].clearValue.color.float32, rgba, 4 * sizeof(float));
                    ++ac;
                }
            }
            if (clr_depth && sActiveTarget->depthImage() != nullptr)
            {
                cop.mClearAtts[ac].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                cop.mClearAtts[ac].clearValue.depthStencil.depth = depth;
                cop.mClearAtts[ac].clearValue.depthStencil.stencil = 0;
                ++ac;
            }
            cop.mClearAttCount = ac;
            VkViewport dummy_vp{};
            computeTargetViewportAndScissor(dummy_vp, cop.mClearRect.rect);
            cop.mClearRect.baseArrayLayer = 0;
            cop.mClearRect.layerCount = 1;
            if (ac > 0 && cop.mClearRect.rect.extent.width > 0 && cop.mClearRect.rect.extent.height > 0)
            {
                sPassForceInline = true; // preserve strict ordering around mid-pass clears
                sPassOps.push_back(cop);
            }
            return;
        }

        if (sActiveTarget)
        {
            endActivePass();
        }
        if (clr_color)
        {
            sPendingClearColor = true;
            memcpy(sPendingClearRGBA, rgba, 4 * sizeof(float));
        }
        if (clr_depth)
        {
            sPendingClearDepth = true;
            sPendingClearDepthVal = depth;
        }
    }

    void onShaderLinked(LLGLSLShader* shader)
    {
        if (!shader || !sActive || !sDevice.hasDevice()) return;
        if (shader->mWolfVk)
        {
            invalidateShadowBatchProgram(&shader->mWolfVk->program());
            delete shader->mWolfVk;
            shader->mWolfVk = nullptr;
        }
        std::string err;
        shader->mWolfVk = WolfVkShaderMirror::create(sDevice, shader, sSpvDir, err);
        if (!shader->mWolfVk)
        {
            warnOnce("spv_missing_" + shader->mName, "No SPIR-V for shader '" + shader->mName + "': " + err);
        }
    }

    bool interceptDraw(const LLVertexBuffer* vb, U32 mode, U32 start, U32 end, U32 count, U32 indices_offset, bool indexed)
    {
        (void)end;
        if (!sActive || !vb || count == 0) return true;

        LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;
        if (!shader || !shader->mWolfVk || !shader->mWolfVk->program().isValid())
        {
            warnOnce("draw_no_shader_" + (shader ? shader->mName : std::string("null")),
                     "Skipped draw: shader '" + (shader ? shader->mName : std::string("<none>")) + "' has no SPIR-V program");
            return true;
        }

        if (!ensureFrameOpen())
        {
            return true;
        }

        VkBuffer vbuf = sBufferStore.vkBuffer(vb->getWolfVkVertexHandle());
        if (vbuf == VK_NULL_HANDLE)
        {
            warnOnce("draw_no_vbuf", "Skipped draw: vertex buffer has no Vulkan buffer");
            return true;
        }
        VkBuffer ibuf = VK_NULL_HANDLE;
        if (indexed)
        {
            ibuf = sBufferStore.vkBuffer(vb->getWolfVkIndexHandle());
            if (ibuf == VK_NULL_HANDLE)
            {
                warnOnce("draw_no_ibuf", "Skipped draw: indexed draw has no Vulkan index buffer");
                return true;
            }
        }

        WolfVkShaderMirror& mirror = *shader->mWolfVk;
        const WolfVkProgram& program = mirror.program();
        const auto& resolved_samplers = mirror.resolvedSamplers();

        // Flush previous render pass if target changed before checking sampled attachment layouts!
        {
            WolfVkRenderTarget* dest_rt = currentTarget();
            if (sActiveTarget && sActiveTarget != dest_rt)
            {
                endActivePass();
            }
            if (!resolved_samplers.empty())
            {
                auto isDestAttachment = [&](WolfVkImage* img) -> bool {
                    if (!dest_rt || !img) return false;
                    if (dest_rt->depthImage() == img) return true;
                    for (uint32_t i = 0; i < dest_rt->getNumTextures(); ++i)
                    {
                        if (dest_rt->image(i) == img) return true;
                    }
                    return false;
                };
                for (const WolfVkShaderMirror::ResolvedSampler& rs : resolved_samplers)
                {
                    for (uint32_t a = 0; a < rs.mArraySize; ++a)
                    {
                        LLTexUnit* unit = gGL.getTexUnit(static_cast<U32>(rs.mChannel) + a);
                        WolfVkImage* img = unit ? sTextureStore.find(unit->mWolfVkName) : nullptr;
                        if (img && img->isValid() && img->layout() != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && !isDestAttachment(img))
                        {
                            endActivePass();
                            sFrameCmdImages.insert(img->image());
                            img->transition(sFrameCmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
                        }
                    }
                }
            }
        }

        if (!ensureRenderPassOpen() || !sActiveTarget)
        {
            return true;
        }

        // Build pipeline state from CPU-tracked LLGLState / LLGLDepthTest / LLRender
        WolfVkPipelineState state;
        LLRender::eBlendFactor cs, cd, as, ad;
        gGL.wolfVkBlendFactors(cs, cd, as, ad);
        state.mBlendEnable = LLGLState::wolfVkIsEnabled(GL_BLEND);
        state.mBlendSrcColor = static_cast<uint8_t>(cs);
        state.mBlendDstColor = static_cast<uint8_t>(cd);
        state.mBlendSrcAlpha = static_cast<uint8_t>(as);
        state.mBlendDstAlpha = static_cast<uint8_t>(ad);
        const bool* cm = gGL.wolfVkColorMask();
        for (int i = 0; i < 4; ++i) state.mColorMask[i] = cm[i];
        state.mDepthTest = LLGLDepthTest::wolfVkDepthEnabled();
        state.mDepthWrite = LLGLDepthTest::wolfVkWriteEnabled();
        state.mDepthFunc = LLGLDepthTest::wolfVkDepthFunc();
        state.mCullFace = LLGLState::wolfVkIsEnabled(GL_CULL_FACE);
        state.mCullFront = WolfVkState::current().mCullFront;
        state.mWireframe = WolfVkState::current().mWireframe;
        state.mDepthClamp = LLGLState::wolfVkIsEnabled(GL_DEPTH_CLAMP) && sDevice.depthClamp();
        if (LLGLState::wolfVkIsEnabled(GL_POLYGON_OFFSET_FILL))
        {
            state.mDepthBiasSlope = WolfVkState::current().mPolyOffsetFactor;
            state.mDepthBiasConstant = WolfVkState::current().mPolyOffsetUnits;
        }
        state.mTopology = static_cast<uint8_t>(mode);
        state.mAttributeMask = program.attributeMask() & vb->getTypeMask();
        state.mColorAttachments = sActiveTarget->getNumTextures();
        state.mCoverageAttachment = false;

        // Point 5: Per-shader pipeline lookup fast-cache (plus 1-entry hot cache)
        const VkRenderPass target_pass = sActiveTarget->renderPass();
        VkPipeline pipeline = VK_NULL_HANDLE;
        if (sLastPipeHandle != VK_NULL_HANDLE && sLastPipeProg == &program && sLastPipePass == target_pass && state == sLastPipeState)
        {
            pipeline = sLastPipeHandle;
        }
        else
        {
            pipeline = mirror.cachedPipeline(state, target_pass, sPipelines);
            if (pipeline != VK_NULL_HANDLE)
            {
                sLastPipeProg = &program;
                sLastPipePass = target_pass;
                sLastPipeState = state;
                sLastPipeHandle = pipeline;
            }
        }
        if (pipeline == VK_NULL_HANDLE)
        {
            warnOnce("draw_no_pipe_" + shader->mName, "Skipped draw: pipeline creation failed for '" + shader->mName + "'");
            return true;
        }

        if (mode == LLRender::POINTS)
        {
            const float ps = WolfVkState::current().mPointSize;
            mirror.set(WolfVkShaderMirror::POINT_SIZE_UNIFORM, WolfVkShaderMirror::Kind::F1, 1, &ps);
        }

        // Resolve set 0 / push constants
        const uint8_t* push_data = nullptr;
        uint32_t push_size = 0;
        VkDescriptorSet set0 = VK_NULL_HANDLE;
        uint32_t set0_offsets[2] = { 0, 0 };
        uint32_t set0_off_cnt = 0;
        if (!mirror.prepareUniforms(sUniforms, push_data, push_size, set0, set0_offsets, set0_off_cnt))
        {
            return true;
        }

        // Resolve set 2 (UBO blocks) - zero heap alloc, cached per (buffer, epoch)
        VkDescriptorSet set2 = VK_NULL_HANDLE;
        uint32_t set2_offsets[8] = {};
        uint32_t set2_off_cnt = 0;
        const auto& blocks = program.blocks();
        if (!blocks.empty() && blocks.size() <= 8)
        {
            uint32_t block_offsets[8];
            const uint64_t cur_epoch = sUniforms.epoch();
            bool ok_blocks = true;
            for (size_t bi = 0; bi < blocks.size(); ++bi)
            {
                const WolfVkProgram::BlockBinding& blk = blocks[bi];
                const uint32_t need = std::min<uint32_t>(65536u, std::max<uint32_t>(16u, blk.mSize));
                GLuint buf_id = (blk.mBinding < LLGLSLShader::NUM_UNIFORM_BLOCKS) ? sBoundUbo[blk.mBinding] : 0;
                VkDeviceSize off = 0;
                if (buf_id != 0)
                {
                    auto& entry = sUboBytes[buf_id];
                    if (entry.mPushedEpoch == cur_epoch && entry.mPushedSize >= need)
                    {
                        off = entry.mPushedOffset;
                    }
                    else
                    {
                        if (entry.mBytes.size() < need) entry.mBytes.resize(need, 0);
                        if (!sUniforms.push(entry.mBytes.data(), need, off))
                        {
                            ok_blocks = false;
                            break;
                        }
                        entry.mPushedEpoch = cur_epoch;
                        entry.mPushedSize = need;
                        entry.mPushedOffset = off;
                    }
                }
                else
                {
                    if (sZeroUboEpoch == cur_epoch && sZeroUboSize >= need)
                    {
                        off = sZeroUboOffset;
                    }
                    else
                    {
                        if (!sUniforms.push(sZeroUboBytes, need, off))
                        {
                            ok_blocks = false;
                            break;
                        }
                        sZeroUboEpoch = cur_epoch;
                        sZeroUboSize = need;
                        sZeroUboOffset = off;
                    }
                }
                block_offsets[bi] = static_cast<uint32_t>(off);
            }
            if (ok_blocks)
            {
                mirror.prepareBlocks(sUniforms, block_offsets, static_cast<uint32_t>(blocks.size()), set2, set2_offsets, set2_off_cnt);
            }
        }

        // Resolve set 1 (samplers) - stack-allocated, pre-resolved channels, lock-free O(1) texture lookup
        VkDescriptorSet set1 = VK_NULL_HANDLE;
        if (!resolved_samplers.empty())
        {
            VkDescriptorImageInfo fallback_2d{};
            fallback_2d.sampler = sSamplers.get(WolfVkSamplerCache::TFO_BILINEAR, WolfVkSamplerCache::TAM_CLAMP, false);
            fallback_2d.imageView = sFallbackWhite.view();
            fallback_2d.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            const uint32_t num_color_att = sActiveTarget->getNumTextures();
            WolfVkImage* active_depth = state.mDepthWrite ? sActiveTarget->depthImage() : nullptr;
            auto isForbiddenAttachment = [&](WolfVkImage* img) -> bool {
                if (!img) return false;
                if (active_depth == img) return true;
                for (uint32_t i = 0; i < num_color_att; ++i)
                {
                    if (sActiveTarget->image(i) == img) return true;
                }
                return false;
            };

            const uint32_t max_binding = mirror.maxSamplerBinding();
            VkDescriptorImageInfo stack_infos[64];
            std::vector<VkDescriptorImageInfo> heap_infos;
            VkDescriptorImageInfo* infos = stack_infos;
            if (max_binding > 64)
            {
                heap_infos.assign(max_binding, fallback_2d);
                infos = heap_infos.data();
            }
            else
            {
                for (uint32_t i = 0; i < max_binding; ++i) stack_infos[i] = fallback_2d;
            }

            for (const WolfVkShaderMirror::ResolvedSampler& rs : resolved_samplers)
            {
                const WolfVkImage::Kind need_kind = expectedImageKind(rs.mDim, rs.mArrayed);
                WolfVkImage* fb_img = fallbackForKind(need_kind);
                VkDescriptorImageInfo def_info{};
                def_info.imageView = (fb_img && fb_img->isValid()) ? fb_img->view() : sFallbackWhite.view();
                def_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                def_info.sampler = sSamplers.get(WolfVkSamplerCache::TFO_BILINEAR, WolfVkSamplerCache::TAM_CLAMP, false, rs.mShadow);

                for (uint32_t a = 0; a < rs.mArraySize && (rs.mBinding + a) < max_binding; ++a)
                {
                    VkDescriptorImageInfo elem = def_info;
                    LLTexUnit* unit = gGL.getTexUnit(static_cast<U32>(rs.mChannel) + a);
                    WolfVkImage* img = unit ? sTextureStore.find(unit->mWolfVkName) : nullptr;
                    if (img && img->isValid() && img->view() != VK_NULL_HANDLE &&
                        img->kind() == need_kind && !isForbiddenAttachment(img))
                    {
                        elem.imageView = img->view();
                        const auto filter = static_cast<WolfVkSamplerCache::Filter>(unit->mWolfVkFilter);
                        const auto addr = static_cast<WolfVkSamplerCache::Address>(unit->mWolfVkAddress);
                        elem.sampler = sSamplers.get(filter, addr, img->mipLevels() > 1, rs.mShadow);
                    }
                    infos[rs.mBinding + a] = elem;
                }
            }
            mirror.prepareSamplers(sUniforms, infos, max_binding, fallback_2d, set1);
        }

        // Queue PassOp for inline or 4-thread parallel secondary command buffer execution
        sPassOps.emplace_back();
        PassOp& op = sPassOps.back();
        op.mPipeline = pipeline;
        op.mLayout = program.pipelineLayout();
        if (push_data && push_size > 0 && push_size <= sizeof(op.mPushBytes))
        {
            op.mPushSize = static_cast<uint16_t>(push_size);
            memcpy(op.mPushBytes, push_data, push_size);
            ++sPerf.mPushConstDrawsThisFrame;
        }
        op.mSet0 = set0;
        op.mSet0OffCount = static_cast<uint8_t>(set0_off_cnt);
        op.mSet0Offsets[0] = set0_offsets[0];
        op.mSet0Offsets[1] = set0_offsets[1];
        op.mSet1 = set1;
        op.mSet2 = set2;
        op.mSet2OffCount = static_cast<uint8_t>(set2_off_cnt);
        if (set2_off_cnt > 0) memcpy(op.mSet2Offsets, set2_offsets, set2_off_cnt * sizeof(uint32_t));
        computeTargetViewportAndScissor(op.mViewport, op.mScissor);

        uint8_t nbind = 0;
        for (uint32_t t = 0; t < LLVertexBuffer::TYPE_MAX; ++t)
        {
            if (state.mAttributeMask & (1u << t))
            {
                uint32_t src_t = t;
                if (t == LLVertexBuffer::TYPE_COLOR && (state.mAttributeMask & (1u << LLVertexBuffer::TYPE_EMISSIVE)))
                {
                    src_t = LLVertexBuffer::TYPE_EMISSIVE;
                }
                op.mVBufs[nbind] = vbuf;
                op.mVOffs[nbind] = (src_t == LLVertexBuffer::TYPE_TEXTURE_INDEX)
                    ? (vb->getOffset(LLVertexBuffer::TYPE_VERTEX) + 12u)
                    : vb->getOffset(static_cast<LLVertexBuffer::AttributeType>(src_t));
                ++nbind;
            }
        }
        op.mNumVBufs = nbind;

        const uint64_t draw_epoch = sUniforms.epoch();
        if (indexed)
        {
            op.mKind = PassOpKind::DrawIndexed;
            op.mIBuf = ibuf;
            op.mIType = (vb->getNumIndices() > 0 && vb->getIndicesSize() == vb->getNumIndices() * 4)
                      ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
            op.mCount = count;
            op.mFirst = indices_offset;
            sBufferStore.markDrawn(vb->getWolfVkIndexHandle(), draw_epoch);
        }
        else
        {
            op.mKind = PassOpKind::Draw;
            op.mCount = count;
            op.mFirst = start;
        }
        sBufferStore.markDrawn(vb->getWolfVkVertexHandle(), draw_epoch);
        ++sPassDrawCount;
        ++sDrawsThisFrame;
        return true;
    }

    // -------------------------------------------------------------------------
    // 6. Populate gGLManager from VkPhysicalDevice (zero OpenGL calls)
    // -------------------------------------------------------------------------
    void populateGLManagerFromVulkan()
    {
        const WolfVkPhysicalDeviceInfo& info = sDevice.chosen();
        gGLManager.mInited = true;
        gGLManager.mIsNativeVulkan = true;
        gGLManager.initNativeVulkanNoopGLPointers();
        gGLManager.mHasRequirements = true;
        gGLManager.mGLVersion = 4.6f;
        gGLManager.mGLSLVersionMajor = 4;
        gGLManager.mGLSLVersionMinor = 60;
        gGLManager.mGLVersionString = "4.6 (Native Vulkan " + info.apiVersionString() + ")";
        gGLManager.mGLRenderer = info.name();
        gGLManager.mIsNVIDIA = (info.mProperties.vendorID == WolfVkDevice::VENDOR_NVIDIA);
        gGLManager.mIsAMD = (info.mProperties.vendorID == WolfVkDevice::VENDOR_AMD);
        gGLManager.mIsIntel = (info.mProperties.vendorID == WolfVkDevice::VENDOR_INTEL);
        gGLManager.mIsApple = (info.mProperties.vendorID == WolfVkDevice::VENDOR_APPLE);
        gGLManager.mGLVendor = gGLManager.mIsNVIDIA ? "NVIDIA CORPORATION"
                             : gGLManager.mIsAMD    ? "AMD"
                             : gGLManager.mIsIntel  ? "INTEL"
                             : gGLManager.mIsApple  ? "APPLE" : "VULKAN";
        gGLManager.mGLVendorShort = gGLManager.mIsNVIDIA ? "NVIDIA"
                                  : gGLManager.mIsAMD    ? "ATI"
                                  : gGLManager.mIsIntel  ? "INTEL"
                                  : gGLManager.mIsApple  ? "APPLE" : "VULKAN";

        // Compute VRAM in MB from device-local heaps
        VkDeviceSize vram_bytes = 0;
        for (uint32_t i = 0; i < info.mMemory.memoryHeapCount; ++i)
        {
            if (info.mMemory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            {
                vram_bytes += info.mMemory.memoryHeaps[i].size;
            }
        }
        gGLManager.mVRAM = static_cast<U32>(std::max<VkDeviceSize>(512, vram_bytes / (1024ull * 1024ull)));
        gGLManager.mVRAMDetected = static_cast<S32>(gGLManager.mVRAM);

        // Capability limits from VkPhysicalDeviceLimits
        gGLManager.mGLMaxTextureSize = static_cast<S32>(std::min<uint32_t>(16384u, info.mProperties.limits.maxImageDimension2D));
        gGLManager.mNumTextureImageUnits = static_cast<S32>(std::min<uint32_t>(32u, info.mProperties.limits.maxPerStageDescriptorSamplers));
        gGLManager.mMaxVaryingVectors = static_cast<S32>(std::max<uint32_t>(16u, info.mProperties.limits.maxVertexOutputComponents / 4u));
        gGLManager.mMaxUniformBlockSize = static_cast<S32>(std::min<uint32_t>(65536u, info.mProperties.limits.maxUniformBufferRange));
        gGLManager.mGLMaxVertexRange = 1048576;
        gGLManager.mGLMaxIndexRange = static_cast<S32>(std::min<uint32_t>(1048576u, info.mProperties.limits.maxDrawIndexedIndexValue));
        gGLManager.mMaxAnisotropy = info.mProperties.limits.maxSamplerAnisotropy;
        gGLManager.mHasAnisotropic = info.mProperties.limits.maxSamplerAnisotropy > 1.0f;
        gGLManager.mHasCubeMapArray = sDevice.cubeArrays();
        gGLManager.mHasTransformFeedback = false;
        gGLManager.mHasDebugOutput = false;
        LLRenderTarget::sUseFBO = true;
        LLRender::sGLCoreProfile = true;

        LLGLState::initClass();
    }
}

namespace WolfVkNative
{

bool init(LLWindow* window, std::string& error)
{
    if (sActive) return true;
    sWindow = window;
    sSpvDir = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "shaders", "spv");

    std::string stamp_line;
    if (!WolfVkShaderMirror::checkStore(sSpvDir, stamp_line))
    {
        error = "SPIR-V shader store invalid: " + stamp_line;
        LL_WARNS("Vulkan") << "[Native] " << error << LL_ENDL;
        return false;
    }
    LL_INFOS("Vulkan") << "[Native] " << stamp_line << LL_ENDL;

    WolfVkDevice::Config cfg;
    cfg.mValidation = gSavedSettings.getBOOL("WolfVulkanValidation");
    cfg.mDeviceOverride = gSavedSettings.getString("WolfVulkanDevice");
    cfg.mFramesInFlight = 3;
    cfg.mApplicationName = "WolfViewer";

    if (!sDevice.createInstance(cfg, error))
    {
        return false;
    }

    // Native Vulkan window has NO OpenGL context, so we attach directly to the window (no child window needed).
    sSurface = WolfVkSurface::create(sDevice.instance(), window, /*shared_x11_window=*/true, error);
    if (sSurface == VK_NULL_HANDLE)
    {
        sDevice.destroy();
        return false;
    }

    if (!sDevice.createDevice(sSurface, error))
    {
        WolfVkSurface::destroy(sDevice.instance(), sSurface);
        sSurface = VK_NULL_HANDLE;
        sDevice.destroy();
        return false;
    }

    sVulkanLine = sDevice.aboutLine();

    LLCoordWindow sz;
    window->getSize(&sz);
    const uint32_t w = static_cast<uint32_t>(std::max<S32>(1, sz.mX));
    const uint32_t h = static_cast<uint32_t>(std::max<S32>(1, sz.mY));
    const bool vsync = gSavedSettings.getBOOL("RenderVSyncEnable");

    if (!sSwapchain.create(sDevice, sSurface, w, h, vsync, error))
    {
        shutdown();
        return false;
    }

    if (!sUniforms.init(sDevice, 64u * 1024u * 1024u, error))
    {
        shutdown();
        return false;
    }

    if (!sStagingRing.init(sDevice, 128u * 1024u * 1024u, error))
    {
        shutdown();
        return false;
    }
    sDevice.setStagingRing(&sStagingRing);

    if (!sIndirectRing.init(sDevice, 16u * 1024u * 1024u, error))
    {
        shutdown();
        return false;
    }

    initOcclusionPool();
    initTimestampPool();
    sParallelRecorder.init();

    const std::string cache_path = gDirUtilp->getExpandedFilename(LL_PATH_CACHE, "wolfvk_pipelines.bin");
    sPipelines.init(sDevice, cache_path);
    sSamplers.init(sDevice, sDevice.chosen().mProperties.limits.maxSamplerAnisotropy);

    // 1x1 fallback textures for every SPIR-V image dimension (2D, Cube, 2DArray, 3D, CubeArray)
    const WolfVkImage::Format white_fmt = WolfVkImage::formatFor(GL_RGBA8, GL_UNSIGNED_BYTE);
    const uint8_t white_px[4] = { 255, 255, 255, 255 };
    const uint8_t env_px[4] = { 64, 70, 82, 255 };
    auto initFallback = [&](WolfVkImage& dst, WolfVkImage::Kind kind, uint32_t layers, const uint8_t px[4]) -> bool
    {
        if (!dst.create(sDevice, kind, white_fmt, 1, 1, layers, 1,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false, error))
        {
            return false;
        }
        for (uint32_t l = 0; l < layers; ++l)
        {
            if (!dst.uploadSubRect(VK_NULL_HANDLE, 0, l, 0, 0, 1, 1, 1, px, GL_RGBA, GL_UNSIGNED_BYTE, false, error))
            {
                return false;
            }
        }
        return true;
    };
    if (!initFallback(sFallbackWhite, WolfVkImage::Kind::TEXTURE_2D, 1, white_px) ||
        !initFallback(sFallbackCube, WolfVkImage::Kind::TEXTURE_CUBE, 6, env_px) ||
        !initFallback(sFallback2DArray, WolfVkImage::Kind::TEXTURE_2D_ARRAY, 1, white_px) ||
        !initFallback(sFallback3D, WolfVkImage::Kind::TEXTURE_3D, 1, white_px))
    {
        shutdown();
        return false;
    }
    if (sDevice.cubeArrays())
    {
        std::string cube_err;
        initFallback(sFallbackCubeArray, WolfVkImage::Kind::TEXTURE_CUBE_ARRAY, 6, env_px);
    }

    if (!buildWindowTarget(sSwapchain.extent().width, sSwapchain.extent().height, error))
    {
        shutdown();
        return false;
    }

    populateGLManagerFromVulkan();

    // Install native hooks into llrender (zero GL fallback)
    LLVertexBuffer::setWolfVkStore(&sBufferStore);
    LLVertexBuffer::sWolfVkInterceptDraw = &interceptDraw;
    LLGLSLShader::sWolfVkOnLinked = &onShaderLinked;
    LLGLSLShader::sWolfVkOnUniformBufferData = &onUniformBufferData;
    LLGLSLShader::sWolfVkOnUniformBufferBind = &onUniformBufferBind;
    LLImageGL::sWolfVkNativeStore = &sTextureStore;
    LLImageGL::sWolfVkTextureOwner = &sTextureStore;
    LLRenderTarget::sWolfVkNativeOwner = &sRenderTargetOwner;
    LLGLState::sWolfVkClear = &onNativeClear;

    sActive = true;
    LL_INFOS("Vulkan") << "[Native] Native Vulkan renderer active (zero OpenGL context): "
                       << sVulkanLine << ", surface " << WolfVkSurface::kind()
                       << ", swapchain " << sSwapchain.extent().width << "x" << sSwapchain.extent().height << LL_ENDL;
    return true;
}

bool isActive()
{
    return sActive;
}

bool present()
{
    if (!sActive) return false;
    const auto t_pres0 = std::chrono::steady_clock::now();

    if (!sFrameOpen)
    {
        if (!ensureRenderPassOpen())
        {
            return true;
        }
    }

    endActivePass();

    const uint32_t slot = sDevice.frameIndex();
    const double frame_uniform_kb = static_cast<double>(sUniforms.used()) / 1024.0;
    const double frame_staging_kb = static_cast<double>(sStagingRing.used()) / 1024.0;
    const uint32_t frame_occ_issued = static_cast<uint32_t>(sRecordedQueriesThisFrame.size());

    std::string err;
    bool out_of_date = false;
    bool timed_out = false;
    WolfVkDevice::Frame& frame = sDevice.currentFrame();
    const bool acquired = sSwapchain.acquire(frame.mAcquire, sSwapImageIndex, out_of_date, timed_out, err) && !out_of_date && !timed_out;
    if (out_of_date)
    {
        sResizePending = true;
    }

    bool submitted_ok = false;
    if (acquired)
    {
        // Blit window target color image to swapchain image
        WolfVkImage* src_img = sWindowTarget.image(0);
        VkImage dst_img = sSwapchain.image(sSwapImageIndex);
        if (src_img && src_img->isValid() && dst_img != VK_NULL_HANDLE)
        {
            src_img->transition(sFrameCmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);

            VkImageMemoryBarrier to_dst{};
            to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            to_dst.srcAccessMask = 0;
            to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_dst.image = dst_img;
            to_dst.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            gVk.vkCmdPipelineBarrier(sFrameCmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     0, 0, nullptr, 0, nullptr, 1, &to_dst);

            // sWindowTarget is rendered in OpenGL row order (row 0 = bottom); flip Y once here into the top-down swapchain.
            VkImageBlit blit{};
            blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            blit.srcOffsets[0] = { 0, static_cast<int32_t>(src_img->height()), 0 };
            blit.srcOffsets[1] = { static_cast<int32_t>(src_img->width()), 0, 1 };
            blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            blit.dstOffsets[0] = { 0, 0, 0 };
            blit.dstOffsets[1] = { static_cast<int32_t>(sSwapchain.extent().width), static_cast<int32_t>(sSwapchain.extent().height), 1 };
            gVk.vkCmdBlitImage(sFrameCmd,
                               src_img->image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               dst_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &blit, VK_FILTER_LINEAR);

            VkImageMemoryBarrier to_present{};
            to_present.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            to_present.dstAccessMask = 0;
            to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            to_present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_present.image = dst_img;
            to_present.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            gVk.vkCmdPipelineBarrier(sFrameCmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                     0, 0, nullptr, 0, nullptr, 1, &to_present);

            src_img->transition(sFrameCmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        }

        if (VkCommandBuffer pre_cmd = sDevice.prePassTransferCmd())
        {
            writeTimestamp(pre_cmd, slot, TS_UPLOADS_DONE, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        }
        writeTimestamp(sFrameCmd, slot, TS_FRAME_END, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

        VkSemaphore render_done = sSwapchain.renderDone(sSwapImageIndex);
        if (!sDevice.endFrame(/*wait_acquire=*/true, render_done, err))
        {
            warnOnce("end_frame_fail", "endFrame failed: " + err);
            onFrameQueriesSubmitted(false);
        }
        else
        {
            submitted_ok = true;
            ++sSubmittedCmdCounter;
            onFrameQueriesSubmitted(true);
            bool pres_out_of_date = false;
            if (!sSwapchain.present(sDevice.queue(), render_done, sSwapImageIndex, pres_out_of_date, err) || pres_out_of_date)
            {
                sResizePending = true;
            }
        }
    }
    else
    {
        if (VkCommandBuffer pre_cmd = sDevice.prePassTransferCmd())
        {
            writeTimestamp(pre_cmd, slot, TS_UPLOADS_DONE, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        }
        writeTimestamp(sFrameCmd, slot, TS_FRAME_END, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        submitted_ok = sDevice.endFrame(/*wait_acquire=*/false, VK_NULL_HANDLE, err);
        if (submitted_ok) ++sSubmittedCmdCounter;
        onFrameQueriesSubmitted(submitted_ok);
    }
    if (slot < MAX_TS_FRAME_SLOTS)
    {
        sTsSubmittedMask[slot] = submitted_ok ? sTsRecordedMask[slot] : 0;
    }

    const auto t_pres1 = std::chrono::steady_clock::now();
    const double pres_ms = std::chrono::duration<double, std::milli>(t_pres1 - t_pres0).count();
    if (sPerf.mHasLastPresent)
    {
        const double cpu_frame_ms = std::chrono::duration<double, std::milli>(t_pres1 - sPerf.mLastPresentEnd).count();
        ++sPerf.mWinFrames;
        sPerf.mWinCpuFrameMs += cpu_frame_ms;
        sPerf.mWinCpuFenceMs += sPerf.mFenceWaitMsThisFrame;
        sPerf.mWinCpuDrawMs += sPerf.mVkDrawMsThisFrame;
        sPerf.mWinCpuPresentMs += pres_ms;
        sPerf.mWinDraws += sDrawsThisFrame;
        sPerf.mWinPushConstDraws += sPerf.mPushConstDrawsThisFrame;
        sPerf.mWinIndirectBatches += sPerf.mIndirectBatchesThisFrame;
        sPerf.mWinPasses += sPerf.mPassesThisFrame;
        sPerf.mWinMtPasses += sPerf.mMtPassesThisFrame;
        sPerf.mWinMtDraws += sPerf.mMtDrawsThisFrame;
        sPerf.mWinOccIssued += frame_occ_issued;
        sPerf.mWinOccRead += sPerf.mOccReadThisFrame;
        sPerf.mWinOccCulled += sPerf.mOccCulledThisFrame;
        sPerf.mWinUniformKb += frame_uniform_kb;
        sPerf.mWinStagingKb += frame_staging_kb;

        const double elapsed_sec = std::chrono::duration<double>(t_pres1 - sPerf.mLastLogTime).count();
        if (elapsed_sec >= 10.0 && sPerf.mWinFrames > 0)
        {
            const double inv_f = 1.0 / static_cast<double>(sPerf.mWinFrames);
            const double inv_g = (sPerf.mWinGpuSamples > 0) ? (1.0 / static_cast<double>(sPerf.mWinGpuSamples)) : 0.0;
            const double fps = static_cast<double>(sPerf.mWinFrames) / elapsed_sec;
            const double cpu_tot = sPerf.mWinCpuFrameMs * inv_f;
            const double cpu_fence = sPerf.mWinCpuFenceMs * inv_f;
            const double cpu_draw = sPerf.mWinCpuDrawMs * inv_f;
            const double cpu_pres = sPerf.mWinCpuPresentMs * inv_f;
            const double cpu_outside = std::max(0.0, cpu_tot - cpu_fence - cpu_draw - cpu_pres);
            const double occ_pct = (sPerf.mWinOccRead > 0)
                ? (100.0 * static_cast<double>(sPerf.mWinOccCulled) / static_cast<double>(sPerf.mWinOccRead))
                : 0.0;
            const double vram_mb = static_cast<double>(sBufferStore.vramBytesUsed()) / (1024.0 * 1024.0);
            const double host_mb = static_cast<double>(sBufferStore.hostBytesUsed()) / (1024.0 * 1024.0);

            LL_INFOS("VulkanPerf")
                << llformat("fps=%.1f | GPU ms: total=%.2f (upload=%.2f shadow=%.2f world+post=%.2f ui+blit=%.2f) | "
                            "CPU ms: frame=%.2f (outside_vk=%.2f vk_draw=%.2f fence_wait=%.2f present=%.2f) | "
                            "draws/f=%.0f (push_const=%.0f mt_draws=%.0f indirect=%.1f) passes/f=%.1f (mt=%.1f) | "
                            "occ/f=%.0f (culled=%.1f%%) | geom_vram=%.1fMB (%llu bufs) geom_host=%.1fMB | "
                            "ring_kb/f: ubo=%.0f staging=%.0f",
                            fps,
                            sPerf.mWinGpuTotalMs * inv_g,
                            sPerf.mWinGpuUploadMs * inv_g,
                            sPerf.mWinGpuShadowMs * inv_g,
                            sPerf.mWinGpuWorldMs * inv_g,
                            sPerf.mWinGpuUiMs * inv_g,
                            cpu_tot, cpu_outside, cpu_draw, cpu_fence, cpu_pres,
                            static_cast<double>(sPerf.mWinDraws) * inv_f,
                            static_cast<double>(sPerf.mWinPushConstDraws) * inv_f,
                            static_cast<double>(sPerf.mWinMtDraws) * inv_f,
                            static_cast<double>(sPerf.mWinIndirectBatches) * inv_f,
                            static_cast<double>(sPerf.mWinPasses) * inv_f,
                            static_cast<double>(sPerf.mWinMtPasses) * inv_f,
                            static_cast<double>(sPerf.mWinOccIssued) * inv_f,
                            occ_pct,
                            vram_mb, static_cast<unsigned long long>(sBufferStore.promotedCount()), host_mb,
                            sPerf.mWinUniformKb * inv_f,
                            sPerf.mWinStagingKb * inv_f)
                << LL_ENDL;
            {
                uint32_t one_time = 0;
                double one_time_ms = 0.0;
                sDevice.takeOneTimeStats(one_time, one_time_ms);
                if (one_time > 0)
                {
                    LL_INFOS("VulkanPerf") << llformat("blocking one-time submits: %.2f a frame, %.2f ms a frame",
                                                       static_cast<double>(one_time) * inv_f, one_time_ms * inv_f) << LL_ENDL;
                }
            }

            sPerf.mWinFrames = 0;
            sPerf.mWinGpuSamples = 0;
            sPerf.mWinCpuFrameMs = sPerf.mWinCpuFenceMs = sPerf.mWinCpuDrawMs = sPerf.mWinCpuPresentMs = 0.0;
            sPerf.mWinGpuTotalMs = sPerf.mWinGpuUploadMs = sPerf.mWinGpuShadowMs = sPerf.mWinGpuWorldMs = sPerf.mWinGpuUiMs = 0.0;
            sPerf.mWinDraws = sPerf.mWinPushConstDraws = sPerf.mWinIndirectBatches = 0;
            sPerf.mWinPasses = sPerf.mWinMtPasses = sPerf.mWinMtDraws = 0;
            sPerf.mWinOccIssued = sPerf.mWinOccRead = sPerf.mWinOccCulled = 0;
            sPerf.mWinUniformKb = sPerf.mWinStagingKb = 0.0;
            sPerf.mLastLogTime = t_pres1;
        }
    }
    sPerf.mLastPresentEnd = t_pres1;
    sPerf.mHasLastPresent = true;

    sFrameOpen = false;
    sFrameCmd = VK_NULL_HANDLE;
    sTextureStore.clearInFlightFrameMip0();
    sDrawsLastFrame = sDrawsThisFrame;
    sDrawsThisFrame = 0;
    sUniforms.forgetBound();
    sPipelines.saveIfDue(LLTimer::getTotalSeconds());

    // Optional env-gated frame dump for visual verification (WOLF_VK_DUMP_FRAME=/path/to/out.ppm)
    static const char* sDumpPath = std::getenv("WOLF_VK_DUMP_FRAME");
    if (sDumpPath && *sDumpPath)
    {
        static const double sStartSec = LLTimer::getTotalSeconds();
        static const double sAfterSec = []() {
            const char* env_sec = std::getenv("WOLF_VK_DUMP_AFTER_SEC");
            return (env_sec && *env_sec) ? std::max(0.5, std::atof(env_sec)) : 8.0;
        }();
        static bool sDumped = false;
        if (!sDumped && (LLTimer::getTotalSeconds() - sStartSec) >= sAfterSec && sWindowTarget.isComplete())
        {
            sDumped = true;
            std::vector<uint8_t> px;
            std::string rerr;
            if (sWindowTarget.readPixels(0, px, rerr))
            {
                const uint32_t rw = sWindowTarget.getWidth();
                const uint32_t rh = sWindowTarget.getHeight();
                if (px.size() >= static_cast<size_t>(rw) * rh * 4u)
                {
                    if (FILE* fp = std::fopen(sDumpPath, "wb"))
                    {
                        std::fprintf(fp, "P6\n%u %u\n255\n", rw, rh);
                        std::vector<uint8_t> row_rgb(static_cast<size_t>(rw) * 3u);
                        for (uint32_t y = 0; y < rh; ++y)
                        {
                            const uint32_t src_y = rh - 1u - y; // OpenGL bottom-up -> top-down
                            const uint8_t* src = px.data() + (static_cast<size_t>(src_y) * rw) * 4u;
                            for (uint32_t x = 0; x < rw; ++x)
                            {
                                row_rgb[x * 3u + 0u] = src[x * 4u + 0u];
                                row_rgb[x * 3u + 1u] = src[x * 4u + 1u];
                                row_rgb[x * 3u + 2u] = src[x * 4u + 2u];
                            }
                            std::fwrite(row_rgb.data(), 1, row_rgb.size(), fp);
                        }
                        std::fclose(fp);
                        LL_INFOS("Vulkan") << "[Native] Dumped window frame " << rw << "x" << rh << " to " << sDumpPath << LL_ENDL;
                    }
                }
            }
        }
    }

    return true;
}

void endUnshownFrame()
{
    if (!sActive || !sFrameOpen) return;
    endActivePass();
    const uint32_t slot = sDevice.frameIndex();
    if (VkCommandBuffer pre_cmd = sDevice.prePassTransferCmd())
    {
        writeTimestamp(pre_cmd, slot, TS_UPLOADS_DONE, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    }
    writeTimestamp(sFrameCmd, slot, TS_FRAME_END, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    std::string err;
    const bool ok_end = sDevice.endFrame(/*wait_acquire=*/false, VK_NULL_HANDLE, err);
    if (ok_end) ++sSubmittedCmdCounter;
    onFrameQueriesSubmitted(ok_end);
    if (slot < MAX_TS_FRAME_SLOTS)
    {
        sTsSubmittedMask[slot] = ok_end ? sTsRecordedMask[slot] : 0;
    }
    sFrameOpen = false;
    sFrameCmd = VK_NULL_HANDLE;
    sTextureStore.clearInFlightFrameMip0();
    sDrawsLastFrame = sDrawsThisFrame;
    sDrawsThisFrame = 0;
    sUniforms.forgetBound();
}

void onResize()
{
    sResizePending = true;
}

void markPhaseShadowsDone()
{
    if (!sActive || !sFrameOpen || sFrameCmd == VK_NULL_HANDLE) return;
    endActivePass();
    writeTimestamp(sFrameCmd, sDevice.frameIndex(), TS_SHADOWS_DONE, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
}

void copyBoundTargetToImage3D(U32 dst_tex, S32 mip, S32 layer, S32 width, S32 height)
{
    if (!sActive || !sDevice.hasDevice() || dst_tex == 0 || mip < 0 || layer < 0 || width <= 0 || height <= 0)
    {
        return;
    }
    if (!ensureFrameOpen()) return;

    WolfVkRenderTarget* src_rt = currentTarget();
    WolfVkImage* src_img = src_rt ? src_rt->image(0) : nullptr;
    WolfVkImage* dst_img = sTextureStore.find(dst_tex);
    if (!src_img || !src_img->isValid() || !dst_img || !dst_img->isValid() || src_img == dst_img) return;

    const uint32_t u_mip = static_cast<uint32_t>(mip);
    const uint32_t u_layer = static_cast<uint32_t>(layer);
    if (u_mip >= dst_img->mipLevels() || u_layer >= dst_img->layers()) return;

    const uint32_t dst_w = std::max<uint32_t>(1u, dst_img->width() >> std::min<uint32_t>(u_mip, 31u));
    const uint32_t dst_h = std::max<uint32_t>(1u, dst_img->height() >> std::min<uint32_t>(u_mip, 31u));
    const uint32_t copy_w = std::min<uint32_t>(static_cast<uint32_t>(width), std::min(src_img->width(), dst_w));
    const uint32_t copy_h = std::min<uint32_t>(static_cast<uint32_t>(height), std::min(src_img->height(), dst_h));
    if (copy_w == 0 || copy_h == 0) return;

    endActivePass();
    sFrameCmdImages.insert(src_img->image());
    sFrameCmdImages.insert(dst_img->image());

    src_img->transition(sFrameCmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    dst_img->transition(sFrameCmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);

    VkImageBlit blit{};
    blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.srcOffsets[0] = { 0, 0, 0 };
    blit.srcOffsets[1] = { static_cast<int32_t>(copy_w), static_cast<int32_t>(copy_h), 1 };
    blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, u_mip, u_layer, 1 };
    blit.dstOffsets[0] = { 0, 0, 0 };
    blit.dstOffsets[1] = { static_cast<int32_t>(copy_w), static_cast<int32_t>(copy_h), 1 };
    gVk.vkCmdBlitImage(sFrameCmd,
                       src_img->image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       dst_img->image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit, VK_FILTER_NEAREST);

    src_img->transition(sFrameCmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    dst_img->transition(sFrameCmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
}

bool uploadTexture2DArrayLayer(U32 dst_tex, S32 layer, S32 width, S32 height, const void* rgba_pixels, bool generate_mips)
{
    if (!sActive || !sDevice.hasDevice() || dst_tex == 0 || layer < 0 || width <= 0 || height <= 0 || !rgba_pixels)
    {
        return false;
    }
    WolfVkImage* img = sTextureStore.find(dst_tex);
    if (!img || !img->isValid() || static_cast<uint32_t>(layer) >= img->layers())
    {
        return false;
    }
    ensureFrameOpen();
    std::string err;
    VkCommandBuffer xfer_cmd = pickTransferCmdForImage(img);
    if (xfer_cmd != VK_NULL_HANDLE)
    {
        if (!img->uploadSubRect(xfer_cmd, 0, static_cast<uint32_t>(layer),
                                0, 0, static_cast<uint32_t>(width), static_cast<uint32_t>(height), 0,
                                rgba_pixels, GL_RGBA, GL_UNSIGNED_BYTE, false, err))
        {
            warnOnce("tex2d_array_upload", "2D array layer upload failed: " + err);
            return false;
        }
        if (generate_mips && img->mipLevels() > 1)
        {
            img->generateMipmaps(xfer_cmd);
        }
        img->transition(xfer_cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        return true;
    }
    if (!img->uploadSubRect(VK_NULL_HANDLE, 0, static_cast<uint32_t>(layer),
                            0, 0, static_cast<uint32_t>(width), static_cast<uint32_t>(height), 0,
                            rgba_pixels, GL_RGBA, GL_UNSIGNED_BYTE, false, err))
    {
        warnOnce("tex2d_array_upload", "2D array layer upload failed: " + err);
        return false;
    }
    if (generate_mips && img->mipLevels() > 1)
    {
        sTextureStore.generateMipmaps(dst_tex);
    }
    return true;
}

namespace
{
    float unpackHalf16ToFloat32(uint16_t h)
    {
        const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
        uint32_t exp = (h >> 10) & 0x1Fu;
        uint32_t mant = h & 0x03FFu;
        uint32_t out = 0;
        if (exp == 0)
        {
            if (mant == 0)
            {
                out = sign;
            }
            else
            {
                exp = 1;
                while ((mant & 0x0400u) == 0)
                {
                    mant <<= 1;
                    --exp;
                }
                mant &= 0x03FFu;
                out = sign | ((exp + (127u - 15u)) << 23) | (mant << 13);
            }
        }
        else if (exp == 31)
        {
            out = sign | 0x7F800000u | (mant << 13);
        }
        else
        {
            out = sign | ((exp + (127u - 15u)) << 23) | (mant << 13);
        }
        float fv = 0.f;
        memcpy(&fv, &out, 4);
        return fv;
    }

    void flushFrameForSyncReadback()
    {
        if (!sActive || !sFrameOpen) return;
        const bool keep_c = sPendingClearColor;
        const bool keep_d = sPendingClearDepth;
        float keep_rgba[4];
        memcpy(keep_rgba, sPendingClearRGBA, sizeof(keep_rgba));
        const float keep_dval = sPendingClearDepthVal;
        endUnshownFrame();
        sDevice.waitIdle();
        if (ensureFrameOpen())
        {
            sPendingClearColor = keep_c;
            sPendingClearDepth = keep_d;
            memcpy(sPendingClearRGBA, keep_rgba, sizeof(keep_rgba));
            sPendingClearDepthVal = keep_dval;
        }
    }
}

bool bindLayerTarget(const LayerAttachment* atts, U32 count)
{
    if (!sActive || !sDevice.hasDevice() || !atts || count == 0 || count > WolfVkRenderTarget::MAX_COLOR_ATTACHMENTS)
    {
        return false;
    }
    if (!ensureFrameOpen())
    {
        return false;
    }
    endActivePass();
    sPendingClearColor = false;
    sPendingClearDepth = false;
    WolfVkState::clearViewportOverride();

    LayerTargetKey key{};
    key.mCount = count;
    WolfVkImage* imgs[WolfVkRenderTarget::MAX_COLOR_ATTACHMENTS] = {};
    for (U32 i = 0; i < count; ++i)
    {
        WolfVkImage* img = sTextureStore.find(atts[i].mTex);
        if (!img || !img->isValid() || atts[i].mLayer < 0)
        {
            return false;
        }
        imgs[i] = img;
        key.mTex[i] = atts[i].mTex;
        key.mLayer[i] = atts[i].mLayer;
    }

    WolfVkRenderTarget* rt = nullptr;
    auto it = sLayerTargets.find(key);
    if (it != sLayerTargets.end() && it->second && it->second->isComplete())
    {
        rt = it->second.get();
    }
    else
    {
        auto created = std::make_unique<WolfVkRenderTarget>();
        created->init(sDevice);
        created->setColorLayout(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        std::string err;
        for (U32 i = 0; i < count; ++i)
        {
            sAttachmentImages.insert(imgs[i]->image());
            if (!created->addExternalLayerAttachment(imgs[i], static_cast<uint32_t>(atts[i].mLayer), err))
            {
                warnOnce("layer_rt_att", "bindLayerTarget attachment failed: " + err);
                return false;
            }
        }
        if (!created->ensurePass(err))
        {
            warnOnce("layer_rt_pass", "bindLayerTarget pass failed: " + err);
            return false;
        }
        rt = created.get();
        sLayerTargets[key] = std::move(created);
    }

    for (U32 i = 0; i < count; ++i)
    {
        bool seen = false;
        for (U32 j = 0; j < i; ++j)
        {
            if (imgs[j] == imgs[i]) { seen = true; break; }
        }
        if (!seen && imgs[i]->layout() != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
        {
            sFrameCmdImages.insert(imgs[i]->image());
            imgs[i]->transition(sFrameCmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        }
    }

    sBoundLayerTarget = rt;
    return true;
}

void unbindLayerTarget()
{
    endActivePass();
    WolfVkState::clearViewportOverride();
    sBoundLayerTarget = nullptr;
}

void clearTextureZero(U32 tex_name)
{
    if (!sDevice.hasDevice() || tex_name == 0) return;
    WolfVkImage* img = sTextureStore.find(tex_name);
    if (!img || !img->isValid() || img->format().mDepth || img->format().mBlockBytes) return;
    if (sActive) ensureFrameOpen();
    auto doClear = [&](VkCommandBuffer cmd)
    {
        img->transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkClearColorValue zero{};
        const uint32_t arr_layers = (img->kind() == WolfVkImage::Kind::TEXTURE_3D) ? 1u : img->layers();
        VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, img->mipLevels(), 0, arr_layers };
        gVk.vkCmdClearColorImage(cmd, img->image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
        img->transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    };
    VkCommandBuffer xfer_cmd = pickTransferCmdForImage(img);
    if (xfer_cmd != VK_NULL_HANDLE)
    {
        doClear(xfer_cmd);
        return;
    }
    VkCommandBuffer cmd = sDevice.beginOneTime();
    if (cmd != VK_NULL_HANDLE)
    {
        doClear(cmd);
        std::string err;
        sDevice.endOneTimeAndWait(cmd, err);
    }
}

bool readBackTextureFloatSync(U32 tex_name, S32 mip, U32 out_channels, F32* out_floats, size_t max_floats)
{
    if (!sDevice.hasDevice() || tex_name == 0 || mip < 0 || !out_floats || (out_channels != 1 && out_channels != 4))
    {
        return false;
    }
    WolfVkImage* img = sTextureStore.find(tex_name);
    if (!img || !img->isValid() || static_cast<uint32_t>(mip) >= img->mipLevels())
    {
        return false;
    }
    flushFrameForSyncReadback();

    const uint32_t u_mip = static_cast<uint32_t>(mip);
    const uint32_t w = std::max<uint32_t>(1u, img->width() >> std::min<uint32_t>(u_mip, 31u));
    const uint32_t h = std::max<uint32_t>(1u, img->height() >> std::min<uint32_t>(u_mip, 31u));
    const uint32_t d = (img->kind() == WolfVkImage::Kind::TEXTURE_3D)
        ? std::max<uint32_t>(1u, img->layers() >> std::min<uint32_t>(u_mip, 31u))
        : 1u;
    const uint32_t arr_layers = (img->kind() == WolfVkImage::Kind::TEXTURE_3D) ? 1u : img->layers();
    const uint32_t slices = d * arr_layers;
    const uint32_t texel_bytes = img->format().mTexelBytes;
    const size_t total_texels = static_cast<size_t>(w) * h * slices;
    if (texel_bytes == 0 || total_texels * out_channels > max_floats)
    {
        return false;
    }

    const VkDeviceSize byte_size = static_cast<VkDeviceSize>(total_texels) * texel_bytes;
    WolfVkBuffer host;
    std::string err;
    if (!host.create(sDevice, byte_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, err))
    {
        return false;
    }
    VkCommandBuffer cmd = sDevice.beginOneTime();
    if (cmd == VK_NULL_HANDLE)
    {
        host.destroy();
        return false;
    }
    img->transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, u_mip, 0, arr_layers };
    region.imageExtent = { w, h, d };
    gVk.vkCmdCopyImageToBuffer(cmd, img->image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, host.buffer(), 1, &region);
    VkBufferMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.buffer = host.buffer();
    b.size = VK_WHOLE_SIZE;
    gVk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &b, 0, nullptr);
    img->transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    if (!sDevice.endOneTimeAndWait(cmd, err))
    {
        host.destroy();
        return false;
    }
    host.invalidateForRead();
    const uint8_t* raw = static_cast<const uint8_t*>(host.hostData());
    const VkFormat vf = img->format().mFormat;
    for (size_t i = 0; i < total_texels; ++i)
    {
        float rgba[4] = { 0.f, 0.f, 0.f, 1.f };
        const uint8_t* p = raw + i * texel_bytes;
        if (vf == VK_FORMAT_R32G32B32A32_SFLOAT)
        {
            memcpy(rgba, p, 16);
        }
        else if (vf == VK_FORMAT_R32_SFLOAT)
        {
            memcpy(&rgba[0], p, 4);
        }
        else if (vf == VK_FORMAT_R16G16B16A16_SFLOAT)
        {
            uint16_t hv[4];
            memcpy(hv, p, 8);
            for (int c = 0; c < 4; ++c) rgba[c] = unpackHalf16ToFloat32(hv[c]);
        }
        else if (vf == VK_FORMAT_R16_SFLOAT)
        {
            uint16_t hv = 0;
            memcpy(&hv, p, 2);
            rgba[0] = unpackHalf16ToFloat32(hv);
        }
        else if (vf == VK_FORMAT_R8G8B8A8_UNORM || vf == VK_FORMAT_R8G8B8A8_SRGB)
        {
            for (int c = 0; c < 4; ++c) rgba[c] = static_cast<float>(p[c]) * (1.f / 255.f);
        }
        else if (vf == VK_FORMAT_R8_UNORM)
        {
            rgba[0] = static_cast<float>(p[0]) * (1.f / 255.f);
        }
        if (out_channels == 1)
        {
            out_floats[i] = rgba[0];
        }
        else
        {
            memcpy(out_floats + i * 4u, rgba, 16);
        }
    }
    host.destroy();
    return true;
}

bool queueAsyncTextureRead(S32 channel_id, U32 tex_name, S32 mip, F32 tag)
{
    if (!sActive || !sFrameOpen || sFrameCmd == VK_NULL_HANDLE || channel_id < 0 || channel_id >= MAX_ASYNC_READ_CHANNELS || tex_name == 0 || mip < 0)
    {
        return false;
    }
    WolfVkImage* img = sTextureStore.find(tex_name);
    if (!img || !img->isValid() || static_cast<uint32_t>(mip) >= img->mipLevels() || img->format().mTexelBytes == 0)
    {
        return false;
    }
    const uint32_t u_mip = static_cast<uint32_t>(mip);
    const uint32_t w = std::max<uint32_t>(1u, img->width() >> std::min<uint32_t>(u_mip, 31u));
    const uint32_t h = std::max<uint32_t>(1u, img->height() >> std::min<uint32_t>(u_mip, 31u));
    const size_t copy_bytes = static_cast<size_t>(w) * h * img->format().mTexelBytes;
    if (copy_bytes == 0) return false;

    AsyncReadChannel& ch = sAsyncReadChannels[channel_id];
    AsyncReadChannel::Slot& sl = ch.mSlots[ch.mWrite % AsyncReadChannel::RING];
    if (sl.mPending && sSubmittedCmdCounter < sl.mReadySubmit)
    {
        return false; // slot still in flight on GPU
    }
    if (!sl.mBuf.isValid() || sl.mCapBytes < copy_bytes)
    {
        sl.mBuf.destroy();
        sl.mCapBytes = 0;
        std::string err;
        if (!sl.mBuf.create(sDevice, static_cast<VkDeviceSize>(copy_bytes), VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, err))
        {
            return false;
        }
        sl.mCapBytes = copy_bytes;
    }

    endActivePass();
    sFrameCmdImages.insert(img->image());
    img->transition(sFrameCmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, u_mip, 0, 1 };
    region.imageExtent = { w, h, 1 };
    gVk.vkCmdCopyImageToBuffer(sFrameCmd, img->image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sl.mBuf.buffer(), 1, &region);
    VkBufferMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.buffer = sl.mBuf.buffer();
    b.size = static_cast<VkDeviceSize>(copy_bytes);
    gVk.vkCmdPipelineBarrier(sFrameCmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &b, 0, nullptr);
    img->transition(sFrameCmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);

    sl.mReadBytes = copy_bytes;
    sl.mTag = tag;
    sl.mReadySubmit = sSubmittedCmdCounter + sDevice.framesInFlight() + 1u;
    sl.mPending = true;
    ch.mWrite = (ch.mWrite + 1u) % AsyncReadChannel::RING;
    return true;
}

bool pollAsyncTextureRead(S32 channel_id, void* out_bytes, size_t byte_count, F32* out_tag)
{
    if (channel_id < 0 || channel_id >= MAX_ASYNC_READ_CHANNELS || !out_bytes || byte_count == 0)
    {
        return false;
    }
    AsyncReadChannel& ch = sAsyncReadChannels[channel_id];
    int best_idx = -1;
    uint64_t best_ready = 0;
    for (uint32_t i = 0; i < AsyncReadChannel::RING; ++i)
    {
        AsyncReadChannel::Slot& sl = ch.mSlots[i];
        if (sl.mPending && sSubmittedCmdCounter >= sl.mReadySubmit)
        {
            if (sl.mReadBytes == byte_count && sl.mBuf.isValid() && (best_idx < 0 || sl.mReadySubmit >= best_ready))
            {
                best_idx = static_cast<int>(i);
                best_ready = sl.mReadySubmit;
            }
            else
            {
                sl.mPending = false;
            }
        }
    }
    if (best_idx < 0) return false;
    for (uint32_t i = 0; i < AsyncReadChannel::RING; ++i)
    {
        AsyncReadChannel::Slot& sl = ch.mSlots[i];
        if (sl.mPending && sSubmittedCmdCounter >= sl.mReadySubmit && sl.mReadySubmit <= best_ready)
        {
            sl.mPending = false;
        }
    }
    AsyncReadChannel::Slot& win = ch.mSlots[best_idx];
    win.mBuf.invalidateForRead();
    memcpy(out_bytes, win.mBuf.hostData(), byte_count);
    if (out_tag) *out_tag = win.mTag;
    return true;
}

void resetAsyncTextureRead(S32 channel_id)
{
    if (channel_id < 0 || channel_id >= MAX_ASYNC_READ_CHANNELS) return;
    AsyncReadChannel& ch = sAsyncReadChannels[channel_id];
    for (uint32_t i = 0; i < AsyncReadChannel::RING; ++i)
    {
        ch.mSlots[i].mPending = false;
    }
}

void gpuSectionBegin(U32 section)
{
    if (section >= GPU_SECTION_MAX || sSecPool == VK_NULL_HANDLE || !sFrameOpen || sFrameCmd == VK_NULL_HANDLE) return;
    const uint32_t slot = sDevice.frameIndex();
    if (slot >= MAX_TS_FRAME_SLOTS || sSecOpen[section] || sSecPairs[slot][section] >= SEC_PAIRS) return;
    endActivePass();
    const uint32_t q = slot * SEC_QUERIES_PER_SLOT + (section * SEC_PAIRS + sSecPairs[slot][section]) * 2;
    gVk.vkCmdWriteTimestamp(sFrameCmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, sSecPool, q);
    sSecOpen[section] = true;
}

void gpuSectionEnd(U32 section)
{
    if (section >= GPU_SECTION_MAX || !sSecOpen[section]) return;
    sSecOpen[section] = false;
    if (sSecPool == VK_NULL_HANDLE || !sFrameOpen || sFrameCmd == VK_NULL_HANDLE) return;
    const uint32_t slot = sDevice.frameIndex();
    if (slot >= MAX_TS_FRAME_SLOTS) return;
    endActivePass();
    const uint32_t q = slot * SEC_QUERIES_PER_SLOT + (section * SEC_PAIRS + sSecPairs[slot][section]) * 2 + 1;
    gVk.vkCmdWriteTimestamp(sFrameCmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, sSecPool, q);
    ++sSecPairs[slot][section];
}

bool takeGpuSectionStats(F64* ms_per_frame, F64* calls_per_frame, U32 count)
{
    if (sSecPool == VK_NULL_HANDLE || sSecWinFrames == 0) return false;
    const F64 inv = 1.0 / static_cast<F64>(sSecWinFrames);
    for (U32 s = 0; s < count && s < GPU_SECTION_MAX; ++s)
    {
        ms_per_frame[s] = sSecWinMs[s] * inv;
        if (calls_per_frame) calls_per_frame[s] = static_cast<F64>(sSecWinCalls[s]) * inv;
        sSecWinMs[s] = 0.0;
        sSecWinCalls[s] = 0;
    }
    sSecWinFrames = 0;
    return true;
}

bool occlusionSupported()
{
    return sActive && sOcclusionPool != VK_NULL_HANDLE && sDevice.hostQueryReset();
}

U32 genOcclusionQuery()
{
    if (!occlusionSupported()) return 0;
    if (!sFreeLogicalIds.empty())
    {
        const U32 id = sFreeLogicalIds.back();
        sFreeLogicalIds.pop_back();
        sLogicalQueries[id] = LogicalOcclusionQuery{};
        return id;
    }
    const U32 id = static_cast<U32>(sLogicalQueries.size());
    sLogicalQueries.push_back(LogicalOcclusionQuery{});
    return id;
}

void deleteOcclusionQuery(U32 id)
{
    if (id == 0 || id >= sLogicalQueries.size()) return;
    if (sActiveOcclusionId == id)
    {
        finishActiveOcclusionQuery();
    }
    LogicalOcclusionQuery& lq = sLogicalQueries[id];
    if (lq.mPhysSlot >= 0)
    {
        sRetiredPhysSlots.push_back({ lq.mPhysSlot, sDevice.frameCounter() });
        lq.mPhysSlot = -1;
    }
    lq.mState = OcclusionQueryState::Idle;
    sFreeLogicalIds.push_back(id);
}

bool beginOcclusionQuery(U32 id)
{
    if (!occlusionSupported() || id == 0 || id >= sLogicalQueries.size()) return false;
    finishActiveOcclusionQuery();

    // Flush any large preceding geometry pass across the 4 parallel secondary command buffer threads first,
    // then run the lightweight occlusion query batch inline on the same target with LOAD_OP_LOAD!
    if (sActiveTarget && sActiveTarget == currentTarget() && !sPassHasOcclusion && sPassDrawCount >= 96)
    {
        endActivePass();
    }

    if (!ensureRenderPassOpen() || !sActiveTarget || sFrameCmd == VK_NULL_HANDLE)
    {
        return false;
    }

    reclaimRetiredOcclusionSlots();
    LogicalOcclusionQuery& lq = sLogicalQueries[id];
    if (lq.mPhysSlot >= 0)
    {
        sRetiredPhysSlots.push_back({ lq.mPhysSlot, sDevice.frameCounter() });
        lq.mPhysSlot = -1;
    }
    if (sFreePhysSlots.empty())
    {
        lq.mState = OcclusionQueryState::Idle;
        return false;
    }

    const int32_t phys = sFreePhysSlots.back();
    sFreePhysSlots.pop_back();

    gVk.vkResetQueryPool(sDevice.device(), sOcclusionPool, static_cast<uint32_t>(phys), 1);
    sPassForceInline = true;
    sPassHasOcclusion = true;
    PassOp op{};
    op.mKind = PassOpKind::OcclusionBegin;
    op.mQuerySlot = static_cast<uint32_t>(phys);
    sPassOps.push_back(op);

    lq.mPhysSlot = phys;
    lq.mState = OcclusionQueryState::Recording;
    sActiveOcclusionId = id;
    sRecordedQueriesThisFrame.push_back(id);
    return true;
}

void endOcclusionQuery()
{
    finishActiveOcclusionQuery();
}

bool getOcclusionQueryResult(U32 id, bool& out_available, U32& out_passed)
{
    out_available = true;
    out_passed = 1;
    if (!occlusionSupported() || id == 0 || id >= sLogicalQueries.size())
    {
        return false;
    }
    const LogicalOcclusionQuery& lq = sLogicalQueries[id];
    if (lq.mState == OcclusionQueryState::Recording || lq.mState == OcclusionQueryState::RecordedInCurrentFrame)
    {
        out_available = false;
        out_passed = 1;
        return true;
    }
    if (lq.mState != OcclusionQueryState::Submitted || lq.mPhysSlot < 0)
    {
        out_available = true;
        out_passed = 1;
        return true;
    }
    uint64_t samples = 0;
    const VkResult res = gVk.vkGetQueryPoolResults(
        sDevice.device(), sOcclusionPool, static_cast<uint32_t>(lq.mPhysSlot), 1,
        sizeof(uint64_t), &samples, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
    if (res == VK_SUCCESS)
    {
        out_available = true;
        out_passed = (samples > 0) ? 1u : 0u;
        ++sPerf.mOccReadThisFrame;
        if (samples == 0) ++sPerf.mOccCulledThisFrame;
        return true;
    }
    if (res == VK_NOT_READY)
    {
        out_available = false;
        out_passed = 1;
        return true;
    }
    out_available = true;
    out_passed = 1;
    return true;
}

bool shadowBatchSupported()
{
    return sActive && sDevice.hasDevice() && sDevice.multiDrawIndirect();
}

bool shadowBatchProgramReady(LLGLSLShader* shader)
{
    if (!shader || !shader->mWolfVk || !shader->mWolfVk->program().isValid()) return false;
    const WolfVkProgram& prog = shader->mWolfVk->program();
    return prog.inputLocation("position") >= 0 && prog.inputLocation("wolf_mvp") >= 0;
}

void shadowBatchAdjustProjection(F32* proj16)
{
    if (!proj16 || sDevice.glDepthRange()) return;
    // Vulkan [0, 1] clip depth fix when VK_EXT_depth_clip_control is absent: z' = 0.5 * z + 0.5 * w
    for (int col = 0; col < 4; ++col)
    {
        const F32 z = proj16[col * 4 + 2];
        const F32 w = proj16[col * 4 + 3];
        proj16[col * 4 + 2] = 0.5f * z + 0.5f * w;
    }
}

S32 createShadowBatchPage(U32 vertex_bytes, U32 index_bytes)
{
    if (!shadowBatchSupported() || vertex_bytes == 0 || index_bytes == 0) return -1;
    auto page = std::make_unique<ShadowBatchPage>();
    std::string err;
    if (!page->mVertexBuf.create(sDevice, vertex_bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, /*host_only=*/false, err) ||
        !page->mIndexBuf.create(sDevice, index_bytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, /*host_only=*/false, err))
    {
        page->mVertexBuf.destroy();
        page->mIndexBuf.destroy();
        return -1;
    }
    page->mAlive = true;
    for (size_t i = 0; i < sShadowBatchPages.size(); ++i)
    {
        if (!sShadowBatchPages[i])
        {
            sShadowBatchPages[i] = std::move(page);
            return static_cast<S32>(i);
        }
    }
    sShadowBatchPages.push_back(std::move(page));
    return static_cast<S32>(sShadowBatchPages.size() - 1);
}

void writeShadowBatchPage(S32 page_id, U32 vertex_byte_offset, const void* vertices, U32 vertex_bytes,
                          U32 index_byte_offset, const void* indices, U32 index_bytes)
{
    if (page_id < 0 || static_cast<size_t>(page_id) >= sShadowBatchPages.size()) return;
    ShadowBatchPage* page = sShadowBatchPages[page_id].get();
    if (!page || !page->mAlive) return;
    if (vertices && vertex_bytes > 0)
    {
        page->mVertexBuf.write(vertex_byte_offset, vertices, vertex_bytes);
    }
    if (indices && index_bytes > 0)
    {
        page->mIndexBuf.write(index_byte_offset, indices, index_bytes);
    }
}

void destroyShadowBatchPage(S32 page_id)
{
    if (page_id < 0 || static_cast<size_t>(page_id) >= sShadowBatchPages.size()) return;
    if (sShadowBatchPages[page_id])
    {
        sShadowBatchPages[page_id]->mVertexBuf.destroy();
        sShadowBatchPages[page_id]->mIndexBuf.destroy();
        sShadowBatchPages[page_id].reset();
    }
}

void flushShadowBatchPages(const S32* page_ids, U32 page_count)
{
    if (!page_ids || page_count == 0 || !shadowBatchSupported()) return;
    bool any_pending = false;
    for (U32 i = 0; i < page_count; ++i)
    {
        const S32 id = page_ids[i];
        if (id >= 0 && static_cast<size_t>(id) < sShadowBatchPages.size() && sShadowBatchPages[id])
        {
            if (sShadowBatchPages[id]->mVertexBuf.hasPendingUploads() || sShadowBatchPages[id]->mIndexBuf.hasPendingUploads())
            {
                any_pending = true;
                break;
            }
        }
    }
    if (!any_pending) return;
    if (!ensureFrameOpen() || sFrameCmd == VK_NULL_HANDLE) return;
    VkCommandBuffer pre_cmd = sDevice.prePassTransferCmd();
    VkCommandBuffer xfer_cmd = (pre_cmd != VK_NULL_HANDLE) ? pre_cmd : sFrameCmd;
    if (xfer_cmd == sFrameCmd)
    {
        endActivePass();
    }
    for (U32 i = 0; i < page_count; ++i)
    {
        const S32 id = page_ids[i];
        if (id >= 0 && static_cast<size_t>(id) < sShadowBatchPages.size() && sShadowBatchPages[id])
        {
            ShadowBatchPage& page = *sShadowBatchPages[id];
            if (page.mVertexBuf.hasPendingUploads())
            {
                page.mVertexBuf.recordUploads(xfer_cmd, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT, &sStagingRing);
            }
            if (page.mIndexBuf.hasPendingUploads())
            {
                page.mIndexBuf.recordUploads(xfer_cmd, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_ACCESS_INDEX_READ_BIT, &sStagingRing);
            }
        }
    }
}

bool uploadShadowBatchMatrices(const void* matrices, U32 matrix_count, U64& out_matrix_token)
{
    out_matrix_token = UINT64_MAX;
    if (!matrices || matrix_count == 0 || !shadowBatchSupported()) return false;
    if (!ensureFrameOpen()) return false;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    if (!sIndirectRing.push(matrices, static_cast<VkDeviceSize>(matrix_count) * 64ull, buf, offset))
    {
        return false;
    }
    out_matrix_token = static_cast<U64>(offset);
    return true;
}

U32 drawShadowBatchPage(LLGLSLShader* batch_shader, S32 page_id, U64 matrix_token,
                        const void* commands, U32 cull_on_count, U32 cull_off_count)
{
    const U32 total_cmds = cull_on_count + cull_off_count;
    if (!shadowBatchSupported() || !batch_shader || !batch_shader->mWolfVk ||
        page_id < 0 || static_cast<size_t>(page_id) >= sShadowBatchPages.size() ||
        !sShadowBatchPages[page_id] || !commands || total_cmds == 0 || matrix_token == UINT64_MAX)
    {
        return 0;
    }
    const auto t_draw0 = std::chrono::steady_clock::now();
    ShadowBatchPage& page = *sShadowBatchPages[page_id];
    if (page.mVertexBuf.hasPendingUploads() || page.mIndexBuf.hasPendingUploads())
    {
        flushShadowBatchPages(&page_id, 1);
    }
    if (!ensureRenderPassOpen() || !sActiveTarget || sFrameCmd == VK_NULL_HANDLE)
    {
        return 0;
    }
    VkBuffer indirect_buf = VK_NULL_HANDLE;
    VkDeviceSize cmd_offset = 0;
    if (!sIndirectRing.push(commands, static_cast<VkDeviceSize>(total_cmds) * sizeof(VkDrawIndexedIndirectCommand), indirect_buf, cmd_offset))
    {
        return 0;
    }
    VkBuffer matrix_buf = sIndirectRing.currentBuffer();
    if (matrix_buf == VK_NULL_HANDLE || indirect_buf == VK_NULL_HANDLE)
    {
        return 0;
    }

    WolfVkShaderMirror& mirror = *batch_shader->mWolfVk;
    const WolfVkProgram& program = mirror.program();
    const uint8_t* push_data = nullptr;
    uint32_t push_size = 0;
    VkDescriptorSet set0 = VK_NULL_HANDLE;
    uint32_t set0_offsets[2] = { 0, 0 };
    uint32_t set0_off_cnt = 0;
    if (!mirror.prepareUniforms(sUniforms, push_data, push_size, set0, set0_offsets, set0_off_cnt))
    {
        return 0;
    }

    VkViewport vp{};
    VkRect2D sc{};
    computeTargetViewportAndScissor(vp, sc);

    const bool* cm = gGL.wolfVkColorMask();
    ShadowBatchPipeKey key{};
    key.mProgram = &program;
    key.mPass = sActiveTarget->renderPass();
    key.mColorAttachments = sActiveTarget->getNumTextures();
    key.mColorMaskBits = static_cast<uint8_t>((cm[0] ? 1u : 0u) | (cm[1] ? 2u : 0u) | (cm[2] ? 4u : 0u) | (cm[3] ? 8u : 0u));
    key.mDepthTest = LLGLDepthTest::wolfVkDepthEnabled();
    key.mDepthWrite = LLGLDepthTest::wolfVkWriteEnabled();
    key.mDepthFunc = LLGLDepthTest::wolfVkDepthFunc();
    key.mDepthClamp = LLGLState::wolfVkIsEnabled(GL_DEPTH_CLAMP) && sDevice.depthClamp();
    if (LLGLState::wolfVkIsEnabled(GL_POLYGON_OFFSET_FILL))
    {
        key.mBiasSlope = WolfVkState::current().mPolyOffsetFactor;
        key.mBiasConst = WolfVkState::current().mPolyOffsetUnits;
    }

    const uint32_t max_indirect = std::max<uint32_t>(1u, sDevice.limits().maxDrawIndirectCount);
    U32 calls = 0;
    auto submitRange = [&](bool cull_face, VkDeviceSize base_off, uint32_t cnt)
    {
        if (cnt == 0) return;
        key.mCullFace = cull_face;
        VkPipeline pipe = getOrBuildShadowBatchPipeline(key);
        if (pipe == VK_NULL_HANDLE) return;
        uint32_t done = 0;
        while (done < cnt)
        {
            const uint32_t batch = std::min<uint32_t>(cnt - done, max_indirect);
            sPassOps.emplace_back();
            PassOp& op = sPassOps.back();
            op.mKind = PassOpKind::DrawIndexedIndirect;
            op.mPipeline = pipe;
            op.mLayout = program.pipelineLayout();
            if (push_data && push_size > 0 && push_size <= sizeof(op.mPushBytes))
            {
                op.mPushSize = static_cast<uint16_t>(push_size);
                memcpy(op.mPushBytes, push_data, push_size);
                ++sPerf.mPushConstDrawsThisFrame;
            }
            op.mSet0 = set0;
            op.mSet0OffCount = static_cast<uint8_t>(set0_off_cnt);
            op.mSet0Offsets[0] = set0_offsets[0];
            op.mSet0Offsets[1] = set0_offsets[1];
            op.mViewport = vp;
            op.mScissor = sc;
            op.mNumVBufs = 2;
            op.mVBufs[0] = page.mVertexBuf.buffer();
            op.mVOffs[0] = 0;
            op.mVBufs[1] = matrix_buf;
            op.mVOffs[1] = static_cast<VkDeviceSize>(matrix_token);
            op.mIBuf = page.mIndexBuf.buffer();
            op.mIType = VK_INDEX_TYPE_UINT16;
            op.mIndirectBuf = indirect_buf;
            op.mIndirectOff = base_off + static_cast<VkDeviceSize>(done) * sizeof(VkDrawIndexedIndirectCommand);
            op.mCount = batch;
            done += batch;
            ++calls;
            ++sPassDrawCount;
            ++sDrawsThisFrame;
            ++sPerf.mIndirectBatchesThisFrame;
        }
    };

    submitRange(LLGLState::wolfVkIsEnabled(GL_CULL_FACE), cmd_offset, cull_on_count);
    submitRange(false, cmd_offset + static_cast<VkDeviceSize>(cull_on_count) * sizeof(VkDrawIndexedIndirectCommand), cull_off_count);
    const auto t_draw1 = std::chrono::steady_clock::now();
    sPerf.mVkDrawMsThisFrame += std::chrono::duration<double, std::milli>(t_draw1 - t_draw0).count();
    return calls;
}

void shutdown()
{
    const bool was_active = sActive;
    if (sFrameOpen)
    {
        endUnshownFrame();
    }
    sActive = false;
    sDevice.setStagingRing(nullptr);
    clearLastPipeCache();

    // Unhook llrender callbacks first
    LLGLState::sWolfVkClear = nullptr;
    LLRenderTarget::sWolfVkNativeOwner = nullptr;
    LLImageGL::sWolfVkTextureOwner = nullptr;
    LLImageGL::sWolfVkNativeStore = nullptr;
    LLGLSLShader::sWolfVkOnUniformBufferBind = nullptr;
    LLGLSLShader::sWolfVkOnUniformBufferData = nullptr;
    LLGLSLShader::sWolfVkOnLinked = nullptr;
    LLVertexBuffer::sWolfVkInterceptDraw = nullptr;
    LLVertexBuffer::setWolfVkStore(nullptr);
    sUboBytes.clear();
    memset(sBoundUbo, 0, sizeof(sBoundUbo));
    sPassOps.clear();
    sAttachmentImages.clear();
    sFrameCmdImages.clear();

    if (sDevice.hasDevice())
    {
        sDevice.waitIdleAndFlushDeferred();

        sParallelRecorder.shutdown();
        shutdownTimestampPool();

        for (auto& kv : sShadowBatchPipelines)
        {
            if (kv.second != VK_NULL_HANDLE)
            {
                gVk.vkDestroyPipeline(sDevice.device(), kv.second, nullptr);
            }
        }
        sShadowBatchPipelines.clear();

        for (auto& p : sShadowBatchPages)
        {
            if (p)
            {
                p->mVertexBuf.destroy();
                p->mIndexBuf.destroy();
            }
        }
        sShadowBatchPages.clear();

        shutdownOcclusionPool();

        // Destroy all live shader mirrors
        for (LLGLSLShader* sh : LLGLSLShader::sInstances)
        {
            if (sh && sh->mWolfVk)
            {
                delete sh->mWolfVk;
                sh->mWolfVk = nullptr;
            }
        }

        sBoundLayerTarget = nullptr;
        sLayerTargets.clear();
        shutdownAsyncReadChannels();
        sRenderTargetOwner.shutdown();
        sWindowTarget.release();
        sFallbackCubeArray.destroy();
        sFallback3D.destroy();
        sFallback2DArray.destroy();
        sFallbackCube.destroy();
        sFallbackWhite.destroy();
        sTextureStore.shutdown();
        sBufferStore.shutdown();
        sSamplers.destroy();
        sPipelines.destroy();
        sIndirectRing.destroy();
        sStagingRing.destroy();
        sUniforms.destroy();
        sSwapchain.destroy();

        // Flush deferred destructions before vkDestroyDevice so 0 child objects leak!
        sDevice.waitIdleAndFlushDeferred();
    }
    else
    {
        sParallelRecorder.shutdown();
    }

    if (sSurface != VK_NULL_HANDLE && sDevice.hasInstance())
    {
        WolfVkSurface::destroy(sDevice.instance(), sSurface);
        sSurface = VK_NULL_HANDLE;
    }

    sDevice.destroy();
    sWindow = nullptr;
    sFrameOpen = false;
    sFrameCmd = VK_NULL_HANDLE;
    sActiveTarget = nullptr;
    // When tearing down an active Native Vulkan run (LLViewerWindow::shutdownGL), no OpenGL context exists
    // in this process: keep mIsNativeVulkan true so subsequent subsystem cleanup (LLFontGL, gPipeline,
    // LLImageGL::cleanupClass, stopGL shader unload, LLVertexBuffer::cleanupClass) skips raw OpenGL calls.
    if (!was_active)
    {
        gGLManager.mIsNativeVulkan = false;
    }
    gGLManager.mInited = false;
}

std::string rendererLine()
{
    if (sActive && sDevice.hasDevice())
    {
        return "Native Vulkan on " + sDevice.chosen().name();
    }
    return "OpenGL on " + gGLManager.mGLRenderer;
}

std::string vulkanLine()
{
    return sVulkanLine;
}

std::string statusLine()
{
    if (sActive)
    {
        return "Native Vulkan active (" + sVulkanLine + ")";
    }
    return "Native Vulkan inactive";
}

LLSD reportFields()
{
    LLSD d = LLSD::emptyMap();
    d["renderer"] = rendererLine();
    d["vulkan_available"] = sActive;
    d["vulkan_device"] = sActive && sDevice.hasDevice() ? sDevice.chosen().name() : "";
    d["vulkan_driver"] = sActive && sDevice.hasDevice() ? sDevice.chosen().driverString() : "";
    d["vulkan_api"] = sActive && sDevice.hasDevice() ? sDevice.chosen().apiVersionString() : "";
    d["vulkan_loader"] = "";
    d["vulkan_native"] = sActive;
    return d;
}

U32 drawsLastFrame()
{
    return sDrawsLastFrame;
}

U32 validationMessageCount()
{
    return sDevice.validationMessageCount();
}

} // namespace WolfVkNative
