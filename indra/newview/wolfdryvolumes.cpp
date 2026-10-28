/**
 * @file wolfdryvolumes.cpp
 * @brief WolfViewer: air-filled volumes under the sea (sea tunnels). See wolfdryvolumes.h.
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

#include "wolfdryvolumes.h"

#include <algorithm>

#include "llagent.h"
#include "llframetimer.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llsurface.h"
#include "llshadermgr.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvolume.h"
#include "llworld.h"
#include "llprimitive.h"
#include "lltextureentry.h"
#include "wolfboatrock.h"
#include "wolffloatertunnels.h"
#include "wolfgrid.h"
#include "wolfobjectprops.h"

const std::string WolfDryVolumes::KEYWORD("wolfdry");

namespace
{
    // The smallest half size used (a flattened prim is still a volume, never a division by zero in the unit map).
    constexpr F32 MIN_HALF = 0.01f;

    // [WOLF DRY HULLS 2026-10-28] measureHull. The levels the hull's skin is cut at, metres in the boat's frame: this far
    // above the waterline (a crest alongside, the hull flaring out), the waterline, then a third and two thirds of the
    // way down to the keel. The volume's top: this far above the waterline (boat rocking: wolfboatrock.cpp's waterline
    // band reaches 2.5 m up). Checked against HMS Wolfy's own mesh (region-terrain-work/underwater-tunnel-2026-10-28/
    // README.md "Dry hulls"): 98.7 % of the inside of her planking at the waterline, 0.03 m2 past her skin.
    constexpr F32 HULL_LEVEL_ABOVE_M = 1.f;
    constexpr F32 HULL_TOP_ABOVE_M = 3.f;
    // Fine samples per station gap for the fit (waterFogF.glsl draws straight lines between the stations).
    constexpr S32 HULL_FINE_PER_GAP = 8;
    constexpr S32 HULL_FIT_PASSES = 16;
    // A boat that rises or sinks this far against the water (wolfSink, a heavier load) is measured again.
    constexpr F32 HULL_REMEASURE_WATER_M = 0.3f;
    // A face this transparent is not part of the hull you see: physics colliders, invisible prims, wolfdry tubes.
    constexpr F32 HULL_MIN_ALPHA = 0.05f;
    // Smaller than this is not a hull (a buoy, a board).
    constexpr F32 HULL_MIN_HALF_M = 0.25f;

    // A segment of the hull's skin where it crosses one level, in the boat's (a, b) plane.
    struct Seg
    {
        F32 a0, b0, a1, b1;
    };

    // The names that are boats with SEA BETWEEN their hulls (the measurement takes the widest point across, so the
    // sea between a catamaran's hulls would be cut away): those keep the water; KEEP IN STEP with wolfstorm wolfdry.js.
    bool multiHullName(const std::string& lower)
    {
        return lower.find("catamaran") != std::string::npos || lower.find("trimaran") != std::string::npos
            || lower.find("outrigger") != std::string::npos;
    }
}

F32 WolfDryVolumes::HullProfile::halfWidth(F32 x, F32 z) const
{
    // Source: waterFogF.glsl wolfHullHalf - the same lookup on the CPU.
    if (fabsf(x) > 1.f || fabsf(z) > 1.f)
    {
        return -1.f;
    }
    const F32 s = (x + 1.f) * 0.5f * (F32)(HULL_STATIONS - 1);
    const S32 i0 = (S32)llmin(floorf(s), (F32)(HULL_STATIONS - 2));
    const F32 f = s - (F32)i0;
    auto at = [&](S32 level) { return lerp(mHalf[level][i0], mHalf[level][i0 + 1], f); };
    if (z >= mLevel[0])
    {
        return at(0);
    }
    S32 k = 3;
    for (S32 l = 1; l < HULL_LEVELS; ++l)
    {
        if (z >= mLevel[l])
        {
            k = l - 1;
            break;
        }
    }
    const F32 za = mLevel[k];
    const F32 zb = (k + 1 < HULL_LEVELS) ? mLevel[k + 1] : -1.f;
    const F32 ha = at(k);
    const F32 hb = (k + 1 < HULL_LEVELS) ? at(k + 1) : 0.f;
    return lerp(hb, ha, llclamp((z - zb) / llmax(za - zb, 1e-6f), 0.f, 1.f));
}

WolfDryVolumes::WolfDryVolumes() = default;
WolfDryVolumes::~WolfDryVolumes() = default;

bool WolfDryVolumes::matches(const std::string& description_lower)
{
    // Source: fswolfwater.cpp FSWolfWater::matches - the keyword anywhere in the lower-cased description
    // (WolfObjectProps::Props::mDescriptionLower).
    return description_lower.find(KEYWORD) != std::string::npos;
}

bool WolfDryVolumes::Volume::contains(const LLVector3& p) const
{
    // Source: waterFogF.glsl wolfInDry - the same test on the CPU.
    const LLVector3 d = p - mCentre;
    const F32 u0 = (d * mAxis[0]) / mHalf[0], u1 = (d * mAxis[1]) / mHalf[1], u2 = (d * mAxis[2]) / mHalf[2];
    switch (mKind)
    {
    case KIND_SPHERE:   return u0 * u0 + u1 * u1 + u2 * u2 <= 1.f;
    case KIND_BOX:      return fabsf(u0) <= 1.f && fabsf(u1) <= 1.f && fabsf(u2) <= 1.f;
    case KIND_CYLINDER: return fabsf(u2) <= 1.f && u0 * u0 + u1 * u1 <= 1.f;
    case KIND_HULL:     return mHull && fabsf(u1) <= mHull->halfWidth(u0, u2);   // [WOLF DRY HULLS/] waterFogF.glsl wolfInHull
    default:            return false;
    }
}

F32 WolfDryVolumes::Volume::boundingRadius() const
{
    return sqrtf(mHalf[0] * mHalf[0] + mHalf[1] * mHalf[1] + mHalf[2] * mHalf[2]);
}

bool WolfDryVolumes::volumeOf(LLViewerObject* objectp, Volume& out)
{
    if (!objectp || objectp->isDead() || objectp->getPCode() != LL_PCODE_VOLUME || !objectp->getVolume())
    {
        return false;
    }
    // Agent frame, as the selection outline uses: llviewerobject.cpp getRenderPosition / getRenderRotation (a linked part's
    // own place and turn in the world, not its offset from the root).
    const LLQuaternion rot = objectp->getRenderRotation();
    const LLVector3 scale = objectp->getScale();
    LLVector3 axis[3] = { LLVector3::x_axis * rot, LLVector3::y_axis * rot, LLVector3::z_axis * rot };
    F32 half[3] = { llmax(scale.mV[VX] * 0.5f, MIN_HALF), llmax(scale.mV[VY] * 0.5f, MIN_HALF), llmax(scale.mV[VZ] * 0.5f, MIN_HALF) };

    EKind kind = KIND_NONE;
    if (objectp->isMesh() || objectp->isSculpted())
    {
        // A mesh or sculpt has no prim shape to read: a round tube along its longest side (a mesh tunnel tube is modelled
        // along any axis). Move the longest axis to slot 2 (the cylinder's length).
        kind = KIND_CYLINDER;
        S32 longest = 2;
        if (half[0] > half[longest]) longest = 0;
        if (half[1] > half[longest]) longest = 1;
        if (longest != 2)
        {
            std::swap(axis[longest], axis[2]);
            std::swap(half[longest], half[2]);
        }
    }
    else
    {
        // Source: llvolume.h:139-167 LL_PCODE_PROFILE_* / LL_PCODE_PATH_*; the build floater's shapes (llpanelobject.cpp
        // MI_CYLINDER = PROFILE_CIRCLE + PATH_LINE, MI_BOX = PROFILE_SQUARE + PATH_LINE, MI_SPHERE = PROFILE_CIRCLE_HALF +
        // PATH_CIRCLE). The hole bits are masked off: a hollow cylinder is a tube whose air is the whole outer shape.
        const LLVolumeParams& vp = objectp->getVolume()->getParams();
        const U8 profile = vp.getProfileParams().getCurveType() & LL_PCODE_PROFILE_MASK;
        const U8 path = vp.getPathParams().getCurveType();
        const bool straight = path == LL_PCODE_PATH_LINE || path == LL_PCODE_PATH_FLEXIBLE;
        if (profile == LL_PCODE_PROFILE_CIRCLE && straight)           kind = KIND_CYLINDER;
        else if (profile == LL_PCODE_PROFILE_SQUARE && straight)      kind = KIND_BOX;
        else if (profile == LL_PCODE_PROFILE_CIRCLE_HALF && path == LL_PCODE_PATH_CIRCLE) kind = KIND_SPHERE;
    }
    if (kind == KIND_NONE)
    {
        return false;
    }
    out.mKind = kind;
    out.mCentre = objectp->getRenderPosition();
    for (S32 k = 0; k < 3; ++k)
    {
        out.mAxis[k] = axis[k];
        out.mHalf[k] = half[k];
    }
    return true;
}

void WolfDryVolumes::idle()
{
    LLViewerRegion* agent_region = gAgent.getRegion();
    if (!agent_region || !WolfGrid::showsWolfRegion())
    {
        mTagged.clear();
        mHulls.clear();
        return;
    }
    // On a region change sweep at once. The set is kept, not cleared (FSWolfWater::idle clears its surfaces): it holds
    // UUIDs, which stay valid across a border, so a train crossing into the next region inside a tunnel keeps its air;
    // after a teleport the old ones are simply not found (collect: findObject) and the sweep replaces them.
    if (agent_region->getHandle() != mRegionHandle)
    {
        mRegionHandle = agent_region->getHandle();
        mNextSweep = 0.0;
    }
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (now < mNextSweep)
    {
        return;
    }
    mNextSweep = now + SWEEP_INTERVAL_SECS;
    sweep();
    sweepHulls();
}

void WolfDryVolumes::sweep()
{
    // Source: fswolfwater.cpp FSWolfWater::sweep - the same walk, reach and harvester use; only prims of a usable shape
    // that reach below their region's water are asked about (anything higher holds no sea to clear).
    const LLVector3 camera_pos = LLViewerCamera::getInstance()->getOrigin();
    static LLCachedControl<F32> draw_distance(gSavedSettings, "RenderFarClip", 128.f);
    const F32 range_sq = (F32)draw_distance * (F32)draw_distance;
    WolfObjectProps& props = WolfObjectProps::instance();

    std::set<LLUUID> found;
    const S32 count = gObjectList.getNumObjects();
    for (S32 i = 0; i < count; ++i)
    {
        LLViewerObject* objectp = gObjectList.getObject(i);
        if (!objectp || objectp->isDead() || objectp->isAvatar() || objectp->isHUDAttachment() || objectp->isAttachment())
        {
            continue;
        }
        if (objectp->getPCode() != LL_PCODE_VOLUME || !objectp->getRegion())
        {
            continue;
        }
        const LLVector3 pos = objectp->getPositionAgent();
        const F32 dist_sq = (pos - camera_pos).lengthSquared();
        if (dist_sq > range_sq)
        {
            continue;
        }
        const LLVector3 scale = objectp->getScale();
        const F32 longest = llmax(scale.mV[VX], llmax(scale.mV[VY], scale.mV[VZ]));
        const F32 reach = NEAR_M + PER_METRE_M * longest;
        if (dist_sq > reach * reach)
        {
            continue;
        }
        // the lowest the prim can reach, whatever its turn: half its diagonal below its centre
        if (pos.mV[VZ] - 0.5f * scale.length() >= objectp->getRegion()->getWaterHeight())
        {
            continue;
        }
        Volume v;
        if (!volumeOf(objectp, v))
        {
            continue;
        }
        const LLUUID& id = objectp->getID();
        const WolfObjectProps::Props* before = props.get(id);
        props.want(objectp, before && matches(before->mDescriptionLower));   // a used one is kept fresh
        const WolfObjectProps::Props* known = props.get(id);
        if (known && matches(known->mDescriptionLower))
        {
            found.insert(id);
        }
    }
    // logged only when the set changes (no per-sweep line: paul-no-eternal-logs)
    if (found != mTagged)
    {
        LL_INFOS("WolfDry") << "air-filled prims near you: " << found.size() << " (was " << mTagged.size() << ")" << LL_ENDL;
    }
    mTagged.swap(found);
}

void WolfDryVolumes::collect()
{
    const U32 frame = LLFrameTimer::getFrameCount();
    if (frame == mFrameCollected)
    {
        return;
    }
    mFrameCollected = frame;
    mFrame.clear();
    if (!WolfGrid::showsWolfRegion())
    {
        return;
    }
    // [WOLF DRY HULLS] the measured boats, where they are DRAWN this frame (getRenderPosition / getRenderRotation: the
    // rocking offset WolfBoatRock puts on the drawable, and the sim's moves, are both in it)
    std::set<const LLViewerObject*> hull_roots;
    for (const auto& kv : mHulls)
    {
        if (!kv.second.mProfile)
        {
            continue;
        }
        LLViewerObject* root = gObjectList.findObject(kv.first);
        if (!root || root->isDead() || root->mDrawable.isNull())
        {
            continue;
        }
        const HullProfile& hp = *kv.second.mProfile;
        const LLQuaternion rot = root->getRenderRotation();
        Volume v;
        v.mKind = KIND_HULL;
        v.mCentre = root->getRenderPosition() + hp.mCentreLocal * rot;
        for (S32 k = 0; k < 3; ++k)
        {
            v.mAxis[k] = hp.mAxisLocal[k] * rot;
            v.mHalf[k] = hp.mHalfLocal[k];
        }
        v.mHull = kv.second.mProfile;
        mFrame.push_back(v);
        hull_roots.insert(root);
    }
    for (const LLUUID& id : mTagged)
    {
        LLViewerObject* objectp = gObjectList.findObject(id);
        // a measured boat's own wolfdry prims (HMS Wolfy's tubes) are inside its hull: not used twice
        if (objectp && !hull_roots.empty() && hull_roots.count(objectp->getRootEdit()))
        {
            continue;
        }
        Volume v;
        if (volumeOf(objectp, v))
        {
            mFrame.push_back(v);
        }
    }
    // The Tunnel tool's walkable boxes (wolffloatertunnels.h WolfTunnels::Box: centre and full sizes in region metres, turn
    // about +Z in degrees) whose floor is below the water AND that have sea over them (the ground at the box's centre or a
    // corner is under the water): a tunnel under the sea bed is air; one under a hill needs no slot of the eight.
    LLViewerRegion* rgn = LLWorld::getInstance()->getRegionFromID(WolfTunnels::boxesRegion());
    if (rgn)
    {
        const F32 water = rgn->getWaterHeight();
        for (const WolfTunnels::Box& b : WolfTunnels::boxes())
        {
            if (b.mCentre.mV[VZ] - b.mSize.mV[VZ] * 0.5f >= water)
            {
                continue;
            }
            const LLQuaternion yaw(b.mYawDeg * DEG_TO_RAD, LLVector3::z_axis);
            const LLVector3 hx = (LLVector3::x_axis * yaw) * (b.mSize.mV[VX] * 0.5f);
            const LLVector3 hy = (LLVector3::y_axis * yaw) * (b.mSize.mV[VY] * 0.5f);
            const LLVector3 spots[5] = { b.mCentre, b.mCentre + hx + hy, b.mCentre + hx - hy, b.mCentre - hx + hy, b.mCentre - hx - hy };
            bool sea_over = false;
            for (const LLVector3& p : spots)
            {
                if (rgn->getLand().resolveHeightRegion(p) < water)
                {
                    sea_over = true;
                    break;
                }
            }
            if (!sea_over)
            {
                continue;
            }
            Volume v;
            v.mKind = KIND_BOX;
            v.mCentre = rgn->getPosAgentFromRegion(b.mCentre);
            v.mAxis[0] = LLVector3::x_axis * yaw;
            v.mAxis[1] = LLVector3::y_axis * yaw;
            v.mAxis[2] = LLVector3::z_axis;
            for (S32 k = 0; k < 3; ++k)
            {
                v.mHalf[k] = llmax(b.mSize.mV[k] * 0.5f, MIN_HALF);
            }
            mFrame.push_back(v);
        }
    }
}

bool WolfDryVolumes::contains(const LLVector3& agent_pos) const
{
    const_cast<WolfDryVolumes*>(this)->collect();
    for (const Volume& v : mFrame)
    {
        if (v.contains(agent_pos))
        {
            return true;
        }
    }
    return false;
}

bool WolfDryVolumes::cameraInside() const
{
    return contains(LLViewerCamera::getInstance()->getOrigin());
}

void WolfDryVolumes::applyUniforms(LLShaderUniforms* uniforms, const F32* modelview)
{
    if (!uniforms || !modelview)
    {
        return;
    }
    collect();
    // The camera's volumes first, then the nearest (by the distance to the volume's bounding sphere).
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    std::vector<std::pair<F32, size_t>> pick;
    pick.reserve(mFrame.size());
    for (size_t i = 0; i < mFrame.size(); ++i)
    {
        const Volume& v = mFrame[i];
        const F32 d = v.contains(cam) ? -1.f : llmax(dist_vec(v.mCentre, cam) - v.boundingRadius(), 0.f);
        pick.emplace_back(d, i);
    }
    std::sort(pick.begin(), pick.end());
    // [WOLF DRY HULLS] at most MAX_SHADER_HULLS of the slots are hulls (their tables have that many places); a hull past
    // those gives its slot to the next volume
    std::vector<size_t> chosen;
    chosen.reserve(MAX_SHADER_VOLUMES);
    S32 hulls = 0;
    for (const auto& pk : pick)
    {
        if ((S32)chosen.size() >= MAX_SHADER_VOLUMES)
        {
            break;
        }
        if (mFrame[pk.second].mKind == KIND_HULL)
        {
            if (hulls >= MAX_SHADER_HULLS)
            {
                continue;
            }
            ++hulls;
        }
        chosen.push_back(pk.second);
    }
    const S32 n = (S32)chosen.size();

    uniforms->uniform1i(LLShaderMgr::WOLF_DRY_COUNT, n);
    if (n == 0)
    {
        return;
    }
    // waterFogF.glsl wolfDry: rows of the map from EYE space to the volume's unit space. In the agent frame row k is
    // (axis_k / half_k, -(axis_k . centre) / half_k); a point p_eye = MV p_agent, so row . p_agent = row . (MV^-1 p_eye)
    // = ((MV^-1)^T row) . p_eye - the same transform llsettingsvo.cpp applySpecial gives the water plane (invtrans).
    const glm::mat4 mv = glm::make_mat4(modelview);
    const glm::mat4 invtrans = glm::transpose(glm::inverse(mv));
    LLVector4 rows[MAX_SHADER_VOLUMES * 4];
    LLVector4 hull_rows[MAX_SHADER_HULLS * HULL_VEC4];
    S32 h = 0;
    for (S32 j = 0; j < n; ++j)
    {
        const Volume& v = mFrame[chosen[j]];
        for (S32 k = 0; k < 3; ++k)
        {
            const LLVector3 a = v.mAxis[k] / v.mHalf[k];
            const glm::vec4 re = invtrans * glm::vec4(a.mV[VX], a.mV[VY], a.mV[VZ], -(a * v.mCentre));
            rows[j * 4 + k].set(re.x, re.y, re.z, re.w);
        }
        // a hull is only chosen above while there is a table place for it (h < MAX_SHADER_HULLS) and always has one
        if (v.mKind == KIND_HULL && v.mHull)
        {
            // waterFogF.glsl wolfHull[h*25 + 0..23]: level-major, station-minor, 4 to a vec4; [h*25 + 24] the levels
            const HullProfile& hp = *v.mHull;
            for (S32 l = 0; l < HULL_LEVELS; ++l)
            {
                for (S32 i = 0; i < HULL_STATIONS; ++i)
                {
                    const S32 at = l * HULL_STATIONS + i;
                    hull_rows[h * HULL_VEC4 + at / 4].mV[at % 4] = hp.mHalf[l][i];
                }
            }
            hull_rows[h * HULL_VEC4 + HULL_VEC4 - 1].set(hp.mLevel[0], hp.mLevel[1], hp.mLevel[2], hp.mLevel[3]);
            rows[j * 4 + 3].set((F32)v.mKind, (F32)h, 0.f, 0.f);
            ++h;
        }
        else
        {
            rows[j * 4 + 3].set((F32)v.mKind, 0.f, 0.f, 0.f);
        }
    }
    uniforms->uniform4fv(LLShaderMgr::WOLF_DRY, (U32)(n * 4), rows);
    if (h > 0)
    {
        uniforms->uniform4fv(LLShaderMgr::WOLF_HULL, (U32)(h * HULL_VEC4), hull_rows);
    }
}

// [WOLF DRY HULLS 2026-10-28] ------------------------------------------------------------------------------------------

U64 WolfDryVolumes::hullSignature(LLViewerObject* root)
{
    // FNV-1a over what measureHull reads: each prim's volume (a new LLVolume on a LOD switch or a reshape), its face and
    // vertex counts (a mesh arriving fills its faces), its size and its place on the root. Positions to the centimetre.
    U64 h = 1469598103934665603ULL;
    auto mix = [&h](U64 v) { h ^= v; h *= 1099511628211ULL; };
    auto add = [&](LLViewerObject* o)
    {
        const LLVolume* vol = o->getVolume();
        mix((U64)(uintptr_t)vol);
        if (vol)
        {
            mix((U64)vol->getNumVolumeFaces());
            for (S32 f = 0; f < vol->getNumVolumeFaces(); ++f)
            {
                mix((U64)(U32)vol->getVolumeFace(f).mNumIndices);
            }
        }
        const LLVector3 sc = o->getScale();
        const LLVector3 ps = o == root ? LLVector3::zero : o->getPosition();
        for (S32 k = 0; k < 3; ++k)
        {
            mix((U64)(S64)llround(sc.mV[k] * 100.f));
            mix((U64)(S64)llround(ps.mV[k] * 100.f));
        }
    };
    add(root);
    for (LLViewerObject* kid : root->getChildren())
    {
        if (kid && !kid->isAvatar())
        {
            add(kid);
        }
    }
    return h;
}

void WolfDryVolumes::sweepHulls()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "WolfViewerDryHulls", true);
    if (!enabled)
    {
        mHulls.clear();
        return;
    }
    // The boats WolfBoatRock's own sweep found (no second walk of the object list): nearest first.
    const std::vector<LLUUID>& boats = WolfBoatRock::instance().boats();
    WolfObjectProps& props = WolfObjectProps::instance();
    std::map<LLUUID, HullCache> next;
    S32 budget = HULL_TRIANGLE_BUDGET;
    bool measured_one = false;
    for (const LLUUID& id : boats)
    {
        LLViewerObject* root = gObjectList.findObject(id);
        if (!root || root->isDead() || !root->getRegion())
        {
            continue;
        }
        // A boat whose name is not known yet waits for it (a physical or crewed hull rocks without one): a catamaran
        // keeps the sea between its hulls (multiHullName).
        const WolfObjectProps::Props* np = props.get(id);
        if (!np)
        {
            props.want(root);
            continue;
        }
        if (multiHullName(np->mDescriptionLower))
        {
            continue;
        }
        std::string name_lower = np->mName;
        LLStringUtil::toLower(name_lower);
        if (multiHullName(name_lower))
        {
            continue;
        }
        const U64 sig = hullSignature(root);
        const F32 water_offset = root->getRegion()->getWaterHeight() - root->getPositionRegion().mV[VZ];
        auto old = mHulls.find(id);
        if (old != mHulls.end() && old->second.mSignature == sig
            && fabsf(old->second.mWaterOffset - water_offset) < HULL_REMEASURE_WATER_M)
        {
            next[id] = old->second;
            continue;
        }
        if (budget <= 0 && measured_one)
        {
            if (old != mHulls.end())
            {
                next[id] = old->second;   // the old table until there is budget to measure it again
            }
            continue;
        }
        HullCache hc;
        hc.mSignature = sig;
        hc.mWaterOffset = water_offset;
        auto profile = std::make_shared<HullProfile>();
        S32 triangles = 0;
        if (measureHull(root, *profile, triangles))
        {
            hc.mProfile = profile;
        }
        budget -= triangles;
        measured_one = true;
        LL_DEBUGS("WolfDry") << "hull " << id << " [" << np->mName << "]: " << (hc.mProfile ? "air inside" : "not usable")
                             << ", " << triangles << " triangles read" << LL_ENDL;
        next[id] = hc;
    }
    // logged only when the number in use changes (paul-no-eternal-logs)
    auto usable = [](const std::map<LLUUID, HullCache>& m)
    {
        S32 c = 0;
        for (const auto& kv : m) { if (kv.second.mProfile) ++c; }
        return c;
    };
    const S32 now_usable = usable(next), was_usable = usable(mHulls);
    if (now_usable != was_usable)
    {
        LL_INFOS("WolfDry") << "boats with a dry hull near you: " << now_usable << " (was " << was_usable << ")" << LL_ENDL;
    }
    mHulls.swap(next);
}

// The hull is measured from the boat's own drawn geometry, in the ROOT prim's frame (so the table rides the boat however
// it moves or rocks):
//  - the boat's "up" is the root's own axis nearest the world's up (as the sim has it; a boat built on a turned root
//    still works); "along" is whichever of the other two the hull is longer on at the waterline;
//  - the waterline is the root's region's water height where the root is; the keel is the lowest visible point;
//  - every VISIBLE face of every prim (a face whose colour is almost fully transparent is not: physics colliders,
//    invisiprims, wolfdry tubes) is cut at the four levels (HULL_LEVEL_ABOVE_M above the waterline, the waterline, a third
//    and two thirds of the way down to the keel) and the half-width from the centre line taken at fine stations;
//  - the 24 stations the shader draws straight lines between are then LOWERED until no line passes the skin at any fine
//    station (the bow's hollow waterline would otherwise cut the sea beside the stem).
// Not usable (false): no volume, nothing visible at the waterline, the keel not below it, or smaller than a dinghy.
bool WolfDryVolumes::measureHull(LLViewerObject* root, HullProfile& out, S32& triangles)
{
    triangles = 0;
    LLViewerRegion* rgn = root ? root->getRegion() : nullptr;
    if (!rgn || root->isDead())
    {
        return false;
    }
    const LLQuaternion rr = root->getRotation();
    // world up in the root's frame, and the root axis nearest it
    const LLVector3 up_local = LLVector3::z_axis * ~rr;
    S32 ku = 2;
    for (S32 k = 0; k < 3; ++k)
    {
        if (fabsf(up_local.mV[k]) > fabsf(up_local.mV[ku])) ku = k;
    }
    const F32 up_sign = up_local.mV[ku] < 0.f ? -1.f : 1.f;
    const S32 ka = (ku + 1) % 3, kb = (ku + 2) % 3;
    const F32 z_wl = (rgn->getWaterHeight() - root->getPositionRegion().mV[VZ]) * fabsf(up_local.mV[ku]);

    // the prims, their faces to read, and each prim's map from volume space to the root's frame
    struct Part
    {
        const LLVolume* mVolume;
        LLVector3 mScale;
        LLQuaternion mRot;
        LLVector3 mPos;
        std::vector<S32> mFaces;
    };
    std::vector<Part> parts;
    auto add = [&](LLViewerObject* o, bool is_root)
    {
        const LLVolume* vol = o->getVolume();
        if (!vol || o->getPCode() != LL_PCODE_VOLUME || o->isRiggedMesh())
        {
            return;
        }
        Part p;
        p.mVolume = vol;
        p.mScale = o->getScale();
        p.mRot = is_root ? LLQuaternion() : o->getRotation();
        p.mPos = is_root ? LLVector3::zero : o->getPosition();
        const S32 nte = (S32)o->getNumTEs();
        for (S32 f = 0; f < vol->getNumVolumeFaces(); ++f)
        {
            // volume face f is drawn with texture entry f (llvovolume.cpp regenFaces: facep->setTEOffset(i))
            const LLTextureEntry* te = f < nte ? o->getTE((U8)f) : nullptr;
            if (te && te->getColor().mV[VALPHA] < HULL_MIN_ALPHA)
            {
                continue;
            }
            p.mFaces.push_back(f);
        }
        if (!p.mFaces.empty())
        {
            parts.push_back(std::move(p));
        }
    };
    add(root, true);
    for (LLViewerObject* kid : root->getChildren())
    {
        if (kid && !kid->isAvatar() && !kid->isDead())
        {
            add(kid, false);
        }
    }
    if (parts.empty())
    {
        return false;
    }
    auto to_boat = [&](const Part& p, const LLVector4a& v)
    {
        // Source: llvovolume.cpp volumePositionToAgent - volume space times the prim's size, then its turn and place
        // (here on the root: a child's getRotation / getPosition are relative to its parent, the root)
        LLVector3 q(v.getF32ptr()[0] * p.mScale.mV[VX], v.getF32ptr()[1] * p.mScale.mV[VY], v.getF32ptr()[2] * p.mScale.mV[VZ]);
        q = q * p.mRot + p.mPos;
        return LLVector3(q.mV[ka], q.mV[kb], q.mV[ku] * up_sign);
    };
    // pass 1: the keel (the lowest visible point)
    F32 keel = z_wl;
    for (const Part& p : parts)
    {
        for (S32 f : p.mFaces)
        {
            const LLVolumeFace& vf = p.mVolume->getVolumeFace(f);
            if (!vf.mPositions)
            {
                continue;
            }
            for (S32 i = 0; i < vf.mNumVertices; ++i)
            {
                keel = llmin(keel, to_boat(p, vf.mPositions[i]).mV[VZ]);
            }
        }
    }
    const F32 depth = z_wl - keel;
    if (depth < 0.05f)
    {
        return false;
    }
    const F32 zl[HULL_LEVELS] = { z_wl + HULL_LEVEL_ABOVE_M, z_wl, z_wl - depth / 3.f, z_wl - depth * 2.f / 3.f };
    // pass 2: the skin cut at each level
    std::vector<Seg> segs[HULL_LEVELS];
    for (const Part& p : parts)
    {
        for (S32 f : p.mFaces)
        {
            const LLVolumeFace& vf = p.mVolume->getVolumeFace(f);
            if (!vf.mPositions || !vf.mIndices)
            {
                continue;
            }
            for (S32 t = 0; t + 2 < vf.mNumIndices; t += 3)
            {
                const U16 i0 = vf.mIndices[t], i1 = vf.mIndices[t + 1], i2 = vf.mIndices[t + 2];
                if (i0 >= vf.mNumVertices || i1 >= vf.mNumVertices || i2 >= vf.mNumVertices)
                {
                    continue;
                }
                ++triangles;
                const LLVector3 c[3] = { to_boat(p, vf.mPositions[i0]), to_boat(p, vf.mPositions[i1]), to_boat(p, vf.mPositions[i2]) };
                const F32 zmin = llmin(c[0].mV[VZ], llmin(c[1].mV[VZ], c[2].mV[VZ]));
                const F32 zmax = llmax(c[0].mV[VZ], llmax(c[1].mV[VZ], c[2].mV[VZ]));
                for (S32 l = 0; l < HULL_LEVELS; ++l)
                {
                    const F32 z = zl[l];
                    if (z < zmin || z > zmax || zmin == zmax)
                    {
                        continue;
                    }
                    // the two edges that cross z (a corner exactly on it counts for the edge going up from it)
                    F32 pts[4];
                    S32 np = 0;
                    for (S32 e = 0; e < 3 && np < 4; ++e)
                    {
                        const LLVector3& a = c[e];
                        const LLVector3& b = c[(e + 1) % 3];
                        const bool above_a = a.mV[VZ] >= z, above_b = b.mV[VZ] >= z;
                        if (above_a == above_b)
                        {
                            continue;
                        }
                        const F32 s = (z - a.mV[VZ]) / (b.mV[VZ] - a.mV[VZ]);
                        pts[np++] = a.mV[VX] + (b.mV[VX] - a.mV[VX]) * s;
                        pts[np++] = a.mV[VY] + (b.mV[VY] - a.mV[VY]) * s;
                    }
                    if (np == 4)
                    {
                        segs[l].push_back({ pts[0], pts[1], pts[2], pts[3] });
                    }
                }
            }
        }
    }
    if (segs[1].empty())
    {
        return false;   // nothing visible at the waterline
    }
    // along = the longer way of the waterline cut; (x, y) = (along, across)
    F32 amin = F32_MAX, amax = -F32_MAX, bmin = F32_MAX, bmax = -F32_MAX;
    for (const Seg& g : segs[1])
    {
        amin = llmin(amin, llmin(g.a0, g.a1)); amax = llmax(amax, llmax(g.a0, g.a1));
        bmin = llmin(bmin, llmin(g.b0, g.b1)); bmax = llmax(bmax, llmax(g.b0, g.b1));
    }
    const bool along_a = (amax - amin) >= (bmax - bmin);
    auto sx0 = [along_a](const Seg& g) { return along_a ? g.a0 : g.b0; };
    auto sy0 = [along_a](const Seg& g) { return along_a ? g.b0 : g.a0; };
    auto sx1 = [along_a](const Seg& g) { return along_a ? g.a1 : g.b1; };
    auto sy1 = [along_a](const Seg& g) { return along_a ? g.b1 : g.a1; };
    const F32 yc = along_a ? 0.5f * (bmin + bmax) : 0.5f * (amin + amax);
    // the length: every cut at and below the waterline
    F32 xmin = F32_MAX, xmax = -F32_MAX;
    for (S32 l = 1; l < HULL_LEVELS; ++l)
    {
        for (const Seg& g : segs[l])
        {
            xmin = llmin(xmin, llmin(sx0(g), sx1(g)));
            xmax = llmax(xmax, llmax(sx0(g), sx1(g)));
        }
    }
    if (xmax - xmin < 2.f * HULL_MIN_HALF_M)
    {
        return false;
    }
    // the skin's half-width at fine stations, per level (-1: no skin there)
    constexpr S32 FINE = HULL_FINE_PER_GAP * (HULL_STATIONS - 1) + 1;
    const F32 fine_step = (xmax - xmin) / (F32)(FINE - 1);
    std::vector<F32> skin[HULL_LEVELS];
    for (S32 l = 0; l < HULL_LEVELS; ++l)
    {
        skin[l].assign(FINE, -1.f);
        for (const Seg& g : segs[l])
        {
            const F32 x0 = sx0(g), x1 = sx1(g), y0 = sy0(g), y1 = sy1(g);
            const F32 lo = llmin(x0, x1), hi = llmax(x0, x1);
            const S32 j0 = llmax(0, (S32)ceilf((lo - xmin) / fine_step - 1e-4f));
            const S32 j1 = llmin(FINE - 1, (S32)floorf((hi - xmin) / fine_step + 1e-4f));
            for (S32 j = j0; j <= j1; ++j)
            {
                const F32 x = xmin + fine_step * (F32)j;
                const F32 s = (fabsf(x1 - x0) < 1e-6f) ? 0.f : llclamp((x - x0) / (x1 - x0), 0.f, 1.f);
                skin[l][j] = llmax(skin[l][j], fabsf(y0 + (y1 - y0) * s - yc));
            }
        }
    }
    for (S32 j = 0; j < FINE; ++j)
    {
        // above the waterline with no skin (a low dinghy's gunwale is under the level): the waterline's
        if (skin[0][j] < 0.f) skin[0][j] = skin[1][j];
        for (S32 l = 0; l < HULL_LEVELS; ++l)
        {
            skin[l][j] = llmax(skin[l][j], 0.f);
        }
    }
    // the stations, lowered until the straight lines between them stay inside the skin at every fine station
    F32 station[HULL_LEVELS][HULL_STATIONS];
    F32 half_beam = 0.f;
    for (S32 l = 0; l < HULL_LEVELS; ++l)
    {
        F32* b = station[l];
        for (S32 i = 0; i < HULL_STATIONS; ++i)
        {
            b[i] = skin[l][i * HULL_FINE_PER_GAP];
        }
        for (S32 pass = 0; pass < HULL_FIT_PASSES; ++pass)
        {
            bool moved = false;
            for (S32 j = 0; j < FINE; ++j)
            {
                const S32 i = llmin(j / HULL_FINE_PER_GAP, HULL_STATIONS - 2);
                const F32 f = (F32)(j - i * HULL_FINE_PER_GAP) / (F32)HULL_FINE_PER_GAP;
                const F32 over = (1.f - f) * b[i] + f * b[i + 1] - skin[l][j];
                if (over > 1e-4f)
                {
                    // the least change to the two ends that brings the line down onto the skin here
                    const F32 w0 = 1.f - f, w1 = f, nn = w0 * w0 + w1 * w1;
                    b[i] = llmax(b[i] - over * w0 / nn, 0.f);
                    b[i + 1] = llmax(b[i + 1] - over * w1 / nn, 0.f);
                    moved = true;
                }
            }
            if (!moved)
            {
                break;
            }
        }
        for (S32 i = 0; i < HULL_STATIONS; ++i)
        {
            half_beam = llmax(half_beam, b[i]);
        }
    }
    if (half_beam < HULL_MIN_HALF_M)
    {
        return false;
    }
    // unit space: x along xmin..xmax, y across +-half_beam about yc, z keel .. waterline + HULL_TOP_ABOVE_M
    const F32 ztop = z_wl + HULL_TOP_ABOVE_M;
    const F32 zc = 0.5f * (keel + ztop), hz = 0.5f * (ztop - keel);
    const F32 xc = 0.5f * (xmin + xmax), hx = 0.5f * (xmax - xmin);
    for (S32 l = 0; l < HULL_LEVELS; ++l)
    {
        out.mLevel[l] = (zl[l] - zc) / hz;
        for (S32 i = 0; i < HULL_STATIONS; ++i)
        {
            out.mHalf[l][i] = station[l][i] / half_beam;
        }
    }
    LLVector3 e[3];
    e[ka].setVec(0.f, 0.f, 0.f); e[kb].setVec(0.f, 0.f, 0.f); e[ku].setVec(0.f, 0.f, 0.f);
    e[ka].mV[ka] = 1.f;
    e[kb].mV[kb] = 1.f;
    e[ku].mV[ku] = up_sign;
    const LLVector3 ex = along_a ? e[ka] : e[kb];
    const LLVector3 ey = along_a ? e[kb] : e[ka];
    const LLVector3 ez = e[ku];
    out.mAxisLocal[0] = ex;
    out.mAxisLocal[1] = ey;
    out.mAxisLocal[2] = ez;
    out.mHalfLocal[0] = llmax(hx, MIN_HALF);
    out.mHalfLocal[1] = llmax(half_beam, MIN_HALF);
    out.mHalfLocal[2] = llmax(hz, MIN_HALF);
    out.mCentreLocal = ex * xc + ey * yc + ez * zc;
    return true;
}
