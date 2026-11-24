/**
 * @file wolfpluginaudio.h
 * @brief [W111 VIDEO 2026-11-24] Media plugin sound to the viewer: the shared-memory ring both processes use.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer Source Code
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef WOLF_PLUGIN_AUDIO_H
#define WOLF_PLUGIN_AUDIO_H

// [W111 VIDEO 2026-11-24] Ticket 1033 (the video recorder), Paul's decision "B": the sound of media plugins (parcel
// music, audio/video on prims, web pages) plays through the viewer's own OpenAL engine, so it is in the engine's mix
// (and so in what the recorder takes from the mirror mix, llaudioengine_openal.h setMirrorMix / setMixTap).
//
// TRANSPORT: one shared-memory ring per media plugin instance, made by the viewer (LLPluginSharedMemory::create, the
// same segments the media textures use: shm_open/mmap on Linux and macOS, CreateFileMapping on Windows,
// llpluginsharedmemory.cpp) and attached by the plugin by name (LLPluginSharedMemory::attach). The name goes to the
// plugin in the media "init" message, so the plugin knows before it makes any player or browser.
// Why not plugin messages: a message is LLSD XML through a socket, parsed on the viewer's main thread
// (LLPluginProcessParent -> LLPluginClassMedia::receivePluginMessage). 48 kHz stereo float is 384,000 bytes a second
// per medium (48000 x 2 x 4), about 512,000 as base64 before the XML around it; five media prims playing at once
// would push ~2.5 MB/s of XML through the main loop, and any main-loop stall (a teleport) would stall the sound. The
// ring costs one memcpy each side, is read by the audio engine's own feeder thread (never the main loop), and the
// plugin side never blocks for more than that memcpy.
//
// Layout: a 256-byte header, then a data area that holds records back to back, wrapping at its end. One writer (the
// plugin; its own threads are serialised by a mutex in WolfPluginAudioWriter) and one reader (the viewer's feeder
// thread). write_pos and read_pos only grow (bytes, 64-bit); the writer owns write_pos, the reader owns read_pos.
// The data are published with release stores and read after acquire loads.
// Source: C++17 [atomics.lockfree] p4: "Operations that are lock-free should also be address-free. That is, atomic
//   operations on the same memory location via two different addresses will communicate atomically. ... This
//   restriction enables communication by memory that is mapped into a process more than once and by memory that is
//   shared between two processes." The static_assert below holds the build to lock-free 64-bit atomics.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class LLPluginSharedMemory;

namespace WolfPluginAudio
{
    // The keys of the media "init" message (LLPluginClassMedia::init -> MediaPluginBase::staticReceiveMessage).
    extern const char* const KEY_THROUGH_VIEWER;    // "wolf_audio_through_viewer" (boolean)
    extern const char* const KEY_SHM_NAME;          // "wolf_audio_shm_name"
    extern const char* const KEY_SHM_SIZE;          // "wolf_audio_shm_size" (S32, bytes)

    // The ring always carries interleaved 32-bit float STEREO (MediaPluginBase turns mono into stereo before writing).
    constexpr uint32_t CHANNELS = 2;
    constexpr uint32_t MIN_RATE = 8000;
    constexpr uint32_t MAX_RATE = 384000;        // Source: VLC 3.0.x modules/audio_output/amem.c AMEM_SAMPLE_RATE_MAX
    constexpr uint32_t MAX_RECORD_FRAMES = 8192; // longer pushes are split
    // 2 MiB: 2.7 s of 48 kHz stereo float (384,000 bytes/s). The feeder thread empties it every 10 ms, so it only has
    // to ride out that thread being held up by the operating system; libVLC hands blocks over up to 3 s early
    // (AOUT_MAX_ADVANCE_TIME, vlc_aout.h:38-42) but they wait in the viewer's own queue, not here.
    constexpr size_t   RING_BYTES = 2u * 1024u * 1024u;

    enum EKind : uint32_t
    {
        KIND_DATA   = 1,    // frames of interleaved stereo float follow
        KIND_FLUSH  = 2,    // drop everything queued before this (stop, seek, new media)
        KIND_PAUSE  = 3,    // hold the sound where it is
        KIND_RESUME = 4     // carry on from where PAUSE held it
    };

    // NO_DUE: play in order, as soon as possible (a plugin that has no presentation time for its blocks).
    constexpr int64_t NO_DUE = 0;

    /** The clock both processes stamp "due" times with: std::chrono::steady_clock in microseconds.
     *  It is one system-wide clock on every platform we ship, so a time stamped in the plugin process means the same
     *  instant in the viewer:
     *  Source: libstdc++ src/c++11/chrono.cc steady_clock::now -> clock_gettime(CLOCK_MONOTONIC) (Linux);
     *  libc++ src/chrono.cpp steady_clock::now on __APPLE__ -> clock_gettime_nsec_np(CLOCK_UPTIME_RAW) (macOS);
     *  MSVC STL <chrono> steady_clock::now -> QueryPerformanceCounter (Windows; Microsoft Learn "Acquiring
     *  high-resolution time stamps": QPC is a system-wide counter). */
    int64_t nowUs();

    struct RingHeader;    // wolfpluginaudio.cpp

    struct Chunk
    {
        uint32_t           mKind = 0;
        uint32_t           mRate = 0;
        int64_t            mDueUs = NO_DUE;   // when the first frame should be heard (nowUs() time), or NO_DUE
        std::vector<float> mSamples;          // interleaved stereo, mSamples.size() / 2 frames (KIND_DATA only)
    };
}

/** Plugin side. Thread-safe: any of the plugin's threads may call write(). */
class WolfPluginAudioWriter
{
public:
    WolfPluginAudioWriter();
    ~WolfPluginAudioWriter();
    bool attach(const std::string& name, size_t size);
    bool attached() const { return mHeader != nullptr; }
    /** Writes one record; false (and the frames counted as dropped) when the ring is full or not attached. */
    bool write(uint32_t kind, uint32_t rate, int64_t due_us, const float* stereo, uint32_t frames);

private:
    std::unique_ptr<LLPluginSharedMemory> mShm;
    WolfPluginAudio::RingHeader* mHeader = nullptr;
    uint8_t*      mData = nullptr;
    uint64_t      mCapacity = 0;
    std::mutex    mMutex;
};

/** Viewer side: owns the segment. One thread reads (the audio engine's feeder); create() before the plugin starts. */
class WolfPluginAudioRing
{
public:
    WolfPluginAudioRing();
    ~WolfPluginAudioRing();
    bool create(size_t size);
    std::string name() const;
    size_t size() const;
    /** The next record, false when none is waiting. A malformed ring is emptied (and logged once). */
    bool read(WolfPluginAudio::Chunk& out);
    /** Frames the plugin could not write because the ring was full (since the last call). */
    uint32_t takeDropped();

private:
    std::unique_ptr<LLPluginSharedMemory> mShm;
    WolfPluginAudio::RingHeader* mHeader = nullptr;
    uint8_t*      mData = nullptr;
    uint64_t      mCapacity = 0;
    bool          mWarned = false;
};

#endif // WOLF_PLUGIN_AUDIO_H
