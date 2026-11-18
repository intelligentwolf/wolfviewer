/**
 * @file wolffloatertrack.h
 * @brief WolfViewer: Lay Railway Track - draw a railway on the ground, see its plan, lay it on your land.
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

#ifndef WOLF_FLOATER_TRACK_H
#define WOLF_FLOATER_TRACK_H

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "llfloater.h"
#include "llsd.h"
#include "lluuid.h"
#include "v2math.h"
#include "v3dmath.h"
#include "v3math.h"
#include "v4color.h"
#include "wolffloatertunnels.h"   // WolfRouteHost, WolfToolTunnel: the Tunnel tool's pen, reused (as the Lifts window)

class LLButton;
class LLCheckBoxCtrl;
class LLComboBox;
class LLLineEditor;
class LLPanel;
class LLProgressBar;
class LLRadioGroup;
class LLScrollListCtrl;
class LLSpinCtrl;
class LLTextBox;
class LLViewerRegion;

/**
 * [TRACK TOOL 2026-11-14] Paul, 13 Nov: "a track laying tool so people can lay tracks with points just by drawing"; "anyone can
 * lay track on their own regions where they have build rights". 14 Nov: "dont forget to add stations as well". Built into the
 * viewer (no HUD), under Wolf World > Build & Land, Wolf Territories only.
 * Design: firestorm/region-terrain-work/track-tool-2026-11-14/DESIGN.md (1 the flow, 5 the viewer side, 10 the contract).
 *
 * Persona: a resident aged 35-65 who owns some land and has never built a railway. Story: "I draw a line on my land, see real
 * track along it, add a station and a branch, press Lay track and run my own train on it within ten minutes."
 * Flow: the window opens drawing (camera from above, the Tunnel tool's pen) -> 1. pick what the clicks make (a line, a branch,
 * a station, a signal) and click on the ground; 0.6 s after the drawing last changed the plan is asked for (php/track_tool.php
 * "plan") and drawn in the world, with its problems as red marks and in the window -> 2. Lay track ("lay", then "status"
 * every 2 s while the job runs and the window is open) -> the window rezzes the region's item exactly (RezObject, as the Caves
 * tool), finds the pieces and sends "saved". Your railways ("open"): Show, Change (a new layout that replaces the old one once
 * it is laid everywhere), Remove ("remove", DeRezObject to the Trash as the viewer's Delete, "removed").
 *
 * All drawing is kept in GRID metres (region origin + local), so a drawing may cross region borders; the pen works in the
 * agent's region's metres, so clicks() is the drawing seen from the agent's region (points may lie outside it).
 */
namespace WolfTrackTool
{
    /** Every AlertMessage (llviewermessage.cpp process_alert_core): a "Can't rez object" refusal is kept for the window. */
    void onAlert(const std::string& message);
}

class WolfFloaterTrack : public LLFloater, public WolfRouteHost
{
public:
    explicit WolfFloaterTrack(const LLSD& key);
    ~WolfFloaterTrack() override;
    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;
    void draw() override;

    // WolfRouteHost (the pen)
    void clickAt(F32 x, F32 y) override;
    void moveClick(S32 k, F32 x, F32 y, bool done) override;
    void undoPoint() override;
    void removeClick(S32 k) override;
    void insertClickNear(F32 x, F32 y) override;
    void setStatus(const std::string& text, bool error) override;
    void penStopped() override;
    F32 planeHeight() const override;
    const std::vector<LLVector2>& clicks() const override { return mFlat; }
    bool routeEditing() const override;
    std::string noGroundText(bool underground) const override;
    std::string escapeText() const override;
    std::string markerLabel(size_t k) const override;
    std::string routeHint() const override;
    std::string needsLineWords(S32 mode) const;   // [TRACK STEPS 2026-11-18] "Then mark the tunnel on it." etc.
    void renderRouteExtras(LLViewerRegion* rgn) override;
    void drawRouteExtras(LLViewerRegion* rgn) override;
    bool customSurface() const override { return true; }
    bool pickSurface(S32 x, S32 y, LLVector3& out) const override;
    bool surfaceZ(F32 x, F32 y, F32& z) const override;
    bool joinClicks() const override { return false; }   // the window draws its own lines (several of them)

    /** WolfTrackTool::onAlert: a rez refusal while this window waits for its pieces. */
    void onRezAlert(const std::string& message);

private:
    // ---- the drawing (grid metres) ----
    struct GP { F64 x = 0.0, y = 0.0; };
    struct Line
    {
        bool mBranch = false;
        GP mStart;                    // a branch: where it leaves its line
        std::vector<GP> mClicks;
        bool mClosed = false;
        bool mJoins = false;          // a branch whose last click is on a line (said in the window only)
        U32 mStamp = 0;
    };
    struct Station
    {
        GP mA, mB;
        bool mHasB = false;
        std::string mSide = "left", mStyle = "town", mName;
        U32 mStamp = 0;
    };
    struct Signal
    {
        GP mAt;
        U32 mStamp = 0;
    };
    /** [TRACK TOOL 2026-11-14] Paul: "can we add the turntable to our track drawing tool please" (DESIGN.md 4.7, 10 REQ). */
    struct Turntable
    {
        GP mAt;                       // the end of a line: the deck's centre
        S32 mSize = 20;               // deck length, m: 15, 20 or 24
        S32 mRoads = 3;               // stall roads, 1-10
        S32 mSpread = 20;             // degrees between roads, 15-30
        bool mShed = false;           // an engine shed over the roads
        U32 mStamp = 0;
    };
    /** [TRACK TOOL TUNNELS 2026-11-15] Paul: "allow the user to go through mountains - we already have a tunnelling tool", then
     *  "let the user choose though sometimes they might just want to go up a hill" (DESIGN.md 3.4, 10 REQ tunnels): a
     *  stretch of a line marked to go through the hill, its two ends clicked on the line as a platform's. */
    struct Stretch
    {
        GP mA, mB;
        bool mHasB = false;
        U32 mStamp = 0;
    };
    typedef Stretch Tunnel;
    /** [TRACK TOOL CUTTINGS 2026-11-17] Paul approved real cuttings, "yes IF the user specifies that" (DESIGN.md 3.6, 10 REQ
     *  cuttings): a stretch of a line where the region's ground is lowered to the rail's level, marked as a tunnel is. */
    typedef Stretch Cutting;
    enum EMode { MODE_LINE = 0, MODE_BRANCH = 1, MODE_STATION = 2, MODE_SIGNAL = 3, MODE_TURNTABLE = 4, MODE_TUNNEL = 5, MODE_CUTTING = 6 };
    enum ERefKind { REF_LINE_START, REF_LINE_CLICK, REF_STATION_A, REF_STATION_B, REF_SIGNAL, REF_TUNNEL_A, REF_TUNNEL_B, REF_CUTTING_A,
                    REF_CUTTING_B };
    struct Ref { ERefKind mKind; size_t mItem; size_t mPoint; };

    // ---- the plan (php/track_tool.php "plan": DESIGN.md 10 PLAN) ----
    struct PlanLine
    {
        std::string mId;
        bool mClosed = false;
        std::vector<LLVector3d> mP;   // every 1 m, grid metres, z = the rail head
        std::vector<F64> mS;          // chainage of each mP, metres along the line
    };
    struct PlanMark
    {
        LLVector3d mAt;               // grid metres
        std::string mText;
        LLColor4 mColour;
    };
    /** Lines to draw: v holds pairs of ends (grid metres), c one colour per pair. */
    struct Strokes
    {
        std::vector<LLVector3d> v;
        std::vector<LLColor4> c;
    };
    struct Plan
    {
        bool mOk = false;
        std::vector<PlanLine> mLines;
        Strokes mRails;       // the two rails, coloured by what carries them
        Strokes mThin;        // sleepers' ends, the legs of banks and viaducts
        Strokes mOutline;     // platforms, points, buffer stops, signals
        std::vector<PlanMark> mLabels;     // station names, points names
        std::vector<PlanMark> mProblems;   // a mark at each problem's place, with its words
        std::string mSummary;
        std::string mProblemText;
        bool mRefused = false;
        LLSD mTunnels;        // [TUNNELS] PLAN layout.tunnels: each laid tunnel as fitted to the hill (DESIGN.md 10)
        LLSD mSuggestions;    // [TUNNELS] PLAN suggestions: where a hill is in the way, Tunnel through or Climb the hill
        LLSD mCuttings;       // [CUTTINGS] PLAN layout.cuttings: each cutting as fitted to the hill (DESIGN.md 3.6)
        LLSD mCutSuggestions; // [CUTTINGS] PLAN cut_suggestions: hills with no tunnel option, Dig a cutting or Climb the hill
        Strokes mCutEdges;    // [CUTTINGS] the dashed tops of a cutting's sides (DESIGN.md 3.6 "Window")
        void clear() { *this = Plan(); }
    };

    // ---- a job's built items (DESIGN.md 10 ITEM) ----
    struct ItemObject { std::string mName; LLVector3 mRoot; };
    struct Item
    {
        LLUUID mRegion, mItem, mFolder;
        std::string mRegionName;
        LLVector3 mRayEnd;
        std::vector<ItemObject> mObjects;
        bool mLaid = false;
        LLSD mTunnels;        // [TUNNELS] ITEM.tunnels: [{id, name, object, root, design, volumes}] (DESIGN.md 10)
    };
    /** [TUNNELS] After a part is saved: its tunnels opened (WolfTunnels boxes) and recorded (php/tunnels.php), then
     *  tunnels_saved. uuid_of(object name) -> the saved piece's uuid. "" = done, else the words. Coroutine only. */
    static std::string finishTunnels(const LLSD& layout, const Item& item, const std::function<LLUUID(const std::string&)>& uuid_of,
                                     const std::function<void(const std::string&)>& progress);
    /** [TUNNELS] Remove: a region's recorded tunnels closed (their boxes) and their records deleted, then tunnels_removed.
     *  "" = done; `rights` false when the resident may no longer change tunnels here (the pieces still go). Coroutine only. */
    static std::string removeTunnels(const LLSD& layout, const LLUUID& region, const LLSD& list, bool& rights);
    enum ESugChoice { SUG_CLIMB, SUG_TUNNEL, SUG_CUTTING };
    void onSuggestion(ESugChoice choice);
    /** The first hill choice not answered yet: a PLAN suggestions[] entry, else a cut_suggestions[] one with "cut_only" true
     *  and its cutting's ends as "a" / "b" (DESIGN.md 3.6). */
    LLSD pendingSuggestion() const;
    void openTunnelsHere(const LLSD& layout_id);

    // ---- [TRACK TOOL CUTTINGS 2026-11-17] cuttings (DESIGN.md 3.6) ----
    /** Where p lies along the drawn lines: the nearest line's index and how far along it (metres). */
    bool drawnChainage(const GP& p, S32& line, F64& s) const;
    /** Two marked stretches share some of one drawn line. */
    bool stretchesOverlap(const Stretch& x, const Stretch& y) const;
    /** A cutting that overlaps a marked tunnel (or a tunnel a cutting): the plain sentence, else "". */
    std::string overlapWords(const Stretch& s, bool is_cutting) const;
    /** A complete cutting at `at` merged with every other complete cutting it overlaps (DESIGN.md 3.6: "marks that overlap are
     *  one cutting"): true when any was joined. */
    bool mergeCuttings(size_t at);
    /** The dig or the fill of a layout's cutting in a region (a JOB, or its id): its progress in the window until done or failed
     *  (DESIGN.md 3.6 "Window"); `lead` is said before it (an earlier error that must stay). */
    void startGroundJob(const LLSD& job, const std::string& kind, const LLSD& layout, const LLUUID& region, const std::string& lead);
    static void groundJobCoro(LLHandle<LLFloater> h, U32 gen, U32 ground_gen);
    /** Dig again ("cut") / Put the ground back ("fill") for a layout's part in a region (DESIGN.md 3.6 new actions). */
    void groundAgain(const std::string& action, const LLSD& layout, const LLUUID& region);
    /** Your railways: the picked layout's cutting in this region in plain words ("" when it has none here). */
    std::string cuttingWords(const LLSD& layout, bool& failed, bool& laid_here) const;
    void onPickMine();

    // ---- the window's own question (Start over, Remove, forget missing pieces) ----
    void ask(const std::string& question, const std::string& yes, std::function<void()> on_yes);
    void endAsk();

    // drawing
    void changed();
    void startOver();
    void finishLine();
    void onMode();
    void onStationControls();
    void onTurntableControls();
    /** The end of a drawn line (not a loop; a branch's far end) within `within` of p: that end exactly, else false. */
    bool lineEnd(const GP& p, F64 within, GP& end) const;
    void onTakeOut();
    void onPickItem();
    void fillItemList();
    void rebuildFlat();
    GP toGrid(F32 x, F32 y) const;               // agent-region metres -> grid
    LLVector2 toRegion(const GP& p) const;        // grid -> agent-region metres
    /** The nearest point on any line (the last plan's line, else the drawn clicks) within `within` metres, or false. */
    bool onALine(const GP& p, F64 within, GP& snapped, S32 exclude_line = -1) const;
    bool planLineAtMouse(GP& out) const;   // [15 Nov] the plan's line drawn at its rail level, under the mouse
    U32 nextStamp() { return ++mStampSeq; }
    bool drawingEmpty() const;
    S32 usableLines() const;

    // the request
    /** DESIGN.md 10 REQ from the drawing; false with `why` (plain words) when it cannot be made. */
    bool buildRequest(LLSD& req, std::string& why) const;
    void sendPlan();
    void applyPlan(const LLSD& plan, Plan& out) const;

    // laying
    void onLay();
    void onJob(const LLSD& job);
    void startStatusPoll();
    static void statusCoro(LLHandle<LLFloater> h, U32 gen);
    void layHere();                               // rez the agent's region's item of mItems
    static void rezCoro(LLHandle<LLFloater> h, U32 gen, Item item, LLSD layout, bool rez);
    void lookAgain();
    void takeAwayPlaced();
    void afterSaved(const LLSD& layout);
    void replaceOld(const LLSD& old_id);
    void partsMessage(const std::string& lead);
    /** Match the item's objects with new root objects of this agent in its region (a multiset: each expected entry takes one
     *  distinct object, name equal and root within MATCH_M); asks for names not known yet. -> how many matched. */
    size_t matchPieces(const Item& item, std::vector<std::pair<std::string, LLUUID>>& matched, bool by_position) const;
    static std::vector<Item> readItems(const LLSD& items);
    std::set<LLUUID> laidByOthers() const;
    /** How many of this agent's root objects stand at the item's roots (within MATCH_M), names not checked. */
    size_t piecesNear(const Item& item) const;

    // your railways
    void onMineCheck();
    void refreshMine();
    void fillMineList();
    LLSD pickedLayout() const;          // the picked row's LAYOUT, or undefined
    void onShow();
    /** Put a stored REQ's drawing (lines, stations, signals, turntables) into the window's drawing. */
    void loadDrawing(const LLSD& req);
    /** A full REQ (ground and others gathered now) for a stored drawing, the window's own drawing left as it was. */
    bool requestFor(const LLSD& stored, const std::vector<LLUUID>& own_laid, LLSD& req, std::string& why);
    void onChange();
    void onLayPart();
    void onRemove();
    /** Delete exactly `uuids` (the objects of `layout` in the agent's region) and then send "removed" for that region. */
    /** What one nearby delete did (nearbyStep). */
    struct NearResult
    {
        size_t taken = 0, stayed = 0, out_of_reach = 0;
        std::vector<LLUUID> gone;
    };
    static bool nearbyStep(const std::function<WolfFloaterTrack*()>& self, const LLUUID& region, const std::vector<LLUUID>& uuids,
                           NearResult& out);
    /** Take away what was placed (a lay whose pieces did not all appear): nearby only, nothing recorded. */
    void takeAwayPart(const LLUUID& region, const std::vector<LLUUID>& uuids);
    /** Remove (and a Change's take-away of the old railway): one removal of `layout`'s part in `region` (removalCoro). */
    void startRemoval(const LLSD& layout, const LLUUID& region, bool replacing, bool nearby_first);
    static void removalCoro(LLHandle<LLFloater> h, U32 gen, LLSD layout, LLUUID region, bool replacing, bool nearby_first);
    void sendRemoved(const LLSD& layout, const LLUUID& region, bool replacing);

    // transport (WolfCaves::serviceRequest, as wolffloaterbuildinggen.cpp requestCoro)
    typedef std::function<void(WolfFloaterTrack*, bool ok, const LLSD& reply, const std::string& error)> reply_fn_t;
    void post(const std::string& action, LLSD body, reply_fn_t then);
    static void postCoro(LLHandle<LLFloater> h, std::string action, LLSD body, reply_fn_t then);
    /** One POST from inside a coroutine; false with the plain-words `error`. */
    static bool callNow(const std::string& action, LLSD body, LLSD& reply, std::string& error);

    // camera and pen (wolffloaterlifts.cpp)
    void lookFromAbove();
    void giveCameraBack();
    bool penAllowed() const;
    void keepPen();

    void updateControls();
    void relayout();
    void setTextIfChanged(LLTextBox* box, const std::string& text);
    std::string regionName(const LLUUID& id) const;
    bool busy() const { return mLaying || mRezzing || mRemoving; }

    // controls
    LLTextBox *mHint = nullptr, *mSummary = nullptr, *mProblems = nullptr, *mStatus = nullptr, *mAskText = nullptr;
    LLRadioGroup* mMode = nullptr;
    LLButton *mUndoBtn = nullptr, *mFinishBtn = nullptr, *mStartOverBtn = nullptr, *mTopViewBtn = nullptr;
    LLButton *mAskYes = nullptr, *mAskNo = nullptr;
    LLComboBox *mSide = nullptr, *mStyle = nullptr;
    LLLineEditor* mName = nullptr;
    LLPanel* mTTPanel = nullptr;
    LLPanel* mSugPanel = nullptr;      // [TUNNELS] a hill is in the way: Tunnel through / Climb the hill
    LLTextBox* mSugText = nullptr;
    LLTextBox* mSugTunnelText = nullptr;  // the two choices' figures (updateControls)
    LLTextBox* mSugClimbText = nullptr;
    LLButton *mSugTunnelBtn = nullptr, *mSugClimbBtn = nullptr;
    LLTextBox* mSugCutText = nullptr;     // [CUTTINGS] the third choice, Dig a cutting (DESIGN.md 3.6)
    LLButton* mSugCutBtn = nullptr;
    LLRect mSugRowRect[3][2];             // [CUTTINGS] the three rows' button and text as in the XUI (a cutting-only hill
                                          // moves Dig a cutting and Climb the hill up into the first two)
    LLButton* mGroundBtn = nullptr;       // [CUTTINGS] Dig again / Put the ground back after a failed dig or fill
    LLComboBox* mTTSize = nullptr;
    LLSpinCtrl *mTTRoads = nullptr, *mTTSpread = nullptr;
    LLCheckBoxCtrl* mTTShed = nullptr;
    LLTextBox* mTTTurns = nullptr;
    LLScrollListCtrl* mItemsList = nullptr;
    LLButton* mTakeOutBtn = nullptr;
    LLButton *mLayBtn = nullptr, *mNextPartBtn = nullptr, *mLookAgainBtn = nullptr, *mTakeAwayBtn = nullptr;
    LLProgressBar* mProgress = nullptr;
    LLCheckBoxCtrl* mMineCheck = nullptr;
    LLPanel* mMinePanel = nullptr;
    LLScrollListCtrl* mMineList = nullptr;
    LLButton *mShowBtn = nullptr, *mChangeBtn = nullptr, *mLayPartBtn = nullptr, *mRemoveBtn = nullptr;

    // the drawing
    std::vector<Line> mLines;
    std::vector<Station> mStations;
    std::vector<Signal> mSignals;
    std::vector<Turntable> mTurntables;
    std::vector<Tunnel> mTunnels;      // [TUNNELS]
    S32 mCurTunnel = -1;               // a tunnel waiting for its other end
    std::vector<Cutting> mCuttings;    // [CUTTINGS]
    S32 mCurCutting = -1;              // a cutting waiting for its other end
    std::set<std::string> mSugHidden;  // suggestions answered "Climb the hill" (line + where), never sent
    S32 mEditTurntable = -1;           // the turntable the turntable controls change
    S32 mCurLine = -1;                 // the line the next click adds to (-1: the next click starts one)
    S32 mCurStation = -1;              // a station waiting for its platform's end
    S32 mEditStation = -1;             // the station the Station controls change
    U32 mStampSeq = 0;
    std::vector<LLVector2> mFlat;      // the pen's markers: every point, in the agent's region's metres
    std::vector<Ref> mFlatRefs;
    U64 mFlatRegion = 0;

    // the plan
    bool mPlanDirty = false;
    F64 mChangedAt = 0.0;
    bool mPlanInFlight = false;
    U32 mPlanSerial = 0;               // bumped on every change: a reply for an older drawing is dropped
    Plan mPlan;
    LLSD mPlannedReq;                  // the REQ the shown plan answers (Lay sends it again; the server plans it again)
    Plan mShown;                       // Your railways > Show: a laid railway's plan, drawn apart from the drawing
    std::string mShownName;
    LLSD mShownId;

    // laying
    bool mLaying = false;              // a job is queued or running
    LLSD mJobId;
    U32 mGen = 0;                      // bumped on close: an older coroutine stops at its next wake-up
    bool mPolling = false;
    LLSD mLayLayout;                   // the layout the items belong to
    std::vector<Item> mItems;          // its built items (a job's result, or "get")
    LLSD mReplaces;                    // Change: the layout the new one replaces once laid everywhere
    std::vector<LLUUID> mReplaceLaid;  // ... its pieces (not counted as someone else's railway in the plan)
    std::string mReplaceName;
    bool mRezzing = false;
    LLUUID mRezRegion;
    std::string mRezAlert;             // the region's words when it refused the rez
    std::map<std::string, std::set<LLUUID>> mRemovedEarlier;   // layout id -> pieces this window took away (this session)
    std::set<std::string> mLaidThisSession;   // layouts laid in this session (their pieces may not be saved by the region yet)
    std::set<LLUUID> mBefore;          // the root objects there were before the rez (never taken as a piece)
    std::set<LLUUID> mNotOurs;         // pieces of the resident's other railways here (never taken as a piece)
    bool mItemsFromDrawing = false;    // mItems came from laying the drawing (laying them all clears the drawing)
    U32 mLaySerial = 0;                // mPlanSerial when the drawing was last laid
    bool mLayAnyway = false;           // the resident said: lay this drawing as another railway
    std::string mLaidNote;             // said after "laid" (the turntable switch-on words), and after the old railway goes
    bool mPartialShown = false;        // "Only N of M pieces appeared" with Look again / Take away what was placed
    std::vector<std::pair<std::string, LLUUID>> mMatched;   // name, uuid of every piece found
    Item mPartial;                     // the item whose pieces did not all appear

    // removing
    bool mRemoving = false;

    // your railways
    LLSD mMine;                        // "open" reply layouts
    LLUUID mMineRegion;
    bool mMayLay = true;
    std::string mNoLayReason;
    bool mMayTunnel = false;           // [TUNNELS] "open" access.tunnel: the resident may make tunnels in this region
    std::string mNoTunnelReason;
    bool mMayCut = false;              // [CUTTINGS] "open" access.cut: the resident may dig anywhere in this region
    std::string mNoCutReason;
    std::map<LLUUID, std::string> mRegionNames;

    // [CUTTINGS] the dig / fill job being followed (one at a time; a newer one takes its place)
    LLSD mGroundJob;                   // its id, undefined when none
    std::string mGroundKind;           // "cut" | "fill"
    LLSD mGroundLayout;
    LLUUID mGroundRegion;
    std::string mGroundLead;           // words said before its progress (an earlier error that stays)
    std::string mGroundSaid;           // the progress last shown (the status is only set when it changes)
    U32 mGroundGen = 0;                // bumped when a newer job takes its place: the older coroutine stops
    bool mGroundPolling = false;
    LLSD mGroundRetry;                 // after a failure: {action "cut" | "fill", layout, region} for Dig again / Put the ground back
    bool mStatusError = false;         // the status line shows an error now

    // the question
    bool mAsking = false;
    std::function<void()> mOnYes;

    bool mPenWanted = false;
    bool mStartPending = false;
    bool mCamSaved = false, mCamWasOnAvatar = true;
    LLVector3d mCamPos, mCamFocus, mCamFocusSet;
};

#endif // WOLF_FLOATER_TRACK_H
