/**
 * @file llaudiopcm_openal.h
 * @brief [W111 VIDEO 2026-11-24] PCM streams (media plugin sound) played in the OpenAL engine.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer Source Code
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef LL_AUDIOPCM_OPENAL_H
#define LL_AUDIOPCM_OPENAL_H

#include "llaudioengine.h"
#include "AL/al.h"
#include "AL/alc.h"
#include "AL/alext.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

// [W111 VIDEO 2026-11-24] Each PCM stream is one OpenAL streaming source (a queue of buffers) in the ENGINE'S context
// (with the VR/recorder mirror on, that is the loopback context, llaudioengine_openal.cpp initMirrorMix), so the sound
// is in the engine's mix. A feeder thread of its own - not the viewer's main loop, which stalls for seconds on a
// teleport while parcel music must play on - reads the feeds every 10 ms and keeps the sources' queues filled.
// It makes its AL calls with the engine's context bound to that thread (alcSetThreadContext, alext.h:163-170
// ALC_EXT_thread_local_context), so a temporary alcMakeContextCurrent elsewhere (the DJ cue output,
// newview/wolfdjaudio.cpp idleCue) never sends its calls to another context. OpenAL Soft's API is thread-safe.
//
// TIMING. A chunk with a due time is heard at that time: the feeder knows when the next frame it queues will be heard
// (frames still queued, from AL_SAMPLE_OFFSET_LATENCY_SOFT's read offset, plus the device latency it returns, plus
// the speakers' latency when the mix is mirrored) and queues a chunk when its due time comes up. A small error is
// steered out with AL_PITCH (at most +/-0.2 %, 3.5 cents, below what anyone hears) - the plugin's own clock and the
// sound card's drift apart by up to ~100 ppm. A big error (a stall, a seek) is fixed at once: late frames are cut,
// an early chunk gets silence in front of it. Chunks without a due time play in order with a 100 ms cushion.
//
// GAIN. The engine's listener gain is the master volume (setInternalGain), but a media's gain already holds master
// volume and the mutes (llvieweraudio.cpp audio_update_volume, as "set_volume" did), so each PCM source's gain is
// divided by the listener gain. The engine mutes itself (listener gain 0) while the progress screen is up (a teleport)
// while media and same-stream parcel music played on there before; so when the engine mutes, the listener gain stays
// 1 and the engine's own sources are silenced one by one instead (LLAudioEngine_OpenAL::setInternalGain), and the PCM
// sources keep the gain the resident set.
class LLAudioPCMStreamsOpenAL
{
public:
    /** ctx: the engine's context. extra_latency_us: called on the feeder thread; latency after the engine's device. */
    LLAudioPCMStreamsOpenAL(ALCcontext* ctx, PFNALCSETTHREADCONTEXTPROC set_thread_context,
                            std::function<S64()> extra_latency_us);
    ~LLAudioPCMStreamsOpenAL();    // stops the thread; every source and buffer is deleted on it

    U32  create(const std::shared_ptr<LLAudioPCMFeed>& feed);
    void setGain(U32 id, F32 gain);
    void destroy(U32 id);
    /** Main thread, from LLAudioEngine_OpenAL::setInternalGain, with the engine's context current: the PCM sources'
     *  gains for a new listener gain, applied in the same deferred-update batch as apply_engine (which sets the
     *  listener and the engine's own sources), so no mix ever has one without the other. */
    void setListenerGain(F32 listener_gain, const std::function<void()>& apply_engine);

    static bool supported();     // the extensions this needs, on the current context

private:
    struct Queued { ALuint mBuffer; U32 mFrames; };
    struct Pending { LLAudioPCMFeed::Chunk mChunk; size_t mOffset = 0; };   // mOffset: frames already used
    struct Stream
    {
        U32                             mId = 0;
        std::shared_ptr<LLAudioPCMFeed> mFeed;
        std::atomic<F32>                mGain{0.f};
        bool                            mDead = false;
        ALuint                          mSource = 0;
        std::deque<Pending>             mPending;
        std::deque<Queued>              mQueued;
        std::vector<ALuint>             mFree;
        U64                             mQueuedFrames = 0;   // frames of the buffers in mQueued
        U32                             mRate = 0;           // the rate of the buffers in the source's queue
        bool                            mPaused = false;
        S64                             mPausedAt = 0;
        bool                            mPlaying = false;    // we asked it to play (it may have run dry since)
        S64                             mAsapSince = 0;      // when the first as-soon-as-possible chunk waited
        F32                             mAppliedGain = -1.f;
        F32                             mPitch = 1.f;
        F32                             mErrorUs = 0.f;      // smoothed timing error, + = late
        bool                            mFailed = false;     // the source could not be made: drain and drop
        // what happened while it played, for the log when it ends (and for tests)
        U32                             mStarts = 0;         // started, or started again after running dry
        U32                             mCuts = 0;           // late frames cut (a stall, a seek)
        U32                             mFills = 0;          // silence queued to wait for a chunk's due time
        F32                             mWorstLateUs = 0.f;  // the largest |error| a queued timed chunk had
    };

    void run();
    void service(Stream& s, S64 now);
    void flush(Stream& s);
    bool ensureSource(Stream& s);
    void deleteSource(Stream& s);
    bool queueFrames(Stream& s, const F32* stereo, U32 frames, U32 rate);
    void applyGain(Stream& s, bool force);
    F32  gainScale() const;

    ALCcontext*                 mContext;
    PFNALCSETTHREADCONTEXTPROC  mSetThreadContext;
    std::function<S64()>        mExtraLatencyUs;
    LPALGETSOURCEI64VSOFT       mGetSourcei64v = nullptr;
    LPALDEFERUPDATESSOFT        mDeferUpdates = nullptr;
    LPALPROCESSUPDATESSOFT      mProcessUpdates = nullptr;
    ALint                       mDirectMode = AL_TRUE;

    std::mutex                  mMutex;          // mStreams, mListenerGain; held while the feeder works a tick
    std::condition_variable     mWake;
    std::map<U32, std::unique_ptr<Stream>> mStreams;
    U32                         mNextId = 1;
    F32                         mListenerGain = 1.f;    // what the engine set the listener to (1 while it is muted)
    bool                        mQuit = false;
    std::thread                 mThread;
};

#endif // LL_AUDIOPCM_OPENAL_H
