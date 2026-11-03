/**
 * @file wolfnaturesounds.h
 * @brief [NATURE SOUNDS 2026-11-03] Ticket 475 (Jimmy Olsen): the sounds of the place round the listener - crickets and
 *        bush crickets on warm nights, soft lapping by lakes and ponds, and waves at the shore that follow the real waves.
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

#ifndef WOLF_NATURE_SOUNDS_H
#define WOLF_NATURE_SOUNDS_H

#include <map>
#include <string>
#include <vector>

#include "llsingleton.h"
#include "lluuid.h"
#include "v3math.h"

class LLViewerRegion;
class LLVOWater;

/**
 * Played by this viewer alone, from what it already knows: no objects, scripts or requests on the region. The grid's part
 * is the owner's switch and the insects' climate, in the weather payload every viewer reads each minute
 * (WolfRegionWeather::soundsAllowed / soundsInsects; wolfstorm/php/wolf_ambient_lib.php wa_sounds_payload).
 *
 *  WHETHER  Wolf regions (WolfGrid::showsWolfRegionWeather), the owner's "Nature sounds on this region" (off unless chosen),
 *           and the resident's own WolfViewerNatureSounds (World > Wolf World > Nature Sounds, on by default).
 *  INSECTS  the grid's climate (latitude, season, the place's temperature) x the night (the sun of the sky on screen) x the
 *           weather on screen (rain, snow) x the region's wind; none high up in a skybox.
 *  WATER    every second, 16 bearings out to 90 m from the camera find the nearest shore (terrain below / above its
 *           region's water height). The drawn waves just off it (WolfBoatRock::drawnWaveHeight, the surface the water
 *           shader draws) are sampled ten times a second; their significant height Hs = 4 x RMS chooses the sound: lapping
 *           when the water is still, then small, medium and big surf as the waves grow. A builder's water (a wolfwater prim,
 *           a painted pond or river) is still water: lapping. Each fades with the distance to it.
 *  LEVEL    every loop is a 2D stereo recording (as the rain, wolfweathersound.cpp) played as AMBIENT, so the Ambient
 *           volume governs it; gentle by design (the gains are in wolfnaturesounds.cpp). Under the water all of it is
 *           muted (the bubbles have the water); under a roof it is quieter.
 *  SOUNDS   app_settings/wolfnature/, CC0 recordings: SOURCES.md there. The same files, numbers and rules as WolfStorm's
 *           js/world/wolf_nature_sounds.js; region-terrain-work/nature-sounds-2026-11-03/README.md.
 */
class WolfNatureSounds : public LLSingleton<WolfNatureSounds>
{
    LLSINGLETON(WolfNatureSounds);
    ~WolfNatureSounds();

public:
    /** Every frame from LLAppViewer::idle(). */
    void idle();

private:
    /** One playing loop, eased towards its target level (wolfweathersound.cpp Layer, without the drift). */
    struct Layer
    {
        LLUUID mSource;
        F32    mGain = 0.f;
        F32    mTarget = 0.f;
        bool   mFast = false;      ///< fade quickly (under the water, the resident's own switch)
        F64    mRetryAt = 0.0;
    };

    void assess(F64 now);
    void scanShore(LLViewerRegion* region, const LLVector3& cam);
    void sampleWaves(F64 now);
    F32  stillWaterDistance(const LLVector3& cam, bool& inside) const;
    void updateLayer(S32 i, F32 dt, F64 now);
    void stopLayer(Layer& l);
    void stopAll();

    std::vector<Layer> mLayers;
    std::vector<LLVOWater*> mPlanes;   ///< this assessment's builders' water planes (wolfwater prims, painted water)
    F64  mLastIdle = 0.0;
    F64  mNextAssess = 0.0;
    F64  mNextScan = 0.0;
    F64  mNextWave = 0.0;
    F64  mNextLog = 0.0;
    bool mActive = false;              ///< anything is (or was) playing: stopAll() owes the sources
    // the shore
    bool      mHaveShore = false;
    F32       mShoreDist = 0.f;        ///< 3D, camera to the shore point at the water's height
    LLVector3 mSeaPoint;               ///< agent metres: just off the shore, where the waves are sampled
    LLUUID    mShoreRegion;
    // the drawn waves there: running mean and mean square (exponential, WAVE_TAU_S)
    F32  mWaveMean = 0.f;
    F32  mWaveMean2 = 0.f;
    bool mWaveSeeded = false;
    F32  mHs = 0.f;
    S32  mWaveSamples = 0;             ///< samples since the statistics were (re)started
    F32  mWind = 0.f;                  ///< the region's wind at the camera, m/s (the log line)
    LLVector3d mLastOrigin;
};

#endif // WOLF_NATURE_SOUNDS_H
