/**
 * @file wolfpluginaudio.cpp
 * @brief [W111 VIDEO 2026-11-24] Media plugin sound to the viewer: the shared-memory ring (see wolfpluginaudio.h).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer Source Code
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "linden_common.h"
#include "wolfpluginaudio.h"
#include "llpluginsharedmemory.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <new>

namespace WolfPluginAudio
{
    const char* const KEY_THROUGH_VIEWER = "wolf_audio_through_viewer";
    const char* const KEY_SHM_NAME = "wolf_audio_shm_name";
    const char* const KEY_SHM_SIZE = "wolf_audio_shm_size";

    int64_t nowUs()
    {
        return (int64_t)std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    static_assert(std::atomic<uint64_t>::is_always_lock_free, "the ring needs address-free (lock-free) 64-bit atomics");
    static_assert(std::atomic<uint32_t>::is_always_lock_free, "the ring needs address-free (lock-free) 32-bit atomics");

    constexpr uint32_t MAGIC = 0x44554157;    // "WAUD" little-endian
    constexpr uint32_t VERSION = 1;
    constexpr size_t   HEADER_BYTES = 256;

    struct RingHeader
    {
        uint32_t                          mMagic;
        uint32_t                          mVersion;
        uint64_t                          mCapacity;      // bytes in the data area
        alignas(64) std::atomic<uint64_t> mWritePos;      // the writer's, bytes written in all
        alignas(64) std::atomic<uint64_t> mReadPos;       // the reader's, bytes read in all
        alignas(64) std::atomic<uint32_t> mDropped;       // frames the writer could not fit
    };
    static_assert(sizeof(RingHeader) <= HEADER_BYTES, "ring header too big");

    // A record in the data area: this header, then mFrames x 2 floats; its size is rounded up to 8 bytes.
    struct RecordHeader
    {
        uint32_t mKind;
        uint32_t mFrames;
        uint32_t mRate;
        uint32_t mReserved;
        int64_t  mDueUs;
    };
    static_assert(sizeof(RecordHeader) == 24, "record header layout");

    static uint64_t recordBytes(uint32_t frames)
    {
        const uint64_t bytes = sizeof(RecordHeader) + (uint64_t)frames * CHANNELS * sizeof(float);
        return (bytes + 7) & ~(uint64_t)7;
    }

    static void copyIn(uint8_t* data, uint64_t capacity, uint64_t pos, const void* src, size_t len)
    {
        const uint64_t off = pos % capacity;
        const size_t first = (size_t)std::min<uint64_t>(len, capacity - off);
        memcpy(data + off, src, first);
        if (first < len)
        {
            memcpy(data, static_cast<const uint8_t*>(src) + first, len - first);
        }
    }

    static void copyOut(const uint8_t* data, uint64_t capacity, uint64_t pos, void* dst, size_t len)
    {
        const uint64_t off = pos % capacity;
        const size_t first = (size_t)std::min<uint64_t>(len, capacity - off);
        memcpy(dst, data + off, first);
        if (first < len)
        {
            memcpy(static_cast<uint8_t*>(dst) + first, data, len - first);
        }
    }
}

using namespace WolfPluginAudio;

// ─── plugin side ────────────────────────────────────────────────────────────────────────────────────────────────────

WolfPluginAudioWriter::WolfPluginAudioWriter() = default;

WolfPluginAudioWriter::~WolfPluginAudioWriter()
{
    std::lock_guard<std::mutex> lock(mMutex);
    mHeader = nullptr;
    mData = nullptr;
    if (mShm)
    {
        mShm->detach();
    }
}

bool WolfPluginAudioWriter::attach(const std::string& name, size_t size)
{
    std::lock_guard<std::mutex> lock(mMutex);
    if (mHeader || name.empty() || size <= HEADER_BYTES)
    {
        return false;
    }
    std::unique_ptr<LLPluginSharedMemory> shm(new LLPluginSharedMemory);
    if (!shm->attach(name, size) || !shm->isMapped())
    {
        return false;
    }
    RingHeader* header = static_cast<RingHeader*>(shm->getMappedAddress());
    // The viewer wrote the header before it sent the name (WolfPluginAudioRing::create).
    if (header->mMagic != MAGIC || header->mVersion != VERSION || header->mCapacity != size - HEADER_BYTES)
    {
        shm->detach();
        return false;
    }
    mShm = std::move(shm);
    mHeader = header;
    mData = static_cast<uint8_t*>(mShm->getMappedAddress()) + HEADER_BYTES;
    mCapacity = header->mCapacity;
    return true;
}

bool WolfPluginAudioWriter::write(uint32_t kind, uint32_t rate, int64_t due_us, const float* stereo, uint32_t frames)
{
    std::lock_guard<std::mutex> lock(mMutex);
    if (!mHeader || frames > MAX_RECORD_FRAMES || (kind == KIND_DATA && (!stereo || frames == 0)))
    {
        return false;
    }
    if (kind != KIND_DATA)
    {
        frames = 0;
    }
    const uint64_t need = recordBytes(frames);
    const uint64_t w = mHeader->mWritePos.load(std::memory_order_relaxed);     // only this writer stores it
    const uint64_t r = mHeader->mReadPos.load(std::memory_order_acquire);      // what the reader has finished with
    if (w - r > mCapacity || mCapacity - (w - r) < need)
    {
        mHeader->mDropped.fetch_add(frames, std::memory_order_relaxed);
        return false;
    }
    RecordHeader rec{ kind, frames, rate, 0, due_us };
    copyIn(mData, mCapacity, w, &rec, sizeof(rec));
    if (frames)
    {
        copyIn(mData, mCapacity, w + sizeof(rec), stereo, (size_t)frames * CHANNELS * sizeof(float));
    }
    mHeader->mWritePos.store(w + need, std::memory_order_release);
    return true;
}

// ─── viewer side ────────────────────────────────────────────────────────────────────────────────────────────────────

WolfPluginAudioRing::WolfPluginAudioRing() = default;

WolfPluginAudioRing::~WolfPluginAudioRing()
{
    if (mShm)
    {
        // The plugin may still have it mapped: on POSIX shm_unlink only removes the name and the pages live until the
        // last munmap; on Windows the mapping lives until its last handle closes (llpluginsharedmemory.cpp).
        mShm->destroy();
    }
}

bool WolfPluginAudioRing::create(size_t size)
{
    if (mShm || size <= HEADER_BYTES)
    {
        return false;
    }
    std::unique_ptr<LLPluginSharedMemory> shm(new LLPluginSharedMemory);
    if (!shm->create(size) || !shm->isMapped())
    {
        return false;
    }
    RingHeader* header = new (shm->getMappedAddress()) RingHeader();
    header->mMagic = MAGIC;
    header->mVersion = VERSION;
    header->mCapacity = size - HEADER_BYTES;
    header->mWritePos.store(0, std::memory_order_relaxed);
    header->mReadPos.store(0, std::memory_order_relaxed);
    header->mDropped.store(0, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    mShm = std::move(shm);
    mHeader = header;
    mData = static_cast<uint8_t*>(mShm->getMappedAddress()) + HEADER_BYTES;
    mCapacity = header->mCapacity;
    return true;
}

std::string WolfPluginAudioRing::name() const
{
    return mShm ? mShm->getName() : std::string();
}

size_t WolfPluginAudioRing::size() const
{
    return mShm ? mShm->getSize() : 0;
}

bool WolfPluginAudioRing::read(Chunk& out)
{
    if (!mHeader)
    {
        return false;
    }
    const uint64_t r = mHeader->mReadPos.load(std::memory_order_relaxed);      // only this reader stores it
    const uint64_t w = mHeader->mWritePos.load(std::memory_order_acquire);
    if (w == r)
    {
        return false;
    }
    RecordHeader rec;
    bool bad = (w - r) > mCapacity || (w - r) < sizeof(rec);
    if (!bad)
    {
        copyOut(mData, mCapacity, r, &rec, sizeof(rec));
        bad = rec.mKind < KIND_DATA || rec.mKind > KIND_RESUME || rec.mFrames > MAX_RECORD_FRAMES
            || (rec.mKind == KIND_DATA && (rec.mFrames == 0 || rec.mRate < MIN_RATE || rec.mRate > MAX_RATE))
            || recordBytes(rec.mFrames) > w - r;
    }
    if (bad)
    {
        // The plugin is a separate process: never trust what is in the segment. Drop all of it.
        if (!mWarned)
        {
            mWarned = true;
            LL_WARNS("Plugin") << "Media plugin sound ring is malformed; emptied it" << LL_ENDL;
        }
        mHeader->mReadPos.store(w, std::memory_order_release);
        return false;
    }
    out.mKind = rec.mKind;
    out.mRate = rec.mRate;
    out.mDueUs = rec.mDueUs;
    out.mSamples.resize((size_t)rec.mFrames * CHANNELS);
    if (rec.mFrames)
    {
        copyOut(mData, mCapacity, r + sizeof(rec), out.mSamples.data(), out.mSamples.size() * sizeof(float));
        // A NaN or infinity would poison the whole mix it is added to (every sound of the viewer): make it silence.
        for (float& v : out.mSamples)
        {
            if (!std::isfinite(v))
            {
                v = 0.f;
            }
        }
    }
    mHeader->mReadPos.store(r + recordBytes(rec.mFrames), std::memory_order_release);
    return true;
}

uint32_t WolfPluginAudioRing::takeDropped()
{
    return mHeader ? mHeader->mDropped.exchange(0, std::memory_order_relaxed) : 0;
}
