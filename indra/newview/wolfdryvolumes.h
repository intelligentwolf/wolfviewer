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

    enum EKind { KIND_NONE = 0, KIND_CYLINDER = 1, KIND_BOX = 2, KIND_SPHERE = 3 };   // waterFogF.glsl wolfDry[i*4+3].x

    /** One volume, agent frame: centre, three unit axes and the half size along each (a cylinder is round in axes 0/1
     *  and runs along axis 2). */
    struct Volume
    {
        EKind mKind = KIND_NONE;
        LLVector3 mCentre;
        LLVector3 mAxis[3];
        F32 mHalf[3] = { 0.f, 0.f, 0.f };
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

    std::set<LLUUID> mTagged;        // prims whose description has the keyword, in reach
    std::vector<Volume> mFrame;      // this frame's volumes, every one (not only the shader's)
    F64 mNextSweep = 0.0;
    U64 mRegionHandle = 0;
    U32 mFrameCollected = 0;         // LLFrameTimer frame count mFrame was made in
};

#endif // WOLF_DRYVOLUMES_H
