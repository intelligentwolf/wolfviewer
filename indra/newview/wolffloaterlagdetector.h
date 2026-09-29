/**
 * @file wolffloaterlagdetector.h
 * @brief WolfViewer: World > Lag Detector… — what on this region is slowing it down, in plain
 *        words, with the fix the region or parcel owner can make.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 * $/LicenseInfo$
 */

#ifndef WOLF_FLOATERLAGDETECTOR_H
#define WOLF_FLOATERLAGDETECTOR_H

#include "llfloater.h"
#include "llframetimer.h"
#include "v3math.h"

#include <deque>
#include <map>
#include <vector>

class LLButton;
class LLCheckBoxCtrl;
class LLMessageSystem;
class LLScrollListCtrl;
class LLTextBox;

// [LAG DETECTOR 2026-09-29] Paul: "i want to have as little load on the region as i can", "the
// problem with top scripts is it doesn't really tell you what's misbehaving", "i only want it
// running when the window is open lets make it a lag detector so it can detect scripts like that
// and also anything else that is causing lag that the region owner can fix".
//
// The region is asked nothing new. While this floater is drawn, every POLL_SECONDS it sends the
// two reports Firestorm's Top Objects already uses (LandStatRequest: Top Scripts, Top Colliders -
// llfloatertopobjects.cpp onRefresh), and reads the SimStats every viewer already receives each
// second (LLStatViewer::SIM_*, llviewerstats.cpp). Closed, it does nothing at all.
//
// The maths is here: Top Scripts' Score is each object's script CPU in ms since the region
// started (OpenSimWolf YEngine GetTopObjectStats sums XMRInstance.m_CPUTime), so two replies
// POLL_SECONDS apart give its share of one CPU core now. Kept for up to WINDOW_INTERVALS:
//   runaway  - at least RUNAWAY_FRACTION in every interval (2+): the rejector on Wolf Mountain,
//              `while(i < n)` with no i++, sat at 94% for hours;
//   heavy    - at least HEAVY_FRACTION on average (2+ intervals);
//   busy     - at least BUSY_FRACTION on average: listed only when the owner asks.
// Region-wide checks:
//   scripts overloaded - the objects' shares add up to SCRIPTS_TOTAL_FRACTION or more with no
//                        single culprit: the busiest objects are listed to thin out. (OpenSim never
//                        fills SIM_PERCENTAGE_SCRIPTS_RUN or script ms - SimStatsReporter.cs never
//                        sets SimPCTSscriptsRun, Scene.AddScriptExecutionTime has no callers.)
//   physics struggling - physics FPS under the Lag Meter's warning level (LFSimFeatureHandler
//                        simulatorFPSWarn): the Top Colliders list (sorted here - OpenSim's
//                        BulletS returns 25 in no order, BSScene.cs GetTopColliders discards its sort).
// Estate owner and managers get the whole region; anyone else gets the parcel they stand on
// (RequestFlags bit 1), which OpenSimWolf allows when they can edit that parcel
// (EstateManagementModule.cs LandStatAllowed) and otherwise answers with an empty report.
class WolfFloaterLagDetector : public LLFloater
{
public:
    WolfFloaterLagDetector(const LLSD& key);
    ~WolfFloaterLagDetector() override;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

    // From LLFloaterTopObjects::handle_land_reply: true when the reply carries REQUEST_MARKER
    // (it is this floater's, whether or not the floater is still open) - Top Objects must not
    // see it.
    static bool handleLandStatReply(LLMessageSystem* msg);

    // Set in RequestFlags; OpenSim echoes RequestFlags in LandStatReply (LLClientView
    // SendLandStatReply) and reads only bits 0x1 (parcel) and 0x0e (name filters).
    static constexpr U32 REQUEST_MARKER = 0x40000000;

private:
    struct ScriptLoad
    {
        U32         mLocalID = 0;
        std::string mName, mOwner, mParcel;
        LLVector3   mPos;
        F32         mLastScore = 0.f;    // ms of script CPU since the region started
        F64         mLastTime = 0.0;     // when that score arrived (seconds)
        std::deque<F32> mFractions;      // share of one core in each interval, newest last
        bool        mSeen = false;       // present in the latest reply
    };

    struct Collider
    {
        LLUUID      mID;
        U32         mLocalID = 0;
        std::string mName, mOwner, mParcel;
        LLVector3   mPos;
        F32         mScore = 0.f;
    };

    enum ERowKind { ROW_RUNAWAY, ROW_HEAVY, ROW_SCRIPTS_SUMMARY, ROW_SCRIPTS_OBJECT,
                    ROW_PHYSICS_SUMMARY, ROW_PHYSICS_OBJECT, ROW_BUSY };

    struct Row
    {
        ERowKind    mKind;
        LLUUID      mObjectID;           // null for the summary rows
        U32         mLocalID = 0;
        std::string mTitle;
        std::string mDetail;
        std::string mObjectName;         // for the beacon and the confirmations
        std::string mOwner;
        LLVector3   mPos;
    };

    void sendRequests();
    void onReply(U32 report_type, LLMessageSystem* msg);
    void resetForRegion();
    void rebuild();
    void updateButtons();
    const Row* selectedRow() const;

    F32  averageFraction(const ScriptLoad& s) const;
    F32  minFraction(const ScriptLoad& s) const;
    std::string percent(F32 fraction) const;
    std::string where(const std::string& owner, const std::string& parcel, const LLVector3& pos) const;

    void onBeacon();
    void onTeleport();
    void onReturn();
    void onDelete();

    LLTextBox*        mVerdict = nullptr;
    LLTextBox*        mSpeed = nullptr;
    LLScrollListCtrl* mList = nullptr;
    LLTextBox*        mDetail = nullptr;
    LLCheckBoxCtrl*   mShowBusy = nullptr;
    LLButton*         mBeaconBtn = nullptr;
    LLButton*         mTeleportBtn = nullptr;
    LLButton*         mReturnBtn = nullptr;
    LLButton*         mDeleteBtn = nullptr;

    LLFrameTimer      mPollTimer;
    bool              mFirstPoll = true;
    U64               mRegionHandle = 0;
    bool              mRegionScope = false;   // estate powers: the whole region
    S32               mParcelLocalID = 0;     // otherwise the parcel stood on
    S32               mScriptReplies = 0;     // replies received since the window opened

    std::map<LLUUID, ScriptLoad> mScripts;
    std::vector<Collider>        mColliders;
    std::vector<Row>             mRows;
};

#endif // WOLF_FLOATERLAGDETECTOR_H
