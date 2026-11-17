/**
 * @file wolfplanesound.h
 * @brief [PLANE SOUND 2026-11-15] Ticket 865 (Jimmy Olsen, a Flea Report idea from TROMSO): "is it possible to add the
 *        engine sound from them (a light subtle one as heard from far away)?" - a faint, distant engine for each passing
 *        plane of the wolf-air plugin.
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

#ifndef WOLF_PLANE_SOUND_H
#define WOLF_PLANE_SOUND_H

#include <map>
#include <string>

#include "llsingleton.h"
#include "lluuid.h"
#include "v3math.h"

/**
 * Played by this viewer alone; the plugin is not asked for anything.
 *
 *  WHICH    the moving roots (LLViewerObjectList::getActiveObjects) described "wolfair:traffic <class>" or
 *           "wolfair:traffic-field <class>": the passing planes and the airfield traffic of the wolf-air plugin
 *           (region-terrain-work/aircraft-dogfight-2026-11-01/c-wolf-air/src/AirTraffic.cs MARK and Launch,
 *           AirTraffic.Airfield.cs FIELD_MARK). The descriptions come from WolfObjectProps, a few asks a second.
 *  WHETHER  Wolf regions (WolfGrid::showsWolfRegionWeather, visitors included) and the resident's own "Passing planes" in
 *           Wolf World > Wolf Sound Effects (WolfViewerPlaneSound, WolfSoundPlanesVolume; on by default).
 *  SOUND    two loops made here, never a recording: a propeller's drone (the classic planes) and a jet's roar (the
 *           airliners, the science-fiction craft) - filtered noise, the propeller's with a soft blade beat. Written into the
 *           sound cache under WolfViewer's own UUIDs, as the weather's synthesised wind (wolfweathersound.cpp writeWav).
 *  LEVEL    AMBIENT, so the viewer's Ambient and master volumes and its mute govern it. Gentle by design: the gain falls as
 *           1 / distance beyond 150 m (spherical spreading), silent beyond 3 km; quieter under a roof, none under the water.
 *  MOVING   each loop is a source of its own, placed a few metres from the camera along the true direction to the plane
 *           (as the birds' and the dolphins' calls) and given the plane's velocity, so the audio engine's own Doppler
 *           (llvieweraudio.cpp setDopplerFactor) lowers the note as she goes past. Fades in and out.
 *  SAME AS  wolfstorm/js/world/wolf_plane_sound.js - the same loops, numbers and rules.
 */
class WolfPlaneSound : public LLSingleton<WolfPlaneSound>
{
    LLSINGLETON(WolfPlaneSound);
    ~WolfPlaneSound();

public:
    /** Every frame from LLAppViewer::idle(). */
    void idle();

private:
    struct Plane
    {
        S32       mVoice = 0;            ///< 0 propeller, 1 jet
        F32       mMult = 1.f;           ///< the class's own loudness
        std::string mClass;
        LLVector3 mPos;                  ///< agent metres, as last seen
        LLVector3 mVel;                  ///< m/s, from the positions seen (smoothed)
        bool      mHaveVel = false;
        F64       mSeenAt = 0.0;         ///< the last scan that found her moving
        F64       mPosAt = 0.0;
        F32       mGain = 0.f;
        F32       mTarget = 0.f;
        LLUUID    mSource;               ///< the playing audio source, or null
        F64       mRetryAt = 0.0;
        bool      mLogged = false;
    };

    void scan(F64 now, const LLVector3& cam);
    void update(const LLUUID& id, Plane& p, F32 dt, F64 now, const LLVector3& cam, F32 level);
    void stopSource(Plane& p);
    bool soundReady(S32 voice);

    std::map<LLUUID, Plane> mPlanes;
    F64  mLastIdle = 0.0;
    F64  mNextScan = 0.0;
    S32  mReady[2] = { 0, 0 };           ///< per voice: 0 not checked, 1 in the sound cache, -1 could not be written
};

#endif // WOLF_PLANE_SOUND_H
