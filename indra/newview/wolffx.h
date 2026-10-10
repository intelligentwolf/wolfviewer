/**
 * @file wolffx.h
 * @brief WolfViewer: explosions, fireballs, fire, smoke, sparks, fireworks, dust and shock rings
 *        from scripts (wolfEffect), and camera shake (wolfShakeCamera).
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

#ifndef WOLF_FX_H
#define WOLF_FX_H

#include "llsingleton.h"
#include "llviewerpartsource.h"
#include "v3dmath.h"
#include "v4color.h"

#include <string>
#include <vector>

// [WOLF FX 2026-10-10] Paul: "add lsl to allow users to cause explosions etc look at how that works in
// other games and implment in our viewers like advanced particles? idk at the moment particles are quite
// basic in opensim. add things like being able to shake the users viewer".
//
// WolfSim's WolfFXModule sends GenericMessage "WolfFX" (the wire format is in WolfFXModule.cs) to Wolf
// viewers; this draws it with the viewer's own particle system (LLViewerPartSim: sorted, blended, glow,
// culled like every particle), built the way game engines build explosions - in layers, each its own
// particles: a white flash, a fireball blooming out and cooling yellow-white -> orange -> dark red
// (additive, emissive), a shock ring racing out, sparks on ballistic arcs that bounce off the ground,
// dark debris, and a smoke column that rises, spreads and darkens for seconds after; the camera shakes
// by distance (WolfCameraFX). Fire, smoke and sparks can run for a while (duration); fireworks burst in
// two colours with trailing sparks. The boom itself is played by the region for every viewer.
//
// Settings: WolfFXEnabled (draw them at all), WolfFXCameraShake (shakes from effects and scripts).
class WolfFXSource;

class WolfFX : public LLSingleton<WolfFX>
{
    LLSINGLETON(WolfFX);
    ~WolfFX();

public:
    /** Every frame (LLAppViewer::idle). */
    void idle();
    /** GenericMessage "WolfFX" from a region (invoice = that region's id). */
    void receive(const LLUUID& region_id, const std::vector<std::string>& strings);

private:
    std::vector<LLPointer<WolfFXSource>> mSources;
};

/** One running effect: its own particle source, standing on the ground under the effect so sparks and
 *  debris bounce off the land (LLViewerPartSim's bounce plane is the source's height). */
class WolfFXSource : public LLViewerPartSource
{
public:
    enum EKind { EXPLOSION, FIREBALL, SMOKE, SPARKS, FIRE, FIREWORKS, DUST, SHOCKWAVE };

    WolfFXSource(EKind kind, const LLVector3d& global, F32 size, const LLColor4& c, const LLColor4& c2,
                 F32 duration, F32 intensity, const LLVector3& dir);
    void update(const F32 dt) override;
    bool finished() const { return mAge > mDuration + 0.25f; }

private:
    LLViewerPart* part(const LLVector3& pos, const LLVector3& vel, const LLVector3& accel, F32 life,
                       F32 scale0, F32 scale1, const LLColor4& c0, const LLColor4& c1, F32 glow0, F32 glow1,
                       U32 flags, bool additive);
    void burst();               // what happens at the first moment
    void stream(F32 dt);        // what keeps coming for the duration
    S32 count(F32 n) const;     // n particles at intensity 1, scaled, at least 1

    EKind mKind;
    LLVector3d mGlobal;
    F32 mSize, mDuration, mIntensity;
    LLColor4 mC, mC2;
    LLVector3 mDir;
    F32 mAge = 0.f;
    bool mBurst = false;
    F32 mCarry = 0.f;           // fractional particles owed by stream()
    LLVector3 mCentre;          // agent space, this frame
};

#endif // WOLF_FX_H
