/**
 * @file wolfaltitudesky.h
 * @brief WolfViewer: the sky thins with height, at real-world heights, until it is space.
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

#ifndef WOLF_ALTITUDE_SKY_H
#define WOLF_ALTITUDE_SKY_H

#include "stdtypes.h"

// Paul 2026-10-07, flying the Concorde: "too high = in space", "realistic heights".
//
// The air thins exponentially with height (scale height 8.5 km): a third of it is left at 10 km,
// a tenth at 20 km, a hundredth at 40 km. WolfSim lets objects go to 50,000 m
// (Constants.MaxSimulationHeight), so that is the band this covers. Everything is read from the
// CAMERA's height above the water (sea level); the region's own EEP sky is never changed, only
// what the sky shaders are given:
//  - airFraction() scales density_multiplier for the SKY shader group (llsettingsvo.cpp
//    applySpecial). skyV.glsl/cloudsV.glsl fade the sky colour by exp(-density * ray length), so
//    thinner air is a darker sky overhead with the blue band left at the horizon, and a whiter sun.
//  - cloudFraction() fades the cloud layer out between 8 km and 14 km: from above, the weather is
//    below you.
//  - spaceStarAlpha() lets the stars out by day from about 18 km, fully from about 33 km
//    (lldrawpoolwlsky.cpp).
// Wolf Territories only, and WolfViewerAltitudeSky turns it off. At ground level nothing changes:
// airFraction() is 0.96 at 350 m.
namespace WolfAltitudeSky
{
    // 1 at sea level, falling to about 0.003 at 50 km; 1 when switched off or on another grid.
    F32 airFraction();

    // 1 below 8 km, 0 above 14 km.
    F32 cloudFraction();

    // 0 low down, 1 in space: the star alpha (0..1, as star_brightness / 500) for the daytime sky.
    F32 spaceStarAlpha();

    // Metres to add to the camera's far clip: its height above the water, so the ground within
    // the draw distance stays in view from the air (Paul, 10-07: from 3.8 km up a 4096 m far clip
    // reached only 1.6 km out and the sea beyond it was bare sky). At most FAR_EXTRA_MAX_M; 0 when
    // switched off (WolfViewerFarClipFollowsHeight) or on another grid.
    F32 farClipExtra();

    // The camera's height above the sea for the sky's ocean (skyF.glsl wolfHorizonSea, waterF.glsl's
    // fade into it), or -1 when it is off: another grid, WolfViewerHorizonSea off, or the camera
    // under the water. Paul, 10-07: "do that goal real horizon make it look perfect".
    F32 horizonSeaHeight();

    // A cloud layer to fly through in bad weather (Paul, 10-07: "if the weather is bad can we put more
    // cloud to fly through? when up high"), drawn by LLPipeline::wolfCloudDeck / wolfCloudDeckF.glsl.
    // Its cover comes from the weather that is on (rain or snow and its level, fog) and the sky's own
    // cloud cover (EEP cloud_shadow); under 0.3 there is none. Low, thick and dense the worse it is:
    // the base 900 m above the sea in light weather down to 350 m in a storm, 0.9 to 2.6 km deep. It
    // fades in as the camera climbs from 150 to 400 m, so on the ground the sky is the region's own, and
    // it is only there while flying (or riding something that is), never for a skybox.
    // WolfViewerFlyClouds turns it off; Wolf Territories only. Heights are agent (region) metres.
    struct CloudDeck
    {
        F32 mCover = 0.f;       // 0..1
        F32 mBase = 0.f, mTop = 0.f;
        F32 mDensity = 0.f;     // extinction per metre in the thick of it
        F32 mFade = 0.f;        // 0..1
    };
    bool cloudDeck(CloudDeck& out);
}

#endif // WOLF_ALTITUDE_SKY_H
