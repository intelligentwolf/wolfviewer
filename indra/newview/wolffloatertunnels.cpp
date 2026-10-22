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

    // [2026-10-22] See-through ground (WolfTunnels::setSeeThrough): the share of pixels left out is kept between these (Paul: "you
    // still need to know where it is" - never fully clear - and "SEMI TRANSPARENT" - never solid while on); the box holds every
    // height a terrain point can have (llsurface.cpp: a region's land is far inside +-10 km).
    const F32 SEE_THROUGH_MIN = 0.25f, SEE_THROUGH_MAX = 0.75f;
    const F32 SEE_THROUGH_HALF_HEIGHT = 10000.f;
    F32 s_seeThrough = 0.f;
    LLUUID s_seeThroughRegion;

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
        // [2026-10-22] the see-through ground goes off on any region change (teleport, crossing, logout); the window shows it
        s_seeThrough = 0.f;
        s_seeThroughRegion.setNull();
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

void WolfTunnels::setSeeThrough(F32 amount)
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (amount <= 0.f || !rgn || !std::isfinite(amount))
    {
        s_seeThrough = 0.f;
        s_seeThroughRegion.setNull();
        return;
    }
    s_seeThrough = llclamp(amount, SEE_THROUGH_MIN, SEE_THROUGH_MAX);
    s_seeThroughRegion = rgn->getRegionID();
}

bool WolfTunnels::seeThroughOn()
{
    LLViewerRegion* rgn = gAgent.getRegion();
    return s_seeThrough > 0.f && rgn && rgn->getRegionID() == s_seeThroughRegion;
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
    // hs[i].w = 0 for every tunnel box: no ground at all inside it (the hole). The see-through ground below sets its own w.
    F32 cy[TERRAIN_MAX * 4] = {}, hs[TERRAIN_MAX * 4] = {};
    S32 n = 0;
    // [2026-10-22] The Tunnels window's see-through ground takes the LAST slot, after the holes: the shaders take the first box a
    // fragment is in (terrainF.glsl / pbrterrainF.glsl wolfTunnelClip), so a hole stays a whole hole while the ground is see-through.
    const bool see_through = s_seeThrough > 0.f && regionp->getRegionID() == s_seeThroughRegion;
    const S32 slots = see_through ? TERRAIN_MAX - 1 : TERRAIN_MAX;
    // LLGLSLShader caches a single value per location (llglslshader.cpp uniform1i), so 0 again costs no GL call
    if (!s_boxes.empty() && regionp->getRegionID() == s_boxesRegion)
    {
        const F64 now = LLFrameTimer::getElapsedSeconds();
        if (s_touches.size() != s_boxes.size() || s_touchesRegion != regionp->getRegionID() || now - s_touchesAt > SURFACE_EVERY)
            refreshTouches(regionp);
        // the boxes the ground passes through, nearest the camera first
        const LLVector3 cam = regionp->getPosRegionFromAgent(LLViewerCamera::getInstance()->getOrigin());
        std::vector<std::pair<F32, size_t>> pick;
        for (size_t i = 0; i < s_boxes.size(); ++i)
            if (s_touches[i]) pick.emplace_back(dist_vec_squared(s_boxes[i].mCentre, cam), i);
        const S32 m = llmin((S32)pick.size(), slots - n);
        std::partial_sort(pick.begin(), pick.begin() + m, pick.end());
        for (S32 j = 0; j < m; ++j, ++n)
        {
            const Box& b = s_boxes[pick[j].second];
            cy[n * 4 + 0] = b.mCentre.mV[VX]; cy[n * 4 + 1] = b.mCentre.mV[VY]; cy[n * 4 + 2] = b.mCentre.mV[VZ];
            cy[n * 4 + 3] = b.mYawDeg * DEG_TO_RAD;
            hs[n * 4 + 0] = b.mSize.mV[VX] * 0.5f; hs[n * 4 + 1] = b.mSize.mV[VY] * 0.5f; hs[n * 4 + 2] = b.mSize.mV[VZ] * 0.5f;
        }
    }
    if (see_through)
    {
        // One box over the whole region and every height (its ground is all inside it; a neighbour region's terrain is drawn
        // with that region's own bindTerrain call and stays solid), w = the share of pixels left out.
        const F32 w = regionp->getWidth();
        cy[n * 4 + 0] = w * 0.5f; cy[n * 4 + 1] = w * 0.5f; cy[n * 4 + 2] = 0.f; cy[n * 4 + 3] = 0.f;
        hs[n * 4 + 0] = w * 0.5f; hs[n * 4 + 1] = w * 0.5f; hs[n * 4 + 2] = SEE_THROUGH_HALF_HEIGHT; hs[n * 4 + 3] = s_seeThrough;
        ++n;
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
    // [2026-10-22] v2: a floor height per point ("pz"; wolftunnel.rs TUNNEL_V = 2, TUNNEL_V1 = 1 still builds).
    const S32 TUNNEL_V = 2;
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
    // [2026-10-22] Drawing underground (the window's own choices): the preview follows a handle drag at most this often, seconds;
    // the drawing plane's grid (half size and line spacing, metres); a handle is picked within this many pixels (wolfcave.cpp
    // HANDLE_PICK_PX = 10).
    const F64 PREVIEW_DRAG_S = 0.1;
    const F32 PLANE_HALF_M = 10.f, PLANE_STEP_M = 2.f;
    const F32 HANDLE_PICK_PX = 10.f;
    // Below the ground at the camera by more than this, the camera counts as underground (clicks land on the drawing plane).
    const F32 UNDERGROUND_M = 0.2f;
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
        F32 floorT(F32 t) const { return floorOn(t); }
        /** Source: wolftunnel.rs Air::floor_t / piece_at (v2): straight between the heights of the two points round t. */
        F32 floorOn(F32 t) const
        {
            const size_t n = route.mPts.size();
            if (n < 2 || d.mZ.size() != n) return d.mZ.empty() ? 0.f : d.mZ[0];
            size_t k = 0;
            while (k + 2 < n && route.mCum[k + 1] < t) ++k;
            const F32 l = llmax(1e-6f, route.mCum[k + 1] - route.mCum[k]);
            const F32 u = llclamp((t - route.mCum[k]) / l, 0.f, 1.f);
            return d.mZ[k] + (d.mZ[k + 1] - d.mZ[k]) * u;
        }
        /** Source: wolftunnel.rs Air::piece_grade: the floor's rise per metre along piece k (point k to k + 1). */
        F32 pieceGrade(size_t k) const
        {
            return (d.mZ[k + 1] - d.mZ[k]) / llmax(1e-6f, route.mCum[k + 1] - route.mCum[k]);
        }
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
        // v2: a floor per point ("pz"); v1 (or a v2 without them): one straight grade from z0 to z1 by arc length, the same floor
        // (wolftunnel.rs Air::floor_t).
        const size_t n = out.mP.size() / 2;
        if (d["v"].asInteger() >= 2 && d["pz"].isArray() && d["pz"].size() == (S32)n)
        {
            for (const LLSD& z : llsd::inArray(d["pz"])) out.mZ.push_back((F32)z.asReal());
        }
        else
        {
            const F32 z0 = (F32)d["z0"].asReal(), z1 = (F32)d["z1"].asReal();
            const Route r(out.mP);
            for (size_t k = 0; k < n; ++k) out.mZ.push_back(k + 1 == n ? z1 : z0 + (z1 - z0) * (r.mLen > 0.f ? r.mCum[k] / r.mLen : 0.f));
        }
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
        LLSD pz = LLSD::emptyArray();
        for (F32 z : d.mZ) pz.append(r2(z));
        out["pz"] = pz;
        out["z0"] = d.mZ.empty() ? 0.f : r2(d.mZ.front());
        out["z1"] = d.mZ.empty() ? 0.f : r2(d.mZ.back());
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
F32 WolfTunnels::floorAt(const Design& d, F32 t) { return Air(d).floorOn(t); }

// Source: wolftunnel.rs validate_design - the same checks, so Build is only offered for what will build.
// [2026-10-22 easy] The words name the points and say what to do (Paul could not tell why Build stayed grey).
std::string WolfTunnels::designProblem(const Design& d, F32 region_w)
{
    const size_t n = d.mP.size() / 2;
    if (n < 2) return n == 0 ? "Click on the ground where the tunnel starts." : "Click where the tunnel ends (a tunnel needs two points at least).";
    if (d.mW < W_MIN || d.mW > W_MAX) return llformat("The tunnel must be %.0f to %.0f m wide (More options).", W_MIN, W_MAX);
    if (d.mH < H_MIN || d.mH > H_MAX) return llformat("The tunnel must be %.1f to %.0f m high (More options).", H_MIN, H_MAX);
    for (size_t k = 0; k < n; ++k)
        if (d.mP[2 * k] < 0.f || d.mP[2 * k] > region_w || d.mP[2 * k + 1] < 0.f || d.mP[2 * k + 1] > region_w)
            return llformat("Point %d is outside this region - drag it back in.", (S32)k + 1);
    const Route route(d.mP);
    F32 first = 0.f, last = 0.f;
    for (size_t k = 1; k < n; ++k)
    {
        const F32 l = route.mCum[k] - route.mCum[k - 1];
        if (l < SEG_MIN_M) return llformat("Points %d and %d are less than 0.5 m apart - press Undo point or drag one away.", (S32)k, (S32)k + 1);
        if (k == 1) first = l;
        last = l;
    }
    if (first < d.mW)
        return llformat("The first piece (points 1 to 2) is %.1f m; it must be at least %.1f m, as wide as the tunnel. Drag point 2 further away.", first, d.mW);
    if (last < d.mW)
        return llformat("The last piece (points %d to %d) is %.1f m; it must be at least %.1f m, as wide as the tunnel. Click further on, or drag point %d further out.",
                        (S32)n - 1, (S32)n, last, d.mW, (S32)n);
    // Source: wolftunnel.rs validate_design (v2): a height per point, each in range, no piece steeper than 1 in 1.
    if (d.mZ.size() != n) return "Every point of the route needs its floor height - draw it again.";
    for (F32 z : d.mZ) if (!std::isfinite(z) || z < Z_MIN || z > Z_MAX) return "The floor heights must be -90 to 4000 m.";
    for (size_t k = 1; k < n; ++k)
    {
        if (fabsf(d.mZ[k] - d.mZ[k - 1]) / llmax(1e-6f, route.mCum[k] - route.mCum[k - 1]) > GRADE_MAX)
            return llformat("Too steep between points %d and %d (more than 1 in 1) - drag them further apart, or bring their depths closer (More options).", (S32)k, (S32)k + 1);
    }
    for (size_t i = 0; i < d.mCaverns.size(); ++i)
    {
        const Cavern& c = d.mCaverns[i];
        if (c.mR < CAVERN_R_MIN || c.mR > CAVERN_R_MAX) return llformat("Cavern %d's radius must be 3 to 30 m.", (S32)i + 1);
        if (c.mH < CAVERN_H_MIN || c.mH > CAVERN_H_MAX) return llformat("Cavern %d's height must be 3 to 25 m.", (S32)i + 1);
        const std::pair<F32, F32> nt = route.nearest(c.mX, c.mY);
        if (nt.first > d.mW * 0.5f) return llformat("Cavern %d is not on the route - Undo cavern and put it on the tunnel.", (S32)i + 1);
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
    mHint = getChild<LLTextBox>("hint");
    mMoreCheck = getChild<LLCheckBoxCtrl>("more_check");
    mMore = getChild<LLPanel>("more_panel");
    mDepth = getChild<LLSpinCtrl>("tun_depth");
    mMouths = getChild<LLCheckBoxCtrl>("tun_mouths");
    mSeeThrough = getChild<LLCheckBoxCtrl>("see_through");
    mSeeThroughAmount = getChild<LLSliderCtrl>("see_through_amount");
    mPointLbl = getChild<LLTextBox>("pt_lbl");
    mPointDepth = getChild<LLSpinCtrl>("pt_depth");
    mPointGroundBtn = getChild<LLButton>("pt_ground");
    mPointRemoveBtn = getChild<LLButton>("pt_remove");
    mCavernBtn = getChild<LLButton>("cavern_btn");
    mUndoCavernBtn = getChild<LLButton>("undo_cavern_btn");
    mBuildBtn = getChild<LLButton>("build_btn");
    mStopBtn = getChild<LLButton>("cancel_edit_btn");
    mKind = getChild<LLComboBox>("tun_kind");
    mW = getChild<LLSpinCtrl>("tun_w");
    mH = getChild<LLSpinCtrl>("tun_h");
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
    mDepth->setValue(llclamp(gSavedSettings.getF32("WolfTunnelDepth"), 0.f, 300.f));
    mDepth->setCommitCallback([this](LLUICtrl*, const LLSD&) { onDepth(); });
    mMouths->set(gSavedSettings.getBOOL("WolfTunnelMouthsAtGround"));
    mMouths->setCommitCallback([this](LLUICtrl*, const LLSD&) { onMouths(); });
    // The box is not saved (on each time the window opens); the amount is (WolfTunnelSeeThrough, a share 0.25..0.75 = 25..75 %).
    mSeeThroughAmount->setValue(llround(llclamp(gSavedSettings.getF32("WolfTunnelSeeThrough"), 0.25f, 0.75f) * 100.f));
    mSeeThrough->setCommitCallback([this](LLUICtrl*, const LLSD&) { applySeeThrough(); });
    mSeeThroughAmount->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        gSavedSettings.setF32("WolfTunnelSeeThrough", llclamp((F32)mSeeThroughAmount->getValueF32() / 100.f, 0.25f, 0.75f));
        applySeeThrough();
    });
    mPointDepth->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPointDepth(); });
    mPointGroundBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onPointGround(); });
    mPointRemoveBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { removePickedPoint(); });
    mUndoCavernBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onUndoCavern(); });
    mBuildBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onBuild(); });
    mStopBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onStopEditing(); });
    mKind->setCommitCallback([this](LLUICtrl*, const LLSD&) { onKind(); });
    mW->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSize(); });
    mH->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSize(); });
    mStyle->setCommitCallback([this](LLUICtrl*, const LLSD&) { onStyle(); });
    mPortals->setCommitCallback([this](LLUICtrl*, const LLSD&) { mDesign.mPortals = mPortals->get(); designChanged(); });
    mAdvancedCheck->setCommitCallback([this](LLUICtrl*, const LLSD&) { onAdvanced(); });
    mMoreCheck->setCommitCallback([this](LLUICtrl*, const LLSD&) { relayout(); });

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
    relayout();
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
    mSeeThrough->set(true);   // on each time the window opens (Paul: "just while you use the builder")
    applySeeThrough();
    refresh();
}

void WolfFloaterTunnels::onClose(bool app_quitting)
{
    if (WolfToolTunnel::instanceExists()) WolfToolTunnel::getInstance()->cancel();
    mRouteOpen = false;
    WolfTunnels::setSeeThrough(0.f);   // the ground is solid again the moment the window closes
}

void WolfFloaterTunnels::applySeeThrough()
{
    const bool on = mSeeThrough->get() && WolfGrid::isOnWolfTerritories();
    mSeeThroughAmount->setEnabled(on);
    WolfTunnels::setSeeThrough(on ? llclamp((F32)mSeeThroughAmount->getValueF32() / 100.f, 0.25f, 0.75f) : 0.f);
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
    // a region change turned the see-through ground off (WolfTunnels onRegionChanged): the box shows it
    if (mSeeThrough->get() && !WolfTunnels::seeThroughOn()) { mSeeThrough->set(false); mSeeThroughAmount->setEnabled(false); }
    // a handle drag: the preview follows at most every PREVIEW_DRAG_S (a long route's preview is not free)
    if (mPreviewDirty && LLFrameTimer::getElapsedSeconds() - mPreviewAt >= PREVIEW_DRAG_S) designChanged();
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
    relayout();
}

// [2026-10-22 easy] Paul: "part of the floater I can't see". The window was 730 high (958 with Advanced). Now: 364 with More
// options closed, + 200 with it open, + 228 with the walkable boxes (floater_wolf_tunnels.xml: floater height, more_panel top
// 362 height 200, advanced_panel top 562 height 228). Build tunnel sits above More options, so opening it never moves Build.
void WolfFloaterTunnels::relayout()
{
    const S32 BASE_H = 364, MORE_H = 200, BOXES_H = 228;
    const bool more = mMoreCheck->get();
    const bool boxes = more && mAdvancedCheck->get();
    mMore->setVisible(more);
    mAdvanced->setVisible(boxes);
    const S32 want = BASE_H + (more ? MORE_H : 0) + (boxes ? BOXES_H : 0);
    if (getRect().getHeight() != want)
    {
        // Grow or shrink downwards, keeping the window's top where it is.
        LLRect r = getRect();
        r.mBottom = r.mTop - want;
        setShape(r);
    }
    // Keep it on screen: when the bottom went below the floaters' area (llfloater.h LLFloaterView::getSnapRect, in the same
    // coordinates as this window's rect), lift it, but never the title bar above that area's top.
    if (gFloaterView && getParent() == gFloaterView)
    {
        const LLRect snap = gFloaterView->getSnapRect();
        const LLRect r = getRect();
        if (r.mBottom < snap.mBottom)
        {
            const S32 up = llmin(snap.mBottom - r.mBottom, llmax(0, snap.mTop - r.mTop));
            if (up > 0) translate(0, up);
        }
    }
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
        mPicked = -1;
        mEndIsMouth = false;
        mPreviewDirty = false;
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
    // A last point on the ground is a mouth: drawing on from it sends it to the depth (addRoutePoint).
    if (LLViewerRegion* rgn = gAgent.getRegion())
    {
        const size_t n = mDesign.mP.size() / 2;
        mEndIsMouth = n >= 2 && mDesign.mZ.size() == n
                      && fabsf(mDesign.mZ[n - 1] - rgn->getLand().resolveHeightRegion(mDesign.mP[2 * n - 2], mDesign.mP[2 * n - 1])) < 0.05f;
    }
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
    mPortals->set(mDesign.mPortals);
    showPoint();
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

// ── [2026-10-22] drawing underground: the depth, the mouths and the picked point ─────────────────────────────────────

void WolfFloaterTunnels::onDepth()
{
    gSavedSettings.setF32("WolfTunnelDepth", llclamp((F32)mDepth->getValueF32(), 0.f, 300.f));
}

// [2026-10-22 easy] Why Paul's Build stayed grey: every new point went straight to the full depth (WolfTunnelDepth, 8 m) under
// a mouth on the ground, so a second point closer than the depth made a piece steeper than 1 in 1 (designProblem, wolftunnel.rs
// validate_design GRADE_MAX), and the reason was only in a text box he could not see. Now the drawing keeps each new point
// within the generator's own "steep" slope of the point before (wolftunnel.rs warnings(): RAIL_GRADE_WARN for rail, else
// WALK_GRADE_WARN), so a drawn route never trips that check; the depth is how deep it goes at most. Points moved by hand
// (handles, the point's depth) are not eased: what the user sets is kept, and the line under Build says if it is too steep.
F32 WolfFloaterTunnels::easeGrade() const
{
    // Source: wolftunnel.rs warnings() `let limit = if rail { RAIL_GRADE_WARN } else { WALK_GRADE_WARN };`
    return mDesign.mKind == "rail" ? RAIL_GRADE_WARN : WALK_GRADE_WARN;
}

F32 WolfFloaterTunnels::eased(F32 target, F32 from_z, F32 len) const
{
    const F32 most = easeGrade() * llmax(0.f, len);
    return llclamp(target, from_z - most, from_z + most);
}

F32 WolfFloaterTunnels::newPointFloor(F32 x, F32 y, F32 ground) const
{
    const F32 depth = llclamp((F32)mDepth->getValueF32(), 0.f, 300.f);
    // With the mouths on the ground every new point is the far mouth, on the ground; the point before it goes down to the depth
    // when it is added (addRoutePoint). The first point is the near mouth.
    if (mMouths->get()) return r2(ground);
    const size_t n = mDesign.mP.size() / 2;
    F32 z = ground - depth;
    if (n > 0 && mDesign.mZ.size() == n)
        z = eased(z, mDesign.mZ[n - 1], sqrtf(powf(x - mDesign.mP[2 * n - 2], 2.f) + powf(y - mDesign.mP[2 * n - 1], 2.f)));
    return r2(llclamp(z, Z_MIN, Z_MAX));
}

F32 WolfFloaterTunnels::planeHeight() const
{
    const size_t n = mDesign.mP.size() / 2;
    if (mPicked >= 0 && (size_t)mPicked < n && mDesign.mZ.size() == n) return mDesign.mZ[mPicked];
    if (n > 0 && mDesign.mZ.size() == n) return mDesign.mZ[n - 1];
    // Nothing drawn yet: about where the avatar stands (its position is about a metre over its feet).
    LLViewerRegion* rgn = gAgent.getRegion();
    return rgn ? rgn->getPosRegionFromAgent(gAgent.getPositionAgent()).mV[VZ] - 1.f : 0.f;
}

void WolfFloaterTunnels::applyMouths(bool end)
{
    LLViewerRegion* rgn = gAgent.getRegion();
    const size_t n = mDesign.mP.size() / 2;
    if (!rgn || n == 0 || mDesign.mZ.size() != n) return;
    const LLSurface& land = rgn->getLand();
    const F32 depth = mMouths->get() ? 0.f : llclamp((F32)mDepth->getValueF32(), 0.f, 300.f);
    auto put = [&](size_t k) { mDesign.mZ[k] = r2(llclamp(land.resolveHeightRegion(mDesign.mP[2 * k], mDesign.mP[2 * k + 1]) - depth, Z_MIN, Z_MAX)); };
    put(0);
    if (end && n >= 2) { put(n - 1); mEndIsMouth = mMouths->get(); }
    // The point before a far mouth on the ground keeps a gentle slope up to it (as addRoutePoint does).
    if (end && n >= 3 && mMouths->get())
    {
        const F32 len = sqrtf(powf(mDesign.mP[2 * n - 2] - mDesign.mP[2 * n - 4], 2.f) + powf(mDesign.mP[2 * n - 1] - mDesign.mP[2 * n - 3], 2.f));
        mDesign.mZ[n - 2] = r2(llclamp(eased(mDesign.mZ[n - 2], mDesign.mZ[n - 1], len), Z_MIN, Z_MAX));
    }
}

void WolfFloaterTunnels::onMouths()
{
    gSavedSettings.setBOOL("WolfTunnelMouthsAtGround", mMouths->get());
    if (!mEditing) return;
    applyMouths(true);   // [2026-10-22 easy] the far end is always the last point (there is no Finish step any more)
    designChanged();
}

void WolfFloaterTunnels::pickPoint(S32 k)
{
    mPicked = (k >= 0 && (size_t)k < mDesign.mP.size() / 2) ? k : -1;
    showPoint();
    updateButtons();
}

void WolfFloaterTunnels::showPoint()
{
    if (!mPointLbl) return;
    const size_t n = mDesign.mP.size() / 2;
    if (mPicked >= (S32)n) mPicked = -1;
    LLViewerRegion* rgn = gAgent.getRegion();
    if (mPicked < 0 || !rgn || mDesign.mZ.size() != n)
    {
        mPointLbl->setText(n > 0 ? std::string("Pick a point") : std::string("No points yet"));
        mPointDepth->setValue(0.f);
        return;
    }
    mPointLbl->setText(llformat("Point %d of %d", mPicked + 1, (S32)n));
    const F32 g = rgn->getLand().resolveHeightRegion(mDesign.mP[2 * mPicked], mDesign.mP[2 * mPicked + 1]);
    mPointDepth->setValue(r2(g - mDesign.mZ[mPicked]));
}

void WolfFloaterTunnels::onPointDepth()
{
    LLViewerRegion* rgn = gAgent.getRegion();
    const size_t n = mDesign.mP.size() / 2;
    if (!rgn || mPicked < 0 || (size_t)mPicked >= n || mDesign.mZ.size() != n) return;
    const F32 g = rgn->getLand().resolveHeightRegion(mDesign.mP[2 * mPicked], mDesign.mP[2 * mPicked + 1]);
    mDesign.mZ[mPicked] = r2(llclamp(g - (F32)mPointDepth->getValueF32(), Z_MIN, Z_MAX));
    if ((size_t)mPicked + 1 == n) mEndIsMouth = false;   // set by hand: no longer the mouths' rule's
    designChanged();
}

void WolfFloaterTunnels::onPointGround()
{
    mPointDepth->setValue(0.f);
    onPointDepth();
}

void WolfFloaterTunnels::removePickedPoint()
{
    const size_t n = mDesign.mP.size() / 2;
    if (mPicked < 0 || (size_t)mPicked >= n || mDesign.mZ.size() != n) return;
    mDesign.mP.erase(mDesign.mP.begin() + 2 * mPicked, mDesign.mP.begin() + 2 * mPicked + 2);
    mDesign.mZ.erase(mDesign.mZ.begin() + mPicked);
    setStatus(llformat("Point %d taken out of the route.", mPicked + 1), false);
    mPicked = -1;
    designChanged();
}

void WolfFloaterTunnels::setPoint(S32 k, F32 x, F32 y, F32 z, bool done)
{
    const size_t n = mDesign.mP.size() / 2;
    if (!mEditing || k < 0 || (size_t)k >= n || mDesign.mZ.size() != n) return;
    mDesign.mP[2 * k] = r2(x);
    mDesign.mP[2 * k + 1] = r2(y);
    mDesign.mZ[k] = r2(llclamp(z, Z_MIN, Z_MAX));
    if ((size_t)k + 1 == n) mEndIsMouth = false;
    if (done) designChanged();
    else { mPreviewDirty = true; showPoint(); }
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
    // [2026-10-22 easy] the hint line at the top says what to click (nextStep); the status line is for what happened
    setStatus(pen == WolfToolTunnel::PEN_CAVERN ? std::string("Click on the tunnel's route to put a cavern there. Esc stops.") : std::string(), false);
    updateButtons();
}

void WolfFloaterTunnels::penStopped()
{
    mRouteOpen = false;
    updateButtons();
}

void WolfFloaterTunnels::addRoutePoint(F32 x, F32 y, F32 z, bool on_plane)
{
    if (!mEditing) return;
    const size_t n = mDesign.mP.size() / 2;
    const F32 len = n > 0 ? sqrtf(powf(x - mDesign.mP[2 * n - 2], 2.f) + powf(y - mDesign.mP[2 * n - 1], 2.f)) : 0.f;
    if (n > 0 && len < SEG_MIN_M) return;   // a double-click's second press
    // [2026-10-22 easy] designProblem refuses a first piece shorter than the tunnel is wide (wolftunnel.rs validate_design):
    // say so at the click, where it can be put right, instead of greying Build later.
    if (n == 1 && len < mDesign.mW)
    {
        setStatus(llformat("Click a little further from point 1: the first piece must be at least %.1f m long (the tunnel is %.1f m wide). It is %.1f m.",
                           mDesign.mW, mDesign.mW, len), true);
        return;
    }
    if (mDesign.mZ.size() != n) mDesign.mZ.resize(n, z);
    const F32 new_z = r2(llclamp(z, Z_MIN, Z_MAX));
    // The far mouth so far becomes a point inside: down to the depth, gently from the point before it and up to the new point.
    LLViewerRegion* rgn = gAgent.getRegion();
    if (mEndIsMouth && n >= 2 && rgn)
    {
        const F32 g = rgn->getLand().resolveHeightRegion(mDesign.mP[2 * n - 2], mDesign.mP[2 * n - 1]);
        const F32 back = sqrtf(powf(mDesign.mP[2 * n - 2] - mDesign.mP[2 * n - 4], 2.f) + powf(mDesign.mP[2 * n - 1] - mDesign.mP[2 * n - 3], 2.f));
        F32 zi = eased(g - llclamp((F32)mDepth->getValueF32(), 0.f, 300.f), mDesign.mZ[n - 2], back);
        zi = eased(zi, new_z, len);
        mDesign.mZ[n - 1] = r2(llclamp(zi, Z_MIN, Z_MAX));
    }
    // The new point is the far mouth when the mouths are on the ground (not a click on the drawing plane underground).
    mEndIsMouth = n >= 1 && mMouths->get() && !on_plane;
    mDesign.mP.push_back(r2(x));
    mDesign.mP.push_back(r2(y));
    mDesign.mZ.push_back(new_z);
    mPicked = (S32)n;   // the new point is the picked one (its depth shows in the Point row)
    setStatus(std::string(), false);
    designChanged();
}

void WolfFloaterTunnels::finishRoute()
{
    if (WolfToolTunnel::instanceExists() && WolfToolTunnel::getInstance()->pen() == WolfToolTunnel::PEN_ROUTE) WolfToolTunnel::getInstance()->cancel();
    mRouteOpen = false;
    // [2026-10-22 easy] Nothing to finish any more: the last point already is the far mouth (addRoutePoint). This only puts the
    // pen away (Enter, a double-click, or the Draw button again).
    if (mDesign.mP.size() < 4) setStatus("A tunnel needs two points at least: click where it starts and where it ends.", true);
    else setStatus("Pen put away. Press Build tunnel, or Draw / move points to change the route.", false);
    designChanged();
}

void WolfFloaterTunnels::undoPoint()
{
    if (mDesign.mP.size() < 2) return;
    mDesign.mP.resize(mDesign.mP.size() - 2);
    mDesign.mZ.resize(mDesign.mP.size() / 2);
    mEndIsMouth = false;
    // A finished route keeps a far mouth: the point now last goes on the ground when the mouths are.
    if (mMouths->get() && mDesign.mP.size() >= 4) applyMouths(true);   // [2026-10-22 easy] the far end is always the last point
    if (mPicked >= (S32)(mDesign.mP.size() / 2)) mPicked = -1;
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
    mPreviewDirty = false;
    mPreviewAt = LLFrameTimer::getElapsedSeconds();
    if (mDesign.mZ.size() != n) mDesign.mZ.resize(n, mDesign.mZ.empty() ? 0.f : mDesign.mZ.back());   // (kept equal everywhere; a guard)
    if (mPicked >= (S32)n) mPicked = -1;
    showPoint();
    mProblem = rgn ? WolfTunnels::designProblem(mDesign, rgn->getWidth()) : std::string("You are not in a region.");
    if (n >= 2 && rgn)
    {
        const Air air(mDesign);
        const F32 len = air.route.mLen;
        // [2026-10-22] Every point has its own floor: the line gives the floor's lowest and highest and its steepest piece.
        F32 lo = mDesign.mZ[0], hi = mDesign.mZ[0], steep = 0.f;
        size_t steep_k = 0;
        for (size_t k = 0; k < n; ++k) { lo = llmin(lo, mDesign.mZ[k]); hi = llmax(hi, mDesign.mZ[k]); }
        for (size_t k = 0; k + 1 < n; ++k) if (fabsf(air.pieceGrade(k)) > fabsf(steep)) { steep = air.pieceGrade(k); steep_k = k; }
        mGradeInfo->setText(fabsf(steep) < 1e-4f ? llformat("%.0f m long, level floor at %.1f m", len, lo)
                            : llformat("%.0f m long, floor %.1f to %.1f m; steepest %.1f%% (1 in %.0f) between points %d and %d", len, lo, hi,
                                       fabsf(steep) * 100.f, 1.f / fabsf(steep), (S32)steep_k + 1, (S32)steep_k + 2));
        // The preview is drawn for any route the shape can be worked out for: every piece 0.5 m or more, the size in range
        // (the checks before the pieces' lengths in designProblem); a too-short end piece, a steep piece or a cavern is drawn.
        bool previewable = mDesign.mW >= W_MIN && mDesign.mW <= W_MAX && mDesign.mH >= H_MIN && mDesign.mH <= H_MAX;
        for (size_t k = 1; k < n && previewable; ++k)
            if (air.route.mCum[k] - air.route.mCum[k - 1] < SEG_MIN_M) previewable = false;
        for (F32 v : mDesign.mP) if (v < 0.f || v > rgn->getWidth()) previewable = false;   // (inside the region only)
        if (previewable)
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
            const F32 ends[2][3] = { { mDesign.mP[0], mDesign.mP[1], mDesign.mZ[0] }, { mDesign.mP[2 * n - 2], mDesign.mP[2 * n - 1], mDesign.mZ[n - 1] } };
            for (S32 k = 0; k < 2; ++k)
            {
                const char* name = k == 0 ? "start" : "end";
                if (!loaded(ends[k][0], ends[k][1])) continue;
                const F32 g = land.resolveHeightRegion(ends[k][0], ends[k][1]), z = ends[k][2];
                if (z < water - 0.1f) mPreview.mWarnings.push_back(llformat("The %s mouth's floor is %.1f m under the water line.", name, water - z));
                if (g - z > air.top()) mPreview.mWarnings.push_back(llformat("The %s mouth is buried: the ground there is %.1f m above the tunnel floor. Move that end or raise its floor.", name, g - z));
                else if (z - g > 1.f) mPreview.mWarnings.push_back(llformat("The %s mouth's floor is %.1f m above the ground - people will need a step or a ramp there.", name, z - g));
            }
            // Source: wolftunnel.rs warnings() (v2): per piece, named by its points.
            const bool rail = mDesign.mKind == "rail";
            for (size_t k = 0; k + 1 < n; ++k)
            {
                const F32 g = air.pieceGrade(k);
                if (fabsf(g) > (rail ? RAIL_GRADE_WARN : WALK_GRADE_WARN))
                    mPreview.mWarnings.push_back(llformat("Between points %d and %d the floor %s %.1f%% - steep for %s. Move the points apart or bring their depths closer.",
                                                          (S32)k + 1, (S32)k + 2, g > 0.f ? "climbs" : "falls", fabsf(g) * 100.f, rail ? "a train" : "walking"));
            }
        }
    }
    else
    {
        mGradeInfo->setText(std::string());
    }
    updateButtons();   // the line under Build tunnel (the blocker or the warnings) and the hint follow
}

void WolfFloaterTunnels::onBuild()
{
    if (!mEditing || s_jobBusy) return;
    LLViewerRegion* rgn = gAgent.getRegion();
    const std::string block = buildBlocker();
    if (!rgn || !block.empty()) { setStatus("Cannot build yet: " + block, true); return; }
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
    const F32 z = WolfTunnels::floorAt(r->mDesign, t) + 1.f;
    gAgent.teleportViaLocation(rgn->getPosGlobalFromRegion(LLVector3(p.mV[0], p.mV[1], z)));
    setStatus("Going inside \"" + r->mName + "\".", false);
}

// [2026-10-22 easy] Every condition that greys Build tunnel, in the order the user can do something about it (onBuild checks
// the same before it starts).
std::string WolfFloaterTunnels::buildBlocker() const
{
    if (!mEditing) return "Press New tunnel first.";
    if (s_jobBusy) return "A build is running - wait for it to finish.";
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return "You are not in a region.";
    if (mBusy && mRegionAtFetch.isNull()) return "Reading this region's tunnels - a moment.";
    if (mCapUrl.empty() || mRegionAtFetch != rgn->getRegionID()) return getString("str_need_cap");
    if (!mEdit) return getString("str_need_rights");
    return mProblem;
}

std::string WolfFloaterTunnels::nextStep() const
{
    if (!mEditing)
    {
        if (s_jobBusy) return "Building - see the line below.";
        const bool here = mRegionAtFetch.notNull() && !mCapUrl.empty();
        if (mBusy && !here) return "Reading this region's tunnels...";
        if (!here) return "Tunnels cannot be made on this region - the reason is below.";
        if (!mEdit) return "You can look at this region's tunnels; only the region owner and estate managers can make them.";
        return s_records.empty() ? "Press New tunnel, then click on the ground where the tunnel starts."
                                 : "Press New tunnel to draw one, or pick a tunnel in the list.";
    }
    const size_t n = mDesign.mP.size() / 2;
    const bool pen = WolfToolTunnel::instanceExists() && WolfToolTunnel::getInstance()->pen() == WolfToolTunnel::PEN_ROUTE;
    if (!pen && n < 2) return "Press Draw / move points, then click on the ground where the tunnel starts.";
    if (n == 0) return "Click on the ground where the tunnel starts.";
    if (n == 1) return "Now click where it ends (or where it bends first).";
    if (pen) return "Click to add more bends, drag a point's box to move it. When it looks right, press Build tunnel.";
    return "Press Build tunnel. To change the route, press Draw / move points.";
}

void WolfFloaterTunnels::updateButtons()
{
    if (!mNewBtn || !mHint) return;
    const bool here = mRegionAtFetch.notNull() && !mCapUrl.empty();
    const bool can = here && mEdit && !s_jobBusy;
    const WolfTunnels::Record* picked = pickedTunnel();
    mNewBtn->setEnabled(can);
    mEditBtn->setEnabled(can && picked);
    mDelTunBtn->setEnabled(can && picked);
    mGoTunBtn->setEnabled(picked != nullptr && !s_jobBusy);
    const bool ed = mEditing && !s_jobBusy;
    const WolfToolTunnel::EPen pen = WolfToolTunnel::instanceExists() ? WolfToolTunnel::getInstance()->pen() : WolfToolTunnel::PEN_NONE;
    for (LLUICtrl* c : std::initializer_list<LLUICtrl*>{ mName, mDrawBtn, mUndoPointBtn, mKind, mW, mH, mStyle, mDepth, mMouths,
                                                          mCavernR, mPortals, mStopBtn })
        c->setEnabled(ed);
    const bool picked_pt = ed && mPicked >= 0 && (size_t)mPicked < mDesign.mP.size() / 2;
    mPointDepth->setEnabled(picked_pt);
    mPointGroundBtn->setEnabled(picked_pt);
    mPointRemoveBtn->setEnabled(picked_pt);
    mCavernBtn->setEnabled(ed && mDesign.mP.size() >= 4);
    mUndoCavernBtn->setEnabled(ed && !mDesign.mCaverns.empty());
    mDrawBtn->setToggleState(pen == WolfToolTunnel::PEN_ROUTE);
    mCavernBtn->setToggleState(pen == WolfToolTunnel::PEN_CAVERN);
    // [2026-10-22 easy] Build tunnel is always shown; when it cannot be pressed the line under it says why (buildBlocker), in
    // the caution colour. When it can, the line gives the preview's warnings (which do not stop a build), or "Ready".
    const std::string block = buildBlocker();
    mBuildBtn->setEnabled(mEditing && block.empty());
    mBuildBtn->setLabel(!mEditId.empty() && findRecord(mEditId) && findRecord(mEditId)->mBuilt.isMap() ? std::string("Build again") : std::string("Build tunnel"));
    std::string line;
    bool caution = false;
    if (mEditing && !block.empty()) { line = "Cannot build yet: " + block; caution = true; }
    else if (mEditing && !mPreview.mWarnings.empty())
    {
        // two at most, so the line never runs out of its box (the red marks in the world show the places)
        const size_t shown = llmin((size_t)2, mPreview.mWarnings.size());
        for (size_t i = 0; i < shown; ++i) line += (line.empty() ? "" : "\n") + mPreview.mWarnings[i];
        if (mPreview.mWarnings.size() > shown) line += llformat(" (and %d more)", (S32)(mPreview.mWarnings.size() - shown));
        caution = true;
    }
    else if (mEditing) line = "Ready: press Build tunnel. The see-through preview shows it in the ground.";
    if (mWarnings->getText() != line) mWarnings->setText(line);
    mWarnings->setColor(LLUIColorTable::instance().getColor(caution ? "AlertCautionTextColor" : "LabelTextColor"));
    const std::string hint = nextStep();
    if (mHint->getText() != hint) mHint->setText(hint);
    // the reason Build is grey goes to the log once per change (Paul's 10-22 test left no trace of it)
    const std::string logged = mEditing ? block : std::string();
    if (logged != mLoggedBlock)
    {
        mLoggedBlock = logged;
        if (!logged.empty()) LL_INFOS("WolfTunnels") << "Build tunnel not available: " << logged << LL_ENDL;
    }

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
        self->setStatus("The region did not answer (" + status.toString() + "). Close this window and open it again to try again.", true);
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
        setStatus(mEdit ? std::string() : getString("str_need_rights"), !mEdit);   // (the hint line says what to do next)
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
    mHover = -1;
    // held only while a pen is down: the base tool stays the pie, so every other click is the pie's (wolftooladventureplace.h)
    LLToolMgr::getInstance()->setTransientTool(this);
}

void WolfToolTunnel::cancel()
{
    const bool was = mActive;
    dropDrag();
    if (hasMouseCapture()) setMouseCapture(false);
    mActive = false;
    mPen = PEN_NONE;
    mHaveCursor = false;
    mHover = -1;
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
// mousePointOnLandGlobal (llviewerwindow.cpp:5788-5890) walks the pointer's ray from the camera until it first goes under the
// height field, so it finds the ground from above whatever the terrain shader draws (the see-through ground does not change it).
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

// Source: wolfcave.cpp WolfToolCave::planeHit (llviewerwindow.h:455 mousePointOnPlaneGlobal(point, x, y, plane_point,
// plane_normal); llviewerwindow.cpp:5738-5765 false when the plane is behind the camera; llviewerregion.h getPosRegionFromGlobal).
bool WolfToolTunnel::planeHit(S32 x, S32 y, F32 z, F32& rx, F32& ry) const
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return false;
    LLVector3d spot;
    if (!gViewerWindow->mousePointOnPlaneGlobal(spot, x, y, rgn->getPosGlobalFromRegion(LLVector3(0.f, 0.f, z)), LLVector3::z_axis)) return false;
    const LLVector3 pos = rgn->getPosRegionFromGlobal(spot);
    // Near the horizon the plane is met kilometres away: no use in this region (wolfcave.cpp the same).
    const F32 w = rgn->getWidth();
    if (!finite3(pos) || pos.mV[VX] < -w || pos.mV[VY] < -w || pos.mV[VX] > 2.f * w || pos.mV[VY] > 2.f * w) return false;
    rx = pos.mV[VX];
    ry = pos.mV[VY];
    return true;
}

// static
bool WolfToolTunnel::cameraUnderground()
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return false;
    // llviewercamera.h getOrigin: the camera in the agent frame (llcoordframe.h).
    const LLVector3 cam = rgn->getPosRegionFromAgent(LLViewerCamera::getInstance()->getOrigin());
    const F32 w = rgn->getWidth();
    if (!finite3(cam) || cam.mV[VX] < 0.f || cam.mV[VY] < 0.f || cam.mV[VX] >= w || cam.mV[VY] >= w) return false;
    return cam.mV[VZ] < rgn->getLand().resolveHeightRegion(cam.mV[VX], cam.mV[VY]) - UNDERGROUND_M;
}

// [2026-10-22] Paul: "want to draw underground ... you want to see and draw the route inside the hill, at the depth you choose".
// From above the ground a click picks the ground spot (as before) and the point goes in at the depth below it. From under the
// ground (the camera in a tunnel) the ray from the camera would only meet the ground from below, so the click lands on a level
// drawing plane at the picked (or last) point's floor instead, and the point goes in exactly there.
bool WolfToolTunnel::place(S32 x, S32 y, LLVector3& floor, LLVector3& ground, bool& on_plane) const
{
    WolfFloaterTunnels* f = static_cast<WolfFloaterTunnels*>(mFloater.get());
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!f || !rgn) return false;
    const LLSurface& land = rgn->getLand();
    const F32 w = rgn->getWidth();
    if (cameraUnderground())
    {
        const F32 z = f->planeHeight();
        F32 px, py;
        if (!planeHit(x, y, z, px, py) || px < 0.f || py < 0.f || px >= w || py >= w) return false;
        on_plane = true;
        floor.set(px, py, z);
        ground.set(px, py, land.resolveHeightRegion(px, py));
        return true;
    }
    LLVector3 p;
    if (!surface(x, y, p)) return false;
    on_plane = false;
    ground = p;
    floor.set(p.mV[VX], p.mV[VY], f->newPointFloor(p.mV[VX], p.mV[VY], p.mV[VZ]));
    return true;
}

// Source: wolfcave.cpp WolfToolCave::pickHandle (llviewercamera.h:75 projectPosAgentToScreen(pos_agent, out_point, clamp) in the
// scaled UI coordinates the tool's x, y are in). Each point has two handles: 2k at its floor (move), 2k + 1 at its roof (up/down).
S32 WolfToolTunnel::pickHandle(S32 x, S32 y) const
{
    WolfFloaterTunnels* f = WolfFloaterTunnels::editing();
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!f || !rgn) return -1;
    const WolfTunnels::Design& d = f->design();
    const size_t n = d.mP.size() / 2;
    if (d.mZ.size() != n) return -1;
    LLViewerCamera* cam = LLViewerCamera::getInstance();
    S32 best = -1;
    F32 best_px = HANDLE_PICK_PX;
    for (size_t k = 0; k < n; ++k)
    {
        for (S32 which = 0; which < 2; ++which)
        {
            const LLVector3 h(d.mP[2 * k], d.mP[2 * k + 1], d.mZ[k] + (which ? d.mH : 0.f));
            LLCoordGL sp;
            if (!cam->projectPosAgentToScreen(rgn->getPosAgentFromRegion(h), sp, true)) continue;
            const F32 dx = (F32)(sp.mX - x), dy = (F32)(sp.mY - y);
            const F32 px = sqrtf(dx * dx + dy * dy);
            if (px <= best_px) { best = (S32)k * 2 + which; best_px = px; }
        }
    }
    return best;
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
    // A point's handle: pick it and drag it (wolfcave.cpp WolfToolCave::handleMouseDown, the box pen on a box's handles).
    const S32 h = mPen == PEN_ROUTE ? pickHandle(x, y) : -1;
    if (h >= 0)
    {
        const WolfTunnels::Design& d = f->design();
        mDragPoint = h / 2;
        mDragStart.set(d.mP[2 * mDragPoint], d.mP[2 * mDragPoint + 1], d.mZ[mDragPoint]);
        f->pickPoint(mDragPoint);
        if (h % 2 == 1)
        {
            // Up and down: metres per pixel from the roof handle and a point 1 m above (or below) it on screen, else from the
            // camera (wolfcave.cpp DRAG_TOP: llcamera.h getView / getViewHeightInPixels, llviewerwindow.h getDisplayScale).
            LLViewerRegion* rgn = gAgent.getRegion();
            LLViewerCamera* cam = LLViewerCamera::getInstance();
            F32 px_per_m = 0.f;
            if (rgn)
            {
                const LLVector3 top_agent = rgn->getPosAgentFromRegion(LLVector3(mDragStart.mV[VX], mDragStart.mV[VY], mDragStart.mV[VZ] + d.mH));
                LLCoordGL s0, s1;
                if (cam->projectPosAgentToScreen(top_agent, s0, false))
                {
                    for (const F32 dz : { 1.f, -1.f })
                    {
                        if (cam->projectPosAgentToScreen(top_agent + LLVector3(0.f, 0.f, dz), s1, false)) { px_per_m = fabsf((F32)(s1.mY - s0.mY)); break; }
                    }
                }
                if (px_per_m < 2.f)
                {
                    const F32 dist = llmax(dist_vec(cam->getOrigin(), top_agent), 0.5f);
                    const F32 scale_y = llmax(gViewerWindow->getDisplayScale().mV[VY], 0.01f);
                    px_per_m = (F32)cam->getViewHeightInPixels() / (2.f * dist * tanf(cam->getView() * 0.5f)) / scale_y;
                }
            }
            // 1 cm to 0.5 m of height per pixel: never a jump of metres for one pixel (wolfcave.cpp the same).
            mMetresPerPixel = llclamp(1.f / llmax(px_per_m, 0.01f), 0.01f, 0.5f);
            mPressY = y;
            mDrag = DRAG_UP;
        }
        else
        {
            // Sideways on the level plane at the point's floor; the grab keeps its offset from the point.
            F32 px, py;
            if (!planeHit(x, y, mDragStart.mV[VZ], px, py)) { px = mDragStart.mV[VX]; py = mDragStart.mV[VY]; }
            mGrabDX = mDragStart.mV[VX] - px;
            mGrabDY = mDragStart.mV[VY] - py;
            mDrag = DRAG_MOVE;
        }
        mHover = h;
        setMouseCapture(true);
        return true;
    }
    LLVector3 floor, ground;
    bool on_plane = false;
    if (!place(x, y, floor, ground, on_plane))
    {
        f->setStatus(cameraUnderground() ? "Point at the drawing plane (the grid) inside this region." : "Click on this region's ground.", true);
        return true;
    }
    if (mPen == PEN_ROUTE) f->addRoutePoint(floor.mV[VX], floor.mV[VY], floor.mV[VZ], on_plane);
    else if (mPen == PEN_CAVERN) f->addCavernAt(floor.mV[VX], floor.mV[VY]);
    return true;
}

bool WolfToolTunnel::handleMouseUp(S32 x, S32 y, MASK mask)
{
    // Read the drag first: letting go of the capture calls onMouseCaptureLost, which drops it (wolfcave.cpp the same).
    const EDrag drag = mDrag;
    const S32 k = mDragPoint;
    mDrag = DRAG_NONE;
    mDragPoint = -1;
    if (hasMouseCapture()) setMouseCapture(false);
    if (drag != DRAG_NONE)
    {
        if (WolfFloaterTunnels* f = static_cast<WolfFloaterTunnels*>(mFloater.get()))
        {
            const WolfTunnels::Design& d = f->design();
            if (k >= 0 && (size_t)k < d.mP.size() / 2 && d.mZ.size() == d.mP.size() / 2)
                f->setPoint(k, d.mP[2 * k], d.mP[2 * k + 1], d.mZ[k], true);
        }
    }
    return mActive;
}

// A drag that is dropped (Esc, the pen put down, the mouse taken by another window) puts the point back as it was at the press.
void WolfToolTunnel::dropDrag()
{
    if (mDrag != DRAG_NONE && mDragPoint >= 0)
    {
        if (WolfFloaterTunnels* f = static_cast<WolfFloaterTunnels*>(mFloater.get()))
            f->setPoint(mDragPoint, mDragStart.mV[VX], mDragStart.mV[VY], mDragStart.mV[VZ], true);
    }
    mDrag = DRAG_NONE;
    mDragPoint = -1;
    if (hasMouseCapture()) setMouseCapture(false);
}

void WolfToolTunnel::onMouseCaptureLost()
{
    dropDrag();
}

bool WolfToolTunnel::handleDoubleClick(S32 x, S32 y, MASK mask)
{
    if (!mActive) return LLToolPie::getInstance()->handleDoubleClick(x, y, mask);
    WolfFloaterTunnels* f = static_cast<WolfFloaterTunnels*>(mFloater.get());
    if (f && mPen == PEN_ROUTE)
    {
        // The double-click's point (dropped when it is the point the first click just added), then the route is finished.
        LLVector3 floor, ground;
        bool on_plane = false;
        if (pickHandle(x, y) < 0 && place(x, y, floor, ground, on_plane)) f->addRoutePoint(floor.mV[VX], floor.mV[VY], floor.mV[VZ], on_plane);
        f->finishRoute();
    }
    return true;
}

bool WolfToolTunnel::handleHover(S32 x, S32 y, MASK mask)
{
    if (!shown()) { cancel(); return false; }
    WolfFloaterTunnels* f = static_cast<WolfFloaterTunnels*>(mFloater.get());
    LLViewerRegion* rgn = gAgent.getRegion();
    ECursorType cursor = UI_CURSOR_CROSS;
    if (mDrag != DRAG_NONE && f && rgn && mDragPoint >= 0)
    {
        F32 nx = mDragStart.mV[VX], ny = mDragStart.mV[VY], nz = mDragStart.mV[VZ];
        if (mDrag == DRAG_UP)
        {
            nz = mDragStart.mV[VZ] + (F32)(y - mPressY) * mMetresPerPixel;
            cursor = UI_CURSOR_SIZENS;
        }
        else
        {
            F32 px, py;
            if (planeHit(x, y, mDragStart.mV[VZ], px, py))
            {
                // Inside the region (a point outside it is refused at Build anyway; keep it in while dragging).
                const F32 w = rgn->getWidth();
                nx = llclamp(px + mGrabDX, 0.f, w);
                ny = llclamp(py + mGrabDY, 0.f, w);
            }
            else
            {
                // pointing above the horizon: keep the last place
                const WolfTunnels::Design& d = f->design();
                nx = d.mP[2 * mDragPoint];
                ny = d.mP[2 * mDragPoint + 1];
            }
            cursor = UI_CURSOR_SIZEALL;
        }
        f->setPoint(mDragPoint, nx, ny, nz, false);
        gViewerWindow->setCursor(cursor);
        return true;
    }
    mHover = mPen == PEN_ROUTE ? pickHandle(x, y) : -1;
    LLVector3 floor, ground;
    mHaveCursor = mHover < 0 && place(x, y, floor, ground, mOnPlane);
    if (mHaveCursor) { mCursor = ground; mGhost = floor; }
    if (mHover >= 0) cursor = (mHover % 2) ? UI_CURSOR_SIZENS : UI_CURSOR_SIZEALL;
    // Source: llcursortypes.h UI_CURSOR_CROSS / UI_CURSOR_SIZENS / UI_CURSOR_SIZEALL.
    gViewerWindow->setCursor(cursor);
    return true;
}

bool WolfToolTunnel::handleKey(KEY key, MASK mask)
{
    if (!mActive) return false;
    WolfFloaterTunnels* f = static_cast<WolfFloaterTunnels*>(mFloater.get());
    if (key == KEY_ESCAPE)
    {
        if (mDrag != DRAG_NONE) { dropDrag(); if (f) f->setStatus("The point is back where it was.", false); return true; }
        cancel();
        if (f) f->setStatus("Stopped drawing - the drawing is kept.", false);
        return true;
    }
    if (f && mPen == PEN_ROUTE && key == KEY_RETURN) { f->finishRoute(); return true; }
    if (f && mPen == PEN_ROUTE && key == KEY_BACKSPACE) { f->undoPoint(); return true; }
    if (f && mPen == PEN_ROUTE && key == KEY_DELETE && f->pickedPoint() >= 0) { f->removePickedPoint(); return true; }
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
    std::vector<LLVector3> route, ground, posts, red, caverns, plane, ghost, handles, hot, picked;
    const LLSurface& land = rgn->getLand();
    const size_t n = d.mP.size() / 2;
    const bool have_z = d.mZ.size() == n;
    const Route rt(d.mP);
    // The route at its floor (orange) and over the ground (white), as line pairs, and a post from the ground down to every point.
    LLVector3 prev_r, prev_g;
    for (size_t k = 0; k < n && have_z; ++k)
    {
        const F32 x = d.mP[2 * k], y = d.mP[2 * k + 1];
        const LLVector3 r(x, y, d.mZ[k] + 0.05f), g(x, y, land.resolveHeightRegion(x, y) + 0.1f);
        if (k > 0) { route.push_back(prev_r); route.push_back(r); ground.push_back(prev_g); ground.push_back(g); }
        posts.push_back(r);
        posts.push_back(g);
        prev_r = r;
        prev_g = g;
    }
    // [2026-10-22] Where the next click goes: from the last point to it at its floor, a line down from the ground spot clicked, a
    // ring round it, and the level drawing plane at its depth (a grid round it).
    const bool drawing_here = f->routeOpen() && mActive && mPen == PEN_ROUTE && mHaveCursor && mDrag == DRAG_NONE && mHover < 0;
    if (drawing_here)
    {
        if (n > 0 && have_z)
        {
            route.emplace_back(d.mP[2 * n - 2], d.mP[2 * n - 1], d.mZ[n - 1] + 0.05f);
            route.emplace_back(mGhost.mV[VX], mGhost.mV[VY], mGhost.mV[VZ] + 0.05f);
            ground.emplace_back(d.mP[2 * n - 2], d.mP[2 * n - 1], land.resolveHeightRegion(d.mP[2 * n - 2], d.mP[2 * n - 1]) + 0.1f);
            ground.emplace_back(mCursor.mV[VX], mCursor.mV[VY], mCursor.mV[VZ] + 0.1f);
        }
        ghost.emplace_back(mCursor.mV[VX], mCursor.mV[VY], mCursor.mV[VZ] + 0.1f);
        ghost.push_back(mGhost);
        for (S32 i = 0; i < 24; ++i)
        {
            const F32 a0 = (F32)i / 24.f * F_TWO_PI, a1 = (F32)(i + 1) / 24.f * F_TWO_PI, r = llmax(0.5f, d.mW * 0.5f);
            ghost.emplace_back(mGhost.mV[VX] + cosf(a0) * r, mGhost.mV[VY] + sinf(a0) * r, mGhost.mV[VZ] + 0.05f);
            ghost.emplace_back(mGhost.mV[VX] + cosf(a1) * r, mGhost.mV[VY] + sinf(a1) * r, mGhost.mV[VZ] + 0.05f);
        }
        const F32 cx = PLANE_STEP_M * floorf(mGhost.mV[VX] / PLANE_STEP_M + 0.5f), cy = PLANE_STEP_M * floorf(mGhost.mV[VY] / PLANE_STEP_M + 0.5f);
        for (F32 o = -PLANE_HALF_M; o <= PLANE_HALF_M + 0.01f; o += PLANE_STEP_M)
        {
            plane.emplace_back(cx + o, cy - PLANE_HALF_M, mGhost.mV[VZ]);
            plane.emplace_back(cx + o, cy + PLANE_HALF_M, mGhost.mV[VZ]);
            plane.emplace_back(cx - PLANE_HALF_M, cy + o, mGhost.mV[VZ]);
            plane.emplace_back(cx + PLANE_HALF_M, cy + o, mGhost.mV[VZ]);
        }
    }
    // The points' handles while the pen is down (wolfcave.cpp WolfToolCave::render box handles: a small wire cube about the same
    // size on screen at any distance; the one under the pointer or being dragged larger and bright). White at the floor = move,
    // yellow at the roof = up and down, joined by a line; the picked point's in amber.
    if (mActive && mPen == PEN_ROUTE && have_z)
    {
        const LLVector3 eye = LLViewerCamera::getInstance()->getOrigin();
        const S32 hot_h = mDrag != DRAG_NONE ? mDragPoint * 2 + (mDrag == DRAG_UP ? 1 : 0) : mHover;
        for (size_t k = 0; k < n; ++k)
        {
            const LLVector3 lo(d.mP[2 * k], d.mP[2 * k + 1], d.mZ[k]), hi(d.mP[2 * k], d.mP[2 * k + 1], d.mZ[k] + d.mH);
            std::vector<LLVector3>& stem = (S32)k == f->pickedPoint() ? picked : handles;
            stem.push_back(lo);
            stem.push_back(hi);
            for (S32 which = 0; which < 2; ++which)
            {
                const LLVector3 c = which ? hi : lo;
                const S32 id = (S32)k * 2 + which;
                F32 r = llclamp(dist_vec(eye, A(c)) * 0.012f, 0.12f, 3.f);
                if (id == hot_h) r *= 1.6f;
                std::vector<LLVector3>& out = id == hot_h ? hot : ((S32)k == f->pickedPoint() ? picked : handles);
                LLVector3 v[8];
                for (S32 q = 0; q < 8; ++q) v[q] = c + LLVector3((q & 1) ? r : -r, (q & 2) ? r : -r, (q & 4) ? r : -r);
                static const S32 E[12][2] = { {0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7} };
                for (const auto& e : E) { out.push_back(v[e[0]]); out.push_back(v[e[1]]); }
            }
        }
    }
    for (size_t k = 0; k + 1 < pv.mPosts.size(); k += 2) { posts.push_back(pv.mPosts[k]); posts.push_back(pv.mPosts[k + 1]); }
    for (const LLVector3& b : pv.mBreakouts) { red.push_back(b); red.emplace_back(b.mV[VX], b.mV[VY], b.mV[VZ] + 3.f); }
    for (const WolfTunnels::Cavern& c : d.mCaverns)
    {
        const F32 z = (have_z ? WolfTunnels::floorAt(d, rt.nearest(c.mX, c.mY).second) : 0.f) + 0.05f;
        for (S32 i = 0; i < 36; ++i)
        {
            const F32 a0 = (F32)i / 36.f * F_TWO_PI, a1 = (F32)(i + 1) / 36.f * F_TWO_PI;
            caverns.emplace_back(c.mX + cosf(a0) * c.mR, c.mY + sinf(a0) * c.mR, z);
            caverns.emplace_back(c.mX + cosf(a1) * c.mR, c.mY + sinf(a1) * c.mR, z);
        }
    }
    {
        LLGLDepthTest depth(GL_FALSE);
        gGL.setLineWidth(1.f);
        gGL.color4f(0.55f, 0.80f, 1.f, 0.35f);
        draw(plane, LLRender::LINES);
        gGL.setLineWidth(2.f);
        gGL.color4f(1.f, 0.70f, 0.28f, 0.9f);
        draw(route, LLRender::LINES);
        gGL.color4f(1.f, 1.f, 1.f, 0.7f);
        draw(ground, LLRender::LINES);
        gGL.color4f(1.f, 1.f, 1.f, 0.45f);
        draw(posts, LLRender::LINES);
        gGL.color4f(0.55f, 0.80f, 1.f, 0.9f);
        draw(caverns, LLRender::LINES);
        gGL.color4f(1.f, 1.f, 0.4f, 0.95f);
        draw(ghost, LLRender::LINES);
        gGL.setLineWidth(4.f);
        gGL.color4f(1.f, 0.2f, 0.15f, 1.f);
        draw(red, LLRender::LINES);
        // the handles last and never hidden (wolfcave.cpp the same)
        gGL.setLineWidth(2.f);
        gGL.color4f(1.f, 1.f, 1.f, 0.9f);
        draw(handles, LLRender::LINES);
        gGL.color4f(0.96f, 0.65f, 0.14f, 1.f);
        draw(picked, LLRender::LINES);
        gGL.setLineWidth(3.f);
        gGL.color4f(1.f, 1.f, 0.3f, 1.f);
        draw(hot, LLRender::LINES);
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
    // [2026-10-22] Each point's depth below the ground at its floor (Paul: "the posts/labels show depth below the surface at each
    // point"), and the depth a click would give, at the drawing ring.
    if (f && rgn)
    {
        const WolfTunnels::Design& d = f->design();
        const size_t n = d.mP.size() / 2;
        LLViewerCamera* cam = LLViewerCamera::getInstance();
        const LLSurface& land = rgn->getLand();
        auto depthText = [](F32 ground, F32 floor) -> std::string
        {
            const F32 dd = ground - floor;
            return fabsf(dd) < 0.05f ? std::string("at the ground") : dd > 0.f ? llformat("%.1f m deep", dd) : llformat("%.1f m above the ground", -dd);
        };
        for (size_t k = 0; k < n && d.mZ.size() == n; ++k)
        {
            LLCoordGL sp;
            if (!cam->projectPosAgentToScreen(rgn->getPosAgentFromRegion(LLVector3(d.mP[2 * k], d.mP[2 * k + 1], d.mZ[k])), sp, false)) continue;
            const F32 g = land.resolveHeightRegion(d.mP[2 * k], d.mP[2 * k + 1]);
            const bool pk = (S32)k == f->pickedPoint();
            font->renderUTF8(llformat("%d: ", (S32)k + 1) + depthText(g, d.mZ[k]), 0, (F32)sp.mX + 10.f, (F32)sp.mY - 6.f,
                             pk ? LLColor4(0.96f, 0.65f, 0.14f, 1.f) : LLColor4(1.f, 0.85f, 0.55f, 0.95f), LLFontGL::LEFT, LLFontGL::TOP,
                             LLFontGL::NORMAL, LLFontGL::DROP_SHADOW);
        }
        if (mActive && mPen == PEN_ROUTE && mHaveCursor && mDrag == DRAG_NONE && mHover < 0 && f->routeOpen())
        {
            LLCoordGL sp;
            // [2026-10-22 easy] with the mouths on the ground a new point is the far end (the one before it goes down)
            const bool mouth = !mOnPlane && n > 0 && fabsf(land.resolveHeightRegion(mGhost.mV[VX], mGhost.mV[VY]) - mGhost.mV[VZ]) < 0.05f;
            if (cam->projectPosAgentToScreen(rgn->getPosAgentFromRegion(mGhost), sp, false))
                font->renderUTF8(mouth ? std::string("new end (a mouth, on the ground)")
                                       : std::string("new point: ") + depthText(land.resolveHeightRegion(mGhost.mV[VX], mGhost.mV[VY]), mGhost.mV[VZ]),
                                 0, (F32)sp.mX + 12.f, (F32)sp.mY + 6.f, LLColor4(1.f, 1.f, 0.4f, 1.f), LLFontGL::LEFT, LLFontGL::BOTTOM,
                                 LLFontGL::NORMAL, LLFontGL::DROP_SHADOW);
        }
    }
    if (!mActive) return;
    // what the pen wants, at the top of the world view (wolftooladventureplace.cpp draw), two lines
    std::vector<std::string> lines;
    if (mPen == PEN_CAVERN) lines.push_back("Caverns: click on the route to put one there. Esc stops.");
    else if (mDrag == DRAG_UP) lines.push_back("Move the mouse up or down to raise or lower the point. Esc puts it back.");
    else if (mDrag == DRAG_MOVE) lines.push_back("Move the point along the level grid at its depth. Esc puts it back.");
    else if (mHover >= 0) lines.push_back(mHover % 2 ? "Drag up or down: raise or lower this point." : "Drag: move this point sideways (it keeps its height).");
    else
    {
        // [2026-10-22 easy] what to click next, then the keys (no Finish step: Enter / double-click only put the pen away)
        const size_t n = f ? f->design().mP.size() / 2 : 0;   // (f: the window editing a tunnel, above)
        lines.push_back(mOnPlane && mHaveCursor ? std::string("You are under the ground: click on the grid to put a point there, at the grid's depth.")
                        : n == 0 ? std::string("Click on the ground where the tunnel starts.")
                        : n == 1 ? std::string("Click where the tunnel ends (or where it bends first).")
                                 : std::string("Click to add a bend or a new end. Then press Build tunnel in the Tunnels window."));
        lines.push_back("Drag a white box to move a point, a yellow box to raise or lower it. Backspace: undo. Enter or Esc: put the pen away.");
    }
    const F32 cx = (F32)gViewerWindow->getWorldViewWidthScaled() * 0.5f;
    F32 ty = (F32)gViewerWindow->getWorldViewHeightScaled() - 60.f;
    for (const std::string& h : lines)
    {
        const F32 tw = (F32)font->getWidthF32(h);
        WolfGameDraw::rect(cx - tw * 0.5f - 8.f, ty - 4.f, cx + tw * 0.5f + 8.f, ty + (F32)font->getLineHeight() + 4.f, LLColor4(0.f, 0.f, 0.f, 0.75f));
        font->renderUTF8(h, 0, cx, ty, LLColor4::white, LLFontGL::HCENTER, LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
        ty -= (F32)font->getLineHeight() + 10.f;
    }
}
