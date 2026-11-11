/**
 * @file lltoolbrush.h
 * @brief toolbrush class header file
 *
 * $LicenseInfo:firstyear=2002&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#ifndef LL_LLTOOLBRUSH_H
#define LL_LLTOOLBRUSH_H

#include "lltool.h"
#include "v3math.h"
#include "lleditmenuhandler.h"

#include <functional>      // [SKY LAND TOOLS 2026-11-10]
#include <unordered_map>
#include <unordered_set>
#include <vector>          // [W104 FEATFIX 2026-11-11/]

class LLSurface;
class LLVector3d;
class LLViewerRegion;

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// Class LLToolBrushLand
//
// A toolbrush that modifies the land.
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

class LLToolBrushLand : public LLTool, public LLEditMenuHandler, public LLSingleton<LLToolBrushLand>
{
    LLSINGLETON(LLToolBrushLand);
    typedef std::set<LLViewerRegion*> region_list_t;

public:

    // x,y in window coords, 0,0 = left,bot
    virtual bool handleMouseDown( S32 x, S32 y, MASK mask ) override;
    virtual bool handleMouseUp( S32 x, S32 y, MASK mask ) override;
    virtual bool handleHover( S32 x, S32 y, MASK mask ) override;
    virtual void handleSelect() override;
    virtual void handleDeselect() override;

    // isAlwaysRendered() - return true if this is a tool that should
    // always be rendered regardless of selection.
    virtual bool isAlwaysRendered()  override { return true; }

    // Draw the area that will be affected.
    virtual void render() override;

    // on Idle is where the land modification actually occurs
    static void onIdle(void* brush_tool);

    void onMouseCaptureLost() override;

    void modifyLandInSelectionGlobal();
    // [WOLF TERRAFORM 2026-10-23] Greyed: the region's UndoLand does nothing (TerrainModule.cs:1491-1493), so Edit > Undo would
    // silently do nothing. The land tool's status line says so (floater_tools.xml status_modifyland).
    // [WOLF TERRAFORM 2026-10-23 Phase 3] ...except in a region with the WolfTerraform module, whose UndoLand puts back the agent's
    // last brush stroke or flood (TerrainModule.WolfTerraform.cs client_OnLandUndo): there it is offered and sends UndoLand.
    virtual void    undo() override;
    virtual bool    canUndo() const  override;
    /** [WOLF TERRAFORM 2026-10-23] The next click on the ground sets Flatten's "Level to" height (WolfLandLevelHeight). */
    void startLevelPick();

    // Does region allow terraform, or are we a god? [WOLF TERRAFORM 2026-10-23] public: the Terraform window asks the same.
    bool canTerraformRegion(LLViewerRegion* regionp) const;

protected:
    void brush( void );
    void modifyLandAtPointGlobal( const LLVector3d &spot, MASK mask, F32 dab_seconds );

    void determineAffectedRegions(region_list_t& regions,
                                  const LLVector3d& spot) const;
    void renderOverlay(LLSurface& land, const LLVector3& pos_region,
                       const LLVector3& pos_world);
    // [SKY LAND TOOLS 2026-11-10] renderOverlay's lines on any surface: z_at(i, j, z) gives the height at grid point (i, j),
    // false where there is none (a sky level's ground ends); grids = the points a side.
    void renderOverlayLines(const std::function<bool(S32, S32, F32&)>& z_at, S32 grids, const LLVector3& pos_region,
                            const LLVector3& pos_world);
    // [SKY LAND TOOLS 2026-11-10] The land under the pointer, a sky level's top included (WolfTerrainLevels::pickLand); during a
    // stroke on a level, that level only (pickOnLevel), so the stroke never falls through to the ground below.
    bool pickSpot(S32 x, S32 y, LLVector3d& spot, LLViewerRegion** regionp = nullptr, S32* level = nullptr);
    // One dab of a stroke on mSkyLevel: the region's brush maths on the level's heights, kept in mSkyWork until sent.
    // [W104 FEATFIX 2026-11-11 B2] begun by beginSkyDab and done by stepSkyDab, a bounded number of cells a frame (mSkyDab).
    void beginSkyDab(const LLVector3d& spot, F32 dab_seconds);
    void stepSkyDab();
    // Sends the cells changed since the last request (one 256 m block a request, one request at a time; the region allows
    // 30 changes a minute, WolfTerrainHistory.cs WolfTerrainRate).
    void sendSkyWork();
    static void onSkyFlushIdle(void* brush_tool);

    bool canTerraformParcel(LLViewerRegion* regionp) const;

    // Modal dialog that you can't terraform the region
    void alertNoTerraformRegion(LLViewerRegion* regionp);

    void alertNoTerraformParcel();

protected:
    F32 mStartingZ;
    S32 mMouseX;
    S32 mMouseY;
    F32 mBrushSize;
    bool mGotHover;
    bool mBrushSelected;
    F64 mLastDabAt = 0.0;     // [WOLF TERRAFORM 2026-10-23] when the last dab went (lltoolbrush.cpp DAB_SECS)
    bool mPickLevel = false;  // [WOLF TERRAFORM 2026-10-23] the next click picks the Level to height
    // Order doesn't matter and we do check for existance of regions, so use a set
    region_list_t mLastAffectedRegions;

    // [SKY LAND TOOLS 2026-11-10] A stroke on a sky level (README.md 3.5: "When the pick hits a level, the brush works on that
    // level ... applied by the cap"). The ground is never sent a ModifyLand while mSkyLevel is set.
    S32 mSkyLevel = 0;                              // the level the stroke shapes; 0 = the ground (stock ModifyLand)
    U64 mSkyRegion = 0;                             // its region's handle
    std::unordered_map<U32, F32> mSkyWork;          // the stroke's heights so far, key x | y << 16 (region metres)
    std::unordered_set<U32> mSkyDirty;              // cells of mSkyWork the region has not been sent yet
    bool mSkyInFlight = false;
    F64 mSkyNextPost = 0.0;                         // not before (pacing, or the region's retry_after)
    // [W104 FEATFIX 2026-11-11 B1] Edit > Undo puts back exactly the last stroke's own cells, as they were before it, with an
    // "apply" (as the Terraform window's sky Undo does, wolfterraform.cpp levelCoro) - never the region's "undo", which
    // undoes the caller's LAST change of any kind (WolfSkyLevelsModule.Edit.cs Undo), perhaps a Terraform window job, and
    // keeps only 20 steps an agent (MaxStepsPerAgent), fewer than a long stroke sends.
    // [W104 FEATFIX 2026-11-11 B2] the dab being done (lltoolbrush.cpp beginSkyDab / stepSkyDab): its square, its maths and the
    // heights before it, read column by column ahead of the writes
    struct SkyDab
    {
        bool mActive = false;
        S32 mAction = 0, mW = 0, mSsize = 0;
        F32 mRx = 0.f, mRy = 0.f, mSize = 0.f, mStrength = 0.f, mRz = 0.f, mLo = 0.f, mHi = 0.f;
        S32 mStartX = 0, mStartY = 0, mEndX = -1, mEndY = -1;   // the square shaped
        S32 mGx0 = 0, mGy0 = 0, mGx1 = -1, mGw = 0, mGh = 0;    // the grid read (the square and Smooth's margin)
        bool mExact = true;                                     // Smooth adds each neighbourhood up one by one
        std::vector<F32> mGrid;
        std::vector<U8> mGround;
        std::vector<F64> mSatSum, mSatN;                        // Smooth above SKY_SMOOTH_EXACT_M: summed-area tables
        S32 mReadX = 0, mNextX = 0;                             // the next column to read / to shape
    };
    SkyDab mSkyDab;
    U32 mSkyStroke = 0;                             // the stroke going on / just ended (a number, for mSkyUndo)
    std::unordered_map<U32, F32> mSkyBefore;        // its cells as they were before it touched them
    struct SkyUndo
    {
        U32 mStroke = 0;                            // the stroke these are (0: nothing to undo)
        U64 mRegion = 0;
        S32 mLevel = 0;
        std::unordered_map<U32, F32> mBefore;       // the cells the region kept, and their heights before the stroke
    };
    SkyUndo mSkyUndo;
    bool mSkyUndoBusy = false;                      // an Undo is being sent
    bool mSkyStrokeAlerted = false;                 // [B5] one alert a stroke when the region refuses or does not answer
    // [B5] the request's cells the region did not keep: back out of the stroke (mSkyWork, mSkyDirty), so later dabs and Undo
    // work from the heights the region has
    void rollBackSkyCells(const std::vector<U32>& cells);
    static void skyUndoCoro(U64 handle, S32 level, std::unordered_map<U32, F32> before);

private:
    U8 getBrushIndex();
};


#endif // LL_LLTOOLBRUSH_H
