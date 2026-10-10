/**
 * @file wolfflightdeck.h
 * @brief WolfViewer Flight Mode: the flight deck drawn across the bottom of the screen.
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

#ifndef WOLF_FLIGHTDECK_H
#define WOLF_FLIGHTDECK_H

#include <string>
#include <vector>

#include "lluictrl.h"
#include "v4color.h"

class LLFontGL;

// Paul, 2026-10-06: "not a floater — almost like when you go into flight mode the main control
// bar minimises (with the up arrow to bring it back) and you get a proper flight display",
// "it needs to be right across, not that tiny box", "make the flight controls realistic, not
// like that horrible kind of standard firestorm look — PROPER gauges".
//
// One control, the width of the screen, sitting on the toolbar's strip (main_view.xml, under
// the floaters). Nothing on it is a stock widget: every instrument is drawn here, in the
// layout and colours of a modern airliner flight deck, lettered in B612 (Airbus's cockpit
// typeface, fonts/B612-OFL.txt):
//   the glareshield     master WARNING / CAUTION, the autopilot mode control panel (A/T ARM,
//                       SPD, LNAV, VNAV, HDG SEL, ALT HOLD, V/S, CMD), EFIS range, HUD, EXIT
//   clock               UTC, and the time since Flight Mode came on
//   gear                the landing gear lever and its lights, the engine command button
//   PFD                 attitude (pitch ladder, bank scale, slip, flight director), speed tape
//                       with trend vector, altitude tape with ground, vertical speed, heading
//   ND                  MAP mode: heading-up compass, route to the destination, terrain
//                       (land from the loaded regions against the aircraft's altitude),
//                       traffic (other avatars), wind, ground speed, ETA
//   EICAS               crew alerts, the flight-control picture (what the autopilot and the
//                       pilot are holding on each axis), position and flight data
//   CDU                 the flight computer: DIRECT TO a region and position, the CONTROLS
//                       page (which keys the aircraft uses, gear and engine commands)
// and, over the world above it, a head-up display: horizon, pitch ladder and flight path
// vector placed where they really are in the view.
// Every number on it is measured (WolfFlight::Data) or is something the pilot set; nothing is
// invented to look busy.
class WolfFlightDeck : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Params() {}
    };

    WolfFlightDeck(const Params& p);
    ~WolfFlightDeck() override;

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleScrollWheel(S32 x, S32 y, S32 clicks) override;
    bool handleRightMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleDoubleClick(S32 x, S32 y, MASK mask) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;
    bool handleKeyHere(KEY key, MASK mask) override;
    bool handleUnicodeCharHere(llwchar uni_char) override;
    bool acceptsTextInput() const override { return true; }
    void onFocusLost() override;
    void onMouseCaptureLost() override;

    // A clickable thing on the panel, rebuilt every frame by draw().
    enum EHit
    {
        H_NONE = 0,
        H_MASTER_WARN, H_MASTER_CAUT,
        H_AT, H_SPD_KNOB, H_LNAV, H_VNAV, H_HDG_KNOB, H_HDG_SEL, H_ALT_KNOB, H_ALT_HOLD,
        H_VS_WHEEL, H_VS, H_CMD, H_RANGE_KNOB, H_HUD, H_HELP, H_EXIT,
        H_GEAR, H_ENGINE, H_STICK, H_THROTTLE,
        H_PLAN, H_ND_SCREEN, H_TWA_KNOB, H_WIND, H_TACK, H_SAIL, H_WEB, H_MAP, H_BACKGROUND, H_FOLLOW_CAM, H_MINI,
        H_CDU_SCREEN, H_CDU_DIR, H_CDU_APT, H_CDU_CTL, H_CDU_EXEC, H_CDU_CLR, H_CDU_DEL, H_CDU_HELP,
        H_LSK_L1, H_LSK_L2, H_LSK_L3, H_LSK_L4, H_LSK_L5, H_LSK_L6,
        H_LSK_R1, H_LSK_R2, H_LSK_R3, H_LSK_R4, H_LSK_R5, H_LSK_R6,
    };

private:
    struct Hit
    {
        F32 mL, mB, mR, mT;
        EHit mId;
        std::string mTip;
    };

    // layout (view-local), recomputed every frame
    F32 mPanelTop = 0.f;       // top of the deck; above it is the world (and the HUD)
    F32 mPanelBottom = 0.f;
    std::vector<Hit> mHits;

    // fonts, picked each frame for the size the deck is drawn at
    struct Fonts
    {
        const LLFontGL* mTiny = nullptr;
        const LLFontGL* mSmall = nullptr;
        const LLFontGL* mMed = nullptr;
        const LLFontGL* mLarge = nullptr;
        const LLFontGL* mMonoSmall = nullptr;
        const LLFontGL* mMonoMed = nullptr;
        const LLFontGL* mMonoLarge = nullptr;
        const LLFontGL* mMonoHuge = nullptr;
    } mF;
    void pickFonts(F32 unit);

    // the flight computer
    enum EPage { PAGE_DIR = 0, PAGE_CTL, PAGE_APT };   // <WolfViewer 2026-10-10/> PAGE_APT: find an airport by name
    EPage mPage = PAGE_DIR;
    std::string mScratch;
    std::string mScratchMsg;        // "INVALID ENTRY" and friends, shown until CLR
    // <WolfViewer 2026-10-10> the AIRPORTS page: what was searched for, the matches (directory ids), the page of them
    std::string mAptQuery;
    std::vector<S32> mAptIds;
    S32 mAptPage = 0;
    void aptSearch(const std::string& text);
    bool pasteScratch();            // Ctrl+V: the clipboard into the scratchpad
    // </WolfViewer>
    bool mModified = false;         // a route typed in and not yet EXECuted
    std::string mModRegion;
    F32 mModX = 128.f, mModY = 128.f, mModZ = 0.f;
    bool mModHasXY = false, mModHasZ = false;
    bool mDelete = false;           // DEL pressed: the next LSK deletes that field
    bool mArmRoute = false;
    U32 mDestSerialSeen = 0;        // <WolfViewer 2026-10-10/> WolfFlight::destSerial last shown on DIR TO         // EXECuted: engage LNAV / VNAV once the region is found

    S32 mPressed = H_NONE;
    F64 mPressedAt = 0.0;
    S32 mPressStep = 0;             // a knob held down: which way it keeps turning
    F64 mLastRepeat = 0.0;
    F64 mFlightStart = 0.0;

    // EGPWS terrain picture on the ND, rebuilt a few times a second
    struct TerrainDot { F32 mX, mY; U8 mLevel; };
    std::vector<TerrainDot> mTerrain;
    F64 mTerrainBuilt = 0.0;
    F32 mTerrainKey[4] = { 0.f, 0.f, 0.f, 0.f };

    void layoutAndDraw();
    void drawHUD(F32 top_of_world);
    void drawWarnings(F32 top_of_world);
    void drawHelp(F32 top_of_world);
    bool mShowHelp = false;
    bool mHelpChecked = false;      // which mode's first-time guide was last considered
    bool mHelpForSail = false;
    void drawGlareshield(F32 l, F32 b, F32 r, F32 t);
    void drawMini(F32 bottom);   // <WolfViewer 2026-10-10/> the deck shrunk to one bar (WolfFlightDeckMini)
    void drawClock(F32 l, F32 b, F32 r, F32 t);
    void drawGear(F32 l, F32 b, F32 r, F32 t);
    void drawStick(F32 l, F32 b, F32 r, F32 t);
    void drawHelm(F32 l, F32 b, F32 r, F32 t);
    void drawWindDial(F32 l, F32 b, F32 r, F32 t);
    void drawSailData(F32 l, F32 b, F32 r, F32 t);
    void drawPlan(F32 l, F32 b, F32 r, F32 t);
    void drawTiles(F32 sl, F32 sb, F32 sr, F32 st, const LLVector3d& centre, F32 ppm);
    // the stick's gate and the lever's slot, for dragging them
    F32 mStickCx = 0.f, mStickCy = 0.f, mStickR = 1.f;
    F32 mLeverCy = 0.f, mLeverHalf = 1.f;
    void dragControl(S32 x, S32 y);
    void drawPFD(F32 l, F32 b, F32 r, F32 t);
    void drawND(F32 l, F32 b, F32 r, F32 t);
    void drawEICAS(F32 l, F32 b, F32 r, F32 t);
    void drawCDU(F32 l, F32 b, F32 r, F32 t);
    void drawBezel(F32 l, F32 b, F32 r, F32 t, F32 inset, F32& sl, F32& sb, F32& sr, F32& st);

    void addHit(F32 l, F32 b, F32 r, F32 t, EHit id, const std::string& tip);
    const Hit* hitAt(S32 x, S32 y) const;
    static bool showBackground();          // WolfFlightDeckBackground: the PANEL button
    bool    onPanel(S32 x, S32 y) const;   // takes the mouse here (see wolfflightdeck.cpp)
    void press(EHit id, S32 x, S32 y, S32 step);
    void knob(EHit id, S32 step);
    void lsk(bool left, S32 line);
    void execRoute();
    void syncRouteFromDest();
    void cduLines(std::string (&label)[6][2], std::string (&data)[6][2], LLColor4 (&col)[6][2], std::string& title);
};

#endif // WOLF_FLIGHTDECK_H
