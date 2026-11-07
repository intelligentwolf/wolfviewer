/**
 * @file wolfskis.h
 * @brief WolfViewer: skis and snowboards (ticket 556) - the ride's link to its script, the run's numbers, the bests.
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

#ifndef WOLF_SKIS_H
#define WOLF_SKIS_H

#include <string>
#include <vector>

#include "llsingleton.h"
#include "lluuid.h"
#include "v3dmath.h"

// [SKIS 2026-11-06] Paul: "build skis and snowboards add a new hud for it that will be fun". Ticket 556 (Indi Moon): ride
// down mountain slopes, the speed from the slope.
//
// [SLEDGES 2026-11-07] Ticket 556 again (Indi Moon): "Would it be possible to add sledding and snow disk sledding to the
// winter fun too?" - Paul: yes. Two more rides on the same link and HUD: a sledge (wolfvehicle:sledge) and a snow disc
// (wolfvehicle:snowdisc), their script region-terrain-work/sledges-2026-11-07/lsl/sled_template.lsl (the skis' protocol,
// arrow keys only, no jump or tuck). The window became Wolf World > Winter Fun... (wolffloaterskis.cpp).
//
// The ride is the object's own script (region-terrain-work/skis-2026-11-06/lsl/ski_template.lsl): every viewer can ride.
// This viewer adds, on Wolf Territories only:
//   - Wolf World > Skis and Snowboards... (wolffloaterskis.cpp): put them on (WolfVehicleRez with "skis" / "snowboard":
//     the object whose description starts "wolfvehicle:skis" / "wolfvehicle:snowboard") and take them off (stand up);
//   - the Ski HUD (wolfskihud.cpp) while riding: speed, steepness, vertical, the run's clock, air time, a style score with
//     friendly pop-ups, the best here (kept on this computer, setting WolfSkiBests).
// The script's own lines (the Captain's Helm protocol, region-terrain-work/pirate-ship/DESIGN.md s20, wolfhelm.h):
//   viewer -> skis  "ski hello 1" said on channel 7 while sitting on them (the script listens to its rider only)
//   skis -> viewer  llRegionSayTo(rider, 0, "WolfSki1|<kind>|...") - hi|<kind>|<title>|1, st|<mode>, jump,
//                   land|<air s>|<spin deg>|<clean 0/1>|<impact m/s>, wipe|<why>, up, bye - only to a viewer that said
//                   hello, taken out of chat here (onObjectChat) when they come from the seat.
// Everything else (speed, slope, vertical, the clock) is measured here from the seat's own motion and the ground under it
// (sky land included: WolfTerrainLevels::resolveSupportHeight). Nothing runs while not riding (idle() returns at once).
class WolfSkis : public LLSingleton<WolfSkis>
{
    LLSINGLETON(WolfSkis);
    ~WolfSkis();

public:
    enum EKind { KIND_NONE = 0, KIND_SKIS, KIND_SNOWBOARD, KIND_SLEDGE, KIND_SNOWDISC };
    static const char* kindWord(EKind k);          // "skis" / "snowboard" / "sledge" / "snowdisc": the wolfvehicle: type
    /** "skis" / "snowboard" / "sledge" / "snowdisc" -> the kind; anything else KIND_NONE. */
    static EKind kindFromWord(const std::string& word);
    /** [SLEDGES 2026-11-07] a sledge or a snow disc: sat on, steered by leaning, arrow keys only (no jump, no tuck). */
    static bool isSled(EKind k) { return k == KIND_SLEDGE || k == KIND_SNOWDISC; }


    void idle();
    /** A chat line from an object (llviewermessage.cpp). True when it was the skis' line to this HUD: shown nowhere. */
    bool onObjectChat(const LLUUID& source, U8 chat_type, const std::string& text);

    // ---- the window ----
    void putOn(EKind k);                           // rez the free ones of that kind and get on them
    void takeOff();                                // stand up (the script takes them back to the inventory)
    bool riding() const { return mKind != KIND_NONE; }
    EKind kind() const { return mKind; }
    /** The seat `root` is skis / a snowboard by its description, if the region has told us it (WolfObjectProps): WolfDrive
     *  keeps its dashboard shut for it. Read straight from the store, so it does not wait for this frame's idle(). */
    static bool seatIsSkis(const LLUUID& root);

    // ---- the HUD ----
    bool hudShowing() const { return mShown && mKind != KIND_NONE; }
    void showHud(bool on);
    bool answered() const { return mAnswered; }    // the script said hi

    struct Popup
    {
        std::string mText, mSub;
        S32 mTone = 0;                             // 0 good (white/cyan), 1 great (gold), 2 ouch (amber)
        F64 mAt = 0.0;
    };
    struct Shown
    {
        F32 mSpeed = 0.f;                          // m/s along the ground and through the air (the whole velocity)
        F32 mSlopeDeg = 0.f;                       // the ground's steepness under them, degrees
        bool mSlopeKnown = false;
        F32 mHeading = 0.f;                        // degrees, 0 north, clockwise (the skis' +X)
        bool mRunning = false;                     // the clock runs: pushed off and not yet stopped
        F32 mRunSecs = 0.f;
        F32 mVertical = 0.f;                       // metres come down this run (the descents added up)
        F32 mTop = 0.f;                            // m/s, this run
        F32 mBigAir = 0.f;                         // s, this run's longest
        S32 mScore = 0;                            // style points this run
        S32 mCombo = 0;                            // carved turns in a row (left, right, left...)
        S32 mSpins = 0;                            // [SLEDGES 2026-11-07] the snow disc: whole turns round this run
        bool mInAir = false;
        F32 mAirNow = 0.f;                         // s in the air so far
        std::string mMode;                         // the script's st (ride tuck brake push carve_l carve_r air wipe water stop)
        bool mWiped = false;
        // the last finished run here, shown until the next one starts
        bool mHaveLast = false;
        F32 mLastSecs = 0.f, mLastVertical = 0.f, mLastTop = 0.f;
        S32 mLastScore = 0;
        bool mLastBest = false;                    // it set a best
        // the bests here (this computer, this region)
        S32 mBestScore = 0;
        F32 mBestTop = 0.f, mBestAir = 0.f;
    };
    const Shown& shown() const { return mShownData; }
    /** The pop-ups still on screen (newest last); old ones are dropped here. */
    const std::vector<Popup>& popups();
    bool mph() const;
    void setMph(bool on);

    static bool onWolf();

private:
    void reset(const LLUUID& seat);
    void say(const std::string& text);
    void measure(F32 dt);
    void endRun(bool keep);
    void popup(const std::string& text, const std::string& sub, S32 tone);
    void addScore(S32 points);
    void claimBottom();
    void loadBests();
    void saveBests();
    std::string bestsKey() const;
    void onLand(F32 air, S32 spin, bool clean, F32 impact);
    void onMode(const std::string& m);

    // seat
    LLUUID mSeatRoot;
    EKind mKind = KIND_NONE;
    bool mDescKnown = false;
    std::string mRegionName;
    // link to the script
    bool mAnswered = false;
    S32 mHelloTries = 0;
    F64 mHelloAt = -100.0;
    // HUD
    bool mShown = false;
    bool mClosedByHand = false;
    Shown mShownData;
    std::vector<Popup> mPopups;
    // the run
    F64 mLastTick = 0.0;
    F64 mStillSince = 0.0;
    F64 mAirStart = 0.0;
    LLVector3d mLastPos;
    bool mHavePos = false;
    std::string mLastCarve;                        // carve_l / carve_r of the last turn counted
    F64 mCarveSince = 0.0;
    F64 mLastTurnAt = 0.0;
    F64 mFastSince = 0.0;
    F32 mLastYawDeg = 0.f;                         // [SLEDGES 2026-11-07] the snow disc's turning round, counted in measure()
    F32 mTurnedDeg = 0.f;
    bool mHaveYaw = false;
};

#endif // WOLF_SKIS_H
