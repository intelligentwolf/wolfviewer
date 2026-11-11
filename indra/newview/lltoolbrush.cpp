/**
 * @file lltoolbrush.cpp
 * @brief Implementation of the toolbrushes
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
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

#include "llviewerprecompiledheaders.h"

#include "lltoolbrush.h"
#include "lltoolselectland.h"

// library headers
#include "llgl.h"
#include "llnotificationsutil.h"
#include "llrender.h"
#include "message.h"

#include "llagent.h"
#include "llcallbacklist.h"
#include "llframetimer.h"
#include "llviewercontrol.h"
#include "llfloatertools.h"
#include "llregionposition.h"
#include "llstatusbar.h"
#include "llsurface.h"
#include "llsurfacepatch.h"
#include "lltoolmgr.h"
#include "llui.h"
#include "llviewerparcelmgr.h"
#include "llviewerparceloverlay.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"
#include "llworld.h"
#include "llappviewer.h"
#include "llparcel.h"
#include "roles_constants.h"
#include "llglheaders.h"
#include "wolfterrainlevels.h"   // [SKY LAND TOOLS 2026-11-10]
#include "llcoros.h"             // [W104 FEATFIX 2026-11-11 B1] the stroke's Undo (skyUndoCoro)
#include "lleventcoro.h"
#include <limits>
#include <map>

const std::string REGION_BLOCKS_TERRAFORM_MSG = "This region does not allow terraforming.\n"
                "You will need to buy land in another part of the world to terraform it.";


///============================================================================
/// Local function declarations, constants, enums, and typedefs
///============================================================================

const S32 LAND_BRUSH_SIZE_COUNT = 3;
const F32 LAND_BRUSH_SIZE[LAND_BRUSH_SIZE_COUNT] = {1.0f, 2.0f, 4.0f};

enum
{
    E_LAND_LEVEL    = 0,
    E_LAND_RAISE    = 1,
    E_LAND_LOWER    = 2,
    E_LAND_SMOOTH   = 3,
    E_LAND_NOISE    = 4,
    E_LAND_REVERT   = 5,
    E_LAND_INVALID  = 6,
};
const LLColor4 OVERLAY_COLOR(1.0f, 1.0f, 1.0f, 1.0f);

///============================================================================
/// Class LLToolBrushLand
///============================================================================

// [WolfViewer 2026-10-20] Paul: "jimmy olsen ... has a 20x20 region ... its a tiny touch on his huge region can we add a 2x
// 4x 8x 10x buttons to alter the scale of the area he is modifying". The brush is the Build floater's size (LandBrushSize,
// slider 1-11) times WolfLandBrushScale (the 1x / 2x / 4x / 8x / 10x buttons). Everything below uses mBrushSize - the
// size sent to the region (ModifyBlockExtended.BrushSize; OpenSim TerrainModule.cs client_OnModifyTerrain paints
// +-brushSize around the point, clipped to the region and to land the user may terraform) and the outline drawn on the
// ground (renderOverlay) - so both grow together.
static F32 wolf_land_brush_size()
{
    return gSavedSettings.getF32("LandBrushSize") * llclamp(gSavedSettings.getF32("WolfLandBrushScale"), 1.f, 10.f);
}

// [WOLF TERRAFORM 2026-10-23] DESIGN.md 3.1 A "Pace the dabs". A dab used to go every rendered frame with Seconds = (1 / fps) *
// LandBrushForce, but the region drops any ModifyLand that comes within 20 ms of the last one (TerrainModule.cs:1792-1794
// client_OnModifyTerrain returns while now < NextModifyTerrainTime; :1946 sets it to now + 0.02 after each), so above about
// 50 fps dabs were lost and a stroke's strength depended on the frame rate. Now a dab goes at most every DAB_SECS (50 ms, well
// clear of the 20 ms cooldown) and carries the time since the last one, so a second of brushing is LandBrushForce at any frame
// rate. The time is capped at DAB_MAX_SECS: the viewer's own cap on a frame's time (llappviewer.cpp:5898 MIN_FRAME_RATE = 1, so
// the old 1 / gFPSClamped never passed 1 s either).
static constexpr F64 DAB_SECS = 0.05;
static constexpr F64 DAB_MAX_SECS = 1.0;

// [WOLF TERRAFORM 2026-10-23] DESIGN.md 3.1 A "Level to height": Flatten aims at a typed (or picked) height when "Level to" is on
// (WolfLandLevelUse / WolfLandLevelHeight, the Build window's land tool), else at the ground where the stroke began (mStartingZ,
// as before). The region blends toward the Height the viewer sends (FlattenSphere.cs PaintEffect rz; FlattenArea.cs height).
static F32 wolf_flatten_height(S32 radio_action, F32 starting_z)
{
    if (radio_action == 0 && gSavedSettings.getBOOL("WolfLandLevelUse")) return gSavedSettings.getF32("WolfLandLevelHeight");
    return starting_z;
}

// [SKY LAND TOOLS 2026-11-10] The region's own maths for a dab, run here on a sky level's heights (the region's ModifyLand only
// shapes the ground). Source: OpenSimWolf TerrainModule.cs client_OnModifyTerrain (the dab's square: Net8Cast.ToInt32 is a
// truncation, Net8Cast.cs:65) and PaintBrushes/RaiseSphere.cs, LowerSphere.cs, FlattenSphere.cs, SmoothSphere.cs, NoiseSphere.cs
// PaintEffect; TerrainUtil.cs Noise / SmoothedNoise1 / Interpolate / InterpolatedNoise / PerlinNoise2D for Roughen.
namespace
{
    constexpr F32 SKY_MAX_BRUSH = 128.f;    // TerrainModule.WolfTerraform.cs:44 WolfMaxBrushSize
    constexpr S32 SKY_BLOCK = 256;          // TerrainModule.WolfTerraform.cs:47 WolfMaxApplySide, metres a side a request
    constexpr F64 SKY_POST_SECS = 1.0;      // at most one request a second while brushing (the region: 30 a minute, a burst of 30)
    // [W104 FEATFIX 2026-11-11 B2] (CHOSEN) Smooth adds each neighbourhood up one height at a time up to this brush size, exactly
    // as before; above it a summed-area table gives the sum (lltoolbrush.cpp stepSkyDab)
    constexpr F32 SKY_SMOOTH_EXACT_M = 32.f;
    // [W104 FEATFIX 2026-11-11 B2] (CHOSEN) at most about this many cells of a dab are read and shaped in one frame (stepSkyDab,
    // whole columns): a dab up to about a 30 m brush is done at once, as before; a 128 m one over about 16 frames
    constexpr S32 SKY_DAB_CELLS_PER_FRAME = 8192;

    U32 sky_key(S32 x, S32 y) { return (U32)x | ((U32)y << 16); }

    // TerrainUtil.cs Noise: C# int arithmetic wraps, so it is done in U32 (the same bits) - signed overflow is undefined in C++.
    F64 sky_noise(F64 x, F64 y)
    {
        U32 n = (U32)(S32)x + (U32)(S32)(y * 749);
        n = (n << 13) ^ n;
        const U32 v = (n * (n * n * 15731u + 789221u) + 1376312589u) & 0x7fffffffu;
        return 1.0 - (F64)v / 1073741824.0;
    }
    F64 sky_smoothed_noise(F64 x, F64 y)
    {
        const F64 corners = (sky_noise(x - 1, y - 1) + sky_noise(x + 1, y - 1) + sky_noise(x - 1, y + 1) + sky_noise(x + 1, y + 1)) / 16;
        const F64 sides = (sky_noise(x - 1, y) + sky_noise(x + 1, y) + sky_noise(x, y - 1) + sky_noise(x, y + 1)) / 8;
        const F64 center = sky_noise(x, y) / 4;
        return corners + sides + center;
    }
    F64 sky_interpolate(F64 x, F64 y, F64 z) { return (x * (1.0 - z)) + (y * z); }
    // [W104 FEATFIX 2026-11-11 B2] sky_smoothed_noise at whole lattice points 0..SKY_LATTICE, made once: Roughen's PerlinNoise2D
    // takes x / Width (0 <= x < 1) times 2^i for its 8 octaves (i < 8), so ix = (S32)(x * 2^i) is at most 127 and ix + 1 at most
    // 128. The same function of the same numbers: the same values, without its 9 noise calls a point each time.
    constexpr S32 SKY_LATTICE = 128;
    F64 sky_lattice(S32 ix, S32 iy)
    {
        static const std::vector<F64> table = []()
        {
            std::vector<F64> t((size_t)(SKY_LATTICE + 1) * (size_t)(SKY_LATTICE + 1));
            for (S32 y = 0; y <= SKY_LATTICE; ++y)
                for (S32 x = 0; x <= SKY_LATTICE; ++x)
                    t[(size_t)y * (size_t)(SKY_LATTICE + 1) + (size_t)x] = sky_smoothed_noise(x, y);
            return t;
        }();
        if (ix < 0 || iy < 0 || ix > SKY_LATTICE || iy > SKY_LATTICE) return sky_smoothed_noise(ix, iy);
        return table[(size_t)iy * (size_t)(SKY_LATTICE + 1) + (size_t)ix];
    }
    F64 sky_interpolated_noise(F64 x, F64 y)
    {
        const S32 ix = (S32)x;
        const F64 fx = x - ix;
        const S32 iy = (S32)y;
        const F64 fy = y - iy;
        const F64 v1 = sky_lattice(ix, iy), v2 = sky_lattice(ix + 1, iy);
        const F64 v3 = sky_lattice(ix, iy + 1), v4 = sky_lattice(ix + 1, iy + 1);
        return sky_interpolate(sky_interpolate(v1, v2, fx), sky_interpolate(v3, v4, fx), fy);
    }
    F64 sky_perlin(F64 x, F64 y, S32 octaves, F64 persistence)
    {
        // [W104 FEATFIX 2026-11-11 B2] Roughen's call (8 octaves, persistence 1.0) takes the powers from a table of the same
        // std::pow results, made once: the same numbers in the same sums, without 16 pow calls a cell
        constexpr S32 OCTAVES = 8;
        static const F64* const pows = []()
        {
            static F64 t[2 * OCTAVES];
            for (S32 i = 0; i < OCTAVES; i++) { t[i] = std::pow(2.0, i); t[OCTAVES + i] = std::pow(1.0, i); }
            return t;
        }();
        F64 total = 0.0;
        if (octaves == OCTAVES && persistence == 1.0)
        {
            for (S32 i = 0; i < OCTAVES; i++)
            {
                total += sky_interpolated_noise(x * pows[i], y * pows[i]) * pows[OCTAVES + i];
            }
            return total;
        }
        for (S32 i = 0; i < octaves; i++)
        {
            total += sky_interpolated_noise(x * std::pow(2.0, i), y * std::pow(2.0, i)) * std::pow(persistence, i);
        }
        return total;
    }

    WolfTerrainLevels* sky_of(U64 handle)
    {
        LLViewerRegion* regionp = handle ? LLWorld::getInstance()->getRegionFromHandle(handle) : nullptr;
        return regionp ? regionp->getWolfSkyLevels() : nullptr;
    }
}

// constructor
LLToolBrushLand::LLToolBrushLand()
:   LLTool(std::string("Land")),
    mStartingZ( 0.0f ),
    mMouseX( 0 ),
    mMouseY(0),
    mGotHover(false),
    mBrushSelected(false)
{
    mBrushSize = wolf_land_brush_size();
}


U8 LLToolBrushLand::getBrushIndex()
{
    // find the best index for desired size
    // (compatibility with old sims, brush_index is now depricated - DEV-8252)
    U8 index = 0;
    for (U8 i = 0; i < LAND_BRUSH_SIZE_COUNT; i++)
    {
        if (mBrushSize > LAND_BRUSH_SIZE[i])
        {
            index = i;
        }
    }

    return index;
}

void LLToolBrushLand::modifyLandAtPointGlobal(const LLVector3d &pos_global,
                                              MASK mask, F32 dab_seconds)
{
    S32 radioAction = gSavedSettings.getS32("RadioLandBrushAction");

    mLastAffectedRegions.clear();
    determineAffectedRegions(mLastAffectedRegions, pos_global);
    for(region_list_t::iterator iter = mLastAffectedRegions.begin();
        iter != mLastAffectedRegions.end(); ++iter)
    {
        LLViewerRegion* regionp = *iter;
        //bool is_changed = false;
        LLVector3 pos_region = regionp->getPosRegionFromGlobal(pos_global);
        LLSurface &land = regionp->getLand();
        char action = E_LAND_LEVEL;
        switch (radioAction)
        {
        case 0:
        //  // average toward mStartingZ
            action = E_LAND_LEVEL;
            break;
        case 1:
            action = E_LAND_RAISE;
            break;
        case 2:
            action = E_LAND_LOWER;
            break;
        case 3:
            action = E_LAND_SMOOTH;
            break;
        case 4:
            action = E_LAND_NOISE;
            break;
        case 5:
            action = E_LAND_REVERT;
            break;
        default:
            action = E_LAND_INVALID;
            break;
        }

        // Don't send a message to the region if nothing changed.
        //if(!is_changed) continue;

        // Now to update the patch information so it will redraw correctly.
        LLSurfacePatch *patchp= land.resolvePatchRegion(pos_region);
        if (patchp)
        {
            patchp->dirtyZ();
        }

        // Also force the property lines to update, normals to recompute, etc.
        regionp->forceUpdate();

        // tell the simulator what we've done
        // [WOLF TERRAFORM 2026-10-23] the time since the last dab, not one frame's (see DAB_SECS)
        F32 seconds = dab_seconds * gSavedSettings.getF32("LandBrushForce");
        F32 x_pos = (F32)pos_region.mV[VX];
        F32 y_pos = (F32)pos_region.mV[VY];
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_ModifyLand);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->nextBlockFast(_PREHASH_ModifyBlock);
        msg->addU8Fast(_PREHASH_Action, (U8)action);
        msg->addU8Fast(_PREHASH_BrushSize, getBrushIndex());
        msg->addF32Fast(_PREHASH_Seconds, seconds);
        msg->addF32Fast(_PREHASH_Height, wolf_flatten_height(radioAction, mStartingZ));   // [WOLF TERRAFORM 2026-10-23/] Level to
        msg->nextBlockFast(_PREHASH_ParcelData);
        msg->addS32Fast(_PREHASH_LocalID, -1);
        msg->addF32Fast(_PREHASH_West, x_pos );
        msg->addF32Fast(_PREHASH_South, y_pos );
        msg->addF32Fast(_PREHASH_East, x_pos );
        msg->addF32Fast(_PREHASH_North, y_pos );
        msg->nextBlock("ModifyBlockExtended");
        msg->addF32("BrushSize", mBrushSize);
        msg->sendMessage(regionp->getHost());
    }
}

void LLToolBrushLand::modifyLandInSelectionGlobal()
{
    if (LLViewerParcelMgr::getInstance()->selectionEmpty())
    {
        return;
    }

    if (LLToolMgr::getInstance()->getCurrentTool() == LLToolSelectLand::getInstance())
    {
        // selecting land, don't do anything
        return;
    }

    LLVector3d min;
    LLVector3d max;

    LLViewerParcelMgr::getInstance()->getSelection(min, max);

    S32 radioAction = gSavedSettings.getS32("RadioLandBrushAction");

    mLastAffectedRegions.clear();

    determineAffectedRegions(mLastAffectedRegions, LLVector3d(min.mdV[VX], min.mdV[VY], 0));
    determineAffectedRegions(mLastAffectedRegions, LLVector3d(min.mdV[VX], max.mdV[VY], 0));
    determineAffectedRegions(mLastAffectedRegions, LLVector3d(max.mdV[VX], min.mdV[VY], 0));
    determineAffectedRegions(mLastAffectedRegions, LLVector3d(max.mdV[VX], max.mdV[VY], 0));

    LLRegionPosition mid_point_region((min + max) * 0.5);
    LLViewerRegion* center_region = mid_point_region.getRegion();
    if (center_region)
    {
        LLVector3 pos_region = mid_point_region.getPositionRegion();
        U32 grids = center_region->getLand().mGridsPerEdge;
        S32 i = llclamp( (S32)pos_region.mV[VX], 0, (S32)grids );
        S32 j = llclamp( (S32)pos_region.mV[VY], 0, (S32)grids );
        mStartingZ = center_region->getLand().getZ(i, j);
    }
    else
    {
        mStartingZ = 0.f;
    }

    // Stop if our selection include a no-terraform region
    for(region_list_t::iterator iter = mLastAffectedRegions.begin();
        iter != mLastAffectedRegions.end(); ++iter)
    {
        LLViewerRegion* regionp = *iter;
        if (!canTerraformRegion(regionp))
        {
            alertNoTerraformRegion(regionp);
            return;
        }
    }

    for(region_list_t::iterator iter = mLastAffectedRegions.begin();
        iter != mLastAffectedRegions.end(); ++iter)
    {
        LLViewerRegion* regionp = *iter;
        //bool is_changed = false;
        LLVector3 min_region = regionp->getPosRegionFromGlobal(min);
        LLVector3 max_region = regionp->getPosRegionFromGlobal(max);

        min_region.clamp(0.f, regionp->getWidth());
        max_region.clamp(0.f, regionp->getWidth());
        F32 seconds = gSavedSettings.getF32("LandBrushForce");

        LLSurface &land = regionp->getLand();
        char action = E_LAND_LEVEL;
        switch (radioAction)
        {
        case 0:
        //  // average toward mStartingZ
            action = E_LAND_LEVEL;
            seconds *= 0.25f;
            break;
        case 1:
            action = E_LAND_RAISE;
            seconds *= 0.25f;
            break;
        case 2:
            action = E_LAND_LOWER;
            seconds *= 0.25f;
            break;
        case 3:
            action = E_LAND_SMOOTH;
            seconds *= 5.0f;
            break;
        case 4:
            action = E_LAND_NOISE;
            seconds *= 0.5f;
            break;
        case 5:
            action = E_LAND_REVERT;
            seconds = 0.5f;
            break;
        default:
            //action = E_LAND_INVALID;
            //seconds = 0.0f;
            return;
            break;
        }

        // Don't send a message to the region if nothing changed.
        //if(!is_changed) continue;

        // Now to update the patch information so it will redraw correctly.
        LLSurfacePatch *patchp= land.resolvePatchRegion(min_region);
        if (patchp)
        {
            patchp->dirtyZ();
        }

        // Also force the property lines to update, normals to recompute, etc.
        regionp->forceUpdate();

        // tell the simulator what we've done
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_ModifyLand);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->nextBlockFast(_PREHASH_ModifyBlock);
        msg->addU8Fast(_PREHASH_Action, (U8)action);
        msg->addU8Fast(_PREHASH_BrushSize, getBrushIndex());
        msg->addF32Fast(_PREHASH_Seconds, seconds);
        msg->addF32Fast(_PREHASH_Height, wolf_flatten_height(radioAction, mStartingZ));   // [WOLF TERRAFORM 2026-10-23/] Level to

        bool parcel_selected = LLViewerParcelMgr::getInstance()->getParcelSelection()->getWholeParcelSelected();
        LLParcel* selected_parcel = LLViewerParcelMgr::getInstance()->getParcelSelection()->getParcel();

        if (parcel_selected && selected_parcel)
        {
            msg->nextBlockFast(_PREHASH_ParcelData);
            msg->addS32Fast(_PREHASH_LocalID, selected_parcel->getLocalID());
            msg->addF32Fast(_PREHASH_West,  min_region.mV[VX] );
            msg->addF32Fast(_PREHASH_South, min_region.mV[VY] );
            msg->addF32Fast(_PREHASH_East,  max_region.mV[VX] );
            msg->addF32Fast(_PREHASH_North, max_region.mV[VY] );
        }
        else
        {
            msg->nextBlockFast(_PREHASH_ParcelData);
            msg->addS32Fast(_PREHASH_LocalID, -1);
            msg->addF32Fast(_PREHASH_West,  min_region.mV[VX] );
            msg->addF32Fast(_PREHASH_South, min_region.mV[VY] );
            msg->addF32Fast(_PREHASH_East,  max_region.mV[VX] );
            msg->addF32Fast(_PREHASH_North, max_region.mV[VY] );
        }

        msg->nextBlock("ModifyBlockExtended");
        msg->addF32("BrushSize", mBrushSize);

        msg->sendMessage(regionp->getHost());
    }
}

void LLToolBrushLand::brush( void )
{
    // [WOLF TERRAFORM 2026-10-23] at most one dab every DAB_SECS, carrying the time since the last (see DAB_SECS)
    const F64 now = LLFrameTimer::getElapsedSeconds();
    // [W104 FEATFIX 2026-11-11 B2] a dab on sky land still being done goes on; the next starts when it is finished
    if (mSkyLevel && mSkyDab.mActive)
    {
        stepSkyDab();
        sendSkyWork();
        return;
    }
    if (now - mLastDabAt < DAB_SECS) return;
    const F32 dab_seconds = (F32)llmin(now - mLastDabAt, DAB_MAX_SECS);
    mLastDabAt = now;
    LLVector3d spot;
    if( pickSpot( mMouseX, mMouseY, spot ) )   // [SKY LAND TOOLS 2026-11-10/] was mousePointOnLandGlobal (the ground only)
    {
        // Round to nearest X,Y grid
        spot.mdV[VX] = floor( spot.mdV[VX] + 0.5 );
        spot.mdV[VY] = floor( spot.mdV[VY] + 0.5 );

        // [SKY LAND TOOLS 2026-11-10] a stroke that began on a sky level shapes that level only - never the ground below it
        if (mSkyLevel)
        {
            beginSkyDab(spot, dab_seconds);   // [W104 FEATFIX 2026-11-11 B2/] begun here, done by stepSkyDab (now, and next frames)
            stepSkyDab();
            sendSkyWork();
            return;
        }
        modifyLandAtPointGlobal(spot, gKeyboard->currentMask(true), dab_seconds);
    }
}

// [SKY LAND TOOLS 2026-11-10] lltoolbrush.h. During a stroke on a level: that level only, else where the pointer's ray crosses the
// flat at the stroke's starting height (WolfTerrainLevels::pickOnLevel), so going past the level's edge does not jump to the ground.
bool LLToolBrushLand::pickSpot(S32 x, S32 y, LLVector3d& spot, LLViewerRegion** regionp, S32* level)
{
    if (mSkyLevel && hasMouseCapture())
    {
        LLViewerRegion* r = LLWorld::getInstance()->getRegionFromHandle(mSkyRegion);
        if (!r || !WolfTerrainLevels::pickOnLevel(x, y, r, mSkyLevel, mStartingZ, spot)) return false;
        if (regionp) *regionp = r;
        if (level) *level = mSkyLevel;
        return true;
    }
    LLViewerRegion* r = nullptr;
    S32 l = 0;
    if (!WolfTerrainLevels::pickLand(x, y, spot, r, l)) return false;
    if (regionp) *regionp = r;
    if (level) *level = l;
    return true;
}

// [SKY LAND TOOLS 2026-11-10] lltoolbrush.h. One dab, on the level's heights as this viewer holds them (and the stroke's own
// changes so far, mSkyWork), with the region's maths for the action (see sky_noise above). Only cells where the level has ground
// are changed: the brush shapes sky land, it does not make it (the Sky Land window's "Make ground here" does). Every height stays
// between the level's base and the region's highest (WolfSkyLevelsModule.Edit.cs Apply refuses a request with one outside).
// [W104 FEATFIX 2026-11-11 B2] The work is bounded: a dab is begun here and done by stepSkyDab, at most SKY_DAB_CELLS_PER_FRAME
// cells a frame, column by column. Each cell's new height depends only on heights from BEFORE the dab (Smooth's neighbours
// included), and those are read into the dab's own grid before any of them is written (a column is read before the column
// Smooth's ssize to its left is written), so a dab done over several frames gives exactly what it gave done at once. Each cell is
// read once (Smooth looked each neighbour up again, up to 81 times a cell); Smooth on a brush wider than SKY_SMOOTH_EXACT_M takes
// each neighbourhood's sum from a summed-area table; Roughen's noise lattice and powers come from tables made once. The next
// dab starts when this one is done, carrying the time since the last began, so a stroke's strength over time is unchanged.
// Up to SKY_SMOOTH_EXACT_M (Smooth) - every size for the other tools - the heights are exactly those of the one-pass code
// (checked against it: region-terrain-work/w104-review-2026-11-11/featfix/b2_dab_check.sh).
void LLToolBrushLand::beginSkyDab(const LLVector3d& spot, F32 dab_seconds)
{
    mSkyDab = SkyDab();
    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(mSkyRegion);
    WolfTerrainLevels* sky = regionp ? regionp->getWolfSkyLevels() : nullptr;
    const WolfTerrainLevels::Level* level = sky ? sky->findLevel(mSkyLevel) : nullptr;
    if (!level) return;
    SkyDab& d = mSkyDab;
    d.mAction = gSavedSettings.getS32("RadioLandBrushAction");
    if (d.mAction < 0 || d.mAction > 4) return;   // Revert (5) has nothing to go back to on sky land (handleMouseDown says so)
    d.mStrength = dab_seconds * gSavedSettings.getF32("LandBrushForce");   // as modifyLandAtPointGlobal's Seconds
    const LLVector3 p = regionp->getPosRegionFromGlobal(spot);
    d.mRx = p.mV[VX];
    d.mRy = p.mV[VY];
    d.mSize = llmin(mBrushSize, SKY_MAX_BRUSH);
    d.mW = (S32)regionp->getWidth();
    d.mStartX = llmax(0, (S32)(d.mRx - d.mSize + 0.5f));
    d.mStartY = llmax(0, (S32)(d.mRy - d.mSize + 0.5f));
    d.mEndX = llmin(d.mW - 1, (S32)(d.mRx + d.mSize + 0.5f));
    d.mEndY = llmin(d.mW - 1, (S32)(d.mRy + d.mSize + 0.5f));
    if (d.mEndX < d.mStartX || d.mEndY < d.mStartY) return;
    d.mLo = level->mBase;
    d.mHi = sky->maxHeight();
    d.mRz = llclamp(wolf_flatten_height(d.mAction, mStartingZ), d.mLo, d.mHi);
    // SmoothSphere.cs: ssize at most 4 (the neighbourhood is (2 ssize + 1)^2 cells round each)
    d.mSsize = d.mAction == 3 ? llmin(4, (S32)(d.mSize + 0.5f)) : 0;
    d.mExact = d.mSize <= SKY_SMOOTH_EXACT_M;
    // the grid: the square and Smooth's margin, inside the region
    d.mGx0 = llmax(0, d.mStartX - d.mSsize);
    d.mGy0 = llmax(0, d.mStartY - d.mSsize);
    d.mGx1 = llmin(d.mW - 1, d.mEndX + d.mSsize);
    const S32 gy1 = llmin(d.mW - 1, d.mEndY + d.mSsize);
    d.mGw = d.mGx1 - d.mGx0 + 1;
    d.mGh = gy1 - d.mGy0 + 1;
    d.mGrid.assign((size_t)d.mGw * (size_t)d.mGh, 0.f);
    d.mGround.assign((size_t)d.mGw * (size_t)d.mGh, 0);
    if (d.mAction == 3 && !d.mExact)
    {
        d.mSatSum.assign((size_t)(d.mGw + 1) * (size_t)(d.mGh + 1), 0.0);
        d.mSatN.assign((size_t)(d.mGw + 1) * (size_t)(d.mGh + 1), 0.0);
    }
    d.mReadX = d.mGx0;
    d.mNextX = d.mStartX;
    d.mActive = true;
}

// [W104 FEATFIX 2026-11-11 B2] lltoolbrush.h. The begun dab's next columns, at most SKY_DAB_CELLS_PER_FRAME cells (read and written).
void LLToolBrushLand::stepSkyDab()
{
    SkyDab& d = mSkyDab;
    if (!d.mActive) return;
    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(mSkyRegion);
    WolfTerrainLevels* sky = regionp ? regionp->getWolfSkyLevels() : nullptr;
    const WolfTerrainLevels::Level* level = sky ? sky->findLevel(mSkyLevel) : nullptr;
    if (!level)
    {
        d = SkyDab();   // the level went: sendSkyWork says so
        return;
    }
    auto at = [&d](S32 x, S32 y) { return (size_t)(y - d.mGy0) * (size_t)d.mGw + (size_t)(x - d.mGx0); };
    const S32 sw = d.mGw + 1;   // the summed-area tables' row length (one more column and row of zeros)
    // one column of the grid: the heights before this dab (the stroke's own, else the level's), and its summed-area columns
    auto read_column = [&](S32 x)
    {
        for (S32 y = d.mGy0; y < d.mGy0 + d.mGh; ++y)
        {
            const size_t i = at(x, y);
            auto it = mSkyWork.find(sky_key(x, y));
            if (it != mSkyWork.end()) { d.mGrid[i] = it->second; d.mGround[i] = 1; }
            else if (sky->sample(*level, x, y, d.mGrid[i])) d.mGround[i] = 1;
        }
        if (d.mSatSum.empty()) return;
        const S32 cx = x - d.mGx0 + 1;
        for (S32 gy = 0; gy < d.mGh; ++gy)
        {
            const size_t c = (size_t)(gy + 1) * (size_t)sw + (size_t)cx;
            const size_t i = (size_t)gy * (size_t)d.mGw + (size_t)(x - d.mGx0);
            d.mSatSum[c] = (d.mGround[i] ? (F64)d.mGrid[i] : 0.0) + d.mSatSum[c - 1] + d.mSatSum[c - (size_t)sw] - d.mSatSum[c - (size_t)sw - 1];
            d.mSatN[c] = (d.mGround[i] ? 1.0 : 0.0) + d.mSatN[c - 1] + d.mSatN[c - (size_t)sw] - d.mSatN[c - (size_t)sw - 1];
        }
    };
    // grid cells ax0..ax1, ay0..ay1 (grid coordinates, inclusive) of a summed-area table
    auto box = [&](const std::vector<F64>& t, S32 ax0, S32 ay0, S32 ax1, S32 ay1)
    {
        const size_t r0 = (size_t)ay0 * (size_t)sw, r1 = (size_t)(ay1 + 1) * (size_t)sw;
        return t[r1 + (size_t)(ax1 + 1)] - t[r0 + (size_t)(ax1 + 1)] - t[r1 + (size_t)ax0] + t[r0 + (size_t)ax0];
    };
    // put: as before, a cell keeps its height when it is already that (or has no ground); [B1] its height before the stroke is
    // kept the first time the stroke changes it
    auto put = [&](S32 x, S32 y, F32 z)
    {
        z = llclamp(z, d.mLo, d.mHi);
        const size_t i = at(x, y);
        if (!d.mGround[i] || d.mGrid[i] == z) return;
        const U32 k = sky_key(x, y);
        mSkyBefore.emplace(k, d.mGrid[i]);   // (no-op when the stroke changed it before: mSkyWork then held this height)
        mSkyWork[k] = z;
        mSkyDirty.insert(k);
    };
    const F32 size2 = d.mSize * d.mSize;
    const F32 st = llmin(d.mStrength, 1.f);
    const F32 cos_size = 0.5f * F_PI / d.mSize;   // RaiseSphere.cs / LowerSphere.cs: size = 0.5 * PI / size
    S32 budget = SKY_DAB_CELLS_PER_FRAME;
    while (d.mNextX <= d.mEndX && budget > 0)
    {
        const S32 x = d.mNextX;
        // every column this one's cells read (Smooth: up to ssize to its right) is read before anything is written to it
        while (d.mReadX <= llmin(d.mGx1, x + d.mSsize))
        {
            read_column(d.mReadX++);
            budget -= d.mGh;
        }
        const F32 dx2 = ((F32)x - d.mRx) * ((F32)x - d.mRx);
        for (S32 y = d.mStartY; y <= d.mEndY; ++y)
        {
            const size_t i = at(x, y);
            if (!d.mGround[i]) continue;
            const F32 here = d.mGrid[i];
            const F32 d2 = dx2 + ((F32)y - d.mRy) * ((F32)y - d.mRy);
            if (d.mAction == 3)
            {
                // SmoothSphere.cs: the delta from the heights before this dab (the grid), then applied (strength at most 1). Only
                // neighbours with ground count in the mean (the ground has every cell; a level's edge has none beyond it).
                F32 df = d2 / size2;
                if (df > 1.f) continue;
                df = st * (1.f - df);
                F32 mean;
                if (d.mExact)
                {
                    F32 sum = 0.f;
                    S32 n = 0;
                    for (S32 a = x - d.mSsize; a <= x + d.mSsize; ++a)
                        for (S32 b = y - d.mSsize; b <= y + d.mSsize; ++b)
                        {
                            if (a < 0 || a >= d.mW || b < 0 || b >= d.mW) continue;
                            const size_t j = at(a, b);
                            if (d.mGround[j]) { sum += d.mGrid[j]; ++n; }
                        }
                    mean = sum / (F32)n;   // n >= 1: (x, y) itself
                }
                else
                {
                    // the neighbourhood's sum and count from the summed-area tables (F64 sums: only the last bits of a big
                    // brush's result differ from adding the 81 heights one by one)
                    const S32 ax0 = llmax(0, x - d.mSsize) - d.mGx0, ax1 = llmin(d.mW - 1, x + d.mSsize) - d.mGx0;
                    const S32 ay0 = llmax(0, y - d.mSsize) - d.mGy0, ay1 = llmin(d.mW - 1, y + d.mSsize) - d.mGy0;
                    mean = (F32)(box(d.mSatSum, ax0, ay0, ax1, ay1) / box(d.mSatN, ax0, ay0, ax1, ay1));
                }
                const F32 tweak = df * (here - mean);
                if (tweak != 0.f) put(x, y, here - tweak);
            }
            else if (d.mAction == 1 || d.mAction == 2)
            {
                const F32 f = (F32)std::cos(std::sqrt((F64)d2) * cos_size);
                if (f > 0.f) put(x, y, d.mAction == 1 ? here + f * d.mStrength : here - f * d.mStrength);
            }
            else if (d.mAction == 0)
            {
                // FlattenSphere.cs: size *= 2 * size
                F32 df = d2 / (2.f * d.mSize * d.mSize);
                if (df > 1.f) continue;
                df = d.mStrength * (1.f - df);
                put(x, y, df >= 1.f ? d.mRz : here + (d.mRz - here) * df);
            }
            else
            {
                // NoiseSphere.cs: size *= size; PerlinNoise2D(x / Width, y / Height, 8, 1.0)
                F32 df = d2 / size2;
                if (df > 1.f) continue;
                df = d.mStrength * (1.f - df);
                put(x, y, here + (F32)sky_perlin((F64)x / (F64)d.mW, (F64)y / (F64)d.mW, 8, 1.0) * df);
            }
        }
        budget -= d.mEndY - d.mStartY + 1;
        ++d.mNextX;
    }
    if (d.mNextX > d.mEndX) d = SkyDab();   // done (and its grid let go)
}

// [SKY LAND TOOLS 2026-11-10] lltoolbrush.h. The cells not sent yet go as one "apply" (WolfTerrainLevels::applyBody, as the Sky Land
// window sends) for the 256 m block of the first of them; the next block goes when the region has answered.
void LLToolBrushLand::sendSkyWork()
{
    if (mSkyInFlight || mSkyDirty.empty()) return;
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (now < mSkyNextPost) return;   // the region's pace (onSkyFlushIdle tries again after the stroke)
    WolfTerrainLevels* sky = sky_of(mSkyRegion);
    const WolfTerrainLevels::Level* level = sky ? sky->findLevel(mSkyLevel) : nullptr;
    if (!level)
    {
        mSkyDirty.clear();
        mSkyWork.clear();
        if (gFloaterTools) gFloaterTools->setStatusText("That sky level is no longer there, so the change could not be kept.");
        return;
    }
    const U32 first = *mSkyDirty.begin();
    const S32 bx = (S32)(first & 0xFFFFu) / SKY_BLOCK * SKY_BLOCK, by = (S32)(first >> 16) / SKY_BLOCK * SKY_BLOCK;
    S32 x0 = S32_MAX, y0 = S32_MAX, x1 = -1, y1 = -1;
    std::vector<U32> cells;
    for (U32 k : mSkyDirty)
    {
        const S32 x = (S32)(k & 0xFFFFu), y = (S32)(k >> 16);
        if (x < bx || x >= bx + SKY_BLOCK || y < by || y >= by + SKY_BLOCK) continue;
        cells.push_back(k);
        x0 = llmin(x0, x); y0 = llmin(y0, y); x1 = llmax(x1, x); y1 = llmax(y1, y);
    }
    const S32 w = x1 - x0 + 1, h = y1 - y0 + 1;
    std::vector<F32> heights((size_t)w * (size_t)h, std::numeric_limits<F32>::quiet_NaN());
    for (U32 k : cells)
    {
        const S32 x = (S32)(k & 0xFFFFu), y = (S32)(k >> 16);
        heights[(size_t)(y - y0) * (size_t)w + (size_t)(x - x0)] = mSkyWork[k];
        mSkyDirty.erase(k);
    }
    mSkyInFlight = true;
    mSkyNextPost = now + SKY_POST_SECS;
    const U64 handle = mSkyRegion;
    const S32 level_id = mSkyLevel;
    const U32 stroke = mSkyStroke;
    sky->post(WolfTerrainLevels::applyBody(mSkyLevel, x0, y0, w, h, heights, "land brush"),
              [this, cells, handle, level_id, stroke, x0, y0, w, h](const LLSD& answer)
    {
        // the tool is a singleton (LLSingleton<LLToolBrushLand>): `this` outlives every request. A new stroke cannot start
        // before this one's cells are all answered (handleMouseDown), so the answer is the current stroke's.
        mSkyInFlight = false;
        if (answer["success"].asBoolean())
        {
            // [W104 FEATFIX 2026-11-11 B1/B5] cell by cell: WolfSkyLevelsModule.Edit.cs Handle sends "cells" (one byte a cell of
            // the rectangle, row by row from y0: CellSet 0, CellLeft 1 = NaN, CellRights 2, CellNoRoom 4) only when some were
            // refused; without it every cell sent was kept
            std::vector<U32> kept, refused;
            const LLSD::Binary* states = answer.has("cells") ? &answer["cells"].asBinary() : nullptr;
            const bool per_cell = states && states->size() == (size_t)w * (size_t)h;
            for (U32 k : cells)
            {
                const S32 x = (S32)(k & 0xFFFFu), y = (S32)(k >> 16);
                const U8 state = per_cell ? (*states)[(size_t)(y - y0) * (size_t)w + (size_t)(x - x0)] : (U8)0;
                (state == 0 ? kept : refused).push_back(k);
            }
            if (!per_cell && (answer["refused"].asInteger() > 0 || answer["no_room"].asInteger() > 0))
            {
                // refused, but not said which: none counted as kept (the Undo then never "puts back" them)
                refused.insert(refused.end(), kept.begin(), kept.end());
                kept.clear();
            }
            if (!kept.empty() && answer["changed"].asInteger() > 0)
            {
                // [W104 FEATFIX review 2026-11-11] the region remembers this as this login's last change (wolfterrainlevels.h)
                WolfTerrainLevels::noteChange(handle, WolfTerrainLevels::EDITOR_BRUSH);
            }
            if (!kept.empty())
            {
                // the stroke's Undo: these cells as they were before it
                if (mSkyUndo.mStroke != stroke)
                {
                    mSkyUndo = SkyUndo();
                    mSkyUndo.mStroke = stroke;
                    mSkyUndo.mRegion = handle;
                    mSkyUndo.mLevel = level_id;
                }
                for (U32 k : kept)
                {
                    auto b = mSkyBefore.find(k);
                    if (b != mSkyBefore.end()) mSkyUndo.mBefore.emplace(k, b->second);
                }
            }
            if (!refused.empty()) rollBackSkyCells(refused);
            if (answer["refused"].asInteger() > 0 || answer["no_room"].asInteger() > 0)
            {
                // Apply's Words(): which parcels, and whose (the cells it would not change)
                if (gFloaterTools) gFloaterTools->setStatusText(answer["message"].asString());
            }
        }
        else if (answer["busy"].asBoolean())
        {
            // too many at once, or the region was busy: the same cells again after retry_after
            for (U32 k : cells) mSkyDirty.insert(k);
            mSkyNextPost = LLFrameTimer::getElapsedSeconds() + llmax(1, answer["retry_after"].asInteger());
        }
        else
        {
            // [W104 FEATFIX 2026-11-11 B5] the region refused the request or did not answer: its cells go back to the region's
            // heights, and ONE alert a stroke (it stays until OK is pressed); the status line says each later one
            rollBackSkyCells(cells);
            const std::string text = answer["message"].asString();
            if (gFloaterTools) gFloaterTools->setStatusText("The sky land change was not kept: " + text);
            if (!mSkyStrokeAlerted)
            {
                mSkyStrokeAlerted = true;
                LLSD args;
                args["MESSAGE"] = "The sky land change was not kept: " + text;
                LLNotificationsUtil::add("GenericAlert", args);
            }
        }
    });
}

// [W104 FEATFIX 2026-11-11 B5] lltoolbrush.h
void LLToolBrushLand::rollBackSkyCells(const std::vector<U32>& cells)
{
    for (U32 k : cells)
    {
        mSkyWork.erase(k);    // later dabs read the region's height again (WolfTerrainLevels::sample)
        mSkyDirty.erase(k);   // and a newer value made from the refused one is not sent
    }
}

// [SKY LAND TOOLS 2026-11-10] After a stroke on a level: what is left goes at the region's pace, then the stroke's heights are let go.
// static
void LLToolBrushLand::onSkyFlushIdle(void* brush_tool)
{
    LLToolBrushLand* self = reinterpret_cast<LLToolBrushLand*>(brush_tool);
    if (self->hasMouseCapture()) return;   // (a new stroke cannot start before this one is all sent: handleMouseDown)
    if (self->mSkyDab.mActive) self->stepSkyDab();   // [W104 FEATFIX 2026-11-11 B2/] the last dab is finished first
    if (self->mSkyDirty.empty() && !self->mSkyInFlight && !self->mSkyDab.mActive)
    {
        self->mSkyWork.clear();
        gIdleCallbacks.deleteFunction(&LLToolBrushLand::onSkyFlushIdle, self);
        return;
    }
    self->sendSkyWork();
}

// [WOLF TERRAFORM 2026-10-23] "Pick": the next click on the ground gives Flatten's "Level to" height instead of brushing.
void LLToolBrushLand::startLevelPick()
{
    mPickLevel = true;
    if (gFloaterTools) gFloaterTools->setStatusText("Click on the ground to take its height for Level to.");
}

bool LLToolBrushLand::handleMouseDown(S32 x, S32 y, MASK mask)
{
    bool handled = false;

    // Find the z value of the initial click.
    // [SKY LAND TOOLS 2026-11-10] on a sky level's top when the pointer is over one (pickSpot), else the ground, as before
    LLVector3d spot;
    S32 sky_level = 0;
    LLViewerRegion* sky_region = nullptr;
    if( pickSpot( x, y, spot, &sky_region, &sky_level ) )
    {
        // Round to nearest X,Y grid
        spot.mdV[VX] = floor( spot.mdV[VX] + 0.5 );
        spot.mdV[VY] = floor( spot.mdV[VY] + 0.5 );

        LLRegionPosition region_position( spot );
        LLViewerRegion* regionp = region_position.getRegion();
        WolfTerrainLevels* sky = sky_level && regionp == sky_region && regionp ? regionp->getWolfSkyLevels() : nullptr;
        const WolfTerrainLevels::Level* level = sky ? sky->findLevel(sky_level) : nullptr;
        if (!level) sky_level = 0;
        F32 level_z = 0.f;
        if (level && !sky->surface(*level, (F32)region_position.getPositionRegion().mV[VX], (F32)region_position.getPositionRegion().mV[VY], level_z))
        {
            level_z = (F32)region_position.getPositionRegion().mV[VZ];   // the rounded spot fell just past the edge: the hit's height
        }

        // [WOLF TERRAFORM 2026-10-23] Pick: the ground's height under the click becomes the Level to height; nothing is changed
        if (mPickLevel && regionp)
        {
            mPickLevel = false;
            const F32 z = level ? level_z : regionp->getLand().resolveHeightRegion(region_position.getPositionRegion());
            gSavedSettings.setF32("WolfLandLevelHeight", ll_round(z * 100.f) / 100.f);
            gSavedSettings.setBOOL("WolfLandLevelUse", true);
            if (gFloaterTools) gFloaterTools->setStatusText(level ? llformat("Flatten now levels to %.2f m (the sky land you clicked).", z)
                                                                  : llformat("Flatten now levels to %.2f m (the ground you clicked).", z));
            return true;
        }

        // [SKY LAND TOOLS 2026-11-10] the last stroke on sky land must be all sent first (its cells and their level are kept here)
        if (mSkyInFlight || !mSkyDirty.empty() || mSkyDab.mActive)
        {
            if (gFloaterTools) gFloaterTools->setStatusText("Still sending your last change to the sky land - try again in a moment.");
            return true;
        }
        if (mSkyUndoBusy)   // [W104 FEATFIX 2026-11-11 B1/]
        {
            if (gFloaterTools) gFloaterTools->setStatusText("Still putting back your last brush stroke on the sky land - try again in a moment.");
            return true;
        }
        if (level && gSavedSettings.getS32("RadioLandBrushAction") == E_LAND_REVERT)
        {
            // Revert puts the ground back to the region's baked shape (RevertSphere.cs); sky land has none, so on a level it does
            // nothing (the Revert button's tool tip says so too, floater_tools.xml)
            if (gFloaterTools) gFloaterTools->setStatusText("Revert works on the ground only: sky land has no saved shape to go back to.");
            return true;
        }

        if (!canTerraformRegion(regionp))
        {
            alertNoTerraformRegion(regionp);
            return true;
        }

        if (!canTerraformParcel(regionp))
        {
            alertNoTerraformParcel();
        }

        LLVector3 pos_region = region_position.getPositionRegion();
        U32 grids = regionp->getLand().mGridsPerEdge;
        S32 i = llclamp( (S32)pos_region.mV[VX], 0, (S32)grids );
        S32 j = llclamp( (S32)pos_region.mV[VY], 0, (S32)grids );
        mStartingZ = regionp->getLand().getZ(i, j);
        // [SKY LAND TOOLS 2026-11-10] a stroke on a level: its height there, and every dab goes to that level (brush)
        mSkyLevel = level ? sky_level : 0;
        mSkyRegion = level ? regionp->getHandle() : 0;
        if (level)
        {
            mStartingZ = level_z;
            mSkyWork.clear();
            mSkyNextPost = 0.0;
            // [W104 FEATFIX 2026-11-11 B1/B5] a new stroke: its own cells' first heights, its own alert; the last stroke's Undo
            // stays offered until this one has a change the region kept
            ++mSkyStroke;
            if (mSkyStroke == 0) ++mSkyStroke;   // 0 is "nothing to undo"
            mSkyBefore.clear();
            mSkyStrokeAlerted = false;
        }
        else
        {
            mSkyUndo = SkyUndo();   // Edit > Undo is the ground's again (UndoLand)
        }
        mMouseX = x;
        mMouseY = y;
        mLastDabAt = LLFrameTimer::getElapsedSeconds() - DAB_SECS;   // [WOLF TERRAFORM 2026-10-23/] the first dab on the next frame
        gIdleCallbacks.addFunction( &LLToolBrushLand::onIdle, (void*)this );
        setMouseCapture( true );

        LLViewerParcelMgr::getInstance()->setSelectionVisible(false);
        handled = true;
    }

    return handled;
}

bool LLToolBrushLand::handleHover( S32 x, S32 y, MASK mask )
{
    LL_DEBUGS("UserInput") << "hover handled by LLToolBrushLand ("
                                << (hasMouseCapture() ? "active":"inactive")
                                << ")" << LL_ENDL;
    mMouseX = x;
    mMouseY = y;
    mGotHover = true;
    gViewerWindow->setCursor(UI_CURSOR_TOOLLAND);

    LLVector3d spot;
    if( pickSpot( mMouseX, mMouseY, spot ) )   // [SKY LAND TOOLS 2026-11-10/] a sky level's parcel is the one under it
    {

        spot.mdV[VX] = floor( spot.mdV[VX] + 0.5 );
        spot.mdV[VY] = floor( spot.mdV[VY] + 0.5 );

        LLViewerParcelMgr::getInstance()->setHoverParcel(spot);
    }
    return true;
}

bool LLToolBrushLand::handleMouseUp(S32 x, S32 y, MASK mask)
{
    bool handled = false;
    mLastAffectedRegions.clear();
    if( hasMouseCapture() )
    {
        // Release the mouse
        setMouseCapture( false );

        LLViewerParcelMgr::getInstance()->setSelectionVisible(true);

        gIdleCallbacks.deleteFunction( &LLToolBrushLand::onIdle, (void*)this );
        // [SKY LAND TOOLS 2026-11-10] the rest of a stroke on sky land goes after it, at the region's pace
        if (mSkyLevel && !gIdleCallbacks.containsFunction(&LLToolBrushLand::onSkyFlushIdle, (void*)this))
        {
            gIdleCallbacks.addFunction(&LLToolBrushLand::onSkyFlushIdle, (void*)this);
        }
        handled = true;
    }

    return handled;
}

void LLToolBrushLand::handleSelect()
{
    gEditMenuHandler = this;

    if (gFloaterTools)
    {
        gFloaterTools->setStatusText("modifyland");
    }
//  if (!mBrushSelected)
    {
        mBrushSelected = true;
    }
}


void LLToolBrushLand::handleDeselect()
{
    mPickLevel = false;   // [WOLF TERRAFORM 2026-10-23/] a pick is for this tool only
    if( gEditMenuHandler == this )
    {
        gEditMenuHandler = NULL;
    }
    LLViewerParcelMgr::getInstance()->setSelectionVisible(true);
    mBrushSelected = false;
}

// Draw the area that will be affected.
void LLToolBrushLand::render()
{
    if(mGotHover)
    {
        //LL_INFOS() << "LLToolBrushLand::render()" << LL_ENDL;
        LLVector3d spot;
        LLViewerRegion* sky_region = nullptr;
        S32 sky_level = 0;
        if(pickSpot(mMouseX, mMouseY, spot, &sky_region, &sky_level))   // [SKY LAND TOOLS 2026-11-10/] was mousePointOnLandGlobal
        {
            spot.mdV[VX] = floor( spot.mdV[VX] + 0.5 );
            spot.mdV[VY] = floor( spot.mdV[VY] + 0.5 );

            mBrushSize = wolf_land_brush_size();

            // [SKY LAND TOOLS 2026-11-10] over a sky level: the lines stand on that level's ground (and only where it has some)
            WolfTerrainLevels* sky = sky_level && sky_region ? sky_region->getWolfSkyLevels() : nullptr;
            const WolfTerrainLevels::Level* level = sky ? sky->findLevel(sky_level) : nullptr;
            if (level)
            {
                auto z_at = [&](S32 i, S32 j, F32& z) { return sky->sample(*level, i, j, z); };
                renderOverlayLines(z_at, (S32)sky_region->getWidth(), sky_region->getPosRegionFromGlobal(spot), gAgent.getPosAgentFromGlobal(spot));
                mGotHover = false;
                return;
            }

            region_list_t regions;
            determineAffectedRegions(regions, spot);

            // Now, for each region, render the overlay
            // [WOLF TERRAFORM 2026-11-09] Ticket 577 (Jimmy, SENJA 5120 m): the outline is drawn in agent space, which was the
            // agent region's frame (stock: gAgent.getRegion()->getPosRegionFromGlobal). On a region wider than 2048 m the agent
            // origin is rebased near the camera (llagent.h WOLF_ORIGIN_REBASE_*, LLAgent::updateHugeRegionOrigin), so the region
            // frame is no longer agent space and the lines were drawn 1024 m multiples away, off screen. getPosAgentFromGlobal is
            // pos - mAgentOriginGlobal, and mAgentOriginGlobal is the region origin whenever no rebase happened (LLAgent::setRegion),
            // so a normal region gets exactly the stock value.
            LLVector3 pos_world = gAgent.getPosAgentFromGlobal(spot);
            for(region_list_t::iterator iter = regions.begin();
                iter != regions.end(); ++iter)
            {
                LLViewerRegion* region = *iter;
                renderOverlay(region->getLand(),
                              region->getPosRegionFromGlobal(spot),
                              pos_world);
            }
        }
        mGotHover = false;
    }
}

/*
 * Draw vertical lines from each vertex straight up in world space
 * with lengths indicating the current "strength" slider.
 * Decorate the tops and bottoms of the lines like this:
 *
 *      Raise        Revert
 *      /|\           ___
 *       |             |
 *       |             |
 *
 *      Rough        Smooth
 *      /|\           ___
 *       |             |
 *       |             |
 *      \|/..........._|_
 *
 *      Lower        Flatten
 *       |             |
 *       |             |
 *      \|/..........._|_
 */
void LLToolBrushLand::renderOverlay(LLSurface& land, const LLVector3& pos_region,
                                    const LLVector3& pos_world)
{
    // [SKY LAND TOOLS 2026-11-10] the lines are drawn by renderOverlayLines; on the ground every grid point has a height
    auto z_at = [&land](S32 i, S32 j, F32& z) { z = land.getZ(i, j); return true; };
    renderOverlayLines(z_at, (S32)land.mGridsPerEdge, pos_region, pos_world);
}

void LLToolBrushLand::renderOverlayLines(const std::function<bool(S32, S32, F32&)>& z_at, S32 grids, const LLVector3& pos_region,
                                         const LLVector3& pos_world)
{
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.pushMatrix();
    gGL.translatef(0.0f, 0.0f, 1.0f);

    S32 i = (S32) pos_region.mV[VX];
    S32 j = (S32) pos_region.mV[VY];
    S32 half_edge = llfloor(mBrushSize);
    S32 radioAction = gSavedSettings.getS32("RadioLandBrushAction");
    F32 force = gSavedSettings.getF32("LandBrushForce"); // .1 to 100?

    // [WOLF TERRAFORM 2026-10-24] Jimmy (20x20): "sometimes when building and u see the bulldoze icon, u dont see the area to
    // be terraformed at all". The lines start 1 m over the heightfield (the translate above), so a floor prim or mesh over the ground (or
    // distant terrain drawn coarser than its heights) hid all of them behind the depth test. Paul agreed (10-24) it should show
    // through - a faint pass with no depth test first, then the usual bright pass where nothing is in front.
    // Source of the pass state: render_hud_elements (pipeline.cpp) LLGLSUIDefault - blending is on here.
    const LLColor4 hidden_color(OVERLAY_COLOR.mV[VRED], OVERLAY_COLOR.mV[VGREEN], OVERLAY_COLOR.mV[VBLUE], 0.35f);
    auto draw_lines = [&]()
    {
        gGL.begin(LLRender::LINES);
        for(S32 di = -half_edge; di <= half_edge; di++)
        {
            if((i+di) < 0 || (i+di) >= grids) continue;
            for(S32 dj = -half_edge; dj <= half_edge; dj++)
            {
                if( (j+dj) < 0 || (j+dj) >= grids ) continue;
                F32 wz;
                if (!z_at(i + di, j + dj, wz)) continue;
                const F32
                    wx = pos_world.mV[VX] + di,
                    wy = pos_world.mV[VY] + dj,
                    norm_dist = sqrt((float)di*di + dj*dj) / half_edge,
                    force_scale = sqrt(2.f) - norm_dist, // 1 at center, 0 at corner
                    wz2 = wz + .2f + (.2f + force/100.f) * force_scale, // top vertex
                    tic = .075f; // arrowhead size
                // vertical line
                gGL.vertex3f(wx, wy, wz);
                gGL.vertex3f(wx, wy, wz2);
                if(radioAction == E_LAND_RAISE || radioAction == E_LAND_NOISE) // up arrow
                {
                    gGL.vertex3f(wx, wy, wz2);
                    gGL.vertex3f(wx+tic, wy, wz2-tic);
                    gGL.vertex3f(wx, wy, wz2);
                    gGL.vertex3f(wx-tic, wy, wz2-tic);
                }
                if(radioAction == E_LAND_LOWER || radioAction == E_LAND_NOISE) // down arrow
                {
                    gGL.vertex3f(wx, wy, wz);
                    gGL.vertex3f(wx+tic, wy, wz+tic);
                    gGL.vertex3f(wx, wy, wz);
                    gGL.vertex3f(wx-tic, wy, wz+tic);
                }
                if(radioAction == E_LAND_REVERT || radioAction == E_LAND_SMOOTH) // flat top
                {
                    gGL.vertex3f(wx-tic, wy, wz2);
                    gGL.vertex3f(wx+tic, wy, wz2);
                }
                if(radioAction == E_LAND_LEVEL || radioAction == E_LAND_SMOOTH) // flat bottom
                {
                    gGL.vertex3f(wx-tic, wy, wz);
                    gGL.vertex3f(wx+tic, wy, wz);
                }
            }
        }
        gGL.end();
    };
    {
        LLGLDepthTest no_depth(GL_FALSE);
        gGL.color4fv(hidden_color.mV);
        draw_lines();
    }
    {
        LLGLDepthTest depth(GL_TRUE);
        gGL.color4fv(OVERLAY_COLOR.mV);
        draw_lines();
    }

    gGL.popMatrix();
}

void LLToolBrushLand::determineAffectedRegions(region_list_t& regions,
                                               const LLVector3d& spot ) const
{
    LLVector3d corner(spot);
    corner.mdV[VX] -= (mBrushSize / 2);
    corner.mdV[VY] -= (mBrushSize / 2);
    LLViewerRegion* region = NULL;
    region = LLWorld::getInstance()->getRegionFromPosGlobal(corner);
    if(region && regions.find(region) == regions.end())
    {
        regions.insert(region);
    }
    corner.mdV[VY] += mBrushSize;
    region = LLWorld::getInstance()->getRegionFromPosGlobal(corner);
    if(region && regions.find(region) == regions.end())
    {
        regions.insert(region);
    }
    corner.mdV[VX] += mBrushSize;
    region = LLWorld::getInstance()->getRegionFromPosGlobal(corner);
    if(region && regions.find(region) == regions.end())
    {
        regions.insert(region);
    }
    corner.mdV[VY] -= mBrushSize;
    region = LLWorld::getInstance()->getRegionFromPosGlobal(corner);
    if(region && regions.find(region) == regions.end())
    {
        regions.insert(region);
    }
}

// static
void LLToolBrushLand::onIdle( void* brush_tool )
{
    LLToolBrushLand* self = reinterpret_cast<LLToolBrushLand*>(brush_tool);

    if( LLToolMgr::getInstance()->getCurrentTool() == self )
    {
        self->brush();
    }
    else
    {
        gIdleCallbacks.deleteFunction( &LLToolBrushLand::onIdle, self );
    }
}

void LLToolBrushLand::onMouseCaptureLost()
{
    gIdleCallbacks.deleteFunction(&LLToolBrushLand::onIdle, this);
    // [W104 FEATFIX 2026-11-11 B5] the mouse taken away mid-stroke on sky land (not by a mouse-up): what is left is still sent,
    // so the next stroke is never refused for good with "Still sending your last change"
    if (mSkyLevel && !gIdleCallbacks.containsFunction(&LLToolBrushLand::onSkyFlushIdle, (void*)this))
    {
        gIdleCallbacks.addFunction(&LLToolBrushLand::onSkyFlushIdle, (void*)this);
    }
}

// [WOLF TERRAFORM 2026-10-23] DESIGN.md 0 / 1.3: "Undo does nothing" - the viewer sent UndoLand, and OpenSim's handler is empty
// (TerrainModule.cs:1491-1493 client_OnLandUndo, :1998-2000 StoreUndoState). Edit > Undo is greyed for the land brush now
// (lltoolbrush.h canUndo), and the Build window's land status says why; the Terraform window keeps its own steps with Undo.

// static
/*
void LLToolBrushLand::redo()
{
    for(region_list_t::iterator iter = mLastAffectedRegions.begin();
        iter != mLastAffectedRegions.end(); ++iter)
    {
        LLViewerRegion* regionp = *iter;
        gMessageSystem->newMessageFast(_PREHASH_RedoLand);
        gMessageSystem->nextBlockFast(_PREHASH_AgentData);
        gMessageSystem->addUUIDFast(_PREHASH_AgentID, gAgent.getID() );
        gMessageSystem->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        gMessageSystem->sendMessage(regionp->getHost());
    }
}*/

// [WOLF TERRAFORM 2026-10-23 Phase 3] Edit > Undo where the agent's region has the WolfTerraform module (wolfterraincap.h CAP): its
// UndoLand puts back the agent's last brush stroke or flood there, never a Terraform window job. The message is the one the old
// undo() sent (git HEAD lltoolbrush.cpp, removed above), to that region only - mLastAffectedRegions is cleared on mouse-up
// (handleMouseUp), so it never named the region after a stroke. A refusal comes back as the region's alert.
bool LLToolBrushLand::canUndo() const
{
    LLViewerRegion* regionp = gAgent.getRegion();
    // [SKY LAND TOOLS 2026-11-10] after a stroke on sky land: its changes ([W104 FEATFIX 2026-11-11 B1] its own cells, skyUndoCoro)
    if (regionp && mSkyUndo.mStroke && !mSkyUndo.mBefore.empty() && regionp->getHandle() == mSkyUndo.mRegion)
        return !mSkyInFlight && mSkyDirty.empty() && !mSkyUndoBusy;
    return regionp && !regionp->getCapability("WolfTerraform").empty();
}

// [W104 FEATFIX 2026-11-11 B1] lltoolbrush.h. The stroke's cells go back to their heights before it, through the WolfSkyLevels cap's
// "apply" (WolfTerrainLevels::applyBody, labelled "undo land brush"), at most SKY_BLOCK metres a side a request, NaN where a cell
// is left alone - as the Terraform window's sky Undo (wolfterraform.cpp levelCoro). The region's own "undo" is never sent: it
// undoes the caller's LAST change, whatever made it (WolfSkyLevelsModule.Edit.cs Undo), so the Terraform window's history and
// this brush's never take each other's steps. Like that window's Undo, a cell someone changed since the stroke is put back too.
// static
void LLToolBrushLand::skyUndoCoro(U64 handle, S32 level, std::unordered_map<U32, F32> before)
{
    constexpr S32 BUSY_TRIES = 6;   // wolfterraform.cpp levelCoro BUSY_TRIES
    LLToolBrushLand* self = LLToolBrushLand::getInstance();
    // the 256 m blocks with cells to put back
    std::map<std::pair<S32, S32>, std::vector<U32>> blocks;
    for (const auto& kv : before)
    {
        const S32 x = (S32)(kv.first & 0xFFFFu), y = (S32)(kv.first >> 16);
        blocks[std::make_pair(x / SKY_BLOCK, y / SKY_BLOCK)].push_back(kv.first);
    }
    S32 changed = 0, refused = 0;
    std::string reason, failed;
    for (const auto& b : blocks)
    {
        S32 x0 = S32_MAX, y0 = S32_MAX, x1 = -1, y1 = -1;
        for (U32 k : b.second)
        {
            const S32 x = (S32)(k & 0xFFFFu), y = (S32)(k >> 16);
            x0 = llmin(x0, x); y0 = llmin(y0, y); x1 = llmax(x1, x); y1 = llmax(y1, y);
        }
        const S32 w = x1 - x0 + 1, h = y1 - y0 + 1;
        std::vector<F32> heights((size_t)w * (size_t)h, std::numeric_limits<F32>::quiet_NaN());
        for (U32 k : b.second)
        {
            const S32 x = (S32)(k & 0xFFFFu), y = (S32)(k >> 16);
            heights[(size_t)(y - y0) * (size_t)w + (size_t)(x - x0)] = before[k];
        }
        const LLSD body = WolfTerrainLevels::applyBody(level, x0, y0, w, h, heights, "undo land brush");
        LLSD answer;
        for (S32 tries = 0; ; ++tries)
        {
            // the region takes changes only from an avatar standing in it (wolfterrainlevels.h post), looked up after every wait
            LLViewerRegion* rgn = gAgent.getRegion();
            if (!rgn || rgn->getHandle() != handle)
            {
                answer = LLSD::emptyMap();
                answer["message"] = "you left the region";
                break;
            }
            answer = WolfTerrainLevels::postAndWait(rgn, body);
            if (answer["success"].asBoolean() || !answer["busy"].asBoolean() || tries + 1 >= BUSY_TRIES) break;
            llcoro::suspendUntilTimeout((F32)llmax(1, answer["retry_after"].asInteger()));
        }
        if (!answer["success"].asBoolean())
        {
            failed = answer["message"].asString();
            break;
        }
        changed += answer["changed"].asInteger();
        if (answer["changed"].asInteger() > 0)
        {
            // [W104 FEATFIX review 2026-11-11] the brush's Undo is itself a change the region remembers (wolfterrainlevels.h),
            // in the region the request was made for ([review 2] its handle, not wherever the agent is now)
            WolfTerrainLevels::noteChange(handle, WolfTerrainLevels::EDITOR_BRUSH);
        }
        const S32 r = answer["refused"].asInteger() + answer["no_room"].asInteger();
        refused += r;
        if (r > 0 && reason.empty()) reason = answer["message"].asString();
    }
    self->mSkyUndoBusy = false;
    std::string text;
    if (!failed.empty())
    {
        text = llformat("Undo of your last brush stroke on the sky land stopped (%d m\xC2\xB2 put back): %s", changed, failed.c_str());
        LLNotificationsUtil::add("GenericAlert", LLSD().with("MESSAGE", text));
    }
    else if (refused > 0)
    {
        text = llformat("Your last brush stroke on the sky land is undone where the region allowed it: %d m\xC2\xB2 put back, %d m\xC2\xB2 not. %s",
                        changed, refused, reason.c_str());
    }
    else
    {
        text = llformat("Your last brush stroke on the sky land is undone (%d m\xC2\xB2 put back).", changed);
    }
    if (gFloaterTools) gFloaterTools->setStatusText(text);
    LL_INFOS("WolfSkyBrush") << "brush undo: " << changed << " changed, " << refused << " refused" << (failed.empty() ? "" : ", stopped: ") << failed << LL_ENDL;
}

void LLToolBrushLand::undo()
{
    LLViewerRegion* regionp = gAgent.getRegion();
    if (regionp && mSkyUndo.mStroke && !mSkyUndo.mBefore.empty() && regionp->getHandle() == mSkyUndo.mRegion)
    {
        if (mSkyInFlight || !mSkyDirty.empty() || mSkyUndoBusy) return;
        // [W104 FEATFIX 2026-11-11 B1] one Undo a stroke: the record is handed to the coroutine and gone from here
        SkyUndo u;
        std::swap(u, mSkyUndo);
        mSkyUndoBusy = true;
        if (gFloaterTools) gFloaterTools->setStatusText("Putting back your last brush stroke on the sky land...");
        const U64 handle = u.mRegion;
        const S32 level = u.mLevel;
        LLCoros::instance().launch("WolfSkyBrushUndo", [handle, level, before = std::move(u.mBefore)]() mutable
        {
            skyUndoCoro(handle, level, std::move(before));
        });
        return;
    }
    if (!regionp || regionp->getCapability("WolfTerraform").empty()) return;
    gMessageSystem->newMessageFast(_PREHASH_UndoLand);
    gMessageSystem->nextBlockFast(_PREHASH_AgentData);
    gMessageSystem->addUUIDFast(_PREHASH_AgentID, gAgent.getID() );
    gMessageSystem->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
    gMessageSystem->sendMessage(regionp->getHost());
}

// static
bool LLToolBrushLand::canTerraformRegion(LLViewerRegion* regionp) const
{
    if (!regionp) return false;
    if (regionp->canManageEstate()) return true;
    return !regionp->getRegionFlag(REGION_FLAGS_BLOCK_TERRAFORM);
}

// static
bool LLToolBrushLand::canTerraformParcel(LLViewerRegion* regionp) const
{
    LLParcel* selected_parcel = LLViewerParcelMgr::getInstance()->getHoverParcel();
    bool is_terraform_allowed = false;
    if (selected_parcel)
    {
        bool owner_release = LLViewerParcelMgr::isParcelOwnedByAgent(selected_parcel, GP_LAND_ALLOW_EDIT_LAND);
        is_terraform_allowed = ( gAgent.canManageEstate() || (selected_parcel->getOwnerID() == regionp->getOwner()) || owner_release);
    }

    return is_terraform_allowed;
}


// static
void LLToolBrushLand::alertNoTerraformRegion(LLViewerRegion* regionp)
{
    if (!regionp) return;

    LLSD args;
    args["REGION"] = regionp->getName();
    LLNotificationsUtil::add("RegionNoTerraforming", args);

}

// static
void LLToolBrushLand::alertNoTerraformParcel()
{
    LLParcel* selected_parcel = LLViewerParcelMgr::getInstance()->getHoverParcel();
    if (selected_parcel)
    {
        LLSD args;
        args["PARCEL"] = selected_parcel->getName();
        LLNotificationsUtil::add("ParcelNoTerraforming", args);
    }

}

///============================================================================
/// Local function definitions
///============================================================================
