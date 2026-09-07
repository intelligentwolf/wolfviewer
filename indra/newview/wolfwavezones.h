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
#include "llsingleton.h"
#include "lluictrl.h"
#include "llviewerparcelmgr.h"

class LLViewerRegion;
class LLButton;
class LLCheckBoxCtrl;
class LLSliderCtrl;
class LLTextBox;

// Source: wolfstorm/js/world/wave_zones.js (2026-09-07) — the same layout, the same API, the
// same default rule, the same texture. WAVES_PLAN_2026-09-07.md §3-5.
//
// A region's sea is 16 m cells, each surf (s) / open (o) / calm (c) / off (x). The layout is
// set by whoever may edit the land and stored on the grid (wolfstorm.app php/waves.php,
// table robust.waves); a region with no stored layout — and every region on another grid —
// gets the automatic default (surf along the outer 48 m where the water is open, calm where
// land closes in, open between). The renderer sees one "zone energy" texture (surf 1, open
// 0.55, calm 0.15, off 0) over the same 3x-region span as the exposure field, baked into
// WolfWaterField::Field beside it from this region's layout AND its neighbours'.
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
    static constexpr S32 CELL_M = 16;
    static constexpr F32 SURF_BAND_M = 48.f;
    static constexpr F32 REFRESH_SECS = 120.f;

    struct Region
    {
        std::string mUuid;
        std::string mName;
        U64         mHandle = 0;
        S32         mSizeX = 256;
        S32         mSizeY = 256;
        S32         mVersion = 0;
        bool        mEnabled = true;
        bool        mStored = false;   // a layout exists on the grid
        std::string mZones;            // stored zones (w*h chars) or empty
        LLSD        mParams;
        S32 w() const { return llmax(1, mSizeX / CELL_M); }
        S32 h() const { return llmax(1, mSizeY / CELL_M); }
    };

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
    /** Save; the reply arrives through the notification system and a refresh. */
    void save(const std::string& zones, const LLSD& params, bool enabled);
    bool saving() const { return mSaving; }
    const std::string& lastError() const { return mLastError; }

    static const char* API_URL;

private:
    void fetchCoro(std::vector<U64> handles);
    void saveCoro(std::string region_uuid, std::string zones, LLSD params, bool enabled, S32 version);
    std::vector<U64> neighbourHandles() const;
    static F32 energyOf(char z);

    std::map<U64, Region> mByHandle;
    std::map<U64, std::string> mPreview;   // handle -> zones being edited
    LLSD mPreviewParams;                   // slider values being edited (undefined = none)
    U64  mFetchedForHandle = 0;
    F64  mNextRefresh = 0.0;
    bool mFetching = false;
    bool mSaving = false;
    std::string mLastError;
};

/**
 * The painted grid in About Land > Waves: one coloured square per 16 m cell, south row at the
 * bottom (as the map draws a region), locked cells darkened; click or drag paints the brush.
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
    S32 cellPx() const;

    S32 mW = 16, mH = 16;
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

    LLParcelSelectionHandle& mParcel;
    WolfWavePainter* mPainter = nullptr;
    LLTextBox*       mStatus = nullptr;
    LLTextBox*       mNote = nullptr;
    LLSliderCtrl*    mSurfHeight = nullptr;
    LLSliderCtrl*    mSetInterval = nullptr;
    LLSliderCtrl*    mCalmRipple = nullptr;
    LLCheckBoxCtrl*  mEnabled = nullptr;
    LLButton*        mSave = nullptr;
    LLButton*        mBrushS = nullptr;
    LLButton*        mBrushO = nullptr;
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
