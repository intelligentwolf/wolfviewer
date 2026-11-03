/**
 * @file wolfvraudio.cpp
 * @brief WolfViewer: the viewer's sound to the VR headset, and the headset's microphone into voice chat.
 *        See wolfvraudio.h.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "wolfvraudio.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#include "llaudioengine.h"
#include "llframetimer.h"
#if LL_OPENAL
#include "llaudioengine_openal.h"
#endif
#include "llviewercontrol.h"
#include "llwebrtc.h"
#include "wolfvrpipe.h"
#include "wolfvrstream.h"

// [VR AUDIO 2026-11-04] Threads: pump() on the main thread; onEngineMix on OpenAL's mixer thread (the speakers'
// device, llaudioengine_openal.cpp onOutputPull); the voice tap on WebRTC's playout thread; onMic on the VR pipe's
// io thread; HeadsetMic::pullMic on the ADM's 10 ms mic clock (llwebrtc.cpp micClock). Between them only atomics
// and single-producer / single-consumer rings; the one mutex (gEmitMutex) is held for one submitAudio call.

namespace
{
    constexpr U32 kRate = 48000;   // the headset stream (wolfvrpipe.h submitAudio) and the engine mirror's rate
    constexpr U32 kBlock = 480;    // 10 ms

    // One producer thread, one consumer thread. Counts only grow; the consumer may also skip (drop the oldest).
    template <typename T> class SpscRing
    {
    public:
        explicit SpscRing(size_t capacity) : mBuf(capacity) {}
        size_t size() const { return mWrite.load(std::memory_order_acquire) - mRead.load(std::memory_order_acquire); }
        size_t push(const T* p, size_t n)   // producer
        {
            const size_t w = mWrite.load(std::memory_order_relaxed);
            const size_t room = mBuf.size() - (w - mRead.load(std::memory_order_acquire));
            n = std::min(n, room);
            for (size_t k = 0; k < n; ++k) mBuf[(w + k) % mBuf.size()] = p[k];
            mWrite.store(w + n, std::memory_order_release);
            return n;
        }
        size_t pop(T* p, size_t n)   // consumer
        {
            const size_t r = mRead.load(std::memory_order_relaxed);
            n = std::min(n, mWrite.load(std::memory_order_acquire) - r);
            for (size_t k = 0; k < n; ++k) p[k] = mBuf[(r + k) % mBuf.size()];
            mRead.store(r + n, std::memory_order_release);
            return n;
        }
        void skip(size_t n)   // consumer
        {
            const size_t r = mRead.load(std::memory_order_relaxed);
            n = std::min(n, mWrite.load(std::memory_order_acquire) - r);
            mRead.store(r + n, std::memory_order_release);
        }

    private:
        std::vector<T> mBuf;
        std::atomic<size_t> mWrite{0};
        std::atomic<size_t> mRead{0};
    };

    // Linear resampling to 48 kHz, CH interleaved float channels; keeps its place between calls (one thread).
    template <int CH> struct ToRate
    {
        double pos = -1.0;   // the next output's position in this call's input; -1 = the last sample of the call before
        float last[CH] = {};
        void run(const float* in, size_t frames, U32 rate_in, std::vector<float>& out)
        {
            out.clear();
            if (!frames || !rate_in) return;
            if (rate_in == kRate)
            {
                out.assign(in, in + frames * CH);
                for (int c = 0; c < CH; ++c) last[c] = in[(frames - 1) * CH + c];
                pos = -1.0;
                return;
            }
            const double step = (double)rate_in / (double)kRate;
            const double end = (double)frames - 1.0;
            while (pos < end)
            {
                const double fl = std::floor(pos);
                const long i = (long)fl;
                const float f = (float)(pos - fl);
                for (int c = 0; c < CH; ++c)
                {
                    const float a = i < 0 ? last[c] : in[(size_t)i * CH + c];
                    const float b = in[(size_t)(i + 1) * CH + c];
                    out.push_back(a + (b - a) * f);
                }
                pos += step;
            }
            pos -= (double)frames;
            for (int c = 0; c < CH; ++c) last[c] = in[(frames - 1) * CH + c];
        }
    };

    inline int16_t toS16(float x)
    {
        x = std::max(-1.f, std::min(1.f, x));
        return (int16_t)std::lrintf(x * 32767.f);
    }

    // ---- shared state ----
    std::atomic<bool> gSend{false};            // blocks go to the headset now
    std::atomic<bool> gMicOn{false};           // the headset microphone feeds voice now
    std::atomic<double> gEngineLastMs{-1e9};   // the last time the engine mirror delivered a mix
    std::mutex gEmitMutex;                     // two threads can emit (engine, or voice alone); one at a time

    // 10 ms blocks of 48 kHz stereo 16-bit, filled by one thread each
    struct Block
    {
        int16_t pcm[kBlock * 2];
        U32 n = 0;
        void add(float l, float r)
        {
            pcm[n * 2] = toS16(l);
            pcm[n * 2 + 1] = toS16(r);
            if (++n == kBlock)
            {
                // stamped with the viewer clock (wolfvrpipe.h clockMs, the pictures' clock) at the block's first sample
                const double mix_ms = wolfvr::clockMs() - 1000.0 * (double)kBlock / (double)kRate;
                std::lock_guard<std::mutex> lk(gEmitMutex);
                WolfVRStream::submitAudio(pcm, kBlock, 2, kRate, mix_ms);
                n = 0;
            }
        }
    };
    Block gEngineBlock;   // OpenAL mixer thread
    Block gVoiceBlock;    // WebRTC playout thread (voice alone, when there is no engine mirror)

    // ---- voice chat ----
    SpscRing<float> gVoiceRing(kRate);   // 0.5 s of stereo frames (2 floats each): WebRTC playout -> engine mix

    bool engineMirrorLive()
    {
        return wolfvr::clockMs() - gEngineLastMs.load() < 100.0;
    }

    struct VoiceTap : public llwebrtc::LLWebRTCPlayoutTap
    {
        std::vector<float> stereo, out;
        ToRate<2> rs;
        void onPlayoutAudio(const int16_t* s, size_t frames, size_t channels, uint32_t rate) override
        {
            if (!gSend.load() || !s || !frames || !channels) return;
            stereo.resize(frames * 2);
            for (size_t i = 0; i < frames; ++i)
            {
                const float l = (float)s[i * channels] / 32768.f;
                const float r = channels > 1 ? (float)s[i * channels + 1] / 32768.f : l;
                stereo[i * 2] = l;
                stereo[i * 2 + 1] = r;
            }
            rs.run(stereo.data(), frames, rate, out);
            if (engineMirrorLive())
            {
                gVoiceRing.push(out.data(), out.size() & ~(size_t)1);   // the engine's mix picks it up
            }
            else
            {
                for (size_t i = 0; i + 1 < out.size(); i += 2) gVoiceBlock.add(out[i], out[i + 1]);
            }
        }
    };
    VoiceTap sVoiceTap;

#if LL_OPENAL
    // OpenAL's mixer thread: the engine's whole mix, `frames` stereo float frames at `rate` (kRate).
    void onEngineMix(const F32* st, U32 frames, U32 rate, void*)
    {
        gEngineLastMs.store(wolfvr::clockMs());
        if (!gSend.load() || rate != kRate) return;
        // Voice and engine run on different clocks: keep the voice queue short (it never grows past 120 ms; back
        // to 40 ms when it does), so voice stays in step with the world and with the pictures.
        const size_t fill = gVoiceRing.size();
        if (fill > 2 * kRate * 120 / 1000) gVoiceRing.skip(fill - 2 * kRate * 40 / 1000);
        float voice[2 * 256];
        U32 done = 0;
        while (done < frames)
        {
            const U32 chunk = std::min<U32>(frames - done, 256);
            const size_t got = gVoiceRing.pop(voice, (size_t)chunk * 2) / 2;
            for (U32 i = 0; i < chunk; ++i)
            {
                const float vl = i < got ? voice[i * 2] : 0.f;
                const float vr = i < got ? voice[i * 2 + 1] : 0.f;
                gEngineBlock.add(st[(done + i) * 2] + vl, st[(done + i) * 2 + 1] + vr);
            }
            done += chunk;
        }
    }
#endif

    // ---- the headset microphone ----
    SpscRing<int16_t> gMicRing(kRate / 2);   // 0.5 s mono: pipe io thread -> ADM mic clock
    ToRate<1> gMicRate;                      // pipe io thread only
    std::vector<float> gMicIn, gMicOut;      // pipe io thread only
    std::vector<int16_t> gMicS16;            // pipe io thread only
    std::atomic<U32> gMicUnderruns{0};

    void onMic(const int16_t* pcm, uint32_t frames, uint32_t rate)
    {
        if (!gMicOn.load()) return;
        gMicIn.resize(frames);
        for (uint32_t i = 0; i < frames; ++i) gMicIn[i] = (float)pcm[i] / 32768.f;
        gMicRate.run(gMicIn.data(), frames, rate, gMicOut);
        gMicS16.resize(gMicOut.size());
        for (size_t i = 0; i < gMicOut.size(); ++i) gMicS16[i] = toS16(gMicOut[i]);
        gMicRing.push(gMicS16.data(), gMicS16.size());   // a full ring drops the newest; pullMic trims the queue
    }

    struct HeadsetMic : public llwebrtc::LLWebRTCExternalMic
    {
        bool primed = false;   // mic clock thread only
        size_t pullMic(int16_t* out, size_t frames, uint32_t rate) override
        {
            if (rate != kRate) return 0;
            size_t have = gMicRing.size();
            // network bursts: never more than 150 ms behind (back to 40 ms), so the voice does not drift late
            if (have > kRate * 150 / 1000)
            {
                gMicRing.skip(have - kRate * 40 / 1000);
                have = gMicRing.size();
            }
            if (!primed)
            {
                if (have < kRate * 30 / 1000) return 0;   // 30 ms in hand before starting (and after a gap)
                primed = true;
            }
            const size_t got = gMicRing.pop(out, frames);
            if (got < frames)
            {
                primed = false;
                gMicUnderruns++;
            }
            return got;
        }
    };
    HeadsetMic sHeadsetMic;

    // ---- main-thread state ----
    llwebrtc::LLWebRTCDeviceInterface* gVoiceDev = nullptr;
    bool gVoiceTapSet = false, gVoiceSilenced = false, gMicSet = false, gWasWanted = false, gMicTold = false;
    F64 gVoiceRefresh = 0.0;
    std::string gProblem;

    void applyVoice(llwebrtc::LLWebRTCDeviceInterface* dev, bool tap, bool silence, bool mic)
    {
        dev->setVRPlayoutTap(tap ? &sVoiceTap : nullptr);
        dev->setPlayoutSilenced(silence);
        dev->setExternalMic(mic ? &sHeadsetMic : nullptr);   // joins/starts the mic clock only when it changes
    }
}

namespace WolfVRAudio
{
void pump()
{
    static LLCachedControl<std::string> out_mode(gSavedSettings, "WolfVRAudioOut", "headset");
    static LLCachedControl<bool> use_mic(gSavedSettings, "WolfVRHeadsetMic", false);

    const bool wanted = WolfVRStream::audioWanted();   // a page is connected and plays the sound
    const std::string mode = out_mode();
    const bool pc_only = mode == "pc";
    const bool headset_only = !pc_only && mode != "both";   // "headset" and anything unknown: the default
    const bool send = wanted && !pc_only;
    const bool want_mic = wanted && use_mic();
    gSend = send;

    // The engine's mix (OpenAL mirror). The speakers are only ever silenced while the mirror sends to the headset.
    bool mirrored = false;
#if LL_OPENAL
    if (LLAudioEngine_OpenAL* al = dynamic_cast<LLAudioEngine_OpenAL*>(gAudiop))
    {
        mirrored = al->isMixMirrored();
        if (mirrored)
        {
            al->setMixTap(send ? &onEngineMix : nullptr, nullptr);
            al->setSpeakersMuted(send && headset_only);
        }
    }
#endif

    // Voice chat. Voice is silenced on the PC only while its copy reaches the headset (always, when sending).
    const bool silence_voice = send && headset_only;
    llwebrtc::LLWebRTCDeviceInterface* dev = llwebrtc::getDeviceInterface();
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (dev && (dev != gVoiceDev || send != gVoiceTapSet || silence_voice != gVoiceSilenced || want_mic != gMicSet ||
                now >= gVoiceRefresh))
    {
        applyVoice(dev, send, silence_voice, want_mic);
        gVoiceDev = dev;
        gVoiceTapSet = send;
        gVoiceSilenced = silence_voice;
        gMicSet = want_mic;
        gVoiceRefresh = now + 1.0;
    }

    // The headset microphone: the pipe hands its sound to onMic only while it is wanted.
    if (want_mic != gMicOn.load())
    {
        wolfvr::setMicSink(want_mic ? &onMic : nullptr);
        gMicOn = want_mic;
    }
    // Tell the page whether to send its microphone (and again each time a page starts playing the sound).
    if (want_mic != gMicTold || (wanted && !gWasWanted))
    {
        WolfVRStream::sendText(std::string("{\"t\":\"micwant\",\"on\":") + (want_mic ? "true" : "false") + "}");
        gMicTold = want_mic;
    }
    gWasWanted = wanted;

    // What the resident should know (shown by the floater; plain words).
    gProblem.clear();
    if (send && gAudiop && !mirrored)
    {
#if LL_OPENAL
        if (!LLAudioEngine_OpenAL::mirrorMixRequested())
        {
            gProblem = "Restart WolfViewer to hear the world's sounds in the headset. Voice chat already plays there.";
            if (headset_only) gProblem += " Until then, the other sounds still play on this PC.";
        }
        else
#endif
        {
            gProblem = "This PC's sound system could not send the world's sounds to the headset, so they play on this "
                       "PC. Voice chat plays in the headset.";
        }
    }
}

void shutdown()
{
    gSend = false;
    gMicOn = false;
    wolfvr::setMicSink(nullptr);
#if LL_OPENAL
    if (LLAudioEngine_OpenAL* al = dynamic_cast<LLAudioEngine_OpenAL*>(gAudiop))
    {
        al->setMixTap(nullptr, nullptr);
        al->setSpeakersMuted(false);
    }
#endif
    if (llwebrtc::LLWebRTCDeviceInterface* dev = llwebrtc::getDeviceInterface())
    {
        applyVoice(dev, false, false, false);
    }
    gVoiceDev = nullptr;
    gVoiceTapSet = gVoiceSilenced = gMicSet = false;
}

std::string problem() { return gProblem; }
bool sending() { return gSend.load(); }
bool micInUse() { return gMicOn.load(); }
}
