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

// Source: wolfstorm/js/world/wave_zones.js (2026-09-07) — the same layout, the same API, the
// same default rule, the same texture. WAVES_PLAN_2026-09-07.md §3-5.
//
// A region's sea is cells (16 m; doubled on huge regions so the grid is at most 256 cells an
// edge — the service's waves_cell() decides and sends `cell`), each surf (s) / open (o) /
// small waves (m) / calm (c) / off (x). The layout is set by whoever may edit the land and
// stored on the grid (wolfstorm.app php/waves.php, table robust.waves).
// [2026-09-10, Paul] "there should be NO waves inside the region only at the outside of it ...
// only do waves if someone has drawn the waves with the wave editor", then "put the waves back
// round the region automatically if there are no user defined settings": a region with no
// stored layout — and every region on another grid — is FLAT inside with OPEN waves on the
// outer SURF_BAND_M where that edge water faces the open sea (defaultZones). The sea beyond
// the region edges stays open water; surf painted along an edge still rolls in from far out.
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
    /** The base cell; a region's real cell is Region::mCell (php/waves.php waves_cell). */
    static constexpr S32 CELL_M = 16;
    static constexpr S32 MAX_CELLS_EDGE = 256;
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
        S32         mCell = CELL_M;    // cell size (m) the service uses for this region
        S32         mVersion = 0;
        bool        mEnabled = true;
        bool        mStored = false;   // a layout exists on the grid
        std::string mZones;            // stored zones (w*h chars) or empty
        LLSD        mParams;
        S32 w() const { return llmax(1, mSizeX / mCell); }
        S32 h() const { return llmax(1, mSizeY / mCell); }
    };

    /** The service's rule: 16 m, doubled until the region is at most MAX_CELLS_EDGE cells an edge. */
    static S32 cellFor(S32 sizeX, S32 sizeY);
    /**
     * Texel size (m) of the baked zone texture for a region's field: its own cell, then doubled
     * until a span of span_m is at most 768 texels (a 25,600 m region would otherwise bake a
     * 92 MB float texture).
     */
    S32 texelM(LLViewerRegion* regionp, F32 span_m) const;
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
    /** Zones for a record: stored+enabled, else the automatic default. */
    std::string zonesFor(const Region& r) const;
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
    /** Preview a working layout in the water until the next fetch/save. */
    void preview(const std::string& zones);
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
    void save(const std::string& zones, const LLSD& params, bool enabled);
    bool saving() const { return mSaving; }
    const std::string& lastError() const { return mLastError; }

    static const char* API_URL;

private:
    void fetchCoro(std::vector<U64> handles);
    void saveCoro(std::string region_uuid, std::string zones, LLSD params, bool enabled, S32 version, bool retried);
    std::vector<U64> neighbourHandles() const;
    static F32 energyOf(char z);

    std::map<U64, Region> mByHandle;
    std::map<U64, std::string> mPreview;   // handle -> zones being edited
    LLSD mPreviewParams;                   // slider values being edited (undefined = none)
    U64  mFetchedForHandle = 0;
    F64  mNextRefresh = 0.0;
    F64  mLastFetchAt = 0.0;
    bool mFetching = false;
    bool mSaving = false;
    std::string mLastError;
};

/**
 * The painted grid in About Land > Waves: the region's terrain (water blue by depth, land
 * green -> brown -> grey -> white by height, hill-shaded) with one tinted square per painted
 * cell over it — off cells show the terrain through — south row at the bottom (as the map
 * draws a region), locked cells darkened; click or drag paints the brush. Cells are drawn at
 * a fractional pixel size so a 256-cell region fits the same box as a 16-cell one (Paul: "on
 * massive regions scale the drawing down"; "the terrain drawn on it so the user can see where
 * they are drawing").
 */
class WolfWavePainter : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Params() {}
    };
    WolfWavePainter(const Params& p);

    void setLayout(S32 w, S32 h, const std::string& zones, const std::vector<bool>& locked);
    const std::string& zones() const { return mZones; }
    void setBrush(char z) { mBrush = z; }
    char brush() const { return mBrush; }
    bool dirty() const { return mDirty; }
    void clearDirty() { mDirty = false; }
    /** Called after each painted cell (the panel previews live). */
    void setPaintCallback(const std::function<void()>& cb) { mOnPaint = cb; }
    /** Called when the brush hit a locked cell (once per stroke). */
    void setRefusedCallback(const std::function<void()>& cb) { mOnRefused = cb; }

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;

private:
    bool cellAt(S32 x, S32 y, S32& cx, S32& cy) const;
    void paint(S32 x, S32 y);
    F32 cellPx() const;
    /** Rebuild the terrain underlay texture from the agent region's heights (every few seconds while drawn). */
    void refreshTerrain();

    S32 mW = 16, mH = 16;
    LLPointer<LLImageRaw>      mTerrainRaw;
    LLPointer<LLViewerTexture> mTerrainTex;
    S32  mTerrainN = 0;
    U64  mTerrainHandle = 0;
    F64  mTerrainBuiltAt = 0.0;
    std::string mZones;
    std::vector<bool> mLocked;
    char mBrush = 's';
    bool mPainting = false;
    bool mRefusedThisStroke = false;
    bool mDirty = false;
    std::function<void()> mOnPaint, mOnRefused;
};

/** About Land > Waves. Source: llfloaterland.cpp LLPanelLandCovenant for the panel shape. */
class WolfPanelLandWaves : public LLPanel
{
public:
    WolfPanelLandWaves(LLParcelSelectionHandle& parcel);
    bool postBuild() override;
    void refresh() override;   // LLPanel::refresh (clang -Winconsistent-missing-override is an error on the mac CI)
    /** Polls the grid's answers (a fetch landing, a save finishing) — LLFloaterLand::refresh
     *  only runs on parcel changes, which left Save greyed and "Saving…" up after a save. */
    void draw() override;

private:
    void rebuild();
    void onBrush(char z);
    void onSave();
    void onRevert();
    void onDefault();
    void onParamChanged();
    void armBakeConfirm();
    LLSD paramsFromControls() const;
    void setStatus(const std::string& msg, bool error);

    WolfWavePainter* mPainter = nullptr;
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
    bool             mShownFetching = false;
    F64              mNextPoll = 0.0;
    U32              mBakeMark = 0;        // bake count when the last preview was requested
    F64              mBakeWaitUntil = 0.0; // > 0 while a preview waits for its bake
};

#endif // WOLF_WAVEZONES_H
