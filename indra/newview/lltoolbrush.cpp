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
    F64 sky_interpolated_noise(F64 x, F64 y)
    {
        const S32 ix = (S32)x;
        const F64 fx = x - ix;
        const S32 iy = (S32)y;
        const F64 fy = y - iy;
        const F64 v1 = sky_smoothed_noise(ix, iy), v2 = sky_smoothed_noise(ix + 1, iy);
        const F64 v3 = sky_smoothed_noise(ix, iy + 1), v4 = sky_smoothed_noise(ix + 1, iy + 1);
        return sky_interpolate(sky_interpolate(v1, v2, fx), sky_interpolate(v3, v4, fx), fy);
    }
    F64 sky_perlin(F64 x, F64 y, S32 octaves, F64 persistence)
    {
        F64 total = 0.0;
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
            modifySkyLevelAtPoint(spot, dab_seconds);
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
void LLToolBrushLand::modifySkyLevelAtPoint(const LLVector3d& spot, F32 dab_seconds)
{
    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(mSkyRegion);
    WolfTerrainLevels* sky = regionp ? regionp->getWolfSkyLevels() : nullptr;
    const WolfTerrainLevels::Level* level = sky ? sky->findLevel(mSkyLevel) : nullptr;
    if (!level) return;
    const S32 action = gSavedSettings.getS32("RadioLandBrushAction");
    if (action < 0 || action > 4) return;   // Revert (5) has nothing to go back to on sky land (handleMouseDown says so)
    const F32 strength = dab_seconds * gSavedSettings.getF32("LandBrushForce");   // as modifyLandAtPointGlobal's Seconds
    const LLVector3 p = regionp->getPosRegionFromGlobal(spot);
    const F32 rx = p.mV[VX], ry = p.mV[VY];
    const F32 size = llmin(mBrushSize, SKY_MAX_BRUSH);
    const S32 W = (S32)regionp->getWidth();
    const S32 start_x = llmax(0, (S32)(rx - size + 0.5f)), start_y = llmax(0, (S32)(ry - size + 0.5f));
    const S32 end_x = llmin(W - 1, (S32)(rx + size + 0.5f)), end_y = llmin(W - 1, (S32)(ry + size + 0.5f));
    if (end_x < start_x || end_y < start_y) return;
    const F32 lo = level->mBase, hi = sky->maxHeight();
    const F32 rz = llclamp(wolf_flatten_height(action, mStartingZ), lo, hi);
    auto get = [&](S32 x, S32 y, F32& z) -> bool
    {
        auto it = mSkyWork.find(sky_key(x, y));
        if (it != mSkyWork.end()) { z = it->second; return true; }
        return sky->sample(*level, x, y, z);
    };
    auto put = [&](S32 x, S32 y, F32 z)
    {
        z = llclamp(z, lo, hi);
        F32 was;
        if (!get(x, y, was) || was == z) return;
        mSkyWork[sky_key(x, y)] = z;
        mSkyDirty.insert(sky_key(x, y));
    };
    if (action == 3)
    {
        // SmoothSphere.cs: the deltas from the heights before this dab, then applied (ssize at most 4, strength at most 1). Only
        // neighbours with ground count in the mean (the ground has every cell; a level's edge has none beyond it).
        const S32 ssize = llmin(4, (S32)(size + 0.5f));
        const F32 size2 = size * size;
        const F32 st = llmin(strength, 1.f);
        std::vector<std::pair<U32, F32>> tweaks;
        for (S32 x = start_x; x <= end_x; ++x)
        {
            const F32 dx2 = ((F32)x - rx) * ((F32)x - rx);
            for (S32 y = start_y; y <= end_y; ++y)
            {
                F32 here;
                if (!get(x, y, here)) continue;
                F32 df = (dx2 + ((F32)y - ry) * ((F32)y - ry)) / size2;
                if (df > 1.f) continue;
                df = st * (1.f - df);
                F32 sum = 0.f;
                S32 n = 0;
                for (S32 a = x - ssize; a <= x + ssize; ++a)
                    for (S32 b = y - ssize; b <= y + ssize; ++b)
                    {
                        F32 q;
                        if (a >= 0 && a < W && b >= 0 && b < W && get(a, b, q)) { sum += q; ++n; }
                    }
                tweaks.emplace_back(sky_key(x, y), df * (here - sum / (F32)n));   // n >= 1: (x, y) itself
            }
        }
        for (const auto& t : tweaks)
        {
            if (t.second == 0.f) continue;
            const S32 x = (S32)(t.first & 0xFFFFu), y = (S32)(t.first >> 16);
            F32 here;
            if (get(x, y, here)) put(x, y, here - t.second);
        }
        return;
    }
    const F32 cos_size = 0.5f * F_PI / size;   // RaiseSphere.cs / LowerSphere.cs: size = 0.5 * PI / size
    for (S32 x = start_x; x <= end_x; ++x)
    {
        const F32 dx2 = ((F32)x - rx) * ((F32)x - rx);
        for (S32 y = start_y; y <= end_y; ++y)
        {
            F32 here;
            if (!get(x, y, here)) continue;
            const F32 d2 = dx2 + ((F32)y - ry) * ((F32)y - ry);
            if (action == 1 || action == 2)
            {
                const F32 f = (F32)std::cos(std::sqrt((F64)d2) * cos_size);
                if (f > 0.f) put(x, y, action == 1 ? here + f * strength : here - f * strength);
            }
            else if (action == 0)
            {
                // FlattenSphere.cs: size *= 2 * size
                F32 df = d2 / (2.f * size * size);
                if (df > 1.f) continue;
                df = strength * (1.f - df);
                put(x, y, df >= 1.f ? rz : here + (rz - here) * df);
            }
            else
            {
                // NoiseSphere.cs: size *= size; PerlinNoise2D(x / Width, y / Height, 8, 1.0)
                F32 df = d2 / (size * size);
                if (df > 1.f) continue;
                df = strength * (1.f - df);
                put(x, y, here + (F32)sky_perlin((F64)x / (F64)W, (F64)y / (F64)W, 8, 1.0) * df);
            }
        }
    }
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
    sky->post(WolfTerrainLevels::applyBody(mSkyLevel, x0, y0, w, h, heights, "land brush"), [this, cells, handle](const LLSD& answer)
    {
        // the tool is a singleton (LLSingleton<LLToolBrushLand>): `this` outlives every request
        mSkyInFlight = false;
        if (answer["success"].asBoolean())
        {
            if (answer["changed"].asInteger() > 0)
            {
                ++mSkyStrokeSteps;   // WolfSkyLevelsModule.Edit.cs Apply: one undo step for a request that changed something
                mSkyUndoSteps = mSkyStrokeSteps;
                mSkyUndoRegion = handle;
            }
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
            const std::string text = answer["message"].asString();
            if (gFloaterTools) gFloaterTools->setStatusText(text);
            LLSD args;
            args["MESSAGE"] = "The sky land change was not kept: " + text;
            LLNotificationsUtil::add("GenericAlert", args);
        }
    });
}

// [SKY LAND TOOLS 2026-11-10] After a stroke on a level: what is left goes at the region's pace, then the stroke's heights are let go.
// static
void LLToolBrushLand::onSkyFlushIdle(void* brush_tool)
{
    LLToolBrushLand* self = reinterpret_cast<LLToolBrushLand*>(brush_tool);
    if (self->hasMouseCapture()) return;   // (a new stroke cannot start before this one is all sent: handleMouseDown)
    if (self->mSkyDirty.empty() && !self->mSkyInFlight)
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
        if (mSkyInFlight || !mSkyDirty.empty())
        {
            if (gFloaterTools) gFloaterTools->setStatusText("Still sending your last change to the sky land - try again in a moment.");
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
        mSkyStrokeSteps = 0;
        if (level)
        {
            mStartingZ = level_z;
            mSkyWork.clear();
            mSkyNextPost = 0.0;
        }
        else
        {
            mSkyUndoSteps = 0;   // Edit > Undo is the ground's again (UndoLand)
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
    // [SKY LAND TOOLS 2026-11-10] after a stroke on sky land: its changes, through the WolfSkyLevels cap's undo
    if (regionp && mSkyUndoSteps > 0 && regionp->getHandle() == mSkyUndoRegion) return !mSkyInFlight && mSkyDirty.empty();
    return regionp && !regionp->getCapability("WolfTerraform").empty();
}

// [SKY LAND TOOLS 2026-11-10] One "undo" a step (WolfSkyLevelsModule.Edit.cs Undo: the caller's last change), one after the other.
static void wolf_sky_undo_steps(U64 handle, S32 left)
{
    WolfTerrainLevels* sky = sky_of(handle);
    if (!sky || left <= 0) return;
    LLSD body = LLSD::emptyMap();
    body["op"] = "undo";
    sky->post(body, [handle, left](const LLSD& answer)
    {
        if (!answer["success"].asBoolean())
        {
            if (gFloaterTools) gFloaterTools->setStatusText("Undo on the sky land stopped: " + answer["message"].asString());
            return;
        }
        if (left > 1) wolf_sky_undo_steps(handle, left - 1);
        else if (gFloaterTools) gFloaterTools->setStatusText("Your last brush stroke on the sky land is undone.");
    });
}

void LLToolBrushLand::undo()
{
    LLViewerRegion* regionp = gAgent.getRegion();
    if (regionp && mSkyUndoSteps > 0 && regionp->getHandle() == mSkyUndoRegion)
    {
        if (mSkyInFlight || !mSkyDirty.empty()) return;
        const S32 steps = mSkyUndoSteps;
        mSkyUndoSteps = 0;
        wolf_sky_undo_steps(mSkyUndoRegion, steps);
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
