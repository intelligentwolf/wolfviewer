/**
 * @file wolflightning.cpp
 * @brief WolfViewer: forks of lightning during a thunderstorm — and the thunder that follows them.
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
#include "wolflightning.h"

#include <cmath>
#include "llagent.h"
#include "llframetimer.h"
#include "llrand.h"
#include "llvector4a.h"
#include "llworld.h"
#include "pipeline.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerpartsim.h"
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "llvoavatar.h"        // LLViewerPartSource holds an LLPointer<LLVOAvatar>: the destructor needs the complete type
#include "wolfweather.h"
#include "wolfweathersound.h"

namespace
{
    // The channel. A real bolt is a random walk that keeps heading for the ground; the walk
    // below steps down a fixed share of the height each node and wanders sideways a bounded
    // amount, pulled back towards the foot so it lands where it was aimed.
    constexpr S32 NODES_MIN = 14, NODES_MAX = 22;
    constexpr F32 WANDER_M = 11.f;            // sideways jitter per node
    constexpr F32 BRANCH_WANDER_M = 9.f;
    constexpr S32 BRANCHES_MIN = 2, BRANCHES_MAX = 4;
    constexpr S32 BRANCH_NODES_MIN = 4, BRANCH_NODES_MAX = 8;
    constexpr F32 BRANCH_STEP_M = 9.f;        // a branch node's drop
    // Cloud base above the foot.
    constexpr F32 HEIGHT_MIN_M = 150.f, HEIGHT_MAX_M = 260.f;
    // How far away it strikes. NEAR is the "thunder" ambience, DISTANT the "distant" one.
    constexpr F32 NEAR_MIN_M = 120.f, NEAR_MAX_M = 450.f;
    constexpr F32 FAR_MIN_M  = 500.f, FAR_MAX_M  = 1400.f;
    // The bolt has to sit inside the draw distance or the particle groups are culled with
    // everything else beyond it (RenderFarClip). This share of it is the ceiling.
    constexpr F32 FAR_CLIP_SHARE = 0.85f;
    // Particle widths, metres. Roughly 4 px at 300 m on a 1080p frame: a line, not a blob.
    constexpr F32 MAIN_WIDTH_M = 2.2f, BRANCH_WIDTH_M = 1.3f;
    // The re-strikes down the same channel: seconds after the first flash, and each one's life.
    constexpr F32 FLASH_AT[3]   = { 0.f,   0.20f, 0.33f };
    constexpr F32 FLASH_LIFE[3] = { 0.14f, 0.07f, 0.10f };
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// WolfLightningPartSource
// ═══════════════════════════════════════════════════════════════════════════════════════

WolfLightningPartSource::WolfLightningPartSource()
    : LLViewerPartSource(LL_PART_SOURCE_NULL)   // as WolfWeatherPartSource (wolfweather.cpp:82)
{
}

void WolfLightningPartSource::update(const F32 dt)
{
    mAge += dt;
    if (!striking()) return;
    if (!mImagep)
    {
        mImagep = LLViewerFetchedTexture::sDefaultParticleImagep;
    }
    // The source's own position only matters for the simulator's bookkeeping: keep it at the
    // camera like the weather does, so the source is never "far away" and reaped.
    mPosAgent = LLViewerCamera::getInstance()->getOrigin();
    const F32 t = mAge - mBolt.mBorn;
    while (striking() && t >= FLASH_AT[mBolt.mFlash])
    {
        const F32 life = FLASH_LIFE[mBolt.mFlash];
        // The first flash is the bright one; the re-strikes are dimmer, as they are.
        const F32 alpha = (mBolt.mFlash == 0) ? 1.f : 0.75f;
        emitLine(mBolt.mChannel, MAIN_WIDTH_M, life, alpha);
        for (const auto& br : mBolt.mBranches)
        {
            emitLine(br, BRANCH_WIDTH_M, life * 0.8f, alpha * 0.8f);
        }
        ++mBolt.mFlash;
    }
}

void WolfLightningPartSource::emitLine(const std::vector<LLVector3>& pts, F32 width, F32 life, F32 alpha)
{
    LLViewerPartSim* sim = LLViewerPartSim::getInstance();
    for (size_t i = 0; i + 1 < pts.size(); ++i)
    {
        const LLVector3 a = pts[i], b = pts[i + 1];
        const LLVector3 d = b - a;
        const F32 len = d.length();
        const S32 n = llmax(1, (S32)ceilf(len / STEP_M));
        for (S32 k = 0; k < n; ++k)
        {
            if (!LLViewerPartSim::shouldAddPart()) return;
            LLViewerPart* part = new LLViewerPart();
            part->init(this, mImagep, NULL);
            part->mBlendFuncDest = LLRender::BF_ONE_MINUS_SOURCE_ALPHA;
            part->mBlendFuncSource = LLRender::BF_SOURCE_ALPHA;
            part->mLastUpdateTime = 0.f;
            part->mPosAgent = a + d * ((F32)k / (F32)n);
            part->mVelocity = LLVector3::zero;
            part->mAccel = LLVector3::zero;
            part->mMaxAge = life;
            part->mScale.set(width, width);
            part->mStartScale = part->mScale;
            part->mEndScale = part->mScale;
            // A cold blue-white core. EMISSIVE: "instead of being lit" (llpartdata.h:115) —
            // this is the one weather particle that IS its own light.
            part->mStartColor = LLColor4(0.90f, 0.94f, 1.0f, alpha);
            part->mEndColor   = LLColor4(0.70f, 0.80f, 1.0f, 0.f);
            part->mColor = part->mStartColor;
            // A little glow for the bloom pass so the core reads as hot, not as a white line.
            part->mStartGlow = 0.35f;
            part->mEndGlow = 0.f;
            part->mGlow = LLColor4U(0, 0, 0, (U8)(part->mStartGlow * 255.f));
            part->mFlags = LLViewerPart::LL_PART_INTERP_COLOR_MASK | LLViewerPart::LL_PART_EMISSIVE_MASK;
            part->mParameter = 0.f;
            sim->addPart(part);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// WolfLightning
// ═══════════════════════════════════════════════════════════════════════════════════════

WolfLightning::WolfLightning() {}
WolfLightning::~WolfLightning() {}

// Storming = the weather switch is on, RAIN is what is falling, and the profile's ambience is
// one of the two thunder beds. Source: wolfweathersound.cpp maybeThunder (2026-09-12) — the
// same test the sound used, so a storm without sound still forks (the visual is not a sound).
bool WolfLightning::storming(bool& distant) const
{
    distant = false;
    if (!WolfWeather::enabled()) return false;
    static LLCachedControl<bool> lightning_on(gSavedSettings, "WolfViewerLightning", true);
    if (!lightning_on) return false;   // the switch: thunder still rolls (the sound engine's own), nothing is drawn
    const WolfWeatherProfile& p = WolfWeather::instance().activeProfile();
    if (p.mKind != WolfWeatherProfile::RAIN) return false;
    if (p.mSoundPreset == "distant") { distant = true; return true; }
    return p.mSoundPreset == "thunder";
}

void WolfLightning::reset()
{
    mNextStrike = 0.0;
    mClapPending = false;
    mWasStorming = false;
    if (mSource.notNull())
    {
        mSource->setDead();
        mSource = nullptr;
    }
}

void WolfLightning::idle()
{
    const F64 now = LLFrameTimer::getElapsedSeconds();
    bool distant = false;
    const bool storm = storming(distant);
    if (!storm)
    {
        if (mWasStorming) reset();
        return;
    }
    if (!mWasStorming || distant != mWasDistant)
    {
        // Do not strike the instant the rain starts: that reads as a bug, not a storm.
        // Source: wolfweathersound.cpp apply (2026-09-12), the same rule for the first crack.
        mNextStrike = now + (distant ? DISTANT_MIN_SECS : THUNDER_MIN_SECS);
        mWasStorming = true;
        mWasDistant = distant;
    }
    // The clap that is still travelling.
    if (mClapPending && now >= mClapAt)
    {
        mClapPending = false;
        WolfWeatherSound::instance().crack(mClapDistant);
    }
    if (now >= mNextStrike)
    {
        mNextStrike = now + (distant ? DISTANT_MIN_SECS : THUNDER_MIN_SECS)
                          + ll_frand((F32)(distant ? DISTANT_SPAN_SECS : THUNDER_SPAN_SECS));
        strike(distant);
    }
}

void WolfLightning::strike(bool distant)
{
    WolfLightningPartSource::Bolt bolt;
    F32 distance = distant ? FAR_MIN_M : NEAR_MIN_M;
    const bool built = buildBolt(distant, bolt, distance);
    // THE GOLDEN RULE: nothing flashes inside a building. Under a roof the bolt is not drawn;
    // the thunder still comes, as it does indoors.
    const bool indoors = WolfWeather::instance().cameraUnderRoof();
    if (built && !indoors)
    {
        if (mSource.isNull() || mSource->isDead())
        {
            mSource = new WolfLightningPartSource();
            LLViewerPartSim::getInstance()->addPartSource(mSource);
        }
        mSource->strike(bolt);
    }
    // The clap arrives when the sound does. A bolt that was not drawn (indoors, or beyond the
    // draw distance) still had a distance, so the delay is honest either way.
    mClapDistant = distant;
    mClapAt = LLFrameTimer::getElapsedSeconds() + (F64)(distance / SOUND_MPS);
    mClapPending = true;
    LL_DEBUGS("WolfLightning") << "strike " << (distant ? "distant" : "near") << " at " << distance
                               << " m, drawn=" << (built && !indoors) << " indoors=" << indoors << LL_ENDL;
}

// The bolt's foot is the first surface under the cloud at the chosen spot: a roof, a deck, the
// ground, the water — the rain's own ray (wolfweather.cpp updateLanding), so lightning obeys
// the same rule the rain does and never reaches a floor that has a roof over it.
bool WolfLightning::buildBolt(bool distant, WolfLightningPartSource::Bolt& out, F32& distance) const
{
    LLViewerRegion* agent_rgn = gAgent.getRegion();
    if (!agent_rgn) return false;
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    static LLCachedControl<F32> far_clip(gSavedSettings, "RenderFarClip", 128.f);
    const F32 ceiling = (F32)far_clip * FAR_CLIP_SHARE;
    // Where it strikes: a distance in the ambience's band, then held inside the draw distance.
    F32 want = distant ? (FAR_MIN_M + ll_frand(FAR_MAX_M - FAR_MIN_M))
                       : (NEAR_MIN_M + ll_frand(NEAR_MAX_M - NEAR_MIN_M));
    distance = want;
    if (want > ceiling)
    {
        // A DISTANT storm that the draw distance cannot show stays a sound. A NEAR one is
        // brought in to the edge of what can be seen.
        if (distant && ceiling < FAR_MIN_M * 0.5f) return false;
        want = llmax(60.f, ceiling);
    }
    const F32 az = ll_frand(F_TWO_PI);
    const F32 fx = cam.mV[VX] + cosf(az) * want;
    const F32 fy = cam.mV[VY] + sinf(az) * want;
    // Ground under the foot, and the first surface above it along a vertical ray.
    F32 foot_z = LLWorld::getInstance()->resolveLandHeightAgent(LLVector3(fx, fy, cam.mV[VZ]));
    LLViewerRegion* foot_rgn = LLWorld::getInstance()->getRegionFromPosAgent(LLVector3(fx, fy, foot_z));
    const F32 water = (foot_rgn ? foot_rgn : agent_rgn)->getWaterHeight();
    if (foot_z < water) foot_z = water;
    const F32 height = HEIGHT_MIN_M + ll_frand(HEIGHT_MAX_M - HEIGHT_MIN_M);
    {
        LLVector4a start, end, hit;
        const LLVector3 s3(fx, fy, foot_z + height), e3(fx, fy, foot_z - 1.f);
        start.load3(s3.mV);
        end.load3(e3.mV);
        // Same arguments as the rain's roof test (wolfweather.cpp updateLanding): unselectable
        // builds count, transparent ones do not stop the ray.
        if (gPipeline.lineSegmentIntersectInWorld(start, end, true, false, true, false, NULL, NULL, NULL, &hit, NULL, NULL, NULL))
        {
            const F32 hz = hit.getF32ptr()[2];
            if (hz > foot_z) foot_z = hz;
        }
    }
    const LLVector3 foot(fx, fy, foot_z);
    const LLVector3 top(fx + ll_frand(2.f * WANDER_M * 3.f) - WANDER_M * 3.f,
                        fy + ll_frand(2.f * WANDER_M * 3.f) - WANDER_M * 3.f,
                        foot_z + height);
    // The channel: a random walk from the cloud to the foot.
    const S32 nodes = NODES_MIN + (S32)ll_frand((F32)(NODES_MAX - NODES_MIN + 1));
    out.mChannel.clear();
    out.mChannel.reserve(nodes + 1);
    LLVector3 p = top;
    out.mChannel.push_back(p);
    for (S32 i = 1; i <= nodes; ++i)
    {
        const F32 f = (F32)i / (F32)nodes;               // 0 at the cloud, 1 at the foot
        // Aim: the straight line's point at this height, pulled to by the walk.
        const LLVector3 aim = top + (foot - top) * f;
        p.mV[VX] = aim.mV[VX] + (p.mV[VX] - aim.mV[VX]) * 0.55f + ll_frand(2.f * WANDER_M) - WANDER_M;
        p.mV[VY] = aim.mV[VY] + (p.mV[VY] - aim.mV[VY]) * 0.55f + ll_frand(2.f * WANDER_M) - WANDER_M;
        p.mV[VZ] = aim.mV[VZ];
        if (i == nodes) p = foot;                        // it lands where it was aimed
        out.mChannel.push_back(p);
    }
    // Branches: from a node in the upper two thirds, stepping down and outwards, and stopping
    // short of the ground — a branch that reaches down is a second channel, which is rarer.
    out.mBranches.clear();
    const S32 branches = BRANCHES_MIN + (S32)ll_frand((F32)(BRANCHES_MAX - BRANCHES_MIN + 1));
    for (S32 b = 0; b < branches; ++b)
    {
        const S32 from = 1 + (S32)ll_frand((F32)llmax(1, nodes * 2 / 3));
        std::vector<LLVector3> br;
        LLVector3 q = out.mChannel[llmin(from, (S32)out.mChannel.size() - 1)];
        br.push_back(q);
        const F32 dir = ll_frand(F_TWO_PI);
        const S32 bn = BRANCH_NODES_MIN + (S32)ll_frand((F32)(BRANCH_NODES_MAX - BRANCH_NODES_MIN + 1));
        for (S32 i = 0; i < bn; ++i)
        {
            q.mV[VX] += cosf(dir) * BRANCH_STEP_M * 0.8f + ll_frand(2.f * BRANCH_WANDER_M) - BRANCH_WANDER_M;
            q.mV[VY] += sinf(dir) * BRANCH_STEP_M * 0.8f + ll_frand(2.f * BRANCH_WANDER_M) - BRANCH_WANDER_M;
            q.mV[VZ] -= BRANCH_STEP_M * (0.6f + ll_frand(0.8f));
            if (q.mV[VZ] <= foot_z + 4.f) break;
            br.push_back(q);
        }
        if (br.size() >= 2) out.mBranches.push_back(br);
    }
    out.mFlashes = 2 + (S32)ll_frand(2.f);   // 2 or 3 flashes down the channel
    out.mFlash = 0;
    return true;
}
