/**
 * @file fstransporterfx.h
 * @brief WolfViewer teleport transition — a shower of grains around the avatar.
 *
 * Port of the WolfStorm web viewer's transporter effect (js/ui/transporter_fx.js), so the
 * two viewers teleport the same way.
 *
 * WHAT IT REPLACES. Firestorm's teleport puts an opaque progress screen over the world
 * (llviewerdisplay.cpp update_tp_display -> gViewerWindow->setShowProgress), which is the
 * "black screen with a bar". While this effect is active that screen is suppressed, so the
 * world stays visible and the region genuinely disappears and reappears.
 *
 * WHAT IT DRAWS. A shower of white grains rising through the avatar's projected rectangle —
 * the effect belongs to the person, not the screen. Each grain is a soft additive glow with a
 * hard elongated core on top: they are backlit metal filings catching the light, and the glow
 * is what makes them read as grains rather than dead pixels.
 *
 * Grains RISE because the original series effect was filmed with the camera upside down.
 *
 * There is NO column of light. An earlier revision drew the "shower curtain" and the bright
 * chest-height core as well, and on screen that read as a white sheet over the avatar rather
 * than a beam around them. [Paul 2026-09-02: "now we have the right particles but a white
 * sheet, get rid of that and its perfect".] The grains alone carry it.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Copyright (C) 2026 Wolf Software Systems Ltd
 * $/LicenseInfo$
 */

#ifndef FS_TRANSPORTERFX_H
#define FS_TRANSPORTERFX_H

#include "llsingleton.h"
#include "v3color.h"
#include "llframetimer.h"
#include <vector>

class FSTransporterFX : public LLSingleton<FSTransporterFX>
{
    LLSINGLETON(FSTransporterFX);

public:
    /** Teleport started — begin dematerialising at the avatar's current position. */
    void start();
    /** Arrival — condense and fade out. Safe to call when not running. */
    void finish();
    /** Hard stop (teleport failed/cancelled). */
    void stop();

    /** True while the effect owns the transition, i.e. the TP screen must stay hidden. */
    bool isActive() const { return mPhase != PHASE_IDLE; }

    /** Advance and draw. Called from render_ui_2d(), after the world, before the UI. */
    void render();

private:
    enum EPhase { PHASE_IDLE, PHASE_DEMAT, PHASE_TRANSIT, PHASE_REMAT };

    struct Spark
    {
        F32 mX, mY;
        F32 mVX, mVY;
        F32 mAge, mLife;
        F32 mSize;
        bool mHot;
    };

    void  updateRect();
    void  useFallbackRect();
    void  emit(F32 strength, F32 rate);
    void  advance(F32 dt);
    void  drawGlow(F32 cx, F32 cy, F32 radius, F32 peak, const LLColor3& col);
    void  drawSparks();

    EPhase              mPhase;
    LLFrameTimer        mPhaseTimer;
    F32                 mLastFrame;      // seconds within the phase, for dt
    F32                 mClock;          // seconds since start(), for the grain twinkle
    std::vector<Spark>  mSparks;

    // Avatar rectangle on screen, in RAW GL pixels (origin bottom-left) — the space
    // setup2DRender()'s ortho is built in. See updateRect() for the conversion.
    F32                 mCentreX;
    F32                 mCentreY;
    F32                 mHeight;
    F32                 mUIScale;        // display scale, so grain speed/size is resolution-independent
    bool                mHaveRect;
};

#endif // FS_TRANSPORTERFX_H
