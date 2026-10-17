/**
 * @file wolfbuilder.cpp
 * @brief WolfViewer: the builder's helper. See wolfbuilder.h.
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

#include "llviewerprecompiledheaders.h"

#include "wolfbuilder.h"

#include <algorithm>
#include <set>
#include <vector>

#include "llagent.h"
#include "llappviewer.h"         // gDisconnected
#include "llbbox.h"
#include "llfloaterreg.h"        // instanceVisible
#include "llworld.h"             // resolveLandHeightAgent
#include "lldir.h"
#include "llenvironment.h"
#include "llfontgl.h"
#include "llgl.h"
#include "llhudobject.h"
#include "llhudtext.h"
#include "llmaniptranslate.h"    // getGridTexName
#include "llrender.h"
#include "llselectmgr.h"
#include "lltoolmgr.h"
#include "llviewercontrol.h"
#include "llviewernetwork.h"     // LLGridManager
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "pipeline.h"
#include "rlvactions.h"
#include "wolfobjectprops.h"

namespace
{
    // How often the object list is swept for new names. Tags already up are moved every frame.
    const F32 SCAN_SECS = 0.5f;
    // The nearest this many root prims get a tag; a crowded build does not become a wall of text.
    const size_t MAX_TAGS = 100;
    // Above the prim's top, so the name clears the prim and any hover text the prim already shows
    // (LLViewerObject::updateText puts that at the prim's own position).
    const F32 TAG_LIFT = 0.25f;
    // The tags' colour: a warm cream that is neither chat white nor hover-text default white.
    const LLColor4 TAG_COLOR(1.f, 0.93f, 0.7f, 1.f);
}

WolfBuilder::WolfBuilder()
{
}

WolfBuilder::~WolfBuilder()
{
    clearNameTags();
}

// ---------------------------------------------------------------------------------------------------
// Builders Mode
// ---------------------------------------------------------------------------------------------------

// Source: llviewermenu.cpp:798-826 wolf_toggle_eep() "off" branch, itself a copy of
// LLWorldEnvSettings::handleEvent "noon" (llviewermenu.cpp:12274-12303 legacy path,
// 12329-12338 EEP path). The snapshot first: llenvironment.h:145 hasEnvironment, :164-166
// getEnvironmentDay / DayLength / DayOffset, :168-169 getEnvironmentFixedSky / Water (resolve false
// = this instance's own sky and water, llenvironment.cpp:1616-1619).
bool WolfBuilder::applyBuildersMode(std::string& why_not)
{
    if (mApplied)
    {
        return true;
    }
    // Source: llviewermenu.cpp:800-801 - the menu refuses under RLV @setenv; so do we, and we say so.
    if (!RlvActions::canChangeEnvironment())
    {
        why_not = "An RLV restriction is stopping environment changes right now.";
        return false;
    }

    LLEnvironment& env = LLEnvironment::instance();

    // Remember what the local environment holds.
    mHadLocal = env.hasEnvironment(LLEnvironment::ENV_LOCAL);
    mSavedSkyFixed = false;
    mSavedDay.reset();
    mSavedSky.reset();
    mSavedWater.reset();
    mLegacyMidday.reset();
    if (mHadLocal)
    {
        mSavedDay = env.getEnvironmentDay(LLEnvironment::ENV_LOCAL);
        mSavedDayLength = env.getEnvironmentDayLength(LLEnvironment::ENV_LOCAL);
        mSavedDayOffset = env.getEnvironmentDayOffset(LLEnvironment::ENV_LOCAL);
        mSavedSky = env.getEnvironmentFixedSky(LLEnvironment::ENV_LOCAL);
        mSavedWater = env.getEnvironmentFixedWater(LLEnvironment::ENV_LOCAL);
        // Source: llenvironment.cpp:1303-1308 - a fixed sky sets NO_ANIMATE_SKY on the instance, and
        // :1273-1278 - a day cycle clears the flags. The day getter alone cannot tell them apart.
        mSavedSkyFixed = (env.getEnvironmentAnimateFlags(LLEnvironment::ENV_LOCAL) & LLEnvironment::DayInstance::NO_ANIMATE_SKY) != 0;
        if (!mSavedDay && !mSavedSky && !mSavedWater)
        {
            // An instance with nothing in it is the same as none: restoring means clearing.
            mHadLocal = false;
        }
    }

#ifdef OPENSIM
    // Source: llviewermenu.cpp:805-823 - a grid without the extended environment gets the legacy
    // windlight Midday preset converted to a sky.
    if (LLGridManager::getInstance()->isInOpenSim() && !env.isExtendedEnvironmentEnabled())
    {
        LLSD messages;
        LLSettingsSky::ptr_t legacysky = LLEnvironment::createSkyFromLegacyPreset(
            gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "windlight", "skies", "Midday.xml"), messages);
        if (!legacysky)
        {
            LL_WARNS("WolfBuilder") << "Legacy windlight conversion failed for Midday, existing env unchanged." << LL_ENDL;
            why_not = "The Midday sky preset could not be loaded, so the sky was left as it is.";
            return false;
        }
        env.setEnvironment(LLEnvironment::ENV_LOCAL, legacysky);
        env.setSelectedEnvironment(LLEnvironment::ENV_LOCAL);
        env.updateEnvironment(LLEnvironment::TRANSITION_FAST, true);
        mLegacyMidday = legacysky;
        mApplied = true;
        return true;
    }
#endif
    // Source: llviewermenu.cpp:824-825 - the EEP Midday asset, with the user's manual transition time.
    env.setManualEnvironment(LLEnvironment::ENV_LOCAL, LLEnvironment::KNOWN_SKY_MIDDAY);
    env.setSelectedEnvironment(LLEnvironment::ENV_LOCAL);
    mApplied = true;
    return true;
}

// Source: llviewermenu.cpp:829-832 (the "region" item: reflection probes reset, local cleared, instant
// transition) for a user who had no local sky; llviewermenu.cpp:12292-12294 (setEnvironment on
// ENV_LOCAL, select it, updateEnvironment TRANSITION_FAST forced) for one who had a fixed sky;
// llenvironment.h:146 setEnvironment(day, daylength, dayoffset) for one who had a day cycle.
void WolfBuilder::restoreEnvironment()
{
    if (!mApplied)
    {
        return;
    }
    mApplied = false;

    LLEnvironment& env = LLEnvironment::instance();
    // The sky as it stands now. If the user picked another sky after Builders Mode went on, that is
    // their choice and it stays; we only undo our own Midday. A null sky means the Midday asset has
    // not landed yet - still ours.
    LLSettingsSky::ptr_t now = env.getEnvironmentFixedSky(LLEnvironment::ENV_LOCAL);
    const bool still_ours = !now
        || now == mLegacyMidday
        || now == mSavedSky
        || now->getAssetId() == LLEnvironment::KNOWN_SKY_MIDDAY;

    if (still_ours && RlvActions::canChangeEnvironment())
    {
        if (!mHadLocal)
        {
            gPipeline.mReflectionMapManager.reset();
            env.clearEnvironment(LLEnvironment::ENV_LOCAL);
            env.setSelectedEnvironment(LLEnvironment::ENV_LOCAL, LLEnvironment::TRANSITION_INSTANT);
        }
        else if (mSavedDay && !mSavedSkyFixed)
        {
            env.setEnvironment(LLEnvironment::ENV_LOCAL, mSavedDay, mSavedDayLength, mSavedDayOffset);
            env.setSelectedEnvironment(LLEnvironment::ENV_LOCAL);
            env.updateEnvironment(LLEnvironment::TRANSITION_FAST, true);
        }
        else
        {
            env.setEnvironment(LLEnvironment::ENV_LOCAL, mSavedSky, mSavedWater);
            env.setSelectedEnvironment(LLEnvironment::ENV_LOCAL);
            env.updateEnvironment(LLEnvironment::TRANSITION_FAST, true);
        }
    }

    mHadLocal = false;
    mSavedSkyFixed = false;
    mSavedDay.reset();
    mSavedSky.reset();
    mSavedWater.reset();
    mLegacyMidday.reset();
}

// ---------------------------------------------------------------------------------------------------
// The grid
// ---------------------------------------------------------------------------------------------------

// static
bool WolfBuilder::snapOriginEnabled()
{
    static LLCachedControl<bool> on(gSavedSettings, "WolfBuilderSnapOrigin", false);
    return on();
}

// static
// Source: llviewerregion.h:341 getPosAgentFromRegion - region metres to agent metres. The world grid's
// own origin is the agent region's corner (llselectmgr.cpp getGrid, region->getOriginAgent(): agent
// (0,0,0) unless a huge-region rebase moved agent space); so with no region the builder's origin is used as it is.
LLVector3 WolfBuilder::snapOriginAgent()
{
    const LLVector3 origin = gSavedSettings.getVector3("WolfBuilderGridOrigin");
    LLViewerRegion* region = gAgent.getRegion();
    return region ? region->getPosAgentFromRegion(origin) : origin;
}

// Source: llmaniptranslate.cpp:1626-1661 renderGrid - a fan of quads whose alpha falls off with
// distance from the centre - and :2297-2316 renderGridVert, whose three plane cases are EPlane:
// LL_YZ_PLANE (0, x, y), LL_XZ_PLANE (x, 0, y), LL_XY_PLANE (x, y, 0).
void WolfBuilder::drawGridTiles(EPlane plane, F32 size, F32 r, F32 g, F32 b, F32 a)
{
    const F32 d = size * 0.5f;
    auto vert = [plane, size, r, g, b, a](F32 dx, F32 dy)
    {
        const F32 da = sqrtf(llmax(0.0f, 1.0f - sqrtf(dx * dx + dy * dy) / size)) * a;
        gGL.texCoord2f(dx, dy);
        gGL.color4f(r, g, b, da);
        switch (plane)
        {
        case PLANE_YZ: gGL.vertex3f(0.f, dx, dy); break;
        case PLANE_XZ: gGL.vertex3f(dx, 0.f, dy); break;
        case PLANE_XY: gGL.vertex3f(dx, dy, 0.f); break;
        }
    };
    for (F32 xx = -size - d; xx < size + d; xx += d)
    {
        gGL.begin(LLRender::TRIANGLE_STRIP);
        for (F32 yy = -size - d; yy < size + d; yy += d)
        {
            vert(xx, yy);
            vert(xx + d, yy);
            vert(xx, yy + d);
            vert(xx + d, yy + d);
        }
        gGL.end();
    }
}

// Source: llmaniptranslate.cpp:1493-1594 renderSnapGuides, the planar branch, for one plane:
// - grid_center = (centre - grid origin) * ~grid_rotation; u, v and the scales are the plane's two
//   axes of it (:1498-1532: YZ -> VY,VZ; XZ -> VX,VZ; XY -> VX,VY);
// - the modelview moves to the centre and turns by the grid rotation (:1537-1542);
// - the texture matrix scales by 1 / scale, with neither scale more than 4x the other (:1547-1562),
//   then shifts by (u, v), so a line falls on every grid step counted from the origin;
// - a black underlay at 0.16 of the opacity, then the grid texture (:142-150 getGridTexName) at the
//   full opacity in the line colour (:1582-1589), depth tested, not depth written (:1568).
void WolfBuilder::drawGridPlane(EPlane plane, const LLVector3& center_agent, const LLQuaternion& rotation,
                                const LLVector3& grid_origin_agent, const LLVector3& scale, F32 size,
                                const LLColor4& line_color, F32 alpha, U32 tex)
{
    LLVector3 grid_center = center_agent - grid_origin_agent;
    grid_center *= ~rotation;

    F32 u = 0.f, v = 0.f, usc = 1.f, vsc = 1.f;
    switch (plane)
    {
    case PLANE_YZ: u = grid_center.mV[VY]; v = grid_center.mV[VZ]; usc = scale.mV[VY]; vsc = scale.mV[VZ]; break;
    case PLANE_XZ: u = grid_center.mV[VX]; v = grid_center.mV[VZ]; usc = scale.mV[VX]; vsc = scale.mV[VZ]; break;
    case PLANE_XY: u = grid_center.mV[VX]; v = grid_center.mV[VY]; usc = scale.mV[VX]; vsc = scale.mV[VY]; break;
    }
    if (usc <= 0.f || vsc <= 0.f)
    {
        return;   // a flat selection's local grid has no step along its flat axis
    }

    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.pushMatrix();
    F32 x, y, z, angle_radians;
    rotation.getAngleAxis(&angle_radians, &x, &y, &z);
    gGL.translatef(center_agent.mV[VX], center_agent.mV[VY], center_agent.mV[VZ]);
    gGL.rotatef(angle_radians * RAD_TO_DEG, x, y, z);

    gGL.matrixMode(LLRender::MM_TEXTURE);
    gGL.pushMatrix();
    usc = 1.0f / usc;
    vsc = 1.0f / vsc;
    while (usc > vsc * 4.0f)
    {
        usc *= 0.5f;
    }
    while (vsc > usc * 4.0f)
    {
        vsc *= 0.5f;
    }
    gGL.scalef(usc, vsc, 1.0f);
    gGL.translatef(u, v, 0.f);

    // Source: llmaniptranslate.cpp:1567-1576 "draw grid behind objects" - where something is in front
    // of the grid (GL_GREATER: the scene is nearer), the grid still darkens it faintly. The ground grid
    // is one flat plane at the land height under its centre, so on a slope the rising land covers part
    // of it; without this pass that part simply vanished.
    {
        LLGLDepthTest gls_depth_behind(GL_TRUE, GL_FALSE, GL_GREATER);
        gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, tex);
        gGL.flush();
        gGL.blendFunc(LLRender::BF_ZERO, LLRender::BF_ONE_MINUS_SOURCE_ALPHA);
        drawGridTiles(plane, size, 0.9f, 0.9f, 0.9f, alpha * 0.15f);
        gGL.flush();
        gGL.setSceneBlendType(LLRender::BT_ALPHA);
    }

    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    drawGridTiles(plane, size, 0.f, 0.f, 0.f, alpha * 0.16f);
    gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, tex);
    drawGridTiles(plane, size, line_color.mV[VRED], line_color.mV[VGREEN], line_color.mV[VBLUE], alpha);
    gGL.flush();

    gGL.popMatrix();
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.popMatrix();
}

// static
// The grids are building aids, so they show while one of the two floaters to build with is open: the
// Build floater (lltoolmgr.cpp:400-410 inBuildMode, which is what the snap grid itself needs) or the
// Wolf Builder floater (llfloaterreg.h:146 instanceVisible) - Paul switched the grid on from Wolf
// Builder with the Build floater closed (his log 10-13: toolbox floater closed 05:56:51, wolf_builder
// open until 05:58:57) and the grid, gated on build mode alone, never drew.
bool WolfBuilder::gridWindowsOpen()
{
    return LLToolMgr::getInstance()->inBuildMode() || LLFloaterReg::instanceVisible("wolf_builder");
}

// Spacing: llselectmgr.cpp:1564 the world grid's step is GridResolution; size: llmaniptranslate.cpp:1898
// GridDrawSize; opacity: :1122 GridOpacity.
void WolfBuilder::renderGrid()
{
    static LLCachedControl<bool> ground_on(gSavedSettings, "WolfBuilderGrid", false);
    static LLCachedControl<bool> object_on(gSavedSettings, "WolfBuilderObjectGrid", false);
    // [WolfViewer 2026-10-13] Paul: "it should only draw it if the builders mode check box is ticked"
    // (llfloatertools.cpp onCommitWolfBuildersMode writes WolfBuildersMode).
    static LLCachedControl<bool> builders_mode(gSavedSettings, "WolfBuildersMode", false);
    // The Wolf Builder floater shows this dependency (its Builders Mode box, and the grid switches
    // greyed out with a line saying why while it is off; wolffloaterbuilder.cpp refreshDrawSwitches).
    if (!builders_mode() || (!ground_on() && !object_on()) || gDisconnected || !gAgent.getRegion() || !gridWindowsOpen())
    {
        logGridDrawing(false, !builders_mode() ? "Builders Mode is off"
                            : (!ground_on() && !object_on()) ? "both grid switches are off"
                            : (gDisconnected || !gAgent.getRegion()) ? "not in a region"
                            : "neither the Build nor the Wolf Builder floater is open");
        return;
    }
    const U32 tex = LLManipTranslate::getGridTexName();
    if (!tex)
    {
        logGridDrawing(false, "the grid texture is not loaded");
        return;
    }
    const F32 alpha = llclamp(gSavedSettings.getF32("GridOpacity"), 0.f, 1.f);
    if (alpha <= 0.f)
    {
        logGridDrawing(false, "GridOpacity is 0");
        return;
    }
    logGridDrawing(true, ground_on() ? (object_on() ? "on the ground and on the selection" : "on the ground") : "on the selection");

    LLGLDepthTest gls_depth(GL_TRUE, GL_FALSE);
    LLGLDisable gls_cull(GL_CULL_FACE);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    if (ground_on())
    {
        renderGroundGrid(tex, alpha);
    }
    if (object_on())
    {
        renderObjectGrid(tex, alpha);
    }

    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.color4f(1.f, 1.f, 1.f, 1.f);
}

// One log line each time the grids start drawing, stop, or stop for a different reason - never per
// frame - so a "the grid did not appear" report can be read straight from the log (Paul 10-14: the
// ground grid was ticked, Builders Mode was off, and nothing in the log said so).
void WolfBuilder::logGridDrawing(bool drawing, const char* why)
{
    if (drawing == mGridWasDrawing && mGridLogReason == why)
    {
        return;
    }
    mGridWasDrawing = drawing;
    mGridLogReason = why;
    LL_INFOS("WolfBuilder") << (drawing ? "grid drawing " : "grid not drawn: ") << why << LL_ENDL;
}

// The ground grid: centred under the selection (an in-world one) or under you, lying on the land
// (llworld.h:98 resolveLandHeightAgent) - the first build drew it at the centre's own height, which
// for you is your hips, so it floated in the air and the ground hid half of it. Lifted a few
// centimetres so the terrain does not z-fight it. Its lines count from the region corner, or from
// the builder's own origin (XY only: the grid stays on the ground).
void WolfBuilder::renderGroundGrid(U32 tex, F32 alpha)
{
    const F32 GROUND_LIFT = 0.05f;

    LLVector3 center = gAgent.getPositionAgent();
    LLObjectSelectionHandle selection = LLSelectMgr::getInstance()->getSelection();
    if (!selection->isEmpty() && selection->getSelectType() == SELECT_TYPE_WORLD)
    {
        center = gAgent.getPosAgentFromGlobal(LLSelectMgr::getInstance()->getSelectionCenterGlobal());
    }
    center.mV[VZ] = LLWorld::getInstance()->resolveLandHeightAgent(center) + GROUND_LIFT;

    // <WolfViewer> the region corner, which is agent (0,0,0) only until a huge-region origin rebase (llagent.h)
    LLVector3 origin = gAgent.getRegion() ? gAgent.getRegion()->getOriginAgent() : LLVector3::zero;
    // </WolfViewer>
    if (snapOriginEnabled())
    {
        origin = snapOriginAgent();
    }
    origin.mV[VZ] = center.mV[VZ];

    const F32 spacing = llmax(0.01f, gSavedSettings.getF32("GridResolution"));
    const F32 size = llmax(1.f, gSavedSettings.getF32("GridDrawSize"));
    drawGridPlane(PLANE_XY, center, LLQuaternion(), origin, LLVector3(spacing, spacing, spacing), size,
                  LLColor4::white, alpha, tex);
}

// The grid on the selection itself, in the frame the Build floater's ruler uses: LLSelectMgr::getGrid
// (llselectmgr.cpp:1468-1573) gives the origin, rotation and step for the current mode - World (the
// region corner or the builder's origin, no rotation, GridResolution), Local (the selection's own
// box and rotation, steps of half its size) or Reference (the reference object's). The selection's
// box is rebuilt every frame in that rotation from each selected prim's box (llselectmgr.cpp:7807-7828
// updateSelectionCenter does the same in the root prim's rotation; llbbox.cpp:89-100 getAxisAligned is
// the pattern for a box in a chosen rotation), so the grid follows a prim being dragged or turned.
// The planes are the box's faces, pushed out a centimetre so they do not z-fight the prim: the
// bottom face only, or all six with WolfBuilderObjectGridAllFaces. Each face takes the plane colour
// the snap guides give it (llmaniptranslate.cpp:1511-1527: YZ cyan, XZ magenta, XY yellow) and is
// sized to reach a little past the face. Nothing selected: nothing drawn.
void WolfBuilder::renderObjectGrid(U32 tex, F32 alpha)
{
    const F32 FACE_PUSH = 0.01f;

    LLSelectMgr* mgr = LLSelectMgr::getInstance();
    LLObjectSelectionHandle selection = mgr->getSelection();
    if (selection->isEmpty() || selection->getSelectType() != SELECT_TYPE_WORLD)
    {
        return;
    }

    LLVector3 grid_origin;
    LLQuaternion grid_rotation;
    LLVector3 grid_scale;
    mgr->getGrid(grid_origin, grid_rotation, grid_scale);

    const LLVector3 sel_center = gAgent.getPosAgentFromGlobal(mgr->getSelectionCenterGlobal());
    LLBBox box(sel_center, grid_rotation, LLVector3(), LLVector3());
    box.addPointAgent(sel_center);
    bool any = false;
    for (LLObjectSelection::iterator iter = selection->begin(); iter != selection->end(); ++iter)
    {
        LLViewerObject* object = (*iter)->getObject();
        if (!object || object->isDead() || object->isAvatar())
        {
            continue;
        }
        box.addBBoxAgent(object->getBoundingBoxAgent());
        any = true;
    }
    if (!any)
    {
        return;
    }

    static LLCachedControl<bool> all_faces(gSavedSettings, "WolfBuilderObjectGridAllFaces", false);
    const LLVector3 mn = box.getMinLocal();
    const LLVector3 mx = box.getMaxLocal();
    const LLVector3 mid = box.getCenterLocal();
    const LLVector3 ext = box.getExtentLocal();
    const LLColor4 XY_COLOR(1.f, 1.f, 0.f, 1.f);
    const LLColor4 XZ_COLOR(1.f, 0.f, 1.f, 1.f);
    const LLColor4 YZ_COLOR(0.f, 1.f, 1.f, 1.f);
    // The grid reaches half as far again as the face's larger half-size, plus two steps, then fades
    // (drawGridTiles' falloff reaches zero at `size`).
    const F32 step = llmax(0.01f, llmin(grid_scale.mV[VX], llmin(grid_scale.mV[VY], grid_scale.mV[VZ])));
    auto face_size = [step](F32 a, F32 b) { return llmax(a, b) * 0.5f * 1.5f + step * 2.f; };

    // bottom (and top)
    drawGridPlane(PLANE_XY, box.localToAgent(LLVector3(mid.mV[VX], mid.mV[VY], mn.mV[VZ] - FACE_PUSH)), grid_rotation,
                  grid_origin, grid_scale, face_size(ext.mV[VX], ext.mV[VY]), XY_COLOR, alpha, tex);
    if (!all_faces())
    {
        return;
    }
    drawGridPlane(PLANE_XY, box.localToAgent(LLVector3(mid.mV[VX], mid.mV[VY], mx.mV[VZ] + FACE_PUSH)), grid_rotation,
                  grid_origin, grid_scale, face_size(ext.mV[VX], ext.mV[VY]), XY_COLOR, alpha, tex);
    drawGridPlane(PLANE_XZ, box.localToAgent(LLVector3(mid.mV[VX], mn.mV[VY] - FACE_PUSH, mid.mV[VZ])), grid_rotation,
                  grid_origin, grid_scale, face_size(ext.mV[VX], ext.mV[VZ]), XZ_COLOR, alpha, tex);
    drawGridPlane(PLANE_XZ, box.localToAgent(LLVector3(mid.mV[VX], mx.mV[VY] + FACE_PUSH, mid.mV[VZ])), grid_rotation,
                  grid_origin, grid_scale, face_size(ext.mV[VX], ext.mV[VZ]), XZ_COLOR, alpha, tex);
    drawGridPlane(PLANE_YZ, box.localToAgent(LLVector3(mn.mV[VX] - FACE_PUSH, mid.mV[VY], mid.mV[VZ])), grid_rotation,
                  grid_origin, grid_scale, face_size(ext.mV[VY], ext.mV[VZ]), YZ_COLOR, alpha, tex);
    drawGridPlane(PLANE_YZ, box.localToAgent(LLVector3(mx.mV[VX] + FACE_PUSH, mid.mV[VY], mid.mV[VZ])), grid_rotation,
                  grid_origin, grid_scale, face_size(ext.mV[VY], ext.mV[VZ]), YZ_COLOR, alpha, tex);
}

// ---------------------------------------------------------------------------------------------------
// Floating object names
// ---------------------------------------------------------------------------------------------------

void WolfBuilder::clearNameTags()
{
    for (auto& entry : mTags)
    {
        if (entry.second.mText)
        {
            entry.second.mText->markDead();   // Source: llhudtext.cpp:739-746 - out of the text set, dead for the HUD list
        }
    }
    mTags.clear();
}

// Source: the name store is WolfObjectProps (wolfobjectprops.h:126-144 note / get / want): names only
// arrive by ObjectPropertiesFamily, one packet a prim, so the sweep declares interest nearest first and
// the tag appears when the answer does. The tag itself is the machinery an object's own hover text
// uses: llviewerobject.cpp:6331-6339 initHudText (addHUDObject LL_HUD_TEXT, sans-serif font, top
// alignment, source object = the prim); its position is set each frame (llhudobject.h:59
// setPositionAgent) because only the object's own mText is moved by LLViewerObject::updateText.
// Objects only, never avatars: pcode LL_PCODE_VOLUME, no attachments, root prims (no parent).
void WolfBuilder::idle()
{
    static LLCachedControl<bool> on(gSavedSettings, "WolfBuilderNames", false);
    static LLCachedControl<F32> radius(gSavedSettings, "WolfBuilderNamesRadius", 20.f);
    // [WolfViewer 2026-10-14] Paul: "builder mode is switched off yet you are labelling all the objects". The names are a
    // Builders Mode tool like the grids (renderGrid): they show only while Builders Mode is on, whatever their own box says.
    static LLCachedControl<bool> builders_mode(gSavedSettings, "WolfBuildersMode", false);

    if (!on() || !builders_mode() || gDisconnected || !gAgent.getRegion())
    {
        if (!mTags.empty())
        {
            clearNameTags();
        }
        if (mNamesWereOn)
        {
            LL_INFOS("WolfBuilder") << "object names off" << LL_ENDL;
        }
        mNamesWereOn = false;
        return;
    }

    // Switched (back) on: start from nothing and sweep this frame, not after the scan interval, so
    // ticking the box shows the names at once every time, not only the first. Paul 10-13: "showed once,
    // i turned them off and then on and they didn't show again".
    bool sweep_now = false;
    if (!mNamesWereOn)
    {
        mNamesWereOn = true;
        clearNameTags();
        sweep_now = true;
    }

    const LLVector3 me = gAgent.getPositionAgent();
    const F32 r = llmax(1.f, (F32)radius());
    const F32 r2 = r * r;

    // Every frame: move each tag with its prim; drop the dead and the out-of-range.
    for (auto it = mTags.begin(); it != mTags.end(); )
    {
        Tag& tag = it->second;
        LLViewerObject* obj = tag.mObject.get();
        const bool keep = obj && !obj->isDead() && tag.mText
            && dist_vec_squared(obj->getPositionAgent(), me) <= r2;
        if (!keep)
        {
            if (tag.mText)
            {
                tag.mText->markDead();
            }
            it = mTags.erase(it);
            continue;
        }
        // A rename reaches the store when the prim is selected (ObjectProperties) or re-asked.
        const WolfObjectProps::Props* props = WolfObjectProps::instance().get(it->first);
        if (props && !props->mName.empty() && props->mName != tag.mName)
        {
            tag.mName = props->mName;
            tag.mText->setString(tag.mName);
        }
        tag.mText->setPositionAgent(obj->getPositionAgent()
            + LLVector3(0.f, 0.f, obj->getScale().mV[VZ] * 0.5f + TAG_LIFT));
        ++it;
    }

    if (!sweep_now && mScanTimer.getElapsedTimeF32() < SCAN_SECS)
    {
        return;
    }
    mScanTimer.reset();

    // The sweep: every in-world root prim within the radius, nearest first.
    std::vector<std::pair<F32, LLViewerObject*> > candidates;
    const S32 count = gObjectList.getNumObjects();
    for (S32 i = 0; i < count; ++i)
    {
        LLViewerObject* obj = gObjectList.getObject(i);   // null for a dead one (llviewerobjectlist.h:309-318)
        if (!obj || obj->getPCode() != LL_PCODE_VOLUME || obj->isAttachment() || obj->isHUDAttachment() || obj->getParent())
        {
            continue;
        }
        const F32 d2 = dist_vec_squared(obj->getPositionAgent(), me);
        if (d2 <= r2)
        {
            candidates.push_back(std::make_pair(d2, obj));
        }
    }
    std::sort(candidates.begin(), candidates.end(),
        [](const std::pair<F32, LLViewerObject*>& a, const std::pair<F32, LLViewerObject*>& b) { return a.first < b.first; });
    if (candidates.size() > MAX_TAGS)
    {
        candidates.resize(MAX_TAGS);
    }

    std::set<LLUUID> wanted;
    for (const auto& cand : candidates)
    {
        LLViewerObject* obj = cand.second;
        wanted.insert(obj->mID);
        WolfObjectProps::instance().want(obj);
        if (mTags.find(obj->mID) != mTags.end())
        {
            continue;
        }
        const WolfObjectProps::Props* props = WolfObjectProps::instance().get(obj->mID);
        if (!props || props->mName.empty())
        {
            continue;   // not answered yet, or a prim with no name
        }
        LLHUDText* text = (LLHUDText*)LLHUDObject::addHUDObject(LLHUDObject::LL_HUD_TEXT);
        text->setFont(LLFontGL::getFontSansSerifSmall());
        text->setVertAlignment(LLHUDText::ALIGN_VERT_TOP);
        text->setMaxLines(1);
        text->setSourceObject(obj);
        text->setDoFade(false);   // the radius is the range; no second fade (llhudtext.cpp:95-100 defaults)
        text->setColor(TAG_COLOR);
        text->setString(props->mName);
        text->setPositionAgent(obj->getPositionAgent()
            + LLVector3(0.f, 0.f, obj->getScale().mV[VZ] * 0.5f + TAG_LIFT));

        Tag tag;
        tag.mObject = obj;
        tag.mText = text;
        tag.mName = props->mName;
        mTags[obj->mID] = tag;
    }

    // Tags for prims that fell outside the nearest MAX_TAGS (the radius is checked above) go.
    for (auto it = mTags.begin(); it != mTags.end(); )
    {
        if (wanted.find(it->first) == wanted.end())
        {
            if (it->second.mText)
            {
                it->second.mText->markDead();
            }
            it = mTags.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // One line when the names are switched on, so a "no names showed" report can be told apart in the
    // log: no prims in range, prims in range not yet named by the region, or tags made.
    if (sweep_now)
    {
        LL_INFOS("WolfBuilder") << "object names on: " << candidates.size() << " root prims within " << r
                                << " m, " << mTags.size() << " named and showing" << LL_ENDL;
    }
}
