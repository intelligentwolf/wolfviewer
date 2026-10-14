/**
 * @file wolfcamerafx.cpp
 * @brief WolfViewer: the Flight Mode chase camera and camera shake. See wolfcamerafx.h.
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

#include "wolfcamerafx.h"

#include <cmath>

#include "llagent.h"
#include "llagentcamera.h"
#include "llframetimer.h"
#include "llrand.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llvoavatarself.h"
#include "wolfflight.h"

namespace
{
    const F32 TWO_PI_F = 6.28318531f;
    const F32 BLEND_SECONDS = 0.5f;       // chase camera in / out
    const F32 ROT_HALF_LIFE = 0.12f;      // the aircraft's rotation, smoothed (seconds to half way)
    const F32 MAX_SHAKE = 3.f;            // metres: a shake never throws the camera further
    const F32 MAX_ROLL_SECONDS = 30.f;    // WolfFXModule.Roll holds seconds to 0..30
    const F32 LEAVE_ROLL_SECONDS = 0.5f;  // levelling when the avatar leaves the roll's region

    LLUUID agent_region_id()
    {
        LLViewerRegion* region = gAgent.getRegion();
        return region ? region->getRegionID() : LLUUID::null;
    }

    /** The aircraft the avatar sits on (the linkset's root), or null. */
    LLViewerObject* seated_vehicle()
    {
        if (!isAgentAvatarValid() || !gAgentAvatarp->isSitting()) return nullptr;
        LLViewerObject* seat = dynamic_cast<LLViewerObject*>(gAgentAvatarp->getParent());
        if (!seat || seat->isDead()) return nullptr;
        LLViewerObject* root = seat->getRootEdit();
        return (root && !root->isDead() && root->mDrawable.notNull()) ? root : nullptr;
    }
}

WolfCameraFX::WolfCameraFX()
{
}

void WolfCameraFX::apply(LLVector3& position, LLVector3& focus, LLVector3& up, U32 camera_mode, bool focus_on_avatar)
{
    const F64 t = LLFrameTimer::getElapsedSeconds();
    const F32 dt = mLastTime > 0.0 ? llclamp((F32)(t - mLastTime), 0.f, 0.25f) : 0.f;
    mLastTime = t;
    chase(position, focus, up, camera_mode, focus_on_avatar, dt);
    applyShake(position, focus, dt);
    applyRoll(position, focus, up, dt);
}

void WolfCameraFX::chase(LLVector3& position, LLVector3& focus, LLVector3& up, U32 camera_mode, bool focus_on_avatar, F32 dt)
{
    static LLCachedControl<bool> follow(gSavedSettings, "WolfFlightFollowCam", true);
    WolfFlight& flight = WolfFlight::instance();
    LLViewerObject* vehicle = seated_vehicle();
    const bool want = follow && flight.active() && !flight.sailing() && vehicle
                      && (camera_mode == CAMERA_MODE_THIRD_PERSON || camera_mode == CAMERA_MODE_FOLLOW) && focus_on_avatar;

    mBlend = llclamp(mBlend + (want ? dt : -dt) / BLEND_SECONDS, 0.f, 1.f);
    if (!vehicle)
    {
        mBlend = 0.f;
        mHaveRot = false;
        return;
    }
    if (mBlend <= 0.f)
    {
        mHaveRot = false;
        return;
    }

    // The aircraft's rotation, smoothed so the small corrections of its flight do not shake the view.
    const LLQuaternion rot = vehicle->getRenderRotation();
    if (!mHaveRot)
    {
        mSmoothRot = rot;
        mHaveRot = true;
    }
    else
    {
        const F32 k = dt > 0.f ? 1.f - powf(0.5f, dt / ROT_HALF_LIFE) : 0.f;
        mSmoothRot = nlerp(k, mSmoothRot, rot);
        mSmoothRot.normalize();
    }
    const LLVector3 fwd = LLVector3::x_axis * mSmoothRot;
    const LLVector3 vup = LLVector3::z_axis * mSmoothRot;
    const LLVector3 pos = vehicle->getRenderPosition();

    // The distance the driver zoomed to (the normal camera's), kept to something sensible behind a plane.
    const F32 dist = llclamp((position - focus).length(), 5.f, 120.f);
    const LLVector3 chase_pos = pos - fwd * dist + vup * (dist * 0.22f);
    const LLVector3 chase_focus = pos + fwd * (dist * 0.35f) + vup * (dist * 0.06f);

    // Blend in / out from the normal camera, smoothed at both ends.
    const F32 b = mBlend * mBlend * (3.f - 2.f * mBlend);
    position = lerp(position, chase_pos, b);
    focus = lerp(focus, chase_focus, b);
    LLVector3 u = lerp(up, vup, b);
    if (u.normalize() < 0.001f) u = vup;
    up = u;
}

void WolfCameraFX::shake(F32 amplitude, F32 seconds, F32 frequency)
{
    if (!(amplitude > 0.f) || !(seconds > 0.f)) return;
    Shake s;
    s.mAmp = llmin(amplitude, MAX_SHAKE);
    s.mSeconds = llclamp(seconds, 0.05f, 10.f);
    s.mFreq = frequency > 0.f ? llclamp(frequency, 1.f, 60.f) : 18.f;
    s.mAge = 0.f;
    for (F32& p : s.mPhase) p = ll_frand(TWO_PI_F);
    if (mShakes.size() >= 16) mShakes.erase(mShakes.begin());   // at most 16 at once: the oldest goes
    mShakes.push_back(s);
}

// Each shake: three axes, each the sum of two sines at unrelated rates with random phases (noise
// that never repeats visibly), its strength falling with the square of the time left (a sharp
// jolt that settles), as game camera shakes do.
void WolfCameraFX::applyShake(LLVector3& position, LLVector3& focus, F32 dt)
{
    if (mShakes.empty()) return;
    LLVector3 offset;
    for (auto it = mShakes.begin(); it != mShakes.end();)
    {
        it->mAge += dt;
        const F32 left = 1.f - it->mAge / it->mSeconds;
        if (left <= 0.f)
        {
            it = mShakes.erase(it);
            continue;
        }
        const F32 a = it->mAmp * left * left;
        const F32 w = TWO_PI_F * it->mFreq * it->mAge;
        offset.mV[VX] += a * (0.7f * sinf(w + it->mPhase[0]) + 0.3f * sinf(w * 2.31f + it->mPhase[1]));
        offset.mV[VY] += a * (0.7f * sinf(w * 1.13f + it->mPhase[2]) + 0.3f * sinf(w * 2.79f + it->mPhase[3]));
        offset.mV[VZ] += a * (0.7f * sinf(w * 0.91f + it->mPhase[4]) + 0.3f * sinf(w * 2.07f + it->mPhase[5]));
        ++it;
    }
    if (offset.length() > MAX_SHAKE) offset *= MAX_SHAKE / offset.length();
    position += offset;
    focus += offset * 0.6f;     // the view also tilts a little, not only slides
}

void WolfCameraFX::roll(F32 degrees, F32 seconds)
{
    if (!std::isfinite(degrees) || !std::isfinite(seconds)) return;
    mRollFrom = mRollNow;           // from wherever it is now, mid-roll included
    mRollStartVel = mRollVel;       // ... and at the speed it is turning
    mRollTo = llclamp(degrees, -180.f, 180.f);
    mRollSeconds = llclamp(seconds, 0.f, MAX_ROLL_SECONDS);
    mRollAge = 0.f;
    mRollRegion = agent_region_id();
    mRollSeated = isAgentAvatarValid() && gAgentAvatarp->isSitting();
}

// Eased from the angle it had to the new one, arriving at rest: a cubic Hermite curve that starts at the
// turn's current speed and ends at zero speed (with no speed to carry it is the smoothstep, 3t^2 - 2t^3).
// The up vector is turned about the line of sight. A positive angle turns "up" towards the camera's right:
// the view rolls right, as a plane banking right (LLQuaternion(angle, axis) turns by the right-hand rule,
// llquaternion.cpp setAngleAxis; LLVector3 * LLQuaternion, llquaternion.cpp:571).
void WolfCameraFX::applyRoll(const LLVector3& position, const LLVector3& focus, LLVector3& up, F32 dt)
{
    static LLCachedControl<bool> shake_on(gSavedSettings, "WolfFXCameraShake", true);
    if (!shake_on)
    {
        mRollFrom = mRollTo = mRollNow = 0.f;
        mRollVel = mRollStartVel = 0.f;
        return;
    }
    if (mRollTo != 0.f)
    {
        // level out: left the roll's region, teleporting (inside the region too), or stood up from the seat
        const bool stood = mRollSeated && !(isAgentAvatarValid() && gAgentAvatarp->isSitting());
        if (mRollRegion != agent_region_id() || gAgent.getTeleportState() != LLAgent::TELEPORT_NONE || stood)
        {
            roll(0.f, LEAVE_ROLL_SECONDS);
        }
    }
    if (mRollTo == 0.f && mRollNow == 0.f)
    {
        mRollVel = 0.f;
        return;
    }

    mRollAge += dt;
    const F32 T = mRollSeconds;
    const F32 t = T > 0.f ? llclamp(mRollAge / T, 0.f, 1.f) : 1.f;
    if (t >= 1.f)
    {
        mRollNow = mRollTo;
        mRollVel = 0.f;
    }
    else
    {
        // p(t) = p0 + (p1 - p0)(3t^2 - 2t^3) + v0 T (t^3 - 2t^2 + t); p'(t) / T for degrees a second
        const F32 d = mRollTo - mRollFrom, v0 = mRollStartVel;
        mRollNow = mRollFrom + d * t * t * (3.f - 2.f * t) + v0 * T * t * (1.f - t) * (1.f - t);
        mRollVel = d * 6.f * t * (1.f - t) / T + v0 * (1.f - t) * (1.f - 3.f * t);
    }
    if (mRollNow == 0.f) return;

    LLVector3 look = focus - position;
    if (look.normalize() < 0.001f) return;
    LLVector3 u = up * LLQuaternion(mRollNow * DEG_TO_RAD, look);
    if (u.normalize() < 0.001f) return;
    up = u;
}
