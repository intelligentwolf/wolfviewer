/**
 * @file wolfstarfold.cpp
 * @brief WolfViewer: Starfold - the wire, the pilot's intents, the sky's numbers, the belt, the beams, the wormhole.
 *        See wolfstarfold.h.
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

#include "wolfstarfold.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "fsnearbychathub.h"
#include "llagent.h"
#include "llagentcamera.h"
#include "llaudioengine.h"
#include "llchat.h"
#include "lldir.h"
#include "lldispatcher.h"
#include "llenvironment.h"
#include "llfile.h"
#include "llframetimer.h"
#include "llglslshader.h"
#include "llimage.h"
#include "llkeyboard.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llvertexbuffer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewergenericmessage.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerpartsim.h"
#include "llviewerpartsource.h"
#include "llviewerregion.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "llvoavatarself.h"
#include "llworld.h"
#include "pipeline.h"
#include "wolfdrive.h"
#include "wolfflight.h"
#include "wolffx.h"
#include "wolfgrid.h"
#include "wolfhelm.h"
#include "wolfspacesky.h"
#include "wolfstarbase.h"

extern bool gCubeSnapshot;   // llviewerdisplay.cpp:208 (the same extern lldrawpoolalpha.cpp:140 uses)

namespace
{
    // ── the wire (Source: wolf-space/src/SpaceRules.cs) ─────────────────────────────────────────────────────────────
    const std::string MESSAGE_FROM_SIM("WolfSpace");   // SpaceRules.METHOD
    const S32 CHANNEL = -73653;                          // SpaceRules.CHANNEL
    const std::string PREFIX("SF1|");                    // SpaceRules.INTENT_PREFIX
    const S32 MAX_INTENTS_PER_S = 20;                    // SpaceRules.MAX_INTENTS_PER_S: we keep 2 spare
    const S32 MAX_CONTROLS_PER_S = 12;                   // ctl's share of them, so a press always has room
    const F64 CONTROL_MIN_GAP_S = 0.08;                  // at most ~12 ctl a second
    const F32 WORMHOLE_M = 25.f;                         // SpaceRules.WORMHOLE_M: the wormhole's reach (its sphere)
    const F32 VACUUM_AT_M = 50000.f;                     // SpaceRules.CEILING_M (OpenSim Constants.cs:160 MaxSimulationHeight)
    const F32 AIR_SCALE_HEIGHT_M = 8500.f;               // wolfaltitudesky.cpp AIR_SCALE_HEIGHT_M
    const F32 EDGE_M = 300.f;                            // README "client look rules": the air fades over the line's 300 m
    const F32 CLOUD_GAP_M = 200.f;                       // README: clouds held 200 m below the line (design: "clouds kept 200 m below")

    // ── GAME TUNING (the viewer's own look and feel) ──────────────────────────────────────────────────────────────────
    const F32 THROTTLE_KEY_RATE = 0.6f;      // throttle per second while W / S (PgUp / PgDn) is held
    const F64 HELLO_AFTER_S = 4.0;           // after arriving, ask for the system if the region has not said it
    const F64 HELLO_RETRY_S = 30.0;
    const F64 NOTE_SHOW_S = 10.0;
    const F64 JUMP_HOLD_S = 12.0;            // the tunnel waits this long at most for the teleport
    const F64 JUMP_FADE_S = 1.2;
    const F32 BEAM_SEG_M = 6.f;
    const S32 BEAM_SEGS_MAX = 120;
    // [WOLFTREK v11] the hangar view (design.html#bay: "frames your ship on its pad, as when you look at an object, and turns
    // slowly round it until you move")
    const F32 HANGAR_TURN_S = 60.f;          // one turn round the ship a minute ("turns slowly")
    const F32 HANGAR_BEARING_DEG = 45.f;     // a three-quarter view: half way between the nose and the side towards the walkway
    const F32 HANGAR_ELEV_DEG = 22.f;        // a little above the ship (the bay is about 22 m high: design.html#bay)
    const F32 HANGAR_DIST_K = 2.6f;          // the ship's half-diagonal times this: the whole ship in a 60-degree view with room round it
    const F32 HANGAR_DIST_MIN = 7.f, HANGAR_DIST_MAX = 24.f;   // metres: a 16-22 m pad's ship stays inside a 56 m wide bay
    const F64 HANGAR_SETTLE_S = 2.0;         // the move to the view (LLAgentCamera animation, at most 1 s) has ended by then
    const F64 HANGAR_WAIT_S = 10.0;          // how long the ship's object is waited for when the region's word comes first
    const F64 ANSWER_FOR_S = 20.0;           // a k=msg this soon after a rename is the Station window's answer (as wolfstarbase.cpp)

    F64 now() { return LLFrameTimer::getElapsedSeconds(); }

    F32 smooth(F32 e0, F32 e1, F32 x)
    {
        const F32 t = llclamp((x - e0) / (e1 - e0), 0.f, 1.f);
        return t * t * (3.f - 2.f * t);
    }

    // Source: wolf-space SpaceEngine.cs Next - xorshift32 13/17/5 (Marsaglia 2003), scaled to [0,1)
    F64 next(U32& s)
    {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return (F64)s / 4294967296.0;
    }

    bool parseVec(const std::string& s, LLVector3& out)
    {
        F32 x, y, z;
        if (sscanf(s.c_str(), "%f,%f,%f", &x, &y, &z) != 3) return false;
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
        out.set(x, y, z);
        return true;
    }

    F32 num(const std::string& s, F32 dflt = 0.f)
    {
        char* end = nullptr;
        const F32 f = strtof(s.c_str(), &end);
        return (end != s.c_str() && std::isfinite(f)) ? f : dflt;
    }

    std::vector<std::string> split(const std::string& s, char sep)
    {
        std::vector<std::string> out;
        size_t a = 0;
        while (true)
        {
            const size_t b = s.find(sep, a);
            out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
            if (b == std::string::npos) break;
            a = b + 1;
        }
        return out;
    }

    // A line the region sends us to show: printable, at most 240 characters (SceneSpaceWorld.Cut keeps 250 with "t=").
    std::string clean(const std::string& s)
    {
        std::string o;
        for (unsigned char ch : s) { if (ch >= 32) o.push_back((char)ch); }   // no control characters
        return o.size() > 240 ? o.substr(0, 240) : o;
    }

    LLColor3 hsv(F32 h, F32 s, F32 v)
    {
        h = h - floorf(h);
        const F32 i = floorf(h * 6.f), f = h * 6.f - i;
        const F32 p = v * (1.f - s), q = v * (1.f - f * s), t = v * (1.f - (1.f - f) * s);
        switch ((S32)i % 6)
        {
            case 0: return LLColor3(v, t, p);
            case 1: return LLColor3(q, v, p);
            case 2: return LLColor3(p, v, t);
            case 3: return LLColor3(p, q, v);
            case 4: return LLColor3(t, p, v);
            default: return LLColor3(v, p, q);
        }
    }

    // ── the planet from the seed (README "client look rules": the draws in this order) ────────────────────────────────
    struct Planet
    {
        U32 mSeed = 0;
        LLVector3 mDir;
        F32 mRadiusRad = 0.f;
        bool mGas = false, mRings = false;
        F32 mHue = 0.f, mNebulaHue = 0.f;
    };
    const Planet& planetOf(U32 seed)
    {
        static Planet p;
        if (p.mSeed == seed && p.mRadiusRad > 0.f) return p;
        p.mSeed = seed;
        U32 x = seed ^ 0x5F3759DFu;
        if (x == 0) x = 1;
        const F32 az = (F32)(next(x) * 2.0 * F_PI);
        const F32 el = (F32)((10.0 + 35.0 * next(x)) * DEG_TO_RAD);
        p.mRadiusRad = (F32)((3.0 + 5.0 * next(x)) * DEG_TO_RAD);
        p.mGas = next(x) >= 0.5;
        p.mHue = (F32)next(x);
        p.mRings = next(x) < 0.4;
        p.mNebulaHue = (F32)next(x);
        p.mDir.set(cosf(el) * cosf(az), cosf(el) * sinf(az), sinf(el));   // from east towards north, up
        LL_INFOS("WolfStarfold") << "planet for seed " << seed << ": azimuth " << az * RAD_TO_DEG << " deg from east towards north, elevation "
                                 << el * RAD_TO_DEG << " deg, radius " << p.mRadiusRad * RAD_TO_DEG << " deg, " << (p.mGas ? "gas" : "rocky")
                                 << (p.mRings ? " with rings" : "") << LL_ENDL;
        return p;
    }

    // ── sounds: our own, made here (Source: wolfair.cpp writeWarnTone - a 16-bit mono WAV where the audio engine looks for
    //    a decoded sound, LLAudioEngine::hasDecodedFile, llaudioengine.cpp builds "<uuid>.dsf" in LL_PATH_FS_SOUND_CACHE) ──
    const char* SOUND_LASER = "b5272086-0e6c-4db6-bf0d-fa6d40048908";     // the Cub's laser: a falling chirp
    const char* SOUND_SHIELD = "b7f85af4-3846-4cfc-bd76-59e30870662a";    // a hit on the shield: a crackling buzz
    const char* SOUND_ARMOUR = "f3e0fcc2-a7ca-4bce-a62b-88fdaf559718";    // a hit on the armour: a dull knock
    const char* SOUND_HUM = "20b7c0a1-41fd-40e5-a083-374cb120f039";       // the wormhole's low hum
    const U32 RATE = 22050;

    void put_u32(std::vector<U8>& out, U32 v) { for (int i = 0; i < 4; ++i) out.push_back((U8)((v >> (8 * i)) & 0xff)); }
    void put_u16(std::vector<U8>& out, U16 v) { out.push_back((U8)(v & 0xff)); out.push_back((U8)((v >> 8) & 0xff)); }

    bool writeWav(const char* uuid, const std::vector<F32>& pcm)
    {
        std::vector<U8> wav;
        const U32 data_bytes = (U32)pcm.size() * 2;
        const char* riff = "RIFF"; const char* wave = "WAVEfmt "; const char* data = "data";
        wav.insert(wav.end(), riff, riff + 4); put_u32(wav, 36 + data_bytes);
        wav.insert(wav.end(), wave, wave + 8); put_u32(wav, 16);
        put_u16(wav, 1); put_u16(wav, 1); put_u32(wav, RATE); put_u32(wav, RATE * 2); put_u16(wav, 2); put_u16(wav, 16);
        wav.insert(wav.end(), data, data + 4); put_u32(wav, data_bytes);
        for (F32 f : pcm) put_u16(wav, (U16)(S16)llclamp((S32)std::lround(f * 32767.f), -32768, 32767));
        const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_FS_SOUND_CACHE, std::string(uuid)) + ".dsf";
        const std::string tmp = path + ".tmp";
        LLFILE* fp = LLFile::fopen(tmp, "wb");
        if (!fp) return false;
        const size_t wrote = fwrite(wav.data(), 1, wav.size(), fp);
        fclose(fp);
        if (wrote != wav.size()) { LLFile::remove(tmp); return false; }
        LLFile::remove(path, ENOENT);
        if (LLFile::rename(tmp, path) != 0) { LLFile::remove(tmp); return false; }
        return true;
    }

    // one of our sounds, through the sound-effects volume (as wolfair.cpp: a SoundData the engine takes by reference)
    void play(const char* uuid, F32 gain)
    {
        if (!gAudiop) return;
        SoundData sd{ LLUUID(uuid), gAgentID, gain, LLAudioEngine::AUDIO_TYPE_SFX };
        gAudiop->triggerSound(sd);
    }

    // a small fixed noise generator for the sounds (the same sounds every time)
    F32 noise(U32& s) { return (F32)(next(s) * 2.0 - 1.0); }

    bool writeSounds()
    {
        bool ok = true;
        U32 ns = 0x2545F491u;
        {
            // the laser: 0.28 s, a chirp falling from 1,400 to 300 Hz with a touch of noise, a fast attack and a falling tail
            std::vector<F32> pcm;
            const U32 n = RATE * 28 / 100;
            F32 ph = 0.f;
            for (U32 i = 0; i < n; ++i)
            {
                const F32 t = (F32)i / (F32)n;
                const F32 f = 1400.f * powf(300.f / 1400.f, t);
                ph += F_TWO_PI * f / (F32)RATE;
                const F32 env = llmin(1.f, (F32)i / (RATE * 0.004f)) * (1.f - t) * (1.f - t);
                pcm.push_back(0.45f * env * (sinf(ph) + 0.35f * sinf(ph * 2.01f) + 0.12f * noise(ns)));
            }
            ok = writeWav(SOUND_LASER, pcm) && ok;
        }
        {
            // the shield hit: 0.3 s, a 160 Hz buzz (odd harmonics) under a 30 Hz tremolo, with crackle
            std::vector<F32> pcm;
            const U32 n = RATE * 30 / 100;
            for (U32 i = 0; i < n; ++i)
            {
                const F32 t = (F32)i / (F32)RATE, u = (F32)i / (F32)n;
                const F32 ph = F_TWO_PI * 160.f * t;
                const F32 buzz = sinf(ph) + sinf(3.f * ph) / 3.f + sinf(5.f * ph) / 5.f;
                const F32 trem = 0.6f + 0.4f * sinf(F_TWO_PI * 30.f * t);
                const F32 env = llmin(1.f, (F32)i / (RATE * 0.003f)) * (1.f - u);
                pcm.push_back(0.35f * env * (buzz * trem + 0.25f * noise(ns)));
            }
            ok = writeWav(SOUND_SHIELD, pcm) && ok;
        }
        {
            // the armour hit: 0.22 s, a knock - low-passed noise and a 70 Hz thump, falling fast
            std::vector<F32> pcm;
            const U32 n = RATE * 22 / 100;
            F32 lp = 0.f;
            for (U32 i = 0; i < n; ++i)
            {
                const F32 t = (F32)i / (F32)RATE, u = (F32)i / (F32)n;
                lp += 0.08f * (noise(ns) - lp);
                const F32 env = expf(-u * 6.f);
                pcm.push_back(0.7f * env * (0.8f * sinf(F_TWO_PI * 70.f * t) + 2.5f * lp));
            }
            ok = writeWav(SOUND_ARMOUR, pcm) && ok;
        }
        {
            // the wormhole: 3 s, two low tones (46 and 69.3 Hz, a fifth, as WolfStorm's hum) beating slowly, faded in and out so it can repeat
            std::vector<F32> pcm;
            const U32 n = RATE * 3;
            for (U32 i = 0; i < n; ++i)
            {
                const F32 t = (F32)i / (F32)RATE, u = (F32)i / (F32)n;
                const F32 env = sinf(F_PI * u);
                const F32 beat = 0.75f + 0.25f * sinf(F_TWO_PI * 0.7f * t);
                pcm.push_back(0.4f * env * beat * (sinf(F_TWO_PI * 46.f * t) + 0.6f * sinf(F_TWO_PI * 69.3f * t)));
            }
            ok = writeWav(SOUND_HUM, pcm) && ok;
        }
        return ok;
    }

    // [WOLFTREK v11 stage B] Source: SpaceFleet.cs FleetArgs - <id>;<title>;<price>;<owned 0|1>;<active 0|1>;<top speed>;<turn>;
    // <hold>;<shield>;<armour>;<hull>;<guns>;<mass>;<pad m>;<role>, numbers by F() ("0.##", invariant); the role is the last field and
    // is taken whole (the rest of the line). False when it does not read (as WolfStorm's parseFleet).
    bool idToken(const std::string& id)
    {
        if (id.empty() || id.size() > 32) return false;
        for (char c : id) if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
        return true;
    }
    bool parseFleet(const std::string& v, WolfStarfold::ShipInfo& out)
    {
        const std::vector<std::string> f = split(v, ';');
        if (f.size() < 15 || !idToken(f[0])) return false;
        if (f[2].empty() || f[2].size() > 12 || f[2].find_first_not_of("0123456789") != std::string::npos) return false;
        if ((f[3] != "0" && f[3] != "1") || (f[4] != "0" && f[4] != "1")) return false;
        F32 n[9];
        for (size_t i = 0; i < 9; ++i)
        {
            char* end = nullptr;
            n[i] = strtof(f[5 + i].c_str(), &end);
            if (end == f[5 + i].c_str() || *end != 0 || !std::isfinite(n[i]) || n[i] < 0.f) return false;
        }
        out.mId = f[0];
        out.mTitle = clean(f[1]).substr(0, 32);
        if (out.mTitle.empty()) out.mTitle = out.mId;
        out.mPrice = (S64)strtoll(f[2].c_str(), nullptr, 10);
        out.mOwned = f[3] == "1";
        out.mActive = f[4] == "1";
        out.mSpeed = n[0]; out.mTurn = n[1]; out.mHold = n[2]; out.mShield = n[3]; out.mArmour = n[4]; out.mHull = n[5];
        out.mGuns = (S32)llround(n[6]); out.mMass = n[7]; out.mPad = n[8];
        std::string role = f[14];
        for (size_t i = 15; i < f.size(); ++i) role += ";" + f[i];
        out.mRole = clean(role).substr(0, 80);
        return true;
    }
    const WolfStarfold::ShipInfo* fleetClass(const std::vector<WolfStarfold::ShipInfo>& fleet, const std::string& id, const std::string& title)
    {
        for (const WolfStarfold::ShipInfo& c : fleet) if ((!id.empty() && c.mId == id) || (id.empty() && !title.empty() && c.mTitle == title)) return &c;
        return nullptr;
    }

    // Source: wolfair.cpp WolfAirHandler - the region's GenericMessage. Read on any Wolf region underfoot (what is DRAWN
    // follows the region: WolfGrid::showsWolfRegion, Paul 24 Nov "Hypergrid visitors see the sky").
    class WolfStarfoldHandler : public LLDispatchHandler
    {
    public:
        bool operator()(const LLDispatcher*, const std::string&, const LLUUID& invoice, const sparam_t& strings) override
        {
            if (!WolfGrid::showsWolfRegion()) return true;
            WolfStarfold::instance().receive(invoice, strings);
            return true;
        }
    };
    WolfStarfoldHandler sHandler;

    // the cockpit's keys, by what they do (wolfstarfold.h noteScanKey)
    enum EAct { A_THR_UP = 0, A_THR_DOWN, A_PITCH_UP, A_PITCH_DOWN, A_YAW_LEFT, A_YAW_RIGHT, A_ROLL_LEFT, A_ROLL_RIGHT,
                A_SLIDE_LEFT, A_SLIDE_RIGHT, A_SLIDE_UP, A_SLIDE_DOWN, A_FIRE, A_COUNT };
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// The beams: particles (the viewer's own particle renderer, additive and emissive with glow, as wolffx.cpp's tracer)
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════

class WolfStarfoldBeamSource : public LLViewerPartSource
{
public:
    WolfStarfoldBeamSource() : LLViewerPartSource(LL_PART_SOURCE_NULL) {}   // as WolfFXSource (wolffx.cpp)

    void update(const F32 dt) override
    {
        if (!mImagep) mImagep = LLViewerFetchedTexture::sDefaultParticleImagep;   // the soft round particle every viewer has
        mPosAgent = LLViewerCamera::getInstance()->getOrigin();
        WolfStarfold& sf = WolfStarfold::instance();
        if (!sf.spaceHere()) return;
        static LLCachedControl<bool> fx_on(gSavedSettings, "WolfFXEnabled", true);
        if (!fx_on) return;
        // a frame's life (a little over), so the beam is drawn afresh each frame wherever its ends have moved
        const F32 life = llclamp(dt * 1.25f, 0.02f, 0.12f);
        const F32 t = (F32)now();
        for (const WolfStarfold::Beam& b : sf.beams())
        {
            LLVector3 a, z;
            if (!sf.beamEnd(b.mFrom, -1, a) || !sf.beamEnd(b.mTo, b.mRock, z)) continue;
            LLVector3 d = z - a;
            const F32 len = d.normVec();
            if (len < 0.5f) continue;
            // the colours (README "client look rules"); the aliens' violet pulses at 4 Hz so it differs by shape as well
            LLColor4 glow, core(1.f, 0.97f, 0.9f, 0.95f);
            F32 width = 1.6f;
            if (b.mKind == 'a')
            {
                const F32 pulse = 0.55f + 0.45f * sinf(F_TWO_PI * 4.f * t);
                glow = LLColor4(0.62f, 0.30f, 1.f, 0.85f * pulse);
                core.mV[VALPHA] = 0.9f * pulse;
                width = 1.9f;
            }
            else if (b.mKind == 'm')
            {
                glow = LLColor4(0.35f, 1.f, 0.70f, 0.6f);
                width = 1.2f;
            }
            else
            {
                glow = LLColor4(1.f, 0.62f, 0.15f, 0.85f);
            }
            const S32 n = llclamp((S32)ceilf(len / BEAM_SEG_M), 1, BEAM_SEGS_MAX);
            const F32 seg = len / (F32)n;
            const U32 flags = LLViewerPart::LL_PART_FOLLOW_VELOCITY_MASK | LLViewerPart::LL_PART_EMISSIVE_MASK;
            const LLVector3 eye = LLViewerCamera::getInstance()->getOrigin();
            for (S32 i = 0; i < n; ++i)
            {
                const LLVector3 c = a + d * (seg * ((F32)i + 0.5f));
                // [rig 24 Nov 16:58Z] a beam from our own ship starts at the camera's shoulder: there a 1.6 m glow filled half the
                // screen as a flat card. Near the eye the beam is no wider than ~3 % of its distance, and nothing within 3 m.
                const F32 fromEye = (c - eye).length();   // not "near": <windef.h> defines it
                if (fromEye < 3.f) continue;
                const F32 w = llmin(width, 0.03f * fromEye);
                part(c, d, life, LLVector2(w, seg * 1.08f), glow, 0.9f, flags);
                part(c, d, life, LLVector2(w * 0.18f, seg * 1.04f), core, 1.f, flags);
            }
        }
    }

private:
    // Source: wolffx.cpp WolfFXSource::part - one particle, additive (BF_SOURCE_ALPHA, BF_ONE), its glow written for the bloom
    void part(const LLVector3& pos, const LLVector3& dir, F32 life, const LLVector2& scale, const LLColor4& c, F32 glow, U32 flags)
    {
        if (!LLViewerPartSim::shouldAddPart()) return;
        LLViewerPart* p = new LLViewerPart();
        p->init(this, mImagep, NULL);
        p->mBlendFuncSource = LLRender::BF_SOURCE_ALPHA;
        p->mBlendFuncDest = LLRender::BF_ONE;
        p->mLastUpdateTime = 0.f;
        p->mPosAgent = pos;
        p->mVelocity = dir * 0.001f;   // FOLLOW_VELOCITY lays the sprite's length along it (llvopartgroup.cpp:533-566); it barely moves
        p->mAccel.setVec(0.f, 0.f, 0.f);
        p->mMaxAge = life;
        p->mScale = scale;
        p->mStartScale = scale;
        p->mEndScale = scale;
        p->mStartColor = c;
        p->mEndColor = c;
        p->mColor = c;
        p->mStartGlow = glow;
        p->mEndGlow = glow;
        p->mGlow = LLColor4U(0, 0, 0, (U8)(llclamp(glow, 0.f, 1.f) * 255.f));
        p->mFlags = flags;
        p->mParameter = 0.f;
        LLViewerPartSim::getInstance()->addPart(p);
    }
};

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// WolfStarfold
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════

WolfStarfold::WolfStarfold()
{
    if (!gGenericDispatcher.isHandlerPresent(MESSAGE_FROM_SIM))
    {
        gGenericDispatcher.addHandler(MESSAGE_FROM_SIM, &sHandler);
    }
}

WolfStarfold::~WolfStarfold()
{
    if (mBeamSource.notNull()) mBeamSource->setDead();
}

LLUUID WolfStarfold::agentRegionId() const
{
    LLViewerRegion* r = gAgent.getRegion();
    return r ? r->getRegionID() : LLUUID::null;
}

bool WolfStarfold::spaceHere() const
{
    return WolfGrid::showsWolfRegion() && mSys.mHave && mSys.mOn && mSys.mRegion == agentRegionId();
}

bool WolfStarfold::piloting() const
{
    return mPiloting && WolfGrid::isOnWolfTerritoriesConfirmed() && mSys.mRegion == agentRegionId();
}

F32 WolfStarfold::cameraAboveSea()
{
    // as wolfaltitudesky.cpp altitude(): the camera over the water, which is sea level on the grid
    return llmax(0.f, LLViewerCamera::getInstance()->getOrigin().mV[VZ] - LLEnvironment::instance().getWaterHeight());
}

F32 WolfStarfold::lineAboveSea() const
{
    if (!spaceHere()) return -1.f;
    return llmax(1.f, mSys.mHeight - LLEnvironment::instance().getWaterHeight());
}

F32 WolfStarfold::spaceFade() const
{
    const F32 ls = lineAboveSea();
    if (ls < 0.f) return 0.f;
    return smooth(ls - EDGE_M, ls, cameraAboveSea());
}

F32 WolfStarfold::skyAltitude(F32 a) const
{
    // README "client look rules", The air: a' = a + (50000 - a) s
    const F32 ls = lineAboveSea();
    if (ls < 0.f) return a;
    const F32 s = smooth(ls - EDGE_M, ls, a);
    return a + llmax(0.f, VACUUM_AT_M - a) * s;
}

F32 WolfStarfold::skyVacuum() const
{
    // v = 1 - exp(-a'/H) / exp(-a/H) = 1 - exp(-(a' - a) / H)
    const F32 a = cameraAboveSea();
    const F32 a2 = skyAltitude(a);
    if (a2 <= a) return 0.f;
    return llclamp(1.f - expf(-(a2 - a) / AIR_SCALE_HEIGHT_M), 0.f, 1.f);
}

F32 WolfStarfold::cloudCapAboveSea() const
{
    const F32 ls = lineAboveSea();
    return ls < 0.f ? -1.f : llmax(ls - CLOUD_GAP_M, 50.f);
}

F32 WolfStarfold::nebula(LLColor3& colour) const
{
    if (!spaceHere()) return 0.f;
    const Planet& p = planetOf(mSys.mSeed);
    colour = hsv(p.mNebulaHue, 0.65f, 1.f);
    return spaceFade();
}

void WolfStarfold::bindSkyUniforms(LLGLSLShader& shader, F32 lum, const LLVector3& sun_dir_world) const
{
    static LLStaticHashedString s_on("sf_planet_on"), s_dir("sf_planet_dir"), s_sin("sf_planet_sin"), s_c1("sf_planet_col1"),
        s_c2("sf_planet_col2"), s_gas("sf_planet_gas"), s_rings("sf_planet_rings"), s_rim("sf_planet_rim"), s_sun("sf_sun_dir"),
        s_lum("sf_planet_lum");
    const F32 fade = spaceFade();
    if (fade <= 0.f)
    {
        shader.uniform1f(s_on, 0.f);
        return;
    }
    const Planet& p = planetOf(mSys.mSeed);
    LLColor3 c1, c2;
    if (p.mGas)
    {
        c1 = hsv(p.mHue, 0.30f, 0.85f);
        c2 = hsv(p.mHue + 0.06f, 0.55f, 0.50f);
    }
    else
    {
        c1 = hsv(0.58f + 0.08f * (p.mHue - 0.5f), 0.70f, 0.30f);   // seas: always a blue, the hue nudging it
        c2 = hsv(p.mHue * 0.2f + 0.05f, 0.45f, 0.45f);              // land: ochre to green
    }
    const LLColor3 rim = p.mGas ? hsv(p.mHue, 0.35f, 1.f) : LLColor3(0.45f, 0.65f, 1.f);
    LLVector3 sun = sun_dir_world;
    if (sun.normVec() < 1e-4f) sun.set(0.f, 0.f, 1.f);
    shader.uniform1f(s_on, fade);
    shader.uniform3fv(s_dir, 1, p.mDir.mV);
    shader.uniform1f(s_sin, sinf(p.mRadiusRad));
    shader.uniform3fv(s_c1, 1, c1.mV);
    shader.uniform3fv(s_c2, 1, c2.mV);
    shader.uniform1f(s_gas, p.mGas ? 1.f : 0.f);
    shader.uniform1f(s_rings, p.mRings ? 1.f : 0.f);
    shader.uniform3fv(s_rim, 1, rim.mV);
    shader.uniform3fv(s_sun, 1, sun.mV);
    shader.uniform1f(s_lum, lum);
}

void WolfStarfold::bindSpaceSky(LLGLSLShader& shader, S32 unit)
{
    static LLStaticHashedString s_on("sf_space_on"), s_map("sf_space_map");
    const S32 loc = shader.getUniformLocation(s_map);
    if (loc < 0)
    {
        return;   // a program without the space (only the Wolf dome has it)
    }
    glUniform1i(loc, unit);
    // not in a reflection-probe face: the probes light the scene, and starlight is no light to see by (as the sun's disc there)
    const F32 fade = gCubeSnapshot ? 0.f : spaceFade();
    if (fade > 0.f && mSpaceMap.isNull())
    {
        // GAME TUNING: 4096 x 2048 (one texel 0.088 deg, a little over a screen pixel at 1080 lines and 60 deg): the stars stay
        // points, magnified rather than minified, so the map needs no mipmaps (and has no seam where a mip level would jump)
        LLPointer<LLImageRaw> raw = WolfSpaceSky::paintSphere(4096);
        if (raw.notNull())
        {
            mSpaceMap = LLViewerTextureManager::getLocalTexture(raw.get(), false);
            if (mSpaceMap.notNull())
            {
                mSpaceMap->setAddressMode(LLTexUnit::TAM_WRAP);   // round the sphere in longitude (the poles never sample past the edge)
                mSpaceMap->setFilteringOption(LLTexUnit::TFO_BILINEAR);
            }
        }
        LL_INFOS("WolfStarfold") << "space sky map " << (mSpaceMap.notNull() ? "made" : "NOT made") << LL_ENDL;
    }
    const bool have = fade > 0.f && mSpaceMap.notNull() && mSpaceMap->hasGLTexture();
    if (have)
    {
        gGL.getTexUnit(unit)->bind(mSpaceMap);
    }
    shader.uniform1f(s_on, have ? fade : 0.f);
}

F32 WolfStarfold::spaceStarFloor() const
{
    // GAME TUNING (README "client look rules", Paul 24 Nov: "i was expecting to be actually in space"): above the line the stars
    // are as bright by day as on a dark night, whatever the sunlit frame's exposure would make of their real light - the space
    // look, not a camera's. A star of magnitude -1 (the tint's reference: WolfEnvironmentRender::starTint) at this many of the
    // frame's units; the catalogue's fainter stars scale down from it in starsF.glsl as before.
    const F32 STAR_FLOOR = 6.f;
    return spaceFade() * STAR_FLOOR;
}

// ── the wire ─────────────────────────────────────────────────────────────────────────────────────────────────────────

void WolfStarfold::receive(const LLUUID& region_id, const std::vector<std::string>& strings)
{
    std::vector<std::pair<std::string, std::string>> kv;
    std::string kind;
    for (const std::string& s : strings)
    {
        const size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        kv.emplace_back(s.substr(0, eq), s.substr(eq + 1));
        if (kv.back().first == "k") kind = kv.back().second;
    }
    // Everything is about the region the avatar is in: SceneSpaceWorld.Broadcast / SendTo reach root agents only
    // (ForEachRootScenePresence, GetScenePresence + !IsChildAgent), and a neighbour's must never steer this one.
    if (region_id != agentRegionId()) return;
    if (kind == "sys") { parseSystem(region_id, kv); return; }
    if (!mSys.mHave || mSys.mRegion != region_id) return;
    if (kind == "st") parseStatus(kv);
    else if (kind == "beam") parseBeam(kv);
    else if (kind == "rock")
    {
        S32 i = -1; bool left = true;
        for (const auto& p : kv)
        {
            if (p.first == "i") i = (S32)num(p.second, -1.f);
            else if (p.first == "left") left = p.second == "1";
        }
        if (i >= 0 && i < (S32)mRocks.size() && mRocks[(size_t)i].mLeft != left)
        {
            mRocks[(size_t)i].mLeft = left;
            mRocksDirty = true;
        }
    }
    else if (kind == "boom")
    {
        // SpaceEngine.Disable / KillAlien: "p=x,y,z", "s=<size>" -> the WolfFX explosion (its own wire, wolffx.cpp receive)
        std::vector<std::string> fx{ "e=explosion" };
        for (const auto& p : kv) if (p.first == "p" || p.first == "s") fx.push_back(p.first + "=" + p.second);
        WolfFX::instance().receive(region_id, fx);
    }
    else if (kind == "docked") parseDocked(kv);
    else if (kind == "setup")
    {
        // Source: SpaceSetup.cs SendSetup: has=a,b,..., h=, on=0/1, next=<step|done>, t=<message> (when something was done)
        Setup su;
        su.mHave = true;
        for (const auto& p : kv)
        {
            if (p.first == "has") { for (const std::string& part : split(p.second, ',')) if (!part.empty() && su.mHas.size() < 8) su.mHas.push_back(clean(part).substr(0, 16)); }
            else if (p.first == "h") su.mHeight = num(p.second);
            else if (p.first == "on") su.mOn = p.second == "1";
            else if (p.first == "next") su.mNext = clean(p.second).substr(0, 16);
            else if (p.first == "t") addNote(clean(p.second));
        }
        mSetup = su;
    }
    else if (kind == "msg")
    {
        for (const auto& p : kv)
        {
            if (p.first != "t") continue;
            addNote(clean(p.second));
            WolfStarbase::instance().noteAnswer(clean(p.second));   // the Starbase window shows its answers until the next order
            // [WOLFTREK v11] a station rename's answer (SpaceNames.cs RenameStation / OnNameAnswer: Refuse = k=msg) stays in the
            // Station window until the next rename, never only a passing note
            if (stationShown() && now() - mStationAskedAt < ANSWER_FOR_S) mStationAnswer = clean(p.second);
        }
    }
    else if (kind == "bay") parseBay(kv);
    // [STARBASES 2026-11-24] k=bases, base, bpart, bghost, bown (SpaceBases.cs): wolfstarbase.cpp
    else if (WolfStarbase::instance().receive(kind, kv)) {}
    else if (kind == "end") endShip();
    else if (kind == "jump")
    {
        // SpaceEngine.Jump: the teleport follows (SceneSpaceWorld.SendToRegion); the tunnel holds until the region changes
        mJumpAt = now();
        mJumpFromRegion = region_id;
        mJumpTpSeen = false;
        mJumpEndAt = -1.0;
        for (const auto& p : kv) if (p.first == "to") addNote("Through the wormhole to " + clean(p.second) + "...");
    }
}

void WolfStarfold::parseSystem(const LLUUID& region_id, const std::vector<std::pair<std::string, std::string>>& kv)
{
    // Source: SpaceEngine.SendSystem: on, name (not used: the name is ours, gameName), h, seed, belt=c;r;w;n[;t], station=,
    // wh=pos;region. A field left out means none.
    System s;
    s.mHave = true;
    s.mRegion = region_id;
    for (const auto& p : kv)
    {
        const std::string& k = p.first;
        const std::string& v = p.second;
        if (k == "on") s.mOn = v == "1";
        else if (k == "h") s.mHeight = llclamp(num(v, 3000.f), 0.f, VACUUM_AT_M);
        else if (k == "seed")
        {
            const unsigned long long u = strtoull(v.c_str(), nullptr, 10);
            s.mSeed = (U32)(u & 0xffffffffull);
            if (s.mSeed == 0) s.mSeed = 1;   // BeltSeed never gives 0
        }
        else if (k == "belt")
        {
            const std::vector<std::string> f = split(v, ';');
            if (f.size() >= 4 && parseVec(f[0], s.mBeltCentre))
            {
                s.mHaveBelt = true;
                s.mBeltR = num(f[1]);
                s.mBeltW = num(f[2]);
                s.mBeltN = llclamp((S32)num(f[3]), 0, 2000);   // MakeBelt clamps n to 10..2000
                // [24 Nov, README] the thickness: the 5th field; the plugin's default (MakeBelt Num(kv, "t", 40)) when absent
                s.mBeltT = f.size() >= 5 ? llclamp(num(f[4], 40.f), 5.f, 1000.f) : 40.f;
            }
        }
        else if (k == "station") s.mHaveStation = parseVec(v, s.mStation);
        else if (k == "sn") s.mStationName = clean(v).substr(0, 40);   // NameFilter.NAME_MAX 32: a checked name, kept whole
        else if (k == "bay") s.mBay = v == "1";
        else if (k == "wh")
        {
            const std::vector<std::string> f = split(v, ';');
            if (f.size() >= 2 && parseVec(f[0], s.mWormhole))
            {
                s.mHaveWormhole = true;
                s.mWormholeTo = clean(f[1]).substr(0, 64);
            }
        }
    }
    const bool beltChanged = !mSys.mHave || mSys.mSeed != s.mSeed || mSys.mHaveBelt != s.mHaveBelt || mSys.mBeltN != s.mBeltN
        || mSys.mBeltR != s.mBeltR || mSys.mBeltW != s.mBeltW || mSys.mBeltT != s.mBeltT || mSys.mBeltCentre != s.mBeltCentre;
    mSys = s;
    mHelloRegion = region_id;
    if (beltChanged) makeBelt();
    if (!mSys.mOn)
    {
        mPiloting = false;
        mStation.mDocked = false;
        mBeams.clear();
    }
    LL_INFOS("WolfStarfold") << "system: on " << mSys.mOn << ", space from " << mSys.mHeight << " m, belt " << mSys.mHaveBelt << " ("
                             << mRocks.size() << " rocks), station " << mSys.mHaveStation << " '" << mSys.mStationName << "' bay "
                             << mSys.mBay << ", wormhole " << mSys.mHaveWormhole << LL_ENDL;
}

void WolfStarfold::makeBelt()
{
    // Source: wolf-space SpaceEngine.cs MakeBelt - the same draws in the same order, so every rock is where the region has it:
    // angle, radius offset, height, size (two draws multiplied), ore. r, w, t and n arrive already clamped by the region
    // (MakeBelt clamps them before it uses them and SendSystem sends what it used).
    mRocks.clear();
    mRocksDirty = true;
    if (!mSys.mHaveBelt) return;
    U32 s = mSys.mSeed;
    const F64 R = mSys.mBeltR, W = mSys.mBeltW, T = mSys.mBeltT;
    for (S32 i = 0; i < mSys.mBeltN; ++i)
    {
        const F64 a = next(s) * F_PI * 2.0, r = R + (next(s) - 0.5) * W, z = (next(s) - 0.5) * T;
        const F64 size = 2.0 + 7.0 * next(s) * next(s);
        const U8 ore = next(s) < 0.75 ? 0 : 1;
        Rock rock;
        rock.mPos = mSys.mBeltCentre + LLVector3((F32)(cos(a) * r), (F32)(sin(a) * r), (F32)z);
        rock.mSize = (F32)size;
        rock.mOre = ore;
        mRocks.push_back(rock);
    }
}

void WolfStarfold::parseStatus(const std::vector<std::pair<std::string, std::string>>& kv)
{
    // Source: SpaceEngine.Status: root, cls, sh, ar, hu, v, thr, m, air, hold=x/y, cr, safe, tg=root;m;sh;ar;hu, al=root,...
    if (!WolfGrid::isOnWolfTerritoriesConfirmed()) return;   // a visitor flies nothing (the region refuses them too: Launch)
    Ship s = mShip;
    s.mTarget.setNull();
    s.mAliens.clear();
    s.mAlienPos.clear();
    s.mHaveTargetPos = false;
    s.mSeq.clear();                      // seq= comes only while the automatic approach or departure flies (SpaceEngine.Status)
    std::vector<LLUUID> alRaw;           // al= and ap= as sent (index for index), paired up below
    std::vector<std::string> apRaw;
    bool haveThr = false;
    F32 thr = 0.f;
    std::string mode;
    for (const auto& p : kv)
    {
        const std::string& k = p.first;
        const std::string& v = p.second;
        if (k == "root") s.mRoot.set(v, false);
        else if (k == "cls") s.mClass = clean(v).substr(0, 32);
        else if (k == "sh") s.mShield = num(v);
        else if (k == "ar") s.mArmour = num(v);
        else if (k == "hu") s.mHull = num(v);
        else if (k == "v") s.mSpeed = num(v);
        else if (k == "thr") { thr = num(v); haveThr = true; }
        else if (k == "m") mode = v;
        else if (k == "air") s.mAir = v == "1";
        else if (k == "safe") s.mSafe = v == "1";
        else if (k == "cr") s.mCredits = (S64)strtoll(v.c_str(), nullptr, 10);
        else if (k == "hold")
        {
            const std::vector<std::string> f = split(v, '/');
            if (f.size() == 2) { s.mHold = num(f[0]); s.mHoldMax = num(f[1]); }
        }
        else if (k == "tg")
        {
            const std::vector<std::string> f = split(v, ';');
            if (f.size() >= 5 && s.mTarget.set(f[0], false))
            {
                s.mTargetM = num(f[1]);
                s.mTargetShield = num(f[2]);
                s.mTargetArmour = num(f[3]);
                s.mTargetHull = num(f[4]);
            }
        }
        else if (k == "al")
        {
            for (const std::string& g : split(v, ','))
            {
                LLUUID id;
                if (alRaw.size() < 6) { id.set(g, false); alRaw.push_back(id); }
            }
        }
        else if (k == "ap") apRaw = split(v, ';');                    // x,y,z;x,y,z;... in al='s order
        else if (k == "tgp") s.mHaveTargetPos = parseVec(v, s.mTargetPos);
        else if (k == "cid") s.mClassId = clean(v).substr(0, 32);
        else if (k == "seq" && (v == "in" || v == "out")) s.mSeq = v;
    }
    const bool pos = apRaw.size() == alRaw.size();
    for (size_t i = 0; i < alRaw.size(); ++i)
    {
        if (alRaw[i].isNull()) continue;
        LLVector3 at;
        if (pos && !parseVec(apRaw[i], at)) continue;   // a position that does not read: the pair is left out
        s.mAliens.push_back(alRaw[i]);
        if (pos) s.mAlienPos.push_back(at);
    }
    // [WOLFTREK v11 stage B] the class flown's full figures: the docked fleet= line for cid= (the region's own numbers), else the
    // class table below by cid= or cls=, else as stage 1 (the Cub's 100s); never below what the ship has now
    F32 fs = 100.f, fa = 100.f, fh = 100.f;
    if (const ShipInfo* fi = fleetClass(mFleet, s.mClassId, s.mClass)) { fs = fi->mShield; fa = fi->mArmour; fh = fi->mHull; }
    else
    {
        // Source: wolf-space SpaceRules.cs:56-101 Cub / Lurcher / Runner / Haulbear / Warden (Shield, Armour, Hull)
        static const struct { const char* id; const char* title; F32 sh, ar, hu; } TABLE[] = {
            { "cub", "Cub", 100.f, 100.f, 100.f }, { "lurcher", "Lurcher", 90.f, 70.f, 80.f }, { "runner", "Runner", 110.f, 60.f, 90.f },
            { "haulbear", "Haulbear", 120.f, 200.f, 180.f }, { "warden", "Warden", 180.f, 220.f, 200.f } };
        for (const auto& c : TABLE)
            if (s.mClassId == c.id || (s.mClassId.empty() && s.mClass == c.title)) { fs = c.sh; fa = c.ar; fh = c.hu; break; }
    }
    s.mShieldMax = llmax(fs, s.mShield);
    s.mArmourMax = llmax(fa, s.mArmour);
    s.mHullMax = llmax(fh, s.mHull);
    const bool first = !mPiloting;
    const bool wasAuto = !first && !mShip.mSeq.empty();
    mShip = s;
    mLastShip = s.mRoot;
    mLastStatusAt = now();
    if (first && (mHangarOn || mHangarWaitUntil > 0.0)) endHangar("the pilot launched");
    if (!wasAuto && !mShip.mSeq.empty())
    {
        // the bay's automatic approach or departure began: whatever the pilot held is let go here (the region ignores it anyway:
        // SpaceEngine.OnChat returns for every verb but leave and a STOP during an approach)
        releaseKeys();
        mFlyTo.clear();
        LL_INFOS("WolfStarfold") << "automatic " << (mShip.mSeq == "in" ? "docking" : "departure") << " (pad " << mBayPad << ")" << LL_ENDL;
    }
    else if (wasAuto && mShip.mSeq.empty())
    {
        LL_INFOS("WolfStarfold") << "automatic flight over: the pilot has the controls" << LL_ENDL;
    }
    if (first)
    {
        mPiloting = true;
        mStation.mDocked = false;     // a ship out means the pilot left the station (SpaceEngine.Launch: Docked.Remove)
        mHidden = false;
        mByHand = false;
        mFlyTo.clear();
        for (F32& v : mSent) v = -9.f;
        mThrottle = haveThr ? thr : 0.f;
        mDrift = mode == "d";
        claimBottom();
        static LLCachedControl<bool> guide_seen(gSavedSettings, "WolfStarfoldGuideSeen", false);
        if (!guide_seen) showGuide(true);
        LL_INFOS("WolfStarfold") << "flying a " << mShip.mClass << " (" << mShip.mRoot << ")" << LL_ENDL;
    }
    else
    {
        // The region's own idea of the throttle and the mode (an autopilot arrival sets the throttle to 0: SpaceEngine.StepShip)
        // while the pilot is not working them: the lever then shows what the ship does.
        const bool keys = mKeyHeld[A_THR_UP] || mKeyHeld[A_THR_DOWN];
        if (haveThr && !keys && now() - mSentAt > 1.5 && now() - mThrottleKeyAt > 1.5) mThrottle = thr;
        if (!mode.empty() && now() - mSentAt > 1.5) mDrift = mode == "d";
    }
    // The region stops firing when the target is gone (SpaceEngine.KillAlien / Fire: FireAt = Empty): so does the button.
    if (mFireWanted && mShip.mTarget.isNull() && now() - mSentAt > 1.0) mFireWanted = false;
    // "Fly there" ends where the region stopped the ship (StepShip: the goal within 30 m, Vel = 0)
    if (!mFlyTo.empty() && mShip.mSpeed < 0.5f && now() - mSentAt > 5.0) mFlyTo.clear();
}

void WolfStarfold::parseBeam(const std::vector<std::pair<std::string, std::string>>& kv)
{
    // Source: SpaceEngine.Beam: a=<shooter root>, b=<target root> (Empty when off), on=1/0, kind=p/a/m. A mining beam's b is
    // new Guid(rockIndex, 0, 0, byte[8]): its first field (the first 8 hex digits) is the rock's index (SpaceEngine.Mine).
    Beam b;
    bool on = false;
    std::string bs;
    for (const auto& p : kv)
    {
        if (p.first == "a") b.mFrom.set(p.second, false);
        else if (p.first == "b") { b.mTo.set(p.second, false); bs = p.second; }
        else if (p.first == "on") on = p.second == "1";
        else if (p.first == "kind" && !p.second.empty()) b.mKind = p.second[0];
    }
    if (b.mFrom.isNull() || (b.mKind != 'p' && b.mKind != 'a' && b.mKind != 'm')) return;
    if (b.mKind == 'm' && bs.size() >= 8)
    {
        const long idx = strtol(bs.substr(0, 8).c_str(), nullptr, 16);
        b.mRock = (idx >= 0 && idx < (long)mRocks.size()) ? (S32)idx : -1;
    }
    const char kind = b.mKind;
    const LLUUID from = b.mFrom;
    mBeams.erase(std::remove_if(mBeams.begin(), mBeams.end(), [&](const Beam& x) { return x.mFrom == from && x.mKind == kind; }), mBeams.end());
    if (!on || (b.mKind != 'm' && b.mTo.isNull()) || (b.mKind == 'm' && b.mRock < 0)) return;
    b.mSince = now();
    mBeams.push_back(b);
    if (mBeams.size() > 64) mBeams.erase(mBeams.begin());
    // sounds as the beam starts: our laser, or the hit on us (WolfStarfold::sounds keeps a held hit going)
    if (gAudiop && mSoundState > 0)
    {
        if (b.mKind == 'p' && b.mFrom == mShip.mRoot && piloting())
            play(SOUND_LASER, 0.6f);
    }
}

void WolfStarfold::parseDocked(const std::vector<std::pair<std::string, std::string>>& kv)
{
    // Source: SpaceEngine.SendDocked: cr, then ore=id;title;price;held per ore (SpaceRules.Ores)
    Station st;
    st.mDocked = true;
    for (const auto& p : kv)
    {
        if (p.first == "cr") st.mCredits = (S64)strtoll(p.second.c_str(), nullptr, 10);
        else if (p.first == "sn") st.mName = clean(p.second).substr(0, 40);
        else if (p.first == "ship") st.mShip.set(p.second, false);
        else if (p.first == "pad") st.mPad = (S32)num(p.second, -1.f);
        else if (p.first == "fleet")
        {
            ShipInfo c;
            if (parseFleet(p.second, c) && st.mFleet.size() < 16
                && std::none_of(st.mFleet.begin(), st.mFleet.end(), [&](const ShipInfo& x) { return x.mId == c.mId; }))
                st.mFleet.push_back(c);
        }
        else if (p.first == "ore")
        {
            const std::vector<std::string> f = split(p.second, ';');
            if (f.size() >= 4 && st.mOres.size() < 8)
            {
                Ore o;
                o.mId = clean(f[0]).substr(0, 32);
                o.mTitle = clean(f[1]).substr(0, 40);
                o.mPrice = (S32)num(f[2]);
                o.mHeld = num(f[3]);
                st.mOres.push_back(o);
            }
        }
    }
    const bool fresh = !mStation.mDocked;
    mStation = st;
    if (!st.mFleet.empty()) mFleet = st.mFleet;   // kept while flying (the gauges)
    if (fresh)
    {
        mStationShown = true;
        mStationAnswer.clear();
    }
    if (WolfGrid::isOnWolfTerritoriesConfirmed()) claimBottom();
    // [WOLFTREK v11] the pilot's ship stands on its pad (SpaceEngine.SendDocked ship=, pad=): frame it (once per docking)
    if (st.mShip.notNull()) startHangar(st.mShip);
}

void WolfStarfold::parseBay(const std::vector<std::pair<std::string, std::string>>& kv)
{
    // Source: SpaceBay.cs - to the pilot: StartSeq "ph=in|out", "pad=<n>", "ship=<root>", "s=<seconds>"; CancelSeq "ph=stop",
    // "ship="; to everyone: EndSeq / BayRescue "ph=landed", "pad=", "ship=", "at=x,y,z" (the pad's top centre), "yaw=<deg>"; Sink
    // "ph=lift", "ship=", "pad=". A pad's number is the one painted on it (bay_pad_<n>); 0 is the lift.
    std::string ph;
    S32 pad = -1;
    LLUUID ship;
    LLVector3 at;
    bool haveAt = false;
    F32 yaw = 0.f;
    for (const auto& p : kv)
    {
        if (p.first == "ph") ph = p.second;
        else if (p.first == "pad") pad = (S32)num(p.second, -1.f);
        else if (p.first == "ship") ship.set(p.second, false);
        else if (p.first == "at") haveAt = parseVec(p.second, at);
        else if (p.first == "yaw") yaw = num(p.second);
    }
    if (ph == "in" || ph == "out")
    {
        if (!WolfGrid::isOnWolfTerritoriesConfirmed()) return;
        mBayPad = pad;
        LL_INFOS("WolfStarfold") << "bay: automatic " << (ph == "in" ? "approach" : "departure") << ", pad " << pad << LL_ENDL;
    }
    else if (ph == "stop")
    {
        // CancelSeq: the ship halts and the pilot has the controls (the next k=st has no seq=); the banner goes now
        mBayPad = -1;
        if (ship.isNull() || ship == mShip.mRoot) mShip.mSeq.clear();
        LL_INFOS("WolfStarfold") << "bay: approach stopped" << LL_ENDL;
    }
    else if (ph == "landed")
    {
        // everyone hears it; only this avatar's own ship is framed (the ship it last flew here). The pad's place is logged for a
        // rig (a camera on the walkway, the sign over the mouth)
        if (haveAt) LL_INFOS("WolfStarfold") << "bay: ship " << ship << " landed on pad " << pad << " at " << at << " yaw " << yaw << LL_ENDL;
        if (ship.notNull() && ship == mLastShip) startHangar(ship);
    }
    else if (ph == "lift")
    {
        // the ship goes down through its pad and is removed (SpaceBay.cs Sink): the view of it ends
        if (ship.notNull() && ship == mHangarShip) endHangar("the ship went down the lift");
    }
}

void WolfStarfold::addNote(const std::string& text)
{
    if (text.empty()) return;
    mNotes.push_back({ text, now() });
    while (mNotes.size() > 4) mNotes.pop_front();
    LL_INFOS("WolfStarfold") << "region says: " << text << LL_ENDL;
}

void WolfStarfold::endShip()
{
    // SpaceEngine.EndShip: the ship is gone (docked, disabled, left, the edge of a jump)
    mPiloting = false;
    releaseKeys();
    mFlyTo.clear();
    mThrottle = 0.f;
    mShip.mSeq.clear();
    mBayPad = -1;
    LL_INFOS("WolfStarfold") << "ship ended" << LL_ENDL;
}

void WolfStarfold::releaseKeys()
{
    mFireWanted = false;
    mMineWanted = false;
    mKeyPitch = mKeyYaw = mKeyRoll = mKeySlideY = mKeySlideZ = 0.f;
    mPointerHeld = false;
    mPointerPitch = mPointerYaw = 0.f;
    for (bool& b : mKeyHeld) b = false;
}

void WolfStarfold::resetRegion()
{
    mSys = System();
    mRocks.clear();
    mRocksDirty = true;
    mBeams.clear();
    mStation = Station();
    mSetup = Setup();
    mSetupShown = false;
    mStationAnswer.clear();
    mFleet.clear();
    mBayPad = -1;
    if (mHangarOn || mHangarWaitUntil > 0.0) endHangar("a new region");
    mHangarDoneFor.setNull();
    WolfStarbase::instance().reset();
    WolfStarbase::instance().show(false);
    if (mPiloting) endShip();
    mByHand = false;
}

void WolfStarfold::claimBottom()
{
    // Source: wolftraincab.cpp WolfTrain::claimBottom (itself wolfhelm.cpp WolfHelm::claimBottom) - one bottom panel at a time:
    // the dashboard closes, the flight / sailing deck is hidden (its mode keeps running), the Captain's Helm closes.
    if (WolfDrive::instance().dashboardOn()) WolfDrive::instance().requestDashboard(false);
    WolfFlight& wf = WolfFlight::instance();
    if (wf.active() && !wf.deckHidden()) wf.toggleDeck(wf.sailing());
    if (WolfHelm::instance().hudShowing()) WolfHelm::instance().showHud(false);
}

// ── sending ──────────────────────────────────────────────────────────────────────────────────────────────────────────

bool WolfStarfold::sendAllowed(bool control)
{
    const F64 t = now();
    while (!mSendTimes.empty() && t - mSendTimes.front() >= 1.0) mSendTimes.pop_front();
    const S32 cap = control ? MAX_CONTROLS_PER_S : MAX_INTENTS_PER_S - 2;
    return (S32)mSendTimes.size() < cap;
}

void WolfStarfold::say(const std::string& verb_and_args, bool control)
{
    // As WolfAir::send: to the region only once it has itself said it is Wolf, never on a foreign grid, never as a visitor.
    if (!WolfGrid::isOnWolfTerritoriesConfirmed()) return;
    if (!sendAllowed(control))
    {
        if (!control) addNote("Too many orders at once - please press that again in a moment.");
        return;
    }
    mSendTimes.push_back(now());
    const std::string text = PREFIX + verb_and_args;
    // Said, not shouted: the region's plugin hears every avatar's chat on its channel wherever they stand (the host's chat
    // event, WolfPluginHost SceneBridge.cs OnChatFromClient), and a channel other than 0 is never shown to anyone.
    FSNearbyChat::sendChatFromViewerFinal(text, text, CHAT_TYPE_NORMAL, false, CHANNEL);
}

void WolfStarfold::intent(const std::string& verb_and_args)
{
    // the Starbase window's orders (wolfstarbase.cpp): the same path, limits and checks as the cockpit's own
    say(verb_and_args);
}

bool WolfStarfold::canLaunch() const
{
    return spaceHere() && WolfGrid::isOnWolfTerritoriesConfirmed() && !mPiloting;
}

void WolfStarfold::launch()
{
    if (!canLaunch()) return;
    say("launch");
}

void WolfStarfold::goTo(const std::string& where)
{
    // Source: wolf-space 1.0.4 (README "goto"): SF1|goto|to=pad or to=station; at most once every 5 s per avatar there
    if (!canLaunch() || (where != "pad" && where != "station")) return;
    say("goto|to=" + where);
}

void WolfStarfold::fire(bool on)
{
    if (!piloting()) return;
    mFireWanted = on;
    if (!on) { say("fire|on=0"); return; }
    // the pilot's pick on the radar, while it is still a Glasswing near; else the region's target, else the nearest
    LLUUID tg;
    if (mSelected.notNull() && std::find(mShip.mAliens.begin(), mShip.mAliens.end(), mSelected) != mShip.mAliens.end()) tg = mSelected;
    else if (mShip.mTarget.notNull()) tg = mShip.mTarget;
    say(tg.notNull() ? "fire|on=1|tg=" + tg.asString() : std::string("fire|on=1"));
}

void WolfStarfold::nextTarget()
{
    if (!piloting()) return;
    mSelected.setNull();
    say("target");
}

bool WolfStarfold::mining() const
{
    for (const Beam& b : mBeams) if (b.mKind == 'm' && b.mFrom == mShip.mRoot) return true;
    return mMineWanted && now() - mSentAt < 1.5;
}

void WolfStarfold::mine(bool on)
{
    if (!piloting()) return;
    mMineWanted = on;
    say(on ? "mine|on=1" : "mine|on=0");
}

void WolfStarfold::flyTo(const std::string& where)
{
    if (!piloting()) return;
    // "base": the pilot's own starbase here; "base:<id>": any base here, by the first 8+ hex digits of its id (SpaceEngine.cs "fly"
    // -> SpaceBases.cs FlyToBase, wolf-space 1.0.7)
    bool baseId = where.size() >= 13 && where.size() <= 37 && where.compare(0, 5, "base:") == 0;
    for (size_t i = 5; baseId && i < where.size(); ++i) baseId = isxdigit((unsigned char)where[i]) != 0;
    if (where != "station" && where != "belt" && where != "wormhole" && where != "stop" && where != "base" && !baseId) return;
    mFlyTo = where == "stop" ? std::string() : where;
    say("fly|to=" + where);
    mSentAt = now();
}

void WolfStarfold::dock()
{
    if (piloting()) say("dock");
}

void WolfStarfold::sell()
{
    if (mStation.mDocked) say("sell");
}

void WolfStarfold::leave()
{
    if (piloting()) say("leave");
}

void WolfStarfold::setDrift(bool drift)
{
    mDrift = drift;
    if (!mDrift && mThrottle < 0.f) mThrottle = 0.f;   // SpaceEngine.OnChat: Steady has no reverse
    sendControls();
}

void WolfStarfold::setThrottle(F32 t)
{
    mThrottle = llclamp(t, mDrift ? -1.f : 0.f, 1.f);
    mThrottleKeyAt = now();
}

void WolfStarfold::setPointer(bool held, F32 pitch, F32 yaw)
{
    mPointerHeld = held;
    mPointerPitch = held ? llclamp(pitch, -1.f, 1.f) : 0.f;
    mPointerYaw = held ? llclamp(yaw, -1.f, 1.f) : 0.f;
}

void WolfStarfold::sendControls()
{
    if (!piloting()) return;
    if (automatic()) return;   // [WOLFTREK v11] the region flies the bay's approach and ignores ctl meanwhile: nothing is sent
    // Source: SpaceEngine.OnChat "ctl": t (kept when absent), p, y, r, sy, sz (0 when absent), m=s/d; each clamped to -1..1
    // there (ShipInput.Clamped) and a Steady throttle below 0 taken as 0. We send all of them, rounded, only on a change.
    auto q = [](F32 v, F32 step) { return llclamp(roundf(v / step) * step, -1.f, 1.f); };
    const F32 v[7] = {
        q(mThrottle, 0.02f),
        q(mKeyPitch + mPointerPitch, 0.05f),
        q(mKeyYaw + mPointerYaw, 0.05f),
        q(mKeyRoll, 0.05f),
        q(mKeySlideY, 0.05f),
        q(mKeySlideZ, 0.05f),
        mDrift ? 1.f : 0.f,
    };
    bool changed = false;
    for (S32 i = 0; i < 7; ++i) if (fabsf(v[i] - mSent[i]) > 0.001f) changed = true;
    if (!changed || now() - mSentAt < CONTROL_MIN_GAP_S || !sendAllowed(true)) return;
    for (S32 i = 0; i < 7; ++i) mSent[i] = v[i];
    mSentAt = now();
    if (fabsf(v[1]) + fabsf(v[2]) + fabsf(v[3]) > 0.f) mFlyTo.clear();   // the pilot takes over (SpaceEngine.OnChat: AutoTo = null)
    say(llformat("ctl|t=%.2f|p=%.2f|y=%.2f|r=%.2f|sy=%.2f|sz=%.2f|m=%s", v[0], v[1], v[2], v[3], v[4], v[5], mDrift ? "d" : "s"), true);
}

// ── the keys ─────────────────────────────────────────────────────────────────────────────────────────────────────────

bool WolfStarfold::noteScanKey(KEY key, MASK mask, bool key_down, bool key_up, bool key_level, bool repeat)
{
    // [WOLF ONLY] off Wolf, or not piloting a ship here, no key is watched or taken at all
    if (!piloting() || (mask != MASK_NONE && mask != MASK_SHIFT)) return false;
    if (automatic())
    {
        // [WOLFTREK v11] the automatic approach or departure: the cockpit's keys are still the cockpit's (they would swing the
        // camera round the seat), but they do nothing and send nothing (design.html#bay "The keys do nothing during the approach")
        return key == 'W' || key == 'S' || key == KEY_PAGE_UP || key == KEY_PAGE_DOWN || key == KEY_UP || key == KEY_DOWN || key == KEY_LEFT
            || key == KEY_RIGHT || key == 'Q' || key == 'E' || key == ' ' || key == KEY_TAB || key == 'Z' || key == 'M';
    }
    // Source: design.html "Controls": Page Up / Page Down or W / S the throttle; the arrows pitch and yaw; Q / E roll; Shift +
    // the arrows slide; Space fire; Tab the next target; Z Steady / Drift. Plus M mine (the MINE button's key).
    const bool held = key_level || (key_down && !key_up);
    const bool shift = mask == MASK_SHIFT;
    static LLCachedControl<bool> invert(gSavedSettings, "WolfStarfoldInvertPitch", false);
    S32 act = -1;
    if (!shift)
    {
        if (key == 'W' || key == KEY_PAGE_UP) act = A_THR_UP;
        else if (key == 'S' || key == KEY_PAGE_DOWN) act = A_THR_DOWN;
        else if (key == KEY_UP) act = invert ? A_PITCH_DOWN : A_PITCH_UP;
        else if (key == KEY_DOWN) act = invert ? A_PITCH_UP : A_PITCH_DOWN;
        else if (key == KEY_LEFT) act = A_YAW_LEFT;
        else if (key == KEY_RIGHT) act = A_YAW_RIGHT;
        else if (key == 'Q') act = A_ROLL_LEFT;
        else if (key == 'E') act = A_ROLL_RIGHT;
        else if (key == ' ') act = A_FIRE;
        else if (key == KEY_TAB) { if (key_down && !repeat) nextTarget(); return true; }
        else if (key == 'Z') { if (key_down && !repeat) setDrift(!mDrift); return true; }
        else if (key == 'M') { if (key_down && !repeat) mine(!mining()); return true; }
    }
    else
    {
        if (key == KEY_LEFT) act = A_SLIDE_LEFT;
        else if (key == KEY_RIGHT) act = A_SLIDE_RIGHT;
        else if (key == KEY_UP) act = A_SLIDE_UP;
        else if (key == KEY_DOWN) act = A_SLIDE_DOWN;
    }
    if (act < 0) return false;
    const bool was = mKeyHeld[act];
    mKeyHeld[act] = held;
    mKeyOf[act] = key;      // the key that holds it (idle lets it go when that key is no longer down)
    if (act == A_FIRE && held != was) fire(held);
    mKeyPitch = (mKeyHeld[A_PITCH_UP] ? 1.f : 0.f) - (mKeyHeld[A_PITCH_DOWN] ? 1.f : 0.f);
    mKeyYaw = (mKeyHeld[A_YAW_LEFT] ? 1.f : 0.f) - (mKeyHeld[A_YAW_RIGHT] ? 1.f : 0.f);       // + = left (ShipModel: about +Z)
    mKeyRoll = (mKeyHeld[A_ROLL_RIGHT] ? 1.f : 0.f) - (mKeyHeld[A_ROLL_LEFT] ? 1.f : 0.f);
    mKeySlideY = (mKeyHeld[A_SLIDE_LEFT] ? 1.f : 0.f) - (mKeyHeld[A_SLIDE_RIGHT] ? 1.f : 0.f);  // +Y = the ship's left
    mKeySlideZ = (mKeyHeld[A_SLIDE_UP] ? 1.f : 0.f) - (mKeyHeld[A_SLIDE_DOWN] ? 1.f : 0.f);
    return true;
}

// ── once a frame ─────────────────────────────────────────────────────────────────────────────────────────────────────

F32 WolfStarfold::tunnel() const
{
    const F64 t = now() - mJumpAt;
    if (t < 0.0 || t > JUMP_HOLD_S + JUMP_FADE_S) return 0.f;
    const F32 in = (F32)llmin(1.0, t / 0.4);
    // out once the region has changed (the teleport landed), or after the hold
    const bool landed = agentRegionId() != mJumpFromRegion;
    if (landed) return 0.f;
    // [24 Nov, WolfStorm's live run] a jump the region could not complete ("The wormhole is closed just now") starts no
    // teleport, or one that fails: then the tunnel goes at once (idle sets mJumpEndAt) instead of holding to the cap
    if (mJumpEndAt >= 0.0) return in * (F32)llmax(0.0, 1.0 - (now() - mJumpEndAt) / 0.6);
    if (t > JUMP_HOLD_S) return in * (F32)llmax(0.0, 1.0 - (t - JUMP_HOLD_S) / JUMP_FADE_S);
    return in;
}

void WolfStarfold::sounds()
{
    if (!gAudiop || !spaceHere()) return;
    if (mSoundState == 0)
    {
        mSoundState = writeSounds() ? 1 : -1;
        if (mSoundState < 0) LL_WARNS("WolfStarfold") << "the Starfold sounds could not be written to the sound cache" << LL_ENDL;
    }
    if (mSoundState < 0) return;
    const F64 t = now();
    // a Glasswing's beam on us: a hit every 0.6 s while it holds, on the shield while there is one, else on the armour
    if (piloting() && t - mHitAt > 0.6)
    {
        for (const Beam& b : mBeams)
        {
            if (b.mKind == 'a' && b.mTo == mShip.mRoot)
            {
                mHitAt = t;
                play(mShip.mShield > 0.5f ? SOUND_SHIELD : SOUND_ARMOUR, 0.55f);
                break;
            }
        }
    }
    // the wormhole's hum within 400 m of it, louder nearer (the 3 s sound faded at both ends, played again as it ends)
    if (mSys.mHaveWormhole && t - mHumAt > 2.9)
    {
        LLViewerRegion* r = gAgent.getRegion();
        if (r)
        {
            const F32 d = (r->getPosAgentFromRegion(mSys.mWormhole) - LLViewerCamera::getInstance()->getOrigin()).length();
            if (d < 400.f)
            {
                mHumAt = t;
                play(SOUND_HUM, 0.15f + 0.55f * (1.f - d / 400.f));
            }
        }
    }
}

void WolfStarfold::idle()
{
    const LLUUID here = agentRegionId();
    if (here != mLastRegion)
    {
        mLastRegion = here;
        mArrivedAt = now();
        if (mSys.mRegion != here) resetRegion();
        mHelloRegion.setNull();
    }
    if (!WolfGrid::showsWolfRegion())
    {
        if (mSys.mHave) resetRegion();
        return;
    }
    // Ask for the system when the region has not said it since we arrived (SpaceEngine.OnArrive sends it by itself to every
    // Wolf viewer that arrives; this covers a viewer that missed it). Once, and once more later; never as a visitor.
    if (here.notNull() && mHelloRegion != here && WolfGrid::isOnWolfTerritoriesConfirmed())
    {
        const F64 since = now() - mArrivedAt;
        if (since > HELLO_AFTER_S && (mAskedRegion != here || (mAsks < 2 && now() - mAskedAt > HELLO_RETRY_S)))
        {
            if (mAskedRegion != here) mAsks = 0;
            mAskedRegion = here;
            mAskedAt = now();
            ++mAsks;
            say("hello");
        }
    }
    // the ship: gone when the region stops talking about it for long (a crash, a lost message)
    if (mPiloting && now() - mLastStatusAt > 10.0) endShip();
    if (mPiloting)
    {
        // the throttle keys move the lever while they are held
        const F32 dt = llclamp((F32)gFrameIntervalSeconds, 0.f, 0.25f);
        if (mKeyHeld[A_THR_UP] || mKeyHeld[A_THR_DOWN])
        {
            const F32 d = (mKeyHeld[A_THR_UP] ? 1.f : 0.f) - (mKeyHeld[A_THR_DOWN] ? 1.f : 0.f);
            setThrottle(mThrottle + d * THROTTLE_KEY_RATE * dt);
        }
        // a key the scan no longer reports as down has gone up (focus taken by a window, the key released elsewhere)
        if (gKeyboard)
        {
            for (S32 a = 0; a < A_COUNT; ++a)
            {
                if (mKeyHeld[a] && !gKeyboard->getKeyDown(mKeyOf[a]))
                {
                    mKeyHeld[a] = false;
                    if (a == A_FIRE) fire(false);
                }
            }
            mKeyPitch = (mKeyHeld[A_PITCH_UP] ? 1.f : 0.f) - (mKeyHeld[A_PITCH_DOWN] ? 1.f : 0.f);
            mKeyYaw = (mKeyHeld[A_YAW_LEFT] ? 1.f : 0.f) - (mKeyHeld[A_YAW_RIGHT] ? 1.f : 0.f);
            mKeyRoll = (mKeyHeld[A_ROLL_RIGHT] ? 1.f : 0.f) - (mKeyHeld[A_ROLL_LEFT] ? 1.f : 0.f);
            mKeySlideY = (mKeyHeld[A_SLIDE_LEFT] ? 1.f : 0.f) - (mKeyHeld[A_SLIDE_RIGHT] ? 1.f : 0.f);
            mKeySlideZ = (mKeyHeld[A_SLIDE_UP] ? 1.f : 0.f) - (mKeyHeld[A_SLIDE_DOWN] ? 1.f : 0.f);
        }
        sendControls();
    }
    // the jump's tunnel: did a teleport begin, and did it end without landing? (tunnel)
    if (mJumpAt > 0.0 && mJumpEndAt < 0.0 && agentRegionId() == mJumpFromRegion)
    {
        const bool tp = gAgent.getTeleportState() != LLAgent::TELEPORT_NONE;
        if (tp) mJumpTpSeen = true;
        const F64 t = now() - mJumpAt;
        if ((!mJumpTpSeen && t > 6.0) || (mJumpTpSeen && !tp)) mJumpEndAt = now();
    }
    // notes go after a while
    while (!mNotes.empty() && now() - mNotes.front().mAt > NOTE_SHOW_S) mNotes.pop_front();
    // the beams' particle source, alive while space is here
    if (spaceHere() && !mBeams.empty())
    {
        if (mBeamSource.isNull() || mBeamSource->isDead())
        {
            mBeamSource = new WolfStarfoldBeamSource();
            LLViewerPartSim::getInstance()->addPartSource(mBeamSource);
        }
    }
    else if (mBeamSource.notNull())
    {
        mBeamSource->setDead();
        mBeamSource = nullptr;
    }
    // a beam whose shooter has gone (deleted without its "off": a region restart) is dropped after a while
    for (auto it = mBeams.begin(); it != mBeams.end();)
    {
        LLVector3 p;
        if (now() - it->mSince > 5.0 && !beamEnd(it->mFrom, -1, p)) it = mBeams.erase(it);
        else ++it;
    }
    WolfStarbase::instance().idle();
    hangarIdle();
    sounds();
}

// ── the setup helper ─────────────────────────────────────────────────────────────────────────────────────────────────

bool WolfStarfold::Setup::has(const std::string& part) const
{
    return std::find(mHas.begin(), mHas.end(), part) != mHas.end();
}

bool WolfStarfold::canSetUp() const
{
    // the region answered (so it has the plugin) and this avatar manages the estate (the region checks again: SpaceSetup.cs Setup
    // W.IsRegionOwner, the estate owner or a manager)
    LLViewerRegion* r = gAgent.getRegion();
    return WolfGrid::isOnWolfTerritoriesConfirmed() && mSys.mHave && r && mSys.mRegion == r->getRegionID() && r->canManageEstate();
}

void WolfStarfold::toggleSetup()
{
    if (!canSetUp()) return;
    mSetupShown = !mSetupShown;
    if (mSetupShown) say("setup|status=1");
}

void WolfStarfold::setupRez(const std::string& part)
{
    if (!canSetUp() || (part != "station" && part != "belt" && part != "wormhole" && part != "pad")) return;
    say("setup|rez=" + part);
}

void WolfStarfold::setupClear()
{
    if (canSetUp()) say("setup|clear=1");
}

// ── [WOLFTREK v11] names and the bay ─────────────────────────────────────────────────────────────────────────────────

void WolfStarfold::stopApproach()
{
    // Source: SpaceEngine.OnChat - during a Seq only "leave" and, while it is an approach (Seq.In), fly|to=stop (CancelSeq) act
    if (automaticIn()) say("fly|to=stop");
}

const std::string& WolfStarfold::stationName() const
{
    // k=sys sn= first: a rename sends the system to everyone again (SpaceNames.cs SetStationName -> SendSystem) but not k=docked;
    // k=docked sn= (SpaceEngine.SendDocked) when the system has not named it
    return mSys.mStationName.empty() ? mStation.mName : mSys.mStationName;
}

bool WolfStarfold::canRenameStation() const
{
    // the setup helper's rule (canSetUp): the region checks again (SpaceNames.cs RenameStation: W.IsRegionOwner, the estate owner
    // or a manager) and says plainly when it refuses
    LLViewerRegion* r = gAgent.getRegion();
    return WolfGrid::isOnWolfTerritoriesConfirmed() && spaceHere() && mSys.mHaveStation && r && r->canManageEstate();
}

void WolfStarfold::renameStation(const std::string& name)
{
    std::string why;
    const std::string tidy = checkName(name, why);
    if (!canRenameStation() || tidy.empty()) return;
    // Source: SpaceEngine.OnChat verb "name": kv "station" -> RenameStation (SpaceNames.cs); the answer is a k=msg (Refuse)
    mStationAnswer = "Renaming your station...";
    mStationAskedAt = now();
    say("name|station=" + tidy);
}

void WolfStarfold::buyShip(const std::string& id)
{
    // Source: SpaceFleet.cs ShipVerb - docked at a station (or a starbase); the answer is a k=msg (Refuse / Say), then a fresh k=docked
    if (!mStation.mDocked || !idToken(id)) return;
    mStationAnswer = "Asking the shipyard...";
    mStationAskedAt = now();
    say("ship|buy=" + id);
}

void WolfStarfold::useShip(const std::string& id)
{
    if (!mStation.mDocked || !idToken(id)) return;
    mStationAnswer = "Asking the shipyard...";
    mStationAskedAt = now();
    say("ship|use=" + id);
}

// static
std::string WolfStarfold::checkName(const std::string& raw, std::string& why)
{
    // Source: NameFilter.cs Check (NAME_MIN 3, NAME_MAX 32; the same words for each refusal)
    why.clear();
    LLWString w = utf8str_to_wstring(raw);
    LLWString shown;
    bool space = false;
    for (llwchar c : w)
    {
        const bool white = c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x00a0;
        if (white) { space = !shown.empty(); continue; }
        if (space) { shown.push_back(' '); space = false; }
        shown.push_back(c);
    }
    if (shown.size() < 3 || shown.size() > 32) { why = "A name needs 3 to 32 letters."; return std::string(); }
    S32 letters = 0;
    for (llwchar c : shown)
    {
        const bool ascii_letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        const bool ok = ascii_letter || (c >= '0' && c <= '9') || c == ' ' || c == '\'' || c == '-' || c >= 0x80;
        if (!ok) { why = "A name may use letters, numbers, spaces, apostrophes and hyphens only."; return std::string(); }
        if (ascii_letter || c >= 0x80) ++letters;
    }
    if (letters < 2) { why = "A name needs some letters."; return std::string(); }
    return wstring_to_utf8str(shown);
}

void WolfStarfold::startHangar(const LLUUID& ship)
{
    // once per docking (k=docked comes again on every sale), only on a Wolf login (the camera is a Wolf tool's)
    if (ship.isNull() || ship == mHangarDoneFor || !WolfGrid::isOnWolfTerritoriesConfirmed()) return;
    mHangarDoneFor = ship;
    mHangarShip = ship;
    mHangarWaitUntil = now() + HANGAR_WAIT_S;
    beginHangarCamera();   // at once when the ship's object is here; else hangarIdle tries until the wait ends
}

void WolfStarfold::beginHangarCamera()
{
    LLViewerObject* o = gObjectList.findObject(mHangarShip);
    if (!o || o->isDead() || !isAgentAvatarValid()) return;
    mHangarWaitUntil = -1.0;
    if (gAgentCamera.cameraMouselook() || gAgentCamera.cameraCustomizeAvatar())
    {
        LL_INFOS("WolfStarfold") << "hangar view: not in mouselook or appearance mode" << LL_ENDL;
        return;
    }
    LLViewerObject* root = o->getRootEdit() ? o->getRootEdit() : o;
    // the linkset's box (each prim's centre give or take half its diagonal), agent metres
    LLVector3 lo, hi;
    bool any = false;
    auto grow = [&](LLViewerObject* p)
    {
        if (!p || p->isDead()) return;
        const LLVector3 c = p->getPositionAgent();
        const F32 r = p->getScale().length() * 0.5f;
        const LLVector3 a = c - LLVector3(r, r, r), b = c + LLVector3(r, r, r);
        if (!any) { lo = a; hi = b; any = true; return; }
        for (S32 i = 0; i < 3; ++i) { lo.mV[i] = llmin(lo.mV[i], a.mV[i]); hi.mV[i] = llmax(hi.mV[i], b.mV[i]); }
    };
    grow(root);
    for (LLViewerObject* child : root->getChildren()) grow(child);
    const LLVector3 centre = (lo + hi) * 0.5f;
    const F32 radius = llmax((hi - lo).length() * 0.5f, 2.f);
    // a three-quarter view from in front of the ship (its nose is +X: SpaceMath), on the side where its pilot stepped out (the
    // walkway towards the middle of the bay: SpaceBay.cs StepOut), a little above
    LLVector3 fwd = LLVector3::x_axis * root->getRotationRegion();
    fwd.mV[VZ] = 0.f;
    if (fwd.normVec() < 0.1f) fwd = LLVector3::x_axis;
    LLVector3 side(-fwd.mV[VY], fwd.mV[VX], 0.f);   // the ship's left
    const LLVector3 toMe = gAgent.getPositionAgent() - centre;
    if (toMe * side < 0.f) side = -side;
    const F32 bearing = HANGAR_BEARING_DEG * DEG_TO_RAD, elev = HANGAR_ELEV_DEG * DEG_TO_RAD;
    const LLVector3 dir = (fwd * cosf(bearing) + side * sinf(bearing)) * cosf(elev) + LLVector3::z_axis * sinf(elev);
    const F32 dist = llclamp(radius * HANGAR_DIST_K, HANGAR_DIST_MIN, HANGAR_DIST_MAX);
    // Source: lltoolfocus.cpp (alt-click focus): setFocusOnAvatar(false, ANIMATE) then the focus; setCameraPosAndFocusGlobal places
    // the eye as well and moves there with the camera's own animation (llagentcamera.cpp:3064)
    mHangarFocus = gAgent.getPosGlobalFromAgent(centre);
    gAgentCamera.setFocusOnAvatar(false, ANIMATE);
    gAgentCamera.setCameraPosAndFocusGlobal(gAgent.getPosGlobalFromAgent(centre + dir * dist), mHangarFocus, root->getID());
    mHangarOn = true;
    mHangarAt = now();
    mHangarSettled = false;
    LL_INFOS("WolfStarfold") << "hangar view: ship " << root->getID() << ", half-diagonal " << radius << " m, eye " << dist << " m away" << LL_ENDL;
}

void WolfStarfold::hangarIdle()
{
    if (!mHangarOn)
    {
        if (mHangarWaitUntil > 0.0)
        {
            if (now() > mHangarWaitUntil)
            {
                mHangarWaitUntil = -1.0;
                LL_INFOS("WolfStarfold") << "hangar view: the ship's object never arrived" << LL_ENDL;
            }
            else beginHangarCamera();
        }
        return;
    }
    LLViewerObject* o = gObjectList.findObject(mHangarShip);
    if (!o || o->isDead()) { endHangar("the ship is gone"); return; }
    // the camera taken back by the viewer itself (Escape, a walk: LLAgentCamera::resetView sets the focus on the avatar), another
    // focus (an alt-click elsewhere), mouselook: the view has ended, and the camera is the user's
    if (gAgentCamera.getFocusOnAvatar() || gAgentCamera.cameraMouselook()
        || (gAgentCamera.getFocusTargetGlobal() - mHangarFocus).magVec() > 1.5)
    {
        mHangarOn = false;
        LL_INFOS("WolfStarfold") << "hangar view ended: the camera went elsewhere" << LL_ENDL;
        return;
    }
    // the avatar walks, turns or flies (its own control flags, as LLAgent sets them: indra_constants.h)
    const U32 MOVING = AGENT_CONTROL_AT_POS | AGENT_CONTROL_AT_NEG | AGENT_CONTROL_LEFT_POS | AGENT_CONTROL_LEFT_NEG | AGENT_CONTROL_UP_POS
        | AGENT_CONTROL_UP_NEG | AGENT_CONTROL_NUDGE_AT_POS | AGENT_CONTROL_NUDGE_AT_NEG | AGENT_CONTROL_NUDGE_LEFT_POS
        | AGENT_CONTROL_NUDGE_LEFT_NEG | AGENT_CONTROL_NUDGE_UP_POS | AGENT_CONTROL_NUDGE_UP_NEG | AGENT_CONTROL_TURN_LEFT | AGENT_CONTROL_TURN_RIGHT;
    if (gAgent.getControlFlags() & MOVING) { endHangar("the avatar moved"); return; }
    const LLVector3d off = gAgentCamera.getCameraPositionGlobal() - mHangarFocus;
    const F32 dist = (F32)off.magVec();
    if (dist < 0.01f) return;
    const F32 elev = asinf(llclamp((F32)off.mdV[VZ] / dist, -1.f, 1.f));
    const F32 az = (F32)atan2(off.mdV[VY], off.mdV[VX]);
    if (!mHangarSettled)
    {
        if (now() - mHangarAt < HANGAR_SETTLE_S) return;
        // the camera is where it went: its distance, height and bearing as they are now are the baseline (the camera's own
        // limits may have moved it a little from what was asked)
        mHangarSettled = true;
        mHangarDist = dist;
        mHangarElev = elev;
        mHangarAz = az;
        mHangarAvatarAt = gAgent.getPositionGlobal();
        return;
    }
    // the user moved the camera (zoom, orbit, pan): away from where the slow turn has it
    F32 daz = az - mHangarAz;
    while (daz > F_PI) daz -= F_TWO_PI;
    while (daz < -F_PI) daz += F_TWO_PI;
    if (fabsf(dist - mHangarDist) > llmax(1.f, 0.15f * mHangarDist) || fabsf(elev - mHangarElev) > 6.f * DEG_TO_RAD || fabsf(daz) > 12.f * DEG_TO_RAD)
    {
        mHangarOn = false;
        LL_INFOS("WolfStarfold") << "hangar view ended: the camera was moved" << LL_ENDL;
        return;
    }
    if ((gAgent.getPositionGlobal() - mHangarAvatarAt).magVec() > 1.0) { endHangar("the avatar moved"); return; }
    // the slow turn: the camera tool's own orbit (llagentcamera.cpp:905 cameraOrbitAround - with the focus off the avatar it turns
    // the camera's offset about +Z by the angle, counter-clockwise: LLVector3d::rotVec(angle, 0, 0, 1) applies LLQuaternion(angle, z))
    const F32 step = F_TWO_PI / HANGAR_TURN_S * llclamp((F32)gFrameIntervalSeconds, 0.f, 0.25f);
    gAgentCamera.cameraOrbitAround(step);
    mHangarAz += step;
}

void WolfStarfold::endHangar(const char* why)
{
    const bool wasOn = mHangarOn;
    mHangarOn = false;
    mHangarWaitUntil = -1.0;
    if (!wasOn) return;
    // the camera is still the view's (nothing else took it): it goes back to the avatar, as a walk would take it
    // (LLAgentCamera::resetView: setFocusOnAvatar(true, ANIMATE))
    if (!gAgentCamera.getFocusOnAvatar() && (gAgentCamera.getFocusTargetGlobal() - mHangarFocus).magVec() <= 1.5)
    {
        static LLCachedControl<bool> reset_on_move(gSavedSettings, "FSResetCameraOnMovement", true);
        // a walk with "reset the camera on movement" off keeps the camera where it is (the resident's own setting); anything
        // else (the ship gone, a launch, a new region) gives it back
        if (reset_on_move || strcmp(why, "the avatar moved") != 0) gAgentCamera.setFocusOnAvatar(true, ANIMATE);
    }
    LL_INFOS("WolfStarfold") << "hangar view ended: " << why << LL_ENDL;
}

void WolfStarfold::logState() const
{
    // read-only, for a LEAP rig (WolfStarfold.Do "state"): what the cockpit's banner says and what the camera is doing
    std::string banner;
    if (automatic())
    {
        if (mShip.mSeq == "out") banner = "AUTOMATIC DEPARTURE";
        else banner = mBayPad > 0 ? llformat("AUTOMATIC DOCKING - pad %d", mBayPad) : (mBayPad == 0 ? std::string("AUTOMATIC DOCKING - the lift") : std::string("AUTOMATIC DOCKING"));
    }
    LLViewerObject* fo = gAgentCamera.getFocusObject();
    // the camera's bearing round the hangar view's focus (degrees from +X towards +Y), so a rig can see it turn
    const LLVector3d off = gAgentCamera.getCameraPositionGlobal() - mHangarFocus;
    const F32 bearing = mHangarOn ? (F32)(atan2(off.mdV[VY], off.mdV[VX]) * RAD_TO_DEG) : 0.f;
    LL_INFOS("WolfStarfold") << "state: piloting " << piloting() << ", seq '" << mShip.mSeq << "', class '" << mShip.mClassId << "', banner '" << banner
                             << "', docked " << mStation.mDocked << " pad " << mStation.mPad << " ship " << mStation.mShip
                             << ", throttle " << mThrottle << ", last ctl or fly sent " << (now() - mSentAt) << " s ago"
                             << ", hangar view " << mHangarOn << " (ship " << mHangarShip << ", bearing " << bearing << " deg), camera mode " << (S32)gAgentCamera.getCameraMode()
                             << ", focus on avatar " << gAgentCamera.getFocusOnAvatar() << ", focus object " << (fo ? fo->getID() : LLUUID::null)
                             << ", station '" << stationName() << "' bay " << mSys.mBay << ", rename answer '" << mStationAnswer << "'" << LL_ENDL;
}

// ── the cockpit's visibility ─────────────────────────────────────────────────────────────────────────────────────────

void WolfStarfold::showGuide(bool on)
{
    if (mGuide && !on) gSavedSettings.setBOOL("WolfStarfoldGuideSeen", true);
    mGuide = on;
    mGuideCard = 0;
}

bool WolfStarfold::panelShown() const
{
    if (!WolfGrid::isOnWolfTerritoriesConfirmed() || !spaceHere()) return false;
    if (piloting()) return !mHidden;
    return mByHand || stationShown();
}

void WolfStarfold::togglePanel()
{
    if (!WolfGrid::isOnWolfTerritories() || !spaceHere()) return;
    if (piloting())
    {
        mHidden = !mHidden;
        if (!mHidden) claimBottom();
        return;
    }
    mByHand = !mByHand;
    if (mByHand) claimBottom();
}

// ── drawing ──────────────────────────────────────────────────────────────────────────────────────────────────────────

bool WolfStarfold::beamEnd(const LLUUID& id, S32 rock, LLVector3& agent) const
{
    if (rock >= 0)
    {
        LLViewerRegion* r = gAgent.getRegion();
        if (!r || rock >= (S32)mRocks.size()) return false;
        agent = r->getPosAgentFromRegion(mRocks[(size_t)rock].mPos);
        return true;
    }
    LLViewerObject* o = gObjectList.findObject(id);
    if (!o || o->isDead()) return false;
    LLViewerObject* root = o->getRootEdit();
    agent = (root ? root : o)->getPositionAgent();
    return true;
}

bool WolfStarfold::alienAt(const LLUUID& id, LLVector3& agent) const
{
    if (id.isNull()) return false;
    if (beamEnd(id, -1, agent)) return true;
    LLViewerRegion* r = gAgent.getRegion();
    if (!r) return false;
    if (id == mShip.mTarget && mShip.mHaveTargetPos) { agent = r->getPosAgentFromRegion(mShip.mTargetPos); return true; }
    for (size_t i = 0; i < mShip.mAliens.size() && i < mShip.mAlienPos.size(); ++i)
    {
        if (mShip.mAliens[i] == id) { agent = r->getPosAgentFromRegion(mShip.mAlienPos[i]); return true; }
    }
    return false;
}

namespace
{
    // An icosahedron, unit radius (Source: the standard (0, +-1, +-phi) construction; e.g. Paul Bourke, "Platonic Solids")
    void icosahedron(std::vector<LLVector3>& v, std::vector<S32>& tri)
    {
        const F32 t = (1.f + sqrtf(5.f)) * 0.5f;
        const F32 p[12][3] = { { -1, t, 0 }, { 1, t, 0 }, { -1, -t, 0 }, { 1, -t, 0 }, { 0, -1, t }, { 0, 1, t },
                               { 0, -1, -t }, { 0, 1, -t }, { t, 0, -1 }, { t, 0, 1 }, { -t, 0, -1 }, { -t, 0, 1 } };
        for (const auto& q : p) { LLVector3 a(q[0], q[1], q[2]); a.normVec(); v.push_back(a); }
        const S32 f[20][3] = { { 0, 11, 5 }, { 0, 5, 1 }, { 0, 1, 7 }, { 0, 7, 10 }, { 0, 10, 11 }, { 1, 5, 9 }, { 5, 11, 4 },
                               { 11, 10, 2 }, { 10, 7, 6 }, { 7, 1, 8 }, { 3, 9, 4 }, { 3, 4, 2 }, { 3, 2, 6 }, { 3, 6, 8 },
                               { 3, 8, 9 }, { 4, 9, 5 }, { 2, 4, 11 }, { 6, 2, 10 }, { 8, 6, 7 }, { 9, 8, 1 } };
        for (const auto& q : f) { tri.push_back(q[0]); tri.push_back(q[1]); tri.push_back(q[2]); }
    }

    // One subdivision: each triangle into four, the new points pushed out to the sphere (80 triangles)
    const std::vector<LLVector3>& rockDirs()
    {
        static std::vector<LLVector3> tris;   // 3 per triangle
        if (!tris.empty()) return tris;
        std::vector<LLVector3> v;
        std::vector<S32> t;
        icosahedron(v, t);
        for (size_t i = 0; i < t.size(); i += 3)
        {
            const LLVector3 a = v[(size_t)t[i]], b = v[(size_t)t[i + 1]], c = v[(size_t)t[i + 2]];
            LLVector3 ab = a + b, bc = b + c, ca = c + a;
            ab.normVec(); bc.normVec(); ca.normVec();
            const LLVector3 out[4][3] = { { a, ab, ca }, { b, bc, ab }, { c, ca, bc }, { ab, bc, ca } };
            for (const auto& f : out) { tris.push_back(f[0]); tris.push_back(f[1]); tris.push_back(f[2]); }
        }
        return tris;
    }
    const U32 ROCK_VERTS = 240;   // 80 triangles
}

void WolfStarfold::renderDeferred()
{
    if (!spaceHere() || mRocks.empty()) return;
    LLViewerRegion* region = gAgent.getRegion();
    if (!region || LLViewerFetchedTexture::sWhiteImagep.isNull()) return;
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    const LLVector3 origin = region->getOriginAgent();
    if (mRocksDirty || mRockVB.isNull() || origin != mRockOrigin)
    {
        mRocksDirty = false;
        mRockOrigin = origin;
        const U32 count = (U32)mRocks.size() * ROCK_VERTS;
        LLPointer<LLVertexBuffer> vb = new LLVertexBuffer(LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_NORMAL
                                                          | LLVertexBuffer::MAP_TEXCOORD0 | LLVertexBuffer::MAP_COLOR);
        if (!vb->allocateBuffer(count, 0))
        {
            LL_WARNS("WolfStarfold") << "could not allocate the belt's vertex buffer (" << count << " vertices)" << LL_ENDL;
            mRockVB = nullptr;
            mRockVerts = 0;
            return;
        }
        LLStrider<LLVector3> pos, norm;
        LLStrider<LLVector2> uv;
        LLStrider<LLColor4U> col;
        if (!vb->getVertexStrider(pos) || !vb->getNormalStrider(norm) || !vb->getTexCoord0Strider(uv) || !vb->getColorStrider(col))
        {
            vb->unmapBuffer();
            mRockVB = nullptr;
            mRockVerts = 0;
            return;
        }
        const std::vector<LLVector3>& dirs = rockDirs();
        for (size_t i = 0; i < mRocks.size(); ++i)
        {
            const Rock& rock = mRocks[i];
            // each rock's own shape: README "client look rules" - seed ^ (i * 0x9E3779B1)
            U32 s = mSys.mSeed ^ (U32)((U32)i * 0x9E3779B1u);
            if (s == 0) s = 1;
            // three lumps (a direction, a frequency, a phase) and a squash per axis, then a turn
            LLVector3 lump[3];
            F32 freq[3], phase[3];
            for (S32 k = 0; k < 3; ++k)
            {
                lump[k].set((F32)next(s) * 2.f - 1.f, (F32)next(s) * 2.f - 1.f, (F32)next(s) * 2.f - 1.f);
                if (lump[k].normVec() < 1e-3f) lump[k].set(1.f, 0.f, 0.f);
                freq[k] = 1.5f + 3.f * (F32)next(s);
                phase[k] = (F32)(next(s) * 2.0 * F_PI);
            }
            const LLVector3 squash(1.f, 0.65f + 0.35f * (F32)next(s), 0.5f + 0.4f * (F32)next(s));
            LLQuaternion turn;
            turn.setQuat((F32)(next(s) * 2.0 * F_PI), LLVector3((F32)next(s) - 0.5f, (F32)next(s) - 0.5f, (F32)next(s) - 0.5f + 0.01f));
            const F32 shade = 0.8f + 0.4f * (F32)next(s);
            const LLVector3 centre = region->getPosAgentFromRegion(rock.mPos);
            auto surface = [&](const LLVector3& d)
            {
                F32 r = 1.f;
                for (S32 k = 0; k < 3; ++k) r += (k == 0 ? 0.22f : 0.12f) * sinf((d * lump[k]) * freq[k] + phase[k]);
                LLVector3 p(d.mV[0] * squash.mV[0], d.mV[1] * squash.mV[1], d.mV[2] * squash.mV[2]);
                return (p * r) * turn * rock.mSize;
            };
            for (size_t f = 0; f < dirs.size(); f += 3)
            {
                const LLVector3 p0 = surface(dirs[f]), p1 = surface(dirs[f + 1]), p2 = surface(dirs[f + 2]);
                LLVector3 n = (p1 - p0) % (p2 - p0);
                if (n.normVec() < 1e-6f) n = dirs[f];
                if (n * (p0 + p1 + p2) < 0.f) n = -n;   // outward
                // the colour: Ferrite grey-brown; Lumen crystal darker blue-grey with glinting faces; mined out: dim
                LLColor3 c = rock.mOre == 0 ? LLColor3(0.36f, 0.31f, 0.27f) : LLColor3(0.27f, 0.30f, 0.38f);
                if (rock.mOre == 1 && ((f / 3) % 7) == 0) c = LLColor3(0.70f, 0.85f, 1.f);
                c *= shade * (rock.mLeft ? 1.f : 0.45f);
                const LLColor4U cu((U8)llclamp(c.mV[0] * 255.f, 0.f, 255.f), (U8)llclamp(c.mV[1] * 255.f, 0.f, 255.f),
                                   (U8)llclamp(c.mV[2] * 255.f, 0.f, 255.f), 255);
                const LLVector3 tri[3] = { p0, p1, p2 };
                for (const LLVector3& p : tri)
                {
                    *pos++ = centre + p;
                    *norm++ = n;
                    *uv++ = LLVector2(0.5f, 0.5f);
                    *col++ = cu;
                }
            }
        }
        vb->unmapBuffer();
        mRockVB = vb;
        mRockVerts = count;
    }
    if (mRockVB.isNull() || mRockVerts == 0) return;
    // Source: WolfBirds::renderDeferred (wolfbirds.cpp, itself WolfFarGround::renderDeferred) - the same program, matrices and
    // attributes, so the rocks are lit, fogged and hazed as the scene round them is; a white texture leaves the colours.
    LLGLDisable blend(GL_BLEND);
    LLGLEnable cull(GL_CULL_FACE);
    LLGLSLShader& shader = gDeferredNonIndexedDiffuseAlphaMaskProgram;
    shader.bind();
    shader.setMinimumAlpha(0.5f);
    gGLLastMatrix = nullptr;
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.loadMatrix(gGLModelView);
    gGL.getTexUnit(0)->bindFast(LLViewerFetchedTexture::sWhiteImagep);
    mRockVB->setBuffer();
    mRockVB->drawArrays(LLRender::TRIANGLES, 0, mRockVerts);
    shader.unbind();
}

bool WolfStarfold::renderLens(LLRenderTarget* src, LLRenderTarget* dst, LLVertexBuffer* screen_triangle)
{
    if (!src || !dst || !screen_triangle || !gWolfStarfoldLensProgram.isComplete() || gCubeSnapshot) return false;
    const F32 tun = tunnel();
    F32 lens = 0.f;
    F32 cx = 0.5f, cy = 0.5f, rad = 0.f;
    LLViewerCamera* cam = LLViewerCamera::getInstance();
    LLViewerRegion* region = gAgent.getRegion();
    if (spaceHere() && mSys.mHaveWormhole && region)
    {
        // the wormhole's centre on screen, from the camera basis (as pipeline.cpp wolfGodRays places the sun: LLCoordFrame
        // at / left / up, LLCamera's vertical field of view and aspect)
        const LLVector3 c = region->getPosAgentFromRegion(mSys.mWormhole);
        const LLVector3 d = c - cam->getOrigin();
        const F32 dist = d.length();
        const F32 fwd = d * cam->getAtAxis();
        if (fwd > WORMHOLE_M * 0.5f && dist < 8000.f)
        {
            const F32 sx = -(d * cam->getLeftAxis()) / fwd;
            const F32 sy = (d * cam->getUpAxis()) / fwd;
            const F32 tan_half = tanf(cam->getView() * 0.5f);
            cx = (sx / (tan_half * cam->getAspect())) * 0.5f + 0.5f;
            cy = (sy / tan_half) * 0.5f + 0.5f;
            // the sphere's angular radius as a share of the frame's height (half the frame = tan_half)
            rad = (WORMHOLE_M / fwd) / tan_half * 0.5f;
            if (cx > -0.5f && cx < 1.5f && cy > -0.5f && cy < 1.5f && rad > 0.002f) lens = llclamp(spaceFade() * 1.5f, 0.f, 1.f);
        }
    }
    if (lens <= 0.f && tun <= 0.f) return false;
    LL_PROFILE_GPU_ZONE("wolf starfold lens");
    static LLStaticHashedString s_centre("uCentre"), s_radius("uRadius"), s_aspect("uAspect"), s_time("uTime"), s_lens("uLens"),
        s_tunnel("uTunnel"), s_ring("uRingColor"), s_throat("uThroatColor");
    // Source: pipeline.cpp LLPipeline::wolfPhotoPass - the finished frame in diffuseRect, the screen triangle into dst
    LLGLSLShader* shader = &gWolfStarfoldLensProgram;
    shader->bind();
    const S32 channel = shader->enableTexture(LLShaderMgr::DEFERRED_DIFFUSE, src->getUsage());
    if (channel < 0)
    {
        shader->unbind();
        return false;
    }
    dst->bindTarget();
    src->bindTexture(0, channel, LLTexUnit::TFO_BILINEAR);
    shader->uniform2f(s_centre, cx, cy);
    shader->uniform1f(s_radius, llmin(rad, 2.f));
    shader->uniform1f(s_aspect, (F32)dst->getWidth() / llmax((F32)dst->getHeight(), 1.f));
    shader->uniform1f(s_time, (F32)fmod(now(), 3600.0));
    shader->uniform1f(s_lens, lens);
    shader->uniform1f(s_tunnel, tun);
    shader->uniform3f(s_ring, 0.35f, 0.95f, 1.f);
    shader->uniform3f(s_throat, 0.25f, 0.35f, 0.9f);
    screen_triangle->setBuffer();
    screen_triangle->drawArrays(LLRender::TRIANGLES, 0, 3);
    shader->disableTexture(LLShaderMgr::DEFERRED_DIFFUSE, src->getUsage());
    shader->unbind();
    dst->flush();
    return true;
}
