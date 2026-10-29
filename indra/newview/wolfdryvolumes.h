/**
 * @file wolfdryvolumes.h
 * @brief WolfViewer: air-filled volumes under the sea (sea tunnels) - no water fog, no water surface inside them.
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

#ifndef WOLF_DRYVOLUMES_H
#define WOLF_DRYVOLUMES_H

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "llsingleton.h"
#include "lluuid.h"
#include "v3math.h"

class LLShaderUniforms;
class LLViewerObject;

/**
 * [WOLF SEA TUNNEL 2026-10-28] Ticket 319, Malachi Coltrane: "An underwater tunnel device that clears the water
 * murkiness. Shape would only need to be cylindrical. For use with vehicle and train traffic. For making the WTG version
 * of the Chunnel." Paul: "yes i do want it". The invisiprim he uses (texture e97cf410-...) only feeds the stock water
 * EXCLUSION mask (lldrawpoolwaterexclusion.cpp): a 2D screen mask over the water SURFACE - the underwater fog, which is
 * what makes the inside of a tube murky, never looks at it.
 * Persona: a builder making a glass rail or road tunnel across the sea bed. Story: "I put wolfdry in my glass tube's
 * description and the inside is air: clear inside, no sea surface through it, the trains run dry."
 *
 * WHAT COUNTS AS AIR
 *  - any prim whose DESCRIPTION contains "wolfdry" (any case; a script can set it with llSetObjectDesc): a cylinder is a
 *    round tube along its own Z (its hollow, cuts and twist are ignored: the air is the whole outer shape), a box is its
 *    box, a sphere (or a stretched one) its ellipsoid; a mesh or sculpt is a round tube along its LONGEST side; other
 *    prim shapes are not used. Linked parts count one by one, so a tunnel can be one linkset of tube pieces.
 *  - the Tunnel tool's walkable boxes (WolfTunnels::boxes) that reach below the water: a tunnel bored under the sea bed
 *    is air too.
 *
 * WHAT THE VIEWER DOES WITH IT (only on a Wolf region, WolfGrid::showsWolfRegion)
 *  - the water fog (waterFogF.glsl getWaterFogViewNoClip, used by the haze pass, the alpha objects and the water itself)
 *    leaves out the stretch of each line of sight inside these volumes;
 *  - the water surface (waterF.glsl / underWaterF.glsl) is not drawn inside them;
 *  - with the camera inside one there are no underwater sounds (llvieweraudio.cpp) and no underwater sun shafts
 *    (LLPipeline::wolfGodRays); an avatar whose head is inside one does not swim (wolfswim.cpp).
 *
 * FINDING THEM costs no new traffic: descriptions come from the shared, throttled WolfObjectProps (wolfobjectprops.h);
 * this class only declares interest in prims of a usable shape that reach below their region's water, and keeps the
 * ones it uses fresh (a script that clears the word turns it off for everyone within REFRESH_SECS). Nothing is fetched
 * from a server and nothing polls one; the tunnel boxes are the ones WolfTunnels already reads on arrival.
 * Shaders get at most MAX_SHADER_VOLUMES, the ones the camera is in first, then the nearest.
 *
 * [WOLF DRY HULLS 2026-10-28] Paul: "cant the viewer just detect that its a boat like we do for waves and not draw the
 * water?" BOATS' HULLS are air too, automatically: every boat WolfBoatRock finds (the same sweep and gates as boat
 * rocking: wolfboatrock.h boats()) is MEASURED once from its own visible geometry (measureHull: the hull's outer skin
 * where it crosses the waterline, 1 m above it and at two depths below it, KIND_HULL) and its inside below the deck is
 * air: no water surface, waves or fog in the hold, no swimming there. The sea outside is never cut: the table is fitted so
 * it never reaches past the skin at a measured height. The volume rides the hull's drawn place and turn every frame, so a
 * sailing or rocking boat keeps it. Setting WolfViewerDryHulls (on) turns it off. A boat measured this way does not also
 * use its own wolfdry prims (HMS Wolfy's five tubes): the hull covers them and they would only take shader slots.
 */
class WolfDryVolumes : public LLSingleton<WolfDryVolumes>
{
    LLSINGLETON(WolfDryVolumes);
    ~WolfDryVolumes();

public:
    /** The keyword, matched case-insensitively anywhere in a description. */
    static const std::string KEYWORD;
    /** waterFogF.glsl wolfDry[32] = 8 volumes x 4 vec4. */
    static constexpr S32 MAX_SHADER_VOLUMES = 8;
    /** How often the object list is swept for tagged prims (the same pace as FSWolfWater, wolfobjectprops.h). */
    static constexpr F64 SWEEP_INTERVAL_SECS = 1.5;
    /** Prims are asked about inside the draw distance and within NEAR_M + PER_METRE_M x their longest side
     *  (FSWolfWater::NEAR_M / PER_METRE_M: the same reach, so the same prims are asked). */
    static constexpr F32 NEAR_M = 64.f;
    static constexpr F32 PER_METRE_M = 32.f;

    enum EKind { KIND_NONE = 0, KIND_CYLINDER = 1, KIND_BOX = 2, KIND_SPHERE = 3, KIND_HULL = 4 };   // waterFogF.glsl wolfDry[i*4+3].x

    // [WOLF DRY HULLS] waterFogF.glsl WOLF_HULL_STATIONS / WOLF_HULL_LEVELS / WOLF_HULL_VEC4 and wolfHull[100] - kept in step.
    static constexpr S32 HULL_STATIONS = 24;
    static constexpr S32 HULL_LEVELS = 4;
    static constexpr S32 HULL_VEC4 = 25;          // 4 x 24 half-widths, 4 to a vec4, then the levels
    static constexpr S32 MAX_SHADER_HULLS = 4;    // of the MAX_SHADER_VOLUMES slots
    /** At most this many triangles of boats are measured per sweep (the nearest boats first; at least one boat). */
    static constexpr S32 HULL_TRIANGLE_BUDGET = 400000;

    /** A boat's hull in its unit space (waterFogF.glsl wolfHullHalf): x along it (-1..1), y across (1 = the widest
     *  half-beam), z keel (-1) to the top (1). mHalf[level][station] is the half-width there in unit y; mLevel the
     *  levels' unit z, top first. mAxisLocal / mCentreLocal / mHalfLocal: that unit space in the ROOT prim's own frame. */
    struct HullProfile
    {
        F32 mHalf[HULL_LEVELS][HULL_STATIONS];
        F32 mLevel[HULL_LEVELS];
        LLVector3 mAxisLocal[3];
        LLVector3 mCentreLocal;
        F32 mHalfLocal[3];
        /** The half-width (unit y) at unit (x, z); -1 outside. The same as waterFogF.glsl wolfHullHalf. */
        F32 halfWidth(F32 x, F32 z) const;
    };

    /** One volume, agent frame: centre, three unit axes and the half size along each (a cylinder is round in axes 0/1
     *  and runs along axis 2). */
    struct Volume
    {
        EKind mKind = KIND_NONE;
        LLVector3 mCentre;
        LLVector3 mAxis[3];
        F32 mHalf[3] = { 0.f, 0.f, 0.f };
        std::shared_ptr<const HullProfile> mHull;   // KIND_HULL only
        bool contains(const LLVector3& agent_pos) const;
        F32 boundingRadius() const;
    };

    /** Every frame from LLAppViewer::idle(): sweeps the object list every SWEEP_INTERVAL_SECS. */
    void idle();
    /** Every frame from LLSettingsVOWater::applySpecial: the volumes of this frame (positions read live, so a moving
     *  one is followed), put into wolfDry / wolfDryCount in eye space. modelview: the agent-to-eye matrix the water plane
     *  is made with there (16 floats, column-major). */
    void applyUniforms(LLShaderUniforms* uniforms, const F32* modelview);
    /** Is this agent-frame point inside an air-filled volume (this frame's list)? */
    bool contains(const LLVector3& agent_pos) const;
    /** Is the camera inside one? */
    bool cameraInside() const;
    /** [WOLF DRY T352] Does any volume of this frame reach both below and above the agent-frame height z (by its bounding
     *  box: a little wider than the shape), i.e. may it cut a hole in a water plane at that height? */
    bool crossesHeight(F32 z) const;
    /** How many tagged prims are in use near here now (the Sea Tunnels window). */
    S32 taggedCount() const { return (S32)mTagged.size(); }

    /** Does this (lower-cased) description ask for air? */
    static bool matches(const std::string& description_lower);
    /** The volume a prim stands for; false when its shape is not one that is used (wolfdryvolumes.h WHAT COUNTS). */
    static bool volumeOf(LLViewerObject* objectp, Volume& out);

private:
    void sweep();
    /** The tagged prims and the tunnel boxes under the water, as volumes now (mFrame). */
    void collect();
    /** [WOLF DRY HULLS] Keep mHulls in step with WolfBoatRock::boats(): measure new or changed ones within the budget. */
    void sweepHulls();
    /** Measure a boat's hull from its visible geometry; false when it is not one that can be (see the .cpp). triangles:
     *  how many were read. */
    static bool measureHull(LLViewerObject* root, HullProfile& out, S32& triangles);
    /** What the measurement depends on: the linkset's prims, their volumes (a mesh LOD switch gives a new one), sizes
     *  and places. */
    static U64 hullSignature(LLViewerObject* root);

    std::set<LLUUID> mTagged;        // prims whose description has the keyword, in reach
    std::vector<Volume> mFrame;      // this frame's volumes, every one (not only the shader's)
    F64 mNextSweep = 0.0;
    U64 mRegionHandle = 0;
    U32 mFrameCollected = 0;         // LLFrameTimer frame count mFrame was made in

    struct HullCache
    {
        U64 mSignature = 0;
        F32 mWaterOffset = 0.f;          // water height - root z when measured (a boat that sinks or rises is measured again)
        std::shared_ptr<const HullProfile> mProfile;   // null: measured, not a hull that can be used
    };
    std::map<LLUUID, HullCache> mHulls;  // the boats WolfBoatRock found at the last sweep
};

#endif // WOLF_DRYVOLUMES_H
