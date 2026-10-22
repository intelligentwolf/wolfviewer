/**
 * @file wolffloatertunnels.cpp
 * @brief WolfViewer: the Tunnel tool. See wolffloatertunnels.h.
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

#include "wolffloatertunnels.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <boost/json.hpp>

#include "fscommon.h"
#include "llagent.h"
#include "llcombobox.h"
#include "lldate.h"
#include "lleventcoro.h"
#include "llinventorymodel.h"
#include "lllineeditor.h"
#include "llpanel.h"
#include "llradiogroup.h"
#include "llrand.h"
#include "llsdjson.h"
#include "llsdutil.h"
#include "llsliderctrl.h"
#include "llsurfacepatch.h"
#include "lltooldraganddrop.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "llviewerobjectlist.h"
#include "message.h"
#include "wolfcave.h"
#include "wolfterrainpaint.h"
#include "llframetimer.h"
#include "llglslshader.h"
#include "llsurface.h"
#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "llfloaterreg.h"
#include "llfontgl.h"
#include "llnotificationsutil.h"
#include "llrender.h"
#include "llscrolllistctrl.h"
#include "llspinctrl.h"
#include "lltextbox.h"
#include "lltoolmgr.h"
#include "lltoolpie.h"
#include "lluicolortable.h"
#include "llviewercamera.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"
#include "wolfgamehud.h"
#include "wolfgrid.h"

namespace
{
    // Source: WolfSim WolfTunnelsModule.cs CAP (the region's capability name; llviewerregion.cpp asks for it).
    const char* CAP = "WolfTunnels";
    // Source: UNDERGROUND_PATCH.md "z0 must be a metre or two BELOW the floor": the box's floor sits this far below the ground
    // where the drag began.
    const F32 FLOOR_BELOW = 1.f;
    // Source: WolfTunnelsModule.cs MIN_SIZE / MAX_SIZE (the region refuses others; the tool keeps to them first).
    const F32 MIN_SIZE = 0.5f, MAX_SIZE = 512.f;
    // Outlines further than this from the camera are not drawn.
    const F32 CULL_M = 512.f;

    std::vector<WolfTunnels::Box> s_boxes;
    LLUUID s_boxesRegion;
    S32 s_selected = 0;

    bool finite3(const LLVector3& v) { return std::isfinite(v.mV[VX]) && std::isfinite(v.mV[VY]) && std::isfinite(v.mV[VZ]); }
    F32 wrap360(F32 a) { a = fmodf(a, 360.f); return a < 0.f ? a + 360.f : a; }
    std::string m1(F32 v) { return llformat("%.1f", v); }

    LLSD boxToLLSD(const WolfTunnels::Box& b)
    {
        LLSD t;
        if (b.mId) t["id"] = b.mId;
        t["c"] = LLSD::emptyArray();
        t["s"] = LLSD::emptyArray();
        for (S32 i = 0; i < 3; ++i) { t["c"].append((F64)b.mCentre.mV[i]); t["s"].append((F64)b.mSize.mV[i]); }
        t["yaw"] = (F64)b.mYawDeg;
        return t;
    }

    /** The 12 edges of a box as line segment ends, agent metres. */
    void boxEdges(LLViewerRegion* rgn, const WolfTunnels::Box& b, std::vector<LLVector3>& out)
    {
        const F32 a = b.mYawDeg * DEG_TO_RAD, c = cosf(a), s = sinf(a);
        const F32 hx = b.mSize.mV[VX] * 0.5f, hy = b.mSize.mV[VY] * 0.5f, hz = b.mSize.mV[VZ] * 0.5f;
        LLVector3 p[8];
        for (S32 i = 0; i < 8; ++i)
        {
            const F32 lx = (i & 1) ? hx : -hx, ly = (i & 2) ? hy : -hy, lz = (i & 4) ? hz : -hz;
            p[i] = rgn->getPosAgentFromRegion(LLVector3(b.mCentre.mV[VX] + c * lx - s * ly, b.mCentre.mV[VY] + s * lx + c * ly, b.mCentre.mV[VZ] + lz));
        }
        static const S32 E[12][2] = { {0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7} };
        for (const auto& e : E) { out.push_back(p[e[0]]); out.push_back(p[e[1]]); }
    }
}

const std::vector<WolfTunnels::Box>& WolfTunnels::boxes() { return s_boxes; }
LLUUID WolfTunnels::boxesRegion() { return s_boxesRegion; }
S32 WolfTunnels::selectedId() { return s_selected; }

// =====================================================================================================================
// [WOLF TUNNELS 2026-10-21] the store outside the window: read on arrival, holes in the terrain, the camera underground
// =====================================================================================================================

namespace
{
    // Source: lldrawpoolterrain.cpp / terrainF.glsl + pbrterrainF.glsl WOLF_TUNNELS_MAX (the uniform arrays' length).
    const S32 TERRAIN_MAX = 32;
    // How often the "does this box reach the ground" flags are worked out again, seconds (only on a region that has
    // boxes; the terrain streams in after arrival - llsurface.cpp reportTerrainDiagnostics shows ~20 s - and can be edited).
    const F64 SURFACE_EVERY = 2.0;
    // Margin round a box when testing the ground against it, metres.
    const F32 SURFACE_MARGIN = 0.5f;
    // The camera is held this far inside the boxes, metres (llagentcamera.cpp CAMERA_COLLIDE_EPSILON is 0.1).
    const F32 CAMERA_INSET = 0.25f;
    // Steps along the head-to-camera line when finding where it leaves the boxes, metres.
    const F32 CAMERA_STEP = 0.2f;

    std::vector<bool> s_touches;       // per s_boxes entry: the ground passes through it (so the terrain is cut there)
    F64 s_touchesAt = -1.0;
    LLUUID s_touchesRegion;
    boost::signals2::connection s_capsConn;
    LLUUID s_fetching;

    /** Is p (region metres) inside b? */
    bool inBox(const WolfTunnels::Box& b, const LLVector3& p, F32 grow = 0.f)
    {
        const F32 a = b.mYawDeg * DEG_TO_RAD, c = cosf(a), s = sinf(a);
        const F32 dx = p.mV[VX] - b.mCentre.mV[VX], dy = p.mV[VY] - b.mCentre.mV[VY];
        // the inverse of boxEdges' turn: local = R(-yaw) * (p - c)
        const F32 lx = c * dx + s * dy, ly = -s * dx + c * dy, lz = p.mV[VZ] - b.mCentre.mV[VZ];
        return fabsf(lx) <= b.mSize.mV[VX] * 0.5f + grow && fabsf(ly) <= b.mSize.mV[VY] * 0.5f + grow
            && fabsf(lz) <= b.mSize.mV[VZ] * 0.5f + grow;
    }

    /** Which boxes the ground passes through: sampled on a grid over each footprint (LLSurface::resolveHeightRegion). */
    void refreshTouches(LLViewerRegion* rgn)
    {
        s_touches.assign(s_boxes.size(), false);
        s_touchesAt = LLFrameTimer::getElapsedSeconds();
        s_touchesRegion = rgn->getRegionID();
        const LLSurface& land = rgn->getLand();
        for (size_t i = 0; i < s_boxes.size(); ++i)
        {
            const WolfTunnels::Box& b = s_boxes[i];
            const F32 a = b.mYawDeg * DEG_TO_RAD, c = cosf(a), s = sinf(a);
            const F32 hx = b.mSize.mV[VX] * 0.5f + SURFACE_MARGIN, hy = b.mSize.mV[VY] * 0.5f + SURFACE_MARGIN;
            const F32 z0 = b.mCentre.mV[VZ] - b.mSize.mV[VZ] * 0.5f - SURFACE_MARGIN;
            const F32 z1 = b.mCentre.mV[VZ] + b.mSize.mV[VZ] * 0.5f + SURFACE_MARGIN;
            // up to 9 x 9 samples, at least every 2 m along a short side
            const S32 nx = llclamp((S32)ceilf(hx), 2, 8), ny = llclamp((S32)ceilf(hy), 2, 8);
            for (S32 ix = 0; ix <= nx && !s_touches[i]; ++ix)
            {
                for (S32 iy = 0; iy <= ny; ++iy)
                {
                    const F32 lx = -hx + 2.f * hx * ix / nx, ly = -hy + 2.f * hy * iy / ny;
                    const F32 x = b.mCentre.mV[VX] + c * lx - s * ly, y = b.mCentre.mV[VY] + s * lx + c * ly;
                    const F32 h = land.resolveHeightRegion(x, y);
                    if (h >= z0 && h <= z1) { s_touches[i] = true; break; }
                }
            }
        }
    }

    void fetchCoro(std::string url, LLUUID region)
    {
        auto adapter = std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfTunnels", LLCore::HttpRequest::DEFAULT_POLICY_ID);
        LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
        LLSD result = adapter->getAndSuspend(request, url);
        LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
        if (s_fetching == region) s_fetching.setNull();
        LLViewerRegion* rgn = gAgent.getRegion();
        if (!rgn || rgn->getRegionID() != region) return;   // moved on meanwhile: that region's turn has its own fetch
        if (!status)
        {
            LL_WARNS("WolfTunnels") << "reading the tunnels of " << rgn->getName() << " failed: " << status.toString() << LL_ENDL;
            return;
        }
        WolfTunnels::setFromAnswer(result, region);
        LL_INFOS("WolfTunnels") << rgn->getName() << ": " << s_boxes.size() << " tunnel box(es)" << LL_ENDL;
    }

    void fetchFor(LLViewerRegion* rgn)
    {
        if (!rgn || rgn != gAgent.getRegion()) return;
        const std::string url = rgn->getCapability(CAP);
        if (url.empty()) return;   // no WolfTunnelsModule there (any other grid or sim): nothing to read, nothing to cut
        const LLUUID region = rgn->getRegionID();
        if (s_fetching == region) return;
        s_fetching = region;
        LLCoros::instance().launch("WolfTunnelsArrive", [url, region]() { fetchCoro(url, region); });
    }

    void onRegionChanged()
    {
        s_capsConn.disconnect();
        LLViewerRegion* rgn = gAgent.getRegion();
        // the boxes belong to the region they were read from: never drawn or used on another
        if (!rgn || rgn->getRegionID() != s_boxesRegion)
        {
            s_boxes.clear();
            s_touches.clear();
            s_boxesRegion.setNull();
            s_selected = 0;
        }
        if (!rgn) return;
        if (rgn->capabilitiesReceived()) fetchFor(rgn);
        else s_capsConn = rgn->setCapabilitiesReceivedCallback([](const LLUUID&, LLViewerRegion* r) { fetchFor(r); });
    }
}

void WolfTunnels::init()
{
    static bool s_done = false;
    if (s_done) return;
    s_done = true;
    // Source: llagent.h:300-318 addRegionChangedCallback (the documented pattern with setCapabilitiesReceivedCallback).
    gAgent.addRegionChangedCallback(&onRegionChanged);
}

void WolfTunnels::setFromAnswer(const LLSD& answer, const LLUUID& region)
{
    s_boxes.clear();
    s_boxesRegion = region;
    const LLSD& list = answer["tunnels"];
    for (LLSD::array_const_iterator it = list.beginArray(); it != list.endArray(); ++it)
    {
        const LLSD& t = *it;
        if (!t.isMap() || t["c"].size() != 3 || t["s"].size() != 3) continue;
        WolfTunnels::Box b;
        b.mId = t["id"].asInteger();
        for (S32 i = 0; i < 3; ++i) { b.mCentre.mV[i] = (F32)t["c"][i].asReal(); b.mSize.mV[i] = (F32)t["s"][i].asReal(); }
        b.mYawDeg = (F32)t["yaw"].asReal();
        if (finite3(b.mCentre) && finite3(b.mSize) && std::isfinite(b.mYawDeg)) s_boxes.push_back(b);
    }
    s_touches.clear();
    s_touchesAt = -1.0;   // worked out again at the next terrain draw
}

bool WolfTunnels::contains(const LLVector3& region_pos)
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (s_boxes.empty() || !rgn || rgn->getRegionID() != s_boxesRegion) return false;
    for (const Box& b : s_boxes)
        if (inBox(b, region_pos)) return true;
    return false;
}

bool WolfTunnels::keepCameraInside(const LLVector3& head_agent, LLVector3& camera_agent)
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (s_boxes.empty() || !rgn || rgn->getRegionID() != s_boxesRegion) return false;
    const LLVector3 head = rgn->getPosRegionFromAgent(head_agent);
    const LLVector3 dir = camera_agent - head_agent;
    const F32 len = dir.magVec();
    // only the boxes the line can reach (a tunnel is a chain of boxes; Moonfold has 356)
    std::vector<const Box*> near;
    bool head_in = false;
    for (const Box& b : s_boxes)
    {
        if (dist_vec(b.mCentre, head) > len + b.mSize.magVec() * 0.5f + 1.f) continue;
        near.push_back(&b);
        if (inBox(b, head)) head_in = true;
    }
    if (!head_in) return false;
    if (len < 0.001f) return true;
    // a point the camera may be at: in a box, or out in the open air above the ground (at a tunnel's mouth the camera may
    // stay outside, behind the avatar walking in; only rock is out of bounds)
    const LLSurface& land = rgn->getLand();
    auto inside = [&](const LLVector3& p)
    {
        for (const Box* b : near) if (inBox(*b, p, -CAMERA_INSET)) return true;
        return p.mV[VZ] > land.resolveHeightRegion(p.mV[VX], p.mV[VY]) + CAMERA_INSET;
    };
    // walk out from the head; the camera stops at the last point still allowed (the boxes overlap along a tunnel)
    F32 keep = 0.f;
    const S32 steps = llmax(1, (S32)ceilf(len / CAMERA_STEP));
    for (S32 i = 1; i <= steps; ++i)
    {
        const F32 t = (F32)i / steps;
        if (!inside(head + dir * t)) break;
        keep = t;
    }
    camera_agent = head_agent + dir * keep;
    return true;
}

void WolfTunnels::bindTerrain(LLGLSLShader* shader, LLViewerRegion* regionp)
{
    static LLStaticHashedString s_count("wolf_tunnel_count");
    static LLStaticHashedString s_cy("wolf_tunnel_cy");
    static LLStaticHashedString s_hs("wolf_tunnel_hs");
    if (!shader || !regionp) return;
    // LLGLSLShader caches a single value per location (llglslshader.cpp uniform1i), so 0 again costs no GL call
    if (s_boxes.empty() || regionp->getRegionID() != s_boxesRegion)
    {
        shader->uniform1i(s_count, 0);
        return;
    }
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (s_touches.size() != s_boxes.size() || s_touchesRegion != regionp->getRegionID() || now - s_touchesAt > SURFACE_EVERY)
        refreshTouches(regionp);
    // the boxes the ground passes through, nearest the camera first
    const LLVector3 cam = regionp->getPosRegionFromAgent(LLViewerCamera::getInstance()->getOrigin());
    std::vector<std::pair<F32, size_t>> pick;
    for (size_t i = 0; i < s_boxes.size(); ++i)
        if (s_touches[i]) pick.emplace_back(dist_vec_squared(s_boxes[i].mCentre, cam), i);
    const S32 n = llmin((S32)pick.size(), TERRAIN_MAX);
    std::partial_sort(pick.begin(), pick.begin() + n, pick.end());
    F32 cy[TERRAIN_MAX * 4] = {}, hs[TERRAIN_MAX * 4] = {};
    for (S32 k = 0; k < n; ++k)
    {
        const Box& b = s_boxes[pick[k].second];
        cy[k * 4 + 0] = b.mCentre.mV[VX]; cy[k * 4 + 1] = b.mCentre.mV[VY]; cy[k * 4 + 2] = b.mCentre.mV[VZ];
        cy[k * 4 + 3] = b.mYawDeg * DEG_TO_RAD;
        hs[k * 4 + 0] = b.mSize.mV[VX] * 0.5f; hs[k * 4 + 1] = b.mSize.mV[VY] * 0.5f; hs[k * 4 + 2] = b.mSize.mV[VZ] * 0.5f;
    }
    shader->uniform1i(s_count, n);
    if (n > 0)
    {
        shader->uniform4fv(s_cy, n, cy);
        shader->uniform4fv(s_hs, n, hs);
    }
}

// =====================================================================================================================
// [WOLF TUNNELS 2026-10-22] the drawn tunnel: its limits, its shape (for the preview), the records and the jobs
// =====================================================================================================================

namespace
{
    // Source: firestorm/wolfstorm/tools/TUNNEL_DESIGN_V1.md "Stored design" and rust_proxy/src/wolftunnel.rs (validate_design,
    // TUNNEL_V, W_MIN..., the generator constants below): the same numbers, so the window refuses what the generator would.
    const S32 TUNNEL_V = 1;
    const F32 W_MIN = 2.f, W_MAX = 20.f, H_MIN = 2.5f, H_MAX = 15.f;
    const F32 SEG_MIN_M = 0.5f;
    const F32 Z_MIN = -90.f, Z_MAX = 4000.f;
    const F32 GRADE_MAX = 1.f;
    const F32 CAVERN_R_MIN = 3.f, CAVERN_R_MAX = 30.f, CAVERN_H_MIN = 3.f, CAVERN_H_MAX = 25.f;
    const S32 SEED_MAX = 2147483647;
    // wolftunnel.rs generator settings (the preview is drawn with the same formulas).
    const F32 WALL_FRAC_SMOOTH = 0.6f, WALL_FRAC_ROUGH = 0.25f;
    const F32 CAVERN_WALL_FRAC_SMOOTH = 0.45f, CAVERN_WALL_FRAC_ROUGH = 0.2f;
    const F32 WALL_FALLOFF = 6.f;
    const F32 ROUGH_W = 0.15f, ROUGH_W_CELL_M = 5.f, ROUGH_H = 0.12f, ROUGH_H_CELL_M = 7.f;
    const F32 ROUGH_WALL_M = 0.22f, ROUGH_WALL_CELL_M = 1.5f;
    const F32 ROUGH_BUMP = 0.15f, ROUGH_BUMP_CELL_M = 1.2f;
    const F32 ROUGH_FLOOR_M = 0.08f, ROUGH_FLOOR_CELL_M = 2.f, ROUGH_EDGE_RISE_M = 0.2f, ROUGH_EDGE_M = 1.f;
    const F32 ROUGH_CAVERN = 0.15f, ROUGH_CAVERN_MAX_M = 2.f;
    const F32 CLEAR_MIN_M = 0.05f;
    const F32 MOUTH_M = 8.f;
    const F32 WALK_GRADE_WARN = 0.25f, RAIL_GRADE_WARN = 0.04f;
    // The window's own: the kinds' sizes (Paul's brief: walking 4 m, wide 6 m, rail 6 x 6), the warnings' thin cover (the
    // region's box reaches wolftunnel.rs VOL_ABOVE_M = 0.3 m over the roof, so less than this may open the ground), a new
    // cavern's height from its radius, how many places a warning names, and the preview's most cross-sections.
    const F32 COVER_THIN_M = 0.5f;
    const S32 WARN_PLACES = 3;
    const S32 PREVIEW_RINGS_MAX = 1500;
    const S32 PREVIEW_ACROSS = 12;
    // Source: php/tunnels.php (TUNNEL_DESIGN_V1.md "Record") and rust_proxy POST /tunnel_mesh (wolftunnel.rs handle()).
    const char* TUNNELS_URL = "https://wolfstorm.app/php/tunnels.php";
    std::string tunnelMeshUrl() { return std::string(WolfGrid::MESH_API_BASE) + "/tunnel_mesh"; }
    // Source: WolfTunnelsModule.cs MIN_POST_MS = 100 (one change per agent per 0.1 s): the jobs wait a little longer.
    const F32 CAP_POST_GAP_S = 0.15f;

    F32 r2(F32 v) { return roundf(v * 100.f) / 100.f; }

    // Source: wolftunnel.rs uses wolfcave.rs value_noise and arch; the same arithmetic here (u32 wrapping multiplies).
    F32 valueNoise(F32 x, F32 y, F32 cell, U32 seed)
    {
        auto hash = [](S32 ix, S32 iy, U32 sd) -> F32
        {
            U32 h = ((U32)ix * 0x8da6b343u) ^ ((U32)iy * 0xd8163841u) ^ (sd * 0xcb1ab31fu);
            h ^= h >> 13;
            h *= 0x5bd1e995u;
            h ^= h >> 15;
            return (F32)(h & 0xffff) / 32767.5f - 1.f;
        };
        const F32 fx = x / cell, fy = y / cell;
        const S32 ix = (S32)floorf(fx), iy = (S32)floorf(fy);
        const F32 tx = fx - (F32)ix, ty = fy - (F32)iy;
        const F32 sx = tx * tx * (3.f - 2.f * tx), sy = ty * ty * (3.f - 2.f * ty);
        const F32 a = hash(ix, iy, seed), b = hash(ix + 1, iy, seed), c = hash(ix, iy + 1, seed), d = hash(ix + 1, iy + 1, seed);
        const F32 top = a + (b - a) * sx, bot = c + (d - c) * sx;
        return top + (bot - top) * sy;
    }

    F32 arch(F32 q)
    {
        q = llmin(q, 1.f);
        return sqrtf(llmax(0.f, 1.f - (1.f - q) * (1.f - q)));
    }

    /** Source: wolftunnel.rs Route. */
    struct Route
    {
        std::vector<LLVector2> mPts;
        std::vector<F32> mCum;
        F32 mLen = 0.f;
        explicit Route(const std::vector<F32>& p)
        {
            for (size_t k = 0; k + 1 < p.size(); k += 2) mPts.emplace_back(p[k], p[k + 1]);
            mCum.assign(mPts.size(), 0.f);
            for (size_t k = 1; k < mPts.size(); ++k) mCum[k] = mCum[k - 1] + dist_vec(mPts[k], mPts[k - 1]);
            mLen = mCum.empty() ? 0.f : mCum.back();
        }
        /** (distance to the route, arc length of the nearest route point) */
        std::pair<F32, F32> nearest(F32 x, F32 y) const
        {
            std::pair<F32, F32> best(1e30f, 0.f);
            for (size_t k = 1; k < mPts.size(); ++k)
            {
                const LLVector2 a = mPts[k - 1], v = mPts[k] - mPts[k - 1];
                const F32 l2 = v.lengthSquared();
                const F32 u = l2 > 1e-9f ? llclamp(((x - a.mV[0]) * v.mV[0] + (y - a.mV[1]) * v.mV[1]) / l2, 0.f, 1.f) : 0.f;
                const F32 d = sqrtf(powf(x - (a.mV[0] + u * v.mV[0]), 2.f) + powf(y - (a.mV[1] + u * v.mV[1]), 2.f));
                if (d < best.first) best = std::make_pair(d, mCum[k - 1] + u * sqrtf(l2));
            }
            return best;
        }
        LLVector2 dir(size_t k) const
        {
            LLVector2 v = mPts[k + 1] - mPts[k];
            const F32 l = llmax(1e-6f, v.length());
            return v / l;
        }
        /** The point at arc length t and the direction of the piece it lies on. */
        void at(F32 t, LLVector2& p, LLVector2& u) const
        {
            t = llclamp(t, 0.f, mLen);
            size_t k = 1;
            while (k + 1 < mPts.size() && mCum[k] < t) ++k;
            const F32 l = llmax(1e-6f, mCum[k] - mCum[k - 1]);
            const F32 f = llclamp((t - mCum[k - 1]) / l, 0.f, 1.f);
            p = mPts[k - 1] + (mPts[k] - mPts[k - 1]) * f;
            u = dir(k - 1);
        }
    };

    struct Field { F32 s = 0.f, e = 0.f, f = 0.f, v = 0.f, t = 0.f; };

    /** Source: wolftunnel.rs Air (eval, reach, top, floor_t) - the same formulas, for the preview. */
    struct Air
    {
        const WolfTunnels::Design& d;
        Route route;
        bool rough;
        U32 seed;
        F32 hw;
        explicit Air(const WolfTunnels::Design& des) : d(des), route(des.mP), rough(des.mStyle == "rough"), seed((U32)des.mSeed), hw(des.mW * 0.5f) {}
        F32 ro() const { return rough ? 1.f : 0.f; }
        F32 floorT(F32 t) const { return d.mZ0 + (d.mZ1 - d.mZ0) * (t / llmax(1e-6f, route.mLen)); }
        F32 reach() const { return hw * (1.f + ro() * ROUGH_W) + ro() * ROUGH_WALL_M; }
        F32 top() const { return d.mH * (1.f + ro() * ROUGH_H); }
        F32 bump(F32 x, F32 y) const { const F32 b = llmax(0.f, valueNoise(x, y, ROUGH_BUMP_CELL_M, seed ^ 0x6b756d70u)); return b * b; }
        F32 roughVault(F32 s, F32 r, F32 h, F32 wall_frac, F32 x, F32 y) const
        {
            const F32 hw2 = wall_frac * h;
            const F32 base = s >= 0.f ? hw2 + (h - hw2) * arch(s / r) : hw2 + s * WALL_FALLOFF;
            return base * (1.f - ROUGH_BUMP * bump(x, y));
        }
        Field eval(F32 x, F32 y) const
        {
            const std::pair<F32, F32> nt = route.nearest(x, y);
            const F32 dist = nt.first, t = nt.second, h = d.mH;
            F32 s, v;
            if (rough)
            {
                const F32 r = hw * (1.f + ROUGH_W * valueNoise(t, 0.f, ROUGH_W_CELL_M, seed ^ 0x77696474u));
                const F32 ht = h * (1.f + ROUGH_H * valueNoise(t, 3.7f, ROUGH_H_CELL_M, seed ^ 0x68656967u));
                s = r - dist + ROUGH_WALL_M * valueNoise(x, y, ROUGH_WALL_CELL_M, seed ^ 0x77616c6cu);
                v = roughVault(s, r, ht, WALL_FRAC_ROUGH, x, y);
            }
            else
            {
                const F32 r = hw, hw2 = WALL_FRAC_SMOOTH * h;
                v = dist <= r ? hw2 + (h - hw2) * sqrtf(llmax(0.f, 1.f - (dist / r) * (dist / r))) : hw2 - (dist - r) * WALL_FALLOFF;
                s = r - dist;
            }
            for (size_t k = 0; k < d.mCaverns.size(); ++k)
            {
                const WolfTunnels::Cavern& c = d.mCaverns[k];
                const F32 dc = sqrtf((x - c.mX) * (x - c.mX) + (y - c.mY) * (y - c.mY));
                F32 sc, vc;
                if (rough)
                {
                    const F32 amp = llmin(ROUGH_CAVERN * c.mR, ROUGH_CAVERN_MAX_M);
                    sc = c.mR - dc + amp * valueNoise(x, y, llmax(c.mR * 0.45f, 1.5f), seed ^ (0x63610000u + (U32)k));
                    vc = roughVault(sc, c.mR, c.mH, CAVERN_WALL_FRAC_ROUGH, x, y);
                }
                else
                {
                    const F32 hw2 = CAVERN_WALL_FRAC_SMOOTH * c.mH;
                    vc = dc <= c.mR ? hw2 + (c.mH - hw2) * sqrtf(llmax(0.f, 1.f - (dc / c.mR) * (dc / c.mR))) : hw2 - (dc - c.mR) * WALL_FALLOFF;
                    sc = c.mR - dc;
                }
                s = llmax(s, sc);
                v = llmax(v, vc);
            }
            F32 f = floorT(t);
            if (rough)
            {
                f += ROUGH_FLOOR_M * valueNoise(x, y, ROUGH_FLOOR_CELL_M, seed ^ 0x666c6f6fu);
                const F32 edge = llmax(0.f, 1.f - llmax(s, 0.f) / ROUGH_EDGE_M);
                f += ROUGH_EDGE_RISE_M * edge * edge;
            }
            const size_t n = route.mPts.size();
            F32 e = 1e6f;
            if (t <= route.mCum[1] + 1e-4f)
            {
                const LLVector2 u = route.dir(0), p = route.mPts[0];
                e = llmin(e, (x - p.mV[0]) * u.mV[0] + (y - p.mV[1]) * u.mV[1]);
            }
            if (t >= route.mCum[n - 2] - 1e-4f)
            {
                const LLVector2 u = route.dir(n - 2), p = route.mPts[n - 1];
                e = llmin(e, (p.mV[0] - x) * u.mV[0] + (p.mV[1] - y) * u.mV[1]);
            }
            Field out;
            out.s = s; out.e = e; out.f = f; out.v = llmax(v, CLEAR_MIN_M); out.t = t;
            return out;
        }
    };

    std::string places(const std::vector<LLVector2>& list)
    {
        std::string out;
        for (size_t i = 0; i < list.size() && (S32)i < WARN_PLACES; ++i) out += (i ? "; " : "") + llformat("%.0f, %.0f", list[i].mV[0], list[i].mV[1]);
        if ((S32)list.size() > WARN_PLACES) out += "; ...";
        return out;
    }

    // ── the records (php/tunnels.php) and the jobs ──────────────────────────────────────────────────────────────────
    std::vector<WolfTunnels::Record> s_records;
    std::string s_recordsRegion;        // region uuid the records belong to
    U32 s_recordsSerial = 0;
    bool s_fetchingRecords = false;
    bool s_jobBusy = false;
    U32 s_jobSerial = 0;                // bumped when a job ends (or its status changes)
    std::string s_jobStatus;
    std::vector<std::string> s_jobErrors;

    void jobFail(const std::string& msg)
    {
        s_jobErrors.push_back(msg);
        LL_WARNS("WolfTunnels") << msg << LL_ENDL;
    }

    WolfTunnels::Design designFromLLSD(const LLSD& d)
    {
        WolfTunnels::Design out;
        out.mStyle = d["style"].asString() == "rough" ? "rough" : "smooth";
        out.mKind = d.has("kind") ? d["kind"].asString() : "custom";
        out.mW = (F32)d["w"].asReal();
        out.mH = (F32)d["h"].asReal();
        for (const LLSD& v : llsd::inArray(d["p"])) out.mP.push_back((F32)v.asReal());
        out.mZ0 = (F32)d["z0"].asReal();
        out.mZ1 = (F32)d["z1"].asReal();
        for (const LLSD& c : llsd::inArray(d["caverns"]))
        {
            WolfTunnels::Cavern cv;
            cv.mX = (F32)c["x"].asReal(); cv.mY = (F32)c["y"].asReal(); cv.mR = (F32)c["r"].asReal(); cv.mH = (F32)c["h"].asReal();
            out.mCaverns.push_back(cv);
        }
        out.mPortals = d["portals"].asBoolean();
        out.mRock = d["rock"].asUUID();
        out.mRockScale = d.has("rockScale") ? (F32)d["rockScale"].asReal() : 4.f;
        out.mSeed = d["seed"].asInteger();
        return out;
    }

    /** The design as tunnels.php stores it (every number rounded to 0.01, as it rounds them). */
    LLSD designToLLSD(const WolfTunnels::Design& d)
    {
        LLSD out;
        out["v"] = TUNNEL_V;
        out["style"] = d.mStyle;
        out["kind"] = d.mKind;
        out["w"] = r2(d.mW);
        out["h"] = r2(d.mH);
        LLSD p = LLSD::emptyArray();
        for (F32 v : d.mP) p.append(r2(v));
        out["p"] = p;
        out["z0"] = r2(d.mZ0);
        out["z1"] = r2(d.mZ1);
        LLSD cs = LLSD::emptyArray();
        for (const WolfTunnels::Cavern& c : d.mCaverns) cs.append(LLSD().with("x", r2(c.mX)).with("y", r2(c.mY)).with("r", r2(c.mR)).with("h", r2(c.mH)));
        out["caverns"] = cs;
        out["portals"] = d.mPortals;
        out["rock"] = (d.mRock.notNull() ? d.mRock : WolfCaves::terrainRockId()).asString();
        out["rockScale"] = r2(d.mRockScale);
        out["seed"] = d.mSeed;
        return out;
    }

    void readRecords(const LLSD& region)
    {
        s_records.clear();
        for (const LLSD& t : llsd::inArray(region["tunnels"]))
        {
            WolfTunnels::Record r;
            r.mId = t["id"].asString();
            r.mName = t["name"].asString();
            r.mOwner = t["owner"].asString();
            r.mDesign = designFromLLSD(t["design"]);
            r.mBuilt = t["built"].isMap() ? t["built"] : LLSD();
            s_records.push_back(r);
        }
        s_recordsRegion = region["uuid"].asString();
        ++s_recordsSerial;
    }

    const WolfTunnels::Record* findRecord(const std::string& id)
    {
        for (const WolfTunnels::Record& r : s_records) if (r.mId == id) return &r;
        return nullptr;
    }

    /** The region's uuid as the grid's database knows it (the Paint record, as the cave builder). */
    std::string regionUuid()
    {
        const WolfTerrainPaint::Record* r = WolfTerrainPaint::instance().current();
        LLViewerRegion* rgn = gAgent.getRegion();
        return (r && rgn && r->mHandle == rgn->getHandle()) ? r->mUuid : std::string();
    }

    void fetchRecordsCoro()
    {
        const std::string uuid = regionUuid();
        if (uuid.empty() || !WolfGrid::isOnWolfTerritoriesConfirmed()) { s_fetchingRecords = false; return; }
        LLSD result = WolfCaves::serviceRequest(std::string(TUNNELS_URL) + "?region=" + uuid, nullptr, 20.f);
        s_fetchingRecords = false;
        LLSD reply;
        std::string transport;
        const S32 status = WolfCaves::serviceReply(result, reply, transport);
        if (status != 200 || !reply["success"].asBoolean())
        {
            jobFail("Could not read this region's tunnels: " + (reply.has("error") ? reply["error"].asString()
                    : (transport.empty() ? llformat("the tunnel service answered HTTP %d", status) : transport)));
            ++s_jobSerial;
            return;
        }
        if (regionUuid() != uuid) return;   // moved on meanwhile
        readRecords(reply["region"]);
    }

    void fetchRecords()
    {
        if (s_fetchingRecords || !WolfCaves::onWolf()) return;
        s_fetchingRecords = true;
        LLCoros::instance().launch("WolfTunnelsRecords", []() { fetchRecordsCoro(); });
    }

    /** POST one change to tunnels.php; the stored records come back. */
    bool putRecord(const LLSD& body, std::string& error)
    {
        const std::string text = boost::json::serialize(LlsdToJson(body));
        LLSD result = WolfCaves::serviceRequest(TUNNELS_URL, &text, 30.f);
        LLSD reply;
        std::string transport;
        const S32 status = WolfCaves::serviceReply(result, reply, transport);
        if (status != 200 || !reply["success"].asBoolean())
        {
            if (reply.isMap() && reply.has("error")) error = reply["error"].asString();
            else if (status == 403) error = "only the region owner, the estate owner and estate managers can make tunnels here";
            else if (status == 404) error = "the tunnel service is not available on this grid yet";
            else error = transport.empty() ? llformat("the tunnel service answered HTTP %d", status) : transport;
            return false;
        }
        readRecords(reply["region"]);
        return true;
    }

    /** One POST to the region's WolfTunnels capability (LLSD XML); the answer, or why not. */
    bool capPost(const std::string& url, const LLSD& body, LLSD& answer, std::string& error)
    {
        llcoro::suspendUntilTimeout(CAP_POST_GAP_S);
        auto adapter = std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfTunnels", LLCore::HttpRequest::DEFAULT_POLICY_ID);
        LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
        answer = adapter->postAndSuspend(request, url, body);
        LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(answer[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
        if (!status) { error = "the region did not answer (" + status.toString() + ")"; return false; }
        if (answer.has("message")) { error = answer["message"].asString(); return false; }
        return true;
    }

    /** Source: wolfcave.cpp WolfCaves::rezExact (the same RezObject, placed exactly; the item is fetched until it shows). */
    bool rezTunnel(const LLUUID& item_id, const LLUUID& folder_id, const LLVector3& ray_end, const LLVector3& root, const LLUUID& region,
                   LLUUID& object, std::string& error)
    {
        LLViewerInventoryItem* item = nullptr;
        for (S32 i = 0; i < 20 && !item; ++i)
        {
            LLViewerInventoryCategory* cat = gInventory.getCategory(folder_id);
            if (cat) cat->setVersion(LLViewerInventoryCategory::VERSION_UNKNOWN);
            gInventory.fetchDescendentsOf(folder_id);
            llcoro::suspendUntilTimeout(1.f);
            item = gInventory.getItem(item_id);
            if (item && !item->isFinished()) item = nullptr;
        }
        if (!item)
        {
            error = "The tunnel was stored in your inventory (Objects), but this viewer could not see it to rez it. Try Build again.";
            return false;
        }
        LLViewerRegion* rgn = gAgent.getRegion();
        if (!rgn || rgn->getRegionID() != region || !WolfGrid::isOnWolfTerritoriesConfirmed())
        {
            error = "You left the region before the tunnel was rezzed; it is in your Objects folder.";
            return false;
        }
        std::set<LLUUID> before;
        for (S32 i = 0, n = gObjectList.getNumObjects(); i < n; ++i)
        {
            LLViewerObject* o = gObjectList.getObject(i);
            if (o && !o->isDead() && o->isRoot()) before.insert(o->getID());
        }
        // Source: wolfcave.cpp rezExact (lltooldraganddrop.cpp:1970-2028 dropObject; RayEndIsIntersection = 1 places the object
        // at RayEnd, Scene.cs:2697-2706; RayStart = RayEnd keeps the water test from moving it). wolftunnel.rs computed RayEnd
        // so the ROOT (the anchor box) lands at `root`.
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_RezObject);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->addUUIDFast(_PREHASH_GroupID, FSCommon::getGroupForRezzing());
        msg->nextBlock("RezData");
        msg->addUUIDFast(_PREHASH_FromTaskID, LLUUID::null);
        msg->addU8Fast(_PREHASH_BypassRaycast, (U8)1);
        msg->addVector3Fast(_PREHASH_RayStart, ray_end);
        msg->addVector3Fast(_PREHASH_RayEnd, ray_end);
        msg->addUUIDFast(_PREHASH_RayTargetID, LLUUID::null);
        msg->addBOOLFast(_PREHASH_RayEndIsIntersection, true);
        msg->addBOOLFast(_PREHASH_RezSelected, false);
        msg->addBOOLFast(_PREHASH_RemoveItem, false);
        pack_permissions_slam(msg, item->getFlags(), item->getPermissions());
        msg->nextBlockFast(_PREHASH_InventoryData);
        item->packMessage(msg);
        msg->sendReliable(rgn->getHost());
        FSCommon::sObjectAddMsg = 0;   // lltooldraganddrop.cpp:2030-2032: no build defaults for it
        for (S32 t = 0; t < 120; ++t)
        {
            llcoro::suspendUntilTimeout(0.5f);
            if (!gAgent.getRegion() || gAgent.getRegion()->getRegionID() != region)
            {
                error = "You left the region while the tunnel was rezzing; if it rezzed, it is in the old region.";
                return false;
            }
            for (S32 i = 0, n = gObjectList.getNumObjects(); i < n; ++i)
            {
                LLViewerObject* o = gObjectList.getObject(i);
                if (!o || o->isDead() || !o->isRoot() || before.count(o->getID()) || o->getRegion() != gAgent.getRegion()) continue;
                if (dist_vec(o->getPositionRegion(), root) > 1.f) continue;
                if (!o->permYouOwner()) continue;
                object = o->getID();
                return true;
            }
        }
        error = "The region did not rez the tunnel. Check that you may build here and that the parcel has room for it; the tunnel is in your Objects folder.";
        return false;
    }

    /** The tunnel's object in the agent's own region, if this viewer has it. */
    LLViewerObject* findTunnelObject(const LLSD& built)
    {
        for (const LLSD& id : llsd::inArray(built["objects"]))
        {
            LLViewerObject* o = gObjectList.findObject(id.asUUID());
            if (o && !o->isDead() && gAgent.getRegion() && o->getRegion() == gAgent.getRegion()) return o->getRootEdit();
        }
        return nullptr;
    }

    /** Source: wolfcave.cpp WolfCaves::removeOne (llselectmgr.cpp:5818-5834 packDeRezHeader, :5865-5869 packObjectLocalID;
     *  llselectmgr.h:83-98 DRD_TRASH = 6, DRD_RETURN_TO_OWNER = 9). */
    void derez(LLViewerObject* obj)
    {
        LLViewerRegion* rgn = obj->getRegion();
        if (!rgn) return;
        const bool own = obj->permYouOwner();
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
        msg->addU32Fast(_PREHASH_ObjectLocalID, obj->getLocalID());
        msg->sendReliable(rgn->getHost());
    }

    /** Take a built tunnel's object and walkable boxes away (the record is kept). `forget`: the object is not in view and the
     *  user chose to leave it. */
    bool takeAway(const WolfTunnels::Record& rec, const std::string& cap, const LLUUID& region, bool forget, std::string& error)
    {
        if (!rec.mBuilt.isMap()) return true;
        LLViewerObject* obj = findTunnelObject(rec.mBuilt);
        if (!obj && rec.mBuilt["objects"].size() > 0 && !forget)
        {
            error = "the tunnel's rock is not in view from here. Stand nearer the tunnel and try again.";
            return false;
        }
        if (obj)
        {
            s_jobStatus = "Taking the old rock away...";
            derez(obj);
        }
        S32 n = 0;
        for (const LLSD& id : llsd::inArray(rec.mBuilt["volumes"]))
        {
            if (!gAgent.getRegion() || gAgent.getRegion()->getRegionID() != region) { error = "you left the region while its walkable boxes were being removed."; return false; }
            s_jobStatus = llformat("Removing the walkable boxes (%d of %d)...", ++n, (S32)rec.mBuilt["volumes"].size());
            LLSD answer;
            std::string err;
            const bool ok = capPost(cap, LLSD().with("op", "remove").with("id", id.asInteger()), answer, err);
            // "that tunnel is not here any more" (WolfTunnelsModule.cs Apply remove): already gone, which is what we want.
            if (!ok && err.find("not here any more") == std::string::npos) { error = "a walkable box could not be removed: " + err; return false; }
            if (answer.has("tunnels")) WolfTunnels::setFromAnswer(answer, region);
        }
        return true;
    }

    void buildCoro(WolfTunnels::Record rec, std::string cap, LLUUID region, std::string region_uuid)
    {
        std::string err;
        auto stop = [&](const std::string& why) { jobFail("Building stopped: " + why); s_jobStatus.clear(); s_jobBusy = false; ++s_jobSerial; };
        // 1. An old build of this tunnel goes first (a rebuild never forgets: its rock must be in view).
        if (const WolfTunnels::Record* old = findRecord(rec.mId))
        {
            if (old->mBuilt.isMap())
            {
                const WolfTunnels::Record copy = *old;
                if (!takeAway(copy, cap, region, false, err)) { stop(err); return; }
            }
        }
        // 2. Save the design (this is also the rights check: tunnels.php takes it from region holders only).
        s_jobStatus = "Step 1 of 5: saving the tunnel...";
        LLSD put;
        put["region"] = region_uuid;
        put["op"] = "put";
        put["tunnel"] = LLSD().with("id", rec.mId).with("name", rec.mName).with("design", designToLLSD(rec.mDesign)).with("built", LLSD());
        if (!putRecord(put, err)) { stop(err); return; }
        // 3. The rock: the proxy builds the STORED design into the Objects folder.
        s_jobStatus = "Step 2 of 5: making the rock (a long tunnel takes a while)...";
        const LLUUID folder = gInventory.findCategoryUUIDForType(LLFolderType::FT_OBJECT);
        LLViewerRegion* rgn = gAgent.getRegion();
        if (folder.isNull()) { stop("your Objects inventory folder has not loaded yet."); return; }
        if (!rgn || rgn->getRegionID() != region) { stop("you left the region the tunnel belongs to."); return; }
        LLSD body;
        body["mode"] = "build";
        body["name"] = rec.mName;
        body["tunnel_id"] = rec.mId;
        body["region_uuid"] = region_uuid;
        body["folder_id"] = folder.asString();
        const LLVector3d origin = rgn->getOriginGlobal();
        body["region"] = LLSD().with("origin_x", origin.mdV[VX]).with("origin_y", origin.mdV[VY])
                               .with("size_x", (S32)rgn->getWidth()).with("size_y", (S32)rgn->getWidth()).with("water", rgn->getWaterHeight());
        const std::string text = boost::json::serialize(LlsdToJson(body));
        LLSD result = WolfCaves::serviceRequest(tunnelMeshUrl(), &text, 600.f);
        LLSD reply;
        std::string transport;
        const S32 status = WolfCaves::serviceReply(result, reply, transport);
        if (status != 200 || !reply["success"].asBoolean()) { stop(WolfCaves::meshServiceError(status, reply, transport)); return; }
        // 4. Rez it.
        const S32 li = reply["li"].asInteger();
        s_jobStatus = llformat("Step 3 of 5: rezzing the tunnel (%d prims)...", li);
        const LLSD& re = reply["rez_ray_end"];
        const LLSD& rp = reply["root_pos"];
        const LLVector3 ray((F32)re[0].asReal(), (F32)re[1].asReal(), (F32)re[2].asReal());
        const LLVector3 root((F32)rp[0].asReal(), (F32)rp[1].asReal(), (F32)rp[2].asReal());
        LLUUID object;
        if (!rezTunnel(reply["item_id"].asUUID(), folder, ray, root, region, object, err)) { stop(err); return; }
        LLSD built;
        built["item"] = reply["item_id"].asString();
        built["objects"] = LLSD::emptyArray().with(0, object.asString());
        built["volumes"] = LLSD::emptyArray();
        built["at"] = (S32)LLDate::now().secondsSinceEpoch();
        auto record = [&](const char* when)
        {
            LLSD p;
            p["region"] = region_uuid;
            p["op"] = "put";
            p["tunnel"] = LLSD().with("id", rec.mId).with("name", rec.mName).with("design", designToLLSD(rec.mDesign)).with("built", built);
            std::string e2;
            if (!putRecord(p, e2)) jobFail(std::string("The tunnel record could not be updated ") + when + ": " + e2);
        };
        record("after the rez");
        // 5. The walkable boxes, one at a time; each answer lists every box, the new one is the id not seen before.
        std::set<S32> known;
        for (const WolfTunnels::Box& b : WolfTunnels::boxes()) known.insert(b.mId);
        const LLSD& vols = reply["volumes"];
        S32 n = 0;
        for (const LLSD& v : llsd::inArray(vols))
        {
            if (!gAgent.getRegion() || gAgent.getRegion()->getRegionID() != region)
            {
                record("when you left the region");
                stop(llformat("you left the region after %d of %d walkable boxes; Build again to finish.", n, (S32)vols.size()));
                return;
            }
            s_jobStatus = llformat("Step 4 of 5: making it walkable (box %d of %d)...", ++n, (S32)vols.size());
            LLSD answer;
            if (!capPost(cap, LLSD().with("op", "add").with("tunnel", LLSD().with("c", v["c"]).with("s", v["s"]).with("yaw", v["yaw"])), answer, err))
            {
                record("after a walkable box was refused");
                stop("a walkable box was refused: " + err);
                return;
            }
            S32 got = -1;
            for (const LLSD& t : llsd::inArray(answer["tunnels"]))
            {
                const S32 id = t["id"].asInteger();
                if (!known.count(id)) { got = llmax(got, id); }
            }
            for (const LLSD& t : llsd::inArray(answer["tunnels"])) known.insert(t["id"].asInteger());
            if (got >= 0) built["volumes"].append(got);
            WolfTunnels::setFromAnswer(answer, region);   // the ground opens and the camera follows at once
        }
        s_jobStatus = "Step 5 of 5: saving...";
        record("at the end");
        for (const LLSD& w : llsd::inArray(reply["warnings"])) jobFail(w.asString());
        s_jobStatus = "\"" + rec.mName + "\" is built: " + llformat("%.0f m, %d prims, %d walkable boxes.", reply["length"].asReal(), li, (S32)built["volumes"].size());
        s_jobBusy = false;
        ++s_jobSerial;
    }

    void deleteCoro(WolfTunnels::Record rec, std::string cap, LLUUID region, std::string region_uuid, bool forget)
    {
        std::string err;
        if (!takeAway(rec, cap, region, forget, err))
        {
            jobFail("Deleting stopped: " + err);
            s_jobBusy = false;
            ++s_jobSerial;
            return;
        }
        s_jobStatus = "Forgetting the tunnel...";
        if (!putRecord(LLSD().with("region", region_uuid).with("op", "delete").with("id", rec.mId), err)) jobFail("The tunnel's record could not be removed: " + err);
        else s_jobStatus = "\"" + rec.mName + "\" is taken away.";
        s_jobBusy = false;
        ++s_jobSerial;
    }
}

F32 WolfTunnels::routeLength(const Design& d) { return Route(d.mP).mLen; }

// Source: wolftunnel.rs validate_design - the same checks and the same words, so Build is only offered for what will build.
std::string WolfTunnels::designProblem(const Design& d, F32 region_w)
{
    const size_t n = d.mP.size() / 2;
    if (n < 2) return "Draw the route: at least two points, one at each mouth.";
    if (d.mW < W_MIN || d.mW > W_MAX) return llformat("The tunnel must be %.0f to %.0f m wide.", W_MIN, W_MAX);
    if (d.mH < H_MIN || d.mH > H_MAX) return llformat("The tunnel must be %.1f to %.0f m high.", H_MIN, H_MAX);
    for (F32 v : d.mP) if (v < 0.f || v > region_w) return "The route leaves the region - keep every point inside it.";
    const Route route(d.mP);
    F32 first = 0.f, last = 0.f;
    for (size_t k = 1; k < n; ++k)
    {
        const F32 l = route.mCum[k] - route.mCum[k - 1];
        if (l < SEG_MIN_M) return "Two points of the route are less than 0.5 m apart - undo one of them.";
        if (k == 1) first = l;
        last = l;
    }
    if (first < d.mW || last < d.mW) return llformat("Make the first and last piece of the route at least as long as the tunnel is wide (%.1f m).", d.mW);
    if (d.mZ0 < Z_MIN || d.mZ0 > Z_MAX || d.mZ1 < Z_MIN || d.mZ1 > Z_MAX) return "The floor heights must be -90 to 4000 m.";
    if (fabsf(d.mZ1 - d.mZ0) / route.mLen > GRADE_MAX) return "The tunnel is steeper than 1 in 1 (45 degrees) - make it longer or bring the floor heights closer.";
    for (size_t i = 0; i < d.mCaverns.size(); ++i)
    {
        const Cavern& c = d.mCaverns[i];
        if (c.mR < CAVERN_R_MIN || c.mR > CAVERN_R_MAX) return llformat("Cavern %d's radius must be 3 to 30 m.", (S32)i + 1);
        if (c.mH < CAVERN_H_MIN || c.mH > CAVERN_H_MAX) return llformat("Cavern %d's height must be 3 to 25 m.", (S32)i + 1);
        const std::pair<F32, F32> nt = route.nearest(c.mX, c.mY);
        if (nt.first > d.mW * 0.5f) return llformat("Cavern %d is not on the route - put it on the tunnel.", (S32)i + 1);
        if (nt.second < c.mR + ROUGH_CAVERN_MAX_M || nt.second > route.mLen - c.mR - ROUGH_CAVERN_MAX_M)
            return llformat("Cavern %d is too near a mouth - move it further in or make it smaller.", (S32)i + 1);
        if (c.mX - c.mR < 0.f || c.mY - c.mR < 0.f || c.mX + c.mR > region_w || c.mY + c.mR > region_w)
            return llformat("Cavern %d reaches outside the region.", (S32)i + 1);
    }
    return std::string();
}

// =====================================================================================================================
// the window
// =====================================================================================================================

WolfFloaterTunnels::WolfFloaterTunnels(const LLSD& key) : LLFloater(key) {}

WolfFloaterTunnels::~WolfFloaterTunnels()
{
    if (WolfToolTunnel::instanceExists()) WolfToolTunnel::getInstance()->cancel();
}

// static
WolfFloaterTunnels* WolfFloaterTunnels::editing()
{
    WolfFloaterTunnels* f = LLFloaterReg::findTypedInstance<WolfFloaterTunnels>("wolf_tunnels");
    return (f && f->getVisible() && !f->isMinimized() && f->mEditing) ? f : nullptr;
}

bool WolfFloaterTunnels::postBuild()
{
    mTunList = getChild<LLScrollListCtrl>("tun_list");
    mTunList->setCommitCallback([this](LLUICtrl*, const LLSD&) { updateButtons(); });
    mNewBtn = getChild<LLButton>("new_btn");
    mEditBtn = getChild<LLButton>("edit_btn");
    mGoTunBtn = getChild<LLButton>("go_tunnel_btn");
    mDelTunBtn = getChild<LLButton>("delete_tunnel_btn");
    mName = getChild<LLLineEditor>("tun_name");
    mDrawBtn = getChild<LLButton>("draw_route_btn");
    mUndoPointBtn = getChild<LLButton>("undo_point_btn");
    mFinishBtn = getChild<LLButton>("finish_btn");
    mGroundBtn = getChild<LLButton>("ground_btn");
    mCavernBtn = getChild<LLButton>("cavern_btn");
    mUndoCavernBtn = getChild<LLButton>("undo_cavern_btn");
    mBuildBtn = getChild<LLButton>("build_btn");
    mStopBtn = getChild<LLButton>("cancel_edit_btn");
    mKind = getChild<LLComboBox>("tun_kind");
    mW = getChild<LLSpinCtrl>("tun_w");
    mH = getChild<LLSpinCtrl>("tun_h");
    mZ0 = getChild<LLSpinCtrl>("tun_z0");
    mZ1 = getChild<LLSpinCtrl>("tun_z1");
    mStyle = getChild<LLRadioGroup>("tun_style");
    mCavernR = getChild<LLSliderCtrl>("cavern_r");
    mPortals = getChild<LLCheckBoxCtrl>("tun_portals");
    mAdvancedCheck = getChild<LLCheckBoxCtrl>("advanced_check");
    mRouteInfo = getChild<LLTextBox>("route_info");
    mGradeInfo = getChild<LLTextBox>("grade_info");
    mWarnings = getChild<LLTextBox>("tun_warnings");
    mAdvanced = getChild<LLPanel>("advanced_panel");
    mStatus = getChild<LLTextBox>("status");

    mNewBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onNewTunnel(); });
    mEditBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onEditTunnel(); });
    mGoTunBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onGoInsideTunnel(); });
    mDelTunBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onDeleteTunnel(); });
    mDrawBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onPen(WolfToolTunnel::PEN_ROUTE); });
    mCavernBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onPen(WolfToolTunnel::PEN_CAVERN); });
    mUndoPointBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { undoPoint(); });
    mFinishBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { finishRoute(); });
    mGroundBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onGround(); });
    mUndoCavernBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onUndoCavern(); });
    mBuildBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onBuild(); });
    mStopBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onStopEditing(); });
    mKind->setCommitCallback([this](LLUICtrl*, const LLSD&) { onKind(); });
    mW->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSize(); });
    mH->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSize(); });
    mZ0->setCommitCallback([this](LLUICtrl*, const LLSD&) { onFloors(); });
    mZ1->setCommitCallback([this](LLUICtrl*, const LLSD&) { onFloors(); });
    mStyle->setCommitCallback([this](LLUICtrl*, const LLSD&) { onStyle(); });
    mPortals->setCommitCallback([this](LLUICtrl*, const LLSD&) { mDesign.mPortals = mPortals->get(); designChanged(); });
    mAdvancedCheck->setCommitCallback([this](LLUICtrl*, const LLSD&) { onAdvanced(); });

    // the walkable boxes (Advanced)
    mBoxList = getChild<LLScrollListCtrl>("box_list");
    mBoxList->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSelectBox(); });
    static const char* SPINS[7] = { "centre_x", "centre_y", "centre_z", "size_l", "size_w", "size_h", "turn" };
    for (S32 i = 0; i < 7; ++i) mSpin[i] = getChild<LLSpinCtrl>(SPINS[i]);
    mSaveBtn = getChild<LLButton>("save_btn");
    mDeleteBtn = getChild<LLButton>("delete_btn");
    mGoBtn = getChild<LLButton>("go_btn");
    mRefreshBtn = getChild<LLButton>("refresh_btn");
    mShowCheck = getChild<LLCheckBoxCtrl>("show_outlines");
    mInfo = getChild<LLTextBox>("info");
    mSaveBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onSaveBox(); });
    mDeleteBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onDeleteBox(); });
    mGoBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onGoInsideBox(); });
    mRefreshBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { refresh(); });
    setEditing(false);
    onAdvanced();
    return true;
}

void WolfFloaterTunnels::onOpen(const LLSD& key)
{
    // [WOLF GRID GATE] checked once, on open (no polling): Tunnels are WolfSim's.
    if (!WolfGrid::isOnWolfTerritories())
    {
        mEdit = false;
        s_boxes.clear();
        s_records.clear();
        fillBoxList();
        fillTunnelList();
        setStatus("Tunnels work on Wolf Territories regions only.", true);
        updateButtons();
        return;
    }
    refresh();
}

void WolfFloaterTunnels::onClose(bool app_quitting)
{
    if (WolfToolTunnel::instanceExists()) WolfToolTunnel::getInstance()->cancel();
    mRouteOpen = false;
}

void WolfFloaterTunnels::draw()
{
    // the region changed under an open window (teleport, crossing): what it shows belongs to the old one
    LLViewerRegion* rgn = gAgent.getRegion();
    if (rgn && mRegionAtFetch.notNull() && rgn->getRegionID() != mRegionAtFetch && !mBusy && !s_jobBusy)
    {
        mRegionAtFetch.setNull();
        if (WolfToolTunnel::instanceExists()) WolfToolTunnel::getInstance()->cancel();
        if (mEditing) { setEditing(false); setStatus("You are on another region now: the drawing was put away.", true); }
        refresh();
    }
    if (mShownRecords != s_recordsSerial) { mShownRecords = s_recordsSerial; fillTunnelList(); }
    if (s_jobBusy && mStatus->getText() != s_jobStatus) setStatus(s_jobStatus, false);
    if (mShownJob != s_jobSerial)
    {
        mShownJob = s_jobSerial;
        if (!s_jobErrors.empty())
        {
            std::string all;
            for (const std::string& e : s_jobErrors) all += (all.empty() ? "" : "\n") + e;
            s_jobErrors.clear();
            setStatus((s_jobStatus.empty() ? "" : s_jobStatus + "\n") + all, true);
        }
        else if (!s_jobStatus.empty()) setStatus(s_jobStatus, false);
        if (!s_jobBusy && mEditing && findRecord(mEditId) && findRecord(mEditId)->mBuilt.isMap()) setEditing(false);
        fillBoxList();   // a job adds and removes walkable boxes (WolfTunnels::setFromAnswer)
        updateButtons();
    }
    LLFloater::draw();
}

void WolfFloaterTunnels::setStatus(const std::string& text, bool error)
{
    if (!mStatus) return;
    mStatus->setText(text);
    mStatus->setColor(LLUIColorTable::instance().getColor(error ? "AlertCautionTextColor" : "LabelTextColor"));
}

void WolfFloaterTunnels::onAdvanced()
{
    // floater_wolf_tunnels.xml: the window is 676 high with Advanced closed; the Advanced panel adds its 228.
    const bool on = mAdvancedCheck->get();
    const S32 want = on ? 676 + 228 : 676;
    if (mAdvanced->getVisible() == on && getRect().getHeight() == want) return;
    mAdvanced->setVisible(on);
    // Grow or shrink downwards, keeping the window's top where it is.
    LLRect r = getRect();
    r.mBottom = r.mTop - want;
    setShape(r);
}

// ── the tunnels ─────────────────────────────────────────────────────────────────────────────────────────────────────

void WolfFloaterTunnels::fillTunnelList()
{
    if (!mTunList) return;
    const std::string keep = mTunList->getSelectedValue().asString();
    mTunList->deleteAllItems();
    const LLFontGL* f = LLFontGL::getFontSansSerifSmall();
    const LLColor4 text = LLUIColorTable::instance().getColor("LabelTextColor");
    for (const WolfTunnels::Record& r : s_records)
    {
        LLScrollListItem::Params rp;
        rp.value(LLSD(r.mId));
        auto cell = [&](const char* col, const std::string& v)
        {
            LLScrollListCell::Params cp;
            cp.column(col);
            cp.value(v);
            cp.color(text);
            cp.font(f);
            rp.columns.add(cp);
        };
        cell("name", r.mName);
        cell("style", r.mDesign.mStyle == "rough" ? "Rough" : "Smooth");
        cell("length", llformat("%.0f m", WolfTunnels::routeLength(r.mDesign)));
        cell("built", r.mBuilt.isMap() ? llformat("built, %d boxes", (S32)r.mBuilt["volumes"].size()) : std::string("not built"));
        mTunList->addRow(rp);
    }
    if (!keep.empty()) mTunList->selectByValue(LLSD(keep));
    updateButtons();
}

const WolfTunnels::Record* WolfFloaterTunnels::pickedTunnel() const
{
    return mTunList ? findRecord(mTunList->getSelectedValue().asString()) : nullptr;
}

void WolfFloaterTunnels::setEditing(bool on)
{
    mEditing = on;
    if (!on)
    {
        if (WolfToolTunnel::instanceExists()) WolfToolTunnel::getInstance()->cancel();
        mRouteOpen = false;
        mDesign = WolfTunnels::Design();
        mPreview = Preview();
        mProblem.clear();
        mEditId.clear();
        if (mName) mName->clear();
    }
    showDesign();
    designChanged();   // the warnings, the grade line and the buttons follow
}

void WolfFloaterTunnels::onNewTunnel()
{
    if (!WolfCaves::onWolf()) { setStatus("Tunnels work on Wolf Territories regions the grid knows only.", true); return; }
    setEditing(false);
    mDesign = WolfTunnels::Design();
    // Remember the last style and type chosen (Paul 10-22: "remember the last choice").
    mDesign.mStyle = gSavedSettings.getString("WolfTunnelStyle") == "rough" ? "rough" : "smooth";
    mDesign.mKind = gSavedSettings.getString("WolfTunnelKind");
    if (mDesign.mKind == "wide") { mDesign.mW = 6.f; mDesign.mH = 4.5f; }
    else if (mDesign.mKind == "rail") { mDesign.mW = 6.f; mDesign.mH = 6.f; }
    else if (mDesign.mKind != "custom") { mDesign.mKind = "walk"; mDesign.mW = 4.f; mDesign.mH = 3.5f; }
    mDesign.mRock = WolfCaves::terrainRockId();
    mDesign.mSeed = ll_rand(SEED_MAX);
    mEditing = true;
    mEditId.clear();
    mName->setText(llformat("Tunnel %d", (S32)s_records.size() + 1));
    showDesign();
    designChanged();
    onPen(WolfToolTunnel::PEN_ROUTE);
}

void WolfFloaterTunnels::onEditTunnel()
{
    const WolfTunnels::Record* r = pickedTunnel();
    if (!r) return;
    setEditing(false);
    mEditing = true;
    mEditId = r->mId;
    mDesign = r->mDesign;
    mName->setText(r->mName);
    showDesign();
    designChanged();
    setStatus("Change what you like, then press Build: the old rock and boxes are taken away and it is built again.", false);
}

void WolfFloaterTunnels::onStopEditing()
{
    setEditing(false);
    setStatus("The drawing was put away; nothing was changed.", false);
}

void WolfFloaterTunnels::showDesign()
{
    if (!mKind) return;
    mKind->setValue(mDesign.mKind);
    mW->setValue(mDesign.mW);
    mH->setValue(mDesign.mH);
    mStyle->setValue(mDesign.mStyle);
    mZ0->setValue(mDesign.mZ0);
    mZ1->setValue(mDesign.mZ1);
    mPortals->set(mDesign.mPortals);
}

void WolfFloaterTunnels::onKind()
{
    mDesign.mKind = mKind->getValue().asString();
    if (mDesign.mKind == "walk") { mDesign.mW = 4.f; mDesign.mH = 3.5f; }
    else if (mDesign.mKind == "wide") { mDesign.mW = 6.f; mDesign.mH = 4.5f; }
    else if (mDesign.mKind == "rail") { mDesign.mW = 6.f; mDesign.mH = 6.f; }
    gSavedSettings.setString("WolfTunnelKind", mDesign.mKind);
    showDesign();
    designChanged();
}

void WolfFloaterTunnels::onSize()
{
    mDesign.mW = llclamp((F32)mW->getValueF32(), W_MIN, W_MAX);
    mDesign.mH = llclamp((F32)mH->getValueF32(), H_MIN, H_MAX);
    mDesign.mKind = "custom";
    mKind->setValue("custom");
    gSavedSettings.setString("WolfTunnelKind", "custom");
    designChanged();
}

void WolfFloaterTunnels::onStyle()
{
    mDesign.mStyle = mStyle->getValue().asString() == "rough" ? "rough" : "smooth";
    gSavedSettings.setString("WolfTunnelStyle", mDesign.mStyle);
    designChanged();
}

void WolfFloaterTunnels::onFloors()
{
    mDesign.mZ0 = (F32)mZ0->getValueF32();
    mDesign.mZ1 = (F32)mZ1->getValueF32();
    designChanged();
}

void WolfFloaterTunnels::onGround()
{
    LLViewerRegion* rgn = gAgent.getRegion();
    const size_t n = mDesign.mP.size() / 2;
    if (!rgn || n < 1) return;
    const LLSurface& land = rgn->getLand();
    mDesign.mZ0 = r2(land.resolveHeightRegion(mDesign.mP[0], mDesign.mP[1]));
    mDesign.mZ1 = r2(land.resolveHeightRegion(mDesign.mP[2 * n - 2], mDesign.mP[2 * n - 1]));
    showDesign();
    designChanged();
}

void WolfFloaterTunnels::onUndoCavern()
{
    if (mDesign.mCaverns.empty()) return;
    mDesign.mCaverns.pop_back();
    designChanged();
}

void WolfFloaterTunnels::onPen(S32 pen)
{
    WolfToolTunnel* t = WolfToolTunnel::getInstance();
    if (t->pen() == pen) { t->cancel(); if (pen == WolfToolTunnel::PEN_ROUTE) mRouteOpen = false; updateButtons(); return; }
    if (pen == WolfToolTunnel::PEN_CAVERN && mDesign.mP.size() < 4) { setStatus("Draw the route first: caverns sit on it.", true); return; }
    t->begin(getHandle(), (WolfToolTunnel::EPen)pen);   // (stops any other pen first, which closes the route)
    if (pen == WolfToolTunnel::PEN_ROUTE) mRouteOpen = true;
    setStatus(getString(pen == WolfToolTunnel::PEN_ROUTE ? "str_draw" : "str_cavern"), false);
    updateButtons();
}

void WolfFloaterTunnels::penStopped()
{
    mRouteOpen = false;
    updateButtons();
}

void WolfFloaterTunnels::addRoutePoint(F32 x, F32 y)
{
    if (!mEditing) return;
    const size_t n = mDesign.mP.size() / 2;
    if (n > 0 && sqrtf(powf(x - mDesign.mP[2 * n - 2], 2.f) + powf(y - mDesign.mP[2 * n - 1], 2.f)) < SEG_MIN_M) return;   // a double-click's second press
    mDesign.mP.push_back(r2(x));
    mDesign.mP.push_back(r2(y));
    // The mouths' floors start on the ground where they are; the user can move them (Floor at start / at end).
    LLViewerRegion* rgn = gAgent.getRegion();
    if (rgn)
    {
        const F32 g = r2(rgn->getLand().resolveHeightRegion(x, y));
        if (n == 0) mDesign.mZ0 = g;
        mDesign.mZ1 = g;
        showDesign();
    }
    designChanged();
}

void WolfFloaterTunnels::finishRoute()
{
    if (WolfToolTunnel::instanceExists() && WolfToolTunnel::getInstance()->pen() == WolfToolTunnel::PEN_ROUTE) WolfToolTunnel::getInstance()->cancel();
    mRouteOpen = false;
    if (mDesign.mP.size() < 4) setStatus("A route needs two points at least: click one at each mouth.", true);
    else setStatus("Route finished. Shape it in step 2, then Build.", false);
    designChanged();
}

void WolfFloaterTunnels::undoPoint()
{
    if (mDesign.mP.size() < 2) return;
    mDesign.mP.resize(mDesign.mP.size() - 2);
    LLViewerRegion* rgn = gAgent.getRegion();
    const size_t n = mDesign.mP.size() / 2;
    if (rgn && n > 0) { mDesign.mZ1 = r2(rgn->getLand().resolveHeightRegion(mDesign.mP[2 * n - 2], mDesign.mP[2 * n - 1])); showDesign(); }
    designChanged();
}

void WolfFloaterTunnels::addCavernAt(F32 x, F32 y)
{
    if (mDesign.mP.size() < 4) return;
    const Route route(mDesign.mP);
    // On the route: snapped to the nearest route point.
    const std::pair<F32, F32> nt = route.nearest(x, y);
    if (nt.first > llmax(mDesign.mW * 2.f, (F32)mCavernR->getValueF32()))
    {
        setStatus("Click on the tunnel's route to put a cavern there.", true);
        return;
    }
    LLVector2 p, u;
    route.at(nt.second, p, u);
    WolfTunnels::Cavern c;
    c.mX = r2(p.mV[0]);
    c.mY = r2(p.mV[1]);
    c.mR = llclamp((F32)mCavernR->getValueF32(), CAVERN_R_MIN, CAVERN_R_MAX);
    // A cavern is taller than the tunnel, and rises with its size.
    c.mH = r2(llclamp(llmax(mDesign.mH + 1.f, c.mR * 0.8f), CAVERN_H_MIN, CAVERN_H_MAX));
    WolfTunnels::Design test = mDesign;
    test.mCaverns.push_back(c);
    LLViewerRegion* rgn = gAgent.getRegion();
    const std::string why = WolfTunnels::designProblem(test, rgn ? rgn->getWidth() : 256.f);
    if (why.find("Cavern") == 0) { setStatus(why, true); return; }
    mDesign.mCaverns.push_back(c);
    setStatus(llformat("Cavern %d added (%.0f m across).", (S32)mDesign.mCaverns.size(), c.mR * 2.f), false);
    designChanged();
}

/** The design changed: its problem, the local preview (tube, caverns, depth, warnings) and the grade line. */
void WolfFloaterTunnels::designChanged()
{
    mPreview = Preview();
    LLViewerRegion* rgn = gAgent.getRegion();
    const size_t n = mDesign.mP.size() / 2;
    mRouteInfo->setText(n == 0 ? std::string() : llformat("%d point%s, %.0f m", (S32)n, n == 1 ? "" : "s", WolfTunnels::routeLength(mDesign)));
    mProblem = rgn ? WolfTunnels::designProblem(mDesign, rgn->getWidth()) : std::string("You are not in a region.");
    if (n >= 2 && rgn)
    {
        const Air air(mDesign);
        const F32 len = air.route.mLen;
        const F32 grade = len > 0.f ? (mDesign.mZ1 - mDesign.mZ0) / len : 0.f;
        mGradeInfo->setText(fabsf(grade) < 1e-4f ? llformat("%.0f m long, level floor", len)
                            : llformat("%.0f m long, the floor %s %.1f m: %.1f%% (1 in %.0f)", len, grade > 0.f ? "climbs" : "falls",
                                       fabsf(mDesign.mZ1 - mDesign.mZ0), fabsf(grade) * 100.f, 1.f / fabsf(grade)));
        if (mProblem.empty() || mProblem.find("Cavern") == 0 || mProblem.find("first and last") != std::string::npos)
        {
            // Cross-sections along the route: from the route outwards to where the air ends, the floor and the roof at
            // PREVIEW_ACROSS points across. Source: wolftunnel.rs Air::eval (the same shape the generator builds).
            const F32 step = llmax(1.f, len / (F32)PREVIEW_RINGS_MAX);
            const S32 rings = llmax(1, (S32)ceilf(len / step));
            F32 far_r = air.reach() + 1.f;
            for (const WolfTunnels::Cavern& c : mDesign.mCaverns) far_r = llmax(far_r, c.mR + ROUGH_CAVERN_MAX_M + c.mR + 1.f);
            std::vector<LLVector3> prev_f, prev_c;
            for (S32 i = 0; i <= rings; ++i)
            {
                const F32 t = len * (F32)i / (F32)rings;
                LLVector2 p, u;
                air.route.at(t, p, u);
                const LLVector2 left(-u.mV[1], u.mV[0]);
                auto edge = [&](F32 sign) -> F32
                {
                    F32 inside = 0.f, q = 0.f;
                    while (q < far_r)
                    {
                        q += 0.25f;
                        const LLVector2 a = p + left * (sign * q);
                        if (air.eval(a.mV[0], a.mV[1]).s < 0.f) break;
                        inside = q;
                    }
                    return inside;
                };
                const F32 ql = edge(1.f), qr = edge(-1.f);
                std::vector<LLVector3> fl, ce;
                for (S32 k = 0; k <= PREVIEW_ACROSS; ++k)
                {
                    const F32 q = -qr + (ql + qr) * (F32)k / (F32)PREVIEW_ACROSS;
                    const LLVector2 a = p + left * q;
                    const Field fd = air.eval(a.mV[0], a.mV[1]);
                    fl.emplace_back(a.mV[0], a.mV[1], fd.f);
                    ce.emplace_back(a.mV[0], a.mV[1], fd.f + fd.v);
                }
                if (!prev_f.empty())
                {
                    for (S32 k = 0; k < PREVIEW_ACROSS; ++k)
                    {
                        mPreview.mFloor.insert(mPreview.mFloor.end(), { prev_f[k], prev_f[k + 1], fl[k + 1], prev_f[k], fl[k + 1], fl[k] });
                        mPreview.mRoof.insert(mPreview.mRoof.end(), { prev_c[k], prev_c[k + 1], ce[k + 1], prev_c[k], ce[k + 1], ce[k] });
                    }
                    // the walls: floor to roof at both edges
                    for (S32 k : { 0, PREVIEW_ACROSS })
                        mPreview.mRoof.insert(mPreview.mRoof.end(), { prev_f[k], fl[k], ce[k], prev_f[k], ce[k], prev_c[k] });
                }
                prev_f = fl;
                prev_c = ce;
            }
            // Warnings over the ground this viewer has (wolftunnel.rs warnings() leaves the ground ones to the viewer).
            const LLSurface& land = rgn->getLand();
            const F32 water = rgn->getWaterHeight();
            auto loaded = [&](F32 x, F32 y) { LLSurfacePatch* pt = land.resolvePatchRegion(x, y); return pt && pt->getHasReceivedData(); };
            std::vector<LLVector2> out_of, thin, wet;
            bool in_out = false, in_thin = false, in_wet = false;
            const F32 post_every = llmax(10.f, len / 20.f);
            F32 next_post = MOUTH_M;
            for (F32 t = 0.f; t <= len; t += 2.f)
            {
                LLVector2 p, u;
                air.route.at(t, p, u);
                if (!loaded(p.mV[0], p.mV[1])) continue;
                const F32 g = land.resolveHeightRegion(p.mV[0], p.mV[1]);
                const F32 roof = air.floorT(t) + air.top();
                const bool inner = t >= MOUTH_M && t <= len - MOUTH_M;
                const bool o = inner && g < roof, th = inner && !o && g - roof < COVER_THIN_M, w = inner && g < water;
                if (o && !in_out) out_of.push_back(p);
                if (th && !in_thin) thin.push_back(p);
                if (w && !in_wet) wet.push_back(p);
                in_out = o; in_thin = th; in_wet = w;
                if (o) mPreview.mBreakouts.emplace_back(p.mV[0], p.mV[1], roof);
                if (inner && t >= next_post)
                {
                    next_post = t + post_every;
                    mPreview.mPosts.emplace_back(p.mV[0], p.mV[1], g);
                    mPreview.mPosts.emplace_back(p.mV[0], p.mV[1], roof);
                    mPreview.mPostLabels.push_back(g >= roof ? llformat("%.1f m of ground over the roof", g - roof) : llformat("roof %.1f m above the ground", roof - g));
                }
            }
            if (!out_of.empty()) mPreview.mWarnings.push_back("The tunnel comes out of the ground at " + places(out_of) + " (red) - its roof is above the hill there. Lower the floors or move the route under higher ground.");
            if (!thin.empty()) mPreview.mWarnings.push_back(llformat("Less than %.1f m of ground over the roof at ", COVER_THIN_M) + places(thin) + " - the ground may open there.");
            if (!wet.empty()) mPreview.mWarnings.push_back("The tunnel runs under the water at " + places(wet) + " - it stays dry inside.");
            for (const WolfTunnels::Cavern& c : mDesign.mCaverns)
            {
                if (!loaded(c.mX, c.mY)) continue;
                const F32 tc = air.route.nearest(c.mX, c.mY).second;
                if (land.resolveHeightRegion(c.mX, c.mY) < air.floorT(tc) + c.mH)
                    mPreview.mWarnings.push_back(llformat("The cavern at %.0f, %.0f comes out of the ground - make it smaller or put it under higher ground.", c.mX, c.mY));
            }
            const F32 ends[2][3] = { { mDesign.mP[0], mDesign.mP[1], mDesign.mZ0 }, { mDesign.mP[2 * n - 2], mDesign.mP[2 * n - 1], mDesign.mZ1 } };
            for (S32 k = 0; k < 2; ++k)
            {
                const char* name = k == 0 ? "start" : "end";
                if (!loaded(ends[k][0], ends[k][1])) continue;
                const F32 g = land.resolveHeightRegion(ends[k][0], ends[k][1]), z = ends[k][2];
                if (z < water - 0.1f) mPreview.mWarnings.push_back(llformat("The %s mouth's floor is %.1f m under the water line.", name, water - z));
                if (g - z > air.top()) mPreview.mWarnings.push_back(llformat("The %s mouth is buried: the ground there is %.1f m above the tunnel floor. Move that end or raise its floor.", name, g - z));
                else if (z - g > 1.f) mPreview.mWarnings.push_back(llformat("The %s mouth's floor is %.1f m above the ground - people will need a step or a ramp there.", name, z - g));
            }
            const bool rail = mDesign.mKind == "rail";
            if (fabsf(grade) > (rail ? RAIL_GRADE_WARN : WALK_GRADE_WARN))
                mPreview.mWarnings.push_back(llformat("The floor climbs %.1f%% - steep for %s.", fabsf(grade) * 100.f, rail ? "a train" : "walking"));
        }
    }
    else
    {
        mGradeInfo->setText(std::string());
    }
    std::string w;
    if (!mProblem.empty() && mEditing) w = mProblem;
    for (const std::string& s : mPreview.mWarnings) w += (w.empty() ? "" : "\n") + s;
    if (w.empty() && mEditing && n >= 2) w = "Ready to build. The see-through preview shows the tunnel in the ground.";
    mWarnings->setText(w);
    mWarnings->setColor(LLUIColorTable::instance().getColor(!mProblem.empty() || !mPreview.mWarnings.empty() ? "AlertCautionTextColor" : "LabelTextColor"));
    updateButtons();
}

void WolfFloaterTunnels::onBuild()
{
    if (!mEditing || s_jobBusy) return;
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn || !mProblem.empty()) { setStatus(mProblem.empty() ? "You are not in a region." : mProblem, true); return; }
    if (mCapUrl.empty() || mRegionAtFetch != rgn->getRegionID()) { setStatus(getString("str_need_cap"), true); return; }
    if (!mEdit) { setStatus(getString("str_need_rights"), true); return; }
    const std::string uuid = regionUuid();
    if (uuid.empty() || !WolfGrid::isOnWolfTerritoriesConfirmed()) { setStatus("This region's record is not loaded yet - wait a moment and try again.", true); return; }
    if (WolfToolTunnel::instanceExists()) WolfToolTunnel::getInstance()->cancel();
    mRouteOpen = false;
    WolfTunnels::Record rec;
    if (mEditId.empty()) { LLUUID id; id.generate(); mEditId = id.asString(); }
    rec.mId = mEditId;
    rec.mName = mName->getText().empty() ? std::string("Wolf Tunnel") : mName->getText();
    rec.mDesign = mDesign;
    s_jobBusy = true;
    s_jobErrors.clear();
    s_jobStatus = "Starting...";
    ++s_jobSerial;
    const std::string cap = mCapUrl;
    const LLUUID region = rgn->getRegionID();
    LLCoros::instance().launch("WolfTunnelsBuild", [rec, cap, region, uuid]() { buildCoro(rec, cap, region, uuid); });
    updateButtons();
}

void WolfFloaterTunnels::onDeleteTunnel()
{
    const WolfTunnels::Record* r = pickedTunnel();
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!r || !rgn || s_jobBusy) return;
    if (mCapUrl.empty() && r->mBuilt.isMap()) { setStatus(getString("str_need_cap"), true); return; }
    const WolfTunnels::Record rec = *r;
    const bool unseen = rec.mBuilt.isMap() && rec.mBuilt["objects"].size() > 0 && !findTunnelObject(rec.mBuilt);
    LLSD args;
    args["MESSAGE"] = unseen
        ? "\"" + rec.mName + "\"'s rock is not in view from here. Delete the tunnel anyway, leaving the rock where it is (you can delete it by hand)? Its walkable boxes are removed."
        : "Delete \"" + rec.mName + "\"? Its rock and its walkable boxes are taken away; anyone inside is lifted to the surface.";
    LLHandle<LLFloater> h = getHandle();
    const std::string cap = mCapUrl;
    const LLUUID region = rgn->getRegionID();
    const std::string uuid = regionUuid();
    LLNotificationsUtil::add("GenericAlertYesCancel", args, LLSD(), [h, rec, cap, region, uuid, unseen](const LLSD& n, const LLSD& resp)
    {
        if (LLNotificationsUtil::getSelectedOption(n, resp) != 0 || s_jobBusy) return;
        if (uuid.empty()) return;
        s_jobBusy = true;
        s_jobErrors.clear();
        s_jobStatus = "Deleting \"" + rec.mName + "\"...";
        ++s_jobSerial;
        LLCoros::instance().launch("WolfTunnelsDelete", [rec, cap, region, uuid, unseen]() { deleteCoro(rec, cap, region, uuid, unseen); });
        if (WolfFloaterTunnels* self = static_cast<WolfFloaterTunnels*>(h.get())) self->updateButtons();
    });
}

void WolfFloaterTunnels::onGoInsideTunnel()
{
    const WolfTunnels::Record* r = pickedTunnel();
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!r || !rgn || r->mDesign.mP.size() < 4) return;
    const Route route(r->mDesign.mP);
    LLVector2 p, u;
    const F32 t = llmin(3.f, route.mLen * 0.5f);
    route.at(t, p, u);
    const F32 z = r->mDesign.mZ0 + (r->mDesign.mZ1 - r->mDesign.mZ0) * t / llmax(1e-6f, route.mLen) + 1.f;
    gAgent.teleportViaLocation(rgn->getPosGlobalFromRegion(LLVector3(p.mV[0], p.mV[1], z)));
    setStatus("Going inside \"" + r->mName + "\".", false);
}

void WolfFloaterTunnels::updateButtons()
{
    if (!mNewBtn) return;
    const bool here = mRegionAtFetch.notNull() && !mCapUrl.empty();
    const bool can = here && mEdit && !s_jobBusy;
    const WolfTunnels::Record* picked = pickedTunnel();
    mNewBtn->setEnabled(can);
    mEditBtn->setEnabled(can && picked);
    mDelTunBtn->setEnabled(can && picked);
    mGoTunBtn->setEnabled(picked != nullptr && !s_jobBusy);
    const bool ed = mEditing && !s_jobBusy;
    const WolfToolTunnel::EPen pen = WolfToolTunnel::instanceExists() ? WolfToolTunnel::getInstance()->pen() : WolfToolTunnel::PEN_NONE;
    for (LLUICtrl* c : std::initializer_list<LLUICtrl*>{ mName, mDrawBtn, mUndoPointBtn, mKind, mW, mH, mStyle, mZ0, mZ1, mGroundBtn,
                                                          mCavernR, mPortals, mStopBtn })
        c->setEnabled(ed);
    mFinishBtn->setEnabled(ed && mRouteOpen);
    mCavernBtn->setEnabled(ed && mDesign.mP.size() >= 4);
    mUndoCavernBtn->setEnabled(ed && !mDesign.mCaverns.empty());
    mDrawBtn->setToggleState(pen == WolfToolTunnel::PEN_ROUTE);
    mCavernBtn->setToggleState(pen == WolfToolTunnel::PEN_CAVERN);
    mBuildBtn->setEnabled(ed && can && mProblem.empty());
    mBuildBtn->setLabel(!mEditId.empty() && findRecord(mEditId) && findRecord(mEditId)->mBuilt.isMap() ? std::string("Build again") : std::string("Build"));

    // Advanced: the boxes
    const bool canBox = mEdit && !mBusy && mRegionAtFetch.notNull() && !s_jobBusy;
    mSaveBtn->setEnabled(canBox && s_selected != 0);
    mDeleteBtn->setEnabled(canBox && s_selected != 0);
    for (LLSpinCtrl* s : mSpin) s->setEnabled(canBox && s_selected != 0);
    mGoBtn->setEnabled(s_selected != 0 && !mBusy);
    mRefreshBtn->setEnabled(!mBusy);
}

// ── the region's walkable boxes (Advanced) ──────────────────────────────────────────────────────────────────────────

void WolfFloaterTunnels::refresh()
{
    LLViewerRegion* rgn = gAgent.getRegion();
    const std::string url = rgn ? rgn->getCapability(CAP) : std::string();
    fetchRecords();
    mCapUrl = url;
    if (url.empty())
    {
        s_boxes.clear();
        mEdit = false;
        fillBoxList();
        setStatus(getString("str_need_cap"), true);
        updateButtons();
        return;
    }
    mBusy = true;
    updateButtons();
    setStatus("Reading this region's tunnels...", false);
    LLHandle<LLFloater> h = getHandle();
    const LLUUID region = rgn->getRegionID();
    LLCoros::instance().launch("WolfTunnelsGet", [h, url, region]() { getCoro(h, url, region); });
}

void WolfFloaterTunnels::post(const LLSD& body)
{
    LLViewerRegion* rgn = gAgent.getRegion();
    const std::string url = rgn ? rgn->getCapability(CAP) : std::string();
    if (url.empty() || !rgn || rgn->getRegionID() != mRegionAtFetch)
    {
        setStatus("You are on another region now - reading its tunnels first.", true);
        refresh();
        return;
    }
    mBusy = true;
    updateButtons();
    LLHandle<LLFloater> h = getHandle();
    const LLUUID region = rgn->getRegionID();
    LLCoros::instance().launch("WolfTunnelsPost", [h, url, body, region]() { postCoro(h, url, body, region); });
}

// static
void WolfFloaterTunnels::getCoro(LLHandle<LLFloater> h, std::string url, LLUUID region)
{
    auto adapter = std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfTunnels", LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
    LLSD result = adapter->getAndSuspend(request, url);
    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
    WolfFloaterTunnels* self = static_cast<WolfFloaterTunnels*>(h.get());
    if (!self) return;
    self->mBusy = false;
    if (!status)
    {
        self->setStatus("The region did not answer (" + status.toString() + "). Press Refresh (under Advanced) to try again.", true);
        self->updateButtons();
        return;
    }
    self->mRegionAtFetch = region;
    self->apply(result, false);
}

// static
void WolfFloaterTunnels::postCoro(LLHandle<LLFloater> h, std::string url, LLSD body, LLUUID region)
{
    auto adapter = std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfTunnels", LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
    LLSD result = adapter->postAndSuspend(request, url, body);
    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
    WolfFloaterTunnels* self = static_cast<WolfFloaterTunnels*>(h.get());
    if (!self) return;
    self->mBusy = false;
    if (!status)
    {
        self->setStatus("The change was not saved: the region did not answer (" + status.toString() + ").", true);
        self->updateButtons();
        return;
    }
    self->mRegionAtFetch = region;
    self->apply(result, true);
}

void WolfFloaterTunnels::apply(const LLSD& answer, bool from_post)
{
    if (!answer.isMap())
    {
        setStatus("The region's answer could not be read.", true);
        updateButtons();
        return;
    }
    mEdit = answer["edit"].asBoolean();
    mMax = answer["max"].asInteger();
    WolfTunnels::setFromAnswer(answer, mRegionAtFetch);   // the same store the terrain and the camera read
    fillBoxList();
    if (answer.has("message"))
        setStatus(answer["message"].asString(), true);
    else if (from_post)
        setStatus("Saved to the region. " + std::to_string(s_boxes.size()) + " walkable box(es) here.", false);
    else if (!mEditing && !s_jobBusy)
        setStatus(mEdit ? (s_records.empty() ? "No tunnels here yet. Press New tunnel and click its route on the ground."
                                             : "Pick a tunnel to change it, or press New tunnel.")
                        : getString("str_need_rights"), !mEdit);
    updateButtons();
}

void WolfFloaterTunnels::fillBoxList()
{
    if (!mBoxList) return;
    mBoxList->deleteAllItems();
    const LLFontGL* f = LLFontGL::getFontSansSerifSmall();
    const LLColor4 text = LLUIColorTable::instance().getColor("LabelTextColor");
    for (const WolfTunnels::Box& b : s_boxes)
    {
        LLScrollListItem::Params rp;
        rp.value(LLSD(b.mId));
        auto cell = [&](const char* col, const std::string& v)
        {
            LLScrollListCell::Params cp;
            cp.column(col);
            cp.value(v);
            cp.color(text);
            cp.font(f);
            rp.columns.add(cp);
        };
        cell("id", "#" + std::to_string(b.mId));
        cell("centre", m1(b.mCentre.mV[VX]) + ", " + m1(b.mCentre.mV[VY]) + ", " + m1(b.mCentre.mV[VZ]));
        cell("size", m1(b.mSize.mV[VX]) + " x " + m1(b.mSize.mV[VY]) + " x " + m1(b.mSize.mV[VZ]));
        cell("turn", m1(b.mYawDeg) + "\xC2\xB0");
        mBoxList->addRow(rp);
    }
    if (mInfo) mInfo->setText(mMax > 0 ? llformat("%d of %d boxes", (S32)s_boxes.size(), mMax) : std::string());
    selectBox(s_selected);
}

void WolfFloaterTunnels::selectBox(S32 id)
{
    s_selected = 0;
    for (const WolfTunnels::Box& b : s_boxes)
    {
        if (b.mId != id) continue;
        s_selected = id;
        mBoxList->selectByValue(LLSD(id));
        mBoxList->scrollToShowSelected();
        const F32 v[7] = { b.mCentre.mV[VX], b.mCentre.mV[VY], b.mCentre.mV[VZ], b.mSize.mV[VX], b.mSize.mV[VY], b.mSize.mV[VZ], b.mYawDeg };
        for (S32 i = 0; i < 7; ++i) mSpin[i]->setValue((F64)v[i]);
    }
    if (!s_selected) mBoxList->deselectAllItems(true);
    updateButtons();
}

void WolfFloaterTunnels::onSelectBox()
{
    selectBox(mBoxList->getSelectedValue().asInteger());
}

void WolfFloaterTunnels::onSaveBox()
{
    if (!s_selected) return;
    WolfTunnels::Box b;
    b.mId = s_selected;
    b.mCentre.set((F32)mSpin[0]->getValueF32(), (F32)mSpin[1]->getValueF32(), (F32)mSpin[2]->getValueF32());
    b.mSize.set((F32)mSpin[3]->getValueF32(), (F32)mSpin[4]->getValueF32(), (F32)mSpin[5]->getValueF32());
    b.mYawDeg = wrap360((F32)mSpin[6]->getValueF32());
    for (S32 i = 0; i < 3; ++i)
    {
        if (b.mSize.mV[i] < MIN_SIZE || b.mSize.mV[i] > MAX_SIZE)
        {
            setStatus(llformat("Every side is from %.1f to %.0f m.", MIN_SIZE, MAX_SIZE), true);
            return;
        }
    }
    LLSD body;
    body["op"] = "update";
    body["tunnel"] = boxToLLSD(b);
    setStatus("Saving the changes...", false);
    post(body);
}

void WolfFloaterTunnels::onDeleteBox()
{
    if (!s_selected) return;
    const S32 id = s_selected;
    LLHandle<LLFloater> h = getHandle();
    LLSD args;
    args["MESSAGE"] = llformat("Delete walkable box #%d? Anyone standing in it below the ground is lifted to the surface. (A tunnel built with this window keeps a record of its boxes: delete the tunnel instead to take all of them away.)", id);
    LLNotificationsUtil::add("GenericAlertYesCancel", args, LLSD(), [h, id](const LLSD& n, const LLSD& r)
    {
        if (LLNotificationsUtil::getSelectedOption(n, r) != 0) return;
        WolfFloaterTunnels* self = static_cast<WolfFloaterTunnels*>(h.get());
        if (!self) return;
        LLSD body;
        body["op"] = "remove";
        body["id"] = id;
        s_selected = 0;
        self->setStatus(llformat("Deleting walkable box #%d...", id), false);
        self->post(body);
    });
}

void WolfFloaterTunnels::onGoInsideBox()
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn || !s_selected) return;
    for (const WolfTunnels::Box& b : s_boxes)
    {
        if (b.mId != s_selected) continue;
        // a metre and a half above the box's floor (a box reaches FLOOR_BELOW under the tunnel's own floor)
        const LLVector3 p(b.mCentre.mV[VX], b.mCentre.mV[VY], b.mCentre.mV[VZ] - b.mSize.mV[VZ] * 0.5f + FLOOR_BELOW + 0.5f);
        gAgent.teleportViaLocation(rgn->getPosGlobalFromRegion(p));
        setStatus(llformat("Going inside walkable box #%d.", b.mId), false);
        return;
    }
}

// =====================================================================================================================
// the pens
// =====================================================================================================================

WolfToolTunnel::WolfToolTunnel() : LLTool(std::string("WolfTunnel")) {}

void WolfToolTunnel::begin(LLHandle<LLFloater> floater, EPen pen)
{
    cancel();
    mFloater = floater;
    mPen = pen;
    mActive = true;
    mHaveCursor = false;
    // held only while a pen is down: the base tool stays the pie, so every other click is the pie's (wolftooladventureplace.h)
    LLToolMgr::getInstance()->setTransientTool(this);
}

void WolfToolTunnel::cancel()
{
    const bool was = mActive;
    if (hasMouseCapture()) setMouseCapture(false);
    mActive = false;
    mPen = PEN_NONE;
    mHaveCursor = false;
    LLToolMgr* tm = LLToolMgr::getInstance();
    if (tm->getCurrentTool() == this) tm->clearTransientTool();
    if (was)
        if (WolfFloaterTunnels* f = static_cast<WolfFloaterTunnels*>(mFloater.get())) f->penStopped();
}

bool WolfToolTunnel::shown() const
{
    LLFloater* open = LLFloaterReg::findInstance("wolf_tunnels");
    return open && open->getVisible() && !open->isMinimized();
}

// Source: wolftooladventureplace.cpp WolfToolAdventurePlace::surface (llviewerwindow.h pickImmediate / mousePointOnLandGlobal).
bool WolfToolTunnel::surface(S32 x, S32 y, LLVector3& out) const
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return false;
    LLVector3d g;
    if (!gViewerWindow->mousePointOnLandGlobal(x, y, &g)) return false;
    const LLVector3 p = rgn->getPosRegionFromGlobal(g);
    const F32 w = rgn->getWidth();
    if (!finite3(p) || p.mV[VX] < 0.f || p.mV[VY] < 0.f || p.mV[VX] >= w || p.mV[VY] >= w) return false;
    out = p;
    return true;
}

bool WolfToolTunnel::handleAnyMouseClick(S32 x, S32 y, MASK mask, EMouseClickType clicktype, bool down)
{
    // The pen takes left clicks and double-clicks only; everything else is LLToolPie's (wolfcave.cpp WolfToolCave the same).
    if (!mActive || (clicktype != CLICK_LEFT && clicktype != CLICK_DOUBLELEFT)) return LLToolPie::getInstance()->handleAnyMouseClick(x, y, mask, clicktype, down);
    return LLTool::handleAnyMouseClick(x, y, mask, clicktype, down);
}

bool WolfToolTunnel::handleScrollWheel(S32 x, S32 y, S32 clicks) { return LLToolPie::getInstance()->handleScrollWheel(x, y, clicks); }

bool WolfToolTunnel::handleMouseDown(S32 x, S32 y, MASK mask)
{
    if (!mActive) return false;
    WolfFloaterTunnels* f = static_cast<WolfFloaterTunnels*>(mFloater.get());
    if (!f) { cancel(); return true; }
    LLVector3 p;
    if (!surface(x, y, p))
    {
        f->setStatus("Click on this region's ground.", true);
        return true;
    }
    if (mPen == PEN_ROUTE) f->addRoutePoint(p.mV[VX], p.mV[VY]);
    else if (mPen == PEN_CAVERN) f->addCavernAt(p.mV[VX], p.mV[VY]);
    return true;
}

bool WolfToolTunnel::handleMouseUp(S32 x, S32 y, MASK mask)
{
    return mActive;
}

bool WolfToolTunnel::handleDoubleClick(S32 x, S32 y, MASK mask)
{
    if (!mActive) return LLToolPie::getInstance()->handleDoubleClick(x, y, mask);
    WolfFloaterTunnels* f = static_cast<WolfFloaterTunnels*>(mFloater.get());
    if (f && mPen == PEN_ROUTE)
    {
        // The double-click's point (dropped when it is the point the first click just added), then the route is finished.
        LLVector3 p;
        if (surface(x, y, p)) f->addRoutePoint(p.mV[VX], p.mV[VY]);
        f->finishRoute();
    }
    return true;
}

bool WolfToolTunnel::handleHover(S32 x, S32 y, MASK mask)
{
    if (!shown()) { cancel(); return false; }
    LLVector3 p;
    mHaveCursor = surface(x, y, p);
    if (mHaveCursor) mCursor = p;
    gViewerWindow->setCursor(UI_CURSOR_CROSS);
    return true;
}

bool WolfToolTunnel::handleKey(KEY key, MASK mask)
{
    if (!mActive) return false;
    WolfFloaterTunnels* f = static_cast<WolfFloaterTunnels*>(mFloater.get());
    if (key == KEY_ESCAPE) { cancel(); if (f) f->setStatus("Stopped drawing - the drawing is kept.", false); return true; }
    if (f && mPen == PEN_ROUTE && key == KEY_RETURN) { f->finishRoute(); return true; }
    if (f && mPen == PEN_ROUTE && key == KEY_BACKSPACE) { f->undoPoint(); return true; }
    return false;
}

void WolfToolTunnel::renderIdle()
{
    if (!instanceExists()) return;
    WolfToolTunnel* t = getInstance();
    if (LLToolMgr::getInstance()->getCurrentTool() == t) return;   // drawn as the current tool
    t->render();
}

void WolfToolTunnel::drawIdle()
{
    if (!instanceExists()) return;
    WolfToolTunnel* t = getInstance();
    if (LLToolMgr::getInstance()->getCurrentTool() == t) return;
    t->draw();
}

// Source: wolftooladventureplace.cpp WolfToolAdventurePlace::render and wolfcave.cpp WolfToolCave::render (lines and
// see-through triangles through gGL; LLGLDepthTest(enabled, write); LLGLDisable(GL_CULL_FACE)).
void WolfToolTunnel::render()
{
    if (!shown()) return;
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return;
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    LLGLEnable blend(GL_BLEND);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    auto A = [&](const LLVector3& p) { return rgn->getPosAgentFromRegion(p); };
    auto draw = [&](const std::vector<LLVector3>& v, U8 mode)
    {
        if (v.empty()) return;
        gGL.begin(mode);
        for (const LLVector3& p : v) gGL.vertex3fv(A(p).mV);
        gGL.end();
    };

    // The region's walkable boxes (Advanced: "Show the boxes in the world").
    LLFloater* open = LLFloaterReg::findInstance("wolf_tunnels");
    LLCheckBoxCtrl* adv = open ? open->findChild<LLCheckBoxCtrl>("advanced_check") : nullptr;
    LLCheckBoxCtrl* show = open ? open->findChild<LLCheckBoxCtrl>("show_outlines") : nullptr;
    if (rgn->getRegionID() == s_boxesRegion && adv && adv->get() && (!show || show->get()))
    {
        std::vector<LLVector3> rest, sel;
        const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
        for (const WolfTunnels::Box& b : s_boxes)
        {
            if (dist_vec(rgn->getPosAgentFromRegion(b.mCentre), cam) > CULL_M + b.mSize.magVec()) continue;
            boxEdges(rgn, b, b.mId == s_selected ? sel : rest);
        }
        LLGLDepthTest depth(GL_FALSE, GL_FALSE);
        auto lines = [](const std::vector<LLVector3>& v, F32 width, F32 r, F32 g, F32 b, F32 a)
        {
            if (v.empty()) return;
            gGL.setLineWidth(width);
            gGL.color4f(r, g, b, a);
            gGL.begin(LLRender::LINES);
            for (const LLVector3& p : v) gGL.vertex3fv(p.mV);
            gGL.end();
        };
        lines(rest, 1.5f, 0.49f, 0.83f, 0.99f, 0.55f);   // ice: the region's boxes
        lines(sel, 3.f, 0.96f, 0.65f, 0.14f, 0.95f);     // amber: the one picked in the list
        gGL.setLineWidth(1.f);
    }

    // The tunnel being drawn: the preview tube (see-through, also through the ground so its depth shows), the route, the
    // caverns, the depth posts and where the roof leaves the ground.
    WolfFloaterTunnels* f = WolfFloaterTunnels::editing();
    if (!f) return;
    const WolfTunnels::Design& d = f->design();
    const WolfFloaterTunnels::Preview& pv = f->preview();
    const bool rough = d.mStyle == "rough";
    const F32 rr = rough ? 0.72f : 0.62f, rg = rough ? 0.58f : 0.76f, rb = rough ? 0.42f : 0.92f;
    {
        LLGLDisable cull(GL_CULL_FACE);
        {
            LLGLDepthTest depth(GL_FALSE, GL_FALSE);   // through the ground: faint
            gGL.color4f(rr, rg, rb, 0.10f);
            draw(pv.mRoof, LLRender::TRIANGLES);
            gGL.color4f(0.45f, 0.80f, 0.50f, 0.10f);
            draw(pv.mFloor, LLRender::TRIANGLES);
        }
        {
            LLGLDepthTest depth(GL_TRUE, GL_FALSE);    // in the open (the mouths): stronger
            gGL.color4f(rr, rg, rb, 0.30f);
            draw(pv.mRoof, LLRender::TRIANGLES);
            gGL.color4f(0.45f, 0.80f, 0.50f, 0.30f);
            draw(pv.mFloor, LLRender::TRIANGLES);
        }
    }
    std::vector<LLVector3> route, ground, posts, red, caverns;
    const LLSurface& land = rgn->getLand();
    const size_t n = d.mP.size() / 2;
    const Route rt(d.mP);
    // The route at its floor (orange) and over the ground (white), as line pairs, and a post at every point.
    LLVector3 prev_r, prev_g;
    for (size_t k = 0; k < n; ++k)
    {
        const F32 x = d.mP[2 * k], y = d.mP[2 * k + 1];
        const F32 z = d.mZ0 + (d.mZ1 - d.mZ0) * (rt.mLen > 0.f ? rt.mCum[k] / rt.mLen : 0.f);
        const LLVector3 r(x, y, z + 0.05f), g(x, y, land.resolveHeightRegion(x, y) + 0.1f);
        if (k > 0) { route.push_back(prev_r); route.push_back(r); ground.push_back(prev_g); ground.push_back(g); }
        posts.push_back(r);
        posts.push_back(g);
        prev_r = r;
        prev_g = g;
    }
    if (f->routeOpen() && mActive && mPen == PEN_ROUTE && mHaveCursor && n > 0)
    {
        ground.emplace_back(d.mP[2 * n - 2], d.mP[2 * n - 1], land.resolveHeightRegion(d.mP[2 * n - 2], d.mP[2 * n - 1]) + 0.1f);
        ground.emplace_back(mCursor.mV[VX], mCursor.mV[VY], mCursor.mV[VZ] + 0.1f);
    }
    for (size_t k = 0; k + 1 < pv.mPosts.size(); k += 2) { posts.push_back(pv.mPosts[k]); posts.push_back(pv.mPosts[k + 1]); }
    for (const LLVector3& b : pv.mBreakouts) { red.push_back(b); red.emplace_back(b.mV[VX], b.mV[VY], b.mV[VZ] + 3.f); }
    for (const WolfTunnels::Cavern& c : d.mCaverns)
    {
        const F32 z = d.mZ0 + (d.mZ1 - d.mZ0) * (rt.mLen > 0.f ? rt.nearest(c.mX, c.mY).second / rt.mLen : 0.f) + 0.05f;
        for (S32 i = 0; i < 36; ++i)
        {
            const F32 a0 = (F32)i / 36.f * F_TWO_PI, a1 = (F32)(i + 1) / 36.f * F_TWO_PI;
            caverns.emplace_back(c.mX + cosf(a0) * c.mR, c.mY + sinf(a0) * c.mR, z);
            caverns.emplace_back(c.mX + cosf(a1) * c.mR, c.mY + sinf(a1) * c.mR, z);
        }
    }
    {
        LLGLDepthTest depth(GL_FALSE);
        gGL.setLineWidth(2.f);
        gGL.color4f(1.f, 0.70f, 0.28f, 0.9f);
        draw(route, LLRender::LINES);
        gGL.color4f(1.f, 1.f, 1.f, 0.7f);
        draw(ground, LLRender::LINES);
        gGL.color4f(1.f, 1.f, 1.f, 0.45f);
        draw(posts, LLRender::LINES);
        gGL.color4f(0.55f, 0.80f, 1.f, 0.9f);
        draw(caverns, LLRender::LINES);
        gGL.setLineWidth(4.f);
        gGL.color4f(1.f, 0.2f, 0.15f, 1.f);
        draw(red, LLRender::LINES);
        gGL.setLineWidth(1.f);
    }
}

void WolfToolTunnel::draw()
{
    WolfFloaterTunnels* f = WolfFloaterTunnels::editing();
    LLViewerRegion* rgn = gAgent.getRegion();
    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    // The depth labels at the posts (wolfcave.cpp pickHandle: llviewercamera.h projectPosAgentToScreen).
    if (f && rgn)
    {
        const WolfFloaterTunnels::Preview& pv = f->preview();
        LLViewerCamera* cam = LLViewerCamera::getInstance();
        for (size_t k = 0; k < pv.mPostLabels.size() && 2 * k + 1 < pv.mPosts.size(); ++k)
        {
            LLCoordGL sp;
            if (!cam->projectPosAgentToScreen(rgn->getPosAgentFromRegion(pv.mPosts[2 * k]), sp, false)) continue;
            font->renderUTF8(pv.mPostLabels[k], 0, (F32)sp.mX, (F32)sp.mY + 4.f, LLColor4(1.f, 1.f, 1.f, 0.9f), LLFontGL::HCENTER, LLFontGL::BOTTOM,
                             LLFontGL::NORMAL, LLFontGL::DROP_SHADOW);
        }
    }
    if (!mActive) return;
    // what the pen wants, at the top of the world view (wolftooladventureplace.cpp draw)
    std::string h = mPen == PEN_CAVERN ? "Caverns: click on the route to put one there. Esc stops."
                                       : "Route: click points on the ground from mouth to mouth; double-click or Enter on the last one. Backspace undoes, Esc stops.";
    const F32 tw = (F32)font->getWidthF32(h);
    const F32 cx = (F32)gViewerWindow->getWorldViewWidthScaled() * 0.5f, ty = (F32)gViewerWindow->getWorldViewHeightScaled() - 60.f;
    WolfGameDraw::rect(cx - tw * 0.5f - 8.f, ty - 4.f, cx + tw * 0.5f + 8.f, ty + (F32)font->getLineHeight() + 4.f, LLColor4(0.f, 0.f, 0.f, 0.75f));
    font->renderUTF8(h, 0, cx, ty, LLColor4::white, LLFontGL::HCENTER, LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
}
