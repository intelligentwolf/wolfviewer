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
#include "wolffloatertunnels.h"
#include "wolfgrid.h"
#include "wolfobjectprops.h"

const std::string WolfDryVolumes::KEYWORD("wolfdry");

namespace
{
    // The smallest half size used (a flattened prim is still a volume, never a division by zero in the unit map).
    constexpr F32 MIN_HALF = 0.01f;
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
    for (const LLUUID& id : mTagged)
    {
        Volume v;
        if (volumeOf(gObjectList.findObject(id), v))
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
    const S32 n = llmin((S32)pick.size(), MAX_SHADER_VOLUMES);
    std::partial_sort(pick.begin(), pick.begin() + n, pick.end());

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
    for (S32 j = 0; j < n; ++j)
    {
        const Volume& v = mFrame[pick[j].second];
        for (S32 k = 0; k < 3; ++k)
        {
            const LLVector3 a = v.mAxis[k] / v.mHalf[k];
            const glm::vec4 re = invtrans * glm::vec4(a.mV[VX], a.mV[VY], a.mV[VZ], -(a * v.mCentre));
            rows[j * 4 + k].set(re.x, re.y, re.z, re.w);
        }
        rows[j * 4 + 3].set((F32)v.mKind, 0.f, 0.f, 0.f);
    }
    uniforms->uniform4fv(LLShaderMgr::WOLF_DRY, (U32)(n * 4), rows);
}
