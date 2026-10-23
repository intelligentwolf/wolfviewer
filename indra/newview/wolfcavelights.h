/**
 * @file wolfcavelights.h
 * @brief WolfViewer: "Light up" for caves and tunnels - lamps placed through a cave, and taken away again.
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

#ifndef WOLF_CAVE_LIGHTS_H
#define WOLF_CAVE_LIGHTS_H

#include <functional>
#include <string>
#include <vector>

#include "llsd.h"
#include "lluuid.h"
#include "v2math.h"
#include "v3math.h"

// [WOLF CAVE LIGHTS 2026-10-23] Paul: "light up cave" - "but they have to be able to remove them as well", then "let them choose
// the light style rather than forcing it" and "make them a linkset so people can edit the lamps and move them if they wish".
// Persona: a resident who has just built a cave or tunnel and wants it lit without scripting or placing lamps one by one.
// Flow: pick a style (and warm or cool light) -> Light up -> a confirmation with the lamp count and the prims -> the lamps are
// placed one by one along the route (every LAMP_SPACING_M on the walls, one in each room, one at each crossroads) -> Remove
// lights takes exactly those lamps away again. Used by the Cave Builder (wolfcave.cpp) and the Tunnel tool
// (wolffloatertunnels.cpp); the lamp ids live in their records (php/caves.php, php/tunnels.php "lights").
//
// Every lamp is ordinary content the resident owns: prims made with ObjectAdd (the build tool's own message, so the region's
// rez rules apply exactly as for a prim made by hand), each lamp ONE linkset (the fitting is the root; a torch's flame and light
// are a linked child at the bowl), phantom, so it can be edited, moved or deleted like anything else. Torches use the CC0 torch
// meshes stored on the grid by firestorm/region-terrain-work/cave-lights/make_torch_assets.py and a tiny one-shot particle script
// (no timer, no listen, no sensor).
namespace WolfCaveLights
{
    enum EStyle { STYLE_TORCHES = 0, STYLE_LAMPS, STYLE_LANTERNS, STYLE_CRYSTALS, STYLE_HIDDEN, STYLE_COUNT };

    /** The record's word for a style (php/caves.php CAVE_LIGHT_STYLES / php/tunnels.php TUN_LIGHT_STYLES). */
    std::string styleKey(EStyle s);
    /** A record's word back to a style; `fallback` for anything else. */
    EStyle styleFrom(const std::string& key, EStyle fallback);
    /** The plural noun people read ("torches", "wall lamps", ...). */
    std::string styleNoun(EStyle s);
    /** Prims one lamp of this style takes (a torch is the fitting plus its flame). */
    S32 partsPerLamp(EStyle s);
    /** The style a cave's walls suggest: torches for natural (rough) walls, wall lamps for built (smooth) ones. */
    EStyle styleForWalls(bool natural);
    /** The remembered choice (settings WolfCaveLightStyle / WolfCaveLightTone), or `walls` when none was made yet. */
    EStyle savedStyle(EStyle walls);
    bool savedWarm();
    void saveChoice(EStyle s, bool warm);

    /** What the air is like at (x, y), region metres: `s` metres inside the air (negative in the rock), the floor's height and
     *  the clear height above it. False when (x, y) is outside every part of the cave. */
    typedef std::function<bool(F32 x, F32 y, F32& s, F32& floor, F32& clear)> field_f;
    /** One route through the cave (a tunnel's line), region metres, and its half-width. */
    struct Route { std::vector<LLVector2> mPts; F32 mHalfW = 2.f; };
    /** A room (cavern) on the route. */
    struct Room { LLVector2 mC; F32 mR = 5.f; };
    /** Where one lamp goes: the fitting's centre (region metres), its turn about up (radians; the fitting's back, local +Y, is
     *  to the wall), whether it stands on the floor (rooms) and how far its light reaches. */
    struct Lamp { LLVector3 mPos; F32 mYaw = 0.f; bool mStanding = false; F32 mRadius = 10.f; };

    /** The lamps for a cave: on the walls every LAMP_SPACING_M along each route (both sides in turn), one in each room, one at each
     *  crossroads. `keep_middle_clear` (a railway): nothing stands on the floor near the route's line. */
    std::vector<Lamp> plan(const std::vector<Route>& routes, const std::vector<Room>& rooms, const field_f& field, EStyle style,
                           bool keep_middle_clear);

    /** The parts of stored lamps this viewer can see in the agent's region (`seen`, root objects to take away) and the ids it
     *  cannot (`unseen`: deleted by hand already, or out of view). */
    void find(const LLSD& parts, std::vector<LLUUID>& seen_roots, LLSD& unseen);
    /** Delete (into Trash) or return every root in `roots` that is still there. Returns how many were sent. */
    S32 takeAway(const std::vector<LLUUID>& roots);

    /** Progress reports from place(): a line to show and 0..1. */
    typedef std::function<void(const std::string& line, F32 progress)> progress_f;
    /** Place the lamps (call from a coroutine: it waits for each prim). `parts` gets every part made, as id strings, also when it
     *  stops part way (so Remove lights can take those away). `stop` is asked between lamps. False with `error` set on failure. */
    bool place(const std::vector<Lamp>& lamps, EStyle style, bool warm, const std::string& cave_name, const LLUUID& region,
               LLSD& parts, std::string& error, const progress_f& progress, const std::function<bool()>& stop);

    /** How many lamps a record's "lights" lists, by its parts and style (0 when none). */
    S32 lampCount(const LLSD& lights);
    /** "12 torches" for a list row; empty when the record has none. */
    std::string summary(const LLSD& lights);

    /** Light up's question, asked before anything is placed: the number of lamps and prims, and what happens to the lamps placed
     *  before (the ones in view are taken away; for ones not in view the person chooses to keep them listed or forget them).
     *  `go(keep_unseen)` runs on yes. */
    void confirmLightUp(const std::string& cave_name, EStyle style, S32 lamps, const LLSD& old_lights,
                        const std::function<void(bool keep_unseen)>& go);
    /** Remove lights' question (the same choice for parts not in view). Says so and asks nothing when there are none. */
    void confirmRemove(const std::string& cave_name, const LLSD& old_lights, const std::function<void(bool keep_unseen)>& go);

    /** Store a record's new "lights" (an undefined LLSD = none left); false with `error` when the record service refused. */
    typedef std::function<bool(const LLSD& lights, std::string& error)> save_f;
    /** The whole job, from a coroutine: the old lamps in view are taken away; `lamps` (null for Remove lights) are placed in
     *  `style`; the record is saved before placing and after every few lamps, so a job that stops part way still lists every
     *  lamp it made. `done` gets the line to show at the end. False with `error` on failure. */
    bool run(const LLSD& old_lights, bool keep_unseen, const std::vector<Lamp>* lamps, EStyle style, bool warm,
             const std::string& cave_name, const LLUUID& region, const save_f& save, const progress_f& progress,
             const std::function<bool()>& stop, std::string& done, std::string& error);
    /** Before a cave or tunnel is deleted or built again: its lamps in view are taken away. Returns the parts not in view (left
     *  where they are: the caller says so). */
    LLSD takeAwayFor(const LLSD& lights);
}

#endif // WOLF_CAVE_LIGHTS_H
