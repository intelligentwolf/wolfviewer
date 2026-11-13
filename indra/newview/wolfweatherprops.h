/**
 * @file wolfweatherprops.h
 * @brief WolfViewer: Weather Props - an umbrella in the rain, a snow shovel in the snow, a leaf rake in autumn (ticket 847).
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

#ifndef WOLF_WEATHERPROPS_H
#define WOLF_WEATHERPROPS_H

#include <map>
#include <set>
#include <string>

#include "llsingleton.h"
#include "lluuid.h"

class LLViewerInventoryItem;

// [WEATHER PROPS 2026-11-13] Ticket 847 (IndigoQueen King, Paul: "yeah go ahead"): "an umbrella when it rains, a snow shovel
// when it snows, a leaf rake in autumn". Design: region-terrain-work/weather-props-tutorials-2026-11-13/DESIGN.md s1.
//
// A prop is an ORDINARY ATTACHMENT - the Wolf Umbrella / Wolf Snow Shovel / Wolf Leaf Rake, Wolf items in the resident's own
// inventory (description "wolfprop:<key>"), worn in the right hand through the normal attach path
// (LLAppearanceMgr::wearItemOnAvatar, add not replace) and taken off the same way (removeItemFromAvatar), exactly as the
// Fishing rod is (wolffishingoverlay.cpp rodCoro). Avatar rendering is untouched; everyone sees the prop on any viewer; the
// right-arm hold is played by the prop's own script. Missing props are given once by the grid (php/wolfprops.php).
//
// The rules (idle(), every CHECK_S): the wanted prop is the umbrella while the SHARED weather here (region, parcel, parcel
// prim, hurricane - never the resident's own Weather menu or a preview) is rain, the shovel while it is snow, else the rake
// while it is autumn at the region's place (wolfleavesseason.h, as the falling leaves); only props ticked. Never while
// sitting; never beside something else in the right hand; only props this viewer put on (or found on at login) are ever
// taken off; a prop the resident takes off stays off until the weather changes; a change must hold SETTLE_S and changes are
// at least SETTLE_S apart. Off by default (WolfViewerWeatherProps). Wolf Territories only: off it, a prop we put on is taken
// off and nothing else runs.
class WolfWeatherProps : public LLSingleton<WolfWeatherProps>
{
    LLSINGLETON(WolfWeatherProps);
    ~WolfWeatherProps() override;

public:
    enum EKind { NONE = 0, UMBRELLA, SHOVEL, RAKE, KIND_COUNT };

    /** "umbrella" / "shovel" / "rake": the description word after "wolfprop:" (build_weatherprops.py Prop keys). */
    static const char* keyOf(EKind k);
    /** "umbrella" / "snow shovel" / "leaf rake": as residents read it. */
    static const char* wordOf(EKind k);
    /** The kind a description names ("wolfprop:<key>" at its start, leading spaces and case aside, the word ending there). */
    static EKind descKind(const std::string& description);

    /** The switch (WolfViewerWeatherProps, off by default) and each prop's tick. */
    static bool enabled();
    static bool kindAllowed(EKind k);

    /** Once a frame (llappviewer.cpp idle, beside WolfLeaves); returns at once between checks. */
    void idle();

    /** What is happening, for the window's status line: the words, and whether they are a problem. */
    const std::string& statusText() const { return mStatus; }
    bool statusIsError() const { return mStatusError; }
    /** The grid's give failed: the window shows Try again, which calls this. */
    bool canRetry() const { return mGive == GIVE_FAILED; }
    void retry();

private:
    enum EGive { GIVE_IDLE = 0, GIVE_ASKING, GIVE_WAITING, GIVE_FAILED };

    EKind wantedKind(bool& sitting, EKind& weather_kind);
    EKind season();
    LLViewerInventoryItem* findProp(EKind k);
    void askGrid();
    static void giveCoro(U32 gen);
    void onGiveReply(bool ok, const std::string& error, const LLUUID& folder);
    void refetchPropFolders();
    void takeOff(const LLUUID& item, const char* why);
    void setStatus(const std::string& text, bool error = false);
    void stopAll(const char* why);
    void forgetScans();

    F64 mNextCheck = 0.0;
    bool mWasActive = false;            // enabled on Wolf at the last check (a change to off takes our prop off once)
    bool mLoginScanned = false;         // the props worn at the first check after login were taken as ours
    EKind mPending = NONE;              // the wanted prop being settled
    F64 mPendingSince = 0.0;
    F64 mLastAction = -1.e9;
    std::set<LLUUID> mManaged;          // props this viewer put on this session, or found on at login: the only ones taken off
    LLUUID mOurItem;                    // the prop we last asked to be worn
    EKind mOurKind = NONE;
    bool mOurConfirmed = false;         // seen worn since we asked
    F64 mOurAskedAt = 0.0;
    LLUUID mRemoving;                   // a prop we asked to take off (its disappearance is ours, not the resident's)
    EKind mDeclined = NONE;             // the resident took this one off: not again until the wanted prop changes
    EKind mWearFailed = NONE;           // would not go on: not again until the wanted prop changes
    std::map<S32, LLUUID> mFound;       // kind -> the item last found (checked again before use)
    // [REVIEW 11-13 W4] per kind: gInventory's item count and the time of the last whole scan that found nothing
    S32 mScanCount[KIND_COUNT] = { -1, -1, -1, -1 };
    F64 mScanAt[KIND_COUNT] = { -1.e9, -1.e9, -1.e9, -1.e9 };
    EKind mCalled = NONE;               // [REVIEW 11-13 W6] the prop the weather / season calls for, sitting aside
    F64 mFirstCheck = -1.0;             // [REVIEW 11-13 W8] the first check while active: the inventory wait has a limit
    S32 mSeasonKind = -1;               // WolfLeavesSeason::Kind, worked out every SEASON_S
    F64 mSeasonAt = -1.e9;
    EGive mGive = GIVE_IDLE;
    U32 mGiveGen = 0;
    F64 mGiveAt = 0.0;
    F64 mRefetchAt = 0.0;
    LLUUID mGiveFolder;
    bool mGiveAlerted = false;          // the failure alert is shown once a session
    std::string mStatus;
    bool mStatusError = false;
};

#endif // WOLF_WEATHERPROPS_H
