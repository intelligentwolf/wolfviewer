/**
 * @file wolfweathersound.cpp
 * @brief WolfViewer: synthesised weather sound. Mirror of wolfstorm/js/world/weather_sound.js.
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

#include "wolfweathersound.h"

#include <cmath>
#include <cstdio>
#include <vector>

#include "llagent.h"
#include "llaudioengine.h"
#include "lldir.h"
#include "llfile.h"
#include "llrand.h"
#include "llframetimer.h"
#include "llviewercamera.h"

namespace
{
    /// The gap between cracks, in seconds — irregular, and rarer for a distant storm.
    constexpr F64 THUNDER_MIN_SECS = 9.0,  THUNDER_SPAN_SECS = 22.0;
    constexpr F64 DISTANT_MIN_SECS = 22.0, DISTANT_SPAN_SECS = 40.0;

    std::string soundPath(const char* uuid_str)
    {
        // Source: LLAudioEngine::hasDecodedFile (llaudioengine.cpp:999) and LLAudioData::load
        // (:1967) — both build exactly this path, so this must match them or the engine will
        // never find what we wrote.
        return gDirUtilp->getExpandedFilename(LL_PATH_FS_SOUND_CACHE, std::string(uuid_str)) + ".dsf";
    }

    void put_u32(std::vector<U8>& out, U32 v)
    {
        out.push_back((U8)(v & 0xff));         out.push_back((U8)((v >> 8) & 0xff));
        out.push_back((U8)((v >> 16) & 0xff)); out.push_back((U8)((v >> 24) & 0xff));
    }
    void put_u16(std::vector<U8>& out, U16 v)
    {
        out.push_back((U8)(v & 0xff)); out.push_back((U8)((v >> 8) & 0xff));
    }
    void put_tag(std::vector<U8>& out, const char* tag)
    {
        for (int i = 0; i < 4; ++i) out.push_back((U8)tag[i]);
    }
}

// The tables live here rather than in the header so the UUID strings have one home.
namespace
{
    const WolfWeatherSound::Voice AMBIENCE[] = {
        // id         uuid                                     rate  lowHz  highHz  gain
        { "thunder",  "7df2a520-726d-4a5e-852c-a96f533e634c",  0.70f,   0.f,  900.f, 0.55f },
        { "steady",   "2ddb90be-a340-4a1a-a5b6-594d42e9c188",  1.00f,   0.f, 2200.f, 0.50f },
        { "wind",     "62951de9-587b-4b4b-9432-088993359658",  0.45f, 160.f,  700.f, 0.60f },
        { "distant",  "f0ae0a15-728f-4dd6-b6e0-c0cc8a41e0e2",  0.60f,   0.f,  550.f, 0.35f },
        { nullptr,    nullptr,                                 0.f,     0.f,    0.f, 0.f   },
    };
    const WolfWeatherSound::Voice NEAR[] = {
        { "soft",     "e09ec915-87aa-46fc-aaa3-36ee9ce6b9ce",  1.40f, 1800.f,    0.f, 0.30f },
        { "heavy",    "89a39f6f-e713-4baa-a49d-491bedc7e4fc",  1.80f, 1200.f,    0.f, 0.50f },
        { "leaves",   "1cca6560-7abc-40c2-803b-67ed63f7e6ab",  2.20f, 2200.f, 4600.f, 0.35f },
        { "water",    "8c588682-91e7-4956-8e4a-c7a26beb5d9d",  1.10f,  500.f, 1400.f, 0.40f },
        { nullptr,    nullptr,                                 0.f,     0.f,    0.f, 0.f   },
    };
    const WolfWeatherSound::Voice CRACK_NEAR =
        { "crack",    "3419684f-4b78-47f2-a20b-c0f8060531a3",  0.35f,   0.f,  700.f, 0.90f };
    const WolfWeatherSound::Voice CRACK_FAR =
        { "crackfar", "5012bda1-e308-4cd8-8423-4c83ef8b8987",  0.30f,   0.f,  220.f, 0.45f };
}

WolfWeatherSound::WolfWeatherSound() {}
WolfWeatherSound::~WolfWeatherSound() {}

// ═══════════════════════════════════════════════════════════════════════════════════════════
// the voice tables
// ═══════════════════════════════════════════════════════════════════════════════════════════

// static
const WolfWeatherSound::Voice* WolfWeatherSound::findVoice(const Voice* table,
                                                           const std::string& id)
{
    if (id.empty() || id == "none") return nullptr;
    for (const Voice* v = table; v->mId; ++v)
    {
        if (id == v->mId) return v;
    }
    return nullptr;
}

// static
const WolfWeatherSound::Voice* WolfWeatherSound::ambience(const std::string& id)
{
    return findVoice(AMBIENCE, id);
}

// static
const WolfWeatherSound::Voice* WolfWeatherSound::nearLayer(const std::string& id)
{
    return findVoice(NEAR, id);
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
// generating the sounds
// ═══════════════════════════════════════════════════════════════════════════════════════════

// static
bool WolfWeatherSound::ensureSound(const Voice& v)
{
    const std::string path = soundPath(v.mUuid);
    if (gDirUtilp->fileExists(path)) return true;
    const bool one_shot = (v.mId && (std::string(v.mId) == "crack" || std::string(v.mId) == "crackfar"));
    return writeWav(path, v, one_shot);
}

/**
 * Write one layer as a 16-bit mono WAV.
 *
 * The signal is brown-ish noise (white noise integrated a little, which is a hush rather than a
 * hiss) put through a one-pole low-pass and/or high-pass at the voice's band edges. A loop gets
 * its tail cross-faded into its head so the seam cannot be heard; a one-shot (thunder) gets a
 * fast attack and a long exponential decay instead, because a fading-IN rumble sounds like a
 * passing lorry rather than a strike.
 */
// static
bool WolfWeatherSound::writeWav(const std::string& path, const Voice& v, bool oneShot)
{
    const S32 rate = SAMPLE_RATE;
    const S32 n = (S32)(rate * (oneShot ? 3 : LOOP_SECS));
    std::vector<F32> buf((size_t)n, 0.f);

    // Brown-ish noise at the voice's own rate: a lower rate is generated more slowly and so
    // comes out deeper, which is the cheap equivalent of resampling.
    F32 last = 0.f;
    F32 held = 0.f;
    F32 phase = 0.f;
    const F32 step = v.mRate > 0.f ? v.mRate : 1.f;
    for (S32 i = 0; i < n; ++i)
    {
        phase += step;
        while (phase >= 1.f)
        {
            phase -= 1.f;
            const F32 white = ll_frand(2.f) - 1.f;
            last = (last + 0.02f * white) / 1.02f;
            held = last * 3.5f;
        }
        buf[(size_t)i] = held;
    }

    // One-pole filters. y += (x - y) * k, with k from the cutoff — the standard RC form, and
    // enough shaping for noise: a steeper filter would not be audible through rain.
    if (v.mHighHz > 0.f)
    {
        const F32 k = 1.f - expf(-2.f * F_PI * v.mHighHz / (F32)rate);
        F32 y = 0.f;
        for (S32 i = 0; i < n; ++i) { y += (buf[(size_t)i] - y) * k; buf[(size_t)i] = y; }
    }
    if (v.mLowHz > 0.f)
    {
        const F32 k = 1.f - expf(-2.f * F_PI * v.mLowHz / (F32)rate);
        F32 y = 0.f;
        for (S32 i = 0; i < n; ++i)
        {
            y += (buf[(size_t)i] - y) * k;
            buf[(size_t)i] = buf[(size_t)i] - y;      // high-pass = signal minus its low-pass
        }
    }

    if (oneShot)
    {
        // A crack, then a rolling decay.
        const F32 attack = (F32)rate * 0.04f;
        for (S32 i = 0; i < n; ++i)
        {
            const F32 t = (F32)i;
            const F32 env = (t < attack) ? (t / attack)
                                         : expf(-(t - attack) / ((F32)n * 0.32f));
            buf[(size_t)i] *= env;
        }
    }
    else
    {
        const S32 fade = (S32)(rate * CROSSFADE_SECS);
        for (S32 i = 0; i < fade && i < n; ++i)
        {
            const F32 t = (F32)i / (F32)fade;
            buf[(size_t)i] = buf[(size_t)i] * t + buf[(size_t)(n - fade + i)] * (1.f - t);
        }
    }

    // Normalise to a known peak so a layer's own mGain means the same thing for every voice.
    F32 peak = 0.f;
    for (S32 i = 0; i < n; ++i) peak = llmax(peak, fabsf(buf[(size_t)i]));
    const F32 scale = (peak > 0.0001f) ? (0.85f / peak) : 0.f;

    std::vector<U8> wav;
    wav.reserve((size_t)n * 2 + 64);
    const U32 data_bytes = (U32)n * 2;
    put_tag(wav, "RIFF");  put_u32(wav, 36 + data_bytes);  put_tag(wav, "WAVE");
    put_tag(wav, "fmt ");  put_u32(wav, 16);
    put_u16(wav, 1);                      // PCM
    put_u16(wav, 1);                      // mono
    put_u32(wav, (U32)rate);
    put_u32(wav, (U32)rate * 2);          // byte rate  = rate * channels * bytes-per-sample
    put_u16(wav, 2);                      // block align
    put_u16(wav, 16);                     // bits per sample
    put_tag(wav, "data");  put_u32(wav, data_bytes);
    for (S32 i = 0; i < n; ++i)
    {
        const S32 s = llclamp((S32)llround(buf[(size_t)i] * scale * 32767.f), -32768, 32767);
        put_u16(wav, (U16)(S16)s);
    }

    LLFILE* fp = LLFile::fopen(path, "wb");
    if (!fp)
    {
        LL_WARNS("WolfWeather") << "could not write weather sound " << path << LL_ENDL;
        return false;
    }
    const size_t wrote = fwrite(wav.data(), 1, wav.size(), fp);
    fclose(fp);
    if (wrote != wav.size())
    {
        LL_WARNS("WolfWeather") << "short write for weather sound " << path << LL_ENDL;
        LLFile::remove(path);            // a truncated WAV would fail to load forever
        return false;
    }
    LL_INFOS("WolfWeather") << "generated weather sound " << v.mId << " -> " << path << LL_ENDL;
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
// playing
// ═══════════════════════════════════════════════════════════════════════════════════════════

LLUUID WolfWeatherSound::startLoop(const Voice& v, F32 gain)
{
    if (!gAudiop) return LLUUID::null;
    if (!ensureSound(v)) return LLUUID::null;
    const LLUUID sound_id(v.mUuid);
    const LLUUID source_id = LLUUID::generateNewID();
    // Source: fsfloaterassetblacklist.cpp:312 — trigger with our own audio-source id so the
    // source can be found again and kept (looped, re-levelled, stopped).
    gAudiop->triggerSound(sound_id, gAgentID, gain, LLAudioEngine::AUDIO_TYPE_AMBIENT,
                          gAgent.getPositionGlobal(), LLUUID::null, source_id);
    LLAudioSource* src = gAudiop->findAudioSource(source_id);
    if (!src) return LLUUID::null;
    src->setLoop(true);
    src->setGain(gain);
    return source_id;
}

void WolfWeatherSound::setLoopGain(const LLUUID& source_id, F32 gain)
{
    if (!gAudiop || source_id.isNull()) return;
    if (LLAudioSource* src = gAudiop->findAudioSource(source_id))
    {
        src->setGain(llclamp(gain, 0.f, 1.f));
    }
}

void WolfWeatherSound::stopLoop(LLUUID& source_id)
{
    if (gAudiop && source_id.notNull())
    {
        if (LLAudioSource* src = gAudiop->findAudioSource(source_id))
        {
            src->setLoop(false);
            src->setGain(0.f);
            src->play(LLUUID::null);      // llaudiosourcevo.cpp:207 — the "stop this" call
        }
    }
    source_id.setNull();
}

void WolfWeatherSound::apply(const WolfWeatherProfile& profile)
{
    mProfile = profile;
    const bool want = profile.mSound
                   && profile.mVolume > 0
                   && profile.mKind != WolfWeatherProfile::CLEAR
                   && (profile.mSoundPreset != "none" || profile.mSoundLayer != "none");
    if (!want) { stop(); return; }
    if (!gAudiop) return;                 // audio not up yet; the next apply() catches it

    // Heavier weather is louder where you are standing — the same intensity the web viewer uses.
    const F32 vol = (F32)profile.mVolume / 100.f;
    const F32 intensity = llmin(1.6f, ((F32)profile.mLevel / 2.f) * ((F32)profile.mDensity / 100.f));

    const Voice* amb = ambience(profile.mSoundPreset);
    const Voice* nr  = nearLayer(profile.mSoundLayer);

    if (!amb) { stopLoop(mAmbSource); mAmbId.clear(); }
    else if (mAmbId != profile.mSoundPreset || mAmbSource.isNull())
    {
        stopLoop(mAmbSource);
        mAmbSource = startLoop(*amb, vol * amb->mGain * (0.6f + intensity * 0.4f));
        mAmbId = mAmbSource.isNull() ? std::string() : profile.mSoundPreset;
    }
    else
    {
        setLoopGain(mAmbSource, vol * amb->mGain * (0.6f + intensity * 0.4f));
    }

    if (!nr) { stopLoop(mNearSource); mNearId.clear(); }
    else if (mNearId != profile.mSoundLayer || mNearSource.isNull())
    {
        stopLoop(mNearSource);
        mNearSource = startLoop(*nr, vol * nr->mGain * intensity);
        mNearId = mNearSource.isNull() ? std::string() : profile.mSoundLayer;
    }
    else
    {
        setLoopGain(mNearSource, vol * nr->mGain * intensity);
    }

    if (!mPlaying)
    {
        mPlaying = true;
        // Do not crack the instant the rain starts: that reads as a bug, not a storm.
        mNextThunder = LLFrameTimer::getElapsedSeconds() + THUNDER_MIN_SECS;
    }
}

void WolfWeatherSound::stop()
{
    stopLoop(mAmbSource);
    stopLoop(mNearSource);
    mAmbId.clear();
    mNearId.clear();
    mPlaying = false;
}

void WolfWeatherSound::idle()
{
    if (!mPlaying || !gAudiop) return;
    // The loops are "here": keep them on the listener so distance attenuation never fades the
    // weather out as the avatar walks.
    const LLVector3d here = gAgent.getPositionGlobal();
    if (mAmbSource.notNull())
    {
        if (LLAudioSource* s = gAudiop->findAudioSource(mAmbSource)) s->setPositionGlobal(here);
        else { mAmbSource.setNull(); mAmbId.clear(); }     // the engine reclaimed it; rebuild
    }
    if (mNearSource.notNull())
    {
        if (LLAudioSource* s = gAudiop->findAudioSource(mNearSource)) s->setPositionGlobal(here);
        else { mNearSource.setNull(); mNearId.clear(); }
    }
    // A source the engine dropped (buffer pressure, a channel taken) comes back on the next
    // apply rather than staying silently gone for the rest of the session.
    if ((mAmbSource.isNull() && mProfile.mSoundPreset != "none")
        || (mNearSource.isNull() && mProfile.mSoundLayer != "none"))
    {
        apply(mProfile);
    }
    maybeThunder();
}

void WolfWeatherSound::maybeThunder()
{
    const bool storm    = (mProfile.mSoundPreset == "thunder");
    const bool distant  = (mProfile.mSoundPreset == "distant");
    if (!storm && !distant) return;
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (now < mNextThunder) return;
    mNextThunder = now + (distant ? DISTANT_MIN_SECS : THUNDER_MIN_SECS)
                       + ll_frand((F32)(distant ? DISTANT_SPAN_SECS : THUNDER_SPAN_SECS));

    const Voice& v = distant ? CRACK_FAR : CRACK_NEAR;
    if (!ensureSound(v)) return;
    const F32 gain = llclamp((F32)mProfile.mVolume / 100.f * v.mGain, 0.f, 1.f);
    gAudiop->triggerSound(LLUUID(v.mUuid), gAgentID, gain, LLAudioEngine::AUDIO_TYPE_AMBIENT,
                          gAgent.getPositionGlobal());
}
