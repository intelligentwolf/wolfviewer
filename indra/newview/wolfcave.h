/**
 * @file wolfcave.h
 * @brief WolfViewer: the Cave Builder — caves dug into the terrain from the Paint window.
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

#ifndef WOLF_CAVE_H
#define WOLF_CAVE_H

#include <string>
#include <utility>
#include <vector>

#include "llfloater.h"
#include "llsingleton.h"
#include "lltool.h"
#include "v3math.h"
#include "wolfcavelights.h"

class LLButton;
class LLComboBox;
class LLLineEditor;
class LLScrollListCtrl;
class LLSliderCtrl;
class LLSpinCtrl;
class LLTextBox;
class LLTextureCtrl;
class WolfDigProgress;

// [WOLF CAVE 2026-10-12] Paul: "i want to have a way to make caves with terrain could we do that
// with the paint on terrain tool? expand it to make caves?". Port of wolfstorm
// js/world/wolf_caves.js (the engine) and js/ui/floaters/floater_wolf_caves.js (the window):
// the same record (wolfstorm php/caves.php), the same generator (rust_proxy wolfcave.rs, POST
// /cave_mesh on WolfGrid::MESH_API_BASE), the same terrain floods. Keep them in step.
//
// A heightmap cannot overhang, so a cave is MESH + TERRAIN: the ground under it is flattened to
// the floor with ordinary ModifyLand floods (the user's own terraform rights, checked by the
// region), and a rock-shell mesh the proxy made in the user's Objects folder is rezzed over it.
// Every viewer draws that. Wolf Territories ONLY (Paul 10-12: "cave builder is just for wolf"):
// nothing is sent, drawn or reachable unless onWolf().
class WolfCaves : public LLSingleton<WolfCaves>
{
    LLSINGLETON(WolfCaves);
    ~WolfCaves();

public:
    static const char* API_URL;

    // [WolfViewer 2026-10-14 v2] The box design. Paul: "how about you draw a box that shows where
    // the cave is going to be in 3d you then draw the other stuff then generate it ... also default
    // to a cave texture". It replaces passages/chambers/entrances completely.
    // [WolfViewer 2026-10-15 v3] Paul: "i draw a tunnel and it just makes a hollow cave". The box is
    // now SOLID ROCK; the air is the tunnels and caverns drawn through it.
    // Source: firestorm/wolfstorm/tools/CAVE_DESIGN_V3.md "Stored design" (field names and limits).
    /** The block of rock: centre x,y; w along its own x, d along its own y; rot degrees
     *  counter-clockwise from east; cave floor z (absolute); rock height h above the floor
     *  (the hill's top is z + h). CAVE_DESIGN_V3.md: clients default a new box's h to 8. */
    struct Box { F32 mX = 0.f, mY = 0.f, mW = 12.f, mD = 8.f, mRot = 0.f, mZ = 20.f, mH = 8.f; };
    /** A polyline x0,y0,x1,y1... at the box floor, mW wide. */
    struct Tunnel { std::vector<F32> mP; F32 mW = 3.f; };
    /** [WolfViewer 2026-10-15 v3] A rounded chamber on a tunnel: centre x,y, radius r, height h. */
    struct Cavern { F32 mX = 0.f, mY = 0.f, mR = 5.f, mH = 5.f; };
    /** Where a tunnel meets the box wall and the rock opens to the outside. */
    struct Door { F32 mX = 0.f, mY = 0.f; };
    /** [WolfViewer 2026-10-15 v3] What Undo puts back for a 'b' step: the box before it, and - for
     *  a move, which carries everything inside along - the tunnels, caverns and doorways before it. */
    struct BoxUndo
    {
        bool mHadBox = false;
        Box mBox;
        bool mMoved = false;
        std::vector<Tunnel> mTunnels;
        std::vector<Cavern> mCaverns;
        std::vector<Door> mDoors;
    };
    struct Design
    {
        bool mHasBox = false;
        Box mBox;
        std::vector<Tunnel> mTunnels;
        std::vector<Cavern> mCaverns;
        std::vector<Door> mDoors;
        F32 mRoof = 1.5f;           // rock thickness over every ceiling
        LLUUID mRock;               // the rock texture (CAVE_DESIGN_V3.md default "Terrain Rock")
        F32 mRockScale = 4.f;
        F32 mRoughness = 0.5f;
        S32 mSeed = 0;
        // What Undo takes back, not stored: 'b' box drawn or box handle edit, 't' tunnel,
        // 'c' cavern, 'd' doorway. A redrawn or resized box replaced the one before it, so Undo
        // puts that one back (mBoxUndo, one entry per 'b').
        std::vector<char> mOrder;
        std::vector<BoxUndo> mBoxUndo;
    };
    // Source: CAVE_DESIGN_V3.md limits.
    static constexpr F32 BOX_SIDE_MIN = 3.f, BOX_SIDE_MAX = 128.f;
    static constexpr F32 BOX_H_MIN = 3.f, BOX_H_MAX = 30.f;
    static constexpr F32 BOX_Z_MIN = -200.f, BOX_Z_MAX = 4096.f;
    static constexpr F32 ROOF_MIN = 0.5f, ROOF_MAX = 6.f;
    static constexpr F32 ROOM_UNDER_ROOF_M = 2.5f;   // h - roof >= 2.5
    static constexpr F32 TUNNEL_W_MIN = 2.f, TUNNEL_W_MAX = 40.f;
    static constexpr F32 TUNNEL_OUTSIDE_M = 1.f;     // a tunnel point at most 1 m outside the box
    static constexpr S32 MAX_TUNNELS = 16, MAX_TUNNEL_POINTS = 256, MAX_ALL_TUNNEL_POINTS = 2048;
    static constexpr F32 CAVERN_R_MIN = 2.f, CAVERN_R_MAX = 30.f;
    static constexpr F32 CAVERN_H_MIN = 2.5f, CAVERN_H_MAX = 20.f;
    static constexpr S32 MAX_CAVERNS = 16;
    static constexpr S32 MAX_DOORS = 16;
    static constexpr F32 DOOR_REACH_M = 1.f;   // a door within 1 m of the wall AND (half-width +) 1 m of a tunnel
    static constexpr F32 ROCK_SCALE_MIN = 0.5f, ROCK_SCALE_MAX = 64.f;
    /** CAVE_DESIGN_V3.md: "Terrain Rock" (OpenSim RegionSettings.cs:116), the default; and "rocks". */
    static const LLUUID& terrainRockId();
    static const LLUUID& rocksId();
    /** The four footprint corners (x,y pairs, counter-clockwise), region metres. */
    static void boxCorners(const Box& b, F32 out[8]);
    /** [WolfViewer 2026-10-15 v3] Box signed distance at x,y: > 0 inside (metres to the nearest
     *  wall), < 0 outside (minus the distance to the footprint). CAVE_DESIGN_V3.md "sb". */
    static F32 boxSigned(const Box& b, F32 x, F32 y);
    /** Why a doorway may not go at x,y; empty = it may. */
    static std::string doorProblem(const Design& d, F32 x, F32 y);
    /** [WolfViewer 2026-10-15 v3] Why a cavern of radius r may not be centred at x,y; empty = it may. */
    static std::string cavernProblem(const Design& d, F32 x, F32 y, F32 r);
    /** Why this design cannot be generated yet, in plain words; empty = it can be.
     *  `region_w` is the region's width (the whole footprint must be inside). */
    static std::string designProblem(const Design& d, F32 region_w);
    struct Cave
    {
        std::string mId;
        std::string mName = "Wolf Cave";
        std::string mLight = "dim";
        Design mDesign;
        LLSD mBuilt;                // php/caves.php "built" (undefined = never built)
        LLSD mZone;
        LLSD mOwner;
        // [WolfViewer 2026-10-15 v3] CAVE_DESIGN_V3.md "Fields caves.php sets": true when the saver
        // held dig rights. Never sent from here; a stored cave without it counts as false.
        bool mDig = false;
        // [WolfViewer 2026-10-15 v3] CAVE_DESIGN_V3.md "Caves from the old builder": a stored cave
        // whose design.v is not 3. It is listed, may be removed, may not be changed, and goes back
        // to caves.php exactly as it came (mRaw, the cave's LLSD as received).
        bool mOld = false;
        LLSD mRaw;
        // [WOLF CAVE LIGHTS 2026-10-23] undefined = no lights; {style, tone, objects:[every part of every lamp], at}
        // (php/caves.php "lights", wolfcavelights.cpp)
        LLSD mLights;
    };
    /** One face of the ghost, in REGION metres. */
    struct GhostFace { bool mRock = false; std::vector<LLVector3> mTris; };
    struct Preview
    {
        bool mReady = false;
        S32 mLi = 0;
        S32 mTargets = 0;
        // [WolfViewer 2026-10-15 v3] The summary's "dig" (CAVE_DESIGN_V3.md CaveOut.dig): whether
        // the generator dug the ground for this preview.
        bool mDig = false;
        std::vector<std::string> mWarnings;
        std::vector<GhostFace> mFaces;
    };

    /** Wolf Territories AND the region known to the grid's own database (the Paint record). */
    static bool onWolf();
    // [WOLF TUNNELS 2026-10-22] The cave builder's own service calls, shared with the Tunnel tool
    // (wolffloatertunnels.cpp) so both talk to the Wolf services the same way. Each only forwards to
    // the helper the cave builder itself uses (wolfcave.cpp wolfRequest / replyOf / caveMeshError).
    /** A request to a Wolf service carrying the session pair (GET when body is null, else JSON POST, no retry). */
    static LLSD serviceRequest(const std::string& url, const std::string* body, F32 timeout);
    /** HTTP status of a serviceRequest result, its JSON body in `reply`, the transport error in `transport`. */
    static S32 serviceReply(const LLSD& result, LLSD& reply, std::string& transport);
    /** The words for a failed mesh-generator answer (success:false words verbatim, a line per status otherwise). */
    static std::string meshServiceError(S32 status, const LLSD& reply, const std::string& transport);
    static Cave newCave();

    void refresh();
    bool fetching() const { return mFetching; }
    const std::vector<Cave>& caves() const { return mCaves; }
    const Cave* find(const std::string& id) const;
    U64 recordHandle() const { return mHandle; }

    /** Ask the generator for the ghost and the land impact (debounced by the caller). */
    void requestPreview(const Cave& cave);
    /** [WolfViewer 2026-10-15 v3] caves.php GET &rights=1 region "dig": "region" | "parcels" |
     *  "none" (empty until a proven fetch answered). */
    const std::string& digHint() const { return mDigHint; }
    /** CAVE_DESIGN_V3.md: a preview sends dig = true when the hint is "region" or "parcels". */
    bool previewDig() const { return mDigHint == "region" || mDigHint == "parcels"; }
    const Preview& preview() const { return mPreview; }
    U32 previewSerial() const { return mPreviewSerial; }
    const std::string& previewError() const { return mPreviewError; }
    // [WolfViewer 2026-10-14] Bumps mPreviewRequest too: a preview still in flight for the design
    // just thrown away (Undo to empty, Back, New cave, a teleport) must not land afterwards.
    void clearPreview() { mPreview = Preview(); mPreviewError.clear(); ++mPreviewSerial; ++mPreviewRequest; }

    /** Build (or rebuild: the old one is removed first) / remove. One job at a time. */
    void build(const Cave& cave);
    /** `forget`: the user agreed to forget a built cave whose rock this viewer cannot see. */
    void remove(const std::string& id, bool forget);
    bool busy() const { return mBusy; }
    /** [WOLF CAVE LIGHTS 2026-10-23] Where a built cave's lamps go (its tunnels, caverns and air, CAVE_DESIGN_V3.md "Generator
     *  rules" as wolfcave.rs Interior::at). */
    static std::vector<WolfCaveLights::Lamp> caveLamps(const Cave& cave, WolfCaveLights::EStyle style);
    /** [WOLF CAVE LIGHTS 2026-10-23] Light up (`lamps` placed, the old ones replaced) or Remove lights (`remove`). One job at a time. */
    void lights(const std::string& id, bool remove, std::vector<WolfCaveLights::Lamp> lamps, WolfCaveLights::EStyle style, bool warm, bool keep_unseen);
    /** [WolfViewer 2026-10-14] The cave the running (or last) job is about, so the window only
     *  leaves the editor when the job was about the cave being drawn. */
    const std::string& jobCave() const { return mJobCave; }
    /** [WolfViewer 2026-10-14] Stop the running job at the next safe point (between floods). */
    void cancel() { if (mBusy) mCancel = true; }
    bool cancelling() const { return mBusy && mCancel; }
    /** The job's progress line, and every failure since the window last looked (kept until shown). */
    const std::string& status() const { return mStatus; }
    /** [2026-10-23] How far the running job is, 0..1 from real counts (steps done, ground points flooded, lamps placed), or
     *  below 0 while a step cannot be counted (the dig plate, wolfdigprogress.cpp, then sweeps instead of a number). */
    F32 progress() const { return mProgress; }
    std::string takeError();

    /** The cave's rock in the agent's own region, if this viewer has it. */
    LLViewerObject* findRock(const Cave& cave) const;
    /** What the last finished job was, for the window (set when busy() goes false). */
    enum EResult { RESULT_NONE, RESULT_BUILT, RESULT_REMOVED, RESULT_FAILED };
    EResult takeResult() { EResult r = mResult; mResult = RESULT_NONE; return r; }
    /** May a job SEND to the region now? Wolf confirmed by the region itself, and still the
     *  region the record belongs to (wolfgrid.h: anything that sends uses the Confirmed gate). */
    bool sendOk() const;

private:
    void fetchCoro();
    bool storeCaves(const std::vector<Cave>& caves, std::string& error, S32* http_status = nullptr);
    bool putCave(const Cave& cave, std::string& error, S32* http_status = nullptr);
    bool sampleGround(const Design& d, LLSD& terrain, std::string& error) const;
    LLSD meshRequest(const Cave& cave, const LLSD& terrain, bool dig) const;
    bool postMesh(const std::string& mode, const LLSD& body, LLSD& reply, std::string& error) const;
    bool rezExact(const LLUUID& item_id, const LLUUID& folder_id, const LLVector3& ray_end, const LLVector3& root, LLUUID& rock, std::string& error);
    /** Flatten [x, y, from, to, ...] to `to` (dig) or `from` (put back); the points a limit held. */
    std::vector<LLSD> shape(const LLSD& targets, bool to_target, const std::string& what);
    LLSD zoneFor(const Cave& cave) const;
    void buildCoro(Cave cave);
    void removeCoro(std::string id, bool then_build, Cave next, bool forget);
    void lightsCoro(std::string id, bool remove, std::vector<WolfCaveLights::Lamp> lamps, WolfCaveLights::EStyle style, bool warm, bool keep_unseen);
    bool removeOne(const Cave& cave, bool forget, std::string& error);
    bool dropCave(const std::string& id, std::string& error);
    static LLSD toLLSD(const Cave& c);
    static Cave fromLLSD(const LLSD& c);
    void fail(const std::string& msg);
    /** [WolfViewer 2026-10-14] The agent stands in the region the cave record was loaded for. */
    bool recordHere() const;

    std::vector<Cave> mCaves;
    std::string mRegionUuid;
    U64 mHandle = 0;
    S32 mVersion = 0;
    std::string mDigHint;   // [WolfViewer 2026-10-15 v3] caves.php GET &rights=1 region "dig"
    bool mFetching = false;
    bool mBusy = false;
    EResult mResult = RESULT_NONE;
    std::string mStatus;
    F32 mProgress = -1.f;                 // [2026-10-23] see progress()
    F32 mShapeBase = 0.f, mShapeSpan = 0.f;   // the share of the job a shape() call covers
    // [WolfViewer 2026-10-14] Every message of the job, not just the last (a later one used to
    // replace an earlier one before the window saw it). Cleared when a new job starts.
    std::vector<std::string> mErrors;
    std::string mJobCave;
    bool mCancel = false;
    Preview mPreview;
    std::string mPreviewError;
    U32 mPreviewSerial = 0;
    U32 mPreviewRequest = 0;
};

/** The pen on the ground (Source: wolfterrainpaint.cpp WolfToolTerrainPaint for the tool shape). */
class WolfToolCave : public LLTool, public LLSingleton<WolfToolCave>
{
    LLSINGLETON(WolfToolCave);
public:
    // [WolfViewer 2026-10-14 v2] The pens of the box flow. [WolfViewer 2026-10-15 v3] + the cavern
    // pen (CAVE_DESIGN_V3.md Flow 1-4).
    enum EPen { PEN_BOX, PEN_TUNNEL, PEN_CAVERN, PEN_DOOR };
    // With the pen off the tool stays current (LLTool::render only runs for the current tool, and it
    // draws the ghost) but hands every click to LLToolPie, so walking, selecting, touching and the
    // context menu work as usual (llviewerwindow.cpp handleAnyMouseClick has no fallback tool).
    bool handleAnyMouseClick(S32 x, S32 y, MASK mask, EMouseClickType clicktype, bool down) override;
    bool handleScrollWheel(S32 x, S32 y, S32 clicks) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleDoubleClick(S32 x, S32 y, MASK mask) override;
    bool handleKey(KEY key, MASK mask) override;
    void handleDeselect() override;
    void onMouseCaptureLost() override;
    bool isAlwaysRendered() override { return true; }
    void render() override;

    /** The cave being edited (owned by the floater); null = nothing to draw. */
    void setEdit(WolfCaves::Cave* cave) { if (cave != mEdit) { dropDrag(); mLive.clear(); } mEdit = cave; }
    // A drag dropped here also lets go of the mouse: with the pen off the next mouse-up goes to
    // LLToolPie (handleAnyMouseClick), so this tool would never see it to let go itself.
    // CAVE_DESIGN_V3.md Flow 2: any other pen finishes a tunnel still being drawn.
    void setPen(EPen p) { finishTunnel(); dropDrag(); mPen = p; mHover = -1; }
    EPen pen() const { return mPen; }
    void setDrawing(bool on) { if (!on) { finishTunnel(); dropDrag(); mHover = -1; } mDrawing = on; }
    bool drawing() const { return mDrawing; }
    /** Set when the design changed (the floater re-previews). */
    bool takeChanged() { bool c = mChanged; mChanged = false; return c; }
    std::string takeMessage() { std::string m = mMessage; mMessage.clear(); return m; }
    /** [WolfViewer 2026-10-15 v3] A box handle is being dragged: the window's spinners follow live. */
    bool boxDragging() const { return mDrag == DRAG_CORNER || mDrag == DRAG_SIDE || mDrag == DRAG_MOVE || mDrag == DRAG_TOP; }
    void finishTunnel();
    /** [WolfViewer 2026-10-15 v3] The tunnel being drawn as finishTunnel would keep it (a point
     *  under 0.5 m from the last kept one dropped); false when it would not be kept (under 2 points). */
    bool liveTunnel(WolfCaves::Tunnel& out) const;
    /** Throw away the tunnel being drawn; true if there was one (Undo takes that back first). */
    bool dropLive() { const bool had = !mLive.empty(); mLive.clear(); return had; }
    static F32 tunnelWidth();
    /** [WolfViewer 2026-10-15 v3] Height of the next cavern (CAVE_DESIGN_V3.md caverns h [2.5, 20]). */
    static F32 cavernHeight();

private:
    // [WolfViewer 2026-10-15 v3] What a press-drag is doing. Handles: 0-3 the corners (boxCorners
    // order), 4-7 the side middles (4 = -y, 5 = +x, 6 = +y, 7 = -x in the box's own frame), 8 the top.
    enum EDrag { DRAG_NONE, DRAG_NEW_BOX, DRAG_CORNER, DRAG_SIDE, DRAG_MOVE, DRAG_TOP, DRAG_CAVERN };
    static constexpr S32 HANDLE_TOP = 8, HANDLE_COUNT = 9;
    bool hit(S32 x, S32 y, F32& rx, F32& ry) const;
    /** The pointer on the level plane at height z, region metres. */
    bool planeHit(S32 x, S32 y, F32 z, F32& rx, F32& ry) const;
    /** Handle i of the box, region metres (corners and sides at the floor, the top at z + h). */
    static LLVector3 handlePos(const WolfCaves::Box& b, S32 i);
    /** The handle under the pointer (within HANDLE_PICK_PX on screen), or -1. */
    S32 pickHandle(S32 x, S32 y) const;
    void dragBox(S32 x, S32 y);
    void finishHandleDrag();
    void finishBox();
    void finishCavern();
    /** Let go of any drag; a box handle drag not finished puts the box (and what it moved) back. */
    void dropDrag();
    WolfCaves::Cave* mEdit = nullptr;
    EPen mPen = PEN_BOX;
    bool mDrawing = false;
    std::vector<F32> mLive;     // the tunnel being drawn
    EDrag mDrag = DRAG_NONE;
    F32 mAX = 0.f, mAY = 0.f, mBX = 0.f, mBY = 0.f;   // new box: corner A to corner B
    S32 mHandle = -1;           // the handle being dragged
    S32 mHover = -1;            // the handle under the pointer (drawn highlighted)
    WolfCaves::BoxUndo mDragStart;   // the box and everything in it when the handle drag began
    F32 mGrabX = 0.f, mGrabY = 0.f;  // where a move grabbed the floor plane
    S32 mPressY = 0;            // top handle: the pointer's screen y at the press
    F32 mMetresPerPixel = 0.f;  // top handle: metres of height per screen pixel
    F32 mCavX = 0.f, mCavY = 0.f, mCavR = 0.f, mCavMaxR = 0.f;   // the cavern being dragged out
    bool mHaveCursor = false;
    F32 mCursorX = 0.f, mCursorY = 0.f;
    bool mChanged = false;
    std::string mMessage;
};

/** The Cave Builder window (floater_wolf_caves.xml), opened from Paint the Ground > Caves... */
class WolfFloaterCaves : public LLFloater
{
public:
    WolfFloaterCaves(const LLSD& key);
    ~WolfFloaterCaves();
    bool postBuild() override;
    void draw() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;

private:
    void rebuildList();
    void showEditor();
    /** [WolfViewer 2026-10-14 v2] The box spinners from mCave (after a drag, an Undo, a load). */
    void showBox();
    void readBox();
    void readControls();
    void schedulePreview();
    /** Why Generate cannot be pressed now, in words; empty = it can. */
    std::string whyNotGenerate() const;
    void onNew();
    void onEdit();
    void onRemove();
    void onListPick();
    // [WOLF CAVE LIGHTS 2026-10-23]
    void onLightUp();
    void onRemoveLights();
    /** The light choice for the picked cave: its own lights' style, else the last choice, else torches (natural rock). */
    void showLightChoice();
    /** [WolfViewer 2026-10-15 v3] The cave picked in the list, or null. */
    const WolfCaves::Cave* picked() const;
    void onBuild();
    void onBack();
    void onPen(WolfToolCave::EPen p);
    void onUndo();
    void onRockCombo();
    void onCancel();
    void setStatus(const std::string& msg, bool error);
    void stopTool();
    void showTool();

    LLScrollListCtrl* mList = nullptr;
    LLButton *mNew = nullptr, *mEdit = nullptr, *mRemove = nullptr, *mBuild = nullptr, *mBack = nullptr, *mUndo = nullptr, *mCancel = nullptr;
    LLButton *mPenBox = nullptr, *mPenTunnel = nullptr, *mPenCavern = nullptr, *mPenDoor = nullptr;
    LLLineEditor* mName = nullptr;
    LLSpinCtrl *mBoxW = nullptr, *mBoxD = nullptr, *mBoxH = nullptr, *mBoxTurn = nullptr, *mBoxFloor = nullptr;
    LLSliderCtrl *mWidth = nullptr, *mCavernH = nullptr, *mRoof = nullptr, *mRough = nullptr;
    LLComboBox *mRock = nullptr, *mLight = nullptr;
    LLComboBox *mLightsStyle = nullptr, *mLightsTone = nullptr;   // [WOLF CAVE LIGHTS 2026-10-23]
    LLButton *mLightsUp = nullptr, *mLightsOff = nullptr;
    std::string mLightPickShown;
    LLTextureCtrl* mRockTex = nullptr;
    LLTextBox *mCounts = nullptr, *mStats = nullptr, *mDig = nullptr, *mStatus = nullptr, *mWhy = nullptr;
    bool mEditing = false;
    WolfCaves::Cave mCave;
    F64 mPreviewAt = 0.0;
    U32 mShownPreview = 0;
    size_t mShownCaves = (size_t)-1;
    U64 mListHandle = 0;      // the region the list was built for
    U64 mShownRegion = 0;     // the agent's region the window last showed
    bool mWasBusy = false;
    std::string mLastStatus;
    std::string mShownWhy = "-";   // the reason line last shown (set again only when it changes)
    // [WolfViewer 2026-10-15 v3] Generate finished a tunnel still being drawn (CAVE_DESIGN_V3.md
    // Flow 2): the preview is worked out again and Generate goes on by itself once it is ready.
    bool mGenerateWhenReady = false;
    std::string mShownDig = "-";   // [WolfViewer 2026-10-15 v3] the dig line last shown
    std::string mShownHint;        // [WolfViewer 2026-10-15 v3] the dig hint the last preview was asked with
    // [WOLF BURROW 2026-10-23] the painted window (wolfburrow.cpp): the groups' plates (heading drawn by its own XUI text, so
    // none here) and the buttons' faces by weight, worked out once in postBuild
    void paintBurrow();
    std::vector<std::vector<LLView*>> mBurrowGroups;
    std::vector<std::pair<LLButton*, S32>> mBurrowButtons;
    WolfDigProgress* mDigPlate = nullptr;   // [2026-10-23] the painted working plate over the status lines (wolfdigprogress.cpp)
};

#endif // WOLF_CAVE_H
