/**
 * @file llaudiopcm_openal.cpp
 * @brief [W111 VIDEO 2026-11-24] PCM streams (media plugin sound) played in the OpenAL engine (see the header).
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
#include "llaudiopcm_openal.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace
{
    constexpr S64 TICK_US = 10000;              // the feeder's period
    // Timed chunks: a chunk whose start would be heard more than this late has the late part cut; one that would be
    // heard more than this early waits (with silence queued in front of it while the source must keep playing).
    // 80 ms: between the ~45 ms at which late sound starts to be noticed against the picture and the 125 ms at which
    // early sound is (ITU-R BT.1359-1, detectability thresholds +45 ms / -125 ms), the pitch steering below keeps the
    // error well inside it; a hard cut only happens after a stall or a seek.
    constexpr S64 LATE_CUT_US = 80000;
    constexpr S64 EARLY_WAIT_US = 20000;        // two ticks: queue a chunk that falls due before the next tick or so
    constexpr S64 KEEP_FED_US = 40000;          // while playing, never let less than this be queued (silence fills)
    constexpr S64 MAX_SILENCE_US = 250000;      // a gap longer than this lets the source run dry and start again
    // Pitch steering (AL_PITCH): +/- 0.2 % at most (3.5 cents), a 3 ms dead band, the error halved in ~5 s.
    constexpr F32 PITCH_MAX = 0.002f;
    constexpr F32 PITCH_DEADBAND_US = 3000.f;
    constexpr F32 PITCH_PER_US = 1.f / 5.0e6f;
    // As-soon-as-possible chunks (no due time): start with 100 ms queued, or once the first waited 100 ms (a short
    // sound); if more than 500 ms piles up (the plugin's clock runs fast against the sound card), cut back to 150 ms.
    constexpr S64 ASAP_START_US = 100000;
    constexpr S64 ASAP_MAX_US = 500000;
    constexpr S64 ASAP_TARGET_US = 150000;
    constexpr S64 MAX_PENDING_US = 6000000;     // libVLC hands blocks over up to 3 s early (vlc_aout.h AOUT_MAX_ADVANCE_TIME)
    constexpr size_t MAX_BUFFERS = 512;         // per stream
    constexpr F32 MAX_SOURCE_GAIN = 1000.f;     // Source: openal-soft 1.24.2 alc/alu.h:18 GainMixMax (+60 dB)

    S64 framesToUs(U64 frames, U32 rate)
    {
        return rate ? (S64)(frames * 1000000ull / rate) : 0;
    }

    U64 usToFrames(S64 us, U32 rate)
    {
        return us > 0 ? (U64)us * rate / 1000000ull : 0;
    }
}

bool LLAudioPCMStreamsOpenAL::supported()
{
    // Source: packages/include/AL/alext.h - AL_EXT_float32 (:53-57, AL_FORMAT_STEREO_FLOAT32), ALC_EXT_thread_local_context
    // (:163-170), AL_SOFT_direct_channels (:281-284), AL_SOFT_source_latency (:328-360), AL_SOFT_deferred_updates
    // (:367-375). OpenAL Soft 1.24.2 (autobuild.xml, all three platforms) has every one of them.
    return alIsExtensionPresent("AL_EXT_FLOAT32") && alcIsExtensionPresent(nullptr, "ALC_EXT_thread_local_context")
        && alIsExtensionPresent("AL_SOFT_direct_channels") && alIsExtensionPresent("AL_SOFT_source_latency")
        && alIsExtensionPresent("AL_SOFT_deferred_updates");
}

LLAudioPCMStreamsOpenAL::LLAudioPCMStreamsOpenAL(ALCcontext* ctx, PFNALCSETTHREADCONTEXTPROC set_thread_context,
                                                 std::function<S64()> extra_latency_us)
    : mContext(ctx), mSetThreadContext(set_thread_context), mExtraLatencyUs(std::move(extra_latency_us))
{
    mGetSourcei64v = (LPALGETSOURCEI64VSOFT)alGetProcAddress("alGetSourcei64vSOFT");
    mDeferUpdates = (LPALDEFERUPDATESSOFT)alGetProcAddress("alDeferUpdatesSOFT");
    mProcessUpdates = (LPALPROCESSUPDATESSOFT)alGetProcAddress("alProcessUpdatesSOFT");
    // Source: alext.h:505-509 AL_SOFT_direct_channels_remix - a channel the output has no speaker for is mixed into
    // the ones it has, rather than dropped (AL_TRUE = AL_DROP_UNMATCHED_SOFT). Stereo on stereo is direct either way.
    if (alIsExtensionPresent("AL_SOFT_direct_channels_remix"))
    {
        mDirectMode = AL_REMIX_UNMATCHED_SOFT;
    }
    mThread = std::thread([this]() { run(); });
}

LLAudioPCMStreamsOpenAL::~LLAudioPCMStreamsOpenAL()
{
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mQuit = true;
    }
    mWake.notify_all();
    if (mThread.joinable())
    {
        mThread.join();
    }
}

U32 LLAudioPCMStreamsOpenAL::create(const std::shared_ptr<LLAudioPCMFeed>& feed)
{
    if (!feed)
    {
        return 0;
    }
    std::lock_guard<std::mutex> lock(mMutex);
    std::unique_ptr<Stream> s(new Stream);
    s->mId = mNextId++;
    if (mNextId == 0)
    {
        mNextId = 1;
    }
    s->mFeed = feed;
    const U32 id = s->mId;
    mStreams[id] = std::move(s);
    return id;
}

void LLAudioPCMStreamsOpenAL::setGain(U32 id, F32 gain)
{
    std::lock_guard<std::mutex> lock(mMutex);
    auto it = mStreams.find(id);
    if (it != mStreams.end())
    {
        it->second->mGain.store(llclamp(gain, 0.f, 1.f));
    }
}

void LLAudioPCMStreamsOpenAL::destroy(U32 id)
{
    std::lock_guard<std::mutex> lock(mMutex);
    auto it = mStreams.find(id);
    if (it != mStreams.end())
    {
        // The feeder deletes the source (on its thread, with the engine's context) and drops the feed.
        it->second->mDead = true;
    }
}

F32 LLAudioPCMStreamsOpenAL::gainScale() const
{
    return mListenerGain > 0.f ? 1.f / mListenerGain : 1.f;
}

void LLAudioPCMStreamsOpenAL::setListenerGain(F32 listener_gain, const std::function<void()>& apply_engine)
{
    std::lock_guard<std::mutex> lock(mMutex);
    // Source: alext.h:367-375 AL_SOFT_deferred_updates - property changes made between alDeferUpdatesSOFT and
    // alProcessUpdatesSOFT reach the mixer together.
    if (mDeferUpdates)
    {
        mDeferUpdates();
    }
    apply_engine();
    mListenerGain = listener_gain;
    for (auto& entry : mStreams)
    {
        Stream& s = *entry.second;
        if (s.mSource)
        {
            applyGain(s, true);     // the main thread's current context is the engine's, where the sources are
        }
    }
    if (mProcessUpdates)
    {
        mProcessUpdates();
    }
}

void LLAudioPCMStreamsOpenAL::applyGain(Stream& s, bool force)
{
    const F32 gain = llclamp(s.mGain.load() * gainScale(), 0.f, MAX_SOURCE_GAIN);
    if (force || gain != s.mAppliedGain)
    {
        alSourcef(s.mSource, AL_GAIN, gain);
        s.mAppliedGain = gain;
    }
}

bool LLAudioPCMStreamsOpenAL::ensureSource(Stream& s)
{
    if (s.mSource)
    {
        return true;
    }
    if (s.mFailed)
    {
        return false;
    }
    alGetError();
    ALuint src = 0;
    alGenSources(1, &src);
    if (alGetError() != AL_NO_ERROR || !src)
    {
        LL_WARNS("Media") << "No OpenAL source for media sound" << LL_ENDL;
        s.mFailed = true;
        return false;
    }
    // Heard as the plugin played it: no position, no distance, no doppler; the two channels straight to the left and
    // right speakers (Source: alext.h:281-284 AL_DIRECT_CHANNELS_SOFT - no panning, no HRTF).
    alSourcei(src, AL_SOURCE_RELATIVE, AL_TRUE);
    alSource3f(src, AL_POSITION, 0.f, 0.f, 0.f);
    alSource3f(src, AL_VELOCITY, 0.f, 0.f, 0.f);
    alSourcef(src, AL_ROLLOFF_FACTOR, 0.f);
    alSourcef(src, AL_DOPPLER_FACTOR, 0.f);
    alSourcei(src, AL_LOOPING, AL_FALSE);
    alSourcei(src, AL_DIRECT_CHANNELS_SOFT, mDirectMode);
    // The gain is divided by the listener gain (see the header), so it can go above 1; Source: openal-soft 1.24.2
    // al/source.cpp:1535-1538 AL_MAX_GAIN only has to be >= 0.
    alSourcef(src, AL_MAX_GAIN, MAX_SOURCE_GAIN);
    alSourcef(src, AL_PITCH, 1.f);
    s.mSource = src;
    s.mPitch = 1.f;
    s.mAppliedGain = -1.f;
    applyGain(s, true);
    return true;
}

void LLAudioPCMStreamsOpenAL::flush(Stream& s)
{
    s.mPending.clear();
    s.mErrorUs = 0.f;
    s.mAsapSince = 0;
    s.mPlaying = false;
    if (!s.mSource)
    {
        return;
    }
    // Source: OpenAL 1.1 spec 4.3.5 (alSourceStop: every queued buffer counts as processed) and 4.3.6 - AL_BUFFER 0
    // on a stopped source empties its queue.
    alSourceStop(s.mSource);
    alSourcei(s.mSource, AL_BUFFER, 0);
    for (const Queued& q : s.mQueued)
    {
        s.mFree.push_back(q.mBuffer);
    }
    s.mQueued.clear();
    s.mQueuedFrames = 0;
    if (s.mPitch != 1.f)
    {
        s.mPitch = 1.f;
        alSourcef(s.mSource, AL_PITCH, 1.f);
    }
}

void LLAudioPCMStreamsOpenAL::deleteSource(Stream& s)
{
    if (s.mSource)
    {
        LL_INFOS("MediaSound") << "Media sound stream " << s.mId << " ended: " << s.mStarts << " start(s), " << s.mCuts
                               << " late cut(s), " << s.mFills << " silence fill(s), worst timing error "
                               << (S32)(s.mWorstLateUs / 1000.f) << " ms" << LL_ENDL;
        flush(s);
        alDeleteSources(1, &s.mSource);
        s.mSource = 0;
    }
    if (!s.mFree.empty())
    {
        alDeleteBuffers((ALsizei)s.mFree.size(), s.mFree.data());
        s.mFree.clear();
    }
}

bool LLAudioPCMStreamsOpenAL::queueFrames(Stream& s, const F32* stereo, U32 frames, U32 rate)
{
    if (!frames)
    {
        return true;
    }
    if (s.mQueued.size() >= MAX_BUFFERS)
    {
        return false;
    }
    ALuint buf = 0;
    if (!s.mFree.empty())
    {
        buf = s.mFree.back();
        s.mFree.pop_back();
    }
    else
    {
        alGetError();
        alGenBuffers(1, &buf);
        if (alGetError() != AL_NO_ERROR || !buf)
        {
            return false;
        }
    }
    static std::vector<F32> silence;    // feeder thread only
    if (!stereo)
    {
        if (silence.size() < (size_t)frames * 2)
        {
            silence.assign((size_t)frames * 2, 0.f);
        }
        stereo = silence.data();
    }
    // Source: alext.h:53-57 AL_EXT_float32 AL_FORMAT_STEREO_FLOAT32 - interleaved 32-bit float, -1..1.
    alGetError();
    alBufferData(buf, AL_FORMAT_STEREO_FLOAT32, stereo, (ALsizei)(frames * 2 * sizeof(F32)), (ALsizei)rate);
    alSourceQueueBuffers(s.mSource, 1, &buf);
    if (alGetError() != AL_NO_ERROR)
    {
        s.mFree.push_back(buf);
        return false;
    }
    s.mQueued.push_back({ buf, frames });
    s.mQueuedFrames += frames;
    s.mRate = rate;
    return true;
}

void LLAudioPCMStreamsOpenAL::run()
{
    // Source: alext.h:163-170 ALC_EXT_thread_local_context - this thread's AL calls go to the engine's context
    // whatever context another thread makes current.
    if (!mSetThreadContext || !mSetThreadContext(mContext))
    {
        LL_WARNS("Media") << "Media sound: could not bind the engine's OpenAL context to the feeder thread" << LL_ENDL;
        std::unique_lock<std::mutex> lock(mMutex);
        while (!mQuit)
        {
            // No AL calls are possible here: drop every feed's data so the plugins' rings do not fill.
            for (auto& entry : mStreams)
            {
                LLAudioPCMFeed::Chunk c;
                while (entry.second->mFeed && entry.second->mFeed->read(c)) {}
            }
            for (auto it = mStreams.begin(); it != mStreams.end();)
            {
                it = it->second->mDead ? mStreams.erase(it) : std::next(it);
            }
            mWake.wait_for(lock, std::chrono::microseconds(TICK_US));
        }
        mStreams.clear();
        return;
    }

    std::unique_lock<std::mutex> lock(mMutex);
    while (!mQuit)
    {
        for (auto it = mStreams.begin(); it != mStreams.end();)
        {
            Stream& s = *it->second;
            if (s.mDead)
            {
                deleteSource(s);
                it = mStreams.erase(it);
                continue;
            }
            service(s, s.mFeed->nowUs());
            ++it;
        }
        mWake.wait_for(lock, std::chrono::microseconds(TICK_US));
    }
    for (auto& entry : mStreams)
    {
        deleteSource(*entry.second);
    }
    mStreams.clear();
    lock.unlock();
    mSetThreadContext(nullptr);
}

void LLAudioPCMStreamsOpenAL::service(Stream& s, S64 now)
{
    // 1. Everything the plugin has written since the last tick.
    LLAudioPCMFeed::Chunk c;
    while (s.mFeed->read(c))
    {
        switch (c.mKind)
        {
        case LLAudioPCMFeed::KIND_DATA:
            if (c.mSamples.size() >= 2 && c.mRate)
            {
                if (!c.mDueUs && !s.mAsapSince)
                {
                    s.mAsapSince = now;
                }
                s.mPending.emplace_back();
                s.mPending.back().mChunk = std::move(c);
                c = LLAudioPCMFeed::Chunk();
            }
            break;
        case LLAudioPCMFeed::KIND_FLUSH:
            flush(s);
            break;
        case LLAudioPCMFeed::KIND_PAUSE:
            if (!s.mPaused)
            {
                s.mPaused = true;
                s.mPausedAt = now;
                if (s.mSource && s.mPlaying)
                {
                    alSourcePause(s.mSource);
                }
            }
            break;
        case LLAudioPCMFeed::KIND_RESUME:
            if (s.mPaused)
            {
                // What was handed over before the pause is due later by as long as the pause lasted.
                const S64 shift = now - s.mPausedAt;
                for (Pending& p : s.mPending)
                {
                    if (p.mChunk.mDueUs)
                    {
                        p.mChunk.mDueUs += shift;
                    }
                }
                s.mPaused = false;
                if (s.mSource && s.mPlaying)
                {
                    alSourcePlay(s.mSource);    // a paused source carries on from where it was (OpenAL 1.1 4.3.5)
                }
            }
            break;
        default:
            break;
        }
    }
    // A plugin that hands over far more than it plays (or a viewer that cannot keep up) must not grow without end.
    S64 pending_us = 0;
    for (const Pending& p : s.mPending)
    {
        pending_us += framesToUs(p.mChunk.mSamples.size() / 2 - p.mOffset, p.mChunk.mRate);
    }
    while (pending_us > MAX_PENDING_US && !s.mPending.empty())
    {
        const Pending& p = s.mPending.front();
        pending_us -= framesToUs(p.mChunk.mSamples.size() / 2 - p.mOffset, p.mChunk.mRate);
        s.mPending.pop_front();
    }

    if (!s.mSource && s.mPending.empty())
    {
        return;     // a medium that never makes a sound (most web pages) never gets a source
    }
    if (!ensureSource(s))
    {
        s.mPending.clear();
        return;
    }
    applyGain(s, false);

    // 2. Buffers the source has finished with.
    ALint processed = 0;
    alGetSourcei(s.mSource, AL_BUFFERS_PROCESSED, &processed);
    while (processed-- > 0 && !s.mQueued.empty())
    {
        ALuint b = 0;
        alSourceUnqueueBuffers(s.mSource, 1, &b);
        s.mQueuedFrames -= std::min<U64>(s.mQueuedFrames, s.mQueued.front().mFrames);
        s.mQueued.pop_front();
        s.mFree.push_back(b);
    }
    if (s.mPaused)
    {
        return;
    }

    // 3. When the next frame queued now would be heard.
    ALint state = AL_INITIAL;
    alGetSourcei(s.mSource, AL_SOURCE_STATE, &state);
    const bool running = state == AL_PLAYING;
    if (!running && s.mPlaying)
    {
        // Ran dry (or the device went away and came back): what is still queued starts again below.
        s.mPlaying = false;
        s.mErrorUs = 0.f;
    }
    U64 played = 0;
    S64 latency_us = 0;
    if (mGetSourcei64v)
    {
        // Source: openal-soft 1.24.2 al/source.cpp:2203-2227 AL_SAMPLE_OFFSET_LATENCY_SOFT - [0] the read offset from
        // the start of the queue in 32.32 fixed-point frames (:200-237), [1] the device's latency in nanoseconds.
        ALint64SOFT v[2] = { 0, 0 };
        mGetSourcei64v(s.mSource, AL_SAMPLE_OFFSET_LATENCY_SOFT, v);
        played = running && v[0] > 0 ? (U64)(v[0] >> 32) : 0;
        latency_us = v[1] > 0 ? (S64)(v[1] / 1000) : 0;
    }
    latency_us += mExtraLatencyUs ? mExtraLatencyUs() : 0;
    const U64 remaining = s.mQueuedFrames > played ? s.mQueuedFrames - played : 0;
    S64 ahead_us = (S64)((F64)framesToUs(remaining, s.mRate) / (F64)s.mPitch);

    // 4. Queue what falls due.
    bool queued_timed = false;
    while (!s.mPending.empty() && s.mQueued.size() < MAX_BUFFERS)
    {
        Pending& p = s.mPending.front();
        const U32 rate = p.mChunk.mRate;
        const U64 total = p.mChunk.mSamples.size() / 2;
        if (p.mOffset >= total)
        {
            s.mPending.pop_front();
            continue;
        }
        if (rate != s.mRate && s.mQueuedFrames > 0)
        {
            break;      // OpenAL queues one rate only (al/source.cpp:3615): the new rate starts once the queue drains
        }
        const U32 left = (U32)(total - p.mOffset);
        const F32* data = p.mChunk.mSamples.data() + p.mOffset * 2;
        const bool started = running || s.mQueuedFrames > 0;
        const S64 heard_at = now + latency_us + (started ? ahead_us : 0);

        if (!p.mChunk.mDueUs)
        {
            // As soon as possible, in order. Too much piled up = the plugin's clock runs ahead of the sound card's.
            if (started && ahead_us > ASAP_MAX_US)
            {
                const U64 cut = std::min<U64>(left, usToFrames(ahead_us - ASAP_TARGET_US, rate));
                p.mOffset += cut;
                ahead_us -= framesToUs(cut, rate);
                continue;
            }
            if (!queueFrames(s, data, left, rate))
            {
                break;
            }
            ahead_us += (S64)((F64)framesToUs(left, rate) / (F64)s.mPitch);
            s.mPending.pop_front();
            continue;
        }

        const S64 due = p.mChunk.mDueUs + framesToUs(p.mOffset, rate);
        const S64 late = heard_at - due;
        if (late > LATE_CUT_US)
        {
            // Would be heard too late (after a stall or a seek): cut the late part, keep the rest on time.
            const U64 cut = std::max<U64>(1, std::min<U64>(left, usToFrames(late, rate)));
            p.mOffset += cut;
            ++s.mCuts;
            LL_DEBUGS("MediaSound") << "stream " << s.mId << ": " << late / 1000 << " ms late, cut " << cut << " frames" << LL_ENDL;
            continue;
        }
        if (late < -EARLY_WAIT_US)
        {
            // Not due yet. While the source plays and would run dry before the next tick, fill with silence up to
            // the due time; a long gap lets it run dry and the chunk starts it again on time.
            if (started && ahead_us < KEEP_FED_US && -late <= MAX_SILENCE_US)
            {
                const U32 fill = (U32)std::min<U64>(usToFrames(-late, rate), usToFrames(KEEP_FED_US, rate));
                if (fill && (rate == s.mRate || s.mQueuedFrames == 0) && queueFrames(s, nullptr, fill, rate))
                {
                    ++s.mFills;
                    ahead_us += (S64)((F64)framesToUs(fill, rate) / (F64)s.mPitch);
                    continue;
                }
            }
            break;
        }
        if (!started && late < 0)
        {
            // Starting: silence in front so the first frame is heard at its due time.
            const U32 fill = (U32)usToFrames(-late, rate);
            if (fill && !queueFrames(s, nullptr, fill, rate))
            {
                break;
            }
            ahead_us += framesToUs(fill, rate);
        }
        else if (started)
        {
            // Small errors: steer with the pitch (smoothed so one odd block does not swing it).
            s.mErrorUs = 0.9f * s.mErrorUs + 0.1f * (F32)late;
            s.mWorstLateUs = std::max(s.mWorstLateUs, std::fabs((F32)late));
        }
        if (!queueFrames(s, data, left, rate))
        {
            break;
        }
        ahead_us += (S64)((F64)framesToUs(left, rate) / (F64)s.mPitch);
        queued_timed = true;
        s.mPending.pop_front();
    }

    // 5. Pitch: + error (late) plays a touch faster until it is gone.
    F32 pitch = 1.f;
    if (running && std::fabs(s.mErrorUs) > PITCH_DEADBAND_US)
    {
        pitch = 1.f + llclamp(s.mErrorUs * PITCH_PER_US, -PITCH_MAX, PITCH_MAX);
    }
    if (std::fabs(pitch - s.mPitch) > 1.0e-5f)
    {
        s.mPitch = pitch;
        alSourcef(s.mSource, AL_PITCH, pitch);
    }

    // 6. Start (or restart after running dry).
    if (!running && s.mQueuedFrames > 0)
    {
        bool go = queued_timed;
        if (!go)
        {
            go = framesToUs(s.mQueuedFrames, s.mRate) >= ASAP_START_US
                || (s.mAsapSince && now - s.mAsapSince >= ASAP_START_US)
                || !s.mAsapSince;
        }
        if (go)
        {
            ++s.mStarts;
            LL_DEBUGS("MediaSound") << "stream " << s.mId << ": start with " << framesToUs(s.mQueuedFrames, s.mRate) / 1000
                                    << " ms queued, latency " << latency_us / 1000 << " ms" << LL_ENDL;
            alSourcePlay(s.mSource);
            s.mPlaying = true;
            s.mAsapSince = 0;
        }
    }
    else if (running)
    {
        s.mAsapSince = 0;
    }
}
