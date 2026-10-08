/**
 * @file wolfaltitudesky.cpp
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

#include "llviewerprecompiledheaders.h"

#include "wolfaltitudesky.h"

#include "llagent.h"
#include "llenvironment.h"
#include "llvoavatarself.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "wolfgrid.h"
#include "wolfweather.h"

namespace
{
    // The standard atmosphere's density scale height, metres.
    constexpr F32 AIR_SCALE_HEIGHT_M = 8500.f;
    // Most cloud is below the tropopause (about 12 km at mid latitudes).
    constexpr F32 CLOUDS_THIN_FROM_M = 8000.f;
    constexpr F32 CLOUDS_GONE_AT_M   = 14000.f;
    // Stars by day: from a tenth of the air left (~19.5 km) to a fiftieth (~33 km).
    constexpr F32 STARS_FROM_AIR = 0.10f;
    constexpr F32 STARS_FULL_AIR = 0.02f;
    // The far clip grows with height up to this: from 30 km the horizon is ~600 km away, far past
    // any ground there is to draw, and depth precision suffers past it.
    constexpr F32 FAR_EXTRA_MAX_M = 30000.f;

    bool enabled()
    {
        static LLCachedControl<bool> on(gSavedSettings, "WolfViewerAltitudeSky", true);
        return on && WolfGrid::isWolfTerritories();
    }

    // Camera height above the water, which is sea level on the grid.
    F32 altitude()
    {
        const F32 z = LLViewerCamera::getInstance()->getOrigin().mV[VZ];
        return llmax(0.f, z - LLEnvironment::instance().getWaterHeight());
    }

    F32 smoothstep(F32 edge0, F32 edge1, F32 x)
    {
        const F32 t = llclamp((x - edge0) / (edge1 - edge0), 0.f, 1.f);
        return t * t * (3.f - 2.f * t);
    }
}

F32 WolfAltitudeSky::airFraction()
{
    if (!enabled())
    {
        return 1.f;
    }
    return expf(-altitude() / AIR_SCALE_HEIGHT_M);
}

F32 WolfAltitudeSky::cloudFraction()
{
    if (!enabled())
    {
        return 1.f;
    }
    return 1.f - smoothstep(CLOUDS_THIN_FROM_M, CLOUDS_GONE_AT_M, altitude());
}

F32 WolfAltitudeSky::spaceStarAlpha()
{
    if (!enabled())
    {
        return 0.f;
    }
    return 1.f - smoothstep(STARS_FULL_AIR, STARS_FROM_AIR, airFraction());
}

F32 WolfAltitudeSky::farClipExtra()
{
    static LLCachedControl<bool> on(gSavedSettings, "WolfViewerFarClipFollowsHeight", true);
    if (!on || !WolfGrid::isWolfTerritories())
    {
        return 0.f;
    }
    return llmin(altitude(), FAR_EXTRA_MAX_M);
}

F32 WolfAltitudeSky::horizonSeaHeight()
{
    static LLCachedControl<bool> on(gSavedSettings, "WolfViewerHorizonSea", true);
    if (!on || !WolfGrid::isWolfTerritories())
    {
        return -1.f;
    }
    const F32 h = LLViewerCamera::getInstance()->getOrigin().mV[VZ] - LLEnvironment::instance().getWaterHeight();
    return h > 0.f ? h : -1.f;
}

bool WolfAltitudeSky::cloudDeck(CloudDeck& out)
{
    static LLCachedControl<bool> on(gSavedSettings, "WolfViewerFlyClouds", true);
    if (!on || !WolfGrid::isWolfTerritories() || !WolfWeather::instanceExists() || !WolfWeather::enabled())
    {
        return false;
    }
    // Only while flying (Paul: "cloud only appears if flying, someone might have a skybox").
    if (!agentFlying())
    {
        return false;
    }
    LLSettingsSky::ptr_t psky = LLEnvironment::instance().getCurrentSky();
    if (!psky)
    {
        return false;
    }
    const WolfWeatherProfile& p = WolfWeather::instance().activeProfile();
    F32 cover = 0.f;
    if (p.mKind == WolfWeatherProfile::RAIN || p.mKind == WolfWeatherProfile::SNOW)
    {
        cover = 0.5f + 0.1f * (F32)llclamp(p.mLevel, 1, 4);   // light 0.6 .. storm 0.9
    }
    cover = llmax(cover, llclamp((F32)p.mFog / 100.f, 0.f, 1.f) * 0.9f);
    // the sky's own cover: from about half (broken cloud) upward
    cover = llmax(cover, llclamp(((F32)psky->getCloudShadow() - 0.45f) / 0.5f, 0.f, 1.f) * 0.85f);
    if (cover < 0.3f)
    {
        return false;
    }
    const F32 water = LLEnvironment::instance().getWaterHeight();
    out.mCover = cover;
    out.mBase = water + lerp(900.f, 350.f, cover);
    out.mTop = out.mBase + lerp(900.f, 2600.f, cover);
    out.mDensity = lerp(0.004f, 0.02f, cover);
    out.mFade = smoothstep(150.f, 400.f, altitude());
    return out.mFade > 0.f;
}

bool WolfAltitudeSky::agentFlying()
{
    if (!isAgentAvatarValid())
    {
        return false;
    }
    if (gAgent.getFlying())
    {
        return true;
    }
    if (gAgentAvatarp->isSitting())
    {
        LLViewerObject* seat = (LLViewerObject*)gAgentAvatarp->getParent();
        while (seat && seat->getParent())
        {
            seat = (LLViewerObject*)seat->getParent();
        }
        return seat && seat->getVelocity().length() > 5.f;
    }
    return false;
}

F32 WolfAltitudeSky::cameraAltitude()
{
    return altitude();
}
