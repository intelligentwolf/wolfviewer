/**
 * @file wolfflight.cpp
 * @brief WolfViewer Flight Mode: the flight data, the autopilot and the aircraft's controls.
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

#include "wolfflight.h"
#include "wolfaltitudesky.h"

#include <algorithm>
#include <queue>

#include "fsnearbychathub.h"
#include "llagent.h"
#include "llappviewer.h"
#include "llframetimer.h"
#include "llregionhandle.h"
#include "lltoolbarview.h"
#include "lltracker.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"   // <WolfViewer 2026-10-07/> obstacleNeed
#include "lldrawable.h"
#include "llviewerregion.h"
#include "llvoavatarself.h"
#include "llworld.h"
#include "llworldmap.h"
#include "llworldmapmessage.h"
#include "llworldmipmap.h"
#include "llnotificationsutil.h"
#include "llviewertexture.h"
#include "llimage.h"
#include "llui.h"
#include "pipeline.h"
#include "wolfairports.h"   // <WolfViewer 2026-10-07/> autoland
#include "wolfobjectprops.h"
#include "wolfgrid.h"

namespace
{
    const F32 G = 9.81f;
    const F32 KT = 1.943844f;       // m/s -> knots
    const F32 FT = 3.280840f;       // m -> feet
    const F32 FPM = 196.8504f;      // m/s -> feet per minute

    // Pulse-width modulation of a held key: the aircraft's script sees the key go down and
    // up, held for |cmd| of every PWM_PERIOD. 0.25 s is four control events a second, short
    // enough to read as steady pressure, long enough that a script running at sim frame rate
    // (OpenSim sends a control event on every AgentUpdate whose flags changed) sees every edge.
    // <WolfViewer 2026-10-06> 0.25 s / 0.08 felt jerky (Paul: "autopilot is a bit jerky"): every
    // small correction became a visible shove four times a second. Twice the rate blends into
    // the airframe's own inertia, and corrections under 12% are left alone.
    const F32 PWM_PERIOD = 0.12f;
    const F32 PWM_DEAD = 0.12f;     // below this, nothing; above 1 - this, held solid
    // A throttle that steps (each PgUp is one notch, the usual SL plane): one tap this long...
    const F32 TAP_SECONDS = 0.15f;
    // ...at most this often while the speed is off target by more than AT_DEADBAND.
    const F32 TAP_INTERVAL = 0.9f;
    const F32 AT_DEADBAND = 1.5f;   // m/s, ~3 kt
    const S32 AT_FAIL_TAPS = 10;    // taps one way with the speed not answering = A/T FAIL

    const F32 DEST_TIMEOUT = 10.f;  // seconds the map server gets to name a region
    const F32 HOLD_ENTRY_M = 250.f; // a plane closer than this to its destination enters the hold
    const F32 HELI_ARRIVE_M = 4.f;

    F32 wrap180(F32 a)
    {
        while (a > 180.f) a -= 360.f;
        while (a < -180.f) a += 360.f;
        return a;
    }

    F32 wrap360(F32 a)
    {
        while (a >= 360.f) a -= 360.f;
        while (a < 0.f) a += 360.f;
        return a;
    }

    F32 ema(F32 prev, F32 now, F32 dt, F32 tau)
    {
        const F32 a = (tau <= 0.f) ? 1.f : llclamp(dt / (tau + dt), 0.f, 1.f);
        return prev + (now - prev) * a;
    }

    F64 nowSeconds()
    {
        return LLFrameTimer::getTotalSeconds();
    }

    // The flags the viewer sends for each key pair (llagent.cpp moveAt / moveYaw / moveUp /
    // moveLeft), [pair][0] the "positive" key (W / Up arrow, turn LEFT, PgUp, slide LEFT).
    const U32 PAIR_FLAGS[WolfFlight::PAIR_COUNT][2] =
    {
        { AGENT_CONTROL_AT_POS | AGENT_CONTROL_FAST_AT,     AGENT_CONTROL_AT_NEG | AGENT_CONTROL_FAST_AT },
        { AGENT_CONTROL_YAW_POS,                            AGENT_CONTROL_YAW_NEG },
        { AGENT_CONTROL_UP_POS | AGENT_CONTROL_FAST_UP,     AGENT_CONTROL_UP_NEG | AGENT_CONTROL_FAST_UP },
        { AGENT_CONTROL_LEFT_POS | AGENT_CONTROL_FAST_LEFT, AGENT_CONTROL_LEFT_NEG | AGENT_CONTROL_FAST_LEFT },
        { 0, 0 },
    };

    // The world rotation of an object: its own, then every parent's (llviewerobject.cpp
    // getRotationRegion goes up ONE level; a pilot sits on a child prim of the aircraft).
    LLQuaternion worldRotation(const LLViewerObject* objectp)
    {
        LLQuaternion q = objectp->getRotation();
        const LLViewerObject* p = (const LLViewerObject*)objectp->getParent();
        while (p)
        {
            q = q * p->getRotation();
            p = (const LLViewerObject*)p->getParent();
        }
        return q;
    }

    LLViewerObject* vehicleRoot()
    {
        if (!isAgentAvatarValid() || !gAgentAvatarp->isSitting())
        {
            return nullptr;
        }
        LLViewerObject* p = (LLViewerObject*)gAgentAvatarp->getParent();
        while (p && p->getParent())
        {
            p = (LLViewerObject*)p->getParent();
        }
        return p;
    }
}

WolfFlight::WolfFlight()
{
    // World > Flight Mode and World > Sailing Mode share this deck; at most one is on.
    if (LLControlVariable* c = gSavedSettings.getControl("WolfFlightMode"))
    {
        c->getSignal()->connect([](LLControlVariable*, const LLSD& v, const LLSD&)
        {
            WolfFlight::instance().requestMode(false, v.asBoolean());
        });
    }
    if (LLControlVariable* c = gSavedSettings.getControl("WolfSailMode"))
    {
        c->getSignal()->connect([](LLControlVariable*, const LLSD& v, const LLSD&)
        {
            WolfFlight::instance().requestMode(true, v.asBoolean());
        });
    }
}

// Paul, 2026-10-06: "can we add a sailing one as well — World, Sailing Mode". Switching from one
// mode to the other keeps the deck up and drops the autopilot (it was flying the other craft).
void WolfFlight::requestMode(bool sail, bool on)
{
    if (mSyncing)
    {
        return;
    }
    if (on)
    {
        if (mActive && mSail == sail)
        {
            return;
        }
        // Paul 2026-10-06: "make sure sailing and flight only works on wolf territories grid".
        // The same gate as the other Wolf-only features (wolfgrid.h): logged in to Wolf
        // Territories and standing on one of its regions.
        if (!WolfGrid::isOnWolfTerritories())
        {
            syncModeSettings();   // the menu tick goes back off
            refuseOffGrid(sail);
            return;
        }
        if (mActive)
        {
            mAP = mAT = false;
            mOutPitch = mOutBank = mOutThrottle = 0.f;
            mSail = sail;
            mLateral = LAT_NONE;
            mVertical = VERT_NONE;
            clearTrip();   // a plane's trip is not a boat's: the other mode starts clean
            syncModeSettings();
            LL_INFOS("WolfFlight") << (sail ? "Sailing" : "Flight") << " Mode on (switched)" << LL_ENDL;
            return;
        }
        mSail = sail;
        setActive(true);
        return;
    }
    if (mActive && mSail == sail)
    {
        setActive(false);
    }
    else
    {
        syncModeSettings();
    }
}

// Paul 2026-10-06: "the airplane and boat interfaces need hide and show ... so people can hide the
// boat interface or plane interface without quitting it, to take pictures". Another mode's icon
// switches to that mode, deck showing.
void WolfFlight::toggleDeck(bool sail)
{
    if (!mActive || mSail != sail)
    {
        mDeckHidden = false;
        requestMode(sail, true);
        return;
    }
    mDeckHidden = !mDeckHidden;
    LL_INFOS("WolfFlight") << (sail ? "Sailing" : "Flight") << " deck " << (mDeckHidden ? "hidden" : "shown") << LL_ENDL;
}

// The two menu ticks follow the deck (its EXIT, a switch of mode).
void WolfFlight::syncModeSettings()
{
    mSyncing = true;
    const bool flight = mActive && !mSail;
    const bool sail = mActive && mSail;
    if (gSavedSettings.getBOOL("WolfFlightMode") != flight) gSavedSettings.setBOOL("WolfFlightMode", flight);
    if (gSavedSettings.getBOOL("WolfSailMode") != sail) gSavedSettings.setBOOL("WolfSailMode", sail);
    mSyncing = false;
}

// Told, not silently ignored: a menu tick that springs back off needs a reason beside it.
void WolfFlight::refuseOffGrid(bool sail)
{
    LLNotificationsUtil::add("GenericAlert", LLSD().with("MESSAGE",
        std::string(sail ? "Sailing Mode" : "Flight Mode") + " works on Wolf Territories regions only."));
}

WolfFlight::~WolfFlight()
{
}

//-----------------------------------------------------------------------------
// Flight Mode on / off
//-----------------------------------------------------------------------------

void WolfFlight::setActive(bool on)
{
    if (on == mActive)
    {
        return;
    }
    mActive = on;
    mDeckHidden = false;
    if (!on)
    {
        if (mAP)
        {
            mAP = false;
            mOutPitch = mOutBank = mOutThrottle = 0.f;
        }
        mAT = false;
        mLateral = LAT_NONE;
        mVertical = VERT_NONE;
    }
    // Paul: "you didnt clear the route on the plan" — leaving the mode (EXIT, the menu) and
    // coming back kept the old destination, route and alerts. Every entry starts clean.
    clearTrip();
    applyToolbar(on);
    syncModeSettings();   // the menu ticks follow the panel's EXIT
    LL_INFOS("WolfFlight") << (mSail ? "Sailing" : "Flight") << " Mode " << (on ? "on" : "off") << LL_ENDL;
}

// Paul: "when you go into flight mode the main control bar minimises (with the up arrow to bring
// it back)". The bar's own hide arrow (lltoolbarview.cpp applyBottomToolbarHidden, Paul
// 2026-09-10) does exactly that; leaving Flight Mode puts it back how the pilot had it.
void WolfFlight::applyToolbar(bool flight_on)
{
    if (!gToolBarView)
    {
        return;
    }
    if (flight_on)
    {
        mToolbarWasHidden = gSavedSettings.getBOOL("WolfViewerBottomToolbarHidden");
        gSavedSettings.setBOOL("WolfViewerBottomToolbarHidden", true);
        gToolBarView->applyBottomToolbarHidden(true);
    }
    else
    {
        gSavedSettings.setBOOL("WolfViewerBottomToolbarHidden", mToolbarWasHidden);
        gToolBarView->applyBottomToolbarHidden(mToolbarWasHidden);
    }
}

//-----------------------------------------------------------------------------
// Per frame
//-----------------------------------------------------------------------------

void WolfFlight::idle()
{
    if (!mActive)
    {
        return;
    }
    // A hypergrid jump off Wolf Territories ends the mode (requestMode() refuses to start it there).
    if (!WolfGrid::isOnWolfTerritories())
    {
        const bool sail = mSail;
        setActive(false);
        refuseOffGrid(sail);
        return;
    }
    const F32 dt = llclamp((F32)gFrameIntervalSeconds, 0.001f, 0.25f);
    measure(dt);
    resolveDestination();
    if (!mData.mValid)
    {
        if (mAP)
        {
            disconnectAP("AUTOPILOT DISC", false);
        }
        return;
    }
    if (mAP && !mData.mSeated)
    {
        disconnectAP("AUTOPILOT DISC", false);
        postCas("AP: NOT SEATED", CAS_CAUTION);
    }
    advanceRoute();
    if (mSail)
    {
        sailAlarms();
        sailGuidance(dt);
    }
    else
    {
        guidance(dt);
    }
    if (mAP)
    {
        if (mSail) sailDrive(dt); else drive(dt);
        // <WolfViewer 2026-10-07/> no AUTO LEARN from a landing's flare and rollout, nor a take-off's roll
        if (mLand < LAND_FLARE && !takeoffOnRunway())
        {
            learn(dt);
        }
    }
    else
    {
        mOutPitch = mOutBank = mOutThrottle = 0.f;
        manualDrive();
    }

    if (!mSail)
    {
        // <WolfViewer 2026-10-07> Not on an autoland's final: the runway ahead is meant to be hit.
        // <WolfViewer 2026-10-07/> nor on the take-off roll and rotation: the same runway
        if (mAP && (mLand >= LAND_FINAL || takeoffOnRunway()))
        {
            clearCas("PULL UP");
            clearCas("TERRAIN");
        }
        else
        {
            groundProximity();
        }
    }
}

// GROUND PROXIMITY. Where will the aircraft be in the next few seconds, and is there land or
// anything solid (a mesh mountain, a building) in the way?
// <WolfViewer 2026-10-06> Paul: "the alarms didnt show when i hit a mountain". The first version
// only armed seated, above 15 m and faster than 12 m/s, and looked at land only — a flying
// avatar, a slow aircraft and every mesh mountain were invisible to it. Now: armed whenever
// flying or seated and moving, land sampled along the full 3-D path every second for 8 s, and
// one ray along the flight path (gPipeline.lineSegmentIntersectInWorld, the picking ray) for
// everything else, skipping the aircraft's own prims. The alarm holds for 2 s after the
// danger clears so it is seen even after an impact, and PULL UP sounds the alert chime.
void WolfFlight::groundProximity()
{
    const F64 now = nowSeconds();
    const F32 z = (F32)mData.mPosGlobal.mdV[VZ];
    const F32 speed = mData.mAirspeed;
    const bool flying = mData.mSeated || gAgent.getFlying();
    F32 impact = 1e9f;   // seconds to the first thing in the way
    if (flying && speed > 2.f && mData.mAGL > 1.5f)
    {
        for (F32 t = 1.f; t <= 8.f; t += 1.f)
        {
            LLVector3d ahead = mData.mPosGlobal;
            ahead.mdV[VX] += mData.mVel.mV[VX] * t;
            ahead.mdV[VY] += mData.mVel.mV[VY] * t;
            if (!LLWorld::getInstance()->getRegionFromPosGlobal(ahead))
            {
                break;   // no land loaded there
            }
            const F32 ground = LLWorld::getInstance()->resolveLandHeightGlobal(ahead);
            if (z + mData.mVS * t < ground + 3.f)
            {
                impact = t;
                break;
            }
        }
        if (now >= mNextObstacleRay)
        {
            mNextObstacleRay = now + 0.1;
            mObstacleImpact = 1e9f;
            LLVector3 dir = mData.mVel;
            dir.normVec();
            // start clear of the aircraft itself; most are under 20 m long
            const LLVector3 pos = gAgent.getPosAgentFromGlobal(mData.mPosGlobal);
            const F32 skip = mData.mSeated ? 12.f : 1.f;
            const F32 reach = speed * 8.f;
            if (reach > skip)
            {
                LLVector4a start, end, hit;
                start.load3((pos + dir * skip).mV);
                end.load3((pos + dir * reach).mV);
                S32 face = -1;
                LLViewerObject* objectp = gPipeline.lineSegmentIntersectInWorld(start, end, false, false, true, false, &face,
                                                                                 nullptr, nullptr, &hit);
                LLViewerObject* own = vehicleRoot();
                if (objectp && !objectp->isAvatar() && (!own || objectp->getRootEdit() != own))
                {
                    LLVector3 h(hit.getF32ptr());
                    mObstacleImpact = (h - pos).length() / speed;
                }
            }
        }
        impact = llmin(impact, mObstacleImpact);
    }
    else
    {
        mObstacleImpact = 1e9f;
    }
    if (impact <= 4.f)
    {
        mPullUpUntil = now + 2.0;
    }
    else if (impact <= 8.f)
    {
        mTerrainUntil = now + 2.0;
    }
    const bool pull_up = now < mPullUpUntil;
    const bool terrain = !pull_up && now < mTerrainUntil;
    if (pull_up)
    {
        postCas("PULL UP", CAS_WARNING);
        if (now >= mNextAlarmSound)
        {
            mNextAlarmSound = now + 1.0;
            make_ui_sound("UISndAlert");
        }
    }
    else
    {
        clearCas("PULL UP");
    }
    if (terrain) postCas("TERRAIN", CAS_CAUTION); else clearCas("TERRAIN");
}

void WolfFlight::measure(F32 dt)
{
    Data d;
    if (!isAgentAvatarValid() || !gAgent.getRegion())
    {
        mData = d;
        mHaveLast = false;
        return;
    }
    LLViewerRegion* regionp = gAgent.getRegion();
    LLViewerObject* root = vehicleRoot();
    d.mValid = true;
    d.mSeated = root != nullptr;
    d.mRegion = regionp->getName();
    d.mPosGlobal = gAgent.getPositionGlobal();
    d.mPosRegion = regionp->getPosRegionFromGlobal(d.mPosGlobal);
    d.mWaterZ = regionp->getWaterHeight();
    d.mTimeDilation = regionp->getTimeDilation();
    d.mWind = regionp->mWind.getVelocity(d.mPosRegion);
    if (root)
    {
        if (const WolfObjectProps::Props* props = WolfObjectProps::instance().get(root->getID()))
        {
            d.mVehicle = props->mName;
        }
        else
        {
            WolfObjectProps::instance().want(root, false);
        }
    }
    d.mVel = root ? root->getVelocity() : gAgentAvatarp->getVelocity();

    // Attitude from the pilot's seat: a pilot faces the nose, whatever way the builder linked
    // the root prim. The seat's frame is the avatar's world rotation (own, then each parent).
    const LLQuaternion q = worldRotation(gAgentAvatarp);
    LLVector3 fwd = LLVector3::x_axis * q;
    LLVector3 left = LLVector3::y_axis * q;
    const LLVector3 up = LLVector3::z_axis * q;

    d.mGS = sqrtf(d.mVel.mV[VX] * d.mVel.mV[VX] + d.mVel.mV[VY] * d.mVel.mV[VY]);
    // A seat facing sideways or backwards: if the aircraft keeps flying 90 / 180 degrees off
    // the way the seat faces, the seat is what is rotated (fast, level, for three seconds).
    // <WolfViewer 2026-10-06> A boat never reaches a plane's 10 m/s; and on a sailboat the helm
    // seat is often sideways. Learn the seat's angle from 1 m/s in Sailing Mode.
    const F32 vote_speed = mSail ? 1.f : 10.f;
    if (d.mSeated && d.mGS > vote_speed && fabsf(d.mVel.mV[VZ]) < d.mGS * 0.5f)
    {
        const F32 raw = atan2f(fwd.mV[VX], fwd.mV[VY]) * RAD_TO_DEG;
        const F32 trk = atan2f(d.mVel.mV[VX], d.mVel.mV[VY]) * RAD_TO_DEG;
        const F32 diff = wrap180(trk - raw);
        const F32 m = 90.f * floorf(diff / 90.f + 0.5f);
        if (fabsf(diff - m) < 20.f && fabsf(wrap180(m - mHeadingOffset)) > 1.f)
        {
            if (fabsf(wrap180(m - mOffsetVote)) > 1.f)
            {
                mOffsetVote = m;
                mOffsetVoteStart = nowSeconds();
            }
            else if (nowSeconds() - mOffsetVoteStart > 3.0)
            {
                mHeadingOffset = wrap180(m);
                LL_INFOS("WolfFlight") << "seat faces " << -mHeadingOffset << " deg off the nose" << LL_ENDL;
            }
        }
        else
        {
            mOffsetVote = mHeadingOffset;
        }
    }
    if (!d.mSeated)
    {
        mHeadingOffset = 0.f;
    }
    const S32 off = (S32)llround(wrap180(mHeadingOffset) / 90.f);
    if (off == 1)        { LLVector3 f = -left; left = fwd; fwd = f; }    // nose 90 right of the seat
    else if (off == -1)  { LLVector3 f = left; left = -fwd; fwd = f; }   // nose 90 left
    else if (off == 2 || off == -2) { fwd = -fwd; left = -left; }

    d.mPitch = asinf(llclamp(fwd.mV[VZ], -1.f, 1.f)) * RAD_TO_DEG;
    d.mRoll = atan2f(left.mV[VZ], up.mV[VZ]) * RAD_TO_DEG;
    d.mHeading = wrap360(atan2f(fwd.mV[VX], fwd.mV[VY]) * RAD_TO_DEG);
    d.mTrack = (d.mGS > 0.5f) ? wrap360(atan2f(d.mVel.mV[VX], d.mVel.mV[VY]) * RAD_TO_DEG) : d.mHeading;
    d.mAirspeed = d.mVel.length();
    d.mVS = d.mVel.mV[VZ];
    d.mFPA = (d.mAirspeed > 1.f) ? atan2f(d.mVS, d.mGS) * RAD_TO_DEG : 0.f;
    if (d.mAirspeed > 3.f)
    {
        d.mSlip = asinf(llclamp((d.mVel * (-left)) / d.mAirspeed, -1.f, 1.f)) * RAD_TO_DEG;
    }
    const F32 z = (F32)d.mPosGlobal.mdV[VZ];
    d.mGround = LLWorld::getInstance()->resolveLandHeightGlobal(d.mPosGlobal);
    d.mAltMSL = z - d.mWaterZ;
    d.mAGL = z - llmax(d.mGround, d.mWaterZ);   // a radio altimeter reads the water surface
    d.mDepth = d.mWaterZ - d.mGround;           // the sounder: water under the hull

    // Wind, true and apparent. The sim's wind (LLWind, the same wind llWind() gives a boat's
    // script) is the true wind; apparent = true minus the boat's own motion. Angles are where
    // the wind comes FROM, relative to the bow, + = starboard.
    {
        const F32 wx = d.mWind.mV[VX], wy = d.mWind.mV[VY];
        d.mTWS = sqrtf(wx * wx + wy * wy);
        d.mTWD = wrap360(atan2f(-wx, -wy) * RAD_TO_DEG);
        d.mTWA = wrap180(d.mTWD - d.mHeading);
        const F32 ax = wx - d.mVel.mV[VX], ay = wy - d.mVel.mV[VY];
        d.mAWS = sqrtf(ax * ax + ay * ay);
        d.mAWA = (d.mAWS > 0.05f) ? wrap180(atan2f(-ax, -ay) * RAD_TO_DEG - d.mHeading) : 0.f;
        // The sim's wind turns about by up to 150 degrees between samples (Paul's boat at
        // Madrigal, 2026-10-06: 146 to 0 in 3 s), and the sailing AP flipped between beating and
        // sailing straight at the mark, hovering head to wind in irons. It steers by the wind
        // averaged as a vector over about 15 s; the dial still shows the wind as it is.
        if (!mHaveWindAvg)
        {
            mWindAvgX = wx;
            mWindAvgY = wy;
            mHaveWindAvg = true;
        }
        mWindAvgX = ema(mWindAvgX, wx, dt, 15.f);
        mWindAvgY = ema(mWindAvgY, wy, dt, 15.f);
        d.mTWSSteady = sqrtf(mWindAvgX * mWindAvgX + mWindAvgY * mWindAvgY);
        d.mTWDSteady = wrap360(atan2f(-mWindAvgX, -mWindAvgY) * RAD_TO_DEG);
    }

    // Rates and load factor, filtered: object updates arrive in steps the interpolation
    // smooths for position but not for its derivatives.
    if (mHaveLast)
    {
        d.mRollRate = ema(mData.mRollRate, wrap180(d.mRoll - mLastRoll) / dt, dt, 0.25f);
        d.mPitchRate = ema(mData.mPitchRate, (d.mPitch - mLastPitch) / dt, dt, 0.25f);
        const F32 dir_now = (d.mGS > 3.f) ? d.mTrack : d.mHeading;
        d.mTurnRate = ema(mData.mTurnRate, wrap180(dir_now - mLastHeading) / dt, dt, 0.4f);
        const LLVector3 acc = (d.mVel - mLastVel) / dt;
        const F32 n = ((acc + LLVector3(0.f, 0.f, G)) * up) / G;
        d.mG = ema(mData.mG, llclamp(n, -3.f, 6.f), dt, 0.6f);
        d.mSpeedTrend = ema(mData.mSpeedTrend, (d.mAirspeed - mLastAirspeed) / dt * 10.f, dt, 1.0f);
    }
    mLastVel = d.mVel;
    mLastRoll = d.mRoll;
    mLastPitch = d.mPitch;
    mLastHeading = (d.mGS > 3.f) ? d.mTrack : d.mHeading;
    mLastAirspeed = d.mAirspeed;
    mHaveLast = true;
    mData = d;
}

//-----------------------------------------------------------------------------
// Guidance: what track, vertical speed and speed the modes want. Runs with the autopilot off
// too, for the flight director.
//-----------------------------------------------------------------------------

F32 WolfFlight::terrainFloorZ() const
{
    const F32 margin = (craft() == CRAFT_HELI) ? 12.f : 40.f;
    F32 high = llmax(mData.mGround, mData.mWaterZ);
    for (F32 t = 1.5f; t <= 15.f; t += 1.5f)
    {
        LLVector3d p = mData.mPosGlobal;
        p.mdV[VX] += mData.mVel.mV[VX] * t;
        p.mdV[VY] += mData.mVel.mV[VY] * t;
        const F32 g = LLWorld::getInstance()->resolveLandHeightGlobal(p);
        if (g > high)
        {
            high = g;
        }
    }
    return high + margin;
}

// The climb (m/s) needed to clear all the land along a track for the next minute with the floor's
// margin: for each point t seconds ahead, (land + margin - z) / t, the worst of them. Paul: "it
// shouldnt hit mountains and we have a lot at wolf". 15 s ahead (terrainFloorZ) is too late at
// 60 m/s: the climb starts when it is already below the floor.
F32 WolfFlight::terrainNeed(F32 track_deg, F32 speed, F32 z) const
{
    const F32 margin = (craft() == CRAFT_HELI) ? 12.f : 40.f;
    const F32 sx = sinf(track_deg * DEG_TO_RAD) * speed, sy = cosf(track_deg * DEG_TO_RAD) * speed;
    F32 need = -1e9f;
    // From 6 s out: nearer than that, dividing by a second or two asked for 15 m/s near the hills
    // just after take-off and stood the plane up and turned it about (Paul: "still wobbling");
    // the floor (terrainFloorZ) already keeps it off the land right ahead.
    for (F32 t = 6.f; t <= 60.f; t += 1.5f)
    {
        LLVector3d p = mData.mPosGlobal;
        p.mdV[VX] += sx * t;
        p.mdV[VY] += sy * t;
        const F32 g = llmax(LLWorld::getInstance()->resolveLandHeightGlobal(p), mData.mWaterZ);
        need = llmax(need, (g + margin - z) / t);
    }
    return need;
}

// <WolfViewer 2026-10-07> Paul: "autopilot on planes ... it should fly higher if it sees an
// obstruction ahead of it". terrainNeed for OBJECTS: the climb (m/s) that clears, with the same
// margin, every object the next minute's track passes over - towers, buildings, masts, ships. One
// pass over the object list: a prim's drawn box (the drawable's octree extents, as
// WolfNaturalWater::rasterizeBuilt reads them) counts when its footprint comes within the margin
// of the track and its bottom is below the aircraft plus the margin (a skybox far overhead is
// flown under, not climbed to). The craft itself, avatars, attachments and phantom prims (which
// cannot be hit) are ignored. Nearer than 6 s it is divided by 6, as terrainNeed does.
F32 WolfFlight::obstacleNeed(F32 track_deg, F32 speed, F32 z) const
{
    const F32 margin = (craft() == CRAFT_HELI) ? 12.f : 40.f;
    const F32 dx = sinf(track_deg * DEG_TO_RAD), dy = cosf(track_deg * DEG_TO_RAD);
    const F32 reach = speed * 60.f;
    const LLVector3 pos = gAgent.getPosAgentFromGlobal(mData.mPosGlobal);
    LLViewerObject* own = vehicleRoot();
    F32 need = -1e9f;
    const S32 count = gObjectList.getNumObjects();
    for (S32 i = 0; i < count; ++i)
    {
        LLViewerObject* o = gObjectList.getObject(i);
        if (!o || o->isDead() || o->mDrawable.isNull() || o->isAvatar() || o->isAttachment() || o->isHUDAttachment()
            || o->flagPhantom() || (own && (o == own || o->getRootEdit() == own)))
        {
            continue;
        }
        const LLVector4a* ext = o->mDrawable->getSpatialExtents();
        if (!ext || !ext[0].isFinite3() || !ext[1].isFinite3())
        {
            continue;
        }
        const F32 top = ext[1][2], bottom = ext[0][2];
        // <WolfViewer 2026-10-07> Only what reaches down to the aircraft's own height (its underside
        // at most 10 m above it) is in the way; anything higher is flown under. With "within the
        // margin above" a skybox or floating build 30-40 m overhead counted, the climb brought the
        // next one into range, and over open sea near Blindside the plane climbed 600 m+ with cruise
        // set to 500 ft (Paul, 2026-10-07).
        if (top + margin <= z || bottom > z + 10.f)
        {
            continue;   // already clear above it, or above the aircraft: flown under
        }
        const F32 cx = (ext[0][0] + ext[1][0]) * 0.5f - pos.mV[VX], cy = (ext[0][1] + ext[1][1]) * 0.5f - pos.mV[VY];
        const F32 hx = (ext[1][0] - ext[0][0]) * 0.5f, hy = (ext[1][1] - ext[0][1]) * 0.5f;
        const F32 r = sqrtf(hx * hx + hy * hy);   // the footprint, as a circle round the box
        const F32 along = cx * dx + cy * dy;
        const F32 across = fabsf(cx * dy - cy * dx);
        if (along + r < 0.f || along - r > reach || across > r + margin)
        {
            continue;
        }
        const F32 t = llmax(6.f, (along - r) / speed);
        need = llmax(need, (top + margin - z) / t);
    }
    return need;
}

// <WolfViewer 2026-10-08> OUT OF THE CLOUD. Paul: "when i'm in cloud i cant see anything", then "make the autopilot fly
// above or below normally below but if its not safe fly above". When the height the plane is sent to lies in the cloud
// deck (WolfAltitudeSky::cloudDeck: base 350..900 m over the water, 900..2600 m thick), cruise CLOUD_CLEAR_M under its
// base - if that still clears the land ahead (terrainFloorZ, the next 15 s, plus CLOUD_MIN_GAP) and no climb over land
// is due in the next minute - otherwise CLOUD_CLEAR_M over its top. Back under only with CLOUD_BACK_GAP to spare, so it
// does not swap sides on every hill. A plane on its way down to land (VNAV past top of descent, autoland) goes through.
namespace
{
    const F32 CLOUD_CLEAR_M = 100.f;
    const F32 CLOUD_MIN_GAP = 100.f;
    const F32 CLOUD_BACK_GAP = 250.f;
}

F32 WolfFlight::clearOfCloud(F32 target_z)
{
    S32 side = 0;
    WolfAltitudeSky::CloudDeck deck;
    const bool descending = mVertical == VERT_VNAV && mDest.mValid && mDest.mHasZ
                            && target_z == (F32)mDest.mGlobal.mdV[VZ];
    WolfAltitudeSky::cloudDeck(deck);   // the deck's numbers are filled in even while its fog is faded out low down
    if (craft() != CRAFT_HELI && mAP && !descending && mLand < LAND_APPROACH && deck.mTop > deck.mBase
        && target_z > deck.mBase - CLOUD_CLEAR_M && target_z < deck.mTop + CLOUD_CLEAR_M)
    {
        const F32 under_z = deck.mBase - CLOUD_CLEAR_M;
        const F32 gap = (mCloudSide > 0) ? CLOUD_BACK_GAP : CLOUD_MIN_GAP;
        const bool under_safe = under_z >= terrainFloorZ() + gap && mTerrainNeedSmooth <= 0.f;
        side = under_safe ? -1 : 1;
    }
    if (side != mCloudSide)
    {
        if (mCloudSide != 0)
        {
            clearCas(mCloudSide < 0 ? "UNDER CLOUD" : "OVER CLOUD");
        }
        if (side != 0)
        {
            postCas(side < 0 ? "UNDER CLOUD" : "OVER CLOUD", CAS_MEMO);
            LL_INFOS("WolfFlight") << "cloud deck " << deck.mBase << ".." << deck.mTop << ": flying "
                                   << (side < 0 ? "under" : "over") << " it, not at " << target_z << LL_ENDL;
        }
        mCloudSide = side;
    }
    if (side < 0)
    {
        return deck.mBase - CLOUD_CLEAR_M;
    }
    if (side > 0)
    {
        return deck.mTop + CLOUD_CLEAR_M;
    }
    return target_z;
}
// </WolfViewer>

F32 WolfFlight::targetAltitudeZ() const
{
    const F32 sel_z = mData.mWaterZ + mSelAltFt / FT;
    if (mVertical == VERT_VNAV && mDest.mValid)
    {
        const F32 cruise_z = mData.mWaterZ + mCruiseAltFt / FT;
        if (mDest.mHasZ)
        {
            const F32 dest_z = (F32)mDest.mGlobal.mdV[VZ];
            const F32 dist = destDistance();
            const F32 z = (F32)mData.mPosGlobal.mdV[VZ];
            // Top of descent: a 1:12 path for a plane (about 5 degrees), steeper for a helicopter.
            const F32 ratio = (craft() == CRAFT_HELI) ? 2.f : 12.f;
            const F32 start = llmax(z - dest_z, 0.f) * ratio + ((craft() == CRAFT_HELI) ? 30.f : 200.f);
            if (dist < start)
            {
                return dest_z;
            }
        }
        return cruise_z;
    }
    if (mVertical == VERT_HOVER && mDest.mValid && mDest.mHasZ)
    {
        return (F32)mDest.mGlobal.mdV[VZ];
    }
    return sel_z;
}

//-----------------------------------------------------------------------------
// AUTOLAND (Paul, 2026-10-07: "autopilot should find if there is an airport on a region and head
// for that we have an airports database, so check the api and try and find the runway ... and land
// when it gets there"). A plane only; a helicopter is just sent to the airport's point.
//
//   CRUISE    LNAV to the airport's point (the destination is moved there from the region's spot).
//   SEARCH    within LAND_SEARCH_M: look for the runway among the prims round the point
//             (WolfAirports::findRunway) every 2 s, circling over the airport (the ordinary hold)
//             while there is none; LAND_SEARCH_SECS without one = give up, keep circling, say so.
//   APPROACH  to the final approach fix: on the extended centreline, far enough out for a 3 degree
//             glide from where the plane is (600 to 3000 m), at the glide path's height there.
//   FINAL     along the centreline down the 3 degree glide path to the aim point, gear down,
//             approach speed; terrain / object climbs and the ground alarms are off (the runway
//             itself is "an obstacle"). Too far off the line or the path close in = go around.
//   FLARE     LAND_FLARE_M above the surface: sink slowly, power back.
//   ROLLOUT   on the runway: speed 0 along the centreline; stopped = autopilot off, LANDED.
//-----------------------------------------------------------------------------
namespace
{
    const F32 LAND_SEARCH_M = 2500.f;       // start looking for the runway this close
    const F32 LAND_SEARCH_SECS = 180.f;     // two or three circles without one: give up
    const F32 GLIDE_TAN = 0.0524f;          // tan 3 degrees
    const F32 LAND_FLARE_M = 5.f;           // root of the aircraft above the surface
    const F32 LAND_APPROACH_SPEED = 0.7f;   // of the selected speed
    const S32 LAND_MAX_GO_AROUNDS = 2;

    // along-track (+ = past the point, in the direction) and cross-track (+ = right) metres
    void land_track(const LLVector3d& p, const LLVector3d& o, const LLVector3& d, F32& along, F32& across)
    {
        const F32 rx = (F32)(p.mdV[VX] - o.mdV[VX]), ry = (F32)(p.mdV[VY] - o.mdV[VY]);
        along = rx * d.mV[VX] + ry * d.mV[VY];
        across = rx * d.mV[VY] - ry * d.mV[VX];
    }
}

void WolfFlight::landReset()
{
    mLand = LAND_NONE;
    mLandChecked = false;
    mLandAirport.clear();
    mLandGoArounds = 0;
    clearCas("NO RUNWAY FOUND");
    clearCas("AUTOLAND FAILED");
}

// The destination's region in the airports list: move the destination to the airport.
void WolfFlight::landCheckAirport()
{
    WolfAirports& airports = WolfAirports::instance();
    airports.refresh();
    if (mLandChecked || !mDest.mValid || mDest.mPending || !airports.loaded())
    {
        return;
    }
    mLandChecked = true;
    const WolfAirports::Airport* ap = airports.forRegion(mDest.mRegion);
    if (!ap)
    {
        return;
    }
    mLandAirport = ap->mName;
    mLandAirportExact = ap->mExact;
    mLandAirportGlobal = ap->mGlobal;
    mDest.mGlobal.mdV[VX] = ap->mGlobal.mdV[VX];
    mDest.mGlobal.mdV[VY] = ap->mGlobal.mdV[VY];
    rebuildRoute();
    mLand = craft() == CRAFT_PLANE ? LAND_CRUISE : LAND_NONE;
    std::string shown = utf8str_truncate(ap->mName, 24);
    LLStringUtil::toUpper(shown);
    postCas("AIRPORT " + shown, CAS_MEMO);
    LL_INFOS("WolfFlight") << "destination " << mDest.mRegion << " has airport " << ap->mName << " at " << ap->mGlobal
                           << (ap->mExact ? "" : " (region centre)") << LL_ENDL;
}

// The runway found: land on it into the wind when there is some, otherwise from the end nearer
// the plane's way in; the final approach fix far enough out for a 3 degree glide from here.
void WolfFlight::landSetRunway(const LLVector3d& a, const LLVector3d& b, F32 width, F32 top, bool keep_direction)
{
    LLVector3 ab((F32)(b.mdV[VX] - a.mdV[VX]), (F32)(b.mdV[VY] - a.mdV[VY]), 0.f);
    const F32 len = ab.normVec();
    if (len < 1.f)
    {
        return;
    }
    bool from_a;
    if (keep_direction)
    {
        from_a = ab * mLandDir >= 0.f;
    }
    else if (LLVector3(mData.mWind.mV[VX], mData.mWind.mV[VY], 0.f).length() > 3.f)
    {
        from_a = mData.mWind * ab < 0.f;   // the wind blows from B toward A: land A -> B, into it
    }
    else
    {
        // the threshold whose approach starts nearer the plane
        const F64 da = (a.mdV[VX] - mData.mPosGlobal.mdV[VX]) * (a.mdV[VX] - mData.mPosGlobal.mdV[VX])
                     + (a.mdV[VY] - mData.mPosGlobal.mdV[VY]) * (a.mdV[VY] - mData.mPosGlobal.mdV[VY]);
        const F64 db = (b.mdV[VX] - mData.mPosGlobal.mdV[VX]) * (b.mdV[VX] - mData.mPosGlobal.mdV[VX])
                     + (b.mdV[VY] - mData.mPosGlobal.mdV[VY]) * (b.mdV[VY] - mData.mPosGlobal.mdV[VY]);
        from_a = da <= db;
    }
    mLandThr = from_a ? a : b;
    mLandDir = from_a ? ab : -ab;
    mLandLen = len;
    mLandWidth = width;
    mLandTop = top;
    mLandThr.mdV[VZ] = top;
    if (!keep_direction)
    {
        const F32 z = (F32)mData.mPosGlobal.mdV[VZ];
        const F32 out = llclamp((z - top) / GLIDE_TAN, 600.f, 3000.f);
        mLandFaf = mLandThr - LLVector3d(mLandDir * out);
        mLandFaf.mdV[VZ] = top + out * GLIDE_TAN;
        const S32 rwy = llmax(1, (S32)llround(wrap360((F32)(atan2(mLandDir.mV[VX], mLandDir.mV[VY]) * RAD_TO_DEG)) / 10.f));
        postCas(llformat("RWY %02d %dM", rwy > 36 ? rwy - 36 : rwy, (S32)len), CAS_MEMO);
        LL_INFOS("WolfFlight") << "runway at " << mLandAirport << ": threshold " << mLandThr << " heading "
                               << atan2(mLandDir.mV[VX], mLandDir.mV[VY]) * RAD_TO_DEG << " length " << len << " width "
                               << width << " surface " << top << ", final approach fix " << out << " m out" << LL_ENDL;
    }
}

void WolfFlight::landGiveUp(const std::string& cas, const std::string& message)
{
    mLand = LAND_NONE;
    postCas(cas, CAS_WARNING);
    make_ui_sound("UISndAlert");
    LLNotificationsUtil::add("GenericAlert", LLSD().with("MESSAGE", message));
    // circle over the airport, as at any destination without one
    if (mDest.mValid)
    {
        mLateral = LAT_HOLD;
    }
    LL_INFOS("WolfFlight") << "autoland: " << cas << LL_ENDL;
}

void WolfFlight::landGuidance(F32 dt)
{
    landCheckAirport();
    if (mLand == LAND_NONE || craft() != CRAFT_PLANE)
    {
        return;
    }
    if (!mAP || (mLateral != LAT_LNAV && mLateral != LAT_HOLD))
    {
        if (mLand >= LAND_APPROACH)
        {
            mLand = LAND_SEARCH;   // the pilot took over: start again from the runway if re-engaged
            mLandSearchStart = nowSeconds();
        }
        return;
    }
    const F64 now = nowSeconds();
    const F32 z = (F32)mData.mPosGlobal.mdV[VZ];
    const F32 cruise = mSelSpeedKt / KT;
    const F32 approach = llmax(1.f, cruise * LAND_APPROACH_SPEED);
    switch (mLand)
    {
    case LAND_CRUISE:
        if (destDistance() < LAND_SEARCH_M)
        {
            mLand = LAND_SEARCH;
            mLandSearchStart = now;
            mNextRunwayScan = 0.0;
        }
        break;
    case LAND_SEARCH:
        if (now >= mNextRunwayScan)
        {
            mNextRunwayScan = now + 2.0;
            WolfAirports::Airport ap;
            ap.mName = mLandAirport;
            ap.mGlobal = mLandAirportGlobal;
            ap.mHasZ = mLandAirportGlobal.mdV[VZ] != 0.0;
            ap.mExact = mLandAirportExact;
            const WolfAirports::Runway r = WolfAirports::findRunway(ap);
            if (r.mValid)
            {
                landSetRunway(r.mA, r.mB, r.mWidth, r.mTopZ, false);
                mLand = LAND_APPROACH;
                mLateral = LAT_LNAV;
                mAT = true;   // the approach is flown on speed
                clearCas("NO RUNWAY FOUND");
                break;
            }
        }
        if (now - mLandSearchStart > LAND_SEARCH_SECS)
        {
            landGiveUp("NO RUNWAY FOUND", "Autopilot: no runway was found at " + mLandAirport
                       + ". It is circling over the airport. Land by hand, or pick another destination.");
        }
        break;
    case LAND_APPROACH:
    {
        // a longer runway as more of it loads: same direction, same fix
        if (now >= mNextRunwayScan)
        {
            mNextRunwayScan = now + 3.0;
            WolfAirports::Airport ap;
            ap.mName = mLandAirport;
            ap.mGlobal = mLandAirportGlobal;
            ap.mHasZ = mLandAirportGlobal.mdV[VZ] != 0.0;
            ap.mExact = mLandAirportExact;
            const WolfAirports::Runway r = WolfAirports::findRunway(ap);
            if (r.mValid && r.length() > mLandLen + 1.f)
            {
                landSetRunway(r.mA, r.mB, r.mWidth, r.mTopZ, true);
            }
        }
        const F64 dx = mLandFaf.mdV[VX] - mData.mPosGlobal.mdV[VX], dy = mLandFaf.mdV[VY] - mData.mPosGlobal.mdV[VY];
        const F32 to_faf = (F32)sqrt(dx * dx + dy * dy);
        mLandWantTrack = wrap360((F32)(atan2(dx, dy) * RAD_TO_DEG));
        const F32 max_vs = llclamp(mData.mAirspeed * 0.176f, 0.7f, 8.f);
        mLandWantVS = llclamp(((F32)mLandFaf.mdV[VZ] - z) * 0.15f, -max_vs, max_vs);
        mLandWantSpeed = llmax(approach, llmin(cruise, approach + to_faf / 60.f));
        F32 along = 0.f, across = 0.f;
        land_track(mData.mPosGlobal, mLandFaf, mLandDir, along, across);
        // At the fix, or already past it on the centreline's side of it AND where final would want
        // the plane: on the glide path (within 30 m) and at least 300 m before the threshold.
        // <WolfViewer 2026-10-07> Paul's first autoland at Breathe Resort: the runway was found with
        // the plane 511 m up beside the airport, "past the fix" sent it straight to final, final
        // saw it far above the path close in and went around, and the go-around's new fix did the
        // same on the next frame - two go-arounds and AUTOLAND FAILED within a second.
        F32 t_along = 0.f, t_across = 0.f;
        land_track(mData.mPosGlobal, mLandThr, mLandDir, t_along, t_across);
        const F32 aim = llmin(mLandLen * 0.25f, 150.f);
        const F32 path = mLandTop + llmax(0.f, aim - t_along) * GLIDE_TAN;
        const bool on_final_already = along > 0.f && fabsf(across) < 300.f && t_along < -300.f && fabsf(z - path) < 30.f;
        if (to_faf < llmax(150.f, mData.mGS * 6.f) || on_final_already)
        {
            mLand = LAND_FINAL;
            if (!mGearDown)
            {
                sendGearCommand();
            }
            postCas("FINAL", CAS_MEMO);
        }
        break;
    }
    case LAND_FINAL:
    case LAND_FLARE:
    case LAND_ROLLOUT:
    {
        F32 along = 0.f, across = 0.f;
        land_track(mData.mPosGlobal, mLandThr, mLandDir, along, across);
        const F32 rwy_hdg = wrap360((F32)(atan2(mLandDir.mV[VX], mLandDir.mV[VY]) * RAD_TO_DEG));
        // onto the centreline: up to 30 degrees of intercept, half a degree per metre off it
        mLandWantTrack = wrap360(rwy_hdg + llclamp(-across * 0.5f, -30.f, 30.f));
        const F32 aim = llmin(mLandLen * 0.25f, 150.f);
        const F32 height = z - mLandTop;
        if (mLand == LAND_FINAL)
        {
            const F32 path = mLandTop + llmax(0.f, aim - along) * GLIDE_TAN;
            mLandWantVS = llclamp((path - z) * 0.25f - mData.mGS * GLIDE_TAN, -4.f, 2.f);
            mLandWantSpeed = approach;
            // close in and not stable: go around
            const bool close_in = along > -400.f;
            const bool off_line = fabsf(across) > mLandWidth * 0.5f + 25.f;
            const bool off_path = z - path > 40.f || path - z > 25.f;
            const bool long_land = along > aim + mLandLen * 0.5f && height > LAND_FLARE_M * 3.f;
            if ((close_in && (off_line || off_path)) || long_land)
            {
                if (++mLandGoArounds > LAND_MAX_GO_AROUNDS)
                {
                    landGiveUp("AUTOLAND FAILED", "Autopilot: could not line up on the runway at " + mLandAirport
                               + " after " + std::to_string(LAND_MAX_GO_AROUNDS) + " go-arounds. It is circling over the"
                               " airport. Land by hand, or pick another destination.");
                    break;
                }
                postCas("GO AROUND", CAS_CAUTION);
                landSetRunway(mLandThr, mLandThr + LLVector3d(mLandDir * mLandLen), mLandWidth, mLandTop, false);
                mLand = LAND_APPROACH;
                mLandFaf.mdV[VZ] = llmax((F32)mLandFaf.mdV[VZ], mLandTop + 100.f);   // climb away first
                break;
            }
            if (height < LAND_FLARE_M && along > -50.f)
            {
                mLand = LAND_FLARE;
                mLandTouchSince = 0.0;
                postCas("FLARE", CAS_MEMO);
            }
        }
        else if (mLand == LAND_FLARE)
        {
            mLandWantVS = -0.6f;
            mLandWantSpeed = approach * 0.8f;
            // down: no longer sinking, within the flare height, over the runway, for a second
            const bool down = mVSf > -0.3f && height < LAND_FLARE_M + 1.f && along > -20.f && along < mLandLen;
            mLandTouchSince = down ? (mLandTouchSince > 0.0 ? mLandTouchSince : now) : 0.0;
            if (mLandTouchSince > 0.0 && now - mLandTouchSince > 1.0)
            {
                mLand = LAND_ROLLOUT;
                mLandStopSince = 0.0;
                postCas("TOUCHDOWN", CAS_MEMO);
            }
        }
        else
        {
            mLandWantVS = 0.f;   // level: no pitch keys on the ground
            mLandWantSpeed = 0.f;
            mLandStopSince = mData.mGS < 1.f ? (mLandStopSince > 0.0 ? mLandStopSince : now) : 0.0;
            if (mLandStopSince > 0.0 && now - mLandStopSince > 2.0)
            {
                mAP = false;
                mAT = false;
                mOutPitch = mOutBank = mOutThrottle = 0.f;
                mLand = LAND_NONE;
                mArrived = true;
                clearCas("FINAL");
                clearCas("FLARE");
                clearCas("TOUCHDOWN");
                postCas("LANDED", CAS_MEMO);
                LL_INFOS("WolfFlight") << "autoland: landed at " << mLandAirport << LL_ENDL;
            }
        }
        break;
    }
    default:
        break;
    }
    (void)dt;
}

//-----------------------------------------------------------------------------
// AUTO TAKE-OFF (Paul, 2026-10-07: "auto pilot, should be able to take off, fly and land perfectly").
// The autopilot engaged with a plane on the ground (under 10 m) takes off along the heading the
// plane points - line it up on the runway first - and then flies whatever was set: the route (LNAV
// and VNAV, autoland at an airport) or the selected heading and altitude.
//
//   ROLL     A/T at the selected speed (a stepping throttle taps up to full), the heading held with
//            the turn keys, the pitch keys left alone. ROTATE at TO_ROTATE_FRACTION of the selected
//            speed, or once the speed has stopped building for TO_PLATEAU_SECS: many SL plane
//            scripts move in fixed steps (Paul's Caravan read exactly 4 and 8 m/s) and never reach
//            a fraction of a selected speed. Not moving after TO_NO_ROLL_SECS = rejected.
//   ROTATE   nose up to TO_ROTATE_PITCH until it climbs (5 m up, or rising at 1 m/s);
//            TO_NO_LIFT_SECS without = rejected.
//   CLIMB    the heading held, climbing on a 10 degree path; gear up at TO_GEAR_UP_M; at
//            TO_DONE_M the ordinary modes take over.
//   REJECT   the power back to nothing (A/T to 0: a stepping throttle stays where it was left, so
//            letting go is not enough), the heading held; stopped for 2 s = the autopilot off.
// The ground alarms, the land / object climbs and AUTO LEARN are off on the runway (as on an
// autoland's final): the runway ahead is meant to be there, and a nose that cannot rise until the
// speed is up is not a reversed elevator.
//-----------------------------------------------------------------------------
namespace
{
    const F32 TO_ROTATE_FRACTION = 0.75f;
    const F32 TO_SPEED_GAIN = 0.5f;         // m/s faster counts as still accelerating
    const F32 TO_PLATEAU_SECS = 6.f;        // longer than a throttle notch takes to build speed
    const F32 TO_MIN_ROTATE_SPEED = 3.f;    // m/s: no plateau rotation before this
    const F32 TO_NO_ROLL_SECS = 20.f;
    const F32 TO_ROTATE_PITCH = 10.f;       // degrees
    const F32 TO_NO_LIFT_SECS = 15.f;
    const F32 TO_LIFT_M = 5.f;
    const F32 TO_GEAR_UP_M = 30.f;
    const F32 TO_DONE_M = 120.f;            // about 400 ft
    const F32 TO_MIN_CRUISE_FT = 500.f;     // above the runway: a lower cruise is raised to this
}

namespace
{
    const F32 SPEED_LOW_MIN_AGL = 150.f;   // <WolfViewer 2026-10-08/> metres: speed protection dives only above this
}

void WolfFlight::takeoffStart()
{
    const F64 now = nowSeconds();
    mTakeoff = TO_ROLL;
    // <WolfViewer 2026-10-08/> Paul: "assume the command to start a plane is start" - the engine command first, so a plane
    // sat on with its engine off rolls instead of being rejected after TO_NO_ROLL_SECS (nothing said when not set)
    if (!gSavedSettings.getString("WolfFlightEngineCommand").empty())
    {
        sendEngineCommand();
    }
    mToHeading = mData.mHeading;
    mToGroundZ = (F32)mData.mPosGlobal.mdV[VZ];
    mToPhaseStart = now;
    mToBestSpeed = mData.mAirspeed;
    mToBestSpeedAt = now;
    mAT = true;
    mPitchTrim = 0.f;
    // Where to after the climb: the route, or straight ahead to a safe height.
    const F32 field_ft = mData.mAltMSL * FT;
    if (mCruiseAltFt < field_ft + TO_MIN_CRUISE_FT)
    {
        mCruiseAltFt = 100.f * ceilf((field_ft + TO_MIN_CRUISE_FT) / 100.f);
    }
    if (mDest.mValid)
    {
        mLateral = LAT_LNAV;
        mVertical = VERT_VNAV;
    }
    else
    {
        mLateral = LAT_HDG;
        mSelHeading = (F32)llround(mToHeading);
        mVertical = VERT_ALT;
        mSelAltFt = llmax(mSelAltFt, mCruiseAltFt);
    }
    if (!mGearDown)
    {
        sendGearCommand();   // on the ground the gear is down; say so to the plane's script
    }
    clearCas("TAKEOFF REJECTED");
    postCas("TAKEOFF", CAS_MEMO);
    LL_INFOS("WolfFlight") << "take-off: heading " << mToHeading << ", runway z " << mToGroundZ << ", then "
                           << (mDest.mValid ? "the route" : "the selected heading") << " at " << mCruiseAltFt << " ft" << LL_ENDL;
}

void WolfFlight::takeoffClearCas()
{
    clearCas("TAKEOFF");
    clearCas("ROTATE");
    clearCas("POSITIVE CLIMB");
}

void WolfFlight::takeoffReject(const std::string& why)
{
    LL_INFOS("WolfFlight") << "take-off rejected: " << why << LL_ENDL;
    mTakeoff = TO_REJECT;
    mToPhaseStart = nowSeconds();
    mToStoppedSince = 0.0;
    mAT = true;   // to take the power off
    takeoffClearCas();
    postCas("TAKEOFF REJECTED", CAS_WARNING);
    make_ui_sound("UISndAlert");
    LLNotificationsUtil::add("GenericAlert", LLSD().with("MESSAGE", "Autopilot: take-off rejected - " + why
                             + ". It is taking the power off and stopping on the runway, then the autopilot lets go."));
}

// The phase, and the climb it wants (mToWantVS); guidance() applies them over the modes' own
// targets, as it does an autoland's.
void WolfFlight::takeoffGuidance()
{
    if (mTakeoff == TO_NONE)
    {
        return;
    }
    if (!mAP || craft() != CRAFT_PLANE)
    {
        mTakeoff = TO_NONE;
        takeoffClearCas();
        return;
    }
    const F64 now = nowSeconds();
    const F32 height = (F32)mData.mPosGlobal.mdV[VZ] - mToGroundZ;
    const F32 sel = mSelSpeedKt / KT;
    const F32 climb = llclamp(mData.mAirspeed * 0.176f, 0.7f, 8.f);   // a 10 degree path, as guidance()
    switch (mTakeoff)
    {
    case TO_ROLL:
    {
        mToWantVS = 0.f;
        if (mData.mAirspeed > mToBestSpeed + TO_SPEED_GAIN)
        {
            mToBestSpeed = mData.mAirspeed;
            mToBestSpeedAt = now;
        }
        const bool fast = mData.mAirspeed >= sel * TO_ROTATE_FRACTION;
        const bool plateau = mData.mAirspeed >= TO_MIN_ROTATE_SPEED && now - mToBestSpeedAt > TO_PLATEAU_SECS;
        if (fast || plateau || height > TO_LIFT_M)
        {
            mTakeoff = TO_ROTATE;
            mToPhaseStart = now;
            postCas("ROTATE", CAS_MEMO);
            LL_INFOS("WolfFlight") << "take-off: rotate at " << mData.mAirspeed << " m/s ("
                                   << (fast ? "speed" : (plateau ? "speed steady" : "already climbing")) << ")" << LL_ENDL;
        }
        else if (now - mToPhaseStart > TO_NO_ROLL_SECS && mData.mAirspeed < TO_MIN_ROTATE_SPEED)
        {
            takeoffReject("it did not start rolling (check the engine and the throttle keys)");
        }
        break;
    }
    case TO_ROTATE:
        mToWantVS = climb;
        if (height > TO_LIFT_M || mVSf > 1.f)
        {
            mTakeoff = TO_CLIMB;
            mToPhaseStart = now;
            mPitchTrim = llclamp(mData.mPitch, -10.f, 12.f);   // the pitch it lifted off at
            mLearnHoldUntil = now + 10.0;                      // AUTO LEARN only once it is flying
            clearCas("ROTATE");
            postCas("POSITIVE CLIMB", CAS_MEMO);
        }
        else if (now - mToPhaseStart > TO_NO_LIFT_SECS)
        {
            takeoffReject("it did not lift off (check the pitch keys on the CDU CONTROLS page)");
        }
        break;
    case TO_CLIMB:
        mToWantVS = climb;
        if (mGearDown && height > TO_GEAR_UP_M)
        {
            sendGearCommand();
        }
        if (height > TO_DONE_M)
        {
            mTakeoff = TO_NONE;
            takeoffClearCas();
            LL_INFOS("WolfFlight") << "take-off: done at " << (S32)height << " m, handing over to lateral "
                                   << (S32)mLateral << " vertical " << (S32)mVertical << LL_ENDL;
        }
        break;
    case TO_REJECT:
        mToWantVS = 0.f;
        mToStoppedSince = mData.mGS < 1.f ? (mToStoppedSince > 0.0 ? mToStoppedSince : now) : 0.0;
        if (mToStoppedSince > 0.0 && now - mToStoppedSince > 2.0)
        {
            mTakeoff = TO_NONE;
            mAP = false;
            mAT = false;
            mOutPitch = mOutBank = mOutThrottle = 0.f;
            LL_INFOS("WolfFlight") << "take-off rejected: stopped, autopilot off" << LL_ENDL;
        }
        break;
    default:
        break;
    }
}

void WolfFlight::guidance(F32 dt)
{
    // the vertical speed arrives in steps with each object update: filtered for the guidance
    mVSf = ema(mVSf, mData.mVS, dt, 0.8f);
    landGuidance(dt);   // <WolfViewer 2026-10-07/> autoland: its targets override below
    takeoffGuidance();  // <WolfViewer 2026-10-07/> auto take-off: the same
    const bool landing = mAP && mLand >= LAND_APPROACH;
    const bool takeoff = mAP && mTakeoff != TO_NONE;
    const bool on_runway = takeoff && takeoffOnRunway();   // rolling, rotating or stopping
    const bool heli = craft() == CRAFT_HELI;
    const F32 z = (F32)mData.mPosGlobal.mdV[VZ];
    mFDValid = mLateral != LAT_NONE || mVertical != VERT_NONE;

    // ---- lateral ----
    bool use_track = mData.mGS > 5.f;
    F32 want = use_track ? mData.mTrack : mData.mHeading;
    mWantSpeed = mSelSpeedKt / KT;
    switch (mLateral)
    {
    case LAT_HDG:
        want = mSelHeading;
        use_track = false;
        break;
    case LAT_LNAV:
        if (mDest.mValid)
        {
            const F32 dist = destDistance();
            want = destBearing();
            if (heli)
            {
                // Slow down on the way in: about a sixth of the distance, m/s.
                mWantSpeed = llmin(mWantSpeed, llmax(0.8f, dist / 6.f));
                if (dist < HELI_ARRIVE_M)
                {
                    mWantSpeed = 0.f;
                    want = mData.mHeading;
                    use_track = false;
                    if (!mArrived)
                    {
                        mArrived = true;
                        postCas("DEST REACHED", CAS_MEMO);
                        if (mVertical == VERT_VNAV)
                        {
                            mVertical = VERT_HOVER;
                        }
                    }
                }
            }
            else if (dist < HOLD_ENTRY_M && mAP && mLand < LAND_APPROACH)   // <WolfViewer 2026-10-07/> not on an approach
            {
                mLateral = LAT_HOLD;
                postCas("HOLDING AT DEST", CAS_MEMO);
            }
        }
        break;
    case LAT_HOLD:
        if (mDest.mValid)
        {
            // A right-hand orbit over the destination: centre on the right wing, the radius a
            // 25 degree bank turn takes at this speed (r = v^2 / (g tan 25)), steering in when wide.
            const F32 v = llmax(mData.mGS, 15.f);
            const F32 r = llclamp(v * v / (G * 0.4663f) * 1.2f, 120.f, 1500.f);
            const F32 dist = destDistance();
            want = wrap360(destBearing() - 90.f + llclamp((dist - r) / r * 90.f, -60.f, 60.f));
        }
        break;
    default:
        break;
    }
    if (landing)   // <WolfViewer 2026-10-07/> autoland steers and sets the speed
    {
        want = mLandWantTrack;
        use_track = mData.mGS > 5.f;
        mWantSpeed = mLandWantSpeed;
    }
    if (takeoff)   // <WolfViewer 2026-10-07/> straight down the runway and out along it
    {
        want = mToHeading;
        use_track = false;
        if (mTakeoff == TO_REJECT)
        {
            mWantSpeed = 0.f;
        }
    }
    mWantTrack = want;
    const F32 current = use_track ? mData.mTrack : mData.mHeading;
    const F32 err = wrap180(want - current);
    // Smoothed: the targets ease in instead of jumping with every object update (Paul: "jerky").
    mFDRoll = ema(mFDRoll, (mLateral == LAT_NONE) ? 0.f : llclamp(err * 1.2f - mData.mTurnRate * 1.5f, -25.f, 25.f), dt, 0.6f);

    // ---- vertical ----
    // A plane climbs at most about 10 degrees up its path: at 4 m/s (Paul's slow plane, 2026-10-06)
    // that is 0.7 m/s, not 8, which it pitched to 25 degrees chasing ("almost went straight up").
    const F32 max_vs = heli ? 4.f : llclamp(mData.mAirspeed * 0.176f, 0.7f, 8.f);   // tan 10 = 0.176
    switch (mVertical)
    {
    case VERT_VS:
        mWantVS = mSelVsFpm / FPM;
        {
            // ALT capture: on reaching the selected altitude, hold it.
            const F32 sel_z = mData.mWaterZ + mSelAltFt / FT;
            if (mAP && ((mWantVS > 0.f && z >= sel_z - 5.f) || (mWantVS < 0.f && z <= sel_z + 5.f)))
            {
                mVertical = VERT_ALT;
            }
        }
        break;
    case VERT_ALT:
    case VERT_VNAV:
    case VERT_HOVER:
        mWantVS = llclamp(((mVertical == VERT_HOVER ? targetAltitudeZ() : clearOfCloud(targetAltitudeZ())) - z) * 0.15f,
                          -max_vs, max_vs);
        // <WolfViewer 2026-10-08> Paul: "the plane is still jerking". Past top of descent the target is the destination's
        // height, and 0.15 x (dest - z) asked for the full -8 m/s at once, against the terrain look-ahead's climb: 16:43Z
        // the ask flipped -8 / +8 every few seconds, pitch 3 / 16, until SPEED LOW. A plane descends ALONG the glide line
        // instead: the height still to lose over the time left to the destination - gentle, and it never dives early.
        if (mVertical == VERT_VNAV && craft() != CRAFT_HELI && mDest.mValid && mDest.mHasZ
            && targetAltitudeZ() == (F32)mDest.mGlobal.mdV[VZ])
        {
            const F32 to_lose = z - (F32)mDest.mGlobal.mdV[VZ];
            const F32 secs_left = destDistance() / llmax(mData.mGS, 15.f);
            if (to_lose > 0.f && secs_left > 1.f)
            {
                mWantVS = llclamp(-to_lose / secs_left, -max_vs, 0.f);
            }
        }
        break;
    default:
        mWantVS = 0.f;
        break;
    }
    if (landing)   // <WolfViewer 2026-10-07/> autoland's glide
    {
        mWantVS = mLandWantVS;
    }
    if (takeoff)   // <WolfViewer 2026-10-07/> level on the roll, then the climb out
    {
        mWantVS = on_runway ? mToWantVS : llmax(mWantVS, mToWantVS);
    }
    // Never below the land ahead, whatever the mode asks (a climb wins over any descent).
    // <WolfViewer 2026-10-07> Except on final: there the runway is the land ahead. Nor on the
    // take-off roll: the floor is 40 m above the runway, and a climb asked for at 0 m/s pulls the
    // nose up before the plane can fly.
    if ((mVertical != VERT_NONE || mAP) && !(landing && mLand >= LAND_FINAL) && !on_runway)
    {
        const F32 floor_z = terrainFloorZ();
        if (z < floor_z)
        {
            mWantVS = llmax(mWantVS, llclamp((floor_z - z) * 0.3f, 1.f, max_vs));
        }
        // A minute ahead: start the climb early enough, at the rate the land calls for.
        const F64 now = nowSeconds();
        const F32 speed = llmax(mData.mGS, heli ? 3.f : 15.f);
        const F32 track = mData.mGS > 5.f ? mData.mTrack : mData.mHeading;
        if (now >= mNextTerrainScan)
        {
            mNextTerrainScan = now + 0.25;
            mTerrainNeed = llmax(terrainNeed(track, speed, z), obstacleNeed(track, speed, z));   // <WolfViewer 2026-10-07/> objects too
        }
        // Paul: "if it sees terrain it should fly UPWARDS not turn constantly". Land ahead is
        // only ever climbed over: up to a 20 degree climb path (at least 2 m/s), steeper than
        // the 10 degrees of an ordinary altitude change.
        mTerrainNeedSmooth = ema(mTerrainNeedSmooth, mTerrainNeed, dt, 2.f);
        if (mTerrainNeedSmooth > 0.f)
        {
            const F32 terrain_vs = heli ? 4.f : llclamp(mData.mAirspeed * 0.364f, 2.f, 8.f);   // tan 20 = 0.364
            mWantVS = llmax(mWantVS, llmin(mTerrainNeedSmooth * 1.2f, terrain_vs));
        }
    }
    // <WolfViewer 2026-10-08> SPEED PROTECTION. Paul's flight 08:20Z: crossing into Diamonds Customs 2 the plane
    // lost its thrust, the autothrottle tapped up ten times for nothing (A/T FAIL), and ALT hold went on
    // asking for height: the pitch trim wound to its +12 stop and the nose came up until the plane stalled
    // (62 -> 3 m/s, then nose down at -60). Below 60% of the selected speed a plane trades height for speed
    // instead: SPEED LOW, the nose-up trim taken off at once, and a descent that grows with the shortfall,
    // until it is back above 75%. Not on the take-off (its own speeds) or once on final (autoland's).
    // <WolfViewer 2026-10-08> Measured against what the plane really flies, not only the selected speed: Paul set SPD far
    // past this plane's best (279 m/s, it tops out near 180), take-off handed over at 131 m/s - under 60% of 279 - and
    // SPEED LOW dived it at 124 m into the ground. The reference is the lower of the selected speed and the fastest flown
    // lately, which falls back 0.5 m/s a second: a plane slowing towards a stall (62 -> 3 m/s in seconds) still trips it.
    mSpeedRef = llmax(mData.mAirspeed, mSpeedRef - 0.5f * dt);
    if (mAP && !mSail && !heli && !takeoff && !(landing && mLand >= LAND_FINAL) && mWantSpeed > 1.f)
    {
        const F32 ref = llmin(mWantSpeed, mSpeedRef);
        const F32 low = ref * 0.6f, recovered = ref * 0.75f;
        if (!mSpeedLow && mData.mAirspeed < low)
        {
            mSpeedLow = true;
            postCas("SPEED LOW", CAS_WARNING);
            make_ui_sound("UISndAlert");
            LL_INFOS("WolfFlight") << "speed protection: ias " << mData.mAirspeed << " < " << low << LL_ENDL;
        }
        else if (mSpeedLow && mData.mAirspeed > recovered)
        {
            mSpeedLow = false;
            clearCas("SPEED LOW");
            LL_INFOS("WolfFlight") << "speed protection off: ias " << mData.mAirspeed << LL_ENDL;
        }
        if (mSpeedLow)
        {
            // the nose-up trim always goes; the dive only with room under it - never against the land climb above, and
            // never below SPEED_LOW_MIN_AGL (it hit the ground from 124 m)
            mPitchTrim = llmin(mPitchTrim, 0.f);
            if (mTerrainNeedSmooth <= 0.f && mData.mAGL > SPEED_LOW_MIN_AGL)
            {
                mWantVS = llmin(mWantVS, -llclamp((recovered - mData.mAirspeed) * 0.2f, 1.f, 8.f));
            }
            else
            {
                mWantVS = llmax(mWantVS, 0.f);   // level, the trim off: speed comes back without losing height
            }
        }
    }
    else if (mSpeedLow)
    {
        mSpeedLow = false;
        clearCas("SPEED LOW");
    }
    // </WolfViewer>
    // <WolfViewer 2026-10-08> Paul, SPD taken "right up" to 170 m/s: "its going crazy" - pitch 2 to 16 degrees every
    // 3 s, vs -20 to +17. A degree of pitch moves the vertical speed by airspeed x sin 1 deg (0.0175): 1 m/s at the
    // 60 m/s these gains were tuned at, 3 m/s at 170, so the same gains overshot three times as hard. Both scale
    // down above 60 m/s (unchanged below it, the slow planes they were tuned on).
    const F32 pitch_scale = pitchSpeedScale();
    // <WolfViewer 2026-10-08> The vertical speed asked for never jumps: it may rise quickly (land or an object ahead
    // to climb over, 2.5 m/s each second) but falls only slowly (0.8 m/s each second), so the end of a terrain climb
    // or a mode change eases the nose down instead of snapping from +8 to -8 (Paul's jerking, 16:43Z). Not on the take-off
    // roll or autoland's final, which set their own exact rates.
    if (mAP && !on_runway && !(landing && mLand >= LAND_FINAL))
    {
        if (!mWantVSSlewInit)
        {
            mWantVSSlew = mVSf;
            mWantVSSlewInit = true;
        }
        mWantVSSlew = llclamp(mWantVS, mWantVSSlew - 0.8f * dt, mWantVSSlew + 2.5f * dt);
        mWantVS = mWantVSSlew;
    }
    else
    {
        mWantVSSlew = mWantVS;
        mWantVSSlewInit = mAP;
    }
    if (mAP)
    {
        // The integrator: the pitch that gives the vertical speed asked for.
        mPitchTrim = llclamp(mPitchTrim + (mWantVS - mVSf) * 0.25f * pitch_scale * dt, -10.f, 12.f);
    }
    mFDPitch = ema(mFDPitch, llclamp(mPitchTrim + (mWantVS - mVSf) * 2.0f * pitch_scale, -12.f, 18.f), dt, 1.5f);   // steadier: 1.5 s
    // </WolfViewer>
    // <WolfViewer 2026-10-07> Take-off: no pitch keys on the roll or a rejected take-off (the director follows the nose, so
    // drive() holds none), then the rotation's nose-up, with the trim kept at the nose until it flies.
    if ((mTakeoff == TO_ROLL || mTakeoff == TO_REJECT) && mAP)
    {
        mPitchTrim = 0.f;
        mFDPitch = mData.mPitch;
    }
    else if (mTakeoff == TO_ROTATE && mAP)
    {
        mPitchTrim = llclamp(mData.mPitch, -10.f, 12.f);
        mFDPitch = TO_ROTATE_PITCH;
    }
    if (mAP && nowSeconds() >= mNextFlightLog)
    {
        mNextFlightLog = nowSeconds() + 3.0;
        LL_INFOS("WolfFlight") << "fly AP: lat " << (S32)mLateral << " vert " << (S32)mVertical << " pitch " << (S32)mData.mPitch
                               << " fd " << (S32)mFDPitch << " roll " << (S32)mData.mRoll << " fdroll " << (S32)mFDRoll
                               << " vs " << mVSf << " want " << mWantVS << " trim " << mPitchTrim << " out p/b/t "
                               << mOutPitch << "/" << mOutBank << "/" << mOutThrottle << " ias " << mData.mAirspeed
                               << " agl " << (S32)mData.mAGL << " terrain " << mTerrainNeedSmooth << LL_ENDL;
    }
}

//-----------------------------------------------------------------------------
// The keys
//-----------------------------------------------------------------------------

// Hold one key of a pair for |cmd| of every PWM period (the sign picks the key: + is the
// pair's first key, W / Up, turn left, PgUp, slide left).
void WolfFlight::holdPair(EPair pair, F32 cmd, F32 phase_offset)
{
    if (pair >= PAIR_NONE)
    {
        return;
    }
    const F32 mag = fabsf(cmd);
    if (mag < PWM_DEAD)
    {
        return;
    }
    bool on = mag > 1.f - PWM_DEAD;
    if (!on)
    {
        const F64 t = nowSeconds() / PWM_PERIOD + phase_offset;
        on = (t - floor(t)) < mag;
    }
    if (on)
    {
        gAgent.setControlFlags(PAIR_FLAGS[pair][cmd > 0.f ? 0 : 1]);
    }
}

void WolfFlight::tapPair(EPair pair, S32 dir)
{
    if (pair >= PAIR_NONE || dir == 0)
    {
        return;
    }
    gAgent.setControlFlags(PAIR_FLAGS[pair][dir > 0 ? 0 : 1]);
}

void WolfFlight::drive(F32 dt)
{
    const bool heli = craft() == CRAFT_HELI;
    const bool pitch_inv = WolfFlight::invert("WolfFlightPitchInvert");
    const bool bank_inv = WolfFlight::invert(WolfFlight::keySetting("BankInvert"));
    const bool thr_inv = WolfFlight::invert(WolfFlight::keySetting("ThrottleInvert"));
    const F64 now = nowSeconds();

    if (heli)
    {
        // Yaw to the wanted heading/track, cyclic forward for speed, collective for climb.
        const F32 cur = (mData.mGS > 5.f && mLateral != LAT_HDG) ? mData.mTrack : mData.mHeading;
        const F32 err = wrap180(mWantTrack - cur);
        mOutBank = ema(mOutBank, llclamp(err / 20.f - mData.mTurnRate / 25.f, -1.f, 1.f), dt, 0.25f);
        const F32 hd = mData.mHeading * DEG_TO_RAD;
        const F32 fwd_speed = mData.mVel.mV[VX] * sinf(hd) + mData.mVel.mV[VY] * cosf(hd);
        mOutPitch = ema(mOutPitch, llclamp((mWantSpeed - fwd_speed) / 4.f, -1.f, 1.f), dt, 0.4f);   // + = forward
        mOutThrottle = ema(mOutThrottle, llclamp((mWantVS - mVSf) / 2.f, -1.f, 1.f), dt, 0.4f);    // + = up
        holdPair(bankPair(), bank_inv ? mOutBank : -mOutBank, 0.0f);          // right = turn-right key
        holdPair(pitchPair(), pitch_inv ? -mOutPitch : mOutPitch, 0.33f);
        holdPair(throttlePair(), thr_inv ? -mOutThrottle : mOutThrottle, 0.66f);
        return;
    }

    // PLANE. Bank toward the flight director's bank, pitch toward its pitch, both damped by
    // their rates so a slow-answering airframe is not over-driven.
    // softer pitch gain and more rate damping than first shipped: smoother on the yoke
    // Steered by HEADING, not bank. Many OpenSim plane scripts turn on the left / right keys and
    // bank the model for show: Paul's Grand Caravan wobbled side to side with the AP chasing a bank
    // angle the keys barely move, and swapping LEFT KEY "makes no difference" (2026-10-06). So, like
    // the boat: toward the wanted track, a 2 degree dead band, damped by the rate of turn.
    {
        const F32 cur = (mData.mGS > 5.f && mLateral != LAT_HDG) ? mData.mTrack : mData.mHeading;
        const F32 err = mLateral == LAT_NONE ? 0.f : wrap180(mWantTrack - cur);
        // Held, not pulsed: the keys went down and up eight times a second for a part turn, and
        // the plane's script tips the wings while a turn key is down and levels them when it is
        // up, so every pulse rocked it (Paul: "still rocking like mad", roll -8 to +32). A pilot
        // holds the key: down when more than 6 degrees off, up again within 1.5 allowing for
        // the turn already going on (half a second of it), so it rolls out on the heading.
        // <WolfViewer 2026-10-08> Paul: "its still jerking". The turn after take-off at 110 m/s: roll -3 -24 -36 -37 -16 -12
        // with the director flipping -24 / +2 / -3 / -10 / +11 (17:13Z). The plane's script keeps the bank on for a second
        // or more after the key comes up, so half a second of lead let it sail past the heading and swing back. Now the
        // key comes up allowing 1.5 s of the turn still to come, and also when the bank passes 30 degrees (it went to 37);
        // it goes down again only below 20, a wide gap so the wings are not rocked by short presses.
        const F32 lead = err - mData.mTurnRate * 1.5f;
        const S32 dir = err > 0.f ? 1 : -1;
        if (mTurnHeld == 0 && fabsf(err) > 6.f && mData.mRoll * dir < 20.f) mTurnHeld = dir;
        else if (mTurnHeld != 0 && (lead * mTurnHeld < 1.5f || mData.mRoll * mTurnHeld > 30.f)) mTurnHeld = 0;
        mOutBank = (F32)mTurnHeld;   // + = right
    }
    // Softer and better damped (Paul: "still wobbling"): the pitch keys swung full down / full up
    // every few seconds with the old 1/8 gain.
    // Then still "wobbling backwards and forwards": pitch 2 to 15 degrees about a wanted 10. Plane
    // scripts pitch fast while a key is held; the keys are left alone within 2.5 degrees, and the
    // correction is gentler and more damped.
    {
        // held the same way: nose up / down when more than 4 degrees off, released within 1. <WolfViewer 2026-10-08/>
        // Fast, 4 degrees is a big vertical speed (12 m/s at 170 m/s): the dead band narrows with the speed, to 1.5
        const F32 perr = mFDPitch - mData.mPitch;
        const F32 plead = perr - mData.mPitchRate * 0.4f;
        const F32 press_deg = llmax(4.f * pitchSpeedScale(), 1.5f);
        if (mPitchHeld == 0 && fabsf(perr) > press_deg) mPitchHeld = perr > 0.f ? 1 : -1;
        else if (mPitchHeld != 0 && (plead * mPitchHeld < 1.f)) mPitchHeld = 0;
        mOutPitch = (F32)mPitchHeld;   // + = nose up
    }
    // Defaults (Paul): Up arrow / W pushes the nose down like a yoke, Down / S pulls it up;
    // the turn keys bank. Turn-right is the pair's second key.
    holdPair(bankPair(), bank_inv ? mOutBank : -mOutBank, 0.0f);
    holdPair(pitchPair(), pitch_inv ? mOutPitch : -mOutPitch, 0.5f);
    autothrottle(dt);
}

// AUTOTHROTTLE: the selected speed through the throttle keys (a plane, or a boat's motor).
// <WolfViewer 2026-10-08/> 1 up to 60 m/s (the speed the pitch gains were tuned at), then 60 / airspeed, at least 0.25
F32 WolfFlight::pitchSpeedScale() const
{
    return llclamp(60.f / llmax(mData.mAirspeed, 1.f), 0.25f, 1.f);
}

void WolfFlight::autothrottle(F32 dt)
{
    const bool thr_inv = WolfFlight::invert(WolfFlight::keySetting("ThrottleInvert"));
    const F64 now = nowSeconds();
    if (!mAT)
    {
        mOutThrottle = 0.f;
        return;
    }
    const F32 err = mWantSpeed - mData.mAirspeed;
    if (gSavedSettings.getBOOL(WolfFlight::keySetting("ThrottleSteps")))
    {
        // A stepping throttle: one notch per tap, a tap at most every TAP_INTERVAL.
        if (mThrottleTapDir != 0 && now < mThrottleTapUntil)
        {
            tapPair(throttlePair(), thr_inv ? -mThrottleTapDir : mThrottleTapDir);
            mOutThrottle = (F32)mThrottleTapDir;
            return;
        }
        mThrottleTapDir = 0;
        mOutThrottle = 0.f;
        if (now >= mThrottleNextTap && fabsf(err) > AT_DEADBAND)
        {
            const S32 dir = err > 0.f ? 1 : -1;
            // A/T FAIL: taps one way that the speed never answers (wrong keys, engine off).
            if (dir * mThrottleStreak > 0)
            {
                mThrottleStreak += dir;
            }
            else
            {
                mThrottleStreak = dir;
                mThrottleStreakSpeed = mData.mAirspeed;
            }
            if (llabs(mThrottleStreak) >= AT_FAIL_TAPS)
            {
                if ((mData.mAirspeed - mThrottleStreakSpeed) * dir < 1.f)
                {
                    mAT = false;
                    mThrottleStreak = 0;
                    postCas("A/T FAIL", CAS_CAUTION);
                    return;
                }
                mThrottleStreak = 0;   // it is answering, just slowly
            }
            mThrottleTapDir = dir;
            mThrottleTapUntil = now + TAP_SECONDS;
            mThrottleNextTap = now + TAP_INTERVAL;
            tapPair(throttlePair(), thr_inv ? -dir : dir);
            mOutThrottle = (F32)dir;
        }
        else if (fabsf(err) <= AT_DEADBAND)
        {
            mThrottleStreak = 0;
        }
    }
    else
    {
        // A held throttle: thrust while the key is down.
        mOutThrottle = ema(mOutThrottle, llclamp(err / 5.f, -1.f, 1.f), dt, 0.4f);
        holdPair(throttlePair(), thr_inv ? -mOutThrottle : mOutThrottle, 0.25f);
    }
}

// THE SIDESTICK AND THROTTLE LEVER (Paul, 2026-10-06: "add a joystick"). Flying by hand with
// the mouse, through the same keys the autopilot uses and the same settings: the stick's
// deflection is the share of each PWM period the key is held, so half a deflection is half
// the rate a held key gives. A stepping throttle gets taps, quicker the further the lever is
// pushed; a held one the PWM like the stick. Only while the autopilot is off — moving the
// stick past half way disconnects it (setStick), as a real sidestick does.
void WolfFlight::setStick(F32 x, F32 y)
{
    mStickX = llclamp(x, -1.f, 1.f);
    mStickY = llclamp(y, -1.f, 1.f);
    if (mAP && (fabsf(mStickX) > 0.5f || fabsf(mStickY) > 0.5f))
    {
        disconnectAP("AUTOPILOT DISC", true);
    }
}

void WolfFlight::setThrottleLever(F32 v)
{
    mThrottleLever = llclamp(v, -1.f, 1.f);
    if (mAT && fabsf(mThrottleLever) > 0.5f)
    {
        mAT = false;   // the pilot's hand on the thrust levers takes them back
        postCas("A/T DISC", CAS_CAUTION);
    }
}

void WolfFlight::manualDrive()
{
    if (!mData.mSeated)
    {
        return;
    }
    const F32 DEAD = 0.12f;
    auto shaped = [DEAD](F32 v)
    {
        const F32 a = fabsf(v);
        if (a < DEAD) return 0.f;
        return (v > 0.f ? 1.f : -1.f) * (a - DEAD) / (1.f - DEAD);
    };
    const F32 sx = shaped(mStickX), sy = shaped(mStickY), th = shaped(mThrottleLever);
    const bool heli = craft() == CRAFT_HELI;
    const bool pitch_inv = WolfFlight::invert("WolfFlightPitchInvert");
    const bool bank_inv = WolfFlight::invert(WolfFlight::keySetting("BankInvert"));
    const bool thr_inv = WolfFlight::invert(WolfFlight::keySetting("ThrottleInvert"));
    // the same key directions drive() uses: right = the turn pair's second key; a plane's nose
    // up = the pitch pair's second key unless inverted; a helicopter's forward = the first.
    holdPair(bankPair(), bank_inv ? sx : -sx, 0.0f);
    if (heli)
    {
        holdPair(pitchPair(), pitch_inv ? -sy : sy, 0.33f);
        holdPair(throttlePair(), thr_inv ? -th : th, 0.66f);
        return;
    }
    holdPair(pitchPair(), pitch_inv ? sy : -sy, 0.5f);
    if (!gSavedSettings.getBOOL(WolfFlight::keySetting("ThrottleSteps")))
    {
        holdPair(throttlePair(), thr_inv ? -th : th, 0.25f);
        return;
    }
    const F64 now = nowSeconds();
    if (mManualTapDir != 0 && now < mManualTapUntil)
    {
        tapPair(throttlePair(), thr_inv ? -mManualTapDir : mManualTapDir);
        return;
    }
    mManualTapDir = 0;
    if (th != 0.f && now >= mManualNextTap)
    {
        // full lever: a notch every 0.3 s; just off centre: one a second
        mManualTapDir = th > 0.f ? 1 : -1;
        mManualTapUntil = now + TAP_SECONDS;
        mManualNextTap = now + llmax(0.3f, 1.0f - 0.7f * fabsf(th));
        tapPair(throttlePair(), thr_inv ? -mManualTapDir : mManualTapDir);
    }
}

// AUTO LEARN. Over three-second windows, does the response follow the command? "Nose up" held
// should raise the nose (pitch rate +), "bank right" should roll or turn right. A clear,
// sustained opposite answer means the key pair is the other way round on this aircraft:
// turn the setting over and say so.
void WolfFlight::learn(F32 dt)
{
    if (!gSavedSettings.getBOOL("WolfFlightAutoLearn"))
    {
        return;
    }
    const bool heli = craft() == CRAFT_HELI;
    const F64 now = nowSeconds();
    if (mLearnWindowStart <= 0.0)
    {
        mLearnWindowStart = now;
    }
    // A rudder only steers with water going past it: below 1 m/s (the speed measure() trusts a
    // boat's course at) wind and waves turn the bow more than the helm does, and Paul's boat,
    // creeping out at 0.1 m/s, was told BANK KEYS REVERSED (2026-10-06). Learn nothing until
    // it is under way; the window starts again once it is.
    // Paul's plane, 2026-10-06: six seconds after CMD, still settling from the climb out, one 3 s
    // look said PITCH and BANK both reversed; with pitch swapped the AP pitched up to level off
    // and stood the plane on its tail ("almost went straight up ... really jerky"). So: nothing
    // for 10 s after engaging, a plane only in flight (10 m/s, 15 m up), 6 s windows, and a
    // reversal must show in two windows running before a setting is changed.
    const bool settling = now < mLearnHoldUntil;
    const bool not_flying = !mSail && !heli && (mData.mAirspeed < 10.f || mData.mAGL < 15.f);
    if ((mSail && mData.mGS < 1.f) || settling || not_flying)
    {
        mLearnPitchAcc = mLearnPitchWeight = 0.f;
        mLearnBankAcc = mLearnBankWeight = 0.f;
        mLearnLiftAcc = mLearnLiftWeight = 0.f;
        mLearnWindowStart = now;
        return;
    }
    if (!heli)
    {
        mLearnPitchAcc += mOutPitch * mData.mPitchRate * dt;
        mLearnPitchWeight += fabsf(mOutPitch) * dt;
    }
    else
    {
        mLearnLiftAcc += mOutThrottle * mData.mVS * dt;
        mLearnLiftWeight += fabsf(mOutThrottle) * dt;
    }
    // a boat heels with the wind, so only its turn says which way the helm went
    mLearnBankAcc += mOutBank * (mSail ? mCourseRate : heli ? mData.mTurnRate : (mData.mRollRate + mData.mTurnRate)) * dt;
    mLearnBankWeight += fabsf(mOutBank) * dt;

    if (now - mLearnWindowStart < 6.0)
    {
        return;
    }
    // A window that looks reversed adds to the streak; one that does not clears it.
    auto check = [](F32 acc, F32 weight, F32 threshold, S32& streak)
    {
        const bool rev = weight > 2.4f && acc / weight < -threshold;
        streak = rev ? streak + 1 : 0;
        return streak >= 2;
    };
    auto evidence = [](const char* what, F32 acc, F32 weight)
    {
        LL_INFOS("WolfFlight") << what << " keys look reversed: response " << (weight > 0.f ? acc / weight : 0.f)
                               << " over " << weight << " s of command" << LL_ENDL;
    };
    if (!heli && check(mLearnPitchAcc, mLearnPitchWeight, 2.f, mPitchRevStreak))
    {
        evidence("pitch", mLearnPitchAcc, mLearnPitchWeight);
        mLearnedFlip["WolfFlightPitchInvert"] = !mLearnedFlip["WolfFlightPitchInvert"];   // this session only
        postCas("PITCH KEYS REVERSED", CAS_ADVISORY);
        mPitchTrim = mData.mPitch;
        mPitchRevStreak = 0;
    }
    if (heli && check(mLearnLiftAcc, mLearnLiftWeight, 0.5f, mLiftRevStreak))
    {
        evidence("lift", mLearnLiftAcc, mLearnLiftWeight);
        mLearnedFlip[WolfFlight::keySetting("ThrottleInvert")] = !mLearnedFlip[WolfFlight::keySetting("ThrottleInvert")];   // this session only
        postCas("LIFT KEYS REVERSED", CAS_ADVISORY);
        mLiftRevStreak = 0;
    }
    if (check(mLearnBankAcc, mLearnBankWeight, 2.f, mBankRevStreak))
    {
        evidence("bank", mLearnBankAcc, mLearnBankWeight);
        mLearnedFlip[WolfFlight::keySetting("BankInvert")] = !mLearnedFlip[WolfFlight::keySetting("BankInvert")];   // this session only
        postCas("BANK KEYS REVERSED", CAS_ADVISORY);
        mBankRevStreak = 0;
    }
    mLearnPitchAcc = mLearnPitchWeight = 0.f;
    mLearnBankAcc = mLearnBankWeight = 0.f;
    mLearnLiftAcc = mLearnLiftWeight = 0.f;
    mLearnWindowStart = now;
}

//-----------------------------------------------------------------------------
// The glareshield
//-----------------------------------------------------------------------------

void WolfFlight::engageAP()
{
    if (mAP)
    {
        disconnectAP("AUTOPILOT DISC", true);
        return;
    }
    if (!mData.mValid || !mData.mSeated)
    {
        postCas("AP: NOT SEATED", CAS_CAUTION);
        return;
    }
    mWantVSSlewInit = false;   // <WolfViewer 2026-10-08/> the climb-rate limiter starts from what the plane is doing
    // <WolfViewer 2026-10-07> A plane on the ground takes off (takeoffStart) instead of refusing.
    const bool take_off = !mSail && craft() == CRAFT_PLANE && mData.mAGL < 10.f;
    clearCas("AP: NOT SEATED");
    clearCas("AP: ON GROUND");
    clearCas("AUTOPILOT DISC");
    // Paul: "why dont you turn on A/T if you need it automatically?" A plane on the autopilot
    // holds its speed too; the pilot can still switch A/T off. Its scripts often move in speed
    // steps (the Caravan read exactly 4 and 8 m/s), which A/T's one-tap-at-a-time throttle suits.
    if (!mSail && craft() == CRAFT_PLANE && !mAT)
    {
        mAT = true;
        mThrottleStreak = 0;
        clearCas("A/T FAIL");
    }
    if (mLateral == LAT_NONE)
    {
        if (mDest.mValid)
        {
            mLateral = LAT_LNAV;
        }
        else
        {
            mLateral = LAT_HDG;
            mSelHeading = (F32)llround(mData.mHeading);
        }
    }
    if (mVertical == VERT_NONE && !mSail)   // a boat has no vertical axis
    {
        mVertical = VERT_ALT;
        mSelAltFt = 100.f * llround(mData.mAltMSL * FT / 100.f);
    }
    mPitchTrim = llclamp(mData.mPitch, -10.f, 12.f);
    mThrottleStreak = 0;
    mCircleStart = nowSeconds();
    mCircleTurn = 0.f;
    mLastCourse = (mData.mGS > 0.7f) ? mData.mTrack : mData.mHeading;
    mCourseRate = 0.f;
    mArrived = false;
    mLearnWindowStart = 0.0;
    mLearnPitchAcc = mLearnPitchWeight = mLearnBankAcc = mLearnBankWeight = mLearnLiftAcc = mLearnLiftWeight = 0.f;
    mLearnHoldUntil = nowSeconds() + 10.0;
    mPitchRevStreak = mBankRevStreak = mLiftRevStreak = 0;
    mAP = true;
    if (take_off)
    {
        takeoffStart();
    }
    LL_INFOS("WolfFlight") << "AP engaged, craft " << (S32)craft() << " lateral " << (S32)mLateral
                           << " vertical " << (S32)mVertical << (take_off ? ", taking off" : "") << LL_ENDL;
}

void WolfFlight::disconnectAP(const std::string& why, bool by_pilot)
{
    if (!mAP)
    {
        return;
    }
    mAP = false;
    mAT = false;
    mOutPitch = mOutBank = mOutThrottle = 0.f;
    if (mSpeedLow)   // <WolfViewer 2026-10-08/> the pilot has it
    {
        mSpeedLow = false;
        clearCas("SPEED LOW");
    }
    if (mTakeoff != TO_NONE)   // <WolfViewer 2026-10-07/> the pilot has it
    {
        mTakeoff = TO_NONE;
        takeoffClearCas();
    }
    mApOffFlashUntil = nowSeconds() + 6.0;
    postCas(why, CAS_WARNING);
    make_ui_sound("UISndAlert");   // an autopilot never lets go silently
    LL_INFOS("WolfFlight") << "AP disconnected (" << why << ", " << (by_pilot ? "pilot" : "system") << ")" << LL_ENDL;
}

void WolfFlight::toggleAT()
{
    mAT = !mAT;
    mThrottleStreak = 0;
    if (mAT)
    {
        clearCas("A/T FAIL");
    }
}

void WolfFlight::pressLNAV()
{
    if (!mDest.mValid)
    {
        postCas("NO ACTIVE ROUTE", CAS_ADVISORY);
        return;
    }
    clearCas("NO ACTIVE ROUTE");
    mArrived = false;
    mLateral = LAT_LNAV;
}

void WolfFlight::pressHDG()
{
    if (mLateral != LAT_HDG)
    {
        mLateral = LAT_HDG;
    }
}

void WolfFlight::pressALT()
{
    mVertical = VERT_ALT;
}

void WolfFlight::pressVS()
{
    if (mVertical == VERT_VS)
    {
        mVertical = VERT_ALT;
        return;
    }
    mSelVsFpm = 100.f * llround(mData.mVS * FPM / 100.f);
    mVertical = VERT_VS;
}

void WolfFlight::pressVNAV()
{
    if (!mDest.mValid)
    {
        postCas("NO ACTIVE ROUTE", CAS_ADVISORY);
        return;
    }
    clearCas("NO ACTIVE ROUTE");
    mVertical = VERT_VNAV;
}

void WolfFlight::setSelSpeedKt(F32 v)  { mSelSpeedKt = llclamp(v, 0.f, 600.f); }
void WolfFlight::setSelHeading(F32 v)  { mSelHeading = wrap360((F32)llround(v)); }
void WolfFlight::setSelAltFt(F32 v)    { mSelAltFt = llclamp(v, -1000.f, 40000.f); }
void WolfFlight::setSelVsFpm(F32 v)    { mSelVsFpm = llclamp(v, -6000.f, 6000.f); }
void WolfFlight::setCruiseAltFt(F32 v) { mCruiseAltFt = llclamp(v, 0.f, 40000.f); }

//-----------------------------------------------------------------------------
// The route
//-----------------------------------------------------------------------------

void WolfFlight::setDestination(const std::string& region_in, const LLVector3& local, bool has_z)
{
    std::string region = region_in;
    LLStringUtil::trim(region);
    if (region.empty() && gAgent.getRegion())
    {
        region = gAgent.getRegion()->getName();
    }
    Dest d;
    d.mRegion = region;
    d.mLocal = local;
    d.mHasZ = has_z;
    d.mRequestedAt = nowSeconds();

    // A region already loaded, then one the world map knows, then ask the map server.
    for (LLViewerRegion* regionp : LLWorld::getInstance()->getRegionList())
    {
        if (regionp && LLStringUtil::compareInsensitive(regionp->getName(), region) == 0)
        {
            d.mRegion = regionp->getName();
            d.mGlobal = regionp->getOriginGlobal() + LLVector3d(local);
            d.mValid = true;
            break;
        }
    }
    if (!d.mValid)
    {
        if (LLSimInfo* info = LLWorldMap::getInstance()->simInfoFromName(region))
        {
            d.mRegion = info->getName();
            d.mGlobal = info->getGlobalOrigin() + LLVector3d(local);
            d.mValid = true;
        }
    }
    if (!d.mValid)
    {
        // <WolfViewer 2026-10-07> Ask the map server, and take ONLY a region of exactly this name when
        // the answer lands in the world map (resolveDestination). Paul: "if the autopilot cant find the
        // region it needs to say not found not take you to some weird far off place". The callback form
        // of this request goes through Firestorm's hypergrid exact-name path
        // (fsworldmapmessage.cpp processExactNamedRegionResponse), which takes a lone result with no name
        // as the match - for a name the grid does not have, that is some other region, and the plane
        // set off for it.
        d.mPending = true;
        LLWorldMapMessage::getInstance()->sendNamedRegionRequest(region);
    }
    if (!has_z)
    {
        d.mGlobal.mdV[VZ] = mData.mPosGlobal.mdV[VZ];
    }
    mDest = d;
    landReset();   // <WolfViewer 2026-10-07/> a new destination is looked up in the airports list
    rebuildRoute();
    mArrived = false;
    clearCas("DEST NOT FOUND");
    LL_INFOS("WolfFlight") << "destination " << region << " " << local << (d.mPending ? " (asking the map)" : "") << LL_ENDL;
}

void WolfFlight::resolveDestination()
{
    if (mDest.mPending)
    {
        // The map server's answer goes into the world map: a region of exactly this name, or nothing.
        if (LLSimInfo* info = LLWorldMap::getInstance()->simInfoFromName(mDest.mRegion))
        {
            mDest.mRegion = info->getName();
            mDest.mGlobal = info->getGlobalOrigin() + LLVector3d(mDest.mLocal);
            if (!mDest.mHasZ)
            {
                mDest.mGlobal.mdV[VZ] = mData.mPosGlobal.mdV[VZ];
            }
            mDest.mPending = false;
            mDest.mValid = true;
            clearCas("DEST NOT FOUND");
            landReset();
            rebuildRoute();
            LL_INFOS("WolfFlight") << "destination " << mDest.mRegion << " found at " << mDest.mGlobal << LL_ENDL;
        }
        else if (nowSeconds() - mDest.mRequestedAt > DEST_TIMEOUT)
        {
            // Not on the grid: say so, and go nowhere - no destination, the heading held.
            mDest.mPending = false;
            mDest.mFailed = true;
            mDest.mValid = false;
            if (mLateral == LAT_LNAV || mLateral == LAT_HOLD)
            {
                mLateral = LAT_HDG;
                mSelHeading = (F32)llround(mData.mHeading);
            }
            if (mVertical == VERT_VNAV)
            {
                mVertical = VERT_ALT;
            }
            postCas("DEST NOT FOUND", CAS_CAUTION);
            make_ui_sound("UISndAlert");
            LLNotificationsUtil::add("GenericAlert", LLSD().with("MESSAGE", "Autopilot: no region called \"" + mDest.mRegion
                                     + "\" was found. Check the name; the autopilot is holding the current heading."));
            LL_INFOS("WolfFlight") << "destination " << mDest.mRegion << " not found" << LL_ENDL;
        }
    }
    if (!mDest.mHasZ && mDest.mValid)
    {
        mDest.mGlobal.mdV[VZ] = mData.mPosGlobal.mdV[VZ];
    }
}

bool WolfFlight::mapHasDestination() const
{
    return LLTracker::getTrackingStatus() != LLTracker::TRACKING_NOTHING;
}

bool WolfFlight::setDestinationFromMap()
{
    if (!mapHasDestination())
    {
        postCas("NO MAP DESTINATION", CAS_ADVISORY);
        return false;
    }
    const LLVector3d pos = LLTracker::getTrackedPositionGlobal();
    Dest d;
    d.mValid = true;
    d.mGlobal = pos;
    d.mHasZ = pos.mdV[VZ] > 1.0;   // a landmark has a height, a map click does not
    d.mRegion = "MAP";
    if (LLSimInfo* info = LLWorldMap::getInstance()->simInfoFromPosGlobal(pos))
    {
        d.mRegion = info->getName();
        const LLVector3d o = info->getGlobalOrigin();
        d.mLocal.set((F32)(pos.mdV[VX] - o.mdV[VX]), (F32)(pos.mdV[VY] - o.mdV[VY]), (F32)pos.mdV[VZ]);
    }
    d.mRequestedAt = nowSeconds();
    mDest = d;
    landReset();   // <WolfViewer 2026-10-07/>
    rebuildRoute();
    mArrived = false;
    clearCas("NO MAP DESTINATION");
    return true;
}

void WolfFlight::clearTrip()
{
    landReset();   // <WolfViewer 2026-10-07/>
    mDest = Dest();
    mRoute = Route();
    mArrived = false;
    mCas.clear();
    mMasterWarning = mMasterCaution = false;
}

void WolfFlight::clearDestination()
{
    landReset();   // <WolfViewer 2026-10-07/>
    mDest = Dest();
    mRoute = Route();
    if (mLateral == LAT_LNAV || mLateral == LAT_HOLD)
    {
        mLateral = LAT_HDG;
        mSelHeading = (F32)llround(mData.mHeading);
    }
    if (mVertical == VERT_VNAV || mVertical == VERT_HOVER)
    {
        mVertical = VERT_ALT;
    }
}

F32 WolfFlight::destDistance() const
{
    if (!mDest.mValid)
    {
        return -1.f;
    }
    const F64 dx = mDest.mGlobal.mdV[VX] - mData.mPosGlobal.mdV[VX];
    const F64 dy = mDest.mGlobal.mdV[VY] - mData.mPosGlobal.mdV[VY];
    return (F32)sqrt(dx * dx + dy * dy);
}

F32 WolfFlight::destBearing() const
{
    if (!mDest.mValid)
    {
        return 0.f;
    }
    const F64 dx = mDest.mGlobal.mdV[VX] - mData.mPosGlobal.mdV[VX];
    const F64 dy = mDest.mGlobal.mdV[VY] - mData.mPosGlobal.mdV[VY];
    return wrap360((F32)(atan2(dx, dy) * RAD_TO_DEG));
}

F32 WolfFlight::destEtaSeconds() const
{
    if (!mDest.mValid || mData.mGS < 1.f)
    {
        return -1.f;
    }
    return destDistance() / mData.mGS;
}

//-----------------------------------------------------------------------------
// Crew alerting
//-----------------------------------------------------------------------------

void WolfFlight::postCas(const std::string& text, ECasLevel level)
{
    for (Cas& c : mCas)
    {
        if (c.mText == text)
        {
            c.mLevel = level;
            return;   // already showing: not a new alert
        }
    }
    Cas c;
    c.mText = text;
    c.mLevel = level;
    c.mTime = nowSeconds();
    mCas.push_front(c);
    while (mCas.size() > 12)
    {
        mCas.pop_back();
    }
    if (level == CAS_WARNING) mMasterWarning = true;
    if (level == CAS_CAUTION) mMasterCaution = true;
    LL_INFOS("WolfFlight") << "CAS: " << text << LL_ENDL;
}

void WolfFlight::clearCas(const std::string& text)
{
    for (auto it = mCas.begin(); it != mCas.end(); ++it)
    {
        if (it->mText == text)
        {
            mCas.erase(it);
            return;
        }
    }
}

bool WolfFlight::masterWarning() const { return mMasterWarning; }
bool WolfFlight::masterCaution() const { return mMasterCaution; }

// Pressing the master lights acknowledges: they go out, and the messages that are only
// reports (not conditions the aircraft is still in) go with them.
void WolfFlight::cancelMasters()
{
    mMasterWarning = mMasterCaution = false;
    mApOffFlashUntil = 0.0;
    for (auto it = mCas.begin(); it != mCas.end();)
    {
        if (it->mText != "PULL UP" && it->mText != "TERRAIN")
        {
            it = mCas.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

//-----------------------------------------------------------------------------
// The aircraft's own controls
//-----------------------------------------------------------------------------

WolfFlight::ECraft WolfFlight::craft()
{
    return gSavedSettings.getS32("WolfFlightCraft") == 1 ? CRAFT_HELI : CRAFT_PLANE;
}

namespace
{
    WolfFlight::EPair pairSetting(const char* name, WolfFlight::EPair fallback)
    {
        const S32 v = gSavedSettings.getS32(name);
        return (v >= 0 && v <= (S32)WolfFlight::PAIR_NONE) ? (WolfFlight::EPair)v : fallback;
    }
}

WolfFlight::EPair WolfFlight::pitchPair()    { return pairSetting("WolfFlightPitchPair", PAIR_FWD_BACK); }
bool WolfFlight::invert(const char* setting)
{
    const WolfFlight& f = WolfFlight::instance();
    const auto it = f.mLearnedFlip.find(setting);
    return gSavedSettings.getBOOL(setting) != (it != f.mLearnedFlip.end() && it->second);
}

void WolfFlight::setInvert(const char* setting, bool reversed)
{
    WolfFlight::instance().mLearnedFlip.erase(setting);
    gSavedSettings.setBOOL(setting, reversed);
}

const char* WolfFlight::keySetting(const char* what)
{
    static const char* const FLIGHT[] = { "WolfFlightBankInvert", "WolfFlightThrottleInvert", "WolfFlightThrottleSteps",
                                          "WolfFlightBankPair", "WolfFlightThrottlePair" };
    static const char* const SAIL[] = { "WolfSailBankInvert", "WolfSailThrottleInvert", "WolfSailThrottleSteps",
                                        "WolfSailBankPair", "WolfSailThrottlePair" };
    static const char* const WHAT[] = { "BankInvert", "ThrottleInvert", "ThrottleSteps", "BankPair", "ThrottlePair" };
    const bool sail = WolfFlight::instance().sailing();
    for (S32 i = 0; i < 5; ++i)
    {
        if (!strcmp(what, WHAT[i]))
        {
            return sail ? SAIL[i] : FLIGHT[i];
        }
    }
    llassert(false);
    return FLIGHT[0];
}

WolfFlight::EPair WolfFlight::bankPair()     { return pairSetting(WolfFlight::keySetting("BankPair"), PAIR_TURN); }
WolfFlight::EPair WolfFlight::throttlePair() { return pairSetting(WolfFlight::keySetting("ThrottlePair"), PAIR_UP_DOWN); }

const char* WolfFlight::pairName(EPair p)
{
    switch (p)
    {
    case PAIR_FWD_BACK: return "UP/DN ARROW";
    case PAIR_TURN:     return "L/R ARROW";
    case PAIR_UP_DOWN:  return "PGUP/PGDN";
    case PAIR_SLIDE:    return "SHIFT L/R";
    default:            return "NONE";
    }
}

void WolfFlight::sendGearCommand()
{
    const std::string cmd = gSavedSettings.getString("WolfFlightGearCommand");
    if (cmd.empty())
    {
        postCas("GEAR CMD NOT SET", CAS_ADVISORY);
        return;
    }
    // The command as chat to the aircraft's script, on the channel set for it (0 = said aloud).
    FSNearbyChat::sendChatFromViewerFinal(cmd, cmd, CHAT_TYPE_NORMAL, false, gSavedSettings.getS32("WolfFlightGearChannel"));
    mGearDown = !mGearDown;
    mGearMovedAt = nowSeconds();
}

void WolfFlight::sendEngineCommand()
{
    const std::string cmd = gSavedSettings.getString("WolfFlightEngineCommand");
    if (cmd.empty())
    {
        postCas("ENG CMD NOT SET", CAS_ADVISORY);
        return;
    }
    FSNearbyChat::sendChatFromViewerFinal(cmd, cmd, CHAT_TYPE_NORMAL, false, gSavedSettings.getS32("WolfFlightEngineChannel"));
}

namespace
{
    const F64 KEY_FRESH = 0.25;   // a held key is scanned every frame; a lost key-up clears after this
}

S32 WolfFlight::pilotPitch() const
{
    const F64 t = nowSeconds();
    const EPair p = pitchPair();
    if (p >= PAIR_NONE) return 0;
    const S32 dir = (t - mKeySeen[p][0] < KEY_FRESH ? 1 : 0) - (t - mKeySeen[p][1] < KEY_FRESH ? 1 : 0);
    if (craft() == CRAFT_HELI)
    {
        return WolfFlight::invert("WolfFlightPitchInvert") ? -dir : dir;   // + = forward
    }
    return WolfFlight::invert("WolfFlightPitchInvert") ? dir : -dir;       // + = nose up
}

S32 WolfFlight::pilotBank() const
{
    const F64 t = nowSeconds();
    const EPair p = bankPair();
    if (p >= PAIR_NONE) return 0;
    const S32 dir = (t - mKeySeen[p][0] < KEY_FRESH ? 1 : 0) - (t - mKeySeen[p][1] < KEY_FRESH ? 1 : 0);
    return WolfFlight::invert(WolfFlight::keySetting("BankInvert")) ? dir : -dir;        // + = right
}

S32 WolfFlight::pilotThrottle() const
{
    const F64 t = nowSeconds();
    const EPair p = throttlePair();
    if (p >= PAIR_NONE) return 0;
    const S32 dir = (t - mKeySeen[p][0] < KEY_FRESH ? 1 : 0) - (t - mKeySeen[p][1] < KEY_FRESH ? 1 : 0);
    return WolfFlight::invert(WolfFlight::keySetting("ThrottleInvert")) ? -dir : dir;    // + = more
}

// Source: wolfvehiclecontrols.cpp WolfVehicle::noteScanKey — the same watch-only hook in
// LLViewerInput::scanKey, after the UI-has-the-key bail. The default bindings
// (key_bindings.xml): arrows and WASD move/turn, PgUp/E up, PgDn/C down, Shift+arrows slide.
void WolfFlight::noteScanKey(KEY key, MASK mask, bool key_down, bool key_up, bool key_level, bool repeat)
{
    if (!mActive)
    {
        return;
    }
    if (mask != MASK_NONE && mask != MASK_SHIFT)
    {
        return;
    }
    const bool shift = mask == MASK_SHIFT;
    S32 pair = -1, side = 0;
    if (key == KEY_UP || key == 'W')               { pair = PAIR_FWD_BACK; side = 0; }
    else if (key == KEY_DOWN || key == 'S')        { pair = PAIR_FWD_BACK; side = 1; }
    else if (key == KEY_LEFT || key == 'A')        { pair = shift ? PAIR_SLIDE : PAIR_TURN; side = 0; }
    else if (key == KEY_RIGHT || key == 'D')       { pair = shift ? PAIR_SLIDE : PAIR_TURN; side = 1; }
    else if (key == KEY_PAGE_UP || key == 'E')     { pair = PAIR_UP_DOWN; side = 0; }
    else if (key == KEY_PAGE_DOWN || key == 'C')   { pair = PAIR_UP_DOWN; side = 1; }
    if (pair < 0)
    {
        return;
    }
    const bool held = key_level || (key_down && !key_up);
    if (held)
    {
        mKeySeen[pair][side] = nowSeconds();
    }
    // The pilot taking the controls disconnects the autopilot, as a force on the yoke does.
    if (mAP && key_down && !repeat && mData.mSeated)
    {
        disconnectAP("AUTOPILOT DISC", true);
    }
}

//-----------------------------------------------------------------------------
// The route, for both modes (Paul: "a map on plane and boat showing the route its planning to
// take, and where it is on that route")
//-----------------------------------------------------------------------------

void WolfFlight::rebuildRoute()
{
    mRoute = Route();
    mTack = 0;
    if (!mDest.mValid)
    {
        return;
    }
    if (mSail)
    {
        planWaterRoute();
        return;
    }
    // A plane flies straight there (Paul: "it can just go straight there").
    mRoute.mPts.push_back(mData.mValid ? mData.mPosGlobal : mDest.mGlobal);
    mRoute.mPts.push_back(mDest.mGlobal);
    mRoute.mLeg = 1;
    mRoute.mValid = true;
    mRoute.mPlannedAt = nowSeconds();
}

LLVector3d WolfFlight::activeWaypoint() const
{
    if (mRoute.mValid && mRoute.mLeg < (S32)mRoute.mPts.size())
    {
        return mRoute.mPts[mRoute.mLeg];
    }
    return mDest.mGlobal;
}

F32 WolfFlight::routeTotal() const
{
    F32 total = 0.f;
    for (size_t i = 1; i < mRoute.mPts.size(); ++i)
    {
        const LLVector3d d = mRoute.mPts[i] - mRoute.mPts[i - 1];
        total += (F32)sqrt(d.mdV[VX] * d.mdV[VX] + d.mdV[VY] * d.mdV[VY]);
    }
    return total;
}

// Along the route: the legs behind, plus how far down the current leg the craft has come.
F32 WolfFlight::routeFlown() const
{
    if (!mRoute.mValid || mRoute.mLeg < 1 || mRoute.mLeg >= (S32)mRoute.mPts.size())
    {
        return 0.f;
    }
    F32 flown = 0.f;
    for (S32 i = 1; i < mRoute.mLeg; ++i)
    {
        const LLVector3d d = mRoute.mPts[i] - mRoute.mPts[i - 1];
        flown += (F32)sqrt(d.mdV[VX] * d.mdV[VX] + d.mdV[VY] * d.mdV[VY]);
    }
    const LLVector3d a = mRoute.mPts[mRoute.mLeg - 1], b = mRoute.mPts[mRoute.mLeg];
    const F64 lx = b.mdV[VX] - a.mdV[VX], ly = b.mdV[VY] - a.mdV[VY];
    const F64 len = sqrt(lx * lx + ly * ly);
    if (len > 0.01)
    {
        const F64 px = mData.mPosGlobal.mdV[VX] - a.mdV[VX], py = mData.mPosGlobal.mdV[VY] - a.mdV[VY];
        flown += (F32)llclamp((px * lx + py * ly) / len, 0.0, len);
    }
    return flown;
}

// Move on to the next leg once the turning point is reached (or passed), and keep a boat's
// route fresh: re-planned every 20 s while it is being followed (land loads in as the boat
// goes), and at once when a shoal or obstacle turned up ahead.
void WolfFlight::advanceRoute()
{
    if (!mDest.mValid)
    {
        return;
    }
    const F64 now = nowSeconds();
    if (mSail && (mRouteDirty || (mLateral == LAT_LNAV && now - mRoute.mPlannedAt > 20.0)))
    {
        mRouteDirty = false;
        planWaterRoute();
    }
    if (!mRoute.mValid)
    {
        return;
    }
    const F32 capture = mSail ? 20.f : 400.f;
    while (mRoute.mLeg < (S32)mRoute.mPts.size() - 1)
    {
        const LLVector3d a = mRoute.mPts[mRoute.mLeg - 1], b = mRoute.mPts[mRoute.mLeg];
        const F64 dx = b.mdV[VX] - mData.mPosGlobal.mdV[VX], dy = b.mdV[VY] - mData.mPosGlobal.mdV[VY];
        const F64 lx = b.mdV[VX] - a.mdV[VX], ly = b.mdV[VY] - a.mdV[VY];
        const bool reached = dx * dx + dy * dy < capture * capture;
        const bool passed = (dx * lx + dy * ly) < 0.0;   // the turning point is behind
        if (!reached && !passed)
        {
            break;
        }
        ++mRoute.mLeg;
        mTack = 0;
    }
}

// Land or sea from the world map, for the parts of the route whose regions are not loaded.
// Source: wolfmapglobe.cpp KEY_COLOURS / KEY_FAR — the water colours the map tiles are painted
// with on this grid (MapColorWater #112D54; the map service's empty space (29,72,96),
// MapImageService.cs:64; OpenSim's default #1D475F), and the distance in RGB units beyond which
// a pixel is not water. Tiles are fetched (and asked to keep their pixels) as they are needed;
// one not here yet answers "unknown" and the next re-plan (20 s) has it.
// Returns 0 water, 1 land, 2 unknown.
namespace
{
    struct MapKey { F32 r, g, b; };
    const MapKey MAP_WATER[] = { { 0x11, 0x2D, 0x54 }, { 29, 72, 96 }, { 0x1D, 0x47, 0x5F } };
    const F32 MAP_WATER_FAR = 26.f;
    // Warp3D draws a region's water as (29, 72, 96) at alpha 216 OVER the seabed
    // (Warp3DImageModule.cs:59 WATER_COLOR), so open sea on the tiles is that blend: each channel
    // 216/255 of the water plus 39/255 of a seabed anywhere from 0 to 255 - r 24.6..63.6,
    // g 61.0..100.0, b 81.3..120.3 - with a little more either side for JPEG. Paul's open sea
    // read (58, 90, 110), 37 from the nearest flat colour, and every route came back NO WATER
    // ROUTE (2026-10-06, to Breathe Resort).
    bool mapBlendIsWater(F32 r, F32 g, F32 b)
    {
        const F32 A = 216.f / 255.f, J = 6.f;
        const F32 r0 = 29.f * A, g0 = 72.f * A, b0 = 96.f * A, span = 255.f * (1.f - A);
        return r >= r0 - J && r <= r0 + span + J && g >= g0 - J && g <= g0 + span + J
            && b >= b0 - J && b <= b0 + span + J;
    }

    U8 mapWaterAt(F64 gx, F64 gy, S32 level)
    {
        U32 tx = 0, ty = 0;
        LLWorldMipmap::globalToMipmap(gx, gy, level, &tx, &ty);
        LLPointer<LLViewerFetchedTexture> tile = LLWorldMap::getInstance()->getObjectsTile(tx, ty, level, true);
        if (!tile)
        {
            return 0;   // no tile there at all: no region, open sea on this grid
        }
        tile->forceToSaveRawImage(0);
        if (!tile->hasSavedRawImage())
        {
            return 2;
        }
        LLPointer<LLImageRaw> raw = tile->getSavedRawImage();
        if (raw.isNull())
        {
            return 2;
        }
        LLImageDataSharedLock lock(raw);
        const S32 w = raw->getWidth(), h = raw->getHeight(), c = raw->getComponents();
        const U8* data = raw->getData();
        if (!data || w <= 0 || h <= 0 || c < 3)
        {
            return 2;
        }
        // the tile's south-west corner and size (llworldmapview.cpp drawMipmapLevel); row 0 is
        // its southern edge, as the map draws texture t = 0 at the bottom
        const F64 tile_m = (F64)LLWorldMipmap::MAP_TILE_SIZE * (1 << (level - 1));
        const F64 u = (gx - (F64)tx * REGION_WIDTH_METERS) / tile_m, v = (gy - (F64)ty * REGION_WIDTH_METERS) / tile_m;
        const S32 px = llclamp((S32)(u * w), 0, w - 1), py = llclamp((S32)(v * h), 0, h - 1);
        const U8* in = data + (py * w + px) * c;
        F32 nearest = F32_MAX;
        for (const MapKey& k : MAP_WATER)
        {
            const F32 dr = in[0] - k.r, dg = in[1] - k.g, db = in[2] - k.b;
            nearest = llmin(nearest, dr * dr + dg * dg + db * db);
        }
        if (sqrtf(nearest) < MAP_WATER_FAR || mapBlendIsWater(in[0], in[1], in[2]))
        {
            return 0;
        }
        // Zoomed-out tiles paint grid squares with no region black - the open sea between regions
        // (Paul: "we have opensea so you dont have to have regions between them"); measured (0, 0, 5)
        // on map-4-5040-5000. And a region may set its own water colour: Breathe Resort's
        // neighbour reads (39, 124, 217). Clearly blue (blue well above red and green) is water.
        const S32 r = in[0], g = in[1], b = in[2];
        if (llmax(r, llmax(g, b)) < 16 || (b >= 80 && b - r >= 40 && b - g >= 20))
        {
            return 0;
        }
        return 1;
    }
}

// THE WATER ROUTE (Paul: "sailing — it has to avoid land when on auto pilot"). A grid over the
// boat, the destination and a margin round both; a cell is open water when the seabed there is
// deeper than the boat's draft (WolfSailDraft) below the region's water level, with a clearance
// of a cell or more round any land. A* (8-neighbour, octile heuristic) from the boat to the
// destination, then the path pulled straight: each turning point is the furthest cell still in
// clear water from the last. Land comes from the terrain of the regions the viewer has loaded,
// and beyond them from the world map's tiles (mapWaterAt); a stretch neither knows yet is taken
// as open sea and the route says UNSURVEYED (it is re-planned every 20 s as tiles arrive).
void WolfFlight::planWaterRoute()
{
    Route r;
    r.mPlannedAt = nowSeconds();
    if (!mDest.mValid || !mData.mValid)
    {
        mRoute = r;
        return;
    }
    const F32 draft = llmax(0.f, gSavedSettings.getF32("WolfSailDraft"));
    const F64 sx = mData.mPosGlobal.mdV[VX], sy = mData.mPosGlobal.mdV[VY];
    const F64 gx = mDest.mGlobal.mdV[VX], gy = mDest.mGlobal.mdV[VY];
    const F64 dist = sqrt((gx - sx) * (gx - sx) + (gy - sy) * (gy - sy));
    const F64 margin = llmax(200.0, dist * 0.35);
    const F64 x0 = llmin(sx, gx) - margin, y0 = llmin(sy, gy) - margin;
    const F64 w = fabs(gx - sx) + 2.0 * margin, h = fabs(gy - sy) + 2.0 * margin;
    const F64 cell = llmax(6.0, llmax(w, h) / 320.0);
    const S32 nx = (S32)ceil(w / cell), ny = (S32)ceil(h / cell);
    const S32 N = nx * ny;
    LLWorld* world = LLWorld::getInstance();

    // 0 open, 1 land, 2 unknown (neither loaded nor on the map yet: sailed as open sea)
    // The map level whose pixels are a quarter of a cell or finer (level L is 2^(L-1) m a pixel).
    S32 map_level = 1;
    while (map_level < 5 && (F64)(1 << map_level) <= cell / 4.0) ++map_level;
    std::vector<U8> kind(N, 0);
    for (S32 j = 0; j < ny; ++j)
    {
        for (S32 i = 0; i < nx; ++i)
        {
            const LLVector3d p(x0 + (i + 0.5) * cell, y0 + (j + 0.5) * cell, 0.0);
            LLViewerRegion* regionp = world->getRegionFromPosGlobal(p);
            if (!regionp)
            {
                // not loaded: the world map says land or sea
                kind[j * nx + i] = mapWaterAt(p.mdV[VX], p.mdV[VY], map_level);
                continue;
            }
            const F32 ground = world->resolveLandHeightGlobal(p);
            kind[j * nx + i] = (ground < regionp->getWaterHeight() - draft) ? 0 : 1;
        }
    }
    // clearance round the land: at least 8 m, at least a cell
    const S32 k = llmax(1, (S32)ceil(8.0 / cell));
    std::vector<U8> blocked(N, 0);
    for (S32 j = 0; j < ny; ++j)
    {
        for (S32 i = 0; i < nx; ++i)
        {
            if (kind[j * nx + i] != 1) continue;
            for (S32 b = llmax(0, j - k); b <= llmin(ny - 1, j + k); ++b)
                for (S32 a = llmax(0, i - k); a <= llmin(nx - 1, i + k); ++a)
                    blocked[b * nx + a] = 1;
        }
    }
    auto cellOf = [&](F64 x, F64 y) { return (S32)llclamp((S32)((y - y0) / cell), 0, ny - 1) * nx
                                           + llclamp((S32)((x - x0) / cell), 0, nx - 1); };
    const S32 start = cellOf(sx, sy);
    S32 goal = cellOf(gx, gy);
    // the boat may be at a quay and the destination on a slipway: their own cells are open
    blocked[start] = 0;
    blocked[goal] = 0;

    // A*
    std::vector<F32> g(N, 1e30f);
    std::vector<S32> parent(N, -1);
    std::vector<U8> closed(N, 0);
    typedef std::pair<F32, S32> QE;
    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> open;
    auto heur = [&](S32 c)
    {
        const F32 dx = (F32)llabs(c % nx - goal % nx), dy = (F32)llabs(c / nx - goal / nx);
        return (dx + dy + (1.41421f - 2.f) * llmin(dx, dy));
    };
    g[start] = 0.f;
    open.push(QE(heur(start), start));
    bool found = false;
    while (!open.empty())
    {
        const S32 c = open.top().second;
        open.pop();
        if (closed[c]) continue;
        closed[c] = 1;
        if (c == goal) { found = true; break; }
        const S32 ci = c % nx, cj = c / nx;
        for (S32 dj = -1; dj <= 1; ++dj)
        {
            for (S32 di = -1; di <= 1; ++di)
            {
                if (!di && !dj) continue;
                const S32 ni = ci + di, nj = cj + dj;
                if (ni < 0 || nj < 0 || ni >= nx || nj >= ny) continue;
                const S32 n = nj * nx + ni;
                if (blocked[n] || closed[n]) continue;
                // no cutting a land corner diagonally
                if (di && dj && (blocked[cj * nx + ni] || blocked[nj * nx + ci])) continue;
                const F32 step = (di && dj) ? 1.41421f : 1.f;
                if (g[c] + step < g[n])
                {
                    g[n] = g[c] + step;
                    parent[n] = c;
                    open.push(QE(g[n] + heur(n), n));
                }
            }
        }
    }
    // The destination on land (a region's middle usually is): sail to the reachable water nearest
    // it, at the region's edge (Paul: "just get it to the region not in the middle its much more
    // likely to be okay at the edge"). A* has closed every cell it could reach.
    static const F64 SHORE_MAX_M = 1000.0;
    if (!found)
    {
        S32 best = -1;
        F64 best_d = 1e30;
        for (S32 c = 0; c < N; ++c)
        {
            if (!closed[c]) continue;
            const F64 di = (F64)(c % nx - goal % nx), dj = (F64)(c / nx - goal / nx);
            const F64 d = di * di + dj * dj;
            if (d < best_d) { best_d = d; best = c; }
        }
        if (best >= 0 && sqrt(best_d) * cell <= SHORE_MAX_M)
        {
            goal = best;
            found = true;
            r.mShoreEnd = true;
        }
    }
    if (!found)
    {
        r.mNoWay = true;
        mRoute = r;
        postCas("NO WATER ROUTE", CAS_CAUTION);
        LL_INFOS("WolfFlight") << "no water route to " << mDest.mRegion << " (" << nx << "x" << ny << " cells of " << cell << " m)" << LL_ENDL;
        return;
    }
    clearCas("NO WATER ROUTE");
    std::vector<S32> path;
    for (S32 c = goal; c != -1; c = parent[c]) path.push_back(c);
    std::reverse(path.begin(), path.end());

    // Pull the path straight: from each turning point, the furthest cell in clear water.
    auto clearLine = [&](S32 a, S32 b)
    {
        const F64 ax = a % nx + 0.5, ay = a / nx + 0.5, bx = b % nx + 0.5, by = b / nx + 0.5;
        const F64 len = sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
        const S32 steps = llmax(1, (S32)ceil(len * 3.0));
        for (S32 s = 0; s <= steps; ++s)
        {
            const F64 t = (F64)s / steps;
            const S32 i = (S32)(ax + (bx - ax) * t), j = (S32)(ay + (by - ay) * t);
            if (blocked[j * nx + i]) return false;
        }
        return true;
    };
    const F32 water = mData.mWaterZ;
    r.mPts.push_back(LLVector3d(sx, sy, water));
    size_t at = 0;
    while (at < path.size() - 1)
    {
        size_t best = at + 1;
        for (size_t t = path.size() - 1; t > at + 1; --t)
        {
            if (clearLine(path[at], path[t])) { best = t; break; }
        }
        at = best;
        if (at == path.size() - 1 && !r.mShoreEnd)
        {
            r.mPts.push_back(LLVector3d(gx, gy, water));
        }
        else
        {
            const S32 c = path[at];
            r.mPts.push_back(LLVector3d(x0 + (c % nx + 0.5) * cell, y0 + (c / nx + 0.5) * cell, water));
        }
    }
    for (S32 c : path)
    {
        if (kind[c] == 2) { r.mUnsurveyed = true; break; }
    }
    r.mLeg = 1;
    r.mValid = r.mPts.size() >= 2;
    mRoute = r;
    LL_INFOS("WolfFlight") << "water route to " << mDest.mRegion << ": " << (r.mPts.size() - 1) << " legs, "
                           << (S32)routeTotal() << " m" << (r.mUnsurveyed ? " (part unsurveyed)" : "")
                           << (r.mShoreEnd ? " (to the shore nearest it)" : "") << LL_ENDL;
}

//-----------------------------------------------------------------------------
// Sailing
//-----------------------------------------------------------------------------

F32 WolfFlight::tackAngle()
{
    return llclamp(gSavedSettings.getF32("WolfSailTackAngle"), 25.f, 80.f);
}

void WolfFlight::setSelTWA(F32 v)
{
    mSelTWA = llclamp(wrap180((F32)llround(v)), -180.f, 180.f);
}

void WolfFlight::pressWIND()
{
    // hold the wind angle the boat has now, to the degree
    mSelTWA = (F32)llround(mData.mTWA);
    mLateral = LAT_WIND;
}

// TACK: through the wind to the same angle on the other side (a gybe, sailing downwind).
void WolfFlight::pressTACK()
{
    mLastTack = nowSeconds();
    if (mLateral == LAT_WIND)
    {
        mSelTWA = -mSelTWA;
    }
    else if (mLateral == LAT_HDG)
    {
        mSelHeading = wrap360(mData.mTWDSteady + wrap180(mData.mTWDSteady - mData.mHeading));   // the mirror image of this heading in the wind
    }
    else
    {
        mTack = (mTack >= 0) ? -1 : 1;
    }
    postCas(fabsf(mData.mTWA) > 90.f ? "GYBING" : "TACKING", CAS_MEMO);
}

// A ray along a direction from the boat or aircraft: seconds to the first solid thing in the
// way (gPipeline.lineSegmentIntersectInWorld, the picking ray), not counting the craft itself.
F32 WolfFlight::obstacleSeconds(const LLVector3& dir_in, F32 speed, F32 skip)
{
    if (speed < 0.5f)
    {
        return 1e9f;
    }
    LLVector3 dir = dir_in;
    dir.normVec();
    const LLVector3 pos = gAgent.getPosAgentFromGlobal(mData.mPosGlobal);
    const F32 reach = llmax(speed * 10.f, skip + 20.f);
    LLVector4a start, end, hit;
    start.load3((pos + dir * skip).mV);
    end.load3((pos + dir * reach).mV);
    S32 face = -1;
    LLViewerObject* objectp = gPipeline.lineSegmentIntersectInWorld(start, end, false, false, true, false, &face,
                                                                     nullptr, nullptr, &hit);
    LLViewerObject* own = vehicleRoot();
    if (!objectp || objectp->isAvatar() || (own && objectp->getRootEdit() == own))
    {
        return 1e9f;
    }
    LLVector3 h(hit.getF32ptr());
    return (h - pos).length() / speed;
}

// Metres along `dir` from `from` to the first thing in the way, or 1e9. The boat's own prims, the
// people aboard and anyone's attachments are stepped through (the ray starts inside the hull).
F32 WolfFlight::boatRay(const LLVector3& from, const LLVector3& dir, F32 reach)
{
    LLViewerObject* own = vehicleRoot();
    const LLVector3 to = from + dir * reach;
    LLVector3 a = from;
    for (S32 k = 0; k < 8; ++k)
    {
        if ((a - from) * dir >= reach)
        {
            break;
        }
        LLVector4a start, end, hit;
        start.load3(a.mV);
        end.load3(to.mV);
        S32 face = -1;
        LLViewerObject* o = gPipeline.lineSegmentIntersectInWorld(start, end, false, false, true, false, &face,
                                                                  nullptr, nullptr, &hit);
        if (!o)
        {
            return 1e9f;
        }
        const LLVector3 h(hit.getF32ptr());
        if ((own && (o == own || o->getRootEdit() == own)) || o->isAvatar() || o->isAttachment())
        {
            a = h + dir * 0.25f;   // through our own hull, a passenger, an attachment
            continue;
        }
        return llmax(0.f, (h - from) * dir);
    }
    return 1e9f;
}

// The boat's alarms: the depth sounder, a shoal ahead on this course, something in the way.
// A shoal or an obstacle ahead under the autopilot turns the boat away (to starboard, as the
// rule of the road has it) and has the route re-planned.
void WolfFlight::sailAlarms()
{
    const F64 now = nowSeconds();
    const F32 draft = llmax(0.f, gSavedSettings.getF32("WolfSailDraft"));
    if (mData.mSeated && mData.mDepth < draft + 0.5f)
    {
        postCas("SHALLOW WATER", CAS_CAUTION);
    }
    else
    {
        clearCas("SHALLOW WATER");
    }
    bool shoal = false;
    const F32 speed = mData.mGS;
    if (mData.mSeated)   // looking ahead even when stopped: a boat stuck against something must see it
    {
        for (F32 t = 2.f; speed > 0.5f && t <= 12.f; t += 2.f)
        {
            LLVector3d p = mData.mPosGlobal;
            p.mdV[VX] += mData.mVel.mV[VX] * t;
            p.mdV[VY] += mData.mVel.mV[VY] * t;
            LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosGlobal(p);
            if (!regionp) break;
            if (LLWorld::getInstance()->resolveLandHeightGlobal(p) > regionp->getWaterHeight() - draft)
            {
                shoal = true;
                break;
            }
        }
        if (now >= mNextBoatRay)
        {
            // Paul: "its sailing through objects on autopilot ... it needs to look ahead and avoid
            // objects in front of it". One ray from the seat missed anything low (pontoons, rocks,
            // other hulls) or off the centre line. Now six: across the bow at its centre and
            // 2.5 m either side, each at 0.4 m and 1.6 m above the water, 12 s or 30 m ahead.
            mNextBoatRay = now + 0.15;
            LLVector3 dir = speed > 0.7f ? LLVector3(mData.mVel.mV[VX], mData.mVel.mV[VY], 0.f)
                                         : LLVector3(sinf(mData.mHeading * DEG_TO_RAD), cosf(mData.mHeading * DEG_TO_RAD), 0.f);
            dir.normVec();
            const LLVector3 right(dir.mV[VY], -dir.mV[VX], 0.f);
            LLVector3 pos = gAgent.getPosAgentFromGlobal(mData.mPosGlobal);
            const F32 reach = llmax(speed * 12.f, 30.f);
            F32 clear[3] = { 1e9f, 1e9f, 1e9f };   // port, centre, starboard
            for (S32 side = -1; side <= 1; ++side)
            {
                for (F32 hgt : { 0.4f, 1.6f })
                {
                    LLVector3 from = pos + right * (2.5f * side);
                    from.mV[VZ] = mData.mWaterZ + hgt;
                    clear[side + 1] = llmin(clear[side + 1], boatRay(from, dir, reach));
                }
            }
            mBoatClearL = clear[0];
            mBoatClearR = clear[2];
            mBoatObstacle = llmin(clear[0], llmin(clear[1], clear[2])) / llmax(speed, 0.5f);
        }
    }
    else
    {
        mBoatObstacle = 1e9f;
        mBoatClearL = mBoatClearR = 1e9f;
    }
    // Close in metres as well as in seconds: at 0.3 m/s eight seconds is 2.4 m, and Paul's boat got
    // "VERY close to another boat before turning" with it seen 14 m ahead (2026-10-06).
    const F32 nearest_m = llmin(mBoatClearL, llmin(mBoatClearR, mBoatObstacle * llmax(speed, 0.5f)));
    const bool collision = mBoatObstacle < 8.f || nearest_m < 15.f;
    if (shoal) postCas("SHOAL AHEAD", CAS_WARNING); else clearCas("SHOAL AHEAD");
    if (collision) postCas("COLLISION AHEAD", CAS_WARNING); else clearCas("COLLISION AHEAD");
    if ((shoal || collision) && now >= mNextAlarmSound)
    {
        mNextAlarmSound = now + 1.5;
        make_ui_sound("UISndAlert");
    }
    // <WolfViewer 2026-10-07> Paul: "boats should automatically turn left until they find a clear
    // route when on auto pilot". While the way ahead is blocked the boat steers 50 degrees to port
    // of where its bow points NOW, re-aimed on every check, so it keeps turning left until the
    // rays and the shoal look-ahead find the way clear. It then holds that clear heading for 4 s
    // before going back to its route, which is re-planned from there. (Was: 50 degrees off the
    // route's bearing to the clearer side for 6 s, then straight back at the obstacle.)
    // Paul: "boats should only do that once though if there's no way clear it should turn off auto
    // pilot and tell the user ... there's no point spinning forever". One full turn: when the boat
    // has turned 360 degrees to port in one blocked spell and the way is still not clear, the
    // autopilot lets go and says why.
    const bool blocked = mAP && (shoal || collision);
    if (blocked)
    {
        if (!mAvoidBlocked)
        {
            mRouteDirty = true;   // once per blocked spell, not every frame
            mAvoidTurned = 0.f;
            mAvoidLastHeading = mData.mHeading;
        }
        // to port is a falling heading: count what it fell by since the last check
        mAvoidTurned += llmax(0.f, wrap180(mAvoidLastHeading - mData.mHeading));
        mAvoidLastHeading = mData.mHeading;
        if (mAvoidTurned >= 360.f)
        {
            mAvoidBlocked = false;
            mAvoidUntil = 0.0;
            disconnectAP("NO CLEAR WAY", false);
            LLNotificationsUtil::add("GenericAlert", LLSD().with("MESSAGE",
                std::string("Autopilot off: the boat turned a full circle and found no clear way ahead"
                            " (shallow water or objects all round). Steer it clear by hand, then switch the autopilot back on.")));
            return;
        }
        mAvoidHeading = wrap360(mData.mHeading - 50.f);
        mAvoidUntil = now + 4.0;
    }
    else if (mAvoidBlocked && now < mAvoidUntil)
    {
        mAvoidHeading = mData.mHeading;   // clear: hold the heading that found the way
        mRouteDirty = true;
    }
    mAvoidBlocked = blocked;
}

void WolfFlight::sailGuidance(F32 dt)
{
    const F64 now = nowSeconds();
    F32 want = mData.mHeading;
    mFDValid = false;
    switch (mLateral)
    {
    case LAT_HDG:
        want = mSelHeading;
        break;
    case LAT_WIND:
        want = wrap360(mData.mTWDSteady - mSelTWA);
        break;
    case LAT_LNAV:
    {
        if (!mDest.mValid)
        {
            break;
        }
        const LLVector3d wp = activeWaypoint();
        const F64 dx = wp.mdV[VX] - mData.mPosGlobal.mdV[VX], dy = wp.mdV[VY] - mData.mPosGlobal.mdV[VY];
        const F32 brg = wrap360((F32)(atan2(dx, dy) * RAD_TO_DEG));
        want = brg;
        const bool last_leg = !mRoute.mValid || mRoute.mLeg >= (S32)mRoute.mPts.size() - 1;
        // arrived: at the destination, or at the end of a route that stops at its shore
        F32 to_end = destDistance();
        if (mRoute.mValid && mRoute.mShoreEnd && !mRoute.mPts.empty())
        {
            const LLVector3d& e = mRoute.mPts.back();
            to_end = (F32)sqrt((e.mdV[VX] - mData.mPosGlobal.mdV[VX]) * (e.mdV[VX] - mData.mPosGlobal.mdV[VX])
                             + (e.mdV[VY] - mData.mPosGlobal.mdV[VY]) * (e.mdV[VY] - mData.mPosGlobal.mdV[VY]));
        }
        if (last_leg && to_end < 15.f && mAP)
        {
            mAP = false;
            mOutPitch = mOutBank = mOutThrottle = 0.f;
            postCas("DEST REACHED", CAS_MEMO);
            break;
        }
        // Upwind or dead downwind of the mark: sail the laylines, tacking (gybing) on them.
        if (mData.mTWSSteady > 0.5f)
        {
            const F32 A = tackAngle();
            const F32 rel = wrap180(brg - mData.mTWDSteady);     // 0 = the mark is dead upwind
            const bool upwind = fabsf(rel) < A;
            const bool downwind = fabsf(rel) > 180.f - 30.f;
            if (upwind || downwind)
            {
                const F32 off = upwind ? A : 180.f - 30.f;
                const F32 h_stbd = wrap360(mData.mTWDSteady - off);    // wind on the starboard side
                const F32 h_port = wrap360(mData.mTWDSteady + off);
                if (mTack == 0)
                {
                    mTack = fabsf(wrap180(brg - h_stbd)) <= fabsf(wrap180(brg - h_port)) ? 1 : -1;
                }
                const F32 other = mTack > 0 ? h_port : h_stbd;
                // on the layline: the other tack now points at the mark
                if (fabsf(wrap180(brg - other)) < 4.f && now - mLastTack > 15.0)
                {
                    mTack = -mTack;
                    mLastTack = now;
                    postCas(upwind ? "TACKING" : "GYBING", CAS_MEMO);
                }
                want = mTack > 0 ? h_stbd : h_port;
            }
            else
            {
                mTack = 0;
            }
        }
        break;
    }
    default:
        break;
    }
    if (mAP && now < mAvoidUntil)
    {
        want = mAvoidHeading;   // <WolfViewer 2026-10-07/> sailAlarms: turning left round an obstacle
    }
    mWantTrack = want;
    mWantSpeed = mSelSpeedKt / KT;
    if (now - mLastTack > 8.0)
    {
        clearCas("TACKING");
        clearCas("GYBING");
    }
}

void WolfFlight::sailDrive(F32 dt)
{
    // The helm: toward the wanted course, damped by the rate of turn (a hull answers slowly).
    // <WolfViewer 2026-10-06> Paul: "autopilot mode on sailing is just going round in circles".
    // Steered by the COURSE OVER THE GROUND once the boat moves: a hull goes where its bow points
    // (give or take leeway), whereas the heading is read from the pilot's seat, which on a
    // sailboat may face any way. Standing still, only the heading is there to go on.
    const bool bank_inv = WolfFlight::invert(WolfFlight::keySetting("BankInvert"));
    const F64 now = nowSeconds();
    const F32 course = (mData.mGS > 0.7f) ? mData.mTrack : mData.mHeading;
    const F32 step = wrap180(course - mLastCourse);
    mLastCourse = course;
    mCourseRate = ema(mCourseRate, step / llmax(dt, 0.001f), dt, 0.5f);
    const F32 err = wrap180(mWantTrack - course);
    // Paul: "the rudder is wiggling left and right". A slow hull answers late, and the old gains
    // overshot every few seconds. Within 4 degrees the helm is left alone; outside, gentler and
    // more damped.
    const F32 steer = fabsf(err) < 4.f ? 0.f : err - (err > 0.f ? 4.f : -4.f);
    mOutBank = ema(mOutBank, llclamp(steer / 35.f - mCourseRate / 8.f, -1.f, 1.f), dt, 0.8f);   // + = starboard
    holdPair(bankPair(), bank_inv ? mOutBank : -mOutBank, 0.0f);
    mOutPitch = 0.f;
    autothrottle(dt);   // the motor, when A/T is on

    // Going round in circles: more than one and a half turns in a minute means the helm is not
    // doing what the autopilot thinks. Stop, rather than circle for ever, and say where to look.
    if (now - mCircleStart > 60.0)
    {
        mCircleStart = now;
        mCircleTurn = 0.f;
    }
    mCircleTurn += step;
    if (fabsf(mCircleTurn) > 540.f)
    {
        mCircleTurn = 0.f;
        disconnectAP("AP: CIRCLING - CHECK CTL", false);
        LL_WARNS("WolfFlight") << "sailing AP circling: want " << mWantTrack << " course " << course << " heading " << mData.mHeading
                               << " helm " << mOutBank << " seat offset " << mHeadingOffset << LL_ENDL;
        return;
    }
    if (now >= mNextSailLog)
    {
        mNextSailLog = now + 3.0;
        LL_INFOS("WolfFlight") << "sail AP: mode " << (S32)mLateral << " want " << (S32)mWantTrack << " course " << (S32)course
                               << " heading " << (S32)mData.mHeading << " err " << (S32)err << " rate " << mCourseRate
                               << " helm " << mOutBank << " gs " << mData.mGS << " twa " << (S32)mData.mTWA
                               << " wind " << (S32)mData.mTWDSteady << "/" << mData.mTWSSteady << " depth " << mData.mDepth
                               << " clear " << (S32)llmin(mBoatClearL, 9999.f) << "/" << (S32)llmin(mBoatObstacle * llmax(mData.mGS, 0.5f), 9999.f)
                               << "/" << (S32)llmin(mBoatClearR, 9999.f)
                               << " tack " << mTack << " leg " << mRoute.mLeg << "/" << ((S32)mRoute.mPts.size() - 1)
                               << " seat " << mHeadingOffset << LL_ENDL;
    }
}

void WolfFlight::sendSailCommand()
{
    const std::string cmd = gSavedSettings.getString("WolfSailCommand");
    if (cmd.empty())
    {
        postCas("SAIL CMD NOT SET", CAS_ADVISORY);
        return;
    }
    FSNearbyChat::sendChatFromViewerFinal(cmd, cmd, CHAT_TYPE_NORMAL, false, gSavedSettings.getS32("WolfSailCommandChannel"));
}
