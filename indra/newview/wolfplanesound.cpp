/**
 * @file wolfplanesound.cpp
 * @brief [PLANE SOUND 2026-11-15] Ticket 865: the passing planes' faint, distant engines. See wolfplanesound.h.
 *        Mirror of wolfstorm/js/world/wolf_plane_sound.js.
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

#include "wolfplanesound.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "llagent.h"
#include "llaudioengine.h"
#include "lldir.h"
#include "llframetimer.h"
#include "llrand.h"
#include "llviewercamera.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "wolfgrid.h"
#include "wolfobjectprops.h"
#include "wolfsoundeffects.h"
#include "wolfweather.h"
#include "wolfweathersound.h"

namespace
{
    // Source: c-wolf-air/src/AirTraffic.cs:46 MARK "wolfair:traffic" - a pass's root is described MARK + " " + its class
    // (AirTraffic.cs Launch, LaunchCircuit); AirTraffic.Airfield.cs:50 FIELD_MARK "wolfair:traffic-field" starts with it.
    const std::string MARK("wolfair:traffic");

    // GAME TUNING (sound design: no source gives a level for a sound effect):
    constexpr F64 SCAN_S = 1.0;          // the moving roots are looked at once a second
    constexpr S32 ASKS_PER_SCAN = 4;     // descriptions asked per look (WolfObjectProps keeps the shared budget)
    constexpr F64 LOST_S = 3.0;          // not seen moving for this long: gone (taken away at the edge, or parked)
    constexpr F32 HEAR_M = 3000.f;       // silent beyond
    constexpr S32 MAX_VOICES = 4;
    constexpr F32 TAU_UP = 2.0f, TAU_DOWN = 1.2f;   // seconds: she fades in as she comes into view, out as she goes
    constexpr F32 VEL_TAU = 0.5f;        // the velocity from her positions, smoothed over about this long
    constexpr F32 MAX_SPEED = 700.f;     // a jump faster than this is a region change, not flight: the velocity starts again
    // Full level out to REF_M, then 1 / distance: spherical spreading, the geometric divergence of ISO 9613-2:1996 s7.1
    // (Adiv = 20 lg(d / d0) + 11 dB, -6 dB each time the distance doubles). REF_M is the lowest a pass plans over the ground
    // (c-wolf-air/src/AirTraffic.cs MIN_ALT_M 150).
    constexpr F32 REF_M = 150.f;
    // Source: wolfsealife.cpp CALL_EAR_M (and wolfbirds.cpp): the sound is placed this far along the true direction from the
    // camera, so the audio engine's own distance roll-off does not take it again; the gain above is the distance.
    constexpr F32 EAR_M = 4.f;
    // Source: wolfsealife.cpp sound() 0.35 under a roof, eased by how far indoors (WolfWeather::indoors, as the rain's muffle
    // and wolfstorm wolf_nature_sounds.js ROOF): 1 - (1 - 0.35) x indoors
    constexpr F32 INDOORS = 0.35f;
    // Source: wolfweathersound.cpp SILENT / START_GAIN (LLAudioEngine::triggerSound makes no source under 2 x FLT_EPSILON)
    constexpr F32 SILENT = 0.0005f, START_GAIN = 0.001f;

    // The two loops. 8 s at 22,050 Hz, mono (wolfweathersound.h SAMPLE_RATE); the tail is folded into the head with equal
    // power (wolfweathersound.cpp writeWav), and every periodic part fits the loop a whole number of times, so it never clicks.
    constexpr S32 RATE = 22050, LOOP_S = 8;
    constexpr F32 FOLD_S = 0.5f;
    struct Voice
    {
        const char* mName;
        const char* mUuid;     ///< WolfViewer's own (made 2026-11-15), never an asset on any grid: written into the sound cache
        F32 mBase;             ///< the level at REF_M, before the class, the Sound Effects volume and the Ambient volume
    };
    enum { PROPELLER = 0, JET = 1 };
    const Voice VOICES[2] = {
        { "propeller", "cf0a4371-0139-4215-a772-6b6f18124d35", 0.30f },
        { "jet",       "8943d057-1015-40f1-80b0-3a969a84430d", 0.40f },
    };
    // Source: AirTraffic.cs:56 Classes. The voice by engine (AirTraffic.cs FliesAs: the tour planes' piston engines, the
    // airliners' and the stand-ins' jets); the class's loudness GAME TUNING (the Camel's small rotary quieter, the
    // science-fiction craft softer: no real engine to copy).
    struct ClassSound { const char* mClass; S32 mVoice; F32 mMult; };
    const ClassSound CLASSES[] = {
        { "traffic_p51d_tours",       PROPELLER, 1.0f },
        { "traffic_spitfire_tours",   PROPELLER, 1.0f },
        { "traffic_camel_tours",      PROPELLER, 0.6f },
        { "traffic_b747_wolfair",     JET,       1.0f },
        { "traffic_concorde_wolfair", JET,       1.0f },
        { "traffic_starfighter",      JET,       0.7f },
        { "traffic_saucer",           JET,       0.5f },
    };
    const ClassSound UNKNOWN_CLASS = { "", PROPELLER, 0.8f };   // a class added to the plugin later

    const ClassSound& class_sound(const std::string& cls)
    {
        for (const ClassSound& c : CLASSES)
        {
            if (cls == c.mClass) return c;
        }
        return UNKNOWN_CLASS;
    }

    /** "wolfair:traffic <class>" or "wolfair:traffic-field <class>" (lower-cased) -> true and the class. */
    bool traffic_class(const std::string& desc_lower, std::string& cls)
    {
        if (desc_lower.compare(0, MARK.size(), MARK) != 0) return false;
        const size_t sp = desc_lower.find(' ', MARK.size());
        cls = (sp == std::string::npos) ? std::string() : desc_lower.substr(sp + 1);
        const size_t end = cls.find_first_of(" |");
        if (end != std::string::npos) cls.resize(end);
        return true;
    }

    /** RMS 1, mean 0. */
    void normalise(std::vector<F32>& v)
    {
        if (v.empty()) return;
        F64 mean = 0.0;
        for (F32 x : v) mean += x;
        mean /= (F64)v.size();
        F64 sq = 0.0;
        for (F32& x : v) { x -= (F32)mean; sq += (F64)x * x; }
        const F32 rms = (F32)sqrt(sq / (F64)v.size());
        if (rms > 1e-9f) for (F32& x : v) x /= rms;
    }

    /** One-pole low-pass, y += (x - y) k (wolfweathersound.cpp writeWav), in place. */
    void low_pass(std::vector<F32>& v, F32 hz)
    {
        const F32 k = 1.f - expf(-2.f * F_PI * hz / (F32)RATE);
        F32 y = 0.f;
        for (F32& x : v) { y += (x - y) * k; x = y; }
    }

    /** High-pass = the signal less its low-pass (wolfweathersound.cpp writeWav), in place. */
    void high_pass(std::vector<F32>& v, F32 hz)
    {
        const F32 k = 1.f - expf(-2.f * F_PI * hz / (F32)RATE);
        F32 y = 0.f;
        for (F32& x : v) { y += (x - y) * k; x -= y; }
    }

    /** Brown-ish noise, white integrated a little: a hush, not a hiss (wolfweathersound.cpp writeWav). */
    std::vector<F32> brown(S32 count)
    {
        std::vector<F32> v((size_t)count);
        F32 last = 0.f;
        for (F32& x : v)
        {
            last = (last + 0.02f * (ll_frand(2.f) - 1.f)) / 1.02f;
            x = last * 3.5f;
        }
        return v;
    }

    /**
     * The loop of one voice, as wolf_plane_sound.js makeLoop. Each part is made WARM_S longer at the front (dropped: the
     * filters settle first) and FOLD_S longer at the end (folded into the head).
     *  propeller  brown noise low-passed at 420 Hz (0.7) and a soft beat at 84 Hz (0.3): a pulse (0.5 + 0.5 cos)^6 low-passed
     *             at 300 Hz - the blades' throb; the whole swelling by 25 % every 2 s (0.5 Hz).
     *  jet        brown noise low-passed at 800 Hz and high-passed at 50 Hz (0.9) and white noise low-passed at 2,500 Hz
     *             (0.1): the roar with a little of its hiss; swelling by 15 % every 4 s (0.25 Hz).
     * GAME TUNING (sound design), heard from far off: the highs that the air takes away over distance are left out.
     */
    std::vector<F32> make_loop(S32 voice)
    {
        const S32 n = RATE * LOOP_S, fold = (S32)(RATE * FOLD_S), warm = RATE;
        const S32 total = warm + n + fold;
        std::vector<F32> a = brown(total), b;
        F32 wa, wb, swell, swell_hz;
        if (voice == PROPELLER)
        {
            low_pass(a, 420.f);
            b.resize((size_t)total);
            constexpr F32 BEAT_HZ = 84.f;             // 672 beats in the 8 s loop
            for (S32 i = 0; i < total; ++i)
            {
                const F32 c = 0.5f + 0.5f * cosf(2.f * F_PI * BEAT_HZ * (F32)(i - warm) / (F32)RATE);
                b[(size_t)i] = powf(c, 6.f);
            }
            low_pass(b, 300.f);
            wa = 0.7f; wb = 0.3f; swell = 0.25f; swell_hz = 0.5f;
        }
        else
        {
            low_pass(a, 800.f);
            high_pass(a, 50.f);
            b.resize((size_t)total);
            for (F32& x : b) x = ll_frand(2.f) - 1.f;
            low_pass(b, 2500.f);
            wa = 0.9f; wb = 0.1f; swell = 0.15f; swell_hz = 0.25f;
        }
        a.erase(a.begin(), a.begin() + warm);
        b.erase(b.begin(), b.begin() + warm);
        normalise(a);
        normalise(b);
        std::vector<F32> out((size_t)(n + fold));
        for (S32 i = 0; i < n + fold; ++i)
        {
            const F32 s = 1.f + swell * sinf(2.f * F_PI * swell_hz * (F32)i / (F32)RATE);
            out[(size_t)i] = (wa * a[(size_t)i] + wb * b[(size_t)i]) * s;
        }
        // equal power: the head fades in (sin) over the tail that followed sample n-1 (cos)
        for (S32 i = 0; i < fold; ++i)
        {
            const F32 t = ((F32)i / (F32)fold) * F_PI_BY_TWO;
            out[(size_t)i] = out[(size_t)i] * sinf(t) + out[(size_t)(n + i)] * cosf(t);
        }
        out.resize((size_t)n);
        F32 peak = 0.f;
        for (F32 x : out) peak = llmax(peak, fabsf(x));
        const F32 scale = peak > 1e-4f ? 0.85f / peak : 0.f;     // the weather's peak (wolfweathersound.cpp writeWav)
        for (F32& x : out) x *= scale;
        return out;
    }

    /** A looping source the plane's velocity can be given, so the audio engine applies its Doppler shift (llaudioengine.h
     *  LLAudioSource keeps mVelocity for the channel's 3D attributes: llaudioengine_fmodstudio.cpp update3DPosition,
     *  llaudioengine_openal.cpp AL_VELOCITY; LLAudioSourceVO sets it from its object the same way). */
    class PlaneSource : public LLAudioSource
    {
    public:
        PlaneSource(const LLUUID& id, F32 gain)
            // Source: LLAudioEngine::triggerSound (llaudioengine.cpp) - the same arguments it gives a triggered source
            : LLAudioSource(id, gAgentID, gain, LLAudioEngine::AUDIO_TYPE_AMBIENT, LLUUID::null, true) {}
        void setVelocity(const LLVector3& v) { mVelocity = v; }
    };
}

WolfPlaneSound::WolfPlaneSound() {}
WolfPlaneSound::~WolfPlaneSound() {}

bool WolfPlaneSound::soundReady(S32 voice)
{
    if (mReady[voice] != 0) return mReady[voice] > 0;
    const Voice& v = VOICES[voice];
    const std::string path = WolfWeatherSound::cachedSoundPath(LLUUID(v.mUuid));
    if (gDirUtilp->fileExists(path))
    {
        mReady[voice] = 1;
        return true;
    }
    const bool ok = WolfWeatherSound::writeCachedWav(path, make_loop(voice), 1, (U32)RATE);
    mReady[voice] = ok ? 1 : -1;
    if (ok) LL_INFOS("WolfPlaneSound") << "generated the " << v.mName << " engine loop -> " << path << LL_ENDL;
    else    LL_WARNS("WolfPlaneSound") << "could not write the " << v.mName << " engine loop " << path << " (that voice stays silent)" << LL_ENDL;
    return ok;
}

void WolfPlaneSound::scan(F64 now, const LLVector3& cam)
{
    std::vector<std::pair<F32, LLViewerObject*>> ask;
    for (const LLPointer<LLViewerObject>& ptr : gObjectList.getActiveObjects())
    {
        LLViewerObject* o = ptr.get();
        if (!o || o->isDead() || o->getParent() || o->isAvatar() || o->isAttachment()) continue;
        const F32 d = (o->getPositionAgent() - cam).length();
        if (d > HEAR_M) continue;
        const WolfObjectProps::Props* pr = WolfObjectProps::instance().get(o->getID());
        if (!pr)
        {
            ask.emplace_back(d, o);
            continue;
        }
        std::string cls;
        if (!traffic_class(pr->mDescriptionLower, cls)) continue;
        auto it = mPlanes.find(o->getID());
        if (it == mPlanes.end())
        {
            Plane p;
            const ClassSound& c = class_sound(cls);
            p.mVoice = c.mVoice;
            p.mMult = c.mMult;
            p.mClass = cls;
            it = mPlanes.emplace(o->getID(), p).first;
            LL_INFOS("WolfPlaneSound") << "passing plane " << (cls.empty() ? "(no class)" : cls) << " " << o->getID() << " at "
                                       << (S32)d << " m: " << VOICES[c.mVoice].mName << " engine" << LL_ENDL;
        }
        it->second.mSeenAt = now;
    }
    // nearest first, a few a look (wolfair.cpp scanSeeker asks the same way)
    std::sort(ask.begin(), ask.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (S32 i = 0; i < (S32)ask.size() && i < ASKS_PER_SCAN; ++i) WolfObjectProps::instance().want(ask[(size_t)i].second, false);
}

void WolfPlaneSound::stopSource(Plane& p)
{
    if (gAudiop && p.mSource.notNull())
    {
        if (LLAudioSource* src = gAudiop->findAudioSource(p.mSource))
        {
            // Source: wolfweathersound.cpp stopSource - the engine's "stop this" (llaudiosourcevo.cpp play(null))
            src->setLoop(false);
            src->setGain(0.f);
            src->play(LLUUID::null);
        }
    }
    p.mSource.setNull();
}

void WolfPlaneSound::update(const LLUUID& id, Plane& p, F32 dt, F64 now, const LLVector3& cam, F32 level)
{
    LLViewerObject* o = gObjectList.findObject(id);
    const bool here = o && !o->isDead();
    if (here)
    {
        const LLVector3 pos = o->getPositionAgent();
        const F64 span = now - p.mPosAt;
        if (p.mPosAt > 0.0 && span > 1e-3)
        {
            const LLVector3 v = (pos - p.mPos) * (F32)(1.0 / span);
            if (v.length() > MAX_SPEED) p.mHaveVel = false;
            else if (!p.mHaveVel) { p.mVel = v; p.mHaveVel = true; }
            else p.mVel += (v - p.mVel) * (1.f - expf(-(F32)span / VEL_TAU));
        }
        p.mPos = pos;
        p.mPosAt = now;
    }

    LLViewerCamera* camera = LLViewerCamera::getInstance();
    const F32 d = (p.mPos - cam).length();
    F32 target = 0.f;
    if (here && level > 0.f && now - p.mSeenAt <= LOST_S && d <= HEAR_M && !camera->cameraUnderWater())
    {
        const F32 indoors = 1.f - (1.f - INDOORS) * llclamp(WolfWeather::indoors(), 0.f, 1.f);
        target = llclamp(VOICES[p.mVoice].mBase * p.mMult * (REF_M / llmax(REF_M, d)) * indoors * level, 0.f, 1.f);
    }
    p.mTarget = target;
    const F32 tau = target > p.mGain ? TAU_UP : TAU_DOWN;
    p.mGain += (target - p.mGain) * (1.f - expf(-dt / tau));
    if (target <= 0.f && p.mGain < SILENT)
    {
        p.mGain = 0.f;
        stopSource(p);
        return;
    }

    LLAudioSource* found = p.mSource.notNull() ? gAudiop->findAudioSource(p.mSource) : nullptr;
    PlaneSource* src = dynamic_cast<PlaneSource*>(found);
    if (p.mSource.notNull() && !src)
    {
        // the engine let it go (a channel taken, the sound cache purged): look at the loop again, start again shortly
        p.mSource.setNull();
        p.mRetryAt = now + 0.5;
        mReady[p.mVoice] = 0;
    }
    LLVector3 dir = p.mPos - cam;
    if (dir.isExactlyZero()) dir = camera->getAtAxis();
    dir.normVec();
    const LLVector3d at = gAgent.getPosGlobalFromAgent(cam + dir * EAR_M);
    if (!src)
    {
        if (target <= 0.f || now < p.mRetryAt || !soundReady(p.mVoice)) return;
        S32 playing = 0;
        for (const auto& kv : mPlanes) if (kv.second.mSource.notNull()) ++playing;
        if (playing >= MAX_VOICES) { p.mRetryAt = now + 1.0; return; }
        if (gAudiop->getMuted())
        {
            // Source: LLAudioEngine::triggerSound - nothing is started while the viewer is muted; it starts once unmuted
            if (!p.mLogged) LL_INFOS("WolfPlaneSound") << "passing plane " << id << ": the viewer is muted, no engine sound (would be gain "
                                                       << llmax(p.mGain, START_GAIN) << " at " << (S32)d << " m)" << LL_ENDL;
            p.mLogged = true;
            p.mRetryAt = now + 2.0;
            return;
        }
        // Source: LLAudioEngine::triggerSound (llaudioengine.cpp) - a new source, added, placed, prioritised, played; with our
        // own id so it can be found again (wolfweathersound.cpp updateLayer), looping
        const LLUUID source_id = LLUUID::generateNewID();
        src = new PlaneSource(source_id, llmax(p.mGain, START_GAIN));
        gAudiop->addAudioSource(src);
        src->setPositionGlobal(at);
        src->setVelocity(p.mHaveVel ? p.mVel : LLVector3::zero);
        src->setLoop(true);
        src->updatePriority();
        src->play(LLUUID(VOICES[p.mVoice].mUuid));
        p.mSource = source_id;
        LL_INFOS("WolfPlaneSound") << "passing plane " << id << ": " << VOICES[p.mVoice].mName << " engine started at " << (S32)d
                                   << " m, gain " << llmax(p.mGain, START_GAIN) << " rising to " << target << LL_ENDL;
    }
    src->setGain(llmax(p.mGain, START_GAIN));
    src->setPositionGlobal(at);
    src->setVelocity(p.mHaveVel ? p.mVel : LLVector3::zero);
}

void WolfPlaneSound::idle()
{
    const F64 now = LLFrameTimer::getElapsedSeconds();
    const F32 dt = mLastIdle > 0.0 ? (F32)llclamp(now - mLastIdle, 0.0, 0.25) : 0.f;
    mLastIdle = now;
    if (!gAudiop) return;
    const F32 level = WolfSoundEffects::level(WolfSoundEffects::PLANES);
    const bool want = level > 0.f && WolfGrid::showsWolfRegionWeather();
    if (!want && mPlanes.empty()) return;
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    if (want && now >= mNextScan)
    {
        mNextScan = now + SCAN_S;
        scan(now, cam);
    }
    for (auto it = mPlanes.begin(); it != mPlanes.end(); )
    {
        Plane& p = it->second;
        update(it->first, p, dt, now, cam, want ? level : 0.f);
        const bool gone = !want || now - p.mSeenAt > LOST_S;
        if (gone && p.mGain <= 0.f && p.mSource.isNull())
        {
            LL_INFOS("WolfPlaneSound") << "passing plane " << it->first << " gone" << LL_ENDL;
            it = mPlanes.erase(it);
        }
        else
        {
            ++it;
        }
    }
}
