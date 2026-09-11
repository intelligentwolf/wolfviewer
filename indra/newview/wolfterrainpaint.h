/**
 * @file wolfterrainpaint.h
 * @brief WolfViewer: painted ground textures — roads and tracks drawn on the terrain (Build > Paint).
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

#ifndef WOLF_TERRAINPAINT_H
#define WOLF_TERRAINPAINT_H

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "llpanel.h"
#include "llpointer.h"
#include "llsingleton.h"
#include "lltool.h"

class LLButton;
class LLCheckBoxCtrl;
class LLGLSLShader;
class LLSliderCtrl;
class LLSpinCtrl;
class LLTextBox;
class LLTextureCtrl;
class LLViewerFetchedTexture;
class LLViewerRegion;
class LLVOWater;

// Source: wolfstorm/js/world/terrain_paint.js (2026-09-10) — the same record, the same API,
// the same rasteriser and the same map layout; keep them in step.
//
// Paul: "add a tab to the build menu ... paint textures on to the terrain ... drawn on the
// terrain after the normal terrain textures have rezzed ... roads and tracks"; "only
// available on wolf territories grid"; "the user must have the rights"; "follow the
// direction the person is painting so they can draw roads"; "add transparency level".
//
// RECORD (wolfstorm.app php/terrain_paint.php, table robust.terrain_paint), one per region:
//   textures: four slots {id, scale} (scale = metres of stroke one repeat of the texture
//   covers along it), strokes: {t (slot 0-3, -1 = erase), w (width m), o (opacity), p
//   (region-metre polyline x,y,...)}. Later strokes cover earlier ones.
//
// RENDERING: per region an RGBA8 PAINT MAP (4 px/m, at most 2048 px a side for the agent's
// region, 1024 for a neighbour) rasterised from the strokes: R,G = cos,sin of the along-phase
// (2π · metres along / tile) of the nearest stroke — an angle survives bilinear filtering where
// a wrapped fraction would not — B = (slot + across)/4 (across 0 = the left edge of travel,
// 1 = the right), A = feathered coverage × opacity. terrainF.glsl / pbrterrainF.glsl
// (wolfTerrainPaint) decode it and mix the palette texture — its x across the band, its y
// along it — over the ground after the composition blend, only once the region's own detail
// textures and every palette texture in use have loaded. Bound by lldrawpoolterrain.cpp
// through bind()/unbind(); the map is uploaded lazily there.
//
// RIGHTS: the service decides (region holder: anything; parcel owner: strokes whose whole
// band is on their land); canPaintAt/canPaintSegment here are the courtesy copy so the brush
// refuses before a save can fail. No credentials live in this (public) viewer: a save
// carries the agent and session ids, verified against the grid's presence service.
class WolfTerrainPaint : public LLSingleton<WolfTerrainPaint>
{
    LLSINGLETON(WolfTerrainPaint);
    ~WolfTerrainPaint();

public:
    static constexpr S32 SLOTS = 4;
    static constexpr F32 MAP_PX_PER_M = 4.f;
    static constexpr S32 MAP_MAX_PX = 2048;
    static constexpr S32 MAP_MAX_PX_NEIGHBOUR = 1024;
    static constexpr F32 MIN_WIDTH = 0.25f;
    static constexpr F32 MAX_WIDTH = 64.f;
    static constexpr F32 MIN_SCALE = 0.5f;
    static constexpr F32 MAX_SCALE = 64.f;
    static constexpr S32 MAX_STROKES = 4000;
    static constexpr S32 MAX_POINTS = 80000;
    static constexpr F32 REFRESH_SECS = 120.f;
    static const char* API_URL;

    struct Slot
    {
        LLUUID mId;          // null = empty slot
        F32    mScale = 4.f; // metres per texture repeat
        bool   mWorld = true;    // true: the world grid (the default — Paul 09-10); false: square tiles following the stroke ("road")
    };
    struct Stroke
    {
        S32 mSlot = 0;       // 0-3, -1 = erase, -2 = water (Paul 09-10: "allow users to draw water")
        F32 mWidth = 4.f;
        F32 mOpacity = 1.f;
        std::vector<F32> mPoints;   // x0, y0, x1, y1, ... region metres
        std::vector<F32> mZ;        // water only: surface height per point (region metres)
        F32 mDepth = 1.f;           // water only: depth (m)
        F32 mLevel = 0.3f;          // water only, while being drawn: metres above the ground
    };
    struct Record
    {
        std::string mUuid;
        std::string mName;
        U64  mHandle = 0;
        S32  mSizeX = 256;
        S32  mSizeY = 256;
        S32  mVersion = 0;
        bool mEnabled = true;
        bool mStored = false;
        Slot mTextures[SLOTS];
        std::vector<Stroke> mStrokes;
    };
    /** The Build > Paint working copy of the agent region's record. */
    struct Edit
    {
        U64  mHandle = 0;
        std::string mUuid;
        std::string mName;
        S32  mBaseVersion = 0;
        std::set<std::string> mBaseKeys;   // stroke identities as fetched, for the 409 merge
        Slot mTextures[SLOTS];
        std::vector<Stroke> mStrokes;
        bool mEnabled = true;
        bool mDirty = false;
        U32  mRev = 0;
        S32  mDropped = 0;
    };

    static constexpr F32 WATER_LEVEL_DEFAULT = 0.3f;
    static constexpr F32 WATER_DEPTH_DEFAULT = 1.f;
    static constexpr F32 WATER_SIMPLIFY_M = 0.3f;   // Douglas-Peucker tolerance of a finished water stroke
    static constexpr S32 MAX_WATER_PLANES = 512;    // LLVOWater planes per region
    static F32 clampWidth(F32 w);
    static F32 clampOpacity(F32 o);
    static F32 clampScale(F32 s);
    static std::string strokeKey(const Stroke& s);
    /** Does the erase band cross the water band? (terrain_paint_water.js intersects — keep in step) */
    static bool waterIntersects(const Stroke& water, const Stroke& erase);
    /** Douglas-Peucker on a water stroke, heights kept in step (terrain_paint_water.js simplify). */
    static void simplifyWater(Stroke& s, F32 tol);

    /** Every frame from LLAppViewer::idle (never from the render pass — it creates and kills objects): fetch on region change / every REFRESH_SECS, bake a slice, keep the water planes. */
    void idle();
    void refresh();
    const Record* current() const;
    bool onGrid() const { return current() != nullptr; }
    bool fetching() const { return mFetching; }
    U64  fetchedFor() const { return mFetchedForHandle; }
    F64  lastFetchAgeSecs() const;
    const std::string& lastError() const { return mLastError; }

    /** lldrawpoolterrain.cpp: bind the map + palette for the region being drawn (or switch the paint off). */
    void bind(LLGLSLShader* shader, LLViewerRegion* regionp);
    void unbind(LLGLSLShader* shader);

    // ── editing ────────────────────────────────────────────────────────────────────
    Edit* beginEdit();
    Edit* edit() { return mEdit.get(); }
    void  endEdit();
    /** Something outside the brush changed (undo, clear, enabled): rebake the preview. */
    void  editChanged();
    /** A palette slot changed; rebakes only when a used slot's tile length changed. */
    void  editPaletteChanged();
    bool  canEditAll() const;
    bool  canPaintAt(F32 x, F32 y, F32 w) const;
    bool  canPaintSegment(F32 x0, F32 y0, F32 x1, F32 y1, F32 w) const;
    /** The brush over the agent's terrain (region metres). */
    /** slot -2 = water: `level` metres above the ground, `depth` metres deep. */
    bool  beginStroke(S32 slot, F32 w, F32 o, F32 x, F32 y, F32 level = WATER_LEVEL_DEFAULT, F32 depth = WATER_DEPTH_DEFAULT);
    void  extendStroke(F32 x, F32 y);
    const Stroke* endStroke();
    bool  strokeInProgress() const { return mLiveStroke; }
    /** True while the agent region's paint map is still being baked (a stroke cannot start yet). */
    bool  agentLayerBaking() const;
    /** True when the working copy has changes no save has taken; names the region for the quit prompt. */
    bool  hasUnsavedPaint(std::string& region_name) const;
    /** Why the last endStroke() returned null on a rights refusal (cleared when read). */
    std::string takeRefusal() { std::string m = mLastRefusal; mLastRefusal.clear(); return m; }
    /** Save the working copy; the result arrives through saving()/lastError() and a notification. */
    void  save();
    bool  saving() const { return mSaving; }
    /** Diagnostics for the panel. */
    S32   bakes() const { return mBakes; }

private:
    struct Box { S32 x0 = 0, y0 = 0, x1 = 0, y1 = 0; bool empty() const { return x1 <= x0 || y1 <= y0; } };
    /** The baked paint of one region. */
    struct Layer
    {
        U64  mHandle = 0;
        S32  mW = 0, mH = 0;
        F32  mPpm = 0.f;
        S32  mSizeX = 0, mSizeY = 0;
        // RGBA16F: R,G = (0.25 + 0.2·slot) · (cos φ, sin φ), φ = 2π·along/tile (0 for a world-grid
        // slot); B = metres across the band from its left edge / tile; A = coverage. Half floats.
        std::vector<U16> mMap;
        U32  mTex = 0;                  // GL texture, uploaded lazily by bind()
        bool mDirtyGL = false;
        F64  mLastUpload = 0.0;         // uploads are throttled while a bake is in progress
        bool mHasPaint = false;
        bool mUsed[SLOTS] = { false, false, false, false };
        F32  mScales[SLOTS] = { 4.f, 4.f, 4.f, 4.f };
        bool mWorld[SLOTS] = { true, true, true, true };
        LLPointer<LLViewerFetchedTexture> mPalette[SLOTS];
        std::string mKey;               // the layout the layer follows (edit revision / record version)
        std::string mTexSig;            // what the paint MAP was baked from (texture strokes + tiles)
        std::string mWaterKey;          // what the water planes were built from
        std::vector<LLPointer<LLVOWater>> mWater;   // one plane per water-stroke segment
        // per-stroke scratch: nearest-segment distance, metres along, signed lateral (left +)
        std::vector<F32> mSDist, mSAlong, mSLat;
        Box  mSBox;
        bool mSBoxSet = false;
        // an incremental bake in progress (idle() does a slice per frame)
        std::vector<Stroke> mPending;
        size_t mPendingIndex = 0;
        bool mBaking = false;
        // the brush drag in progress; mLiveBase is the map as committed before it began; the
        // whole stroke is re-rasterised on every update (updateLiveStroke)
        bool mLive = false;
        std::vector<U16> mLiveBase;
        Stroke mLiveStroke;
        Box  mLiveBox;
        bool mLiveBoxSet = false;
    };

    void fetchCoro(std::vector<U64> handles);
    void saveCoro(std::string region_uuid, LLSD body_layout, bool enabled, S32 version, bool retried);
    /** The working copy's layout as the service wants it (textures + strokes). */
    LLSD editLayoutLLSD() const;
    std::vector<U64> neighbourHandles() const;
    static Record parseRecord(const LLSD& r);
    static Stroke parseStroke(const LLSD& s, bool& ok);
    static LLSD strokeToLLSD(const Stroke& s);
    void mergeOnto(const Record& theirs);

    /** The layout a region should show: the edit for the agent's region, else the stored record. */
    bool layoutFor(U64 handle, const Slot*& textures, const std::vector<Stroke>*& strokes, std::string& key) const;
    Layer& layerFor(LLViewerRegion* regionp);
    void ensureBuffers(Layer& L, LLViewerRegion* regionp);
    void ensureScratch(Layer& L);
    void clearScratch(Layer& L);
    void releaseLayer(Layer& L);
    void setPalette(Layer& L, const Slot* textures, bool& bakeChanged);
    void updateLiveStroke(Layer& L);
    static U16 f32_to_f16(F32 v);
    static F32 f16_to_f32(U16 h);
    void startBake(Layer& L, const std::vector<Stroke>& strokes);
    static std::string texSig(const std::string& key, const Slot* textures, const std::vector<Stroke>& strokes);
    void rebuildWater(Layer& L, LLViewerRegion* regionp, const std::vector<Stroke>& strokes);
    void killWater(Layer& L);
    void bakeSlice(Layer& L, F64 budget_secs);
    Box  bbox(const Layer& L, const F32* pts, size_t n, F32 w) const;
    static void unionBox(Box& a, bool& a_set, const Box& b);
    bool rasterSegment(Layer& L, F32 x0, F32 y0, F32 x1, F32 y1, F32 w, F32 cum, Box& out);
    bool rasterPolyline(Layer& L, const std::vector<F32>& p, F32 w, Box& out);
    void composite(Layer& L, const Box& bb, const Stroke& s, const std::vector<U16>& src, std::vector<U16>& dst);
    void rasterStroke(Layer& L, const Stroke& s);
    bool layerReady(const Layer& L, LLViewerRegion* regionp) const;
    void upload(Layer& L);

    std::map<U64, Record> mByHandle;
    std::map<U64, Layer>  mLayers;
    std::unique_ptr<Edit> mEdit;
    bool mLiveStroke = false;
    bool mLiveIsWater = false;
    Stroke mLiveWater;
    std::string mLastRefusal;
    // Rubber band (terrain_paint.js extendStroke): fixed corners, the hand's path since the last
    // one, the cursor, and the deviation that fixes a corner.
    std::vector<F32> mBandAnchors, mBandRaw;
    F32  mBandCursor[2] = { 0.f, 0.f };
    bool mBandHasCursor = false;
    F32  mBandTol = 0.3f;
    U64  mFetchedForHandle = 0;
    F64  mNextRefresh = 0.0;
    F64  mLastFetchAt = 0.0;
    bool mFetching = false;
    bool mSaving = false;
    S32  mBakes = 0;
    std::string mLastError;
};

/**
 * The brush over the 3-D view: drag on the ground to lay a stroke (Source: lltoolbrush.cpp
 * LLToolBrushLand for the drag tool shape — mousePointOnLandGlobal, mouse capture, hover
 * cursor). The panel's "Paint on the ground" button selects it in the basic toolset.
 */
class WolfToolTerrainPaint : public LLTool, public LLSingleton<WolfToolTerrainPaint>
{
    LLSINGLETON(WolfToolTerrainPaint);
public:
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    void handleSelect() override;
    void handleDeselect() override;
    bool isAlwaysRendered() override { return true; }
    void render() override;
    void onMouseCaptureLost() override;
    /** Esc puts the brush down (back to the edit tool). */
    bool handleKey(KEY key, MASK mask) override;

    /** Brush settings the panel edits (persisted: WolfTerrainPaintBrushWidth / ...Transparency). */
    /** 0-3 a palette slot, -2 water. */
    void setSlot(S32 slot) { mSlot = slot; }
    S32  slot() const { return mSlot; }
    void setErase(bool b) { mErase = b; }
    bool erase() const { return mErase; }
    /** The tool's last complaint for the panel's status line (cleared when read). */
    std::string takeMessage() { std::string m = mMessage; mMessage.clear(); return m; }

private:
    bool hit(S32 x, S32 y, F32& rx, F32& ry, F32& rz) const;
    void finishStroke();
    F32  brushWidth() const;
    F32  brushOpacity() const;

    S32  mSlot = 0;
    bool mErase = false;
    S32  mMouseX = 0, mMouseY = 0;
    bool mGotHover = false;
    bool mDragging = false;
    bool mStoppedOnce = false;
    F32  mLastX = 0.f, mLastY = 0.f;
    std::string mMessage;
};

/** Build floater > Paint (floater_tools.xml wolf_terrain_paint_panel). */
class WolfPanelTerrainPaint : public LLPanel
{
public:
    WolfPanelTerrainPaint();
    bool postBuild() override;
    void draw() override;
    void refresh() override;

private:
    void rebuild();
    void onTexture(S32 slot);
    void onScale(S32 slot);
    void onClearSlot(S32 slot);
    void onModeSlot(S32 slot);
    void onUseSlot(S32 slot);
    void onWaterParam();
    void onWidth();
    void onTransparency();
    void onErase();
    void onArm();
    void onEnabled();
    void onUndo();
    void onClear();
    void onRevert();
    void onSave();
    void setStatus(const std::string& msg, bool error);
    bool armed() const;

    LLTextBox*      mNote = nullptr;
    LLTextBox*      mStatus = nullptr;
    LLTextBox*      mCount = nullptr;
    LLTextureCtrl*  mTex[WolfTerrainPaint::SLOTS] = { nullptr, nullptr, nullptr, nullptr };
    LLSpinCtrl*     mScale[WolfTerrainPaint::SLOTS] = { nullptr, nullptr, nullptr, nullptr };
    LLButton*       mUse[WolfTerrainPaint::SLOTS] = { nullptr, nullptr, nullptr, nullptr };
    LLButton*       mUseWater = nullptr;
    LLSpinCtrl*     mWaterLevel = nullptr;
    LLSpinCtrl*     mWaterDepth = nullptr;
    LLButton*       mClearSlot[WolfTerrainPaint::SLOTS] = { nullptr, nullptr, nullptr, nullptr };
    LLButton*       mModeSlot[WolfTerrainPaint::SLOTS] = { nullptr, nullptr, nullptr, nullptr };
    LLSliderCtrl*   mWidth = nullptr;
    LLSliderCtrl*   mTransparency = nullptr;
    LLCheckBoxCtrl* mErase = nullptr;
    LLButton*       mArm = nullptr;
    LLCheckBoxCtrl* mEnabled = nullptr;
    LLButton*       mUndo = nullptr;
    LLButton*       mClear = nullptr;
    LLButton*       mSave = nullptr;
    U64  mShownHandle = 0;
    S32  mShownVersion = -1;
    U32  mShownRev = 0xffffffff;
    bool mWasSaving = false;
    bool mShownFetching = false;
    bool mShownArmed = false;
    F64  mNextPoll = 0.0;
};

#endif // WOLF_TERRAINPAINT_H
