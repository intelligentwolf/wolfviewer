/**
 * @file wolfswim.h
 * @brief WolfViewer: swimming - an avatar all the way under the water swims instead of walking on the bottom.
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

#ifndef WOLF_SWIM_H
#define WOLF_SWIM_H

// [WOLF SWIM 2026-10-20] Paul: "implement swimming when an avatar is all the way in the water and is not sitting.
// remember that quads are a bit different".
//
// Who and what: a resident walks or flies into the sea on a Wolf region; once the top of the avatar is under the
// surface it swims - moves freely in 3D, slower than flying, can never swim up out of the water, does not land on the
// bottom - and everyone around sees a swim. Out of the water (or sat down, or the Wolf World > Swimming switch off) it
// is back to walking.
//
// How (each part read from the source, see wolfswim.cpp):
//   - Movement: flying (LLAgent::setFlying) with AGENT_CONTROL_STOP held, which the region turns into its slow speed:
//     WolfSim ScenePresence.cs HandleAgentUpdate uses AgentControlStopSlowVel (0.2 x 4.096 m/s) while STOP is held, and
//     AddNewMovement multiplies a flying move by 4 - 3.28 m/s, against 9.8 m/s for plain flying (0.6 x 4.096 x 4).
//     Upward control is taken away at the surface; downward control on the bottom (so neither the region nor the
//     viewer lands the avatar), and the viewer's own auto-land waits while swimming (LLAgent::propagate).
//   - Animations, visible to everyone (requested from the region by UUID):
//       biped: wolf_swim_tread (still) / wolf_swim_stroke (moving), Wolf grid assets with fixed ids, priority 5;
//       quadruped: never a biped swim (it breaks the rig) - its own walk as a paddle: the Firestorm AO's Walking state
//       (AOEngine::setWolfSwimPaddle maps the fly / hover motions to it), else the walk its other AO (a scripted one,
//       e.g. WolfAvatars' llSetAnimationOverride HUD) was seen playing on land;
//       an AO set with its own swim states (Floating / Swimming ...) is preferred for both: the AO plays them already
//       (AOEngine::checkBelowWater) and nothing is added.
//   - Wolf regions only (WolfGrid::showsWolfRegion): the biped animations are on the Wolf grid's asset service.
namespace WolfSwim
{
    /** The whole per-frame step: decide, set the agent's fly state and control flags, start / stop the animations.
     *  From LLAppViewer::idle right before send_agent_update, so the flags it holds and the keys it takes away go out
     *  in this frame's AgentUpdate (the key handlers have already set theirs this frame). */
    void beforeAgentUpdate();

    /** Swimming now (the avatar is in the swim, flying). LLAgent::propagate leaves auto-land alone while true. */
    bool isSwimming();
}

#endif // WOLF_SWIM_H
