/**
 * @file wolflightning.h
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
#ifndef WOLF_LIGHTNING_H
#define WOLF_LIGHTNING_H

#include <vector>
#include "llpointer.h"
#include "llsingleton.h"
#include "llviewerpartsource.h"
#include "v3math.h"

// [LIGHTNING 2026-09-13] Paul: "when we have a thunderstorm in weather it needs to have
// lightning as well but not screen flashing as we dont want to cause epileptic fits just
// forks of lightning and remember NO WEATHER CAN ENTER A BUILDING".
//
// So: a BOLT, drawn in the world as a jagged channel with a few side branches, that re-strikes
// two or three times down the same channel the way a real one does — and nothing else. No
// sky brightening, no full-screen flash, no tint on the frame. The thunder is the same crack
// the weather sound already had, but it now FOLLOWS the bolt at the speed of sound
// (WolfWeatherSound::crack), so the delay tells the resident how far away it struck.
//
// The bolt is particles: one WolfLightningPartSource, emissive (LL_PART_EMISSIVE_MASK, the one
// thing the rain deliberately is not), every flash a fresh set with exactly the flash's
// lifetime. Reusing the particle system means the bolt is drawn, culled, sorted and blended by
// the same code as everything else and there is no new render path to keep alive.
//
// THE GOLDEN RULE for lightning: the bolt's foot is the FIRST SURFACE under the cloud — a roof,
// a bridge, the ground, the water — never a floor under a roof (the same ray the rain uses,
// gPipeline.lineSegmentIntersectInWorld). And when the CAMERA is under a roof no bolt is drawn
// at all: the thunder still rolls, as it does indoors, but nothing flashes inside the room.
// Source: wolfweather.cpp updateLanding (the roof test), WolfWeatherPartSource::cameraUnderRoof.

class WolfLightningPartSource : public LLViewerPartSource
{
public:
    WolfLightningPartSource();
    /** One bolt: its channel and branches, agent space. Re-struck by the flash schedule. */
    struct Bolt
    {
        std::vector<LLVector3> mChannel;                 // top -> foot
        std::vector<std::vector<LLVector3> > mBranches;  // each top -> tip
        F32 mBorn = 0.f;                                 // source age at first flash
        S32 mFlash = 0;                                  // next flash to fire
        S32 mFlashes = 0;                                // how many in total (2..3)
    };
    void strike(const Bolt& bolt) { mBolt = bolt; mBolt.mBorn = mAge; mBolt.mFlash = 0; }
    bool striking() const { return mBolt.mFlash < mBolt.mFlashes; }
    void update(const F32 dt) override;
    /** Particle spacing along a channel, metres. */
    static constexpr F32 STEP_M = 3.5f;
private:
    void emitLine(const std::vector<LLVector3>& pts, F32 width, F32 life, F32 alpha);
    F32  mAge = 0.f;
    Bolt mBolt;
};

class WolfLightning : public LLSingleton<WolfLightning>
{
    LLSINGLETON(WolfLightning);
    ~WolfLightning();
public:
    /** Every frame from WolfWeather::idle(): decides whether it is storming and schedules strikes. */
    void idle();
    /** Storm over / weather switched off: nothing pending, nothing drawn. */
    void reset();
    // Strike cadence. Source: wolfweathersound.cpp 2026-09-12 THUNDER_MIN/SPAN, DISTANT_MIN/SPAN —
    // the thunder's own intervals, now owned here because the bolt comes first and the clap
    // is timed from it.
    static constexpr F64 THUNDER_MIN_SECS = 9.0,  THUNDER_SPAN_SECS = 22.0;
    static constexpr F64 DISTANT_MIN_SECS = 22.0, DISTANT_SPAN_SECS = 40.0;
    /** Speed of sound, m/s at ~15 C — the clap's delay per metre of distance. */
    static constexpr F32 SOUND_MPS = 340.f;
private:
    bool storming(bool& distant) const;
    void strike(bool distant);
    bool buildBolt(bool distant, WolfLightningPartSource::Bolt& out, F32& distance) const;
    F64  mNextStrike = 0.0;
    bool mWasStorming = false;
    bool mWasDistant = false;
    // A clap waiting for the sound to reach the listener.
    F64  mClapAt = 0.0;
    bool mClapDistant = false;
    bool mClapPending = false;
    LLPointer<WolfLightningPartSource> mSource;
};

#endif // WOLF_LIGHTNING_H
