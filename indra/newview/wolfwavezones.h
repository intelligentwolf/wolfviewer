/**
 * @file wolfwavezones.h
 * @brief WolfViewer: per-region wave zones — the layout set in About Land > Waves.
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

#ifndef WOLF_WAVEZONES_H
#define WOLF_WAVEZONES_H

#include <map>
#include <string>
#include <vector>

#include "llpanel.h"
#include "llpointer.h"
#include "llsingleton.h"
#include "lluictrl.h"
#include "llviewerparcelmgr.h"

class LLImageRaw;
class LLViewerTexture;
class LLViewerRegion;
class LLButton;
class LLCheckBoxCtrl;
class LLSliderCtrl;
class LLTextBox;
class WolfToolWavePaint;

// Source: wolfstorm/js/world/wave_zones.js (2026-09-07) — the same layout, the same API, the
// same default rule, the same texture. WAVES_PLAN_2026-09-07.md §3-5.
//
// A region's sea is painted in 16 m cells, each surf (s) / open (o) / small waves (m) /
// calm (c) / off (x); a cell nobody painted takes the automatic layout below. The layout is
// set by whoever may edit the land and stored on the grid (wolfstorm.app php/waves.php, table
// robust.waves).
// <WolfViewer 2026-09-26> Layout v2: the SAME 16 m paint cell on every region (Paul: "i cant
// go to region 1 and put the size on 1 and its huge and go to region2 and the size on 1 its a
// different size" — the v1 grid grew with the region, 512 m cells on Ireland). Painted cells
// arrive as sparse 256 m tiles (php/waves.php WAVES_PAINT_CELL_M); the automatic layout is
// still worked out on the coarser waves_cell() grid (Region::mCell, at most MAX_CELLS_EDGE an
// edge). wave_zones.js same.
// [2026-09-10, Paul] "there should be NO waves inside the region only at the outside of it ...
// only do waves if someone has drawn the waves with the wave editor", then "put the waves back
// round the region automatically if there are no user defined settings": a region with no
// stored layout — and every region on another grid — gets the automatic layout: OPEN sea
// for water that connects through water to the region edge and reaches open water, SMALL waves
// (no breakers, no swash foam) for enclosed water — dug lakes, ponds, rivers, narrow inlets
// (defaultZones: a flood fill over the region's own terrain, not the exposure alone). The sea beyond the region
// edges stays open water; surf painted along an edge still rolls in from far out.
// The renderer sees one "zone energy" texture (surf 1, open 0.55, small 0.35, calm 0.15,
// off 0) over the same 3x-region span as the exposure field, baked into WolfWaterField::Field
// beside it from this region's layout AND its neighbours'; waterV.glsl turns it into a swell
// scale with zoneScale() (off 0 = flat, no breakers, no swash).
//
// No credentials live in this viewer (the repository is public): a save carries the agent
// and session ids, which the service checks against the grid's presence service, and the
// service decides the rights (region owner, estate owner or manager, god: every cell; a
// parcel owner: the cells inside their parcels). cellEditable() here is the courtesy copy.
class WolfWaveZones : public LLSingleton<WolfWaveZones>
{
    LLSINGLETON(WolfWaveZones);
    ~WolfWaveZones();

public:
    /** The base cell; a region's AUTOMATIC grid is Region::mCell (php/waves.php waves_cell). */
    static constexpr S32 CELL_M = 16;
    static constexpr S32 MAX_CELLS_EDGE = 1024;   // <WolfViewer 2026-09-18/> was 256; php/waves.php + wave_zones.js same
    // <WolfViewer 2026-09-26> Layout v2: the paint cell on every region, cells per tile edge and
    // per tile (php/waves.php WAVES_PAINT_CELL_M, WAVES_TILE_CELLS). Tile index math uses >> 4 / & 15.
    static constexpr S32 PAINT_CELL_M = 16;
    static constexpr S32 TILE_CELLS = 16;
    static constexpr S32 TILE_LEN = 256;
    /** Painted tiles: key tileKey(tx, ty) -> 256 chars (row-major, south row first; '.' = not painted). */
    using Tiles = std::map<U32, std::string>;
    static U32 tileKey(S32 tx, S32 ty) { return (U32)ty * 65536u + (U32)tx; }
    /** Paint 16 m cell (fx, fy) of a tile map; true when it changed. */
    static bool setTileCell(Tiles& tiles, S32 fx, S32 fy, char z);
    /** A tile map as the service takes it: {"tx,ty": cells}, a repeated char sent once; always a map. */
    static LLSD tilesToLLSD(const Tiles& tiles);
    /** Source: wave_zones.js cellHasWater — any of five points (centre + quarter points) under the water. */
    static bool cellHasWater(LLViewerRegion* regionp, S32 cx, S32 cy, S32 cell);
    static constexpr F32 SURF_BAND_M = 48.f;
    static constexpr F32 REFRESH_SECS = 120.f;
    /** Default of the small-wave cells' swell as a fraction of the open sea (params smallScale). */
    static constexpr F32 SMALL_SCALE_DEFAULT = 0.30f;

    struct Region
    {
        std::string mUuid;
        std::string mName;
        U64         mHandle = 0;
        S32         mSizeX = 256;
        S32         mSizeY = 256;
        S32         mCell = CELL_M;    // the AUTOMATIC layout's cell (m) for this region (waves_cell)
        S32         mVersion = 0;
        bool        mEnabled = true;
        bool        mStored = false;   // a layout exists on the grid
        Tiles       mTiles;            // <WolfViewer 2026-09-26/> stored painted tiles (layout v2)
        LLSD        mParams;
        /** The automatic grid, in mCell cells. */
        S32 w() const { return llmax(1, mSizeX / mCell); }
        S32 h() const { return llmax(1, mSizeY / mCell); }
        /** The paint grid, in 16 m cells (php/waves.php waves_paint_dims). */
        S32 pw() const { return llmax(1, llmax(256, mSizeX) / PAINT_CELL_M); }
        S32 ph() const { return llmax(1, llmax(256, mSizeY) / PAINT_CELL_M); }
    };

    /** <WolfViewer 2026-09-26> One region ready for zone lookups: painted tiles over the automatic layout. */
    struct Source
    {
        const Tiles* mTiles = nullptr;   // nullptr = automatic only
        std::string  mAuto;              // defaultZones(), aw x ah on the mCell grid
        S32 mAutoW = 1, mAutoCell = CELL_M;
        S32 mW = 16, mH = 16;            // paint grid
        /** The zone of 16 m cell (fx, fy): the painted char, else the automatic one. */
        char at(S32 fx, S32 fy) const;
    };

    // Source: php/waves.php POST region/version; retained when the editor adopts a row.
    struct SaveTarget
    {
        std::string mUuid;
        U64 mHandle = 0;
        S32 mVersion = 0;
    };

    /** The service's rule for the AUTOMATIC grid: 16 m, doubled until the region is at most MAX_CELLS_EDGE cells an edge. */
    static S32 cellFor(S32 sizeX, S32 sizeY);
    /**
     * Texel size (m) of the baked zone texture: the 16 m paint cell, doubled until a span of
     * span_m is at most 768 texels. The field span is at most 3 x WolfWaterField::WINDOW_M, so
     * this is 16 m on every region and a painted cell reaches the water at its own size.
     */
    static S32 texelM(F32 span_m);
    /**
     * Zone energy -> swell scale, the ONE mapping waterV.glsl (zoneScale), the boat rocker and
     * WolfStorm (Water.js, WaveZones.zoneScale) all use — keep them in step. Piecewise linear
     * through the five painted energies: off 0 -> 0 (flat), calm 0.15 -> the calm ripple as a
     * fraction of the swell, small 0.35 -> small_scale, open 0.55 and surf 1.0 -> 1.
     */
    static F32 zoneScale(F32 energy, F32 amplitude, F32 calm_ripple, F32 small_scale);

    /** Every frame (from WolfWaterField::idle): fetch on region change and every REFRESH_SECS. */
    void idle();
    /** Ask the grid now (a save elsewhere, the tab opening). */
    void refresh();
    /** The current region's record, or nullptr (other grid, not fetched yet). */
    const Region* current() const;
    /** True when the current region is known to the grid — the only case the editor exists. */
    bool onGrid() const { return current() != nullptr; }
    /** The painted tiles the WATER uses for a record: the editor preview, else the stored ones while enabled; nullptr = automatic only. */
    const Tiles* paintedFor(const Region& r) const;
    /** A record ready for zone lookups (paintedFor over defaultZones). */
    Source sourceFor(const Region& r) const;
    /**
     * <WolfViewer 2026-09-20> What the EDITOR starts from: the stored cells whenever a
     * layout is stored, even while it is switched off ("Reset to automatic" keeps the painted
     * cells and only unticks the box, so ticking it again must bring them back). The WATER
     * uses paintedFor(): off = automatic. wave_zones.js editorTilesFor same.
     */
    const Tiles& editorTilesFor(const Region& r) const { return r.mTiles; }
    /** The automatic layout, from the exposure field of the agent region's bake. */
    std::string defaultZones(const Region& r) const;

    /**
     * Fill the zone-energy span of a region's field (WolfWaterField::bake uploads it): w x h
     * texels over region-relative [x0, x0+sx) x [y0, y0+sy), from every fetched region that
     * covers a texel; space no region covers is open water (0.55).
     */
    void fill(LLViewerRegion* regionp, F32 x0, F32 y0, F32 sx, F32 sy, S32 w, S32 h, std::vector<F32>& out) const;
    /** Zone energy for the CURRENT region at region-relative (rx, ry), for the boat rocker. */
    F32 energyAt(F32 rx, F32 ry) const;
    /** The default "open" energy, for space no layout covers. */
    static constexpr F32 OPEN_ENERGY = 0.55f;

    /** Editor: the working copy for the current region (zones + params + enabled). */
    bool canEditAll() const;
    bool cellEditable(S32 cx, S32 cy) const;
    /** Preview a working layout in the water until Save or Revert: the painted tiles, or enabled=false for the automatic layout alone. */
    void preview(const Tiles& tiles, bool enabled);
    /** Preview the editor's slider values (surf height, set interval, calm ripple) likewise. */
    void previewParams(const LLSD& params);
    /** The parameters the water should use now: the preview if one is up, else the saved ones. */
    const LLSD& params() const;
    void clearPreview();
    /** True while a fetch is in flight (the editor says "asking the grid"). */
    bool fetching() const { return mFetching; }
    /** The region handle the last fetch answered for (0 = none yet). */
    U64 fetchedFor() const { return mFetchedForHandle; }
    /** Seconds since the last fetch answered (a large number before the first). */
    F64 lastFetchAgeSecs() const;
    /** Save; the reply arrives through the notification system and a refresh. */
    bool save(const SaveTarget& target, const Tiles& tiles, const LLSD& params, bool enabled);
    bool saving() const { return mSaving; }
    const std::string& lastError() const { return mLastError; }
    const std::string& lastSaveError() const { return mLastSaveError; }
    S32 lastSavedVersion() const { return mLastSavedVersion; }

    static const char* API_URL;

private:
    void fetchCoro(std::vector<U64> handles, U64 requested_handle, U64 generation);
    void saveCoro(SaveTarget target, Tiles tiles, LLSD params, bool enabled, U64 preview_revision, bool retried);
    /** Stored tiles from a v2 payload layout; entries the service would not have written are dropped. */
    static Tiles parseTiles(const LLSD& layout, S32 pw, S32 ph);
    std::vector<U64> neighbourHandles() const;
    static F32 energyOf(char z);

    std::map<U64, Region> mByHandle;
    struct Preview { Tiles mTiles; bool mEnabled = true; };
    std::map<U64, Preview> mPreview;       // handle -> the layout being edited
    LLSD mPreviewParams;                   // slider values being edited (undefined = none)
    U64 mPreviewParamsFor = 0;
    U64 mPreviewRevision = 0;
    U64  mFetchedForHandle = 0;
    F64  mNextRefresh = 0.0;
    std::vector<U64> mRequestedHandles;   // <WolfViewer 2026-09-23/> the set the last fetch asked for
    F64 mNextNeighbourCheck = 0.0;       // <WolfViewer 2026-09-23/> idle compares the neighbour set once a second
    F64  mLastFetchAt = 0.0;
    bool mFetching = false;
    bool mSaving = false;
    std::string mLastError;
    std::string mLastSaveError;
    S32 mLastSavedVersion = 0;
    U64 mFetchGeneration = 0;
    boost::signals2::connection mRegionChangedConnection;
};

/**
 * The painted grid in About Land > Waves: the region's terrain (water blue by depth, land
 * green -> brown -> grey -> white by height, hill-shaded) with the zones tinted over it — off
 * cells show the terrain through — south row at the bottom (as the map draws a region), locked
 * areas darkened; click or drag paints with the panel's brush. Drawn scaled to fit the box
 * (Paul: "on massive regions scale the drawing down"; "the terrain drawn on it so the user can
 * see where they are drawing").
 * <WolfViewer 2026-09-26> The zones are one texture of up to 512 texels an edge, rebuilt when
 * they change: with 16 m cells on every region Ireland is 28,800 cells an edge, far below a
 * pixel each, so painted cells are put on their own texel after the grid is sampled.
 */
class WolfWavePainter : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Params() {}
    };
    WolfWavePainter(const Params& p);

    /** The working copy: tiles painted over src's automatic layout; locked areas shaded unless all. */
    void setLayout(const WolfWaveZones::Source& src, const WolfWaveZones::Tiles& tiles, bool all);
    void clearLayout();
    const WolfWaveZones::Tiles& tiles() const { return mTiles; }
    S32 cellsW() const { return mSrc.mW; }
    S32 cellsH() const { return mSrc.mH; }
    bool hasLayout() const { return mHasLayout; }
    void setBrush(char z) { mBrush = z; }
    char brush() const { return mBrush; }
    bool dirty() const { return mDirty; }
    void clearDirty() { mDirty = false; }
    /** The panel's brush writes the working copy through this, one 16 m cell at a time. */
    bool paintCell(S32 cx, S32 cy);
    /** Called with each stroke segment in region metres (the panel runs its brush over it). */
    void setStrokeCallback(const std::function<void(F32 ax, F32 ay, F32 bx, F32 by)>& cb) { mOnStroke = cb; }

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;

private:
    bool pointAt(S32 x, S32 y, F32& mx, F32& my) const;
    void stroke(S32 x, S32 y);
    F32 cellPx() const;
    /** Rebuild the terrain underlay texture from the agent region's heights (every few seconds while drawn). */
    void refreshTerrain();
    /** Rebuild the zone overlay texture when the working copy changed (at most ten times a second). */
    void refreshZones();

    LLPointer<LLImageRaw>      mTerrainRaw;
    LLPointer<LLViewerTexture> mTerrainTex;
    S32  mTerrainN = 0;
    U64  mTerrainHandle = 0;
    F64  mTerrainBuiltAt = 0.0;
    WolfWaveZones::Source mSrc;           // mSrc.mTiles points at mTiles
    WolfWaveZones::Tiles  mTiles;
    bool mHasLayout = false;
    LLPointer<LLImageRaw>      mZoneRaw;
    LLPointer<LLViewerTexture> mZoneTex;
    S32  mZoneK = 1;                      // cells per overlay texel
    S32  mZoneTW = 0, mZoneTH = 0;
    std::vector<U8> mLockedTexels;        // per overlay texel, computed in setLayout
    bool mZonesStale = true;
    F64  mZonesBuiltAt = 0.0;
    char mBrush = 's';
    bool mPainting = false;
    bool mPrevious = false;
    F32  mLastX = 0.f, mLastY = 0.f;
    bool mDirty = false;
    std::function<void(F32, F32, F32, F32)> mOnStroke;
};

/** Region / Estate > Waves. Source: llfloaterregioninfo.cpp programmatic region-panel shape. */
class WolfPanelLandWaves : public LLPanel
{
public:
    WolfPanelLandWaves();
    ~WolfPanelLandWaves() override;
    bool postBuild() override;
    void refresh() override;   // LLPanel::refresh (clang -Winconsistent-missing-override is an error on the mac CI)
    /** Polls the grid's answers (a fetch landing, a save finishing) — LLFloaterLand::refresh
     *  only runs on parcel changes, which left Save greyed and "Saving…" up after a save. */
    void draw() override;
    void onVisibilityChange(bool visible) override;

private:
    friend class WolfToolWavePaint;
    void toggleWorldBrush();
    void stopWorldBrush();
    /** The brush along a segment in region metres; waterOnly for the in-world brush (the map paints any cell). */
    bool paintStroke(F32 ax, F32 ay, F32 bx, F32 by, bool waterOnly);
    void rebuild();
    void onBrush(char z);
    void onSave();
    void onRevert();
    void onDefault();
    void onParamChanged();
    void previewEdit();
    void armBakeConfirm();
    LLSD paramsFromControls() const;
    void setStatus(const std::string& msg, bool error);
    bool targetCurrent() const;
    void invalidateTarget();
    /**
     * <WolfViewer 2026-09-20> Painting means "use my layout". Wolf Nation's record was saved
     * with the box off (Reset to automatic + Save) and every later brush stroke was accepted,
     * previewed nothing and saved as enabled=0 — Paul: "I draw on the water and it doesn't
     * work". A region holder painting now ticks the box; a parcel owner, who cannot, is told
     * why. Returns whether painting may go ahead. land_waves_tab.js _wavesEnsureEnabled same.
     */
    bool ensureEnabled();

    WolfWavePainter* mPainter = nullptr;
    LLPointer<WolfToolWavePaint> mWorldTool;
    LLButton*       mWorldPaint = nullptr;
    LLSliderCtrl*   mBrushDiameter = nullptr;
    LLTextBox*       mStatus = nullptr;
    LLTextBox*       mNote = nullptr;
    LLSliderCtrl*    mSurfHeight = nullptr;
    LLSliderCtrl*    mSetInterval = nullptr;
    LLSliderCtrl*    mCalmRipple = nullptr;
    LLSliderCtrl*    mSmallScale = nullptr;
    LLCheckBoxCtrl*  mEnabled = nullptr;
    LLButton*        mSave = nullptr;
    LLButton*        mBrushS = nullptr;
    LLButton*        mBrushO = nullptr;
    LLButton*        mBrushM = nullptr;
    LLButton*        mBrushC = nullptr;
    LLButton*        mBrushX = nullptr;
    S32              mShownVersion = -1;
    U64              mShownHandle = 0;
    bool             mWasSaving = false;
    bool             mDirty = false;
    bool             mWriting = false;
    bool             mStatusError = false;
    U64              mEditRevision = 0;
    U64              mSubmittedRevision = 0;
    WolfWaveZones::SaveTarget mTarget;
    boost::signals2::connection mRegionChangedConnection;
    bool             mShownFetching = false;
    F64              mNextPoll = 0.0;
    U32              mBakeMark = 0;        // bake count when the last preview was requested
    F64              mBakeWaitUntil = 0.0; // > 0 while a preview waits for its bake
};

#endif // WOLF_WAVEZONES_H
