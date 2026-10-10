/**
 * @file wolfdjdesk.h
 * @brief WolfViewer Wolf DJ: the mixing desk drawn in the Wolf DJ window - channel strips with
 *        knobs, long-throw faders and LED meters, a stereo master, and the meter bridge with the
 *        playlist's deck display and the on-air spectrum.
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

#ifndef WOLF_DJDESK_H
#define WOLF_DJDESK_H

#include <array>
#include <string>
#include <vector>

#include "lluictrl.h"
#include "wolfdjaudio.h"

// [WOLF DJ 2026-10-10] Paul, after the first radio show: "make the volume controls into proper
// mixing desk sliders (please research on the internet how mixing desks look) upgrade the whole
// look to look like a mixing desk with graphics", "a MON button on each channel", "always show
// the eq if something is playing and flash the volume control if its down and there is something
// playing", "i need to see how long a track is as it's playing", "we need stereo sliders for the
// main mix".
//
// The look follows broadcast and live desks (Sonifex S2, Behringer X32 / Midas M32, Allen & Heath
// channel strips), top to bottom: the channel's name, its source, the colour-capped EQ knobs
// (HF / MF / LF), the effect, the routing buttons - MON (green: heard in the DJ's own output),
// CUE (amber: pre-fader listen) and MUTE (red) - then the long-throw fader with its dB scale
// (+10 at the top, 0 at three quarters, -inf at the bottom stop: wolfdj_fader_db) beside the
// channel's LED meter. The master has a fader per side and a stereo meter. Across the top, a
// meter bridge: the playlist deck (song, elapsed, length, time left, the song loaded next, the
// transport) and the spectrum of what is on air.
//
// Everything here is drawn and handled by this one control (the wolfdashboard.cpp pattern); only
// the music channels' source lists are stock combo boxes, which the floater lays over the slots
// this control leaves for them (sourceRect). The values live in WolfDJMixer: this control reads
// and writes them directly, and commits (onCommit) when a change is finished, for saving.
class WolfDJDesk : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Params() {}
    };

    WolfDJDesk(const Params& p);
    ~WolfDJDesk() override;

    static constexpr S32 WIDTH = 760;
    static constexpr S32 HEIGHT = 486;

    // Desk-local rect left free for a music channel's source list.
    LLRect sourceRect(int ch) const;

    // The master faders move together (LINK, saved with the levels by the floater).
    bool masterLinked() const { return mLinked; }
    void setMasterLinked(bool linked) { mLinked = linked; }

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleDoubleClick(S32 x, S32 y, MASK mask) override;
    bool handleRightMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleScrollWheel(S32 x, S32 y, S32 clicks) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;
    void onMouseCaptureLost() override;

    // What a control on the desk is: its kind and which strip it is on.
    enum EKind
    {
        K_NONE = 0,
        K_FADER, K_EQ_HIGH, K_EQ_MID, K_EQ_LOW, K_FX_TYPE, K_FX_AMOUNT,
        K_MON, K_CUE, K_MUTE,
        K_MASTER_L, K_MASTER_R, K_LINK,
        K_PREV, K_PLAY, K_NEXT, K_STOP, K_DECK,
    };
    static constexpr int STRIP_JINGLE = WolfDJ::CH_COUNT;           // after the channels
    static constexpr int STRIP_MASTER = WolfDJ::CH_COUNT + 1;

private:
    struct Hit
    {
        S32 mL, mB, mR, mT;
        EKind mKind;
        int mStrip;
        std::string mTip;
        // Faders: the travel, for turning a mouse height into a position.
        S32 mTravelB = 0, mTravelT = 0;
    };
    std::vector<Hit> mHits;

    void addHit(S32 l, S32 b, S32 r, S32 t, EKind kind, int strip, const std::string& tip, S32 travel_b = 0, S32 travel_t = 0);
    const Hit* hitAt(S32 x, S32 y) const;

    // The value a control changes (normalised 0..1 for knobs and faders) and its reset value.
    float value(EKind kind, int strip) const;
    void setValue(EKind kind, int strip, float v);
    float defaultValue(EKind kind) const;
    void toggle(EKind kind, int strip);
    void nudge(const Hit& h, S32 clicks);

    void drawBridge();
    void drawStrip(int strip);
    void drawMaster();
    void drawKnob(S32 cx, S32 cy, F32 r, float v, const LLColor4& cap, bool centred);
    void drawFader(S32 cx, S32 b, S32 t, float pos, const LLColor4& cap, bool flash, bool scale_left);
    void drawMeter(S32 l, S32 b, S32 r, S32 t, float post, float pre, int slot);
    void drawButton(S32 l, S32 b, S32 r, S32 t, const std::string& label, bool lit, const LLColor4& lamp);

    // Mouse drag.
    EKind mDragKind = K_NONE;
    int mDragStrip = -1;
    S32 mDragY = 0;
    float mDragStart = 0.f;
    float mDragOther = 0.f;             // the other master side, when linked
    S32 mDragB = 0, mDragT = 0;
    S32 mGrabOffset = 0;                // a fader cap grabbed off its centre stays where it was held
    bool mDragFine = false;             // Shift held: fine adjustment
    bool mLinked = true;

    // Meter ballistics per meter slot (channels, jingle, master L / R), shown dB falling 24 dB/s.
    static constexpr int METER_SLOTS = WolfDJ::CH_COUNT + 3;
    std::array<float, METER_SLOTS> mShownDb;
    std::array<float, METER_SLOTS> mShownPreDb;
    std::array<float, METER_SLOTS> mClipUntil;
    std::array<float, WolfDJ::SPECTRUM_BANDS> mSpecDb;
    std::array<float, WolfDJ::SPECTRUM_BANDS> mSpecPeakDb;
    std::array<float, WolfDJ::SPECTRUM_BANDS> mSpecPeakUntil;
};

#endif // WOLF_DJDESK_H
