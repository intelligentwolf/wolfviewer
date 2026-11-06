/**
 * @file wolfswimbubbles.cpp
 * @brief WolfViewer: bubbles from the mouths of avatars under the water. See wolfswimbubbles.h.
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
#include "wolfswimbubbles.h"
#include "wolfsoundeffects.h"   // [SOUND EFFECTS 2026-11-06/]

#include <algorithm>
#include <cmath>

#include "fswolfwater.h"
#include "llagent.h"
#include "llaudioengine.h"
#include "llcharacter.h"
#include "llframetimer.h"
#include "llrand.h"
#include "llrender.h"
#include "llvertexbuffer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "llvoavatar.h"
#include "llvowater.h"
#include "llworld.h"
#include "pipeline.h"
#include "wolfboatrock.h"
#include "wolfdryvolumes.h"
#include "wolfframegovernor.h"
#include "wolfgrid.h"
#include "wolfterrainpaint.h"
#include "wolfweathersound.h"

namespace
{
    // ── who ──────────────────────────────────────────────────────────────────────────────────────────────────────
    // Avatars further than this from the camera are skipped (the brief: "skip far avatars, e.g. beyond 40 m"; a 1 cm
    // bubble there is a third of a pixel on a 1080p frame at a 60-degree view, about 1 mrad a pixel).
    constexpr F32 REACH_M = 40.f;
    // Source: wolfswim.cpp:50 ENTER_BELOW - the swim starts once the avatar's top is this far under the water height; the
    // mouth must be this far under before the first puff.
    constexpr F32 ENTER_BELOW = 0.10f;
    // Once puffing, a mouth that comes out of the water (a wave, a bob at the surface) keeps its breathing rhythm for this
    // long (chosen: about one breath, BREATHS_MAX).
    constexpr F64 OUT_KEEP_S = 3.0;

    // ── the mouth ────────────────────────────────────────────────────────────────────────────────────────────────
    // Source: avatar_lad.xml:120-127 - attachment point 11 "Mouth", joint mHead, position "0.12 0 0.001": the viewer's
    // own mouth, used for an avatar wearing no rigged mesh on its head.
    constexpr F32 MOUTH_POINT_X = 0.12f, MOUTH_POINT_Z = 0.001f;
    // Source: wolfstorm/python/charbuilder/wolfbreath.py:33 TIP_AHEAD - the dragon breath's emitter sits this far ahead of
    // the snout tip (mouth_tip, :57-68: the frontmost mesh point round mHead), so it starts outside the face.
    constexpr F32 TIP_AHEAD_M = 0.05f;
    // A sanity bound on the measured front of the head (chosen: larger than any head the grid's avatars have, so only a
    // broken mesh is clamped).
    constexpr F32 MAX_FORWARD_M = 2.f;
    // The front of the head is measured again this often (chosen: the rig changes only when the outfit does).
    constexpr F64 RIG_EVERY_S = 2.0;

    // ── breathing ────────────────────────────────────────────────────────────────────────────────────────────────
    // Source: Cleveland Clinic, "Vital Signs" (my.clevelandclinic.org/health/articles/10881-vital-signs, read 2 Nov 2026):
    // "The normal respiratory rate for an adult at rest is 12 to 18 breaths per minute." One puff a breath.
    constexpr F32 BREATHS_MIN = 12.f, BREATHS_MAX = 18.f;
    // Source: wolfswim.h:24-26 - the swim is 3.28 m/s. Swimming that fast halves the time between puffs (chosen: "more
    // while swimming fast"; no figure for breathing while swimming was found).
    constexpr F32 SWIM_SPEED = 3.28f;
    // A few bubbles a puff, leaving the mouth one after another (chosen: Jimmy's "tiny bubble", our reply's "a few").
    constexpr S32 PUFF_MIN = 3, PUFF_MAX = 6;
    constexpr F64 BURST_GAP_S = 0.07;
    constexpr F32 FIRST_PUFF_MIN_S = 0.3f, FIRST_PUFF_MAX_S = 1.2f;   // chosen: soon after the mouth goes under

    // ── the bubbles ──────────────────────────────────────────────────────────────────────────────────────────────
    // Diameter as they leave the mouth (chosen: small, but at least a few pixels a few metres off - see REACH_M).
    constexpr F32 D_MIN_M = 0.004f, D_MAX_M = 0.010f;
    // Rise speed. Source: Mendelson (1967), "The prediction of bubble terminal velocities from wave theory", AIChE J.
    // 13:250 - a bubble rises at the speed of a deep-water surface wave whose wavelength is its perimeter (Lehrer, "A
    // rational terminal velocity equation for bubbles and drops"): c^2 = g lambda / (2 pi) + 2 pi sigma / (rho lambda)
    // with lambda = pi d gives U = sqrt(2 sigma / (rho d) + g d / 2) - 0.24 m/s at 4 mm, 0.25 at 3 mm, 0.26 at 10 mm.
    // (Checked against Weber, Bart & Klar, arXiv:1706.05840, the Magdeburg bubble-column data: 3 mm bubbles at 0.29 m/s.)
    constexpr F32 SIGMA = 0.0727f;      // N/m, water at 20 C (IAPWS R1-76(2014): 72.74 mN/m)
    constexpr F32 RHO_WATER = 998.2f;   // kg/m^3, water at 20 C (IAPWS-95)
    constexpr F32 G = 9.80665f;         // m/s^2, standard gravity (3rd CGPM, 1901)
    // The wobble. Source: Weber, Bart & Klar (arXiv:1706.05840, sec. "Experimental data") - 3 mm bubbles in water: path
    // amplitude A = 1.87 mm, wavelength 4.14 cm, rise 0.29 m/s, frequency 7 Hz. Kept for every size as A / d = 1.87 / 3
    // and the Strouhal number f d / U = 7 x 0.003 / 0.29 (an assumption: the same shape of path, scaled). Herrada & Eggers
    // (2023, PNAS 120:e2216830120): a straight path is unstable above a radius of 0.926 mm, so all of these wobble; the
    // zigzag turns into a helix as it rises (Ellingsen & Risso 2001, J. Fluid Mech. 440:235), drawn as a helix.
    constexpr F32 WOBBLE_A_PER_D = 1.87f / 3.f;
    constexpr F32 STROUHAL = 7.f * 0.003f / 0.29f;
    // Growth: Boyle's law, r = r0 (p0 / p)^(1/3), p = p_atm + rho_sea g depth.
    constexpr F32 P_ATM = 101325.f;     // Pa, the standard atmosphere
    constexpr F32 RHO_SEA = 1025.f;     // kg/m^3, mean sea water
    // The puff's own push out of the mouth, decaying (chosen: a gentle exhale, gone within half a second).
    constexpr F32 PUFF_SPEED = 0.3f, PUFF_DECAY_S = 0.15f;
    // At the surface it swells and is gone (chosen).
    constexpr F32 POP_S = 0.08f, POP_SWELL = 1.4f;
    // A bubble that has risen this long without reaching a surface is let go (chosen: 15 m of rise).
    constexpr F32 MAX_AGE_S = 60.f;
    // The surface over a bubble (the drawn waves) is sampled this often (chosen; the birds' floating gulls take 20 Hz,
    // a bubble only needs to know when it arrives).
    constexpr F64 SURFACE_SAMPLE_S = 0.1;
    // At most this many bubbles (the frame budget lowers it, WolfFrameGovernor::budget 0.25 .. 1).
    constexpr S32 MAX_BUBBLES = 160;
    // Room over that for the ones popping (POP_S each) - the buffer's size (review 2026-11-02: an evicted bubble pops,
    // it never just vanishes in mid-water).
    constexpr S32 HARD_MAX = 200;

    // ── the look ─────────────────────────────────────────────────────────────────────────────────────────────────
    // An icosahedron, smooth-shaded: 20 triangles, each with its own three vertices (the birds' buffer layout).
    constexpr S32 TRIS_PER_BUBBLE = 20, VERTS_PER_BUBBLE = TRIS_PER_BUBBLE * 3;
    constexpr S32 MAX_VERTS = HARD_MAX * VERTS_PER_BUBBLE;   // 12000, U16 indices
    // Chosen: a bubble is mostly see-through with a bright silvery rim (light reflected at a grazing angle); drawn opaque
    // in the deferred pass, it is a water tint where it faces the camera and silver towards its rim.
    const F32 CORE[3] = { 0.62f, 0.74f, 0.78f };
    const F32 RIM[3]  = { 0.95f, 0.98f, 1.00f };

    // ── the sound ────────────────────────────────────────────────────────────────────────────────────────────────
    // Source: app_settings/wolfbubbles/SOURCES.md (CC0: Freesound 17341 by Halion, an underwater recording of bubbles);
    // the recipe is region-terrain-work/swim-bubbles-2026-11-02/tools/build_sounds.py.
    const WolfWeatherSound::Recording PUFF[3] = {
        { "wolf_bubbles_1.ogg", "8a554e88-87a5-4ce0-bdb3-6addcdda9965", nullptr, false, "wolfbubbles" },
        { "wolf_bubbles_2.ogg", "3e67e53b-0c94-4b26-ab7a-e8b63782c6f3", nullptr, false, "wolfbubbles" },
        { "wolf_bubbles_3.ogg", "bf25ff3e-35b6-4ffd-a17c-e290971f7c19", nullptr, false, "wolfbubbles" },
    };
    // Every avatar's puffs are heard, quietly (README "Sound": you hear the bubbles you see, a friend's as well as your
    // own); full level within SOUND_FULL_M, falling as 1/distance, none beyond SOUND_MAX_M. Another avatar's puff starts at
    // most every OTHER_SOUND_GAP_S, so a group dive is never a wall of bubbling; your own always plays. All chosen.
    constexpr F32 SOUND_BASE = 0.25f, SOUND_FULL_M = 2.f, SOUND_MAX_M = 15.f;
    constexpr F64 OTHER_SOUND_GAP_S = 0.5;
    // As wolfbirds.cpp CALL_EAR_M: the sound is placed this far along its direction from the camera and its distance
    // gain is ours (OpenAL's 1 m inverse roll-off would drown a puff 5 m away).
    constexpr F32 SOUND_EAR_M = 2.f;
    // A listener above the water hears nothing of a puff under it: sound crossing the surface keeps
    // 4 Z1 Z2 / (Z1 + Z2)^2 of its energy - air 1.204 kg/m^3 x 343.2 m/s = 413 Pa s/m, water 998.2 x 1482 = 1.48e6 Pa s/m
    // - 0.11 % (-29.5 dB), below hearing at this level; so it is not played at all.

    F32 frand_range(F32 lo, F32 hi) { return lo + ll_frand(hi - lo); }

    /** Mendelson's terminal rise speed of a bubble of diameter d (see SIGMA). */
    F32 rise_speed(F32 d)
    {
        d = llmax(d, 0.001f);
        return sqrtf(2.f * SIGMA / (RHO_WATER * d) + G * d * 0.5f);
    }

    F32 pressure_at(F32 depth) { return P_ATM + RHO_SEA * G * llmax(depth, 0.f); }

    /**
     * Is p inside the water body of a bounded water plane (a wolfwater prim's, a painted stroke segment's)? Source:
     * llvowater.cpp:469-487 updateGeometry - the surface's vertices are center + (x, y, -scale.z / 2) * rotation over the
     * plane's X / Y footprint; the water below it is getBoundedWaterDepth() deep (fswolfwater.cpp:367-371: the prim's own
     * thickness; wolfterrainpaint.cpp rebuildWater: the stroke's depth). surface: the agent z of the surface over p.
     */
    bool in_plane(const LLVOWater* w, const LLVector3& p, F32& surface, bool depth_test = true)
    {
        const F32 depth = w->getBoundedWaterDepth();
        if (depth <= 0.f) return false;
        const LLVector3 half = w->getScale() * 0.5f;
        const LLQuaternion rot = w->getRotation();
        const LLVector3 local = (p - w->getPositionAgent()) * ~rot;
        if (fabsf(local.mV[VX]) > half.mV[VX] || fabsf(local.mV[VY]) > half.mV[VY]) return false;
        const F32 top = -half.mV[VZ];
        if (depth_test && (local.mV[VZ] > top || local.mV[VZ] < top - depth)) return false;
        surface = (w->getPositionAgent() + LLVector3(local.mV[VX], local.mV[VY], top) * rot).mV[VZ];
        return true;
    }

    // The icosahedron's twelve corners (unit sphere) and twenty faces, outward winding.
    struct Ico
    {
        LLVector3 v[12];
        S32 f[TRIS_PER_BUBBLE][3];
        Ico()
        {
            const F32 t = (1.f + sqrtf(5.f)) * 0.5f;
            const F32 c[12][3] = { { -1, t, 0 }, { 1, t, 0 }, { -1, -t, 0 }, { 1, -t, 0 }, { 0, -1, t }, { 0, 1, t },
                                   { 0, -1, -t }, { 0, 1, -t }, { t, 0, -1 }, { t, 0, 1 }, { -t, 0, -1 }, { -t, 0, 1 } };
            for (S32 i = 0; i < 12; ++i)
            {
                v[i].setVec(c[i][0], c[i][1], c[i][2]);
                v[i].normVec();
            }
            const S32 faces[TRIS_PER_BUBBLE][3] = { { 0, 11, 5 }, { 0, 5, 1 }, { 0, 1, 7 }, { 0, 7, 10 }, { 0, 10, 11 },
                                                    { 1, 5, 9 }, { 5, 11, 4 }, { 11, 10, 2 }, { 10, 7, 6 }, { 7, 1, 8 },
                                                    { 3, 9, 4 }, { 3, 4, 2 }, { 3, 2, 6 }, { 3, 6, 8 }, { 3, 8, 9 },
                                                    { 4, 9, 5 }, { 2, 4, 11 }, { 6, 2, 10 }, { 8, 6, 7 }, { 9, 8, 1 } };
            for (S32 i = 0; i < TRIS_PER_BUBBLE; ++i)
            {
                // keep every face wound outward (counter-clockwise seen from outside), whatever the table's order
                const LLVector3& a = v[faces[i][0]];
                const LLVector3& b = v[faces[i][1]];
                const LLVector3& cc = v[faces[i][2]];
                const bool out = ((b - a) % (cc - a)) * (a + b + cc) > 0.f;
                f[i][0] = faces[i][0];
                f[i][1] = out ? faces[i][1] : faces[i][2];
                f[i][2] = out ? faces[i][2] : faces[i][1];
            }
        }
    };
    const Ico& ico()
    {
        static const Ico s;
        return s;
    }
}

WolfSwimBubbles::WolfSwimBubbles() {}
WolfSwimBubbles::~WolfSwimBubbles() {}

void WolfSwimBubbles::reset()
{
    mBubbles.clear();
    mDivers.clear();
}

void WolfSwimBubbles::rebase(const LLVector3& delta)
{
    for (Bubble& b : mBubbles)
    {
        b.mCentre += delta;
        b.mPos += delta;
        b.mSurface += delta.mV[VZ];
    }
}

bool WolfSwimBubbles::surfaceOf(const Water& w, const LLVector3& p, F32& surface) const
{
    if (w.mPlane.notNull())
    {
        // a pool's bubble: up through that plane's water, gone if it drifts out of its footprint (it met the pool's side)
        if (w.mPlane->isDead()) return false;
        return in_plane(w.mPlane.get(), p, surface, false);
    }
    // the sea: the water height of the region under the point and the waves as drawn there (wolfbirds.cpp waterSurfaceZ)
    LLViewerRegion* r = LLWorld::getInstance()->getRegionFromPosAgent(p);
    if (!r) r = gAgent.getRegion();
    if (!r) return false;
    const F32 wave = WolfBoatRock::instanceExists() ? WolfBoatRock::instance().drawnWaveHeight(r, p.mV[VX], p.mV[VY]) : 0.f;
    surface = r->getWaterHeight() + wave;
    return std::isfinite(surface);   // review 2026-11-02: a NaN surface would make bubbles that never pop
}

bool WolfSwimBubbles::underWater(const LLVector3& p, F32& surface, Water& water) const
{
    // never in the air of a sea tunnel or a boat's hull (the same test that keeps an avatar from swimming there,
    // wolfswim.cpp:683)
    if (WolfDryVolumes::instance().contains(p)) return false;
    // a builder's own water (a wolfwater prim, a painted pond or river): its body first, it may be above the sea
    for (size_t i = 0; i < mPlanes.size(); ++i)
    {
        LLVOWater* w = mPlanes[i];
        // this frame's bounding sphere of the plane's body (idle): most planes are rejected without a rotation
        if (dist_vec_squared(p, mPlaneCentre[i]) > mPlaneRadius2[i]) continue;
        F32 s = 0.f;
        if (in_plane(w, p, s))
        {
            surface = s;
            water.mPlane = w;
            return true;
        }
    }
    Water sea;
    F32 s = 0.f;
    if (!surfaceOf(sea, p, s) || p.mV[VZ] >= s) return false;
    surface = s;
    water = sea;
    return true;
}

bool WolfSwimBubbles::mouthOf(LLVOAvatar* av, Diver& d, bool measure, LLVector3& mouth, LLVector3& forward)
{
    LLJoint* head = av->mHeadp;
    if (!head) return false;
    if (measure)
    {
        d.mForward = MOUTH_POINT_X;
        // The frontmost point of the rigged mesh the head carries: llskinningutil.cpp:324-379 updateRiggingInfo puts every
        // vertex weighted over 0.2 to a joint into that joint's box in the joint's own space (bind pose: the inverse bind
        // times the bind shape, :363-370), and llvoavatar.cpp:12081-12133 merges the avatar's rigged attachments; the
        // box's +X end is the tip of the face or snout (the joint's +X is forward, avatar_lad.xml's Mouth at +0.12).
        // llvoavatar.cpp:1661-1683 draws the same box with joint->getWorldMatrix() for the avatar's bounding box.
        av->updateRiggingInfo();
        const S32 jn = head->getJointNum();
        if (jn >= 0 && jn < av->mJointRiggingInfoTab.size() && av->mJointRiggingInfoTab[jn].isRiggedTo())
        {
            const F32 front = av->mJointRiggingInfoTab[jn].getRiggedExtents()[1][0];
            if (std::isfinite(front)) d.mForward = llclamp(front, MOUTH_POINT_X, MAX_FORWARD_M);
        }
    }
    const LLVector3 local((d.mForward > 0.f ? d.mForward : MOUTH_POINT_X) + TIP_AHEAD_M, 0.f, MOUTH_POINT_Z);
    mouth = local * head->getWorldMatrix();   // m4math.h:383 - the full transform, as llvoavatar.cpp:1681 for the box
    forward = LLVector3(1.f, 0.f, 0.f) * head->getWorldRotation();
    if (!mouth.isFinite()) return false;
    return true;
}

void WolfSwimBubbles::emit(const LLVector3& mouth, const LLVector3& forward, F32 surface, const Water& water, S32 cap)
{
    // At the cap the bubble furthest from the camera pops to make room; if the new one would be the furthest it is not
    // made (review 2026-11-02: dropping the oldest made them vanish in mid-water).
    if ((S32)mBubbles.size() >= HARD_MAX) return;
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    S32 live = 0;
    Bubble* furthest = nullptr;
    F32 furthest_d = -1.f;
    for (Bubble& o : mBubbles)
    {
        if (o.mPopping) continue;
        ++live;
        const F32 dd = (o.mPos - cam).length();
        if (dd > furthest_d)
        {
            furthest_d = dd;
            furthest = &o;
        }
    }
    if (live >= cap)
    {
        if (!furthest || furthest_d <= (mouth - cam).length()) return;
        furthest->mPopping = true;
        furthest->mPop = POP_S;
    }
    Bubble b;
    b.mCentre = mouth;
    b.mPos = mouth;
    LLVector3 push = forward;
    push.mV[VZ] = llmax(push.mV[VZ], 0.f);
    if (!push.isExactlyZero()) push.normVec();
    b.mDrift = push * PUFF_SPEED;
    b.mR0 = frand_range(D_MIN_M, D_MAX_M) * 0.5f;
    b.mR = b.mR0;
    b.mP0 = pressure_at(surface - mouth.mV[VZ]);
    b.mPhase = ll_frand(F_TWO_PI);
    b.mTurn = ll_frand() < 0.5f ? -1.f : 1.f;
    b.mSurface = surface;
    b.mSurfaceT = LLFrameTimer::getElapsedSeconds();
    b.mWater = water;
    mBubbles.push_back(b);
}

void WolfSwimBubbles::puffSound(const LLVector3& mouth, bool self, F64 now)
{
    if (!gAudiop) return;
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    const F32 dist = (mouth - cam).length();
    if (dist > SOUND_MAX_M) return;
    // the listener under the water too (see the surface note above); in the same water body or not, it is water
    F32 s = 0.f;
    Water w;
    if (!underWater(cam, s, w)) return;
    if (!self && now < mNextOtherSound) return;
    const WolfWeatherSound::Recording& rec = PUFF[ll_rand(3)];
    WolfWeatherSound& ws = WolfWeatherSound::instance();
    if (!ws.recordingReady(rec)) return;   // decoding: the next puff will play
    if (!self) mNextOtherSound = now + OTHER_SOUND_GAP_S;   // only a puff that is played takes the slot (review)
    const F32 gain = llclamp(SOUND_BASE * (SOUND_FULL_M / llmax(SOUND_FULL_M, dist)) * WolfSoundEffects::level(WolfSoundEffects::BUBBLES), 0.f, 1.f);   // [SOUND EFFECTS 2026-11-06/]
    if (gain <= 0.f) return;
    LLVector3 dir = mouth - cam;
    const F32 len = dir.length();
    if (len < 1e-3f) dir.setVec(0.f, 0.f, 1.f);
    else dir /= len;
    const LLVector3 pos = cam + dir * llmin(len, SOUND_EAR_M);
    // AUDIO_TYPE_AMBIENT: the Ambient volume (llvieweraudio.cpp:511-518 setSecondaryGain), as the birds' calls
    gAudiop->triggerSound(LLUUID(rec.mUuid), gAgentID, gain, LLAudioEngine::AUDIO_TYPE_AMBIENT, gAgent.getPosGlobalFromAgent(pos));
}

void WolfSwimBubbles::move(F32 dt, F64 now)
{
    for (Bubble& b : mBubbles)
    {
        b.mAge += dt;
        if (b.mPopping)
        {
            b.mPop -= dt;
            continue;
        }
        if (now - b.mSurfaceT >= SURFACE_SAMPLE_S)
        {
            b.mSurfaceT = now;
            F32 s = 0.f;
            if (!surfaceOf(b.mWater, b.mCentre, s) || !std::isfinite(s))
            {
                b.mPopping = true;   // out of its pool (it met the side)
                b.mPop = POP_S;
                continue;
            }
            b.mSurface = s;
        }
        // Boyle: the radius from the pressure at its depth now
        const F32 depth = b.mSurface - b.mCentre.mV[VZ];
        b.mR = b.mR0 * cbrtf(b.mP0 / pressure_at(depth));
        const F32 d = 2.f * b.mR;
        const F32 u = rise_speed(d);
        b.mCentre += b.mDrift * dt;
        b.mCentre.mV[VZ] += u * dt;
        b.mDrift *= expf(-dt / PUFF_DECAY_S);
        // the helix: amplitude A = (1.87 / 3) d, frequency f = St U / d
        b.mPhase += b.mTurn * F_TWO_PI * (STROUHAL * u / d) * dt;
        const F32 a = WOBBLE_A_PER_D * d;
        // the helix grows in over the first few centimetres of rise (Ellingsen & Risso: the path instability develops
        // as it rises; chosen: over its first 0.2 s)
        const F32 grow = llclamp(b.mAge / 0.2f, 0.f, 1.f);
        b.mPos = b.mCentre + LLVector3(cosf(b.mPhase), sinf(b.mPhase), 0.f) * (a * grow);
        if (b.mPos.mV[VZ] + b.mR >= b.mSurface || b.mAge > MAX_AGE_S || WolfDryVolumes::instance().contains(b.mPos))
        {
            b.mPopping = true;   // at the surface (or into the air of a tunnel or hull): it pops
            b.mPop = POP_S;
            if (b.mPos.mV[VZ] + b.mR > b.mSurface) b.mPos.mV[VZ] = b.mSurface - b.mR;
        }
    }
    mBubbles.erase(std::remove_if(mBubbles.begin(), mBubbles.end(),
                                  [](const Bubble& b) { return b.mPopping && b.mPop <= 0.f; }),
                   mBubbles.end());
}

void WolfSwimBubbles::idle()
{
    LLTimer cost;
    const F64 now = LLFrameTimer::getElapsedSeconds();
    const F32 dt = llclamp((F32)(now - mLastIdle), 0.f, 0.1f);
    mLastIdle = now;

    static LLCachedControl<bool> on(gSavedSettings, "WolfViewerSwimBubbles", true);
    const bool teleporting = gAgent.getTeleportState() != LLAgent::TELEPORT_NONE;
    if (!on || !WolfGrid::showsWolfRegion() || teleporting || !gAgent.getRegion())
    {
        if (!mBubbles.empty() || !mDivers.empty()) reset();   // the resident's switch, a teleport, off Wolf: at once
        return;
    }
    // The agent frame moves on a region crossing (LLAgent::setRegion) or a rebase (LLAgent::rebaseOrigin): every stored
    // agent-metre position moves with it (wolfbirds.cpp idle, the same rule).
    const LLVector3d origin = gAgent.getAgentOriginGlobal();
    if (origin != mLastOrigin)
    {
        if (!mLastOrigin.isExactlyZero()) rebase(LLVector3(mLastOrigin - origin));
        mLastOrigin = origin;
    }

    // this frame's builders' water planes
    mPlanes.clear();
    if (FSWolfWater::instanceExists()) FSWolfWater::instance().appendPlanes(mPlanes);
    if (WolfTerrainPaint::instanceExists()) WolfTerrainPaint::instance().appendWaterPlanes(mPlanes);
    mPlaneCentre.resize(mPlanes.size());
    mPlaneRadius2.resize(mPlanes.size());
    for (size_t i = 0; i < mPlanes.size(); ++i)
    {
        const LLVector3 half = mPlanes[i]->getScale() * 0.5f;
        const F32 r = LLVector3(half.mV[VX], half.mV[VY], 0.f).length() + llmax(mPlanes[i]->getBoundedWaterDepth(), 0.f) + half.mV[VZ];
        mPlaneCentre[i] = mPlanes[i]->getPositionAgent();
        mPlaneRadius2[i] = r * r;
    }

    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    const S32 cap = llclamp((S32)((F32)MAX_BUBBLES * WolfFrameGovernor::budget()), 0, MAX_BUBBLES);
    S32 divers = 0;
    for (LLCharacter* c : LLCharacter::sInstances)
    {
        LLVOAvatar* av = static_cast<LLVOAvatar*>(c);
        if (av->isDead() || av->isControlAvatar() || av->isUIAvatar() || !av->mHeadp) continue;
        // never for an avatar this resident does not see (review 2026-11-02): Do Not Render / derendered / too complex
        // (llvoavatar.cpp updateOverallAppearance: AOA_INVISIBLE) or blocked - its bubbles would show where it is
        if (av->getOverallAppearance() == LLVOAvatar::AOA_INVISIBLE || av->isInMuteList()) continue;
        if ((av->getPositionAgent() - cam).length() > REACH_M) continue;
        Diver& d = mDivers[av->getID()];
        d.mSeen = now;
        LLVector3 mouth, forward;
        F32 surface = 0.f;
        Water water;
        // the front of the head is measured only for an avatar whose mouth is under the water (review 2026-11-02)
        bool under = mouthOf(av, d, false, mouth, forward) && underWater(mouth, surface, water);
        if (under && (now >= d.mNextRig || d.mForward <= 0.f))
        {
            d.mNextRig = now + RIG_EVERY_S;
            under = mouthOf(av, d, true, mouth, forward) && underWater(mouth, surface, water);
        }
        // Hysteresis (review 2026-11-02): puffing starts once the mouth is ENTER_BELOW under, and stops only when it is
        // out of the water; a breath rhythm survives a short dip of the mouth above a wave (OUT_KEEP_S) so a swimmer at
        // the surface keeps breathing every few seconds, not on every re-entry.
        if (!under)
        {
            d.mBurstLeft = 0;   // a mouth out of the water makes no bubbles
            if (!d.mUnder)
            {
                d.mNextPuff = 0.0;
            }
            else if (d.mOutSince <= 0.0)
            {
                d.mOutSince = now;
            }
            else if (now - d.mOutSince > OUT_KEEP_S)
            {
                d.mUnder = false;
                d.mNextPuff = 0.0;
                d.mOutSince = 0.0;
            }
            continue;
        }
        d.mOutSince = 0.0;
        if (!d.mUnder)
        {
            if (surface - mouth.mV[VZ] < ENTER_BELOW) continue;
            d.mUnder = true;
        }
        ++divers;
        if (d.mNextPuff == 0.0) d.mNextPuff = now + frand_range(FIRST_PUFF_MIN_S, FIRST_PUFF_MAX_S);
        if (now >= d.mNextPuff)
        {
            const F32 speed = av->getVelocity().length();
            const F32 rest = 60.f / frand_range(BREATHS_MIN, BREATHS_MAX);
            d.mNextPuff = now + rest / (1.f + llclamp(speed / SWIM_SPEED, 0.f, 1.f));
            d.mBurstLeft = PUFF_MIN + ll_rand(PUFF_MAX - PUFF_MIN + 1);
            d.mBurstNext = now;
            static LLCachedControl<bool> sound(gSavedSettings, "WolfViewerSwimBubblesSound", true);
            if (sound) puffSound(mouth, av->isSelf(), now);
        }
        if (d.mBurstLeft > 0 && now >= d.mBurstNext)
        {
            --d.mBurstLeft;
            d.mBurstNext = now + BURST_GAP_S;
            emit(mouth, forward, surface, water, cap);
        }
    }
    for (auto it = mDivers.begin(); it != mDivers.end();)
    {
        it = (now - it->second.mSeen > 5.0) ? mDivers.erase(it) : std::next(it);
    }

    move(dt, now);
    // a cap lowered by the frame budget: the live bubbles furthest from the camera pop until it holds (review 2026-11-02)
    S32 live = 0;
    for (const Bubble& b : mBubbles) live += b.mPopping ? 0 : 1;
    while (live > cap)
    {
        Bubble* furthest = nullptr;
        F32 fd = -1.f;
        for (Bubble& b : mBubbles)
        {
            if (b.mPopping) continue;
            const F32 dd = dist_vec(b.mPos, cam);
            if (dd > fd) { fd = dd; furthest = &b; }
        }
        if (!furthest) break;
        furthest->mPopping = true;
        furthest->mPop = POP_S;
        --live;
    }

    mCostIdleUs += cost.getElapsedTimeF64() * 1e6;
    ++mCostFrames;
    mPeakBubbles = llmax(mPeakBubbles, (S32)mBubbles.size());
    mPeakDivers = llmax(mPeakDivers, divers);
    if (now - mLastLog >= 60.0)
    {
        if (mPeakDivers > 0 || mPeakBubbles > 0)
        {
            LL_INFOS("WolfSwimBubbles") << "last minute: up to " << mPeakDivers << " avatars under the water, up to "
                                        << mPeakBubbles << " bubbles (cap " << cap << "), "
                                        << (mCostFrames ? mCostIdleUs / mCostFrames : 0.0) << " us idle + "
                                        << (mCostFrames ? mCostMeshUs / mCostFrames : 0.0) << " us mesh a frame over "
                                        << mCostFrames << " frames" << LL_ENDL;
        }
        mLastLog = now;
        mCostIdleUs = mCostMeshUs = 0.0;
        mCostFrames = mPeakBubbles = mPeakDivers = 0;
    }
}

bool WolfSwimBubbles::ensureBuffer()
{
    if (mVB.notNull()) return true;
    // Source: wolfbirds.cpp ensureBuffer - the attributes WolfFarGround's program reads
    LLPointer<LLVertexBuffer> vb = new LLVertexBuffer(LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_NORMAL
                                                      | LLVertexBuffer::MAP_TEXCOORD0 | LLVertexBuffer::MAP_COLOR
                                                      | LLVertexBuffer::MAP_TEXTURE_INDEX);
    if (!vb->allocateBuffer(MAX_VERTS, MAX_VERTS))
    {
        LL_WARNS("WolfSwimBubbles") << "could not allocate the bubbles' vertex buffer" << LL_ENDL;
        return false;
    }
    LLStrider<U16> idx;
    if (!vb->getIndexStrider(idx))
    {
        vb->unmapBuffer();
        return false;
    }
    for (S32 i = 0; i < MAX_VERTS; ++i) *idx++ = (U16)i;
    vb->unmapBuffer();
    mVB = vb;
    return true;
}

void WolfSwimBubbles::buildMesh(S32& verts)
{
    verts = 0;
    if (mBubbles.empty() || !ensureBuffer()) return;
    LLTimer cost;
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    const LLVector3 cam = camera->getOrigin();
    mVisible.clear();
    for (size_t i = 0; i < mBubbles.size() && (S32)mVisible.size() < HARD_MAX; ++i)
    {
        if (camera->sphereInFrustum(mBubbles[i].mPos, mBubbles[i].mR * POP_SWELL)) mVisible.push_back((S32)i);
    }
    const S32 visible = (S32)mVisible.size();
    if (visible == 0) return;
    const S32 count = visible * VERTS_PER_BUBBLE;
    LLStrider<LLVector4a> pos;
    LLStrider<LLVector3> norm;
    LLStrider<LLVector2> uv;
    LLStrider<LLColor4U> col;
    if (!mVB->getVertexStrider(pos, 0, count) || !mVB->getNormalStrider(norm, 0, count)
        || !mVB->getTexCoord0Strider(uv, 0, count) || !mVB->getColorStrider(col, 0, count))
    {
        mVB->unmapBuffer();
        return;
    }
    const Ico& g = ico();
    S32 written = 0;
    for (S32 vi : mVisible)
    {
        const Bubble& b = mBubbles[(size_t)vi];
        const F32 r = b.mR * (b.mPopping ? POP_SWELL - (POP_SWELL - 1.f) * llclamp(b.mPop / POP_S, 0.f, 1.f) : 1.f);
        LLVector3 view = cam - b.mPos;
        if (!view.isExactlyZero()) view.normVec();
        for (S32 t = 0; t < TRIS_PER_BUBBLE; ++t)
        {
            for (S32 k = 0; k < 3; ++k)
            {
                const LLVector3& n = g.v[g.f[t][k]];
                const LLVector3 p = b.mPos + n * r;
                (*pos++).set(p.mV[VX], p.mV[VY], p.mV[VZ], 0.f);   // w = texture index 0 (wolffarground.cpp)
                *norm++ = n;
                *uv++ = LLVector2(0.5f, 0.5f);
                const F32 rim = 1.f - fabsf(n * view);
                const F32 m = rim * rim;
                *col++ = LLColor4U((U8)((CORE[0] + (RIM[0] - CORE[0]) * m) * 255.f), (U8)((CORE[1] + (RIM[1] - CORE[1]) * m) * 255.f),
                                   (U8)((CORE[2] + (RIM[2] - CORE[2]) * m) * 255.f), 255);
            }
        }
        ++written;
    }
    mVB->unmapBuffer();
    verts = written * VERTS_PER_BUBBLE;
    mCostMeshUs += cost.getElapsedTimeF64() * 1e6;
}

void WolfSwimBubbles::renderDeferred()
{
    if (mBubbles.empty()) return;
    S32 verts = 0;
    buildMesh(verts);
    if (verts == 0) return;
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    // Source: wolfbirds.cpp renderDeferred (from WolfFarGround::renderDeferred) - the same program, matrices and
    // attributes, so the bubbles are lit, fogged and seen through the water surface as the scene round them is.
    if (LLViewerFetchedTexture::sWhiteImagep.isNull()) return;
    LLGLDisable blend(GL_BLEND);
    LLGLEnable cull(GL_CULL_FACE);
    LLGLSLShader& shader = gDeferredNonIndexedDiffuseAlphaMaskProgram;
    shader.bind();
    shader.setMinimumAlpha(0.5f);
    gGLLastMatrix = nullptr;
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.loadMatrix(gGLModelView);
    gGL.getTexUnit(0)->bindFast(LLViewerFetchedTexture::sWhiteImagep);
    mVB->setBuffer();
    mVB->drawRange(LLRender::TRIANGLES, 0, (U32)(verts - 1), (U32)verts, 0);
    shader.unbind();
}
