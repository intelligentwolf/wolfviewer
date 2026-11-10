/**
 * @file wolfleaves.cpp
 * @brief WolfViewer: a few leaves falling from the trees whose description says "wolftree". See wolfleaves.h.
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
#include "wolfleaves.h"

#include <algorithm>
#include <cmath>

#include "llagent.h"
#include "lldate.h"
#include "llframetimer.h"
#include "llrand.h"
#include "llrender.h"
#include "llvertexbuffer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "llworld.h"
#include "pipeline.h"
#include "wolfframegovernor.h"
#include "wolfgrid.h"
#include "wolfobjectprops.h"
#include "wolfregionweather.h"
#include "wolfweather.h"
#include "wolfweatherprofile.h"

const std::string WolfLeaves::KEYWORD("wolftree");

namespace
{
    // ── the trees ────────────────────────────────────────────────────────────────────────────────────────────────
    // The trees are looked for this often (as FSWolfWater's sweep, fswolfwater.cpp SWEEP_INTERVAL_SECS 1.5, a little slower:
    // a tree does not come and go).
    constexpr F64 SWEEP_S = 2.0;
    // Trees whose box comes within this of the camera drop leaves. Source: fswolfwater.h NEAR_M (64 m) - every prim this
    // close is already asked about by the wolfwater sweep (inside the draw distance, as here), so the leaves add no requests
    // of their own. A 12 cm leaf at 64 m is about 2 pixels of a 1080p screen at 60 degrees: past it nothing is seen.
    constexpr F32 TREE_REACH_M = 64.f;
    // At most this many trees, the nearest (chosen: a wood is still a wood with 32 trees dropping leaves round you).
    constexpr size_t MAX_TREES = 32;
    // A tree's share of leaves goes with the area under its crown, against a tree this size (chosen: a 5.5 m crown),
    // within these bounds so a bush still drops the odd leaf and a giant does not snow them.
    constexpr F32 TREE_AREA_M2 = 30.f, TREE_SIZE_MIN = 0.3f, TREE_SIZE_MAX = 2.5f;
    // The most leaves a tree of TREE_AREA_M2 drops, a second, in the middle of autumn (chosen: "subtle" but seen - in-world
    // 2026-11-10 one every two seconds left a 24 m larch with a leaf or two in sight at a time; a few in spring and summer,
    // wolfleavesseason.h RATE_*).
    constexpr F32 MOST_PER_TREE_S = 1.5f;
    // The leaves are born in this part of the tree's height and footprint (chosen: the crown of a typical tree).
    constexpr F32 CROWN_LOW = 0.45f, CROWN_HIGH = 0.95f, CROWN_SPREAD = 0.8f;

    // ── the leaves ───────────────────────────────────────────────────────────────────────────────────────────────
    // At most this many by default (WolfViewerFallingLeavesMax, 0 .. HARD_MAX), lowered by WolfFrameGovernor::budget.
    constexpr S32 DEFAULT_MAX = 300, HARD_MAX = 600;
    // A leaf's length; its width is a share of it (chosen: an oak or beech leaf is 8-12 cm; a little larger is still seen
    // at the distances the leaves fall).
    constexpr F32 LEN_MIN_M = 0.10f, LEN_MAX_M = 0.16f, WID_MIN = 0.55f, WID_MAX = 0.7f;
    // It falls at about a metre a second (chosen: a leaf's flutter keeps it slow), swinging side to side a little.
    constexpr F32 FALL_MIN = 0.8f, FALL_MAX = 1.4f;
    constexpr F32 AMP_MIN_M = 0.15f, AMP_MAX_M = 0.45f, PERIOD_MIN_S = 1.6f, PERIOD_MAX_S = 3.0f;
    // It rocks with the swing up to this angle from flat (chosen: about 45 degrees).
    constexpr F32 TILT_MAX = 0.8f;
    // The region's wind carries it this share of the wind's speed, at most this fast (chosen: a gentle drift that keeps the
    // leaves round their tree; a sim's wind can be 10 m/s, and in-world 2026-11-10 0.3 of it, up to 2.5 m/s, carried them
    // 40-50 m away from a 24 m larch before they landed).
    constexpr F32 WIND_FOLLOW = 0.15f, WIND_MAX = 0.5f;
    // Where it lands: the ground, the water or the tree's base, looked up this often (chosen; the birds' floating gulls
    // take 20 Hz, a leaf only needs to know when it arrives).
    constexpr F64 GROUND_SAMPLE_S = 0.25;
    // Settled, it lies this long and shrinks away in SHRINK_S (chosen). Lying a little above the ground keeps it out of it.
    constexpr F32 REST_MIN_S = 8.f, REST_MAX_S = 14.f, SHRINK_S = 1.2f, LIE_ABOVE_M = 0.015f;
    // A leaf still falling after this long (a tree over a deep drop) is let go (chosen: about 50 m of fall).
    constexpr F32 MAX_FALL_S = 45.f;
    // The season is worked out again this often (it changes by the day; a region crossing brings a new place).
    constexpr F64 SEASON_S = 10.0;

    // ── the look ─────────────────────────────────────────────────────────────────────────────────────────────────
    // The outlines: wolfleavesseason.h OUTLINES (oak, beech; a needle is the beech outline drawn thin).
    constexpr S32 SIDES = WolfLeavesSeason::OUTLINE_POINTS;
    constexpr S32 VERTS_PER_LEAF = SIDES * 3 * 2;   // a fan of 14 triangles, front and back
    constexpr S32 MAX_VERTS = HARD_MAX * VERTS_PER_LEAF;   // 50400, U16 indices
    static_assert(MAX_VERTS <= 65536, "the leaves' buffer is indexed with U16");
    // A conifer needle's length (chosen: a pine or spruce needle is 2-7 cm)
    constexpr F32 NEEDLE_LEN_MIN_M = 0.04f, NEEDLE_LEN_MAX_M = 0.07f;
    constexpr F32 VEIN_SHADE = 0.8f, BACK_SHADE = 0.9f;   // the centre a little darker; the underside a little paler

    F32 frand_range(F32 lo, F32 hi) { return lo + ll_frand(hi - lo); }

    /** Grow a box by a prim's own box: its eight corners, rotated, in agent metres. */
    void add_prim_box(const LLViewerObject* o, LLVector3& lo, LLVector3& hi, bool& any)
    {
        const LLVector3 half = o->getScale() * 0.5f;
        const LLQuaternion rot = o->getRotationRegion();
        const LLVector3 c = o->getPositionAgent();
        for (S32 i = 0; i < 8; ++i)
        {
            const LLVector3 p = c + LLVector3((i & 1) ? half.mV[VX] : -half.mV[VX], (i & 2) ? half.mV[VY] : -half.mV[VY],
                                              (i & 4) ? half.mV[VZ] : -half.mV[VZ]) * rot;
            if (!any)
            {
                lo = hi = p;
                any = true;
                continue;
            }
            lo.mV[VX] = llmin(lo.mV[VX], p.mV[VX]); lo.mV[VY] = llmin(lo.mV[VY], p.mV[VY]); lo.mV[VZ] = llmin(lo.mV[VZ], p.mV[VZ]);
            hi.mV[VX] = llmax(hi.mV[VX], p.mV[VX]); hi.mV[VY] = llmax(hi.mV[VY], p.mV[VY]); hi.mV[VZ] = llmax(hi.mV[VZ], p.mV[VZ]);
        }
    }
}

WolfLeaves::WolfLeaves() {}
WolfLeaves::~WolfLeaves() {}

// static
bool WolfLeaves::matches(const std::string& description_lower)
{
    return !description_lower.empty() && description_lower.find(KEYWORD) != std::string::npos;
}

void WolfLeaves::reset()
{
    mLeaves.clear();
    mTrees.clear();
    mCarry.clear();
    mNextSweep = 0.0;
    mNextSeason = 0.0;
}

void WolfLeaves::rebase(const LLVector3& delta)
{
    for (Leaf& l : mLeaves)
    {
        l.mCentre += delta;
        l.mPos += delta;
        l.mFloor += delta.mV[VZ];
        l.mGround += delta.mV[VZ];
    }
    for (Tree& t : mTrees)
    {
        t.mMin += delta;
        t.mMax += delta;
    }
}

void WolfLeaves::updateSeason(F64 now)
{
    mNextSeason = now + SEASON_S;
    F64 lat = 0.0;
    bool place = false;
    F64 when = LLDate::now().secondsSinceEpoch();
    if (WolfRegionWeather::instanceExists())
    {
        const WolfRegionWeather& rw = WolfRegionWeather::instance();
        place = rw.autoEnvPlaceLat(lat);
        when = rw.autoEnvSkyTime();   // the moment the region's sky shows (now + skyShift, + a Weather tab preview)
    }
    S32 day = WolfLeavesSeason::dayOfYearUTC(when);
    // A day of the year to use instead of the sky's (1 .. 366; 0 = the sky's own), for checking the seasons in-world.
    static LLCachedControl<S32> test_day(gSavedSettings, "WolfViewerFallingLeavesTestDay", 0);
    if (test_day >= 1 && test_day <= 366) day = test_day;
    mSeason = WolfLeavesSeason::at(place, lat, day);
    // Not while snow falls on screen (WolfWeatherProfile::SNOW, any level): a snowy day is a winter's day.
    mSnowing = WolfWeather::instanceExists() && WolfWeather::enabled()
               && WolfWeather::instance().activeProfile().mKind == WolfWeatherProfile::SNOW;
    const S32 kind = mSnowing ? -2 : (S32)mSeason.kind;
    if (kind != mLastSeasonKind)
    {
        mLastSeasonKind = kind;
        LL_INFOS("WolfLeaves") << "season: " << WolfLeavesSeason::name(mSeason.kind) << " (day " << day
                               << (test_day >= 1 && test_day <= 366 ? " - WolfViewerFallingLeavesTestDay" : "") << ", "
                               << (place ? "latitude " + llformat("%.1f", lat) : std::string("no place: the northern calendar"))
                               << "), leaves at " << mSeason.rate << " of the most, autumn progress " << mSeason.progress
                               << (mSnowing ? "; snow is falling: no leaves" : "") << LL_ENDL;
    }
}

void WolfLeaves::sweep(F64 now)
{
    mNextSweep = now + SWEEP_S;
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    // Source: fswolfwater.cpp sweep - only what is inside the draw distance
    static LLCachedControl<F32> draw_distance(gSavedSettings, "RenderFarClip", 128.f);
    const F32 reach = llmin(TREE_REACH_M, (F32)draw_distance);
    WolfObjectProps& props = WolfObjectProps::instance();

    struct Hit
    {
        LLViewerObject* mObj;
        F32 mDist;
    };
    std::vector<Hit> hits;
    S32 swept = 0, known = 0;
    const S32 count = gObjectList.getNumObjects();
    for (S32 i = 0; i < count; ++i)
    {
        LLViewerObject* o = gObjectList.getObject(i);
        // Source: fswolfwater.cpp sweep - prims only: not avatars, attachments, HUDs or the water planes it makes itself
        if (!o || o->isDead() || o->isAvatar() || o->isHUDAttachment() || o->isAttachment()) continue;
        if (o->getPCode() != LL_PCODE_VOLUME) continue;
        const F32 dist = dist_vec(o->getPositionAgent(), cam) - o->getScale().length() * 0.5f;
        if (dist > reach) continue;
        ++swept;
        const WolfObjectProps::Props* p = props.get(o->getID());
        const bool hit = p && matches(p->mDescriptionLower);
        // want() is idempotent; a tree is kept fresh so a script or an edit that takes the word out stops it
        props.want(o, hit);
        if (p) ++known;
        if (hit) hits.push_back({ o, dist });
    }
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.mDist < b.mDist; });
    if (hits.size() > MAX_TREES) hits.resize(MAX_TREES);

    mTrees.clear();
    std::map<LLUUID, F32> carry;
    for (const Hit& h : hits)
    {
        Tree t;
        t.mId = h.mObj->getID();
        t.mDist = h.mDist;
        bool any = false;
        add_prim_box(h.mObj, t.mMin, t.mMax, any);
        // the root of a linkset brings the whole tree (its trunk and crown prims); not the avatars sitting on it
        t.mWhole = h.mObj->isRoot();
        if (const WolfObjectProps::Props* p = WolfObjectProps::instance().get(t.mId))
        {
            std::string name = p->mName;
            LLStringUtil::toLower(name);
            t.mKind = WolfLeavesSeason::treeKind(p->mDescriptionLower, name);
        }
        if (t.mWhole)
        {
            for (LLViewerObject* c : h.mObj->getChildren())
            {
                if (c && !c->isDead() && !c->isAvatar()) add_prim_box(c, t.mMin, t.mMax, any);
            }
        }
        if (!any || t.mMax.mV[VZ] - t.mMin.mV[VZ] < 0.1f) continue;
        const F32 area = (t.mMax.mV[VX] - t.mMin.mV[VX]) * (t.mMax.mV[VY] - t.mMin.mV[VY]) * CROWN_SPREAD * CROWN_SPREAD;
        t.mSize = llclamp(area / TREE_AREA_M2, TREE_SIZE_MIN, TREE_SIZE_MAX);
        mTrees.push_back(t);
        auto it = mCarry.find(t.mId);
        carry[t.mId] = it != mCarry.end() ? it->second : ll_frand();   // a new tree starts part-way to its first leaf
    }
    mCarry.swap(carry);
    mSwept = swept;
    mKnown = known;
}

F32 WolfLeaves::groundAt(const LLVector3& p, F32 floor) const
{
    F32 g = LLWorld::getInstance()->resolveLandHeightAgent(p);
    if (LLViewerRegion* r = gAgent.getRegion()) g = llmax(g, r->getWaterHeight());   // the sea: it floats there
    return llmax(g, floor);
}

void WolfLeaves::spawn(const Tree& t)
{
    Leaf l;
    F32 u = 0.f, v = 0.f;
    do
    {
        u = ll_frand(2.f) - 1.f;
        v = ll_frand(2.f) - 1.f;
    } while (u * u + v * v > 1.f);   // a round crown in the box
    const LLVector3 c = (t.mMin + t.mMax) * 0.5f;
    const LLVector3 half = (t.mMax - t.mMin) * 0.5f;
    const F32 h = t.mMax.mV[VZ] - t.mMin.mV[VZ];
    l.mCentre.setVec(c.mV[VX] + u * half.mV[VX] * CROWN_SPREAD, c.mV[VY] + v * half.mV[VY] * CROWN_SPREAD,
                     t.mMin.mV[VZ] + h * frand_range(CROWN_LOW, CROWN_HIGH));
    l.mPos = l.mCentre;
    const F32 a = ll_frand(F_TWO_PI);
    l.mSwing.setVec(cosf(a), sinf(a), 0.f);
    l.mFall = frand_range(FALL_MIN, FALL_MAX);
    l.mAmp = frand_range(AMP_MIN_M, AMP_MAX_M);
    l.mOmega = F_TWO_PI / frand_range(PERIOD_MIN_S, PERIOD_MAX_S);
    l.mPhase = ll_frand(F_TWO_PI);
    l.mYaw = ll_frand(F_TWO_PI);
    l.mYawRate = frand_range(-0.6f, 0.6f);
    const bool needle = t.mKind == WolfLeavesSeason::CONIFER;
    l.mShape = needle ? WolfLeavesSeason::NEEDLE : (ll_frand() < 0.5f ? WolfLeavesSeason::OAK : WolfLeavesSeason::BEECH);
    l.mLen = needle ? frand_range(NEEDLE_LEN_MIN_M, NEEDLE_LEN_MAX_M) : frand_range(LEN_MIN_M, LEN_MAX_M);
    l.mWid = l.mLen * (needle ? WolfLeavesSeason::NEEDLE_WIDTH : frand_range(WID_MIN, WID_MAX));
    l.mTilt = frand_range(-0.25f, 0.25f);
    // The tree's own base is a floor only for a whole tree (a one-prim mesh tree or a linkset root): its box reaches the
    // trunk's foot. Any other prim's box is part of a tree, and its bottom may be the crown's.
    l.mFloor = t.mWhole ? t.mMin.mV[VZ] : -F32_MAX;
    l.mGround = groundAt(l.mCentre, l.mFloor);
    l.mGroundT = mLastIdle;
    // the colour: one of the season's, a little lighter or darker than the next
    F32 pick = ll_frand();
    S32 k = WolfLeavesSeason::COLOURS - 1;
    for (S32 i = 0; i < WolfLeavesSeason::COLOURS; ++i)
    {
        if (pick < mSeason.weights[i]) { k = i; break; }
        pick -= mSeason.weights[i];
    }
    const F32 shade = frand_range(0.85f, 1.1f);
    const F32* rgb = WolfLeavesSeason::PALETTE[k];
    if (needle) rgb = WolfLeavesSeason::NEEDLE_PALETTE[ll_rand(WolfLeavesSeason::NEEDLE_COLOURS)];
    for (S32 i = 0; i < 3; ++i) l.mColour[i] = (U8)llclamp(rgb[i] * shade * 255.f, 0.f, 255.f);
    mLeaves.push_back(l);
}

void WolfLeaves::move(F32 dt, F64 now)
{
    LLViewerRegion* region = gAgent.getRegion();
    for (Leaf& l : mLeaves)
    {
        l.mAge += dt;
        if (l.mSettled)
        {
            if (l.mRest > 0.f) l.mRest -= dt;
            else l.mShrink -= dt / SHRINK_S;
            continue;
        }
        if (now - l.mGroundT >= GROUND_SAMPLE_S)
        {
            l.mGroundT = now;
            l.mGround = groundAt(l.mPos, l.mFloor);
        }
        // the region's wind where it is (llwind.h getVelocity: region metres), a share of it, capped
        LLVector3 wind;
        if (region)
        {
            wind = region->mWind.getVelocity(region->getPosRegionFromAgent(l.mPos)) * WIND_FOLLOW;
            wind.mV[VZ] = 0.f;
            const F32 s = wind.length();
            if (s > WIND_MAX) wind *= WIND_MAX / s;
        }
        l.mPhase += l.mOmega * dt;
        l.mYaw += l.mYawRate * dt;
        l.mCentre += wind * dt;
        l.mCentre.mV[VZ] -= l.mFall * dt;
        // a pendulum's path: across with the swing, a little up at each end of it
        l.mPos = l.mCentre + l.mSwing * (l.mAmp * sinf(l.mPhase));
        l.mPos.mV[VZ] += 0.15f * l.mAmp * (1.f - cosf(2.f * l.mPhase));
        if (l.mPos.mV[VZ] <= l.mGround + LIE_ABOVE_M)
        {
            l.mPos.mV[VZ] = l.mGround + LIE_ABOVE_M;
            l.mSettled = true;
            l.mRest = frand_range(REST_MIN_S, REST_MAX_S);
        }
        else if (l.mAge > MAX_FALL_S)
        {
            l.mSettled = true;   // shrinks away where it is
            l.mRest = 0.f;
        }
    }
    mLeaves.erase(std::remove_if(mLeaves.begin(), mLeaves.end(), [](const Leaf& l) { return l.mShrink <= 0.f; }), mLeaves.end());
}

void WolfLeaves::idle()
{
    LLTimer cost;
    const F64 now = LLFrameTimer::getElapsedSeconds();
    const F32 dt = llclamp((F32)(now - mLastIdle), 0.f, 0.1f);
    mLastIdle = now;

    static LLCachedControl<bool> on(gSavedSettings, "WolfViewerFallingLeaves", true);
    const bool teleporting = gAgent.getTeleportState() != LLAgent::TELEPORT_NONE;
    if (!on || !WolfGrid::showsWolfRegion() || teleporting || !gAgent.getRegion())
    {
        if (!mLeaves.empty() || !mTrees.empty()) reset();   // the resident's switch, a teleport, off Wolf: at once
        return;
    }
    // The agent frame moves on a region crossing or a rebase (wolfswimbubbles.cpp idle, the same rule)
    const LLVector3d origin = gAgent.getAgentOriginGlobal();
    if (origin != mLastOrigin)
    {
        if (!mLastOrigin.isExactlyZero()) rebase(LLVector3(mLastOrigin - origin));
        mLastOrigin = origin;
    }
    if (now >= mNextSeason) updateSeason(now);
    if (now >= mNextSweep) sweep(now);

    static LLCachedControl<S32> max_setting(gSavedSettings, "WolfViewerFallingLeavesMax", DEFAULT_MAX);
    const S32 cap = llclamp((S32)((F32)llclamp((S32)max_setting, 0, HARD_MAX) * WolfFrameGovernor::budget()), 0, HARD_MAX);
    if (!mSnowing && mSeason.rate > 0.f)
    {
        for (const Tree& t : mTrees)
        {
            F32& carry = mCarry[t.mId];
            const F32 rate = t.mKind == WolfLeavesSeason::CONIFER ? WolfLeavesSeason::needleRate(mSeason.kind) : mSeason.rate;
            carry = llmin(carry + MOST_PER_TREE_S * rate * t.mSize * dt, 2.f);   // never a burst after a stall
            while (carry >= 1.f)
            {
                carry -= 1.f;
                if ((S32)mLeaves.size() < cap) spawn(t);
            }
        }
    }
    move(dt, now);

    mCostIdleUs += cost.getElapsedTimeF64() * 1e6;
    ++mCostFrames;
    mPeakLeaves = llmax(mPeakLeaves, (S32)mLeaves.size());
    mPeakTrees = llmax(mPeakTrees, (S32)mTrees.size());
    // a line every 30 s while there are trees about (none at all otherwise): what the tests read
    if (now - mLastLog >= 30.0)
    {
        if (mPeakTrees > 0 || mPeakLeaves > 0)
        {
            S32 conifers = 0;
            for (const Tree& t : mTrees) conifers += t.mKind == WolfLeavesSeason::CONIFER ? 1 : 0;
            LL_INFOS("WolfLeaves") << "last 30 s: " << mSwept << " prims within reach, " << mKnown << " descriptions known, up to "
                                   << mPeakTrees << " wolftree trees (now " << conifers << " of " << mTrees.size()
                                   << " conifers), up to " << mPeakLeaves << " leaves (cap " << cap << "), "
                                   << WolfLeavesSeason::name(mSeason.kind) << (mSnowing ? " (snowing)" : "") << ", "
                                   << (mCostFrames ? mCostIdleUs / mCostFrames : 0.0) << " us idle + "
                                   << (mCostFrames ? mCostMeshUs / mCostFrames : 0.0) << " us mesh a frame" << LL_ENDL;
        }
        mLastLog = now;
        mCostIdleUs = mCostMeshUs = 0.0;
        mCostFrames = mPeakLeaves = mPeakTrees = 0;
    }
}

bool WolfLeaves::ensureBuffer()
{
    if (mVB.notNull()) return true;
    // Source: wolfswimbubbles.cpp ensureBuffer (wolfbirds.cpp's) - the attributes WolfFarGround's program reads
    LLPointer<LLVertexBuffer> vb = new LLVertexBuffer(LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_NORMAL
                                                      | LLVertexBuffer::MAP_TEXCOORD0 | LLVertexBuffer::MAP_COLOR
                                                      | LLVertexBuffer::MAP_TEXTURE_INDEX);
    if (!vb->allocateBuffer(MAX_VERTS, MAX_VERTS))
    {
        LL_WARNS("WolfLeaves") << "could not allocate the leaves' vertex buffer" << LL_ENDL;
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

void WolfLeaves::buildMesh(S32& verts)
{
    verts = 0;
    if (mLeaves.empty() || !ensureBuffer()) return;
    LLTimer cost;
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    mVisible.clear();
    for (size_t i = 0; i < mLeaves.size() && (S32)mVisible.size() < HARD_MAX; ++i)
    {
        if (camera->sphereInFrustum(mLeaves[i].mPos, mLeaves[i].mLen)) mVisible.push_back((S32)i);
    }
    const S32 visible = (S32)mVisible.size();
    if (visible == 0) return;
    const S32 count = visible * VERTS_PER_LEAF;
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
    const LLVector3 up(0.f, 0.f, 1.f);
    for (S32 vi : mVisible)
    {
        const Leaf& l = mLeaves[(size_t)vi];
        // its face: rocking with the swing while it falls (the tilt is greatest as it passes the middle), a slight lie on
        // the ground once settled
        const F32 tilt = l.mSettled ? l.mTilt : TILT_MAX * cosf(l.mPhase);
        LLVector3 n = up * cosf(tilt) + l.mSwing * sinf(tilt);
        n.normVec();
        // its length along its yaw, laid in that face
        const LLVector3 f(cosf(l.mYaw), sinf(l.mYaw), 0.f);
        LLVector3 len = f - n * (f * n);
        if (len.lengthSquared() < 1e-6f) len = l.mSwing % n;
        len.normVec();
        const LLVector3 across = n % len;   // (len, across, n) right-handed: the outline runs counter-clockwise round n
        const F32 s = llclamp(l.mShrink, 0.f, 1.f);
        LLVector3 rim[SIDES];
        for (S32 k = 0; k < SIDES; ++k)
        {
            const F32* o = WolfLeavesSeason::OUTLINES[l.mShape == WolfLeavesSeason::OAK ? 0 : 1][k];
            rim[k] = l.mPos + len * (o[0] * l.mLen * s) + across * (o[1] * 0.5f * l.mWid * s);
        }
        const LLColor4U edge(l.mColour[0], l.mColour[1], l.mColour[2], 255);
        const LLColor4U vein((U8)(l.mColour[0] * VEIN_SHADE), (U8)(l.mColour[1] * VEIN_SHADE), (U8)(l.mColour[2] * VEIN_SHADE), 255);
        const LLColor4U back((U8)llmin(255.f, l.mColour[0] * BACK_SHADE + 12.f), (U8)llmin(255.f, l.mColour[1] * BACK_SHADE + 12.f),
                             (U8)llmin(255.f, l.mColour[2] * BACK_SHADE + 12.f), 255);
        const LLVector3 nb = -n;
        for (S32 side = 0; side < 2; ++side)
        {
            for (S32 k = 0; k < SIDES; ++k)
            {
                // the front winds round n, the back the other way round -n (GL_CULL_FACE shows one of them)
                const LLVector3& a = rim[k];
                const LLVector3& b = rim[(k + 1) % SIDES];
                const LLVector3* tri[3] = { &l.mPos, side == 0 ? &a : &b, side == 0 ? &b : &a };
                for (S32 j = 0; j < 3; ++j)
                {
                    (*pos++).set(tri[j]->mV[VX], tri[j]->mV[VY], tri[j]->mV[VZ], 0.f);   // w = texture index 0 (wolffarground.cpp)
                    *norm++ = side == 0 ? n : nb;
                    *uv++ = LLVector2(0.5f, 0.5f);
                    *col++ = j == 0 ? vein : (side == 0 ? edge : back);
                }
            }
        }
    }
    mVB->unmapBuffer();
    verts = count;
    mCostMeshUs += cost.getElapsedTimeF64() * 1e6;
}

void WolfLeaves::renderDeferred()
{
    if (mLeaves.empty()) return;
    S32 verts = 0;
    buildMesh(verts);
    if (verts == 0) return;
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    // Source: wolfswimbubbles.cpp renderDeferred (wolfbirds.cpp's, from WolfFarGround::renderDeferred) - the same program,
    // matrices and attributes, so the leaves are lit, shadowed and fogged as the scene round them is.
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
