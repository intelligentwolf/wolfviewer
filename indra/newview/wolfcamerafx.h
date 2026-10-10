/**
 * @file wolfcamerafx.h
 * @brief WolfViewer: what the camera does on top of the normal camera - the Flight Mode chase
 *        camera that rolls with the aircraft, and camera shake (explosions, wolfShakeCamera).
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

#ifndef WOLF_CAMERAFX_H
#define WOLF_CAMERAFX_H

#include "llquaternion.h"
#include "llsingleton.h"
#include "v3math.h"

#include <vector>

// [WOLF CAMERA 2026-10-10] Called by LLAgentCamera::updateCamera just before the camera is placed
// (LLViewerCamera::updateCameraLocation), with the position, focus and up it was about to use.
//
// CHASE (Paul: "add to the plane hud a thing where i can turn it on and the viewer follows the
// plane, so if i fly upside down the viewer renders the same way", "plane follow camera SHOULD BE
// DEFAULT"): with Flight Mode on (not Sailing), seated, in the third-person camera with the focus
// on the avatar (not orbiting with Alt, not mouselook - mouselook already turns with the seat), the
// camera sits behind the aircraft IN THE AIRCRAFT'S OWN FRAME at the distance the driver zoomed to,
// and its "up" is the aircraft's up: bank, loop or fly inverted and the world turns with you, as in
// a flight game's chase view. The aircraft's rotation is smoothed (not its position, so nothing lags
// at speed); turning it on or off blends over half a second. Setting WolfFlightFollowCam (on by
// default; the CAM button on the flight deck).
//
// SHAKE (Paul: "add things like being able to shake the users viewer"): a decaying, smooth random
// jolt of the camera, as games shake theirs for blasts and impacts (amplitude falling with time,
// a few octaves of noise so it never looks like a sine). Shakes add up; the strongest wins out as
// they decay. Started by explosions (wolfexplosion.cpp) and by the region (wolfShakeCamera).
class WolfCameraFX : public LLSingleton<WolfCameraFX>
{
    LLSINGLETON(WolfCameraFX);

public:
    void apply(LLVector3& position, LLVector3& focus, LLVector3& up, U32 camera_mode, bool focus_on_avatar);

    // metres of jolt at the start, seconds to die away, jolts per second (0 = the default 18)
    void shake(F32 amplitude, F32 seconds, F32 frequency = 0.f);

    bool chaseActive() const { return mBlend > 0.f; }

private:
    void chase(LLVector3& position, LLVector3& focus, LLVector3& up, U32 camera_mode, bool focus_on_avatar, F32 dt);
    void applyShake(LLVector3& position, LLVector3& focus, F32 dt);

    // chase
    F32 mBlend = 0.f;               // 0 normal camera .. 1 chase
    LLQuaternion mSmoothRot;
    bool mHaveRot = false;

    // shake
    struct Shake
    {
        F32 mAmp, mSeconds, mFreq, mAge;
        F32 mPhase[6];
    };
    std::vector<Shake> mShakes;

    F64 mLastTime = 0.0;
};

#endif // WOLF_CAMERAFX_H
