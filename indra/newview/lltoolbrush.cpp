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
    if( gViewerWindow->mousePointOnLandGlobal( mMouseX, mMouseY, &spot ) )
    {
        // Round to nearest X,Y grid
        spot.mdV[VX] = floor( spot.mdV[VX] + 0.5 );
        spot.mdV[VY] = floor( spot.mdV[VY] + 0.5 );

        modifyLandAtPointGlobal(spot, gKeyboard->currentMask(true), dab_seconds);
    }
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
    LLVector3d spot;
    if( gViewerWindow->mousePointOnLandGlobal( x, y, &spot ) )
    {
        // Round to nearest X,Y grid
        spot.mdV[VX] = floor( spot.mdV[VX] + 0.5 );
        spot.mdV[VY] = floor( spot.mdV[VY] + 0.5 );

        LLRegionPosition region_position( spot );
        LLViewerRegion* regionp = region_position.getRegion();

        // [WOLF TERRAFORM 2026-10-23] Pick: the ground's height under the click becomes the Level to height; nothing is changed
        if (mPickLevel && regionp)
        {
            mPickLevel = false;
            const F32 z = regionp->getLand().resolveHeightRegion(region_position.getPositionRegion());
            gSavedSettings.setF32("WolfLandLevelHeight", ll_round(z * 100.f) / 100.f);
            gSavedSettings.setBOOL("WolfLandLevelUse", true);
            if (gFloaterTools) gFloaterTools->setStatusText(llformat("Flatten now levels to %.2f m (the ground you clicked).", z));
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
    if( gViewerWindow->mousePointOnLandGlobal( mMouseX, mMouseY, &spot ) )
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
        if(gViewerWindow->mousePointOnLandGlobal(mMouseX, mMouseY, &spot))
        {
            spot.mdV[VX] = floor( spot.mdV[VX] + 0.5 );
            spot.mdV[VY] = floor( spot.mdV[VY] + 0.5 );

            mBrushSize = wolf_land_brush_size();

            region_list_t regions;
            determineAffectedRegions(regions, spot);

            // Now, for each region, render the overlay
            LLVector3 pos_world = gAgent.getRegion()->getPosRegionFromGlobal(spot);
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
            if((i+di) < 0 || (i+di) >= (S32)land.mGridsPerEdge) continue;
            for(S32 dj = -half_edge; dj <= half_edge; dj++)
            {
                if( (j+dj) < 0 || (j+dj) >= (S32)land.mGridsPerEdge ) continue;
                const F32
                    wx = pos_world.mV[VX] + di,
                    wy = pos_world.mV[VY] + dj,
                    wz = land.getZ(i + di, j + dj),
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
    return regionp && !regionp->getCapability("WolfTerraform").empty();
}

void LLToolBrushLand::undo()
{
    LLViewerRegion* regionp = gAgent.getRegion();
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
