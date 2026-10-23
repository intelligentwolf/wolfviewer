/**
 * @file wolfcavelights.cpp
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

// [WOLF CAVE LIGHTS 2026-10-23] See wolfcavelights.h.

#include "llviewerprecompiledheaders.h"

#include "wolfcavelights.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <set>

#include "fscommon.h"
#include "llagent.h"
#include "llcoros.h"
#include "lleventcoro.h"
#include "llinventorymodel.h"
#include "llsdutil.h"
#include "llmaterial.h"
#include "llmaterialmgr.h"
#include "llnotificationsutil.h"
#include "lldate.h"
#include "llprimitive.h"
#include "llviewerassetupload.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvolume.h"
#include "llvolumemessage.h"
#include "message.h"
#include "object_flags.h"

namespace
{
    using namespace WolfCaveLights;

    // ── the plan ─────────────────────────────────────────────────────────────────────────────────────────────────────
    /** Lamps on the walls this far apart along a route (Paul's brief: "about every 8-10 m"). */
    constexpr F32 LAMP_SPACING_M = 9.f;
    /** No wall lamp nearer a way in or out than this (the mouths are open to the outside). */
    constexpr F32 END_CLEAR_M = 4.f;
    /** No wall lamp this near a crossroads (the crossroads has its own). */
    constexpr F32 CROSS_CLEAR_M = 4.f;
    /** Crossroads closer than this are one. */
    constexpr F32 CROSS_MERGE_M = 3.f;
    /** A route's end this near another route's side (past its half-width) makes a T crossroads. */
    constexpr F32 TEE_REACH_M = 0.75f;
    /** Steps of the walk from the middle of the way out to the wall. */
    constexpr F32 WALK_STEP_M = 0.05f;
    constexpr F32 WALK_MAX_M = 40.f;
    /** A wall lamp's back is this far off the rock (so no rock bump hides it). */
    constexpr F32 WALL_GAP_M = 0.03f;
    /** Room under a lamp's top and above its bottom. */
    constexpr F32 TOP_ROOM_M = 0.3f, BOTTOM_ROOM_M = 0.4f;
    /** Light reach: wall lamps a little over the spacing so the light meets between them; rooms by size. */
    constexpr F32 WALL_RADIUS_M = 10.f, HIDDEN_RADIUS_M = 12.f;
    /** LLLightParams' own limit (llprimitive.cpp LIGHT_MAX_RADIUS = 20). */
    constexpr F32 RADIUS_MAX_M = 20.f;

    // ── the torch assets (firestorm/region-terrain-work/cave-lights/make_torch_assets.py, out/deliver_report.json) ──────────
    // CC0 "Fantasy Torch Pack 1" by rubberduck (opengameart.org/content/fantasy-torch-pack-1); stored 2026-10-23 with fixed ids.
    const LLUUID TORCH_WALL_MESH("8318eedd-0950-5739-95c5-291a6e123b18");
    const LLUUID TORCH_STAND_MESH("acd59229-4314-567b-b221-5ec97b697dd3");
    const LLUUID TORCH_DIFFUSE("fdac7a79-b9cb-5e07-bd47-9b2bbe3d318e");
    const LLUUID TORCH_NORMAL("b0021b50-d6bf-53da-a281-e3321b48e19c");
    // The prim sizes and the bowl's middle (the top rim's centre, in the prim's own frame) from the same report.
    const LLVector3 TORCH_WALL_SCALE(0.3778f, 0.2218f, 0.85f), TORCH_WALL_BOWL(-0.078f, 0.f, 0.4228f);
    const LLVector3 TORCH_STAND_SCALE(0.3224f, 0.3138f, 1.35f), TORCH_STAND_BOWL(-0.038f, -0.0147f, 0.6726f);
    /** The flame sits this far below the bowl's rim. */
    constexpr F32 FLAME_DROP_M = 0.03f;
    const LLVector3 FLAME_SCALE(0.05f, 0.05f, 0.05f);

    const char* FLAME_ITEM_NAME = "Wolf cave torch flame";
    const char* FLAME_ITEM_DESC = "WolfViewer Light up, flame v1 (sets the particles once; no timer)";
    // Paul: "can we use particles for the flame please?" - set once, then nothing runs (no timer, listen or sensor), so a cave
    // full of torches costs the region next to nothing. The particles are a property of the prim and keep going on their own.
    // Every PSYS_ constant is one OpenSim knows (LSL_Constants.cs:111-159; LSL_Api.cs:7702-7704 blend and glow rules).
    const char* FLAME_LSL =
        "// Wolf cave torch flame - placed by WolfViewer's Light up. Sets the flame once and does nothing else:\n"
        "// no timer, no listen, no sensor. The particles belong to this prim and keep burning on their own.\n"
        "flame()\n"
        "{\n"
        "    llParticleSystem([\n"
        "        PSYS_PART_FLAGS, PSYS_PART_EMISSIVE_MASK | PSYS_PART_INTERP_COLOR_MASK | PSYS_PART_INTERP_SCALE_MASK | PSYS_PART_FOLLOW_SRC_MASK,\n"
        "        PSYS_SRC_PATTERN, PSYS_SRC_PATTERN_ANGLE_CONE,\n"
        "        PSYS_SRC_ANGLE_BEGIN, 0.0, PSYS_SRC_ANGLE_END, 0.35,\n"
        "        PSYS_SRC_BURST_RADIUS, 0.02,\n"
        "        PSYS_SRC_BURST_RATE, 0.07,\n"
        "        PSYS_SRC_BURST_PART_COUNT, 2,\n"
        "        PSYS_SRC_BURST_SPEED_MIN, 0.10, PSYS_SRC_BURST_SPEED_MAX, 0.28,\n"
        "        PSYS_SRC_ACCEL, <0.0, 0.0, 0.35>,\n"
        "        PSYS_SRC_MAX_AGE, 0.0,\n"
        "        PSYS_PART_MAX_AGE, 0.55,\n"
        "        PSYS_PART_START_COLOR, <1.0, 0.50, 0.10>, PSYS_PART_END_COLOR, <1.0, 0.90, 0.35>,\n"
        "        PSYS_PART_START_ALPHA, 0.9, PSYS_PART_END_ALPHA, 0.0,\n"
        "        PSYS_PART_START_SCALE, <0.10, 0.14, 0.0>, PSYS_PART_END_SCALE, <0.03, 0.06, 0.0>,\n"
        "        PSYS_PART_START_GLOW, 0.2, PSYS_PART_END_GLOW, 0.0,\n"
        "        PSYS_PART_BLEND_FUNC_SOURCE, PSYS_PART_BF_SOURCE_ALPHA,\n"
        "        PSYS_PART_BLEND_FUNC_DEST, PSYS_PART_BF_ONE\n"
        "    ]);\n"
        "}\n"
        "\n"
        "default\n"
        "{\n"
        "    state_entry() { flame(); }\n"
        "    on_rez(integer start) { flame(); }\n"
        "}\n";

    // ── the fittings ─────────────────────────────────────────────────────────────────────────────────────────────────
    enum EShape { SHAPE_MESH, SHAPE_CYLINDER, SHAPE_FRUSTUM, SHAPE_CUBE };
    struct Fitting
    {
        EShape mShape = SHAPE_CUBE;
        LLVector3 mScale;               // x along the wall, y out of it, z up
        F32 mTopRatio = 1.f;            // SHAPE_FRUSTUM: the top's size (LLVolumeParams setRatio)
        LLUUID mMesh;
        bool mFlame = false;            // a linked flame child at mBowl carries the light and the particle script
        LLVector3 mBowl;
        F32 mWantTop = 2.f;             // the top of a wall fitting above the floor, when the rock leaves room
        LLColor4 mColour = LLColor4::white;
        bool mFullbright = false;
        F32 mGlow = 0.f;
        U8 mShiny = 0;
        U8 mMaterial = LL_MCODE_STONE;  // material_codes.h
        F32 mIntensity = 1.f;
        F32 mFalloff = 0.75f;
    };

    LLColor3 lightColour(bool warm)
    {
        return warm ? LLColor3(1.f, 0.68f, 0.38f) : LLColor3(0.62f, 0.80f, 1.f);
    }

    Fitting fitting(EStyle style, bool standing, bool warm)
    {
        Fitting f;
        const LLColor3 c = lightColour(warm);
        switch (style)
        {
        case STYLE_TORCHES:
            f.mShape = SHAPE_MESH;
            f.mMesh = standing ? TORCH_STAND_MESH : TORCH_WALL_MESH;
            f.mScale = standing ? TORCH_STAND_SCALE : TORCH_WALL_SCALE;
            f.mBowl = standing ? TORCH_STAND_BOWL : TORCH_WALL_BOWL;
            f.mFlame = true;
            f.mWantTop = 2.05f;
            f.mMaterial = LL_MCODE_METAL;
            f.mIntensity = 1.f;
            f.mFalloff = 0.75f;
            break;
        case STYLE_LAMPS:
            f.mShape = SHAPE_CYLINDER;
            f.mScale = standing ? LLVector3(0.32f, 0.32f, 0.5f) : LLVector3(0.16f, 0.16f, 0.28f);
            f.mColour = LLColor4(llmin(1.f, c.mV[0] + 0.25f), llmin(1.f, c.mV[1] + 0.25f), llmin(1.f, c.mV[2] + 0.25f), 1.f);
            f.mFullbright = true;
            f.mGlow = 0.2f;
            f.mWantTop = 2.1f;
            f.mMaterial = LL_MCODE_GLASS;
            f.mIntensity = 0.9f;
            break;
        case STYLE_LANTERNS:
            f.mShape = SHAPE_FRUSTUM;
            f.mTopRatio = 0.65f;
            f.mScale = standing ? LLVector3(0.3f, 0.3f, 0.45f) : LLVector3(0.22f, 0.22f, 0.32f);
            f.mColour = LLColor4(c.mV[0], c.mV[1], c.mV[2], 1.f);
            f.mFullbright = true;
            f.mGlow = 0.3f;
            f.mWantTop = 2.1f;
            f.mMaterial = LL_MCODE_GLASS;
            f.mIntensity = 0.9f;
            break;
        case STYLE_CRYSTALS:
            f.mShape = SHAPE_FRUSTUM;
            f.mTopRatio = 0.1f;
            f.mScale = standing ? LLVector3(0.4f, 0.4f, 1.f) : LLVector3(0.16f, 0.16f, 0.42f);
            f.mColour = LLColor4(llmin(1.f, c.mV[0] * 0.6f + 0.4f), llmin(1.f, c.mV[1] * 0.6f + 0.4f), llmin(1.f, c.mV[2] * 0.6f + 0.4f), 0.85f);
            f.mFullbright = true;
            f.mGlow = 0.35f;
            f.mShiny = 3;
            f.mWantTop = 1.9f;
            f.mMaterial = LL_MCODE_GLASS;
            f.mIntensity = 0.8f;
            f.mFalloff = 0.9f;
            break;
        case STYLE_HIDDEN:
        default:
            f.mShape = SHAPE_CUBE;
            f.mScale = LLVector3(0.1f, 0.1f, 0.1f);
            f.mColour = LLColor4(1.f, 1.f, 1.f, 0.f);   // nothing to see: only the light
            f.mWantTop = 2.6f;
            f.mIntensity = 1.f;
            f.mFalloff = 0.75f;
            break;
        }
        return f;
    }

    // ── planning helpers ─────────────────────────────────────────────────────────────────────────────────────────────
    F32 routeLength(const Route& r)
    {
        F32 l = 0.f;
        for (size_t k = 1; k < r.mPts.size(); ++k) l += dist_vec(r.mPts[k], r.mPts[k - 1]);
        return l;
    }

    /** The point at arc length t along the route and the direction there. */
    void routeAt(const Route& r, F32 t, LLVector2& p, LLVector2& u)
    {
        F32 acc = 0.f;
        for (size_t k = 1; k < r.mPts.size(); ++k)
        {
            const F32 l = dist_vec(r.mPts[k], r.mPts[k - 1]);
            if (acc + l >= t || k + 1 == r.mPts.size())
            {
                const F32 f = l > 1e-6f ? llclamp((t - acc) / l, 0.f, 1.f) : 0.f;
                p = r.mPts[k - 1] + (r.mPts[k] - r.mPts[k - 1]) * f;
                u = r.mPts[k] - r.mPts[k - 1];
                u.normVec();
                return;
            }
            acc += l;
        }
        p = r.mPts.empty() ? LLVector2() : r.mPts.front();
        u = LLVector2(1.f, 0.f);
    }

    /** Nearest point on a route: distance, the point, the piece's direction and its arc length. */
    F32 nearestOn(const Route& r, const LLVector2& q, LLVector2& at, LLVector2& dir, F32& t)
    {
        F32 best = 1e30f, acc = 0.f;
        for (size_t k = 1; k < r.mPts.size(); ++k)
        {
            const LLVector2 a = r.mPts[k - 1], v = r.mPts[k] - r.mPts[k - 1];
            const F32 l2 = v.lengthSquared(), l = sqrtf(l2);
            const F32 f = l2 > 1e-9f ? llclamp(((q - a) * v) / l2, 0.f, 1.f) : 0.f;
            const LLVector2 p = a + v * f;
            const F32 d = dist_vec(p, q);
            if (d < best)
            {
                best = d;
                at = p;
                dir = l > 1e-6f ? v / l : LLVector2(1.f, 0.f);
                t = acc + f * l;
            }
            acc += l;
        }
        return best;
    }

    /** Where two pieces cross (proper crossing, not at their ends). */
    bool crossing(const LLVector2& a, const LLVector2& b, const LLVector2& c, const LLVector2& d, LLVector2& out)
    {
        const LLVector2 r = b - a, s = d - c;
        const F32 den = r.mV[0] * s.mV[1] - r.mV[1] * s.mV[0];
        if (fabsf(den) < 1e-6f) return false;
        const LLVector2 w = c - a;
        const F32 t = (w.mV[0] * s.mV[1] - w.mV[1] * s.mV[0]) / den;
        const F32 u = (w.mV[0] * r.mV[1] - w.mV[1] * r.mV[0]) / den;
        if (t <= 0.f || t >= 1.f || u <= 0.f || u >= 1.f) return false;
        out = a + r * t;
        return true;
    }

    struct Cross { LLVector2 mAt, mA, mB; };

    std::vector<Cross> crossroads(const std::vector<Route>& routes)
    {
        std::vector<Cross> out;
        auto add = [&](const LLVector2& at, LLVector2 a, LLVector2 b)
        {
            for (const Cross& c : out) if (dist_vec(c.mAt, at) < CROSS_MERGE_M) return;
            a.normVec();
            b.normVec();
            out.push_back(Cross{ at, a, b });
        };
        // two pieces crossing (the same route's pieces that are not neighbours, or two routes)
        for (size_t i = 0; i < routes.size(); ++i)
            for (size_t j = i; j < routes.size(); ++j)
            {
                const std::vector<LLVector2>& p = routes[i].mPts;
                const std::vector<LLVector2>& q = routes[j].mPts;
                for (size_t a = 1; a < p.size(); ++a)
                    for (size_t b = (i == j ? a + 2 : 1); b < q.size(); ++b)
                    {
                        LLVector2 at;
                        if (crossing(p[a - 1], p[a], q[b - 1], q[b], at)) add(at, p[a] - p[a - 1], q[b] - q[b - 1]);
                    }
            }
        // a route ending on another's side (a T)
        for (size_t i = 0; i < routes.size(); ++i)
        {
            const std::vector<LLVector2>& p = routes[i].mPts;
            if (p.size() < 2) continue;
            const LLVector2 ends[2] = { p.front(), p.back() };
            const LLVector2 dirs[2] = { p[1] - p[0], p[p.size() - 1] - p[p.size() - 2] };
            for (S32 e = 0; e < 2; ++e)
                for (size_t j = 0; j < routes.size(); ++j)
                {
                    if (j == i || routes[j].mPts.size() < 2) continue;
                    LLVector2 at, dir;
                    F32 t = 0.f;
                    const F32 d = nearestOn(routes[j], ends[e], at, dir, t);
                    const F32 len = routeLength(routes[j]);
                    if (d <= routes[j].mHalfW + TEE_REACH_M && t > 1.f && t < len - 1.f) add(at, dirs[e], dir);
                }
        }
        return out;
    }

    /** From `p0` (in the air) along `n` to the rock: the distance to the wall, or < 0 when there is none within reach. */
    F32 wallDistance(const field_f& field, const LLVector2& p0, const LLVector2& n)
    {
        F32 s = 0.f, fl = 0.f, cl = 0.f;
        if (!field(p0.mV[0], p0.mV[1], s, fl, cl) || s <= 0.f) return -1.f;
        for (F32 d = WALK_STEP_M; d <= WALK_MAX_M; d += WALK_STEP_M)
        {
            const LLVector2 q = p0 + n * d;
            if (!field(q.mV[0], q.mV[1], s, fl, cl) || s <= 0.f) return d;
        }
        return -1.f;
    }

    /** A wall fitting on the wall `n` points to (from `p0`, in the air). The fitting's back (local +Y) faces the wall. */
    bool wallLamp(const field_f& field, const LLVector2& p0, LLVector2 n, const Fitting& f, bool hidden, Lamp& out)
    {
        n.normVec();
        const F32 dw = wallDistance(field, p0, n);
        if (dw < 0.f) return false;
        const F32 yaw = atan2f(-n.mV[0], n.mV[1]);   // R(yaw) * (0, 1) = (-sin, cos) = n
        F32 s = 0.f, fl = 0.f, cl = 0.f;
        if (hidden)
        {
            // only a light, a metre in from the wall and near the roof
            const LLVector2 q = p0 + n * llmax(0.f, dw - 1.f);
            if (!field(q.mV[0], q.mV[1], s, fl, cl) || cl < 1.f) return false;
            out.mPos.set(q.mV[0], q.mV[1], fl + llmin(cl - 0.4f, f.mWantTop));
            out.mYaw = yaw;
            out.mStanding = false;
            out.mRadius = HIDDEN_RADIUS_M;
            return true;
        }
        const F32 h = f.mScale.mV[VZ];
        // the closest place to the wall with room for the fitting under the roof (an arched cave wall leans in overhead)
        for (F32 dm = dw - f.mScale.mV[VY] * 0.5f - WALL_GAP_M; dm >= 0.2f; dm -= 0.1f)
        {
            const LLVector2 q = p0 + n * dm;
            if (!field(q.mV[0], q.mV[1], s, fl, cl) || s <= f.mScale.mV[VY] * 0.5f) continue;
            if (cl < h + TOP_ROOM_M + BOTTOM_ROOM_M) continue;
            const F32 centre = llclamp(f.mWantTop - h * 0.5f, h * 0.5f + BOTTOM_ROOM_M, cl - TOP_ROOM_M - h * 0.5f);
            out.mPos.set(q.mV[0], q.mV[1], fl + centre);
            out.mYaw = yaw;
            out.mStanding = false;
            out.mRadius = WALL_RADIUS_M;
            return true;
        }
        return false;
    }

    /** A fitting standing on a room's floor, off the route's line by `off` metres (0 = the middle). */
    bool floorLamp(const field_f& field, const LLVector2& q, const LLVector2& face, const Fitting& f, Lamp& out)
    {
        F32 s = 0.f, fl = 0.f, cl = 0.f;
        if (!field(q.mV[0], q.mV[1], s, fl, cl)) return false;
        const F32 half = llmax(f.mScale.mV[VX], f.mScale.mV[VY]) * 0.5f;
        if (s < half + 0.3f || cl < f.mScale.mV[VZ] + TOP_ROOM_M) return false;
        out.mPos.set(q.mV[0], q.mV[1], fl + f.mScale.mV[VZ] * 0.5f + 0.01f);
        out.mYaw = atan2f(-face.mV[0], face.mV[1]);
        out.mStanding = true;
        return true;
    }

    // ── placing ──────────────────────────────────────────────────────────────────────────────────────────────────────
    bool inAgentRegion(const LLUUID& region)
    {
        return gAgent.getRegion() && gAgent.getRegion()->getRegionID() == region;
    }

    LLViewerObject* liveObject(const LLUUID& id)
    {
        LLViewerObject* o = gObjectList.findObject(id);
        return (o && !o->isDead()) ? o : nullptr;
    }

    /** Source: lltoolplacer.cpp LLToolPlacer::addObject (the ObjectAdd message and its volume shapes: cube, cylinder) and
     *  fsfloaterimport.cpp createPrim. RayEndIsIntersection = 1 makes the region put the prim at RayEnd + 0.25 m up
     *  (Scene.cs GetNewRezLocation: no raycast, pos = RayEnd, pos.Z += 0.5 / 2 for the 0.5 m scale AddNewPrim passes), so
     *  RayEnd is the wanted centre 0.25 m lower. The region checks the rez rules there (Scene.cs AddNewPrim -> CanRezObject;
     *  refused: "You cannot create objects here."). Not created selected, so the build tool's defaults and selection stay out. */
    void sendObjectAdd(const Fitting& f, const LLVector3& centre, const LLQuaternion& rot, LLViewerRegion* rgn)
    {
        LLVolumeParams vp;
        switch (f.mShape)
        {
        case SHAPE_CYLINDER: vp.setType(LL_PCODE_PROFILE_CIRCLE, LL_PCODE_PATH_LINE); break;
        default:             vp.setType(LL_PCODE_PROFILE_SQUARE, LL_PCODE_PATH_LINE); break;
        }
        vp.setBeginAndEndS(0.f, 1.f);
        vp.setBeginAndEndT(0.f, 1.f);
        vp.setRatio(f.mShape == SHAPE_FRUSTUM ? f.mTopRatio : 1.f, f.mShape == SHAPE_FRUSTUM ? f.mTopRatio : 1.f);
        vp.setShear(0.f, 0.f);
        const LLVector3 ray_end = centre - LLVector3(0.f, 0.f, 0.25f);
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_ObjectAdd);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->addUUIDFast(_PREHASH_GroupID, FSCommon::getGroupForRezzing());
        msg->nextBlockFast(_PREHASH_ObjectData);
        msg->addU8Fast(_PREHASH_Material, f.mMaterial);
        msg->addU32Fast(_PREHASH_AddFlags, (U32)0);
        LLVolumeMessage::packVolumeParams(&vp, msg);
        msg->addU8Fast(_PREHASH_PCode, LL_PCODE_VOLUME);
        msg->addVector3Fast(_PREHASH_Scale, f.mScale);
        msg->addQuatFast(_PREHASH_Rotation, rot);
        msg->addVector3Fast(_PREHASH_RayStart, ray_end);
        msg->addVector3Fast(_PREHASH_RayEnd, ray_end);
        msg->addU8Fast(_PREHASH_BypassRaycast, (U8)1);
        msg->addU8Fast(_PREHASH_RayEndIsIntersection, (U8)1);
        msg->addU8Fast(_PREHASH_State, (U8)0);
        msg->addUUIDFast(_PREHASH_RayTargetID, LLUUID::null);
        msg->sendReliable(rgn->getHost());
    }

    /** Make one prim and wait for the region to send it: a root object we own that was not there before, at the centre. */
    LLUUID makePrim(const Fitting& f, const LLVector3& centre, const LLQuaternion& rot, const LLUUID& region, std::string& error)
    {
        LLViewerRegion* rgn = gAgent.getRegion();
        if (!rgn || rgn->getRegionID() != region) { error = "you left the region."; return LLUUID::null; }
        std::set<LLUUID> before;   // what is already there (another lamp, someone's prim) is never taken for the new one
        for (S32 i = 0, n = gObjectList.getNumObjects(); i < n; ++i)
        {
            LLViewerObject* o = gObjectList.getObject(i);
            if (o && !o->isDead() && o->getRegion() == rgn && dist_vec(o->getPositionRegion(), centre) < 1.f) before.insert(o->getID());
        }
        sendObjectAdd(f, centre, rot, rgn);
        for (S32 t = 0; t < 150; ++t)   // 15 s
        {
            llcoro::suspendUntilTimeout(0.1f);
            if (!inAgentRegion(region)) { error = "you left the region."; return LLUUID::null; }
            for (S32 i = 0, n = gObjectList.getNumObjects(); i < n; ++i)
            {
                LLViewerObject* o = gObjectList.getObject(i);
                if (!o || o->isDead() || !o->isRoot() || o->getRegion() != gAgent.getRegion() || before.count(o->getID())) continue;
                if (o->getPCode() != LL_PCODE_VOLUME || !o->permYouOwner()) continue;
                if (dist_vec(o->getPositionRegion(), centre) > 0.3f) continue;
                return o->getID();
            }
        }
        error = llformat("the region did not make a lamp at %.0f, %.0f, %.0f m. You may not be allowed to build on that land, or it may be "
                         "full; the region's own message, if it sent one, is on your screen.", centre.mV[VX], centre.mV[VY], centre.mV[VZ]);
        return LLUUID::null;
    }

    /** Source: fsfloaterimport.cpp processPrimCreated (ObjectName / ObjectDescription). */
    void nameObject(LLViewerObject* o, const std::string& name, const std::string& desc)
    {
        LLViewerRegion* rgn = o->getRegion();
        if (!rgn) return;
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_ObjectName);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->nextBlockFast(_PREHASH_ObjectData);
        msg->addU32Fast(_PREHASH_LocalID, o->getLocalID());
        msg->addStringFast(_PREHASH_Name, name);
        msg->sendReliable(rgn->getHost());
        msg->newMessageFast(_PREHASH_ObjectDescription);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->nextBlockFast(_PREHASH_ObjectData);
        msg->addU32Fast(_PREHASH_LocalID, o->getLocalID());
        msg->addStringFast(_PREHASH_Description, desc);
        msg->sendReliable(rgn->getHost());
    }

    /** Source: fsfloaterimport.cpp processPrimCreated (sculpt, light: setParameterEntry sets it here and sends it; faces:
     *  setTE... then sendTEUpdate; materials: LLMaterialMgr::put, the build floater's way for a normal map). */
    void dressFitting(LLViewerObject* o, const Fitting& f)
    {
        if (f.mShape == SHAPE_MESH)
        {
            LLSculptParams sculpt;
            sculpt.setSculptTexture(f.mMesh, LL_SCULPT_TYPE_MESH);
            o->setParameterEntry(LLNetworkData::PARAMS_SCULPT, sculpt, true);
        }
        const S32 faces = o->getNumTEs();
        for (S32 te = 0; te < faces; ++te)
        {
            o->setTETexture((U8)te, f.mShape == SHAPE_MESH ? TORCH_DIFFUSE : IMG_WHITE);
            o->setTEColor((U8)te, f.mColour);
            o->setTEFullbright((U8)te, f.mFullbright ? 1 : 0);
            o->setTEGlow((U8)te, f.mGlow);
            o->setTEShiny((U8)te, f.mShiny);
        }
        o->sendTEUpdate();
        if (f.mShape == SHAPE_MESH)
        {
            LLMaterial mat;
            mat.setNormalID(TORCH_NORMAL);
            for (S32 te = 0; te < faces; ++te) LLMaterialMgr::getInstance()->put(o->getID(), (U8)te, mat);
        }
    }

    void lightOn(LLViewerObject* o, const Fitting& f, bool warm, F32 radius)
    {
        const LLColor3 c = lightColour(warm);
        LLLightParams light;
        light.setSRGBColor(LLColor4(c.mV[0], c.mV[1], c.mV[2], f.mIntensity));
        light.setRadius(llmin(radius, RADIUS_MAX_M));
        light.setFalloff(f.mFalloff);
        light.setCutoff(0.f);
        o->setParameterEntry(LLNetworkData::PARAMS_LIGHT, light, true);
    }

    /** The flame: a tiny invisible prim (linked at the bowl), the light and the particle script live in it. */
    void dressFlame(LLViewerObject* o)
    {
        const S32 faces = o->getNumTEs();
        for (S32 te = 0; te < faces; ++te)
        {
            o->setTETexture((U8)te, IMG_WHITE);
            o->setTEColor((U8)te, LLColor4(1.f, 1.f, 1.f, 0.f));
        }
        o->sendTEUpdate();
    }

    /** Source: llselectmgr.cpp sendLink (ObjectLink, packObjectLocalID); OpenSim LLClientView.cs HandleObjectLink takes the
     *  FIRST block as the root and the rest as children (Scene.Inventory.cs LinkObjects keeps each child where it is, so the
     *  flame keeps its place at the bowl and from then on moves and turns with the fitting). */
    bool link(const LLUUID& root_id, const LLUUID& child_id, const LLUUID& region, std::string& error)
    {
        LLViewerObject* root = liveObject(root_id);
        LLViewerObject* child = liveObject(child_id);
        if (!root || !child || !root->getRegion()) { error = "a lamp went away while it was being put together."; return false; }
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_ObjectLink);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->nextBlockFast(_PREHASH_ObjectData);
        msg->addU32Fast(_PREHASH_ObjectLocalID, root->getLocalID());
        msg->nextBlockFast(_PREHASH_ObjectData);
        msg->addU32Fast(_PREHASH_ObjectLocalID, child->getLocalID());
        msg->sendReliable(root->getRegion()->getHost());
        for (S32 t = 0; t < 100; ++t)   // 10 s
        {
            llcoro::suspendUntilTimeout(0.1f);
            if (!inAgentRegion(region)) { error = "you left the region."; return false; }
            LLViewerObject* c = liveObject(child_id);
            if (!c) { error = "a lamp's flame went away while it was being linked."; return false; }
            LLViewerObject* p = (LLViewerObject*)c->getParent();
            if (p && p->getID() == root_id) return true;
        }
        error = "the region did not link a lamp's flame to its torch (you may not be allowed to change objects here).";
        return false;
    }

    /** The flame script in the agent's inventory: found (made before), or made now (Source: particleeditor.cpp
     *  createScriptInventoryItem / callbackReturned - create_inventory_item, then the text through the UpdateScriptAgent
     *  capability, LLScriptAssetUpload's agent form). Call from a coroutine. */
    LLUUID flameScript(std::string& error)
    {
        const LLUUID folder = gInventory.findCategoryUUIDForType(LLFolderType::FT_LSL_TEXT);
        if (folder.isNull()) { error = "your Scripts inventory folder has not loaded yet."; return LLUUID::null; }
        LLInventoryModel::cat_array_t* cats = nullptr;
        LLInventoryModel::item_array_t* items = nullptr;
        gInventory.getDirectDescendentsOf(folder, cats, items);
        if (items)
        {
            for (const LLPointer<LLViewerInventoryItem>& it : *items)
            {
                if (it && it->getName() == FLAME_ITEM_NAME && it->getDescription() == FLAME_ITEM_DESC
                    && it->getType() == LLAssetType::AT_LSL_TEXT && it->getPermissions().getOwner() == gAgent.getID())
                    return it->getUUID();
            }
        }
        const std::string url = gAgent.getRegionCapability("UpdateScriptAgent");
        if (url.empty()) { error = "this region cannot take a script from the viewer (no UpdateScriptAgent capability)."; return LLUUID::null; }
        auto made = std::make_shared<LLUUID>();
        auto answered = std::make_shared<bool>(false);
        LLPermissions perm;
        perm.init(gAgent.getID(), gAgent.getID(), LLUUID::null, LLUUID::null);
        perm.initMasks(PERM_ALL, PERM_ALL, PERM_ALL, PERM_ALL, PERM_ALL);
        create_inventory_item(gAgent.getID(), gAgent.getSessionID(), folder, LLTransactionID::tnull, FLAME_ITEM_NAME, FLAME_ITEM_DESC,
                              LLAssetType::AT_LSL_TEXT, LLInventoryType::IT_LSL, NO_INV_SUBTYPE, perm.getMaskNextOwner(),
                              new LLBoostFuncInventoryCallback([made, answered](const LLUUID& id) { *made = id; *answered = true; }));
        for (S32 t = 0; t < 200 && !*answered; ++t) llcoro::suspendUntilTimeout(0.1f);
        if (made->isNull() || !gInventory.getItem(*made)) { error = "the torch flame script could not be made in your inventory."; return LLUUID::null; }
        auto done = std::make_shared<S32>(0);   // 1 stored, -1 refused
        auto why = std::make_shared<std::string>();
        LLResourceUploadInfo::ptr_t upload = std::make_shared<LLScriptAssetUpload>(*made, std::string(FLAME_LSL),
            [done, why](LLUUID, LLUUID, LLUUID, LLSD response)
            {
                if (response.has("errors") && response["errors"].size() > 0)
                {
                    *why = response["errors"][0].asString();
                    *done = -1;
                }
                else *done = 1;
            },
            [done, why](LLUUID, LLUUID, LLSD, std::string reason) { *why = reason; *done = -1; return true; });
        LLViewerAssetUpload::EnqueueInventoryUpload(url, upload);
        for (S32 t = 0; t < 300 && *done == 0; ++t) llcoro::suspendUntilTimeout(0.1f);
        if (*done != 1)
        {
            error = "the torch flame script could not be saved" + (why->empty() ? std::string(".") : ": " + *why);
            return LLUUID::null;
        }
        return *made;
    }

    /** Source: llviewerobject.cpp saveScript (RezScript with the inventory item; OpenSim Scene.Inventory.cs RezScript ->
     *  RezScriptFromAgentInventory copies it into that prim and starts it). */
    void addScript(LLViewerObject* o, const LLUUID& item_id)
    {
        LLViewerInventoryItem* item = gInventory.getItem(item_id);
        if (item) o->saveScript(item, true, false);
    }
}

// ═══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
std::string WolfCaveLights::styleKey(EStyle s)
{
    switch (s)
    {
    case STYLE_TORCHES:  return "torches";
    case STYLE_LAMPS:    return "lamps";
    case STYLE_LANTERNS: return "lanterns";
    case STYLE_CRYSTALS: return "crystals";
    default:             return "hidden";
    }
}

WolfCaveLights::EStyle WolfCaveLights::styleFrom(const std::string& key, EStyle fallback)
{
    for (S32 s = 0; s < STYLE_COUNT; ++s) if (styleKey((EStyle)s) == key) return (EStyle)s;
    return fallback;
}

std::string WolfCaveLights::styleNoun(EStyle s)
{
    switch (s)
    {
    case STYLE_TORCHES:  return "torches";
    case STYLE_LAMPS:    return "wall lamps";
    case STYLE_LANTERNS: return "lanterns";
    case STYLE_CRYSTALS: return "crystals";
    default:             return "hidden lights";
    }
}

S32 WolfCaveLights::partsPerLamp(EStyle s)
{
    return s == STYLE_TORCHES ? 2 : 1;
}

WolfCaveLights::EStyle WolfCaveLights::styleForWalls(bool natural)
{
    return natural ? STYLE_TORCHES : STYLE_LAMPS;
}

WolfCaveLights::EStyle WolfCaveLights::savedStyle(EStyle walls)
{
    return styleFrom(gSavedSettings.getString("WolfCaveLightStyle"), walls);
}

bool WolfCaveLights::savedWarm()
{
    return gSavedSettings.getString("WolfCaveLightTone") != "cool";
}

void WolfCaveLights::saveChoice(EStyle s, bool warm)
{
    gSavedSettings.setString("WolfCaveLightStyle", styleKey(s));
    gSavedSettings.setString("WolfCaveLightTone", warm ? "warm" : "cool");
}

std::vector<WolfCaveLights::Lamp> WolfCaveLights::plan(const std::vector<Route>& routes, const std::vector<Room>& rooms,
                                                       const field_f& field, EStyle style, bool keep_middle_clear)
{
    std::vector<Lamp> out;
    const bool hidden = style == STYLE_HIDDEN;
    const Fitting wall = fitting(style, false, true);
    const Fitting stand = fitting(style, true, true);
    // 1. the crossroads: a lamp in the corner the walk from the middle reaches first
    const std::vector<Cross> crosses = crossroads(routes);
    for (const Cross& c : crosses)
    {
        F32 best = 1e30f;
        LLVector2 best_dir;
        for (S32 sa = -1; sa <= 1; sa += 2)
            for (S32 sb = -1; sb <= 1; sb += 2)
            {
                LLVector2 d = c.mA * (F32)sa + c.mB * (F32)sb;
                if (d.lengthSquared() < 1e-6f) continue;
                d.normVec();
                const F32 w = wallDistance(field, c.mAt, d);
                if (w > 0.f && w < best) { best = w; best_dir = d; }
            }
        Lamp l;
        if (best < 1e29f && wallLamp(field, c.mAt, best_dir, wall, hidden, l)) out.push_back(l);
    }
    // 2. the rooms: one each, standing on the floor off the route's line (hidden: a light high in the middle)
    for (const Room& r : rooms)
    {
        LLVector2 dir(1.f, 0.f), at = r.mC;
        F32 near_d = 1e30f, half = 2.f;
        for (const Route& rt : routes)
        {
            LLVector2 a, u;
            F32 t = 0.f;
            const F32 d = nearestOn(rt, r.mC, a, u, t);
            if (d < near_d) { near_d = d; dir = u; at = a; half = rt.mHalfW; }
        }
        const LLVector2 perp(-dir.mV[1], dir.mV[0]);
        Lamp l;
        bool ok = false;
        if (hidden)
        {
            F32 s = 0.f, fl = 0.f, cl = 0.f;
            if (field(r.mC.mV[0], r.mC.mV[1], s, fl, cl) && cl > 1.f)
            {
                l.mPos.set(r.mC.mV[0], r.mC.mV[1], fl + llmin(cl * 0.6f, 4.f));
                ok = true;
            }
        }
        else
        {
            const F32 off = llmax(half + 1.2f, r.mR * 0.5f);
            for (S32 sgn = 1; sgn >= -1 && !ok; sgn -= 2) ok = floorLamp(field, r.mC + perp * (off * (F32)sgn), perp * (F32)-sgn, stand, l);
            if (!ok && !keep_middle_clear) ok = floorLamp(field, r.mC, perp, stand, l);
            // no room on the floor (a small room on a railway): a lamp on its wall instead
            for (S32 sgn = 1; sgn >= -1 && !ok; sgn -= 2) ok = wallLamp(field, r.mC, perp * (F32)sgn, wall, false, l);
        }
        if (ok)
        {
            if (l.mStanding || hidden) l.mRadius = llmin(RADIUS_MAX_M, r.mR * 1.2f + 4.f);
            out.push_back(l);
        }
    }
    // 3. the walls, every LAMP_SPACING_M, the sides in turn, never in a room or at a crossroads (they have their own)
    S32 side = 1;
    for (const Route& rt : routes)
    {
        const F32 len = routeLength(rt);
        if (rt.mPts.size() < 2 || len < 1.f) continue;
        std::vector<F32> at;
        if (len < 2.f * END_CLEAR_M) at.push_back(len * 0.5f);
        else for (F32 t = END_CLEAR_M; t <= len - END_CLEAR_M + 1e-3f; t += LAMP_SPACING_M) at.push_back(t);
        for (F32 t : at)
        {
            LLVector2 p, u;
            routeAt(rt, t, p, u);
            bool skip = false;
            for (const Room& r : rooms) if (dist_vec(p, r.mC) < r.mR + 1.f) skip = true;
            for (const Cross& c : crosses) if (dist_vec(p, c.mAt) < CROSS_CLEAR_M) skip = true;
            if (skip) continue;
            const LLVector2 left(-u.mV[1], u.mV[0]);
            Lamp l;
            if (wallLamp(field, p, left * (F32)side, wall, hidden, l) || wallLamp(field, p, left * (F32)-side, wall, hidden, l))
                out.push_back(l);
            side = -side;
        }
    }
    return out;
}

void WolfCaveLights::find(const LLSD& parts, std::vector<LLUUID>& seen_roots, LLSD& unseen)
{
    seen_roots.clear();
    unseen = LLSD::emptyArray();
    std::set<LLUUID> ours, roots;
    for (const LLSD& id : llsd::inArray(parts)) ours.insert(id.asUUID());
    LLViewerRegion* rgn = gAgent.getRegion();
    for (const LLSD& id : llsd::inArray(parts))
    {
        LLViewerObject* o = liveObject(id.asUUID());
        // Only the agent's own region: a local id means nothing in a neighbour's circuit.
        if (!o || !rgn || o->getRegion() != rgn) { unseen.append(id.asString()); continue; }
        LLViewerObject* root = o->getRootEdit();
        if (!root || root->isAvatar()) { unseen.append(id.asString()); continue; }
        // Never take away anything Light up did not make: a lamp linked into someone's own object (or with their prims linked
        // into it) is left alone and counted with the parts not in reach.
        bool all_ours = ours.count(root->getID()) > 0;
        for (LLViewerObject* child : root->getChildren()) if (child && !ours.count(child->getID())) all_ours = false;
        if (!all_ours) { unseen.append(id.asString()); continue; }
        if (roots.insert(root->getID()).second) seen_roots.push_back(root->getID());
    }
}

// Source: wolffloatertunnels.cpp derez / wolfcave.cpp removeOne (llselectmgr.cpp:5818-5834 packDeRezHeader, :5865-5869
// packObjectLocalID; llselectmgr.h:83-98 DRD_TRASH = 6, DRD_RETURN_TO_OWNER = 9).
S32 WolfCaveLights::takeAway(const std::vector<LLUUID>& roots)
{
    S32 n = 0;
    for (const LLUUID& id : roots)
    {
        LLViewerObject* o = liveObject(id);
        LLViewerRegion* rgn = o ? o->getRegion() : nullptr;
        if (!o || !rgn) continue;
        const bool own = o->permYouOwner();
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_DeRezObject);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->nextBlockFast(_PREHASH_AgentBlock);
        msg->addUUIDFast(_PREHASH_GroupID, gAgent.getGroupID());
        msg->addU8Fast(_PREHASH_Destination, (U8)(own ? 6 : 9));
        msg->addUUIDFast(_PREHASH_DestinationID, own ? gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH) : LLUUID::null);
        LLUUID tid;
        tid.generate();
        msg->addUUIDFast(_PREHASH_TransactionID, tid);
        msg->addU8Fast(_PREHASH_PacketCount, (U8)1);
        msg->addU8Fast(_PREHASH_PacketNumber, (U8)1);
        msg->nextBlockFast(_PREHASH_ObjectData);
        msg->addU32Fast(_PREHASH_ObjectLocalID, o->getLocalID());
        msg->sendReliable(rgn->getHost());
        ++n;
    }
    return n;
}

bool WolfCaveLights::place(const std::vector<Lamp>& lamps, EStyle style, bool warm, const std::string& cave_name, const LLUUID& region,
                           LLSD& parts, std::string& error, const progress_f& progress, const std::function<bool()>& stop)
{
    if (!parts.isArray()) parts = LLSD::emptyArray();
    LLUUID script;
    if (style == STYLE_TORCHES)
    {
        progress("Getting the torch flame ready...", 0.f);
        script = flameScript(error);
        if (script.isNull()) return false;
    }
    const std::string noun = styleNoun(style);
    const std::string desc = "Light up: " + cave_name;
    std::string one = noun;
    if (one.size() > 1 && one.back() == 's') one.pop_back();
    if (one == "torche") one = "torch";
    const std::string lamp_name = "Wolf cave " + one;
    for (size_t i = 0; i < lamps.size(); ++i)
    {
        if (stop && stop())
        {
            error = llformat("stopped as you asked: %d of %d %s placed.", (S32)i, (S32)lamps.size(), noun.c_str());
            return false;
        }
        if (!inAgentRegion(region)) { error = "you left the region."; return false; }
        progress(llformat("Placing %s (%d of %d)...", noun.c_str(), (S32)i + 1, (S32)lamps.size()), (F32)i / (F32)llmax((size_t)1, lamps.size()));
        const Lamp& l = lamps[i];
        const Fitting f = fitting(style, l.mStanding, warm);
        const LLQuaternion rot(l.mYaw, LLVector3::z_axis);
        const LLUUID root_id = makePrim(f, l.mPos, rot, region, error);
        if (root_id.isNull()) return false;
        parts.append(root_id.asString());
        LLViewerObject* root = liveObject(root_id);
        if (!root) { error = "a lamp went away while it was being made."; return false; }
        dressFitting(root, f);
        nameObject(root, lamp_name, desc);
        if (f.mFlame)
        {
            Fitting flame;
            flame.mShape = SHAPE_CUBE;
            flame.mScale = FLAME_SCALE;
            flame.mMaterial = LL_MCODE_GLASS;
            flame.mIntensity = f.mIntensity;
            flame.mFalloff = f.mFalloff;
            const LLVector3 bowl = (f.mBowl - LLVector3(0.f, 0.f, FLAME_DROP_M)) * rot;
            const LLUUID flame_id = makePrim(flame, l.mPos + bowl, rot, region, error);
            if (flame_id.isNull()) return false;
            parts.append(flame_id.asString());
            LLViewerObject* fl = liveObject(flame_id);
            if (!fl) { error = "a torch's flame went away while it was being made."; return false; }
            dressFlame(fl);
            lightOn(fl, f, warm, l.mRadius);
            nameObject(fl, lamp_name + " flame", desc);
            if (!link(root_id, flame_id, region, error)) return false;
            if (LLViewerObject* lit = liveObject(flame_id)) addScript(lit, script);
        }
        else
        {
            lightOn(root, f, warm, l.mRadius);
        }
        // phantom: walked through, never in the way (OpenSim applies it to the whole linkset)
        if (LLViewerObject* r = liveObject(root_id)) r->setFlags(FLAGS_PHANTOM, true);
    }
    progress(llformat("%d %s placed.", (S32)lamps.size(), noun.c_str()), 1.f);
    return true;
}

S32 WolfCaveLights::lampCount(const LLSD& lights)
{
    if (!lights.isMap() || !lights["objects"].isArray()) return 0;
    const S32 per = partsPerLamp(styleFrom(lights["style"].asString(), STYLE_HIDDEN));
    return ((S32)lights["objects"].size() + per - 1) / per;
}

std::string WolfCaveLights::summary(const LLSD& lights)
{
    const S32 n = lampCount(lights);
    if (n <= 0) return std::string();
    return llformat("%d ", n) + styleNoun(styleFrom(lights["style"].asString(), STYLE_HIDDEN));
}

namespace
{
    /** The old lamps' line in a question: what is taken away, and how many parts are out of view. */
    std::string oldLampsLine(const LLSD& old_lights, S32& unseen_count)
    {
        unseen_count = 0;
        if (!old_lights.isMap() || old_lights["objects"].size() == 0) return std::string();
        std::vector<LLUUID> roots;
        LLSD unseen;
        WolfCaveLights::find(old_lights["objects"], roots, unseen);
        unseen_count = (S32)unseen.size();
        std::string line = llformat("%d lamp object%s placed before %s taken away (into your Trash if this region keeps deleted objects). ",
                                    (S32)roots.size(), roots.size() == 1 ? "" : "s", roots.size() == 1 ? "is" : "are");
        if (unseen_count > 0)
            line += llformat("%d part%s placed before %s not in view from here: deleted by hand already, or too far away. Keep them "
                             "listed, so Remove lights takes them when you are nearer, or forget them? (A lamp linked into another object counts here too: "
                             "it is never taken away with that object.) ",
                             unseen_count, unseen_count == 1 ? "" : "s", unseen_count == 1 ? "is" : "are");
        return line;
    }

    void ask(const std::string& msg, S32 unseen, const std::function<void(bool keep_unseen)>& go)
    {
        if (unseen > 0)
        {
            LLNotificationsUtil::add("WolfCaveLightsUnseen", LLSD().with("MESSAGE", msg), LLSD(), [go](const LLSD& n, const LLSD& r)
            {
                const S32 opt = LLNotificationsUtil::getSelectedOption(n, r);
                if (opt == 0) go(true);        // Keep them listed
                else if (opt == 1) go(false);  // Forget them
            });
            return;
        }
        LLNotificationsUtil::add("GenericAlertYesCancel", LLSD().with("MESSAGE", msg), LLSD(), [go](const LLSD& n, const LLSD& r)
        {
            if (LLNotificationsUtil::getSelectedOption(n, r) == 0) go(false);
        });
    }
}

void WolfCaveLights::confirmLightUp(const std::string& cave_name, EStyle style, S32 lamps, const LLSD& old_lights,
                                    const std::function<void(bool keep_unseen)>& go)
{
    S32 unseen = 0;
    const std::string old = oldLampsLine(old_lights, unseen);
    const S32 per = partsPerLamp(style);
    std::string msg = llformat("Light up \"%s\" with %d %s? That is %d prim%s (land impact %d), owned by you, placed one by one; you can "
                               "move, turn or delete any of them afterwards. ", cave_name.c_str(), lamps, styleNoun(style).c_str(),
                               lamps * per, lamps * per == 1 ? "" : "s", lamps * per);
    if (style == STYLE_TORCHES) msg += "Each torch's flame is a particle script that runs once and then stops. ";
    msg += old;
    ask(msg, unseen, go);
}

void WolfCaveLights::confirmRemove(const std::string& cave_name, const LLSD& old_lights, const std::function<void(bool keep_unseen)>& go)
{
    S32 unseen = 0;
    const std::string old = oldLampsLine(old_lights, unseen);
    if (old.empty())
    {
        LLNotificationsUtil::add("GenericAlertOK", LLSD().with("MESSAGE", "\"" + cave_name + "\" has no lights to remove."));
        return;
    }
    ask("Remove the lights of \"" + cave_name + "\"? " + old, unseen, go);
}

LLSD WolfCaveLights::takeAwayFor(const LLSD& lights)
{
    if (!lights.isMap() || lights["objects"].size() == 0) return LLSD::emptyArray();
    std::vector<LLUUID> roots;
    LLSD unseen;
    find(lights["objects"], roots, unseen);
    takeAway(roots);
    return unseen;
}

bool WolfCaveLights::run(const LLSD& old_lights, bool keep_unseen, const std::vector<Lamp>* lamps, EStyle style, bool warm,
                         const std::string& cave_name, const LLUUID& region, const save_f& save, const progress_f& progress,
                         const std::function<bool()>& stop, std::string& done, std::string& error)
{
    // 1. the lamps placed before: the ones in view go; the rest stay listed or are forgotten, as the person chose
    LLSD kept = LLSD::emptyArray();
    S32 taken = 0, forgotten = 0;
    if (old_lights.isMap() && old_lights["objects"].size() > 0)
    {
        progress("Taking the lamps placed before away...", 0.f);
        std::vector<LLUUID> roots;
        LLSD unseen;
        find(old_lights["objects"], roots, unseen);
        taken = takeAway(roots);
        if (keep_unseen) kept = unseen;
        else forgotten = (S32)unseen.size();
    }
    const EStyle old_style = styleFrom(old_lights["style"].asString(), style);
    auto record = [&](const LLSD& objects, EStyle st, bool w, std::string& err)
    {
        LLSD l;   // undefined = no lights left
        if (objects.size() > 0)
        {
            l["style"] = styleKey(st);
            l["tone"] = w ? "warm" : "cool";
            l["objects"] = objects;
            l["at"] = (S32)LLDate::now().secondsSinceEpoch();
        }
        return save(l, err);
    };
    if (!lamps)
    {
        if (!record(kept, old_style, old_lights["tone"].asString() != "cool", error)) return false;
        done = llformat("%d lamp object%s taken away.", taken, taken == 1 ? "" : "s");
        if (kept.size() > 0) done += llformat(" %d part%s not in view stay listed: press Remove lights again when you are nearer.", (S32)kept.size(), kept.size() == 1 ? "" : "s");
        if (forgotten > 0) done += llformat(" %d part%s not in view %s forgotten.", forgotten, forgotten == 1 ? "" : "s", forgotten == 1 ? "was" : "were");
        return true;
    }
    // 2. the new lamps, a few at a time, the record saved after each few (so a stop, a quit or a refusal part way still lists
    //    every lamp made, for Remove lights)
    LLSD objects = kept;
    if (!record(objects, style, warm, error)) return false;
    const size_t CHUNK = 8;
    for (size_t i = 0; i < lamps->size(); i += CHUNK)
    {
        const std::vector<Lamp> some(lamps->begin() + i, lamps->begin() + llmin(lamps->size(), i + CHUNK));
        std::string err;
        const size_t base = i;
        const size_t total = lamps->size();
        const bool ok = place(some, style, warm, cave_name, region, objects, err,
            [&](const std::string&, F32 f)
            {
                const S32 at = (S32)base + llmin((S32)some.size(), (S32)ll_round(f * (F32)some.size())) + 1;
                progress(llformat("Placing %s (%d of %d)...", styleNoun(style).c_str(), llmin(at, (S32)total), (S32)total),
                         ((F32)base + f * (F32)some.size()) / (F32)llmax((size_t)1, total));
            }, stop);
        std::string save_err;
        const bool saved = record(objects, style, warm, save_err);
        if (!ok)
        {
            error = err + (saved ? " The lamps already placed are listed: Remove lights takes them away."
                                 : " (The record of the lamps placed could not be saved either: " + save_err + ")");
            return false;
        }
        if (!saved) { error = "the lamps are placed, but the record of them could not be saved: " + save_err; return false; }
    }
    done = llformat("\"%s\" is lit: %d %s placed.", cave_name.c_str(), (S32)lamps->size(), styleNoun(style).c_str());
    if (taken > 0) done += llformat(" %d lamp object%s placed before %s taken away.", taken, taken == 1 ? "" : "s", taken == 1 ? "was" : "were");
    if (kept.size() > 0) done += llformat(" %d old part%s not in view stay listed.", (S32)kept.size(), kept.size() == 1 ? "" : "s");
    return true;
}
