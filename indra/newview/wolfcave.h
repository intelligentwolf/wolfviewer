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

class LLButton;
class LLComboBox;
class LLLineEditor;
class LLScrollListCtrl;
class LLSliderCtrl;
class LLSpinCtrl;
class LLTextBox;
class LLTextureCtrl;

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
    // Source: firestorm/wolfstorm/tools/CAVE_DESIGN_V2.md "Stored design" (field names and limits).
    /** The cave's main room: centre x,y; w along its own x, d along its own y; rot degrees
     *  counter-clockwise from east; floor z (absolute); clear height h. */
    struct Box { F32 mX = 0.f, mY = 0.f, mW = 12.f, mD = 8.f, mRot = 0.f, mZ = 20.f, mH = 4.f; };
    /** A polyline x0,y0,x1,y1... at the box floor, mW wide. */
    struct Tunnel { std::vector<F32> mP; F32 mW = 3.f; };
    /** Where the rock opens to the outside. */
    struct Door { F32 mX = 0.f, mY = 0.f; };
    struct Design
    {
        bool mHasBox = false;
        Box mBox;
        std::vector<Tunnel> mTunnels;
        std::vector<Door> mDoors;
        F32 mRoof = 1.5f;           // rock thickness over the ceiling
        LLUUID mRock;               // the rock texture (CAVE_DESIGN_V2.md default "Terrain Rock")
        F32 mRockScale = 4.f;
        F32 mRoughness = 0.5f;
        S32 mSeed = 0;
        // What Undo takes back, not stored: 'b' box drawn, 't' tunnel, 'd' doorway. A redrawn box
        // replaced the one before it, so Undo puts that one back (mBoxUndo).
        std::vector<char> mOrder;
        std::vector<std::pair<bool, Box>> mBoxUndo;
    };
    // Source: CAVE_DESIGN_V2.md limits.
    static constexpr F32 BOX_SIDE_MIN = 3.f, BOX_SIDE_MAX = 128.f;
    static constexpr F32 BOX_H_MIN = 2.5f, BOX_H_MAX = 20.f;
    static constexpr F32 BOX_Z_MIN = -200.f, BOX_Z_MAX = 4096.f;
    static constexpr F32 TUNNEL_W_MIN = 2.f, TUNNEL_W_MAX = 40.f;
    static constexpr S32 MAX_TUNNELS = 16, MAX_TUNNEL_POINTS = 256, MAX_ALL_TUNNEL_POINTS = 2048;
    static constexpr S32 MAX_DOORS = 16;
    static constexpr F32 DOOR_REACH_M = 1.f;   // a door within (half-width +) 1 m of a tunnel or the wall
    /** CAVE_DESIGN_V2.md: "Terrain Rock" (OpenSim RegionSettings.cs:116), the default; and "rocks". */
    static const LLUUID& terrainRockId();
    static const LLUUID& rocksId();
    /** The four footprint corners (x,y pairs, counter-clockwise), region metres. */
    static void boxCorners(const Box& b, F32 out[8]);
    /** A tunnel's clear height above the floor: min(h, w * 0.9 + 1.0) (CAVE_DESIGN_V2.md). */
    static F32 tunnelCeiling(const Box& b, F32 w) { return llmin(b.mH, w * 0.9f + 1.f); }
    /** Why a doorway may not go at x,y; empty = it may. */
    static std::string doorProblem(const Design& d, F32 x, F32 y);
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
    };
    /** One face of the ghost, in REGION metres. */
    struct GhostFace { bool mRock = false; std::vector<LLVector3> mTris; };
    struct Preview
    {
        bool mReady = false;
        S32 mLi = 0;
        S32 mTargets = 0;
        std::vector<std::string> mWarnings;
        std::vector<GhostFace> mFaces;
    };

    /** Wolf Territories AND the region known to the grid's own database (the Paint record). */
    static bool onWolf();
    static Cave newCave();

    void refresh();
    bool fetching() const { return mFetching; }
    const std::vector<Cave>& caves() const { return mCaves; }
    const Cave* find(const std::string& id) const;
    U64 recordHandle() const { return mHandle; }

    /** Ask the generator for the ghost and the land impact (debounced by the caller). */
    void requestPreview(const Cave& cave);
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
    /** [WolfViewer 2026-10-14] The cave the running (or last) job is about, so the window only
     *  leaves the editor when the job was about the cave being drawn. */
    const std::string& jobCave() const { return mJobCave; }
    /** [WolfViewer 2026-10-14] Stop the running job at the next safe point (between floods). */
    void cancel() { if (mBusy) mCancel = true; }
    bool cancelling() const { return mBusy && mCancel; }
    /** The job's progress line, and every failure since the window last looked (kept until shown). */
    const std::string& status() const { return mStatus; }
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
    LLSD meshRequest(const Cave& cave, const LLSD& terrain) const;
    bool postMesh(const std::string& mode, const LLSD& body, LLSD& reply, std::string& error) const;
    bool rezExact(const LLUUID& item_id, const LLUUID& folder_id, const LLVector3& ray_end, const LLVector3& root, LLUUID& rock, std::string& error);
    /** Flatten [x, y, from, to, ...] to `to` (dig) or `from` (put back); the points a limit held. */
    std::vector<LLSD> shape(const LLSD& targets, bool to_target, const std::string& what);
    LLSD zoneFor(const Cave& cave) const;
    void buildCoro(Cave cave);
    void removeCoro(std::string id, bool then_build, Cave next, bool forget);
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
    bool mFetching = false;
    bool mBusy = false;
    EResult mResult = RESULT_NONE;
    std::string mStatus;
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
    // [WolfViewer 2026-10-14 v2] The three pens of the box flow (CAVE_DESIGN_V2.md Flow 1-3).
    enum EPen { PEN_BOX, PEN_TUNNEL, PEN_DOOR };
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
    void setEdit(WolfCaves::Cave* cave) { mEdit = cave; mLive.clear(); dropDrag(); }
    // A box drag dropped here also lets go of the mouse: with the pen off the next mouse-up goes to
    // LLToolPie (handleAnyMouseClick), so this tool would never see it to let go itself.
    void setPen(EPen p) { finishTunnel(); dropDrag(); mPen = p; }
    EPen pen() const { return mPen; }
    void setDrawing(bool on) { if (!on) { finishTunnel(); dropDrag(); } mDrawing = on; }
    bool drawing() const { return mDrawing; }
    /** Set when the design changed (the floater re-previews). */
    bool takeChanged() { bool c = mChanged; mChanged = false; return c; }
    std::string takeMessage() { std::string m = mMessage; mMessage.clear(); return m; }
    void finishTunnel();
    /** Throw away the tunnel being drawn; true if there was one (Undo takes that back first). */
    bool dropLive() { const bool had = !mLive.empty(); mLive.clear(); return had; }
    static F32 tunnelWidth();

private:
    bool hit(S32 x, S32 y, F32& rx, F32& ry) const;
    void finishBox();
    void dropDrag() { mDragging = false; if (hasMouseCapture()) setMouseCapture(false); }
    WolfCaves::Cave* mEdit = nullptr;
    EPen mPen = PEN_BOX;
    bool mDrawing = false;
    std::vector<F32> mLive;     // the tunnel being drawn
    bool mDragging = false;     // the box footprint being dragged out, corner A to corner B
    F32 mAX = 0.f, mAY = 0.f, mBX = 0.f, mBY = 0.f;
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
    LLButton *mPenBox = nullptr, *mPenTunnel = nullptr, *mPenDoor = nullptr;
    LLLineEditor* mName = nullptr;
    LLSpinCtrl *mBoxW = nullptr, *mBoxD = nullptr, *mBoxH = nullptr, *mBoxTurn = nullptr, *mBoxFloor = nullptr;
    LLSliderCtrl *mWidth = nullptr, *mRoof = nullptr, *mRough = nullptr;
    LLComboBox *mRock = nullptr, *mLight = nullptr;
    LLTextureCtrl* mRockTex = nullptr;
    LLTextBox *mCounts = nullptr, *mStats = nullptr, *mStatus = nullptr, *mWhy = nullptr;
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
};

#endif // WOLF_CAVE_H
