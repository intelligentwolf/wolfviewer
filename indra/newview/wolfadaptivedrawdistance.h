/**
 * @file wolfadaptivedrawdistance.h
 * @brief [ADAPTIVE DRAW DISTANCE 2026-11-18] WolfViewer: when frames get slow the draw distance in use is lowered quickly,
 *        and raised again slowly once frames are smooth; the resident's own draw distance setting is never changed.
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

#ifndef WOLF_ADAPTIVE_DRAW_DISTANCE_H
#define WOLF_ADAPTIVE_DRAW_DISTANCE_H

#include "stdtypes.h"

/**
 * Paul, 18 Nov 2026, after his viewer froze at Paddington Station (RenderFarClip 4096 m, 22,000 of 81,500 prims in range,
 * the laptop throttling, 11 GB resident): "i'm not starting the viewer again till adaptive draw distance is done".
 *
 * Two values, kept apart:
 *  - the resident's own draw distance: the RenderFarClip setting (the Preferences slider), copied by stock into
 *    gAgentCamera.mDrawDistance (llviewercontrol.cpp handleRenderFarClipChanged);
 *  - the draw distance in use: gAgentCamera.mDrawDistance, which everything that costs frames reads - the camera's far
 *    plane (llviewerdisplay.cpp display_update_camera final_far), what the region is asked to send
 *    (llviewermessage.cpp send_agent_update AgentUpdate Far), the object cache's radius (llvocache.cpp, llviewerregion.cpp)
 *    and the land's far clip (LLWorld::setLandFarClip).
 * This module only ever lowers the second, to min(the resident's own, its cap), and puts it back. The one stock line that
 * copied the value in use into the setting at exit (LLAppViewer::cleanupSavedSettings -> RenderFarClip) now saves
 * userDrawDistance() instead, so a lowered value can never become the resident's setting. [TRACK PICK 2026-11-18] A land
 * pick (llviewerwindow.cpp mousePointOnLandGlobal) costs no frames and reaches userDrawDistance() too: with the value in
 * use the Lay Railway Track pen could not find the ground more than 128 m from the camera.
 *
 * [ADAPTIVE DD CHECKS ITS CUTS 2026-11-18] Paul at Tafelberg the same day: "when i came out of the tunnel the auto draw
 * distance had dramatically reduced - this region has nothing but the railway on it". His frames stayed at ~20 fps (dips
 * to ~9) from 4096 m down to 136 m: the GPU was throttling, the distance was not what held the frames. So every cut is
 * now a trial: the frame time after it is compared with the frame time before; a cut that did not clearly help is checked
 * by a few seconds at a much shorter distance, and if that is no faster either the cut is undone (back to the distance
 * before it, or the resident's own when no cut has helped) and no cut is tried for a while unless frames get clearly worse.
 * A trial during which the GPU itself changed speed (the fixed-size sky dome pass, WolfEnvironmentRender::skyPassGpuMs) is
 * void: the distance before it goes back. Raising is proportional to how far frames are above the target, and quicker when
 * they are well above it.
 *
 * It stands aside (the cap goes, the resident's value is back at once) while it is switched off
 * (WolfAdaptiveDrawDistance, Wolf World > Display & Effects), while LL's Auto-tune is on or locked (it moves RenderFarClip
 * itself: llperfstats.cpp updateAvatarParams / Tunables::applyUpdates), and while a VR headset has the viewer (VR has its
 * own WolfVRDrawDistance). It does not measure, and keeps the cap it has, during log-in, teleports, the progress screen,
 * Firestorm's draw distance stepping after a teleport, and while the window is minimised, hidden or not in front.
 */
namespace WolfAdaptiveDrawDistance
{
    /** Every frame from LLAppViewer::idle(): measure the frame, lower or raise the cap, write the draw distance in use. */
    void idle();

    /** The resident's own draw distance: what gAgentCamera.mDrawDistance would be without this module. */
    F32 userDrawDistance();

    /** True while the draw distance in use is held below the resident's own (the note shows). */
    bool holding();

    /** From LLViewerWindow::draw, in the UI pass: the small note while the draw distance is held down. */
    void drawNote();
}

#endif // WOLF_ADAPTIVE_DRAW_DISTANCE_H
