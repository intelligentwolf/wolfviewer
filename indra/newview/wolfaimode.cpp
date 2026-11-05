/**
 * @file wolfaimode.cpp
 * @brief WolfViewer: AI Mode - the resident's own avatar gets an AI personality that hears and speaks over normal voice.
 *        See wolfaimode.h.
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

#include "wolfaimode.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <set>

#include <boost/json.hpp>

#include "llagent.h"
#include "llaudioengine.h"
#include "llavatarnamecache.h"
#include "llchat.h"
#include "llcoros.h"
#include "llcorehttputil.h"
#include "lldir.h"
#include "llenvironment.h"
#include "llfile.h"
#include "llfloaterreg.h"
#include "llframetimer.h"
#include "llhttpconstants.h"
#include "llnamevalue.h"
#include "llnotificationsutil.h"
#include "llsdjson.h"
#include "llviewerregion.h"
#include "llvoavatarself.h"
#include "llvoicewebrtc.h"
#include "llwebrtc.h"
#include "llworld.h"
#include "message.h"
#include "fsnearbychathub.h"
#include "wolfgrid.h"
#include "wolfregionweather.h"
#include "wolfspeech.h"

// Threads: idle() and everything it calls on the main thread; the two hearing taps on WebRTC's capture and playout threads;
// the speech source on the capture thread (the ADM's or the VR mic clock's). Between them only atomics and single-producer /
// single-consumer rings (wolfvraudio.cpp's SpscRing pattern).

namespace
{
    // Source: region-terrain-work/ai-mode-2026-11-05/README.md "Contract" - php/aimode.php on the WolfStorm web host every
    // WolfViewer call to a wolfstorm/php service uses (wolffloateranimator.cpp SERVICE_URL, the same host).
    const char* const SERVICE_URL = "https://wolfstorm.app/php/aimode.php";
    const char* const CHAT_PREFIX = "(AI) ";
    constexpr U32 kSttRate = WolfSpeech::TARGET_RATE;   // 16 kHz, what whisper wants (wolfspeech.h)
    constexpr U32 kSpeechRate = 48000;                  // the speech ring; the ADM's usual rate (llwebrtc.cpp micClock kRate)
    constexpr S32 kBlock = 1600;                        // 100 ms at 16 kHz: the unit the voice decision is made on
    constexpr S32 kPreRollBlocks = 3;                   // 300 ms kept from before the onset of speech
    constexpr F32 kOpenLead = 0.15f;                    // the voice path opened and the gain ramped (llwebrtc.cpp Process:
                                                        // 30 ms hold + 20 ms ramp) before his first word
    constexpr F32 kHoldTail = 0.6f;                     // the mic stays held after he finishes, past the 40 ms mute fade

    // Source: wolfvraudio.cpp SpscRing - one producer thread, one consumer thread; counts only grow.
    template <typename T> class SpscRing
    {
    public:
        explicit SpscRing(size_t capacity) : mBuf(capacity) {}
        size_t size() const { return mWrite.load(std::memory_order_acquire) - mRead.load(std::memory_order_acquire); }
        size_t push(const T* p, size_t n)
        {
            const size_t w = mWrite.load(std::memory_order_relaxed);
            const size_t room = mBuf.size() - (w - mRead.load(std::memory_order_acquire));
            n = std::min(n, room);
            for (size_t k = 0; k < n; ++k) mBuf[(w + k) % mBuf.size()] = p[k];
            mWrite.store(w + n, std::memory_order_release);
            return n;
        }
        size_t pop(T* p, size_t n)
        {
            const size_t r = mRead.load(std::memory_order_relaxed);
            n = std::min(n, mWrite.load(std::memory_order_acquire) - r);
            for (size_t k = 0; k < n; ++k) p[k] = mBuf[(r + k) % mBuf.size()];
            mRead.store(r + n, std::memory_order_release);
            return n;
        }
        size_t written() const { return mWrite.load(std::memory_order_acquire); }   // producer's count so far
        size_t taken() const { return mRead.load(std::memory_order_relaxed); }      // consumer's count so far
        void skip(size_t n)
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

    // One hearing tap: whatever rate and channels the device gives, down to 16 kHz mono (channel average, then a box
    // average over each output sample's span of input - enough for speech recognition) into a ring the main thread reads.
    struct HearTap : public llwebrtc::LLWebRTCPlayoutTap
    {
        SpscRing<S16> mRing{kSttRate * 8};   // 8 s: the main thread reads every frame
        std::atomic<bool> mOn{false};
        F64 mSum = 0.0;
        S32 mCount = 0;
        F64 mPhase = 0.0;
        void onPlayoutAudio(const int16_t* samples, size_t frames, size_t channels, uint32_t sample_rate) override
        {
            if (!mOn.load(std::memory_order_acquire) || channels == 0 || sample_rate < kSttRate) return;
            const F64 step = (F64)sample_rate / (F64)kSttRate;
            S16 out[1024];
            size_t n = 0;
            for (size_t f = 0; f < frames; ++f)
            {
                S32 m = 0;
                for (size_t c = 0; c < channels; ++c) m += samples[f * channels + c];
                mSum += (F64)m / (F64)channels;
                ++mCount;
                mPhase += 1.0;
                if (mPhase >= step)
                {
                    mPhase -= step;
                    out[n++] = (S16)llclamp((S32)std::lround(mSum / mCount), -32768, 32767);
                    mSum = 0.0;
                    mCount = 0;
                    if (n == sizeof(out) / sizeof(out[0])) { mRing.push(out, n); n = 0; }
                }
            }
            if (n) mRing.push(out, n);   // a full ring drops the newest: the main thread is stalled, nothing to hear then
        }
    };
    HearTap sMicTap, sVoiceTap;

    // The AI's voice into the resident's outgoing voice (llwebrtc.h LLWebRTCSpeechSource): the main thread pushes 48 kHz mono,
    // the capture thread reads it at the device's rate (linear interpolation) into every channel. While mHold is set the
    // microphone is held: speak() returns true, silence where there is no voice left.
    struct Speech : public llwebrtc::LLWebRTCSpeechSource
    {
        SpscRing<S16> mRing{kSpeechRate * 40};   // 40 s: the longest reply (300 characters) is ~20 s of speech
        std::atomic<bool> mHold{false};
        // the producer's count up to which what is in the ring is stale (AI Mode switched off mid-line): the consumer skips it.
        // A count, not a flag, so a new line pushed before the consumer next runs is never thrown away with the old one.
        std::atomic<size_t> mStaleTo{0};
        void flush() { mStaleTo.store(mRing.written(), std::memory_order_release); }
        F64 mT = 1.0;
        F32 mCur = 0.f, mNext = 0.f;
        bool speak(int16_t* out, size_t frames, size_t channels, uint32_t sample_rate) override
        {
            const size_t stale = mStaleTo.load(std::memory_order_acquire);
            if (mRing.taken() < stale)
            {
                mRing.skip(stale - mRing.taken());
                mT = 1.0; mCur = mNext = 0.f;
            }
            if (!mHold.load(std::memory_order_acquire)) return false;
            if (sample_rate == 0 || channels == 0) return true;
            const F64 step = (F64)kSpeechRate / (F64)sample_rate;
            for (size_t f = 0; f < frames; ++f)
            {
                while (mT >= 1.0)
                {
                    mCur = mNext;
                    S16 v = 0;
                    mNext = mRing.pop(&v, 1) == 1 ? (F32)v : 0.f;
                    mT -= 1.0;
                }
                const S16 s = (S16)llclamp((S32)std::lround(mCur + (mNext - mCur) * (F32)mT), -32768, 32767);
                for (size_t c = 0; c < channels; ++c) out[f * channels + c] = s;
                mT += step;
            }
            return true;
        }
    };
    Speech sSpeech;

    LLWebRTCVoiceClient* voiceClient()
    {
        return LLWebRTCVoiceClient::instanceExists() ? LLWebRTCVoiceClient::getInstance() : nullptr;
    }

    bool voiceUp()
    {
        LLWebRTCVoiceClient* v = voiceClient();
        return v && v->isVoiceWorking() && llwebrtc::getDeviceInterface();
    }

    void tell(const std::string& text)
    {
        LLSD args;
        args["MESSAGE"] = text;
        LLNotificationsUtil::add("SystemMessageTip", args);
    }

    void alert(const std::string& text)
    {
        LLSD args;
        args["MESSAGE"] = text;
        LLNotificationsUtil::add("GenericAlertOK", args);
    }

    // A WAV (RIFF, PCM 16-bit, any channels and rate) to 48 kHz mono. Source: the RIFF/WAVE layout WolfSpeech::encodeWav writes
    // (wolfspeech.cpp: "RIFF", "WAVE", "fmt " chunk of 16, "data") - read chunk by chunk, so piper's own header is taken as it is.
    bool wavTo48k(const std::vector<U8>& wav, std::vector<S16>& out, F32& seconds)
    {
        auto u16 = [&wav](size_t o) { return (U32)wav[o] | ((U32)wav[o + 1] << 8); };
        auto u32 = [&wav](size_t o) { return (U32)wav[o] | ((U32)wav[o + 1] << 8) | ((U32)wav[o + 2] << 16) | ((U32)wav[o + 3] << 24); };
        if (wav.size() < 44 || memcmp(&wav[0], "RIFF", 4) || memcmp(&wav[8], "WAVE", 4)) return false;
        U32 channels = 0, rate = 0, bits = 0, format = 0;
        size_t data = 0, data_len = 0;
        for (size_t o = 12; o + 8 <= wav.size();)
        {
            const U32 len = u32(o + 4);
            if (!memcmp(&wav[o], "fmt ", 4) && o + 8 + 16 <= wav.size())
            {
                format = u16(o + 8); channels = u16(o + 10); rate = u32(o + 12); bits = u16(o + 22);
            }
            else if (!memcmp(&wav[o], "data", 4))
            {
                data = o + 8;
                data_len = std::min((size_t)len, wav.size() - data);
                break;
            }
            o += 8 + len + (len & 1);
        }
        if (format != 1 || bits != 16 || channels == 0 || rate == 0 || data == 0) return false;
        const size_t frames = data_len / (2 * channels);
        std::vector<F32> mono(frames);
        for (size_t f = 0; f < frames; ++f)
        {
            S32 m = 0;
            for (U32 c = 0; c < channels; ++c) m += (S16)u16(data + (f * channels + c) * 2);
            mono[f] = (F32)m / (F32)channels;
        }
        const size_t n = (size_t)((F64)frames * kSpeechRate / rate);
        out.resize(n);
        for (size_t i = 0; i < n; ++i)
        {
            const F64 p = (F64)i * rate / kSpeechRate;
            const size_t a = (size_t)p;
            const F32 t = (F32)(p - a);
            const F32 x0 = mono[std::min(a, frames - 1)], x1 = mono[std::min(a + 1, frames - 1)];
            out[i] = (S16)llclamp((S32)std::lround(x0 + (x1 - x0) * t), -32768, 32767);
        }
        seconds = (F32)frames / (F32)rate;
        return frames > 0;
    }

    void fetchClipCoro(std::string url, std::function<void(bool, std::vector<U8>)> done)
    {
        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
            std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfAIMode clip", LLCore::HttpRequest::DEFAULT_POLICY_ID);
        LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
        // Source: wolfadventure.cpp clip fetch (:826-848) - the same request shape for the brain's signed media URL
        LLCore::HttpOptions::ptr_t options = WolfGrid::makeVerifiedHttpOptions();
        options->setTimeout(20);
        options->setRetries(0);
        LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
        headers->append(HTTP_OUT_HEADER_ACCEPT, "audio/wav");
        LLSD result = adapter->getRawAndSuspend(request, url, options, headers);
        LLCore::HttpStatus st = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
        std::vector<U8> wav;
        if (st && result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
        {
            const LLSD::Binary& b = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
            wav.assign(b.begin(), b.end());
        }
        if (!st || wav.size() <= 44)
        {
            LL_WARNS("WolfAIMode") << "voice clip: " << st.toString() << ", " << wav.size() << " bytes" << LL_ENDL;
        }
        const bool got = st && wav.size() > 44;   // before the move: argument evaluation order is unspecified
        done(got, std::move(wav));
    }

    void sttCoro(std::vector<U8> wav, std::function<void(bool, S32, std::string)> done)
    {
        // Source: wolfspeech.cpp WolfSpeech::sttCoro - POST audio/wav to /stt; reply {text} or {error}.
        LLSD::Binary reply;
        std::string error;
        const S32 status = WolfSpeech::postSpeechApi("/stt", "audio/wav", wav, reply, error);
        if (status >= 200 && status < 300)
        {
            const std::string err = WolfSpeech::replyField(reply, "error");
            if (err.empty()) done(true, status, WolfSpeech::replyField(reply, "text"));
            else done(false, status, err);
            return;
        }
        std::string msg = WolfSpeech::replyField(reply, "error");
        if (msg.empty()) msg = "the speech service answered HTTP " + std::to_string(status) + (error.empty() ? "" : " (" + error + ")");
        done(false, status, msg);
    }
}

// One ear: 16 kHz mono from a tap, cut into utterances by WolfSpeech's detector (the same speech level, idle-level estimate,
// hangover and length limits - wolfspeech.cpp processBlock), timed by the samples themselves.
struct WolfAIMode::Ear
{
    std::vector<S16> mPending, mUtterance;
    std::vector<S16> mPreRoll;      // the last kPreRollBlocks of quiet before speech starts
    bool mSpeaking = false;
    S32  mQuietSamples = 0;
    F32  mNoiseFloor = 0.f;
    std::map<LLUUID, S32> mTally;   // voice: who was speaking during the utterance, counted per block
    F64  mLastDataAt = 0.0;         // the tap stops when the resident mutes, or voice drops: an utterance then ends
    size_t mStatSamples = 0;        // the 30 s summary line
    S32  mStatPeak = 0;
};

namespace
{
    WolfAIMode::Ear* sMicEar = nullptr;
    WolfAIMode::Ear* sVoiceEar = nullptr;
}

WolfAIMode::WolfAIMode()
{
    sMicEar = new Ear();
    sVoiceEar = new Ear();
}

WolfAIMode::~WolfAIMode()
{
    // the voice engine's hooks were taken off in shutdown() (before voice terminates); at static destruction it may be gone
    for (const auto& d : mToDelete) LLFile::remove(d.first);
    delete sMicEar;
    delete sVoiceEar;
    sMicEar = sVoiceEar = nullptr;
}

// static
bool WolfAIMode::isAvailable()
{
    return WolfGrid::isOnWolfTerritories() && isAgentAvatarValid();
}

std::string WolfAIMode::myFirstName() const
{
    LLAvatarName av;
    std::string full = LLAvatarNameCache::get(gAgentID, &av) ? av.getLegacyName() : std::string();
    LLStringUtil::trim(full);
    const size_t sp = full.find(' ');
    return sp == std::string::npos ? full : full.substr(0, sp);
}

void WolfAIMode::toggle()
{
    if (mOn)
    {
        turnOff();
        tell("AI Mode is off.");
        return;
    }
    if (!isAvailable())
    {
        alert("AI Mode works on Wolf Territories regions only.");
        return;
    }
    if (mOpening) return;
    mOpening = true;
    api("open", LLSD::emptyMap(), [this](bool ok, const LLSD& r, const std::string& error, const std::string&)
    {
        mOpening = false;
        if (!ok) { alert("AI Mode could not start: " + error); return; }
        if (!r["allowed"].asBoolean()) { alert(r["reason"].asString()); return; }
        if (!r["setup"].isMap())
        {
            LLFloaterReg::showInstance("wolf_aimode", LLSD().with("turn_on", true));   // the first time: personality and voice
            return;
        }
        turnOn();
    });
}

void WolfAIMode::turnOnAfterSetup()
{
    if (!mOn && isAvailable()) turnOn();
}

void WolfAIMode::turnOn()
{
    mOn = true;
    ++mGen;
    mFirst = myFirstName();
    mHeard.clear();
    mHistory.clear();
    mReplyTimes.clear();
    mUnaddressedSince = 0;
    mLastReplyAt = mLastChimeAt = -1e9;
    mLastPartner.setNull();
    *sMicEar = Ear();
    *sVoiceEar = Ear();
    mTagCheckAt = 0.0;   // the tag at once
    mHookRefresh = 0.0;
    LL_INFOS("WolfAIMode") << "on (" << mFirst << ")" << LL_ENDL;
    tell("AI Mode is on: " + mFirst + " listens to you, to voice and to local chat, and talks back. Each time " + mFirst +
         " speaks it uses a little of your Wolf Credits. Turn it off in Wolf World > AI Mode.");
}

void WolfAIMode::turnOff(const std::string& why)
{
    if (!mOn) return;
    mOn = false;
    ++mGen;   // answers still on their way are dropped
    hookVoice(false);
    if (mSay != Say::IDLE)
    {
        if (LLWebRTCVoiceClient* v = voiceClient()) v->wolfSetSpeechOpen(false);
        mVoiceOpened = false;
        mSay = Say::IDLE;
    }
    sSpeech.mHold = false;
    sSpeech.flush();
    mHeard.clear();
    mSayPcm.clear();
    mSayWav.clear();
    sendTag(false);
    mTagCheckAt = LLFrameTimer::getElapsedSeconds() + 10.0;
    LL_INFOS("WolfAIMode") << "off" << (why.empty() ? std::string() : ": " + why) << LL_ENDL;
    if (!why.empty()) alert(why);
}

void WolfAIMode::shutdown()
{
    if (mOn)
    {
        mOn = false;
        ++mGen;
    }
    hookVoice(false);
    if (LLWebRTCVoiceClient* v = voiceClient()) v->wolfSetSpeechOpen(false);
}

// The taps and the speech source on the voice engine's device (re-applied every second: the device module is made again when
// voice reconnects - wolfvraudio.cpp pump does the same).
void WolfAIMode::hookVoice(bool on)
{
    llwebrtc::LLWebRTCDeviceInterface* dev = llwebrtc::getDeviceInterface();
    sMicTap.mOn = on;
    sVoiceTap.mOn = on;
    if (dev)
    {
        dev->setCaptureTap(on ? &sMicTap : nullptr);
        dev->setListenTap(on ? &sVoiceTap : nullptr);
        dev->setSpeechSource(on ? &sSpeech : nullptr);
        if (!on) dev->setSpeechGain(false);
    }
    mHookedDev = on ? (void*)dev : nullptr;
}

// The region plugin wolf-aitag (region-terrain-work/ai-mode-2026-11-05/aitag) sets / clears the avatar's name-tag title.
// Sent on a negative channel (fsnearbychathub.cpp really_send_chat_from_viewer: ScriptDialogReply, ObjectID = the agent),
// which LLClientView turns into chat from this avatar on that channel (LLClientView.cs HandlerScriptDialogReply :9081-9095).
void WolfAIMode::sendTag(bool on)
{
    if (!WolfGrid::isOnWolfTerritoriesConfirmed() || !gAgent.getRegion()) return;
    LLMessageSystem* msg = gMessageSystem;
    if (!msg) return;
    msg->newMessage("ScriptDialogReply");
    msg->nextBlock("AgentData");
    msg->addUUID("AgentID", gAgentID);
    msg->addUUID("SessionID", gAgentSessionID);
    msg->nextBlock("Data");
    msg->addUUID("ObjectID", gAgentID);
    msg->addS32("ChatChannel", TAG_CHANNEL);
    msg->addS32("ButtonIndex", 0);
    msg->addString("ButtonLabel", on ? "wolf.aimode on" : "wolf.aimode off");
    gAgent.sendReliableMessage();
    LL_DEBUGS("WolfAIMode") << "tag " << (on ? "on" : "off") << " asked of " << gAgent.getRegion()->getName() << LL_ENDL;
}

void WolfAIMode::idle()
{
    // collect the files the resident's own playback used (30 s after the end, as WolfSpeech's mPlayed)
    const F64 now = LLFrameTimer::getElapsedSeconds();
    for (auto it = mToDelete.begin(); it != mToDelete.end();)
    {
        if (now >= it->second.mAt)
        {
            if (gAudiop) gAudiop->removeAudioData(it->second.mId);   // the engine's entry with the file (WolfSpeech mPlayed)
            LLFile::remove(it->first);
            it = mToDelete.erase(it);
        }
        else ++it;
    }
    // The tag follows the avatar's own name-tag title (the region sends it to every viewer, this one too): AI Mode on and
    // the title is not the AI Mode one (a new region, or the region's own group title came in after us at arrival -
    // ScenePresence.cs:2380 CompleteMovement sets it), or off and it still is: tell the region again, every 10 s at most.
    if (now >= mTagCheckAt && WolfGrid::isOnWolfTerritoriesConfirmed() && isAgentAvatarValid())
    {
        mTagCheckAt = now + 10.0;
        LLNameValue* title = gAgentAvatarp->getNVPair("Title");
        const bool tagged = title && title->getString() && std::string(title->getString()) == TAG_TITLE;
        if (mOn != tagged) sendTag(mOn);
    }
    if (!mOn) return;
    // [WOLF GRID GATE] off Wolf (a hypergrid jump) everything stops; the resident turns it on again on Wolf
    if (!WolfGrid::isOnWolfTerritories() || !isAgentAvatarValid())
    {
        turnOff();
        tell("AI Mode is off: it works on Wolf Territories only.");
        return;
    }
    llwebrtc::LLWebRTCDeviceInterface* dev = llwebrtc::getDeviceInterface();
    if (now >= mHookRefresh || (void*)dev != mHookedDev)
    {
        hookVoice(true);
        mHookRefresh = now + 1.0;
    }
    pumpEars();
    // one summary line every 30 s while on (what each ear received), never per frame
    if (now >= mStatAt)
    {
        mStatAt = now + 30.0;
        LL_INFOS("WolfAIMode") << "ears: mic " << sMicEar->mStatSamples << " samples (peak " << sMicEar->mStatPeak << "), voices "
                               << sVoiceEar->mStatSamples << " samples (peak " << sVoiceEar->mStatPeak << "), voice "
                               << (voiceUp() ? "up" : "not connected") << LL_ENDL;
        sMicEar->mStatSamples = sVoiceEar->mStatSamples = 0;
        sMicEar->mStatPeak = sVoiceEar->mStatPeak = 0;
    }
    maybeAsk();
    pumpSpeaking();
}

void WolfAIMode::pumpEars()
{
    S16 buf[4096];
    for (int which = 0; which < 2; ++which)
    {
        HearTap& tap = which == 0 ? sMicTap : sVoiceTap;
        Ear& ear = which == 0 ? *sMicEar : *sVoiceEar;
        size_t n;
        bool got = false;
        while ((n = tap.mRing.pop(buf, sizeof(buf) / sizeof(buf[0]))) > 0)
        {
            ear.mPending.insert(ear.mPending.end(), buf, buf + n);
            got = true;
            ear.mStatSamples += n;
            for (size_t i = 0; i < n; ++i) ear.mStatPeak = llmax(ear.mStatPeak, (S32)std::abs((S32)buf[i]));
        }
        pumpEar(ear, which == 0, got);
    }
}

void WolfAIMode::pumpEar(Ear& ear, bool mic, bool got)
{
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (got) ear.mLastDataAt = now;
    else if (ear.mSpeaking && now - ear.mLastDataAt > 0.5)
    {
        endUtterance(ear, mic);   // no sound for half a second (muted, voice gone): what was said so far is the utterance
        ear.mPending.clear();
        return;
    }
    // While he talks the microphone is held (nothing comes) and voices played are others; his own speech is never heard.
    size_t at = 0;
    while (ear.mPending.size() - at >= (size_t)kBlock)
    {
        const S16* s = ear.mPending.data() + at;
        at += kBlock;
        F64 sum = 0.0;
        for (S32 i = 0; i < kBlock; ++i) { const F64 x = s[i] / 32768.0; sum += x * x; }
        const F32 rms = (F32)sqrt(sum / (F64)kBlock);
        // Source: wolfspeech.cpp processBlock - the idle-level estimate and threshold
        if (ear.mNoiseFloor <= 0.f || rms < ear.mNoiseFloor) ear.mNoiseFloor = rms;
        else ear.mNoiseFloor += (rms - ear.mNoiseFloor) * WolfSpeech::NOISE_FLOOR_RISE;
        const F32 threshold = llmax(WolfSpeech::SPEECH_RMS_MIN, ear.mNoiseFloor * WolfSpeech::NOISE_RATIO);
        if (rms >= threshold)
        {
            if (!ear.mSpeaking)
            {
                // the start of a word is quieter than its middle: the blocks just before the onset go first, so a name said
                // first ("Lone, ...") is not clipped (2026-11-05 test: "Shadow, can you hear me" came back without "Shadow")
                ear.mUtterance.assign(ear.mPreRoll.begin(), ear.mPreRoll.end());
            }
            ear.mSpeaking = true;
            ear.mQuietSamples = 0;
        }
        else if (ear.mSpeaking) ear.mQuietSamples += kBlock;
        if (!ear.mSpeaking)
        {
            ear.mPreRoll.insert(ear.mPreRoll.end(), s, s + kBlock);
            if (ear.mPreRoll.size() > (size_t)(kPreRollBlocks * kBlock)) ear.mPreRoll.erase(ear.mPreRoll.begin(), ear.mPreRoll.begin() + kBlock);
            continue;
        }
        ear.mPreRoll.clear();
        ear.mUtterance.insert(ear.mUtterance.end(), s, s + kBlock);
        if (!mic)
        {
            // who is speaking now (the voice server's own speaking flags, llvoicewebrtc.cpp :2334 / :3907)
            if (LLWebRTCVoiceClient* v = voiceClient())
            {
                std::set<LLUUID> who;
                v->getParticipantList(who);
                for (const LLUUID& id : who) if (id != gAgentID && v->getIsSpeaking(id)) ++ear.mTally[id];
            }
        }
        const F32 duration = (F32)ear.mUtterance.size() / (F32)kSttRate;
        const F32 silence = (F32)ear.mQuietSamples / (F32)kSttRate;
        if (silence >= WolfSpeech::SILENCE_HANGOVER_SECS || duration >= WolfSpeech::MAX_UTTERANCE_SECS) endUtterance(ear, mic);
    }
    ear.mPending.erase(ear.mPending.begin(), ear.mPending.begin() + at);
}

void WolfAIMode::endUtterance(Ear& ear, bool mic)
{
    const F32 duration = (F32)ear.mUtterance.size() / (F32)kSttRate;
    if (duration >= WolfSpeech::MIN_UTTERANCE_SECS && mSttInflight < WolfSpeech::MAX_INFLIGHT)
    {
        LLUUID who = mic ? gAgentID : LLUUID::null;
        S32 best = 0;
        for (const auto& t : ear.mTally) if (t.second > best) { best = t.second; who = t.first; }
        ++mSttInflight;
        const U32 gen = mGen;
        const std::string via = mic ? "mic" : "voice";
        std::vector<U8> wav = WolfSpeech::wav16k(ear.mUtterance);
        LL_INFOS("WolfAIMode") << via << ": " << duration << " s utterance to /stt" << LL_ENDL;
        LLCoros::instance().launch("WolfAIMode stt", [this, wav, gen, who, via]()
        {
            sttCoro(wav, [this, gen, who, via](bool ok, S32 status, std::string text)
            {
                if (mSttInflight > 0) --mSttInflight;
                if (gen != mGen || !mOn) return;
                if (!ok) { LL_WARNS("WolfAIMode") << "stt HTTP " << status << ": " << text << LL_ENDL; return; }
                Line l;
                l.mWho = who;
                l.mVia = via;
                l.mText = text;
                heard(l);
            });
        });
    }
    ear.mUtterance.clear();
    ear.mSpeaking = false;
    ear.mQuietSamples = 0;
    ear.mTally.clear();
}

// whisper's words for no speech, and noise it writes as words (whisper.cpp prints these for silence / music)
// static
std::string WolfAIMode::cleanHeard(const std::string& text)
{
    std::string t = text;
    LLStringUtil::trim(t);
    // whole-line markers such as "[BLANK_AUDIO]", "(music)", "*laughs*"
    if (t.size() >= 2 && ((t.front() == '[' && t.back() == ']') || (t.front() == '(' && t.back() == ')') || (t.front() == '*' && t.back() == '*')))
        return std::string();
    std::string low = t;
    LLStringUtil::toLower(low);
    static const char* const kNoise[] = { "you", "you.", "thank you.", "thanks for watching!", "thank you for watching.", ".", "..." };
    for (const char* n : kNoise) if (low == n) return std::string();
    return t.size() < 2 ? std::string() : t;
}

void WolfAIMode::onNearbyChat(const LLChat& chat)
{
    if (!mOn || chat.mSourceType != CHAT_SOURCE_AGENT || chat.mChatType == CHAT_TYPE_START || chat.mChatType == CHAT_TYPE_STOP) return;
    // his own lines come back as the resident's chat: never heard again
    if (chat.mFromID == gAgentID && chat.mText.compare(0, strlen(CHAT_PREFIX), CHAT_PREFIX) == 0) return;
    // another AI Mode avatar's "(AI)" line is also spoken over voice, which he hears: the chat copy would be heard twice
    if (chat.mFromID != gAgentID && chat.mText.compare(0, strlen(CHAT_PREFIX), CHAT_PREFIX) == 0 && voiceUp()) return;
    LL_INFOS("WolfAIMode") << "chat from " << chat.mFromName << LL_ENDL;
    Line l;
    l.mWho = chat.mFromID;
    l.mName = chat.mFromName;
    l.mText = chat.mText;
    l.mVia = "chat";
    heard(l);
}

namespace
{
    // A word's consonant sound, for names speech recognition writes as a sound-alike ("Shadow" heard as "Chateau"): lower
    // case letters only; sh / ch / sch -> x, ph -> f, ck / c / q -> k, z -> s, d -> t; vowels and w, h, y dropped after the
    // first letter; doubles collapsed. "" when the word starts with a vowel ("alone" is never "Lone").
    std::string consonantSound(const std::string& word)
    {
        std::string w;
        for (char c : word) if (isalpha((unsigned char)c)) w += (char)tolower((unsigned char)c);
        if (w.empty() || strchr("aeiou", w[0])) return std::string();
        std::string out;
        for (size_t i = 0; i < w.size(); ++i)
        {
            char c = w[i];
            if ((c == 's' || c == 'c') && i + 1 < w.size() && w[i + 1] == 'h') { c = 'x'; ++i; }
            else if (c == 'p' && i + 1 < w.size() && w[i + 1] == 'h') { c = 'f'; ++i; }
            else if (c == 'c' || c == 'q') c = 'k';
            else if (c == 'z') c = 's';
            else if (c == 'd') c = 't';
            if (i > 0 && strchr("aeiouwhy", c)) continue;
            if (!out.empty() && out.back() == c) continue;
            out += c;
        }
        return out;
    }

    /** Is the name said TO someone in this line - the first word with a comma or stop after it, "hey / hi / hello / oi
     *  <name>", or ", <name>" closing it - as written, or (speech only) as a sound-alike of it? */
    bool namedInLine(const std::string& text, const std::string& name, bool sound_alike)
    {
        std::vector<std::string> words;
        std::vector<char> after;   // the character after each word ('\0' at the end)
        std::string cur;
        for (size_t i = 0; i <= text.size(); ++i)
        {
            const char c = i < text.size() ? text[i] : '\0';
            if (isalnum((unsigned char)c) || c == '\'') { cur += c; continue; }
            if (!cur.empty()) { words.push_back(cur); after.push_back(c); cur.clear(); }
            else if (!after.empty() && c != ' ' && after.back() == ' ') after.back() = c;
        }
        std::string low_name = name;
        LLStringUtil::toLower(low_name);
        const std::string name_sound = consonantSound(name);
        auto same = [&](const std::string& w, bool loose)
        {
            std::string lw = w;
            LLStringUtil::toLower(lw);
            if (lw == low_name) return true;
            return loose && name_sound.size() >= 2 && consonantSound(w) == name_sound;
        };
        for (size_t i = 0; i < words.size(); ++i)
        {
            if (same(words[i], false)) return true;   // the name itself, anywhere
            if (!sound_alike) continue;
            std::string prev = i > 0 ? words[i - 1] : std::string();
            LLStringUtil::toLower(prev);
            const bool vocative = (i == 0 && after[i] != ' ' && after[i] != '\0')
                                  || prev == "hey" || prev == "hi" || prev == "hello" || prev == "oi"
                                  || (i + 1 == words.size() && i > 0 && after[i - 1] == ',');
            if (vocative && same(words[i], true)) return true;
        }
        return false;
    }
}

bool WolfAIMode::addressedToMe(const Line& l) const
{
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (!mFirst.empty() && namedInLine(l.mText, mFirst, l.mVia != "chat")) return true;
    if (l.mWho == gAgentID)
    {
        // the resident: talking with him already, or nobody else within earshot
        if (now - mLastReplyAt < CONVERSATION_SECS) return true;
        uuid_vec_t ids;
        LLWorld::getInstance()->getAvatars(&ids, nullptr, gAgent.getPositionGlobal(), EARSHOT_M);
        for (const LLUUID& id : ids) if (id != gAgentID) return false;
        return true;
    }
    return l.mWho.notNull() && l.mWho == mLastPartner && now - mLastReplyAt < FOLLOW_ON_SECS;
}

void WolfAIMode::heard(Line l)
{
    l.mText = cleanHeard(l.mText);
    if (l.mText.empty()) return;
    l.mAt = LLFrameTimer::getElapsedSeconds();
    if (l.mName.empty() && l.mWho.notNull())
    {
        LLAvatarName av;
        if (LLAvatarNameCache::get(l.mWho, &av)) l.mName = av.getLegacyName();
    }
    l.mAddressed = addressedToMe(l);
    LL_INFOS("WolfAIMode") << "heard (" << l.mVia << ", " << (l.mName.empty() ? "someone" : l.mName) << (l.mAddressed ? ", to him" : "")
                           << "): " << l.mText << LL_ENDL;
    if (!l.mAddressed) ++mUnaddressedSince;
    mHeard.push_back(l);
    while (mHeard.size() > MAX_HEARD) mHeard.pop_front();
}

bool WolfAIMode::someoneSpeaking() const
{
    if (sMicEar->mSpeaking || sVoiceEar->mSpeaking) return true;
    if (LLWebRTCVoiceClient* v = voiceClient())
    {
        std::set<LLUUID> who;
        v->getParticipantList(who);
        for (const LLUUID& id : who) if (id != gAgentID && v->getIsSpeaking(id)) return true;
    }
    return false;
}

void WolfAIMode::maybeAsk()
{
    if (mAsking || mSay != Say::IDLE || mHeard.empty()) return;
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (now - mLastReplyAt < MIN_GAP_SECS) return;
    while (!mReplyTimes.empty() && now - mReplyTimes.front() > 60.0) mReplyTimes.pop_front();
    if ((S32)mReplyTimes.size() >= MAX_PER_MINUTE) return;
    bool addressed = false;
    for (const Line& l : mHeard) addressed = addressed || l.mAddressed;
    if (addressed)
    {
        ask(true);
        return;
    }
    // joining in: rarely, after a natural gap in someone else's talk
    if (mUnaddressedSince >= 2 && now - mLastChimeAt >= CHIME_EVERY_SECS && now - mHeard.back().mAt >= CHIME_GAP_SECS && !someoneSpeaking())
    {
        mLastChimeAt = now;
        ask(false);
    }
}

void WolfAIMode::ask(bool addressed)
{
    LLSD body = LLSD::emptyMap();
    LLSD heard = LLSD::emptyArray();
    LLUUID partner;
    for (const Line& l : mHeard)
    {
        LLSD h;
        h["who"] = l.mWho.notNull() ? l.mWho.asString() : std::string();
        h["name"] = l.mName;
        h["text"] = l.mText;
        h["via"] = l.mVia;
        heard.append(h);
        if (l.mAddressed || partner.isNull()) partner = l.mWho;
        LLSD hist;
        hist["me"] = false;
        hist["name"] = l.mName;
        hist["text"] = l.mText;
        mHistory.push_back(hist);
    }
    LLSD history = LLSD::emptyArray();
    for (size_t i = 0; i + mHeard.size() < mHistory.size(); ++i) history.append(mHistory[i]);   // before these lines
    while (mHistory.size() > MAX_HISTORY) mHistory.pop_front();
    body["heard"] = heard;
    body["history"] = history;
    LLSD nearby = LLSD::emptyArray();
    uuid_vec_t ids;
    LLWorld::getInstance()->getAvatars(&ids, nullptr, gAgent.getPositionGlobal(), EARSHOT_M);
    for (const LLUUID& id : ids)
    {
        if (id == gAgentID) continue;
        LLAvatarName av;
        LLSD n;
        n["who"] = id.asString();
        n["name"] = LLAvatarNameCache::get(id, &av) ? av.getLegacyName() : std::string();
        nearby.append(n);
    }
    body["nearby"] = nearby;
    body["addressed"] = addressed;
    body["night"] = !LLEnvironment::instance().getIsSunUp();
    body["weather"] = WolfRegionWeather::instance().kind();
    body["place"] = gAgent.getRegion() ? gAgent.getRegion()->getName() : std::string();
    body["voice"] = true;   // the clip is also played for the resident when voice is off
    mHeard.clear();
    mUnaddressedSince = 0;
    mAsking = true;
    const U32 gen = mGen;
    mLastPartner = partner == gAgentID ? LLUUID::null : partner;
    api("reply", body, [this, gen](bool ok, const LLSD& r, const std::string& error, const std::string& code)
    {
        onReply(gen, ok, r, error, code);
    });
}

void WolfAIMode::onReply(U32 gen, bool ok, const LLSD& r, const std::string& error, const std::string& code)
{
    mAsking = false;
    if (gen != mGen || !mOn) return;
    if (!ok)
    {
        LL_WARNS("WolfAIMode") << "reply refused (" << code << "): " << error << LL_ENDL;
        if (code == "credits_short")
        {
            turnOff();
            LLSD args;
            args["MESSAGE"] = error;
            LLNotificationsUtil::add("GenericAlertOK", args);
            if (r.has("buyurl")) LL_INFOS("WolfAIMode") << "buy credits: " << r["buyurl"].asString() << LL_ENDL;
        }
        else if (code == "not_open" || code == "no_setup") turnOff(error);
        else if (code != "busy") tell(mFirst + " could not answer: " + error);
        return;
    }
    LL_INFOS("WolfAIMode") << "reply (" << r["cost"]["credits"].asReal() << " credits" << (r["introduced"].asBoolean() ? ", introduced" : "")
                           << "): " << r["say"].asString() << LL_ENDL;
    const std::string say = r["say"].asString();
    if (say.empty()) return;   // he chose to stay quiet
    const std::string url = r["voiceUrl"].asString();
    if (url.empty())
    {
        onVoiceClip(gen, say, std::vector<U8>());
        return;
    }
    LLCoros::instance().launch("WolfAIMode clip", [this, gen, say, url]()
    {
        fetchClipCoro(url, [this, gen, say](bool got, std::vector<U8> wav)
        {
            if (!got) LL_WARNS("WolfAIMode") << "voice clip not fetched: the line goes to chat only" << LL_ENDL;
            onVoiceClip(gen, say, got ? wav : std::vector<U8>());
        });
    });
}

void WolfAIMode::onVoiceClip(U32 gen, const std::string& say, const std::vector<U8>& wav)
{
    if (gen != mGen || !mOn || mSay != Say::IDLE) return;
    mSayText = say;
    mSayPcm.clear();
    mSaySeconds = 0.f;
    mSayWav.clear();
    if (!wav.empty() && wavTo48k(wav, mSayPcm, mSaySeconds)) mSayWav = wav;
    mSay = Say::WAITING;
    mSayReadyAt = LLFrameTimer::getElapsedSeconds();
}

void WolfAIMode::pumpSpeaking()
{
    const F64 now = LLFrameTimer::getElapsedSeconds();
    static F64 s_quiet_since = 0.0;
    if (someoneSpeaking()) s_quiet_since = now;
    switch (mSay)
    {
    case Say::IDLE:
        break;
    case Say::WAITING:
        if (now - s_quiet_since >= QUIET_BEFORE_SPEAKING) startSpeaking();
        else if (now - mSayReadyAt > GIVE_UP_WAITING)
        {
            LL_INFOS("WolfAIMode") << "no quiet moment in " << GIVE_UP_WAITING << " s: line dropped" << LL_ENDL;
            mSay = Say::IDLE;
            mLastReplyAt = now;
        }
        break;
    case Say::OPENING:
        if (now - mSayPhaseAt >= kOpenLead)
        {
            sSpeech.mRing.push(mSayPcm.data(), mSayPcm.size());
            mSay = Say::PLAYING;
            mSayPhaseAt = now;
        }
        break;
    case Say::PLAYING:
        if (mVoiceOpened)
        {
            mServerPeak = llmax(mServerPeak, LLWebRTCVoiceClient::sWolfServerOwnLevel);
            // the capture clock reads the ring only once recording has started (it may start late after an unmute): done
            // when it is all read, never cut short; a stalled device ends it 3 s past the line's length
            if ((sSpeech.mRing.size() == 0 && now - mSayPhaseAt >= mSaySeconds) || now - mSayPhaseAt >= mSaySeconds + 3.f) finishSpeaking();
        }
        else if (now - mSayPhaseAt >= mSaySeconds + 0.25f) finishSpeaking();
        break;
    case Say::CLOSING:
        if (now - mSayPhaseAt >= kHoldTail)
        {
            sSpeech.mHold = false;
            mSay = Say::IDLE;
        }
        break;
    }
}

void WolfAIMode::startSpeaking()
{
    const F64 now = LLFrameTimer::getElapsedSeconds();
    mReplyTimes.push_back(now);
    // the line in local chat, as his: people without voice, or who cannot hear, read it (and it is clearly marked AI)
    FSNearbyChat::instance().sendChatFromViewer(std::string(CHAT_PREFIX) + mSayText, CHAT_TYPE_NORMAL, false);
    LLSD hist;
    hist["me"] = true;
    hist["name"] = mFirst;
    hist["text"] = mSayText;
    mHistory.push_back(hist);
    while (mHistory.size() > MAX_HISTORY) mHistory.pop_front();
    // the resident hears him from the avatar (the sound cache + trigger, as WolfSpeech plays a line - wolfspeech.cpp onSynthesised)
    if (!mSayWav.empty() && gAudiop && isAgentAvatarValid())
    {
        const LLUUID id = LLUUID::generateNewID();
        const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_FS_SOUND_CACHE, id.asString()) + ".dsf";
        llofstream out(path, std::ios::binary);
        if (out.is_open())
        {
            out.write(reinterpret_cast<const char*>(mSayWav.data()), mSayWav.size());
            out.close();
            gAudiop->triggerSound(id, gAgentID, 1.f, LLAudioEngine::AUDIO_TYPE_SFX, gAgentAvatarp->getPositionGlobal());
            mToDelete.push_back({ path, { id, now + mSaySeconds + 30.0 } });
        }
    }
    if (!mSayPcm.empty() && voiceUp())
    {
        // into his own voice: the mic held, the gain stage at unity, the voice path kept open even if the resident is muted
        if (llwebrtc::LLWebRTCDeviceInterface* dev = llwebrtc::getDeviceInterface()) dev->setSpeechGain(true);
        sSpeech.mHold = true;
        if (LLWebRTCVoiceClient* v = voiceClient()) v->wolfSetSpeechOpen(true);
        mVoiceOpened = true;
        mServerPeak = 0.f;
        mSay = Say::OPENING;
        mSayPhaseAt = now;
        LL_INFOS("WolfAIMode") << "speaking over voice (" << mSaySeconds << " s)" << LL_ENDL;
    }
    else
    {
        mVoiceOpened = false;
        mSay = Say::PLAYING;   // chat (and the resident's own playback) only: voice is not connected
        mSayPhaseAt = now;
        LL_INFOS("WolfAIMode") << "speaking in chat only (voice " << (voiceUp() ? "up" : "not connected") << ")" << LL_ENDL;
    }
}

void WolfAIMode::finishSpeaking()
{
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (mVoiceOpened)
    {
        // the evidence his voice went out: the voice server's own measure of this viewer's sound while he spoke
        LL_INFOS("WolfAIMode") << "spoke " << mSaySeconds << " s over voice; the voice server measured our sound at peak "
                               << mServerPeak << " (it reads > 0.30 as speaking, wolfvoice mixer.rs:148)" << LL_ENDL;
        if (sSpeech.mRing.size() > 0) sSpeech.flush();   // a stalled device: what it never read is not said later
        if (llwebrtc::LLWebRTCDeviceInterface* dev = llwebrtc::getDeviceInterface()) dev->setSpeechGain(false);
        if (LLWebRTCVoiceClient* v = voiceClient()) v->wolfSetSpeechOpen(false);
        mVoiceOpened = false;
        mSay = Say::CLOSING;   // the mic stays held a little longer (kHoldTail)
    }
    else
    {
        mSay = Say::IDLE;
    }
    mSayPhaseAt = now;
    mLastReplyAt = now;
    mSayPcm.clear();
    mSayWav.clear();
}

// static
void WolfAIMode::api(const std::string& action, const LLSD& body, api_callback_t done)
{
    // Wolf only, and only once the region underfoot has said it is Wolf (WolfGrid::isOnWolfTerritoriesConfirmed, the predicate
    // for SENDING the session pair - wolfadventure.cpp WolfAdventure::api)
    if (!WolfGrid::isOnWolfTerritoriesConfirmed())
    {
        if (done) done(false, LLSD(), "AI Mode works on Wolf Territories only - or you are between regions; try again in a moment.", "");
        return;
    }
    LLSD req = body.isMap() ? body : LLSD::emptyMap();
    req["action"] = action;
    const std::string text = boost::json::serialize(LlsdToJson(req));
    LLCoros::instance().launch("WolfAIMode " + action, [text, done]()
    {
        // Source: wolfadventure.cpp WolfAdventure::api - the session pair on a host-verified, non-redirecting request, no
        // retry; a refusal's JSON in the raw result (200) or the error body (4xx/5xx, llcorehttputil.cpp httpStatus["error_body"]).
        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
            std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfAIMode", LLCore::HttpRequest::DEFAULT_POLICY_ID);
        LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
        LLCore::HttpOptions::ptr_t options = WolfGrid::makeVerifiedHttpOptions();
        options->setTimeout(40);   // the reply waits for the AI (WM_TALK_TIMEOUT 12 s a provider, two providers) and piper
        options->setRetries(0);
        LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
        headers->append(HTTP_OUT_HEADER_ACCEPT, "application/json");
        headers->append(HTTP_OUT_HEADER_CONTENT_TYPE, "application/json");
        headers->append("X-Wolf-Agent", gAgentID.asString());
        headers->append("X-Wolf-Session", gAgentSessionID.asString());
        LLCore::BufferArray::ptr_t raw(new LLCore::BufferArray());
        raw->append(text.data(), text.size());
        LLSD result = adapter->postRawAndSuspend(request, SERVICE_URL, raw, options, headers);
        LLSD http = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
        LLCore::HttpStatus st = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(http);
        LLSD reply;
        if (result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
        {
            const LLSD::Binary& bytes = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
            boost::system::error_code ec;
            boost::json::value v = boost::json::parse(std::string(bytes.begin(), bytes.end()), ec);
            if (!ec) reply = LlsdFromJson(v);
        }
        if (!reply.isMap() && http.has("error_body"))
        {
            boost::system::error_code ec;
            boost::json::value v = boost::json::parse(http["error_body"].asString(), ec);
            if (!ec && v.is_object()) reply = LlsdFromJson(v);
        }
        const S32 status = st.isHttpStatus() ? (S32)st.getType() : (st ? 200 : 0);
        bool ok = false;
        std::string error, code;
        if (reply.isMap())
        {
            ok = status == 200 && reply["success"].asBoolean();
            error = reply["error"].asString();
            code = reply["code"].asString();
        }
        if (!ok && error.empty())
        {
            error = status == 429 ? "too many requests - wait a moment" : status == 0 ? "the AI Mode service could not be reached"
                  : "the AI Mode service answered HTTP " + std::to_string(status);
        }
        if (!ok && code.empty() && status == 429) code = "busy";
        if (done) done(ok, reply, error, code);
    });
}
