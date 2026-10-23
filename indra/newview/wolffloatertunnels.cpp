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
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <boost/json.hpp>

#include "fscommon.h"
#include "llagent.h"
#include "llapp.h"
#include "llagentcamera.h"
#include "llfloatertools.h"
#include "llfocusmgr.h"
#include "lldate.h"
#include "lleventcoro.h"
#include "llinventorymodel.h"
#include "llparcel.h"
#include "llprogressbar.h"
#include "lllineeditor.h"
#include "llpanel.h"
#include "llradiogroup.h"
#include "llrand.h"
#include "llsdjson.h"
#include "llsdutil.h"
#include "llselectmgr.h"
#include "llsliderctrl.h"
#include "llsurfacepatch.h"
#include "lltooldraganddrop.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "llviewerobjectlist.h"
#include "llviewerparcelmgr.h"
#include "message.h"
#include "wolfcave.h"
#include "wolftunnelcamera.h"   // [2026-10-23] the camera in a tunnel (keepCameraInside)
#include "wolfcavelights.h"
#include "wolfdigprogress.h"
#include "wolfburrow.h"
#include "lldraghandle.h"
#include "wolfpaint.h"
#include "llcombobox.h"
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
    // [2026-10-23] The camera's inset, steps and easing are in wolftunnelcamera.h (WolfTunnelCamera::INSET_M, STEP_M, ease).
    // The kept camera distance (metres from the head along the wanted line) and the frame time it was worked out at; the
    // distance is eased between frames, so it is kept from one frame to the next while the head is in a box.
    F32 s_camKeep = 0.f;
    F64 s_camKeepAt = -1.0;

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

bool WolfTunnels::lowestBottom(const LLVector3& region_pos, F32& bottom_z)
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (s_boxes.empty() || !rgn || rgn->getRegionID() != s_boxesRegion) return false;
    bool found = false;
    for (const Box& b : s_boxes)
    {
        if (!inBox(b, region_pos)) continue;
        // boxes only turn about +Z (inBox: lz = p.z - c.z), so the bottom is level at c.z - s.z / 2
        const F32 bottom = b.mCentre.mV[VZ] - b.mSize.mV[VZ] * 0.5f;
        bottom_z = found ? llmin(bottom_z, bottom) : bottom;
        found = true;
    }
    return found;
}

bool WolfTunnels::keepCameraInside(const LLVector3& head_agent, LLVector3& camera_agent)
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (s_boxes.empty() || !rgn || rgn->getRegionID() != s_boxesRegion) return false;
    const LLVector3 head = rgn->getPosRegionFromAgent(head_agent);
    const LLVector3 dir = camera_agent - head_agent;
    const F32 len = dir.magVec();
    // only the boxes the line can reach (a tunnel is a chain of boxes; Moonfold has 356)
    std::vector<const Box*> nearby;   // not "near": windef.h defines near (and far) as nothing
    bool head_in = false;
    for (const Box& b : s_boxes)
    {
        if (dist_vec(b.mCentre, head) > len + b.mSize.magVec() * 0.5f + 1.f) continue;
        nearby.push_back(&b);
        if (inBox(b, head)) head_in = true;
    }
    if (!head_in)
    {
        s_camKeepAt = -1.0;   // out of the tunnel: the next time in starts the easing again
        return false;
    }
    if (len < 0.001f) return true;
    // [2026-10-23] Paul: "i've still got this thing when i walk through a tunnel where the camera flashes back". The camera may
    // be where a ball of WolfTunnelCamera::INSET_M round it stays in the boxes' union or the open air above the ground (at a
    // tunnel's mouth it may stay outside, behind the avatar walking in; only rock is out of bounds) - the joins between boxes
    // no longer stop it (wolftunnelcamera.h says why they did). How far it may go along the wanted line is then eased.
    const LLSurface& land = rgn->getLand();
    auto ground = [&land](float x, float y) { return land.resolveHeightRegion(x, y); };
    std::vector<WolfTunnelCamera::Box> local;
    local.reserve(nearby.size());
    for (const Box* b : nearby)
    {
        WolfTunnelCamera::Box t;
        t.c = WolfTunnelCamera::Vec3(b->mCentre.mV[VX], b->mCentre.mV[VY], b->mCentre.mV[VZ]);
        t.s = WolfTunnelCamera::Vec3(b->mSize.mV[VX], b->mSize.mV[VY], b->mSize.mV[VZ]);
        t.yaw_deg = b->mYawDeg;
        local.push_back(t);
    }
    std::vector<const WolfTunnelCamera::Box*> ptrs;
    ptrs.reserve(local.size());
    for (const WolfTunnelCamera::Box& t : local) ptrs.push_back(&t);
    const LLVector3 unit = dir / len;
    const F32 target = WolfTunnelCamera::allowedDistance(ptrs, ground, WolfTunnelCamera::Vec3(head.mV[VX], head.mV[VY], head.mV[VZ]),
                                                         WolfTunnelCamera::Vec3(unit.mV[VX], unit.mV[VY], unit.mV[VZ]), len);
    // Frame time: the same for every call in one frame (llframetimer.h getElapsedSeconds returns sFrameTime), so the other
    // callers of calcCameraPositionTargetGlobal in a frame (llagentcamera.cpp resets, focus) do not ease it again.
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (s_camKeepAt < 0.0 || now - s_camKeepAt > WolfTunnelCamera::EASE_RESTART_S)
    {
        // just in (walked in at a mouth, or the camera mode changed): ease from where the camera was last frame, so the change
        // from the open-air camera to the tunnel's is a glide, not a snap
        s_camKeep = llclamp(dist_vec(LLViewerCamera::getInstance()->getOrigin(), head_agent), 0.f, len);
    }
    else
    {
        s_camKeep = WolfTunnelCamera::ease(s_camKeep, target, (F32)(now - s_camKeepAt));
    }
    s_camKeepAt = now;
    // never past the wanted camera (the stock zoom eases that itself)
    s_camKeep = llclamp(s_camKeep, 0.f, len);
    camera_agent = head_agent + unit * s_camKeep;
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
    // [2026-10-23 subway] wolftunnel.rs MAX_EXITS, EXIT_POINTS_MAX, FLAT_TOL_M - the same numbers. The window's own: a way up
    // climbs at EXIT_GRADE (1 in 2, about 27 degrees: walkable for an avatar and short), starts with a level stub EXIT_STUB_M past
    // the tunnel's or the room's edge, is EXIT_W_M wide (a path's width less a metre), and its mouth is at least EXIT_CLEAR_M from
    // the tunnel's line and other ways up.
    const S32 MAX_EXITS = 12;
    const F32 FLAT_TOL_M = 0.02f;
    const F32 EXIT_GRADE = 0.5f;
    const F32 EXIT_STUB_M = 1.f;
    const F32 EXIT_W_M = 3.f;
    const F32 EXIT_CLEAR_M = 2.f;
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
    // [2026-10-22 crossroads] wolftunnel.rs WALL_T, WALL_TOL_SMOOTH / _ROUGH, LINING_T (= WALL_T), LINING_CORNER_MAX,
    // STATION_M, LOCAL_K, LEVEL_STEP_M, LEVEL_TOL_M, LEVEL_ROUNDS: the same numbers, so levelRoute levels as `level` does.
    const F32 WALL_T = 0.4f, WALL_TOL_SMOOTH = 0.08f, WALL_TOL_ROUGH = 0.45f, LINING_T = WALL_T, LINING_CORNER_MAX = 2.f;
    const F32 STATION_M = 0.52f, LOCAL_K = 1.41421356f, LEVEL_STEP_M = 0.15f, LEVEL_TOL_M = 0.005f;
    const S32 LEVEL_ROUNDS = 64;
    const F32 MOUTH_M = 8.f;
    // [WOLF TERRAFORM 2026-10-23] WALK_GRADE_WARN is in wolffloatertunnels.h now (the Terraform window's ramps warn at it too)
    using WolfTunnels::WALK_GRADE_WARN;
    const F32 RAIL_GRADE_WARN = 0.04f;
    // The window's own: the kinds' sizes (Paul's brief: walking 4 m, wide 6 m, rail 6 x 6), the warnings' thin cover (the
    // region's box reaches wolftunnel.rs VOL_ABOVE_M = 0.3 m over the roof, so less than this may open the ground), a new
    // cavern's height from its radius, how many places a warning names, and the preview's most cross-sections.
    const F32 COVER_THIN_M = 0.5f;
    const S32 PREVIEW_RINGS_MAX = 1500;
    const S32 PREVIEW_ACROSS = 12;
    // [2026-10-22] Drawing underground (the window's own choices): the preview follows a marker drag at most this often, seconds;
    // a marker is picked within this many pixels (wolfcave.cpp HANDLE_PICK_PX = 10).
    const F64 PREVIEW_DRAG_S = 0.1;
    const F32 HANDLE_PICK_PX = 10.f;
    // [2026-10-22 simple] The window's own choices for the worked-out floor (fitRoute) and the pen:
    // - COVER_M: rock kept over the roof inside the hill (TUNNEL_STRANGER_ASSESSMENT.md section 5 ">= 2 m"; Expert changes it);
    // - the ground is read every FIT_STEP_M along the route, at most FIT_SAMPLES_MAX times (a longer route reads it further apart);
    // - the floor's own points are kept where the worked-out floor leaves a straight line between them by more than FIT_TOL_M;
    // - a point of a stored route that is within BEND_TOL_M of the straight line between its neighbours is one of those floor
    //   points, not a bend the user clicked (the design rounds every number to 0.01 m, designToLLSD);
    // - a press on a marker becomes a drag after DRAG_START_PX of movement (the assessment: "5 px or more while held");
    // - the camera looks down from TOP_VIEW_M over the ground (about 115 m of ground across the view at the viewer's default
    //   60 degree field of view, llcamera.h:36 DEFAULT_FIELD_OF_VIEW), tilted TOP_VIEW_TILT of that to the south so the
    //   camera's up stays defined (llagentcamera.cpp setCameraPosAndFocusGlobal looks from the camera at the focus).
    const F32 COVER_M = 2.f;
    const F32 FIT_STEP_M = 1.f;
    const S32 FIT_SAMPLES_MAX = 4000;
    const F32 FIT_TOL_M = 0.1f;
    const F32 BEND_TOL_M = 0.05f;
    const F32 DRAG_START_PX = 5.f;
    const F32 TOP_VIEW_M = 100.f, TOP_VIEW_TILT = 0.02f;
    // Source: WolfTunnelsModule.cs:58 MIN_POST_MS / :268-269 "one change at a time - try again": a refused add is tried again
    // this many times, this long apart (seconds).
    const S32 CAP_RETRIES = 3;
    const F32 CAP_RETRY_S = 0.5f;
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

    struct Air;
    /** [2026-10-23 subway] Source: wolftunnel.rs exit_design - a way up as a tunnel design of its own: the tunnel's height,
     *  walls, rock and seed (offset per exit), its start at the tunnel's floor where it starts, the points after it at that same
     *  height kept level with it. */
    WolfTunnels::Design exitDesign(const WolfTunnels::Design& d, const Air& main, const WolfTunnels::Exit& x, size_t i);

    /** Source: wolftunnel.rs Air (eval, reach, top, floor_t) - the same formulas, for the preview. */
    struct Air
    {
        const WolfTunnels::Design& d;
        Route route;
        bool rough;
        U32 seed;
        F32 hw;
        bool fill = false;  // [crossroads] wolftunnel.rs Air::fill: the rock between parts of the route that meet is air
        // [2026-10-23 subway] wolftunnel.rs Air::exits / cut_start: the ways up, joined to this air in eval
        std::vector<std::shared_ptr<WolfTunnels::Design>> xd;
        std::vector<std::shared_ptr<Air>> xa;
        bool cut_start = true;
        explicit Air(const WolfTunnels::Design& des, bool with_fill = false)
            : d(des), route(des.mP), rough(des.mStyle == "rough"), seed((U32)des.mSeed), hw(des.mW * 0.5f), fill(with_fill)
        {
            if (!des.mFlat || des.mExits.empty() || route.mPts.size() < 2) return;
            for (size_t i = 0; i < des.mExits.size(); ++i)
            {
                const WolfTunnels::Exit& x = des.mExits[i];
                if (x.mP.size() < 4 || x.mP.size() / 2 != x.mZ.size()) continue;
                xd.push_back(std::make_shared<WolfTunnels::Design>(exitDesign(des, *this, x, i)));
                xa.push_back(std::make_shared<Air>(*xd.back()));
                xa.back()->cut_start = false;
            }
        }
        F32 ro() const { return rough ? 1.f : 0.f; }
        /** Source: wolftunnel.rs Air::join_gap: the least rock between two parts of the route (the shell's thickest corner or
         *  a wall box and its set-back). */
        F32 joinGap() const { return llmax(LINING_T * LINING_CORNER_MAX, WALL_T + (rough ? WALL_TOL_ROUGH : WALL_TOL_SMOOTH)); }
        F32 cavernReach(const WolfTunnels::Cavern& c) const { return c.mR + ro() * llmin(ROUGH_CAVERN * c.mR, ROUGH_CAVERN_MAX_M); }
        /** Source: wolftunnel.rs Air::near: Route::nearest, the distance shortened (with fill) between parts of the route that
         *  meet and face each other across rock thinner than joinGap. */
        std::pair<F32, F32> nearJoined(F32 x, F32 y) const   // wolftunnel.rs Air::near ("near" is a windef.h macro)
        {
            const std::pair<F32, F32> best = route.nearest(x, y);
            if (!fill) return best;
            LLVector2 bp, bu;
            route.at(best.second, bp, bu);
            const F32 half = joinGap() * 0.5f, rch = reach();
            F32 dist = best.first;
            for (size_t k = 1; k < route.mPts.size(); ++k)
            {
                const LLVector2 a = route.mPts[k - 1], v = route.mPts[k] - route.mPts[k - 1];
                const F32 l2 = v.lengthSquared();
                const F32 u = l2 > 1e-9f ? llclamp(((x - a.mV[0]) * v.mV[0] + (y - a.mV[1]) * v.mV[1]) / l2, 0.f, 1.f) : 0.f;
                const F32 fx = a.mV[0] + u * v.mV[0], fy = a.mV[1] + u * v.mV[1];
                const F32 dk = sqrtf((x - fx) * (x - fx) + (y - fy) * (y - fy));
                if (dk >= rch + half) continue;
                const F32 tk = route.mCum[k - 1] + u * sqrtf(l2);
                const F32 apart = sqrtf((fx - bp.mV[0]) * (fx - bp.mV[0]) + (fy - bp.mV[1]) * (fy - bp.mV[1]));
                const bool facing = (fx - x) * (bp.mV[0] - x) + (fy - y) * (bp.mV[1] - y) < 0.f;
                if (facing && fabsf(tk - best.second) > LOCAL_K * apart + 1e-3f) dist = llmin(dist, llmax(dk, best.first) - half);
            }
            return std::make_pair(dist, best.second);
        }
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
        /** wolftunnel.rs Air::eval: this air joined with every way up (the deeper inside is read; the higher ceiling). */
        Field eval(F32 x, F32 y) const
        {
            const Field m = evalPart(x, y);
            if (xa.empty()) { Field o = m; o.v = llmax(m.v, CLEAR_MIN_M); return o; }
            Field best = m;
            F32 ceil = m.f + m.v;
            for (const std::shared_ptr<Air>& a : xa)
            {
                const Field fx = a->evalPart(x, y);
                ceil = llmax(ceil, fx.f + fx.v);
                if (fx.s > best.s) best = fx;
            }
            Field o = best;
            o.v = llmax(ceil - best.f, CLEAR_MIN_M);
            o.t = m.t;
            return o;
        }
        Field evalPart(F32 x, F32 y) const
        {
            const std::pair<F32, F32> nt = nearJoined(x, y);
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
            if (cut_start && t <= route.mCum[1] + 1e-4f)
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
            out.s = s; out.e = e; out.f = f; out.v = v; out.t = t;
            return out;
        }
    };

    WolfTunnels::Design exitDesign(const WolfTunnels::Design& d, const Air& main, const WolfTunnels::Exit& x, size_t i)
    {
        WolfTunnels::Design o;
        o.mStyle = d.mStyle;
        o.mKind = "walk";
        o.mW = x.mW;
        o.mH = d.mH;
        o.mP = x.mP;
        o.mZ = x.mZ;
        const F32 zf = main.floorOn(main.route.nearest(x.mP[0], x.mP[1]).second);
        const F32 drawn = o.mZ.empty() ? zf : o.mZ[0];
        for (F32& z : o.mZ) { if (fabsf(z - drawn) <= FLAT_TOL_M) z = zf; else break; }
        o.mPortals = false;
        o.mRock = d.mRock;
        o.mRockScale = d.mRockScale;
        o.mSeed = (S32)(((S64)d.mSeed + 7919 * ((S64)i + 1)) % ((S64)SEED_MAX + 1));
        return o;
    }

    // ── the records (php/tunnels.php) and the jobs ──────────────────────────────────────────────────────────────────
    std::vector<WolfTunnels::Record> s_records;
    std::string s_recordsRegion;        // region uuid the records belong to
    U32 s_recordsSerial = 0;
    bool s_fetchingRecords = false;
    bool s_jobBusy = false;
    bool s_jobOk = false;               // [2026-10-22 simple] the last build ended with every part made (Walk in shows)
    U32 s_jobSerial = 0;                // bumped when a job ends (or its status changes)
    std::string s_jobStatus;
    F32 s_jobProgress = 0.f;            // [2026-10-22 simple] 0..1 for the progress bar while a build runs; [2026-10-23] below 0
                                        // = the step cannot be counted (the dig plate sweeps instead of showing a number)
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
        // [2026-10-23 subway] php/tunnels.php "flat", "exits"
        out.mFlat = d["flat"].asBoolean();
        for (const LLSD& x : llsd::inArray(d["exits"]))
        {
            WolfTunnels::Exit e;
            for (const LLSD& v : llsd::inArray(x["p"])) e.mP.push_back((F32)v.asReal());
            for (const LLSD& v : llsd::inArray(x["pz"])) e.mZ.push_back((F32)v.asReal());
            e.mW = x.has("w") ? (F32)x["w"].asReal() : EXIT_W_M;
            e.mRoom = x.has("room") ? x["room"].asInteger() : -1;
            if (e.mP.size() >= 4 && e.mP.size() / 2 == e.mZ.size()) out.mExits.push_back(e);
        }
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
        // [2026-10-23 subway] only a level tunnel says so (tunnels.php keeps "flat" and "exits" for it alone)
        if (d.mFlat)
        {
            out["flat"] = true;
            LLSD xs = LLSD::emptyArray();
            for (const WolfTunnels::Exit& e : d.mExits)
            {
                LLSD p = LLSD::emptyArray(), z = LLSD::emptyArray();
                for (F32 v : e.mP) p.append(r2(v));
                for (F32 v : e.mZ) z.append(r2(v));
                xs.append(LLSD().with("p", p).with("pz", z).with("w", r2(e.mW)).with("room", e.mRoom));
            }
            out["exits"] = xs;
        }
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
            r.mLights = t["lights"].isMap() ? t["lights"] : LLSD();   // [WOLF CAVE LIGHTS 2026-10-23]
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

    /** How far the rezzed root may be from the asked-for height before it is moved (the generator's numbers are kept to the
     *  centimetre, wolftunnel.rs summary_string). */
    const F32 ROOT_TOL_M = 0.02f;

    /** Move a just-rezzed tunnel's whole linkset so its root is at `root`, and wait until the region says it is there.
     *  Source: wolfvehiclerez.cpp sendRotation (llselectmgr.cpp:4932-4953 packMultipleUpdate: the position first, 12 bytes
     *  MVT_LLVector3), with UPD_POSITION | UPD_LINKED_SETS (llselectmgr.h:60, :63) - OpenSim LLClientView.cs
     *  HandleMultipleObjUpdate "case 9: group position" -> SceneObjectGroup.cs UpdateGroupPosition (no ground clamp). */
    bool moveRoot(LLViewerObject* obj, const LLVector3& root, const LLUUID& region, std::string& error)
    {
        const LLUUID id = obj->getID();
        LLViewerRegion* rgn = obj->getRegion();
        if (!rgn) { error = "You left the region while the tunnel was being put at its height."; return false; }
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_MultipleObjectUpdate);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->nextBlockFast(_PREHASH_ObjectData);
        msg->addU32Fast(_PREHASH_ObjectLocalID, obj->getLocalID());
        msg->addU8Fast(_PREHASH_Type, UPD_POSITION | UPD_LINKED_SETS);
        U8 data[12];
        htolememcpy(data, root.mV, MVT_LLVector3, 12);
        msg->addBinaryDataFast(_PREHASH_Data, data, 12);
        msg->sendReliable(rgn->getHost());
        F32 off = dist_vec(obj->getPositionRegion(), root);
        for (S32 t = 0; t < 40; ++t)
        {
            llcoro::suspendUntilTimeout(0.25f);
            if (!gAgent.getRegion() || gAgent.getRegion()->getRegionID() != region)
            {
                error = "You left the region while the tunnel was being put at its height.";
                return false;
            }
            LLViewerObject* o = gObjectList.findObject(id);
            if (!o || o->isDead()) { error = "The tunnel's rock went away while it was being put at its height."; return false; }
            off = dist_vec(o->getPositionRegion(), root);
            if (off <= ROOT_TOL_M) return true;
        }
        error = llformat("The region put the tunnel %.1f m away from where it belongs and did not let it be moved there. Delete it "
                         "and press Build again.", off);
        return false;
    }

    /** Source: wolfcave.cpp WolfCaves::rezExact (the same RezObject, placed exactly; the item is fetched until it shows). */
    bool rezTunnel(const LLUUID& item_id, const LLUUID& folder_id, const LLVector3& ray_end, const LLVector3& root, const LLUUID& region,
                   S32 prims, LLUUID& object, std::string& error)
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
                // [2026-10-22 rez height] Matched across the ground only: on Sandbox (2026-10-22 11:27Z, "Tunnel 1") the region
                // stored the rock 4.17 m above the height wolftunnel.rs generate() gives that design's root, at the right x/y, so
                // a 3D match never found it and the walkable boxes (which cut the ground) were never made. The region's rez maths
                // (InventoryAccessModule.cs RezObject + Scene.cs GetNewRezLocation + SceneObjectGroup.cs
                // GetAxisAlignedBoundingBox) give RayEnd - minZ, the asked-for height, so where the lift comes from is not known;
                // the height is put right below and logged.
                const LLVector3 at = o->getPositionRegion();
                if (dist_vec(LLVector2(at.mV[VX], at.mV[VY]), LLVector2(root.mV[VX], root.mV[VY])) > 1.f) continue;
                if (!o->permYouOwner()) continue;
                object = o->getID();
                if (fabsf(at.mV[VZ] - root.mV[VZ]) <= ROOT_TOL_M) return true;
                LL_WARNS("WolfTunnels") << "the region rezzed the tunnel's root at " << at << ", asked for " << root
                                        << "; moving it to the asked-for height" << LL_ENDL;
                return moveRoot(o, root, region, error);
            }
        }
        // [2026-10-22 simple] Say why in plain words when the land's numbers show it: OpenSim PrimLimitsModule.cs DoCommonChecks
        // refuses a rez when prims + PrimCounts.Simulator > GetSimulatorMaxPrimCount ("Unable to rez object because the parcel is
        // full"), and the parcel properties carry those two numbers to the viewer (LLClientView.cs:6734-6738 SimWideMaxPrims /
        // SimWideTotalPrims -> llparcel.h getSimWideMaxPrimCapacity / getSimWidePrimCount; 0 = not known). Only the parcel the
        // avatar stands on is known here (llviewerparcelmgr.h getAgentParcel / inAgentParcel).
        LLViewerParcelMgr& pm = LLViewerParcelMgr::instance();
        const LLParcel* parcel = pm.getAgentParcel();
        LLViewerRegion* here = gAgent.getRegion();
        if (parcel && here && pm.inAgentParcel(here->getPosGlobalFromRegion(root)) && parcel->getSimWideMaxPrimCapacity() > 0
            && parcel->getSimWidePrimCount() + prims > parcel->getSimWideMaxPrimCapacity())
        {
            error = llformat("The land is full: it has room for %d more prims and the tunnel needs %d. Make the tunnel shorter or "
                             "take some objects away, then press Build again. (The tunnel is in your Objects folder.)",
                             llmax(0, parcel->getSimWideMaxPrimCapacity() - parcel->getSimWidePrimCount()), prims);
            return false;
        }
        error = "The region did not put the tunnel down within a minute. Check that you may build on this land and that it has room "
                "for " + llformat("%d", prims) + " more prims; the region's own message, if it sent one, is on your screen. (The "
                "tunnel is in your Objects folder.)";
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
    bool capPostRetry(const std::string& url, const LLSD& body, LLSD& answer, std::string& error);

    bool takeAway(const WolfTunnels::Record& rec, const std::string& cap, const LLUUID& region, bool forget, std::string& error)
    {
        // [WOLF TUNNELS 2026-10-23] Paul (editing a built tunnel): the old one goes in this order - its rock, its holes and
        // walkable parts (a box taken away closes its hole), then its lights - and only then is the new design built.
        if (rec.mBuilt.isMap())
        {
            LLViewerObject* obj = findTunnelObject(rec.mBuilt);
            if (!obj && rec.mBuilt["objects"].size() > 0 && !forget)
            {
                error = "the tunnel's rock is not in view from here. Stand nearer the tunnel and try again.";
                return false;
            }
            if (obj)
            {
                s_jobStatus = "Backfilling the old tunnel: taking its rock away...";
                derez(obj);
            }
            S32 n = 0;
            for (const LLSD& id : llsd::inArray(rec.mBuilt["volumes"]))
            {
                if (!gAgent.getRegion() || gAgent.getRegion()->getRegionID() != region) { error = "you left the region while its walkable boxes were being removed."; return false; }
                s_jobStatus = llformat("Backfilling the old tunnel: closing its holes, removing the walkable boxes (%d of %d)...", ++n, (S32)rec.mBuilt["volumes"].size());
                s_jobProgress = (F32)n / (F32)llmax(1, (S32)rec.mBuilt["volumes"].size());
                LLSD answer;
                std::string err;
                const bool ok = capPostRetry(cap, LLSD().with("op", "remove").with("id", id.asInteger()), answer, err);
                // "that tunnel is not here any more" (WolfTunnelsModule.cs Apply remove): already gone, which is what we want.
                if (!ok && err.find("not here any more") == std::string::npos) { error = "a walkable box could not be removed: " + err; return false; }
                if (answer.has("tunnels")) WolfTunnels::setFromAnswer(answer, region);
            }
        }
        // [WOLF CAVE LIGHTS 2026-10-23] Paul: deleting the cave or tunnel also deletes its lights (a rebuilt tunnel's lamps would
        // hang in the rock). The lamps in view go; parts not in view are said and left, and the record forgets them.
        if (rec.mLights.isMap() && rec.mLights["objects"].size() > 0)
        {
            s_jobStatus = "Taking the tunnel's lights away...";
            s_jobProgress = -1.f;
            const LLSD unseen = WolfCaveLights::takeAwayFor(rec.mLights);
            if (unseen.size() > 0)
                jobFail(llformat("%d light part%s of \"%s\" %s not in view, so %s left where %s: delete %s by hand if %s still there.",
                                 (S32)unseen.size(), unseen.size() == 1 ? "" : "s", rec.mName.c_str(), unseen.size() == 1 ? "was" : "were",
                                 unseen.size() == 1 ? "it is" : "they are", unseen.size() == 1 ? "it is" : "they are",
                                 unseen.size() == 1 ? "it" : "them", unseen.size() == 1 ? "it is" : "they are"));
            std::string e2;
            if (!putRecord(LLSD().with("region", s_recordsRegion).with("op", "lights").with("id", rec.mId).with("lights", LLSD()), e2))
            {
                error = "the tunnel's lights were taken away, but its record could not be updated: " + e2;
                return false;
            }
        }
        return true;
    }

    /** A capability POST, tried again when the region says "one change at a time" (WolfTunnelsModule.cs:268-269, MIN_POST_MS). */
    bool capPostRetry(const std::string& url, const LLSD& body, LLSD& answer, std::string& error)
    {
        for (S32 i = 0;; ++i)
        {
            if (capPost(url, body, answer, error)) return true;
            if (i >= CAP_RETRIES || error.find("one change at a time") == std::string::npos) return false;
            llcoro::suspendUntilTimeout(CAP_RETRY_S);
        }
    }

    // [2026-10-22 simple] Quitting while this runs: LLCoros stops a waiting coroutine (llcoros.cpp:418-439 checkStop throws
    // Stopping / Stopped at the next suspend; :154-179 pumps it at most 10 times at shutdown), so nothing here holds quitting up.
    // What is left behind depends on the step (the report says): before step 3 the tunnel is only in the Objects folder; during the
    // rez wait the rock may appear without the record knowing it; from then on the record is saved after every walkable box, so
    // Build again or Delete finds everything that was made.
    // [WOLF TUNNELS 2026-10-23] A rebuilt tunnel that had lights: after the build the window offers to light it again in the
    // same style (its old lamps were taken away with the old rock).
    struct Relight { std::string mId; LLSD mLights; };
    Relight s_relight;

    void buildCoro(WolfTunnels::Record rec, std::string cap, LLUUID region, std::string region_uuid, S32 max_boxes)
    {
        std::string err;
        s_relight = Relight();
        LLSD old_lights;
        if (const WolfTunnels::Record* was = findRecord(rec.mId)) old_lights = was->mLights;
        auto stop = [&](const std::string& why) { jobFail("Building stopped: " + why); s_jobStatus.clear(); s_jobProgress = 0.f; s_jobBusy = false; ++s_jobSerial; };
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
        s_jobProgress = 0.05f;
        LLSD put;
        put["region"] = region_uuid;
        put["op"] = "put";
        put["tunnel"] = LLSD().with("id", rec.mId).with("name", rec.mName).with("design", designToLLSD(rec.mDesign)).with("built", LLSD());
        if (!putRecord(put, err)) { stop(err); return; }
        // 3. The rock: the proxy builds the STORED design into the Objects folder.
        s_jobStatus = "Step 2 of 5: making the rock (a long tunnel takes a while)...";
        s_jobProgress = 0.1f;
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
        const LLSD& vols = reply["volumes"];
        // [2026-10-22 simple] The region keeps at most `max` walkable boxes (WolfTunnelsModule.cs:53 MAX_BOXES, sent as "max" in every
        // answer, :333): say so before anything is put down, instead of stopping half-way through the boxes.
        // (an old build's boxes were taken away in 1., and each answer there refreshed the store: WolfTunnels::setFromAnswer)
        const S32 free_boxes = max_boxes - (S32)WolfTunnels::boxes().size();
        if (max_boxes > 0 && (S32)vols.size() > free_boxes)
        {
            stop(llformat("this region can keep %d more walkable parts and the tunnel needs %d. Make it shorter, or delete another "
                          "tunnel here first. (The tunnel is in your Objects folder.)", llmax(0, free_boxes), (S32)vols.size()));
            return;
        }
        // 4. Rez it.
        const S32 li = reply["li"].asInteger();
        s_jobStatus = llformat("Step 3 of 5: putting the tunnel down (%d prims)...", li);
        s_jobProgress = 0.6f;
        const LLSD& re = reply["rez_ray_end"];
        const LLSD& rp = reply["root_pos"];
        const LLVector3 ray((F32)re[0].asReal(), (F32)re[1].asReal(), (F32)re[2].asReal());
        const LLVector3 root((F32)rp[0].asReal(), (F32)rp[1].asReal(), (F32)rp[2].asReal());
        LLUUID object;
        if (!rezTunnel(reply["item_id"].asUUID(), folder, ray, root, region, li, object, err)) { stop(err); return; }
        LLSD built;
        built["item"] = reply["item_id"].asString();
        // [2026-10-22] append, not with(0, ...): LLSD::with(Integer) inserts, and on an empty array ImplArray::insert
        // (llsd.cpp:680-691) first resizes to index + 1 - the id plus a stray null, which tunnels.php refused as "a bad id".
        built["objects"] = LLSD::emptyArray();
        built["objects"].append(object.asString());
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
        const S32 want = (S32)vols.size();
        // [2026-10-22 simple] Every box the region did not add is said in plain words (the rock without them pushes you up out of
        // the tunnel), never left silent.
        auto unfinished = [&](S32 made, const std::string& why)
        {
            return llformat("the tunnel's rock is in place, but only %d of its %d walkable parts were made, so you would be pushed up "
                            "out of it. Press Build again to finish it. (", made, want) + why + ")";
        };
        S32 n = 0;
        for (const LLSD& v : llsd::inArray(vols))
        {
            if (!gAgent.getRegion() || gAgent.getRegion()->getRegionID() != region)
            {
                stop(unfinished((S32)built["volumes"].size(), "you left the region"));
                return;
            }
            s_jobStatus = llformat("Step 4 of 5: making it walkable (part %d of %d)...", ++n, want);
            s_jobProgress = 0.65f + 0.3f * (F32)n / (F32)llmax(1, want);
            LLSD answer;
            if (!capPostRetry(cap, LLSD().with("op", "add").with("tunnel", LLSD().with("c", v["c"]).with("s", v["s"]).with("yaw", v["yaw"])), answer, err))
            {
                record("after a walkable part was refused");
                stop(unfinished((S32)built["volumes"].size(), "the region said: " + err));
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
            record("after a walkable part");              // (so a quit or a crash part-way leaves a record of every part made)
        }
        s_jobStatus = "Step 5 of 5: saving...";
        s_jobProgress = 1.f;
        const S32 made = (S32)built["volumes"].size();
        if (made < want)
        {
            stop(unfinished(made, "the region answered without the new parts"));
            return;
        }
        for (const LLSD& w : llsd::inArray(reply["warnings"])) jobFail(w.asString());
        s_jobStatus = "\"" + rec.mName + "\" is built: " + llformat("%.0f m, %d prims. Press Walk in to go inside.", reply["length"].asReal(), li);
        s_jobOk = true;
        if (old_lights.isMap() && old_lights.has("style") && old_lights["objects"].size() > 0) s_relight = Relight{ rec.mId, old_lights };
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
        s_jobProgress = -1.f;
        if (!putRecord(LLSD().with("region", region_uuid).with("op", "delete").with("id", rec.mId), err)) jobFail("The tunnel's record could not be removed: " + err);
        else { s_jobStatus = "\"" + rec.mName + "\" is taken away."; s_jobOk = true; }
        s_jobBusy = false;
        ++s_jobSerial;
    }

    /** The walkable boxes no tunnel record lists (a rock deleted by hand, or a build whose record was never saved):
     *  removed one at a time, as takeAway does. */
    std::vector<S32> leftoverBoxes()
    {
        std::set<S32> owned;
        for (const WolfTunnels::Record& r : s_records)
            if (r.mBuilt.isMap())
                for (const LLSD& id : llsd::inArray(r.mBuilt["volumes"])) owned.insert(id.asInteger());
        std::vector<S32> out;
        for (const WolfTunnels::Box& b : s_boxes) if (!owned.count(b.mId)) out.push_back(b.mId);
        return out;
    }

    void leftoversCoro(std::vector<S32> ids, std::string cap, LLUUID region)
    {
        S32 n = 0;
        for (S32 id : ids)
        {
            if (!gAgent.getRegion() || gAgent.getRegion()->getRegionID() != region) { jobFail("Stopped: you left the region."); break; }
            s_jobStatus = llformat("Removing leftover walkable parts (%d of %d)...", ++n, (S32)ids.size());
            s_jobProgress = (F32)n / (F32)llmax((size_t)1, ids.size());
            LLSD answer;
            std::string err;
            const bool ok = capPostRetry(cap, LLSD().with("op", "remove").with("id", id), answer, err);
            // "that tunnel is not here any more" (WolfTunnelsModule.cs Apply remove): already gone, which is what we want.
            if (!ok && err.find("not here any more") == std::string::npos) { jobFail("A walkable part could not be removed: " + err); break; }
            if (answer.has("tunnels")) WolfTunnels::setFromAnswer(answer, region);
        }
        if (s_jobErrors.empty()) { s_jobStatus = llformat("%d leftover walkable part(s) removed - the ground is whole again there.", n); s_jobOk = true; }
        s_jobBusy = false;
        ++s_jobSerial;
    }

    // ── [WOLF CAVE LIGHTS 2026-10-23] Light up / Remove lights ────────────────────────────────────────────────────────
    /** Where a tunnel's lamps go: its route, its rooms, and the air of the same formulas the generator built it from (Air::eval:
     *  inside distance, floor and clear height; the open mouths count as the end of the air). */
    std::vector<WolfCaveLights::Lamp> tunnelLamps(const WolfTunnels::Design& d, WolfCaveLights::EStyle style)
    {
        const Air air(d, true);
        WolfCaveLights::Route r;
        for (size_t k = 0; k + 1 < d.mP.size(); k += 2) r.mPts.emplace_back(d.mP[k], d.mP[k + 1]);
        r.mHalfW = d.mW * 0.5f;
        std::vector<WolfCaveLights::Room> rooms;
        for (const WolfTunnels::Cavern& c : d.mCaverns) rooms.push_back(WolfCaveLights::Room{ LLVector2(c.mX, c.mY), c.mR });
        // [2026-10-23 subway] the ways up are lit too (their own lines; the air below already holds them, Air's exits)
        std::vector<WolfCaveLights::Route> routes{ r };
        for (const WolfTunnels::Exit& e : d.mExits)
        {
            if (!d.mFlat || e.mP.size() < 4) continue;
            WolfCaveLights::Route xr;
            for (size_t k = 0; k + 1 < e.mP.size(); k += 2) xr.mPts.emplace_back(e.mP[k], e.mP[k + 1]);
            xr.mHalfW = e.mW * 0.5f;
            routes.push_back(xr);
        }
        auto field = [&air](F32 x, F32 y, F32& s, F32& fl, F32& cl)
        {
            const Field f = air.eval(x, y);
            s = llmin(f.s, f.e);
            fl = f.f;
            cl = f.v;
            return true;
        };
        return WolfCaveLights::plan(routes, rooms, field, style, d.mKind == "rail");
    }

    /** `lamps` empty with `remove` = Remove lights. The record's lights are saved through tunnels.php op "lights". */
    void lightsCoro(WolfTunnels::Record rec, std::vector<WolfCaveLights::Lamp> lamps, bool remove, WolfCaveLights::EStyle style,
                    bool warm, bool keep_unseen, LLUUID region, std::string region_uuid)
    {
        auto save = [&](const LLSD& lights, std::string& e)
        {
            return putRecord(LLSD().with("region", region_uuid).with("op", "lights").with("id", rec.mId).with("lights", lights), e);
        };
        auto progress = [](const std::string& line, F32 f) { s_jobStatus = line; s_jobProgress = f; };
        std::string done, err;
        const bool ok = WolfCaveLights::run(rec.mLights, keep_unseen, remove ? nullptr : &lamps, style, warm, rec.mName, region,
                                            save, progress, []() { return false; }, done, err);
        if (ok) { s_jobStatus = done; s_jobOk = true; }
        else { s_jobStatus.clear(); jobFail((remove ? "Removing the lights stopped: " : "Lighting up stopped: ") + err); s_jobOk = false; }
        s_jobProgress = 0.f;
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
    if (n < 2) return n == 0 ? "Click on the ground where the tunnel goes in." : "Click where the tunnel comes out.";
    if (d.mW < W_MIN || d.mW > W_MAX) return llformat("The tunnel must be %.0f to %.0f m wide (Expert).", W_MIN, W_MAX);
    if (d.mH < H_MIN || d.mH > H_MAX) return llformat("The tunnel must be %.1f to %.0f m high (Expert).", H_MIN, H_MAX);
    for (size_t k = 0; k < n; ++k)
        if (d.mP[2 * k] < 0.f || d.mP[2 * k] > region_w || d.mP[2 * k + 1] < 0.f || d.mP[2 * k + 1] > region_w)
            return "A point is outside this region - drag its marker back in.";
    const Route route(d.mP);
    F32 first = 0.f, last = 0.f;
    for (size_t k = 1; k < n; ++k)
    {
        const F32 l = route.mCum[k] - route.mCum[k - 1];
        if (l < SEG_MIN_M) return "Two points are less than half a metre apart - press Undo last point or drag a marker away.";
        if (k == 1) first = l;
        last = l;
    }
    // [2026-10-22 crossroads] Source: wolftunnel.rs validate_design / straight_run: the straight run from each mouth, through
    // points on its line (the floor's own points and levelRoute's).
    auto straight_run = [](const std::vector<LLVector2>& pts) -> F32
    {
        F32 run = 0.f;
        for (size_t k = 1; k < pts.size(); ++k)
        {
            const LLVector2 a = pts[0], b = pts[k], v = b - a;
            const F32 l = v.length();
            if (l < 1e-6f) break;
            bool on = true;
            for (size_t j = 1; j < k && on; ++j)
            {
                const LLVector2 q = pts[j] - a;
                const F32 u = (q * v) / (l * l);
                on = u >= 0.f && u <= 1.f && fabsf(q.mV[0] * v.mV[1] - q.mV[1] * v.mV[0]) / l <= BEND_TOL_M;
            }
            if (!on) break;
            run = l;
        }
        return run;
    };
    std::vector<LLVector2> rev(route.mPts.rbegin(), route.mPts.rend());
    first = llmax(first, straight_run(route.mPts));
    last = llmax(last, straight_run(rev));
    if (first < d.mW)
        return llformat("The way in needs %.0f m of straight tunnel before the first bend - drag that bend further in.", d.mW);
    if (last < d.mW)
        return llformat("The way out needs %.0f m of straight tunnel after the last bend - drag the Out marker further out.", d.mW);
    // Source: wolftunnel.rs validate_design (v2): a height per point, each in range, no piece steeper than 1 in 1.
    if (d.mZ.size() != n) return "The tunnel's floor could not be worked out - press Start over and draw it again.";
    for (F32 z : d.mZ) if (!std::isfinite(z) || z < Z_MIN || z > Z_MAX) return "The tunnel would go deeper than 90 m below sea level - draw it under lower ground.";
    for (size_t k = 1; k < n; ++k)
    {
        if (fabsf(d.mZ[k] - d.mZ[k - 1]) / llmax(1e-6f, route.mCum[k] - route.mCum[k - 1]) > GRADE_MAX)
            return "Part of the floor is steeper than 45 degrees - drag the markers further apart (or set the depths closer in Expert).";
    }
    for (size_t i = 0; i < d.mCaverns.size(); ++i)
    {
        const Cavern& c = d.mCaverns[i];
        if (c.mR < CAVERN_R_MIN || c.mR > CAVERN_R_MAX) return llformat("Room %d must be 3 to 30 m across its middle (Expert).", (S32)i + 1);
        if (c.mH < CAVERN_H_MIN || c.mH > CAVERN_H_MAX) return llformat("Room %d must be 3 to 25 m high.", (S32)i + 1);
        const std::pair<F32, F32> nt = route.nearest(c.mX, c.mY);
        if (nt.first > d.mW * 0.5f) return llformat("Room %d is not on the tunnel - press Undo room and put it on the tunnel.", (S32)i + 1);
        if (nt.second < c.mR + ROUGH_CAVERN_MAX_M || nt.second > route.mLen - c.mR - ROUGH_CAVERN_MAX_M)
            return llformat("Room %d is too near a way in or out - press Undo room and put it further inside.", (S32)i + 1);
        if (c.mX - c.mR < 0.f || c.mY - c.mR < 0.f || c.mX + c.mR > region_w || c.mY + c.mR > region_w)
            return llformat("Room %d reaches outside the region.", (S32)i + 1);
    }
    return std::string();
}

// =====================================================================================================================
// [WOLF TUNNELS 2026-10-22 simple] the worked-out floor
// =====================================================================================================================

namespace
{
    const char* READING = "Reading this region's tunnels...";

    /** What fitRoute worked out. */
    struct Fit
    {
        std::vector<F32> p, z;      // the design's route (the clicks plus the floor's own points) and a floor per point
        std::string problem;        // the one refusal: two fixed floors too far apart in height for the distance between them
        bool bad_end = false;       // that is the way Out (its marker is drawn red)
        F32 steep = 0.f;            // the steepest slope the floor had to take beyond the kind's own (0 = none)
        F32 rise = 0.f, run = 0.f;  // for that piece: the height and the distance between its fixed floors, metres
        bool pinned = false;        // that piece ends at a depth set in Expert
        F32 grade = 0.f;            // the slope the floor was kept to (the kind's, or more where it had to)
        F32 level = 0.f;            // [2026-10-23 subway] a level tunnel's floor between its ramps
    };

    // Source: wolftunnel.rs Air::top (the roof's highest point: h, more for rough).
    F32 roofTop(const WolfTunnels::Design& d) { return d.mH * (1.f + (d.mStyle == "rough" ? ROUGH_H : 0.f)); }
    // Source: wolftunnel.rs warnings() `let limit = if rail { RAIL_GRADE_WARN } else { WALK_GRADE_WARN };` (the slope the
    // generator calls steep for this kind: the floor is kept to it).
    F32 kindGrade(const WolfTunnels::Design& d) { return d.mKind == "rail" ? RAIL_GRADE_WARN : WALK_GRADE_WARN; }
    F32 clampZ(F32 z) { return llclamp(z, Z_MIN, Z_MAX); }

    /**
     * The floor, worked out (TUNNEL_STRANGER_ASSESSMENT.md section 5 "The floor is computed, never drawn"). The fixed floors are
     * the two ends - on the ground ("mouths"), else as deep as the inside - and any depth set by hand in Expert. Between two
     * fixed floors the floor is the straight line between them, lowered wherever that would leave less than `cover` of ground
     * over the roof, and never steeper than `grade` (the kind's slope; more only where the two fixed floors themselves are
     * further apart in height than that slope allows - then the straight line - and a refusal past GRADE_MAX, the generator's
     * limit, wolftunnel.rs validate_design). Near a way in on level ground the floor goes down at `grade` from the mouth: there
     * the cover cannot be had sooner.
     * The lowered floor is the largest `grade`-steep function under the cover line (the cover line raised to the slopes down
     * from both fixed floors, so it can always start and end at them): min over the samples s of cover(s) + grade * |t - s|,
     * two passes along the route. Every function it is the minimum of keeps to `grade`, so it does too, and so does every chord
     * of it - the points kept below (Douglas-Peucker to FIT_TOL_M) never make a piece steeper.
     * The generator takes a floor straight between points (design v2 "pz"), so the floor's bends become points of the route on
     * the straight pieces between the clicks; none within `min_end` of an end (wolftunnel.rs validate_design: the first and
     * last pieces at least as long as the tunnel is wide) or within SEG_MIN_M of another point.
     */
    void fitRoute(LLViewerRegion* rgn, const std::vector<LLVector2>& clicks, const std::vector<F32>& pins, F32 top, F32 cover,
                  F32 grade, bool mouths, F32 min_end, Fit& out, bool flat = false, F32 level_set = std::numeric_limits<F32>::quiet_NaN())
    {
        out = Fit();
        out.grade = grade;
        const size_t m = clicks.size();
        if (!rgn || m == 0) return;
        const LLSurface& land = rgn->getLand();
        auto ground = [&](const LLVector2& p) { return land.resolveHeightRegion(p.mV[0], p.mV[1]); };
        auto loaded = [&](const LLVector2& p) { LLSurfacePatch* pt = land.resolvePatchRegion(p.mV[0], p.mV[1]); return pt && pt->getHasReceivedData(); };
        if (m == 1)
        {
            out.p = { r2(clicks[0].mV[0]), r2(clicks[0].mV[1]) };
            out.z = { r2(clampZ(ground(clicks[0]))) };
            return;
        }
        std::vector<F32> cum(m, 0.f);
        for (size_t k = 1; k < m; ++k) cum[k] = cum[k - 1] + dist_vec(clicks[k], clicks[k - 1]);
        const F32 len = cum.back();
        const F32 step = llmax(FIT_STEP_M, len / (F32)FIT_SAMPLES_MAX);
        // The samples: every click, and points between them at most `step` apart.
        std::vector<F32> T, U;
        std::vector<LLVector2> P;
        std::vector<size_t> at(m, 0);       // the sample of each click
        for (size_t k = 0; k < m; ++k)
        {
            at[k] = T.size();
            T.push_back(cum[k]);
            P.push_back(clicks[k]);
            if (k + 1 == m) break;
            const S32 parts = llmax(1, (S32)ceilf((cum[k + 1] - cum[k]) / step));
            for (S32 i = 1; i < parts; ++i)
            {
                const F32 f = (F32)i / (F32)parts;
                T.push_back(cum[k] + (cum[k + 1] - cum[k]) * f);
                P.push_back(clicks[k] + (clicks[k + 1] - clicks[k]) * f);
            }
        }
        // the floor may be no higher than this for the cover (no limit where this viewer has no ground yet)
        U.resize(T.size());
        for (size_t j = 0; j < T.size(); ++j) U[j] = loaded(P[j]) ? ground(P[j]) - top - cover : 1e30f;
        auto fixedAt = [&](size_t k, bool& pinned) -> bool
        {
            pinned = k < pins.size() && std::isfinite(pins[k]);
            return pinned || k == 0 || k + 1 == m;
        };
        auto fixedZ = [&](size_t k) -> F32
        {
            const F32 g = ground(clicks[k]);
            if (k < pins.size() && std::isfinite(pins[k])) return clampZ(g - pins[k]);
            return clampZ(mouths ? g : g - top - cover);
        };
        std::vector<F32> F(T.size(), 0.f);
        size_t ka = 0;
        bool pa = false;
        fixedAt(0, pa);
        F32 za = fixedZ(0);
        F[0] = za;
        // [2026-10-23 subway] Paul: "keep the tunnel flat, i.e. like a subway". One floor height from ramp to ramp: deep enough
        // to keep `cover` of ground over the roof along the whole route (the lowest of the cover line - the same limit the
        // worked-out floor keeps), or the height set in Expert; from each way in a ramp at the kind's slope down (or up) to it -
        // steeper only where the route is too short for that, refused past GRADE_MAX. Depths set for points do not apply.
        if (flat)
        {
            F32 lvl = level_set;
            if (!std::isfinite(lvl))
            {
                lvl = 1e30f;
                for (F32 u : U) lvl = llmin(lvl, u);
                if (lvl > 1e29f) lvl = llmin(fixedZ(0), fixedZ(m - 1)) - (mouths ? top + cover : 0.f);
            }
            lvl = clampZ(lvl);
            const F32 z0 = mouths ? clampZ(ground(clicks[0])) : lvl, z1 = mouths ? clampZ(ground(clicks[m - 1])) : lvl;
            F32 g = grade;
            const F32 need = (fabsf(z0 - lvl) + fabsf(z1 - lvl)) / llmax(len, 1e-3f);
            if (need > g)
            {
                g = need;
                out.steep = need;
                out.rise = z1 - z0;
                out.run = len;
                if (need > GRADE_MAX)
                {
                    out.problem = llformat("The tunnel is too short to go down %.0f m to the level and back up again - make it longer, or set the level higher in Expert.",
                                           llmax(fabsf(z0 - lvl), fabsf(z1 - lvl)));
                    out.bad_end = true;
                }
            }
            out.grade = g;
            out.level = lvl;
            for (size_t i = 0; i < T.size(); ++i)
            {
                F32 f = lvl;
                f = llclamp(f, z0 - g * T[i], z0 + g * T[i]);
                f = llclamp(f, z1 - g * (len - T[i]), z1 + g * (len - T[i]));
                F[i] = f;
            }
            ka = m - 1;
        }
        for (size_t k = 1; k < m && !flat; ++k)
        {
            bool pk = false;
            if (!fixedAt(k, pk)) continue;
            const F32 zb = fixedZ(k);
            const size_t ia = at[ka], ib = at[k];
            const F32 a = T[ia], b = T[ib], run = llmax(b - a, 1e-3f);
            const F32 need = fabsf(zb - za) / run;
            F32 g = grade;
            if (need > grade)
            {
                g = need;
                if (need > out.steep) { out.steep = need; out.rise = zb - za; out.run = b - a; out.pinned = pa || pk; }
                if (need > GRADE_MAX && out.problem.empty())
                {
                    out.bad_end = k + 1 == m;
                    out.problem = (pa || pk)
                        ? llformat("Two depths set in Expert are %.0f m apart in height but only %.0f m apart - too steep. Set them closer, or drag the markers further apart.",
                                   fabsf(zb - za), b - a)
                        : llformat("The way out is %.0f m %s than the way in but only %.0f m away - too steep. Drag the Out marker further away.",
                                   fabsf(zb - za), zb > za ? "higher" : "lower", b - a);
                }
            }
            out.grade = llmax(out.grade, g);
            // the two passes (see above), from za at a to zb at b
            std::vector<F32> E(ib - ia + 1);
            E[0] = za;
            for (size_t j = 1; j < E.size(); ++j)
            {
                const size_t i = ia + j;
                const F32 lim = i == ib ? zb : llmax(U[i], llmax(za - g * (T[i] - a), zb - g * (b - T[i])));
                E[j] = llmin(lim, E[j - 1] + g * (T[i] - T[i - 1]));
            }
            for (size_t j = E.size() - 1; j-- > 0;) E[j] = llmin(E[j], E[j + 1] + g * (T[ia + j + 1] - T[ia + j]));
            for (size_t j = 0; j < E.size(); ++j)
            {
                const size_t i = ia + j;
                F[i] = llmin(za + (zb - za) * (T[i] - a) / run, E[j]);
            }
            ka = k;
            za = zb;
            pa = pk;
        }
        // The points kept: every click, and the floor's bends (Douglas-Peucker between each two clicks).
        std::vector<char> keep(T.size(), 0);
        for (size_t k = 0; k < m; ++k) keep[at[k]] = 2;
        std::vector<std::pair<size_t, size_t>> todo;
        for (size_t k = 0; k + 1 < m; ++k) todo.emplace_back(at[k], at[k + 1]);
        while (!todo.empty())
        {
            const std::pair<size_t, size_t> r = todo.back();
            todo.pop_back();
            if (r.second <= r.first + 1) continue;
            F32 worst = FIT_TOL_M;
            size_t wj = 0;
            for (size_t j = r.first + 1; j < r.second; ++j)
            {
                const F32 u = (T[j] - T[r.first]) / llmax(1e-6f, T[r.second] - T[r.first]);
                const F32 e = fabsf(F[j] - (F[r.first] + (F[r.second] - F[r.first]) * u));
                if (e > worst) { worst = e; wj = j; }
            }
            if (!wj) continue;
            keep[wj] = 1;
            todo.emplace_back(r.first, wj);
            todo.emplace_back(wj, r.second);
        }
        F32 last_t = -1e30f;
        size_t next_click = 1;
        for (size_t j = 0; j < T.size(); ++j)
        {
            while (next_click < m && at[next_click] <= j) ++next_click;
            if (!keep[j]) continue;
            if (keep[j] == 1)
            {
                const F32 next_t = next_click < m ? cum[next_click] : len;
                if (T[j] < min_end || T[j] > len - min_end || T[j] - last_t < SEG_MIN_M + 0.01f || next_t - T[j] < SEG_MIN_M + 0.01f) continue;
            }
            out.p.push_back(r2(P[j].mV[0]));
            out.p.push_back(r2(P[j].mV[1]));
            out.z.push_back(r2(clampZ(F[j])));
            last_t = T[j];
        }
    }

    /** What levelRoute made of the design. */
    struct Levelled
    {
        bool meets = false;         // the route meets itself somewhere (the preview's Air fills the rock between)
        S32 places = 0;             // places made level, and the most a floor was lowered there
        F32 lowered = 0.f;
        std::string problem;        // the refusal (wolftunnel.rs level's words), empty = none
        bool bad_end = false;       // it is about the way out (its marker is drawn red)
    };

    /** Source: wolftunnel.rs level() (union-find over the route's stations, its `Groups`). */
    struct Groups
    {
        std::vector<size_t> up;
        explicit Groups(size_t m) : up(m) { for (size_t i = 0; i < m; ++i) up[i] = i; }
        size_t root(size_t i) { while (up[i] != i) { up[i] = up[up[i]]; i = up[i]; } return i; }
        void join(size_t a, size_t b) { const size_t ra = root(a), rb = root(b); if (ra != rb) up[llmax(ra, rb)] = llmin(ra, rb); }
    };

    /**
     * [2026-10-22 crossroads] Paul: "if they double back just put in a cross roads". Source: wolftunnel.rs level() and
     * TUNNEL_DESIGN_V1.md "Crossroads" - the same steps and numbers, so the design saved here is the one the generator builds
     * (its `level` finds nothing more to do). Wherever the route meets itself (a hairpin, a crossing, a run beside itself, a bend
     * sharper than 90 degrees) the floor is made level at the lowest floor there, a bend up to 90 degrees that would leave a step
     * over LEVEL_STEP_M is levelled too, the floor beside comes down at the kind's slope and climbs to a mouth (which keeps its
     * height) as steeply as it must, up to GRADE_MAX. The route keeps every point; the levelled floor adds points between them.
     */
    Levelled levelRoute(WolfTunnels::Design& d)
    {
        Levelled out;
        const size_t n = d.mP.size() / 2;
        if (n < 2 || d.mZ.size() != n) return out;
        const Air air(d);
        const Route& route = air.route;
        for (size_t k = 1; k < n; ++k) if (route.mCum[k] - route.mCum[k - 1] < 1e-4f) return out;   // designProblem says why
        // stations
        std::vector<F32> st_t;
        std::vector<size_t> first;
        for (size_t k = 0; k + 1 < n; ++k)
        {
            first.push_back(st_t.size());
            const F32 seg = route.mCum[k + 1] - route.mCum[k];
            const S32 parts = llmax(1, (S32)floorf(seg / STATION_M));
            for (S32 i = 0; i < parts; ++i) st_t.push_back(route.mCum[k] + seg * (F32)i / (F32)parts);
        }
        first.push_back(st_t.size());
        st_t.push_back(route.mLen);
        const size_t m = st_t.size();
        std::vector<LLVector2> pos(m);
        std::vector<F32> f0(m);
        for (size_t i = 0; i < m; ++i) { LLVector2 u; route.at(st_t[i], pos[i], u); f0[i] = air.floorOn(st_t[i]); }
        auto station_at = [&](F32 t) -> size_t
        {
            return llmin((size_t)(std::lower_bound(st_t.begin(), st_t.end(), t) - st_t.begin()), m - 1);
        };
        // where the route meets itself
        const F32 gap = air.joinGap(), rch = air.reach(), meet = 2.f * rch + gap;
        std::map<std::pair<S32, S32>, std::vector<size_t>> grid;
        auto cell = [&](const LLVector2& p) { return std::make_pair((S32)floorf(p.mV[0] / meet), (S32)floorf(p.mV[1] / meet)); };
        for (size_t i = 0; i < m; ++i) grid[cell(pos[i])].push_back(i);
        Groups base(m);
        std::vector<char> met(m, 0);
        for (size_t i = 0; i < m; ++i)
        {
            const std::pair<S32, S32> c = cell(pos[i]);
            for (S32 di = -1; di <= 1; ++di) for (S32 dj = -1; dj <= 1; ++dj)
            {
                auto it = grid.find(std::make_pair(c.first + di, c.second + dj));
                if (it == grid.end()) continue;
                for (size_t j : it->second)
                {
                    if (j <= i) continue;
                    const F32 a = dist_vec(pos[i], pos[j]);
                    if (a < meet && st_t[j] - st_t[i] > LOCAL_K * a + 1e-3f) { base.join(i, j); out.meets = true; met[i] = met[j] = 1; }
                }
            }
        }
        for (const WolfTunnels::Cavern& c : d.mCaverns)
        {
            const F32 tc = route.nearest(c.mX, c.mY).second;
            LLVector2 cp, cu;
            route.at(tc, cp, cu);
            const F32 near_m = air.cavernReach(c) + rch + gap;
            const size_t ic = station_at(tc);
            for (size_t i = 0; i < m; ++i)
            {
                if (dist_vec(pos[i], LLVector2(c.mX, c.mY)) >= near_m) continue;
                if (fabsf(st_t[i] - tc) > LOCAL_K * dist_vec(pos[i], cp) + 1e-3f) { base.join(i, ic); out.meets = true; met[i] = met[ic] = 1; }
            }
        }
        // level, round after round
        const F32 grade = kindGrade(d), rs = rch + WALL_T;
        std::vector<F32> f = f0;
        bool settled = false;
        for (S32 round = 0; round < LEVEL_ROUNDS; ++round)
        {
            Groups groups = base;
            for (size_t k = 1; k + 1 < n; ++k)
            {
                const LLVector2 a = route.dir(k - 1), b = route.dir(k);
                const F32 turn = acosf(llclamp(a * b, -1.f, 1.f));
                if (turn < 1e-3f) continue;
                const size_t iv = first[k];
                F32 reach_l;
                if (turn > F_PI_BY_TWO) reach_l = rs;   // the pieces meet (above) everywhere but at the bend point itself
                else
                {
                    // Source: wolftunnel.rs level(): the floors' step across the slab inside the bend, and each piece's slab
                    // over the other piece's floor out to l, at l from the slopes beside the bend and at every station within l.
                    const F32 g1 = (f[iv] - f[iv - 1]) / llmax(1e-6f, st_t[iv] - st_t[iv - 1]);
                    const F32 g2 = (f[iv + 1] - f[iv]) / llmax(1e-6f, st_t[iv + 1] - st_t[iv]);
                    const F32 l = llmin(rs * tanf(turn * 0.5f) / llmax(1e-6f, cosf(turn)), rs / llmax(1e-6f, sinf(turn)));
                    const F32 across = rs * tanf(turn * 0.5f);
                    const F32 tv = st_t[iv], fv = f[iv];
                    F32 step = llmax(l * (g1 * cosf(turn) - g2), l * (g1 - g2 * cosf(turn)));
                    for (size_t i = station_at(tv - l); i < m; ++i)
                    {
                        const F32 a = st_t[i] - tv;
                        if (a > l) break;
                        if (a > 0.f) step = llmax(step, fv + g1 * a * cosf(turn) - f[i]);
                        if (a < 0.f) step = llmax(step, fv + g2 * a * cosf(turn) - f[i]);
                        if (a > 0.f && a <= across) step = llmax(step, fabsf(f[i] - f[station_at(tv - a)]));
                    }
                    if (step <= LEVEL_STEP_M) continue;
                    reach_l = l + 2.f * STATION_M;   // out to the first station past l, so it is level all the way to l
                }
                const F32 tv = st_t[iv];
                for (size_t i = station_at(tv - reach_l); i < m && st_t[i] <= tv + reach_l; ++i) groups.join(i, iv);
            }
            // Source: wolftunnel.rs level(): a cavern's square floor slab reaches out to its corners; unless every station that
            // far from its centre lies on one straight, evenly sloping run, the floor there is made level.
            for (const WolfTunnels::Cavern& c : d.mCaverns)
            {
                const F32 reach_c = (air.cavernReach(c) + WALL_T) * 1.41421356f + rs;
                std::vector<size_t> under;
                for (size_t i = 0; i < m; ++i) if (dist_vec(pos[i], LLVector2(c.mX, c.mY)) <= reach_c) under.push_back(i);
                if (under.empty()) continue;
                const size_t a = under.front(), b = under.back();
                bool one_run = b - a + 1 == under.size();
                for (size_t k = 1; k + 1 < n && one_run; ++k)
                    if (route.mCum[k] > st_t[a] && route.mCum[k] < st_t[b] && route.dir(k - 1) * route.dir(k) < 1.f - 1e-6f) one_run = false;
                for (size_t i = a; i <= b && one_run; ++i)
                {
                    const F32 u = (st_t[i] - st_t[a]) / llmax(1e-6f, st_t[b] - st_t[a]);
                    if (fabsf(f[i] - (f[a] + (f[b] - f[a]) * u)) > 0.01f) one_run = false;
                }
                bool level_now = true;
                for (size_t i : under) if (fabsf(f[i] - f[a]) > 1e-4f) level_now = false;
                if (!one_run && !level_now) for (size_t i : under) groups.join(i, a);
            }
            std::map<size_t, std::vector<size_t>> members;
            for (size_t i = 0; i < m; ++i) members[groups.root(i)].push_back(i);
            std::vector<F32> next = f;
            std::vector<char> lowered_here(m, 0);
            bool changed = false;
            for (const auto& g : members)
            {
                if (g.second.size() < 2) continue;
                F32 h = 1e30f;
                for (size_t i : g.second) h = llmin(h, f[i]);
                for (size_t i : g.second) if (f[i] - h > 1e-4f) { next[i] = h; lowered_here[i] = 1; changed = true; }
            }
            const size_t mouths[2] = { 0, m - 1 };
            for (S32 e = 0; e < 2; ++e)
            {
                // a mouth opens onto the ground: one that meets another part, or lies in a junction that needs it lower, is refused
                if (met[mouths[e]] || f[mouths[e]] - next[mouths[e]] > 1e-4f)
                {
                    out.problem = llformat("The %s runs into another part of the tunnel, so it cannot reach the ground there - %s the route at least %.0f m away from the rest of the tunnel.",
                                           e ? "way out" : "way in", e ? "end" : "start", meet);
                    out.bad_end = e == 1;
                    return out;
                }
            }
            if (!changed) { settled = true; break; }
            std::vector<F32> cone(m, 1e30f);
            for (size_t i = 0; i < m; ++i) if (lowered_here[i]) cone[i] = next[i];
            for (size_t i = 1; i < m; ++i) cone[i] = llmin(cone[i], cone[i - 1] + grade * (st_t[i] - st_t[i - 1]));
            for (size_t i = m - 1; i-- > 0;) cone[i] = llmin(cone[i], cone[i + 1] + grade * (st_t[i + 1] - st_t[i]));
            for (size_t i = 0; i < m; ++i) if (lowered_here[i]) next[i] = llmin(next[i], cone[i]);
            struct Line { F32 t, z, slope; };
            std::vector<Line> lines;
            for (S32 e = 0; e < 2; ++e)
            {
                const size_t mo = mouths[e];
                if (cone[mo] >= f[mo] - 1e-4f) continue;
                F32 slope = grade;
                for (size_t i = 0; i < m; ++i) if (lowered_here[i]) slope = llmax(slope, (f[mo] - next[i]) / llmax(1e-6f, fabsf(st_t[i] - st_t[mo])));
                if (slope > GRADE_MAX)
                {
                    const char* name = e ? "way out" : "way in";
                    out.problem = llformat("Where the tunnel meets itself or turns sharply it is made level, and from there it cannot climb to the %s at 45 degrees or less - move that place further from the %s.",
                                           name, name);
                    out.bad_end = e == 1;
                    return out;
                }
                lines.push_back({ st_t[mo], f[mo], slope });
            }
            for (size_t i = 0; i < m; ++i)
            {
                if (lowered_here[i]) continue;
                F32 c = cone[i];
                for (const Line& l : lines) c = llmax(c, l.z - l.slope * fabsf(st_t[i] - l.t));
                next[i] = llmin(f[i], c);
            }
            for (F32 z : next)
            {
                if (z < Z_MIN)
                {
                    out.problem = llformat("Levelling the place where the tunnel meets itself would take the floor below %.0f m - draw that part under higher ground.", Z_MIN);
                    return out;
                }
            }
            f = next;
        }
        if (!settled) { out.problem = "The route meets itself in too many places close together - spread the crossings and bends out."; return out; }
        for (size_t i = 0; i < m; ++i)
        {
            out.lowered = llmax(out.lowered, f0[i] - f[i]);
            if (f0[i] - f[i] > 1e-3f && (i == 0 || f0[i - 1] - f[i - 1] <= 1e-3f)) ++out.places;
        }
        if (out.lowered <= 1e-4f) { out.lowered = 0.f; out.places = 0; return out; }
        // back to a route: every point kept, the floor's own bends between
        std::vector<char> keep(m, 0);
        for (size_t k = 0; k < n; ++k) keep[first[k]] = 1;
        std::vector<std::pair<size_t, size_t>> todo;
        for (size_t k = 0; k + 1 < n; ++k) todo.emplace_back(first[k], first[k + 1]);
        while (!todo.empty())
        {
            const std::pair<size_t, size_t> r = todo.back();
            todo.pop_back();
            if (r.second <= r.first + 1) continue;
            F32 worst = LEVEL_TOL_M;
            size_t wi = 0;
            for (size_t i = r.first + 1; i < r.second; ++i)
            {
                const F32 u = (st_t[i] - st_t[r.first]) / llmax(1e-6f, st_t[r.second] - st_t[r.first]);
                const F32 e = fabsf(f[i] - (f[r.first] + (f[r.second] - f[r.first]) * u));
                if (e > worst) { worst = e; wi = i; }
            }
            if (!wi) continue;
            keep[wi] = 1;
            todo.emplace_back(r.first, wi);
            todo.emplace_back(wi, r.second);
        }
        std::vector<F32> p, z;
        size_t k = 0;
        for (size_t i = 0; i < m; ++i)
        {
            if (!keep[i]) continue;
            if (k < n && first[k] == i) { p.push_back(route.mPts[k].mV[0]); p.push_back(route.mPts[k].mV[1]); ++k; }
            else { p.push_back(pos[i].mV[0]); p.push_back(pos[i].mV[1]); }
            z.push_back(f[i]);
        }
        d.mP = p;
        d.mZ = z;
        return out;
    }

    /** The points of a stored route the user clicked: the ends, and every point that is off the straight line between its
     *  neighbours by more than BEND_TOL_M (the others are the floor's own points, fitRoute). */
    std::vector<LLVector2> clicksOf(const WolfTunnels::Design& d)
    {
        std::vector<LLVector2> all, out;
        for (size_t k = 0; k + 1 < d.mP.size(); k += 2) all.emplace_back(d.mP[k], d.mP[k + 1]);
        if (all.size() < 3) return all;
        out.push_back(all[0]);
        for (size_t k = 1; k + 1 < all.size(); ++k)
        {
            const LLVector2 a = out.back(), b = all[k + 1], v = b - a;
            const F32 l = v.length();
            const LLVector2 q = all[k] - a;
            const F32 off = l > 1e-4f ? fabsf(q.mV[0] * v.mV[1] - q.mV[1] * v.mV[0]) / l : q.length();
            if (off > BEND_TOL_M) out.push_back(all[k]);
        }
        out.push_back(all.back());
        return out;
    }

    // An unbuilt drawing kept while the window is closed (the floater is destroyed on close: llfloater.cpp closeFloater ->
    // destroy() unless mReuseInstance), for the next time it opens on the same region.
    struct Kept
    {
        LLUUID mRegion;
        std::string mEditId, mName;
        std::vector<LLVector2> mClicks;
        std::vector<F32> mPins;
        WolfTunnels::Design mDesign;
        F32 mCover = COVER_M;
        bool mMouths = true;
        // [2026-10-23 subway]
        F32 mLevelSet = std::numeric_limits<F32>::quiet_NaN();
        std::vector<F32> mExitAt;
        std::vector<LLVector2> mClosedRooms;
    };
    Kept s_kept;
}

// =====================================================================================================================
// the window
// =====================================================================================================================

WolfFloaterTunnels::WolfFloaterTunnels(const LLSD& key) : LLFloater(key) {}

WolfFloaterTunnels::~WolfFloaterTunnels()
{
    // [2026-10-22 simple] Paul: "if I quit the viewer in tunnel mode it doesn't quit". The pen is told to forget this window
    // first, so putting it away never calls back into a window that is being destroyed (at quit: shutdownViews).
    // [WOLF TERRAFORM 2026-10-23] only when the pen is down for this window (it may be the Terraform window's)
    if (WolfToolTunnel::instanceExists() && WolfToolTunnel::getInstance()->hostIs(this)) WolfToolTunnel::getInstance()->detach();
}

// static
WolfFloaterTunnels* WolfFloaterTunnels::editing()
{
    WolfFloaterTunnels* f = LLFloaterReg::findTypedInstance<WolfFloaterTunnels>("wolf_tunnels");
    return (f && f->getVisible() && !f->isMinimized() && f->mEditing) ? f : nullptr;
}

bool WolfFloaterTunnels::postBuild()
{
    mHint = getChild<LLTextBox>("hint");
    mUndoPointBtn = getChild<LLButton>("undo_point_btn");
    mStartOverBtn = getChild<LLButton>("start_over_btn");
    mTopViewBtn = getChild<LLButton>("top_view_btn");
    mConfirmText = getChild<LLTextBox>("confirm_text");
    mConfirmYes = getChild<LLButton>("confirm_yes");
    mConfirmNo = getChild<LLButton>("confirm_no");
    mRouteInfo = getChild<LLTextBox>("route_info");
    mKind = getChild<LLRadioGroup>("tun_kind");
    mStyle = getChild<LLRadioGroup>("tun_style");
    mRoomBtn = getChild<LLButton>("room_btn");
    mBuildBtn = getChild<LLButton>("build_btn");
    mWalkInBtn = getChild<LLButton>("walk_in_btn");
    mWarnings = getChild<LLTextBox>("tun_warnings");
    mProgress = getChild<LLProgressBar>("build_progress");
    mStatus = getChild<LLTextBox>("status");
    mExpertCheck = getChild<LLCheckBoxCtrl>("expert_check");
    mListCheck = getChild<LLCheckBoxCtrl>("list_check");
    mExpert = getChild<LLPanel>("expert_panel");
    mAdvanced = getChild<LLPanel>("advanced_panel");
    mListPanel = getChild<LLPanel>("list_panel");
    mName = getChild<LLLineEditor>("tun_name");
    mPortals = getChild<LLCheckBoxCtrl>("tun_portals");
    mMouths = getChild<LLCheckBoxCtrl>("tun_mouths");
    mSeeThrough = getChild<LLCheckBoxCtrl>("see_through");
    mAdvancedCheck = getChild<LLCheckBoxCtrl>("advanced_check");
    mW = getChild<LLSpinCtrl>("tun_w");
    mH = getChild<LLSpinCtrl>("tun_h");
    mCover = getChild<LLSpinCtrl>("tun_cover");
    mPointNum = getChild<LLSpinCtrl>("pt_num");
    mPointDepth = getChild<LLSpinCtrl>("pt_depth");
    mPointAutoBtn = getChild<LLButton>("pt_auto");
    mUndoRoomBtn = getChild<LLButton>("undo_room_btn");
    mRoomR = getChild<LLSliderCtrl>("room_r");
    mSeeThroughAmount = getChild<LLSliderCtrl>("see_through_amount");
    mGradeInfo = getChild<LLTextBox>("grade_info");
    mTunList = getChild<LLScrollListCtrl>("tun_list");
    mEditBtn = getChild<LLButton>("edit_btn");
    mGoTunBtn = getChild<LLButton>("go_tunnel_btn");
    mDelTunBtn = getChild<LLButton>("delete_tunnel_btn");
    mLightStyle = getChild<LLComboBox>("lights_style");
    mLightTone = getChild<LLComboBox>("lights_tone");
    mLightUpBtn = getChild<LLButton>("lights_btn");
    mUnlightBtn = getChild<LLButton>("unlights_btn");

    mUndoPointBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { undoPoint(); });
    mStartOverBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onStartOver(); });
    mTopViewBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { lookFromAbove(); });
    mConfirmYes->setClickedCallback([this](LLUICtrl*, const LLSD&) { onStartOverAnswer(true); });
    mConfirmNo->setClickedCallback([this](LLUICtrl*, const LLSD&) { onStartOverAnswer(false); });
    mKind->setCommitCallback([this](LLUICtrl*, const LLSD&) { onKind(); });
    mStyle->setCommitCallback([this](LLUICtrl*, const LLSD&) { onStyle(); });
    mRoomBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onRoomPen(); });
    // [2026-10-23 subway]
    mFlat = getChild<LLCheckBoxCtrl>("tun_flat");
    mExitBtn = getChild<LLButton>("exit_btn");
    mLevel = getChild<LLSpinCtrl>("tun_level");
    mLevelAuto = getChild<LLButton>("tun_level_auto");
    mFlat->setCommitCallback([this](LLUICtrl*, const LLSD&) { onFlat(); });
    mExitBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onExitPen(); });
    mLevel->setCommitCallback([this](LLUICtrl*, const LLSD&) { onLevel(); });
    mLevelAuto->setClickedCallback([this](LLUICtrl*, const LLSD&) { onLevelAuto(); });
    mBuildBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onBuild(); });
    mWalkInBtn->setClickedCallback([this](LLUICtrl*, const LLSD&)
    {
        if (const WolfTunnels::Record* r = findRecord(mEditId)) walkInto(*r);
    });
    mExpertCheck->setCommitCallback([this](LLUICtrl*, const LLSD&) { relayout(); designChanged(); });
    mListCheck->setCommitCallback([this](LLUICtrl*, const LLSD&) { relayout(); });
    mAdvancedCheck->setCommitCallback([this](LLUICtrl*, const LLSD&) { relayout(); });
    mPortals->setCommitCallback([this](LLUICtrl*, const LLSD&) { mDesign.mPortals = mPortals->get(); designChanged(); });
    mMouths->setCommitCallback([this](LLUICtrl*, const LLSD&) { designChanged(); });
    mW->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSize(); });
    mH->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSize(); });
    mCover->setCommitCallback([this](LLUICtrl*, const LLSD&) { designChanged(); });
    mPointNum->setCommitCallback([this](LLUICtrl*, const LLSD&) { onExpertPoint(); });
    mPointDepth->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPointDepth(); });
    mPointAutoBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onPointAuto(); });
    mUndoRoomBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onUndoRoom(); });
    // The box is not saved (on each time the window opens); the amount is (WolfTunnelSeeThrough, a share 0.25..0.75 = 25..75 %).
    mSeeThroughAmount->setValue((F32)ll_round(llclamp(gSavedSettings.getF32("WolfTunnelSeeThrough"), 0.25f, 0.75f) * 100.f));
    mSeeThrough->setCommitCallback([this](LLUICtrl*, const LLSD&) { applySeeThrough(); });
    mSeeThroughAmount->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        gSavedSettings.setF32("WolfTunnelSeeThrough", llclamp((F32)mSeeThroughAmount->getValueF32() / 100.f, 0.25f, 0.75f));
        applySeeThrough();
    });
    mTunList->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPickTunnel(); updateButtons(); });
    mEditBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onEditTunnel(); });
    mGoTunBtn->setClickedCallback([this](LLUICtrl*, const LLSD&)
    {
        if (const WolfTunnels::Record* r = pickedTunnel()) walkInto(*r);
    });
    mDelTunBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onDeleteTunnel(); });
    // [WOLF CAVE LIGHTS 2026-10-23] Paul: "let them choose the light style rather than forcing it" - the last choice is kept
    // (WolfCaveLightStyle / WolfCaveLightTone).
    auto keep_choice = [this](LLUICtrl*, const LLSD&)
    {
        WolfCaveLights::saveChoice(WolfCaveLights::styleFrom(mLightStyle->getValue().asString(), WolfCaveLights::STYLE_LAMPS),
                                   mLightTone->getValue().asString() != "cool");
    };
    mLightStyle->setCommitCallback(keep_choice);
    mLightTone->setCommitCallback(keep_choice);
    mLightUpBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onLightUp(); });
    mUnlightBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onRemoveLights(); });

    // the walkable boxes (Expert)
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
    mLeftoversBtn = getChild<LLButton>("leftovers_btn");
    mLeftoversBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onRemoveLeftovers(); });
    // [2026-10-23] Paul: "make the interface look much more fancy like our uploaders" - the painted working plate lies over the
    // notes line, the old bar and the status line (floater_wolf_tunnels.xml tun_warnings .. status) while a job runs or stopped
    {
        LLRect area = mWarnings->getRect();
        area.unionWith(mStatus->getRect());
        mDig = WolfDigProgress::add(this, area);
    }
    // [WOLF BURROW 2026-10-23] the painted window behind every control (wolfburrow.cpp)
    mKindLbl = getChild<LLView>("kind_lbl");
    mStyleLbl = getChild<LLView>("style_lbl");
    WolfBurrow::Sheet::add(this, [this]() { paintBurrow(); });
    if (getDragHandle()) sendChildToBack(getDragHandle());   // empty paper still drags the window
    relayout();
    return true;
}

// [2026-10-22 simple] Paul: "when I first show it, I just draw the tunnel, and it doesn't require me to create a new tunnel
// first." Opening is drawing: a new drawing (or the unbuilt one from the last time on this region), and in the first draw() the
// Build window is closed, the camera looks down and the pen goes down.
void WolfFloaterTunnels::onOpen(const LLSD& key)
{
    // [WOLF GRID GATE] checked once, on open (no polling): Tunnels are WolfSim's.
    if (!WolfGrid::isOnWolfTerritories())
    {
        mEdit = false;
        mEditing = false;
        s_boxes.clear();
        s_records.clear();
        fillBoxList();
        fillTunnelList();
        setStatus("Tunnels work on Wolf Territories regions only.", true);
        updateButtons();
        return;
    }
    // Shown again while already open (the menu or the Build window's button again): llfloater.cpp openFloater calls onOpen on
    // every show, so the drawing, the camera and the pen are left as they are.
    if (mOpened)
    {
        applySeeThrough();
        mStartPending = true;   // (the Build window is closed again if it is open; the camera stays)
        return;
    }
    mOpened = true;
    if (restoreDrawing()) setStatus("Your unfinished drawing is back. Press Start over to draw a new one.", false);
    else newDrawing();
    mSeeThrough->set(true);   // on each time the window opens (Paul: "just while you use the builder")
    applySeeThrough();
    refresh();
    mStartPending = true;
    mLookPending = true;
    relayout();
}

void WolfFloaterTunnels::onClose(bool app_quitting)
{
    // [2026-10-22 simple] At quit only what must not outlive the window is undone (the pen, its mouse capture, the see-through
    // ground); nothing waits and nothing asks, so the quit goes on at once (llappviewer.cpp idleShutdown waits only for modal
    // dialogs and visible floaters, and this one is hidden by closeFloater). The camera is not turned at quit.
    // [WOLF TERRAFORM 2026-10-23] only when the pen is down for this window (it may be the Terraform window's)
    if (WolfToolTunnel::instanceExists() && WolfToolTunnel::getInstance()->hostIs(this)) WolfToolTunnel::getInstance()->detach();
    mPenWanted = false;
    WolfTunnels::setSeeThrough(0.f);   // the ground is solid again the moment the window closes
    if (app_quitting) return;
    keepDrawing();
    giveCameraBack();
}

void WolfFloaterTunnels::applySeeThrough()
{
    const bool on = mSeeThrough->get() && WolfGrid::isOnWolfTerritories();
    mSeeThroughAmount->setEnabled(on);
    WolfTunnels::setSeeThrough(on ? llclamp((F32)mSeeThroughAmount->getValueF32() / 100.f, 0.25f, 0.75f) : 0.f);
}

void WolfFloaterTunnels::draw()
{
    if (mStartPending && !LLApp::isExiting())
    {
        // Here and not in onOpen: the window may be opened by a button on the Build window itself (floater_tools.xml
        // wolf_tunnels_btn), which is closed here. Its focus would drop the pen at once (llfloatertools.cpp:1619-1621
        // onFocusReceived -> setCurrentToolset -> lltoolmgr.cpp:190-195 setCurrentTool clears the transient tool).
        mStartPending = false;
        if (gFloaterTools && gFloaterTools->getVisible())
        {
            gFloaterTools->closeFloater();
            setStatus("The Build window was closed so that your clicks draw the tunnel.", false);
        }
        if (mEditing && !mJustBuilt && !s_jobBusy)
        {
            if (mLookPending) lookFromAbove();   // after the Build window's own camera reset (llfloatertools.cpp:1282 resetView)
            mPenWanted = true;
        }
        mLookPending = false;
    }
    // the region changed under an open window (teleport, crossing): what it shows belongs to the old one
    LLViewerRegion* rgn = gAgent.getRegion();
    if (rgn && mRegionAtFetch.notNull() && rgn->getRegionID() != mRegionAtFetch && !mBusy && !s_jobBusy)
    {
        mRegionAtFetch.setNull();
        // [WOLF TERRAFORM 2026-10-23] only when the pen is down for this window (it may be the Terraform window's)
        if (WolfToolTunnel::instanceExists() && WolfToolTunnel::getInstance()->hostIs(this)) WolfToolTunnel::getInstance()->cancel();
        if (mEditing && !mClicks.empty()) setStatus("You are on another region now: the drawing was put away.", true);
        s_kept = Kept();
        newDrawing();
        refresh();
    }
    if (mShownRecords != s_recordsSerial) { mShownRecords = s_recordsSerial; fillTunnelList(); }
    // a region change turned the see-through ground off (WolfTunnels onRegionChanged): the box shows it
    if (mSeeThrough->get() && !WolfTunnels::seeThroughOn()) { mSeeThrough->set(false); mSeeThroughAmount->setEnabled(false); }
    // a marker drag: the floor and the preview follow at most every PREVIEW_DRAG_S (a long route's preview is not free)
    if (mPreviewDirty && LLFrameTimer::getElapsedSeconds() - mPreviewAt >= PREVIEW_DRAG_S) designChanged();
    if (s_jobBusy && mStatus->getText() != s_jobStatus) setStatus(s_jobStatus, false);
    if (s_jobBusy && mDig) { mDig->showWork(s_jobStatus, s_jobProgress); mDigSawJob = true; }
    if (s_jobBusy) mProgress->setValue(s_jobProgress * 100.f);
    if (mShownJob != s_jobSerial)
    {
        mShownJob = s_jobSerial;
        if (!s_jobErrors.empty())
        {
            std::string all;
            for (const std::string& e : s_jobErrors) all += (all.empty() ? "" : "\n") + e;
            s_jobErrors.clear();
            setStatus((s_jobStatus.empty() ? "" : s_jobStatus + "\n") + all, !s_jobOk);
            // [2026-10-23] a failure stays on the red plate, selectable, until OK (a note with a finished job is its status)
            if (mDig && !s_jobBusy) { if (!s_jobOk) mDig->showFailed((s_jobStatus.empty() ? "" : s_jobStatus + "\n") + all); else if (mDigSawJob) mDig->showDone(s_jobStatus); mDigSawJob = false; }
        }
        else if (!s_jobStatus.empty())
        {
            setStatus(s_jobStatus, false);
            if (mDig && !s_jobBusy) { if (s_jobOk && mDigSawJob) mDig->showDone(s_jobStatus); else if (!mDig->failedShown()) mDig->hide(); mDigSawJob = false; }
        }
        if (!s_jobBusy && mBuilding)
        {
            mBuilding = false;
            // a build of this drawing ended: on success Walk in; else the drawing is kept and the pen comes back to fix it
            const WolfTunnels::Record* r = findRecord(mEditId);
            mJustBuilt = s_jobOk && r && r->mBuilt.isMap();
            if (mJustBuilt) { s_kept = Kept(); lookAtWayIn(); }
            // [WOLF TUNNELS 2026-10-23] it had lights before the rebuild: offer them again, in the same style
            if (mJustBuilt && s_relight.mId == mEditId && rgn)
            {
                const WolfCaveLights::EStyle st = WolfCaveLights::styleFrom(s_relight.mLights["style"].asString(), WolfCaveLights::STYLE_LAMPS);
                const bool warm = s_relight.mLights["tone"].asString() != "cool";
                const std::string old = WolfCaveLights::summary(s_relight.mLights);
                const WolfTunnels::Record rec = *r;
                const std::vector<WolfCaveLights::Lamp> lamps = tunnelLamps(rec.mDesign, st);
                const LLUUID region = rgn->getRegionID();
                const std::string uuid = regionUuid();
                s_relight = Relight();
                if (!lamps.empty() && !uuid.empty())
                {
                    LLSD args;
                    args["MESSAGE"] = llformat("\"%s\" had %s before it was rebuilt. Light it up again the same way? That is %d %s, %d prim%s.",
                                               rec.mName.c_str(), old.c_str(), (S32)lamps.size(), WolfCaveLights::styleNoun(st).c_str(),
                                               (S32)lamps.size() * WolfCaveLights::partsPerLamp(st),
                                               lamps.size() * WolfCaveLights::partsPerLamp(st) == 1 ? "" : "s");
                    LLNotificationsUtil::add("GenericAlertYesCancel", args, LLSD(), [rec, lamps, st, warm, region, uuid](const LLSD& n, const LLSD& resp)
                    {
                        if (LLNotificationsUtil::getSelectedOption(n, resp) != 0 || s_jobBusy) return;
                        if (!gAgent.getRegion() || gAgent.getRegion()->getRegionID() != region) return;
                        s_jobBusy = true;
                        s_jobOk = false;
                        s_jobErrors.clear();
                        s_jobStatus = "Lighting up \"" + rec.mName + "\" again...";
                        s_jobProgress = -1.f;
                        ++s_jobSerial;
                        LLCoros::instance().launch("WolfTunnelsRelight", [rec, lamps, st, warm, region, uuid]()
                        {
                            lightsCoro(rec, lamps, false, st, warm, false, region, uuid);
                        });
                    });
                }
            }
            else mPenWanted = true;
        }
        fillBoxList();   // a job adds and removes walkable boxes (WolfTunnels::setFromAnswer)
        updateButtons();
    }
    keepPen();
    LLFloater::draw();
}

// [WOLF BURROW 2026-10-23] Paul: "upgrade the whole tunnel floater please i LOVE the mole". Who and why: a region owner making a
// tunnel wants to see at once what to do next and where the main action is. The paper, the header with the mole (digging while a
// job runs), a plate per group under its heading (1 Draw the route, 2 What it is for, 3 Build; the open panels), and every
// button's face by weight: Build tunnel, Walk in and Light up amber (the main actions), the destructive ones with a red rule.
void WolfFloaterTunnels::paintBurrow()
{
    using namespace WolfBurrow;
    const LLRect all = getLocalRect();
    const LLRect client(0, all.mTop - getHeaderHeight(), all.mRight, 0);
    drawSheet(client);
    const S32 L = 4, R = all.mRight - 4;
    drawHeader(LLRect(L, client.mTop - 3, R, client.mTop - 45), "TUNNELS", "Draw it under the hill, build it, walk in.",
               s_jobBusy ? WolfPaint::animSeconds() : 0.f);
    auto wide = [R](LLRect r) { if (!r.isEmpty()) { r.mLeft = L; r.mRight = R; } return r; };
    drawSection(wide(around({ mHint, mUndoPointBtn, mStartOverBtn, mTopViewBtn, mRouteInfo, mConfirmText, mConfirmYes, mConfirmNo }, this, 4, 16)),
                "1  DRAW THE ROUTE");
    drawSection(wide(around({ mKindLbl, mKind, mStyleLbl, mStyle, mRoomBtn, mFlat, mExitBtn }, this, 4, 16)), "2  WHAT IT IS FOR");
    drawSection(wide(around({ mBuildBtn, mWalkInBtn, mWarnings, mStatus }, this, 4, 16)), "3  BUILD");
    drawSection(wide(around({ mExpertCheck, mListCheck }, this, 4, 0)), std::string());
    auto panel = [&](LLPanel* p, const char* heading)
    {
        if (!p || !p->getVisible()) return;
        LLRect r = WolfPaint::rectIn(p, this);
        r.mLeft = L;
        r.mRight = R;
        r.mTop -= 1;
        r.mBottom += 2;
        drawSection(r, heading);
    };
    panel(mExpert, "EXPERT");
    panel(mAdvanced, "WALKABLE PARTS");
    panel(mListPanel, "THIS REGION'S TUNNELS");
    for (LLButton* b : { mBuildBtn, mWalkInBtn, mLightUpBtn }) drawButtonFace(b, this, WEIGHT_PRIMARY);
    for (LLButton* b : { mStartOverBtn, mConfirmYes, mDelTunBtn, mDeleteBtn, mUnlightBtn, mLeftoversBtn }) drawButtonFace(b, this, WEIGHT_DANGER);
    for (LLButton* b : { mUndoPointBtn, mTopViewBtn, mConfirmNo, mRoomBtn, mExitBtn, mPointAutoBtn, mLevelAuto, mUndoRoomBtn, mRefreshBtn,
                         mGoBtn, mSaveBtn, mEditBtn, mGoTunBtn })
        drawButtonFace(b, this, WEIGHT_SECONDARY);
}

void WolfFloaterTunnels::setStatus(const std::string& text, bool error)
{
    if (!mStatus) return;
    mStatus->setText(text);
    // [WOLF BURROW 2026-10-23] fixed colours on the painted plate (the skin's would not hold their contrast on it)
    mStatus->setColor(error ? WolfBurrow::ERR_INK : WolfBurrow::CREAM);
}

// [2026-10-22 simple] The window is BASE_H high with Expert and the tunnels list closed; Expert adds EXPERT_H, the walkable
// boxes (inside Expert) BOXES_H, the tunnels list LIST_H (floater_wolf_tunnels.xml: the floater's height and the panels'
// heights). Build tunnel is in the part that is always shown, so opening a section never moves it. On Paul's screen (2560 x
// 1558 at UI scale 1.35 = 1896 x 1154 UI pixels, TUNNEL_STRANGER_ASSESSMENT.md) everything open is 852: it fits with room.
void WolfFloaterTunnels::relayout()
{
    // [2026-10-23] + the Light up row; [WOLF BURROW 2026-10-23] + the painted header and the groups' headings (440 base) and
    // 22 px of heading at the top of each panel (floater_wolf_tunnels.xml)
    const S32 BASE_H = 464, EXPERT_H = 260, BOXES_H = 250, LIST_H = 140;   // [2026-10-23 subway] + the level row and Expert's level
    const bool expert = mExpertCheck->get();
    const bool boxes = expert && mAdvancedCheck->get();
    const bool list = mListCheck->get();
    mExpert->setVisible(expert);
    mAdvanced->setVisible(boxes);
    mListPanel->setVisible(list);
    // [2026-10-22] The part always shown ends under the Expert / tunnels-list ticks, measured where they really are: on Paul's
    // screen every row sat about 30 px lower than BASE_H allows, the ticks were drawn below the window's edge and could not
    // be clicked, so the tunnels list (Change / Delete) could not be opened. The rows follow the window's top, so their
    // distance from the top is the window's height less a row's bottom.
    const S32 ticks_bottom = getRect().getHeight() - llmin(mExpertCheck->getRect().mBottom, mListCheck->getRect().mBottom);
    const S32 base = llmax(BASE_H, ticks_bottom + 4);
    const S32 want = base + (expert ? EXPERT_H : 0) + (boxes ? BOXES_H : 0) + (list ? LIST_H : 0);
    if (getRect().getHeight() != want)
    {
        // Grow or shrink downwards, keeping the window's top where it is.
        LLRect r = getRect();
        r.mBottom = r.mTop - want;
        setShape(r);
    }
    // The open sections one under the other, from the top (a child's origin is its bottom left in the window's frame).
    S32 top = base;
    auto place = [&](LLPanel* p, bool shown, S32 h)
    {
        if (!shown) return;
        p->setOrigin(0, want - top - h);
        top += h;
    };
    place(mExpert, expert, EXPERT_H);
    place(mAdvanced, boxes, BOXES_H);
    place(mListPanel, list, LIST_H);
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

// ── the camera ────────────────────────────────────────────────────────────────────────────────────────────────────────

// [2026-10-22 simple] TUNNEL_STRANGER_ASSESSMENT.md: from the side a click lands on the near face of the hill
// (llviewerwindow.cpp mousePointOnLandGlobal meets the first ground on the ray), not where the user looks. From above the ray
// meets the ground at the spot under the pointer. Source: wolfadventure.cpp WolfAdventure camera cue (setFocusOnAvatar(false)
// then llagentcamera.cpp:3017-3062 setCameraPosAndFocusGlobal, which animates to it).
void WolfFloaterTunnels::lookFromAbove()
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn || LLApp::isExiting()) return;
    if (gAgentCamera.cameraMouselook()) gAgentCamera.changeCameraToDefault();
    if (!mCamSaved)
    {
        mCamSaved = true;
        mCamWasOnAvatar = gAgentCamera.getFocusOnAvatar();
        mCamPos = gAgentCamera.getCameraPositionGlobal();
        mCamFocus = gAgentCamera.getFocusTargetGlobal();
    }
    // Over the drawing when there is one, else over the avatar; high enough for the whole drawing to show.
    LLVector2 c;
    F32 span = 0.f;
    if (!mClicks.empty())
    {
        LLVector2 lo = mClicks[0], hi = mClicks[0];
        for (const LLVector2& p : mClicks)
        {
            lo.set(llmin(lo.mV[0], p.mV[0]), llmin(lo.mV[1], p.mV[1]));
            hi.set(llmax(hi.mV[0], p.mV[0]), llmax(hi.mV[1], p.mV[1]));
        }
        c = (lo + hi) * 0.5f;
        span = llmax(hi.mV[0] - lo.mV[0], hi.mV[1] - lo.mV[1]);
    }
    else
    {
        const LLVector3 me = rgn->getPosRegionFromGlobal(gAgent.getPositionGlobal());
        c.set(me.mV[VX], me.mV[VY]);
    }
    const F32 g = rgn->getLand().resolveHeightRegion(c.mV[0], c.mV[1]);
    // the view's height (llcamera.h getView: the vertical field of view) shows the drawing and a fifth more round it
    const F32 half = llmax(0.05f, tanf(LLViewerCamera::getInstance()->getView() * 0.5f));
    const F32 h = llmax(TOP_VIEW_M, 0.6f * span / half);
    const LLVector3d focus = rgn->getPosGlobalFromRegion(LLVector3(c.mV[0], c.mV[1], g));
    const LLVector3d cam = focus + LLVector3d(0.0, -(F64)(h * TOP_VIEW_TILT), (F64)h);
    gAgentCamera.setFocusOnAvatar(false, false);
    gAgentCamera.setCameraPosAndFocusGlobal(cam, focus, LLUUID::null);
    mCamFocusSet = focus;
}

// After a build: the camera flies to the way in, from outside it (the assessment: "the camera flies to the entrance").
void WolfFloaterTunnels::lookAtWayIn()
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn || mDesign.mP.size() < 4 || mDesign.mZ.empty() || LLApp::isExiting()) return;
    if (!mCamSaved)
    {
        mCamSaved = true;
        mCamWasOnAvatar = gAgentCamera.getFocusOnAvatar();
        mCamPos = gAgentCamera.getCameraPositionGlobal();
        mCamFocus = gAgentCamera.getFocusTargetGlobal();
    }
    LLVector2 in(mDesign.mP[0], mDesign.mP[1]), next(mDesign.mP[2], mDesign.mP[3]);
    LLVector2 u = next - in;
    u.normVec();
    // a little way inside the mouth, from 15 m out and 6 m up (the window's own choice of a view that shows the arch)
    const LLVector3 f(in.mV[0] + u.mV[0] * 2.f, in.mV[1] + u.mV[1] * 2.f, mDesign.mZ[0] + mDesign.mH * 0.5f);
    const LLVector3 c(in.mV[0] - u.mV[0] * 15.f, in.mV[1] - u.mV[1] * 15.f, mDesign.mZ[0] + 6.f);
    const LLVector3d focus = rgn->getPosGlobalFromRegion(f);
    gAgentCamera.setFocusOnAvatar(false, false);
    gAgentCamera.setCameraPosAndFocusGlobal(rgn->getPosGlobalFromRegion(c), focus, LLUUID::null);
    mCamFocusSet = focus;
}

// Source: wolfadventure.cpp WolfAdventure::endCamera - given back unless the user has taken the camera meanwhile (an
// alt-click, a teleport): then the focus target is no longer the one set last (llagentcamera.cpp:3030 stores it) and it is left.
void WolfFloaterTunnels::giveCameraBack()
{
    if (!mCamSaved) return;
    mCamSaved = false;
    const bool still_ours = !gAgentCamera.getFocusOnAvatar() && (gAgentCamera.getFocusTargetGlobal() - mCamFocusSet).magVecSquared() < 0.01;
    if (!still_ours) return;
    if (mCamWasOnAvatar) gAgentCamera.setFocusOnAvatar(true, true, false);
    else gAgentCamera.setCameraPosAndFocusGlobal(mCamPos, mCamFocus, LLUUID::null);
}

// ── the pen ───────────────────────────────────────────────────────────────────────────────────────────────────────────

bool WolfFloaterTunnels::penAllowed() const
{
    if (!mEditing || s_jobBusy || mJustBuilt || !getVisible() || isMinimized() || !WolfGrid::isOnWolfTerritories()) return false;
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return false;
    if (mCapUrl.empty() && !mBusy) return false;                                        // no WolfTunnels here
    if (mRegionAtFetch.notNull() && mRegionAtFetch == rgn->getRegionID() && !mEdit) return false;   // no right to
    return true;
}

void WolfFloaterTunnels::keepPen()
{
    if (!WolfToolTunnel::instanceExists() && !mPenWanted) return;
    WolfToolTunnel* t = WolfToolTunnel::getInstance();
    if (!mPenWanted || !penAllowed() || LLApp::isExiting())
    {
        // [WOLF TERRAFORM 2026-10-23] only this window's pen (the Terraform window may have it down for itself)
        if (t->pen() != WolfToolTunnel::PEN_NONE && t->hostIs(this)) t->cancel();
        return;
    }
    if (t->current()) return;
    // Not while something else has the mouse: the Build window (the hint says to close it), another transient tool (a drag
    // from the inventory: lltooldraganddrop.cpp), a mouse capture, mouselook.
    if (gFloaterTools && gFloaterTools->getVisible()) return;
    if (LLToolMgr::getInstance()->usingTransientTool() || gFocusMgr.getMouseCapture() || gAgentCamera.cameraMouselook()) return;
    t->begin(getHandle(), WolfToolTunnel::PEN_ROUTE);
}

void WolfFloaterTunnels::penStopped()
{
    updateButtons();
}

// [WOLF TERRAFORM 2026-10-23] WolfRouteHost: the words the pen said here before it served other windows too (unchanged).
std::string WolfFloaterTunnels::noGroundText(bool underground) const
{
    return underground ? "Point at the ground inside this region (you are under the ground)."
                       : "Click on the ground of the region you are in - a tunnel stays inside one region.";
}

std::string WolfFloaterTunnels::escapeText() const
{
    return "You are drawing a tunnel. Backspace takes the last point back; closing the Tunnels window stops.";
}

F32 WolfFloaterTunnels::planeHeight() const
{
    if (!mDesign.mZ.empty()) return mDesign.mZ.back();
    // Nothing drawn yet: about where the avatar stands (its position is about a metre over its feet).
    LLViewerRegion* rgn = gAgent.getRegion();
    return rgn ? rgn->getPosRegionFromAgent(gAgent.getPositionAgent()).mV[VZ] - 1.f : 0.f;
}

// ── the drawing ───────────────────────────────────────────────────────────────────────────────────────────────────────

// [2026-10-22 simple] Paul's saved "Rail" and "Rough" (WolfTunnelKind / WolfTunnelStyle) were applied without a word and made
// every limit 6 m (TUNNEL_STRANGER_ASSESSMENT.md section 1): a new drawing is Path + Built, and Expert's numbers are at their
// defaults, every time.
void WolfFloaterTunnels::newDrawing()
{
    if (WolfToolTunnel::instanceExists() && WolfToolTunnel::getInstance()->pen() == WolfToolTunnel::PEN_ROOM) WolfToolTunnel::getInstance()->cancel();
    mEditing = true;
    mEditId.clear();
    mClicks.clear();
    mPins.clear();
    mJustBuilt = false;
    mConfirming = false;
    mDesign = WolfTunnels::Design();
    mDesign.mKind = "walk";
    mDesign.mW = 4.f;
    mDesign.mH = 3.5f;
    mDesign.mStyle = "smooth";
    mDesign.mRock = WolfCaves::terrainRockId();
    mDesign.mSeed = ll_rand(SEED_MAX);
    mDesign.mPortals = true;
    mDesign.mFlat = false;   // [2026-10-23 subway]
    mLevelSet = std::numeric_limits<F32>::quiet_NaN();
    mExitAt.clear();
    mClosedRooms.clear();
    mExitNotes.clear();
    if (mName) mName->setText(llformat("Tunnel %d", (S32)s_records.size() + 1));
    if (mCover) mCover->setValue(COVER_M);
    if (mMouths) mMouths->set(true);
    showDesign();
    designChanged();
}

void WolfFloaterTunnels::keepDrawing()
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!mEditing || mJustBuilt || mClicks.empty() || !rgn) { s_kept = Kept(); return; }
    s_kept.mRegion = rgn->getRegionID();
    s_kept.mEditId = mEditId;
    s_kept.mName = mName->getText();
    s_kept.mClicks = mClicks;
    s_kept.mPins = mPins;
    s_kept.mDesign = mDesign;
    s_kept.mCover = (F32)mCover->getValueF32();
    s_kept.mMouths = mMouths->get();
    s_kept.mLevelSet = mLevelSet;
    s_kept.mExitAt = mExitAt;
    s_kept.mClosedRooms = mClosedRooms;
}

bool WolfFloaterTunnels::restoreDrawing()
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn || s_kept.mClicks.empty() || s_kept.mRegion != rgn->getRegionID()) { s_kept = Kept(); return false; }
    mEditing = true;
    mJustBuilt = false;
    mConfirming = false;
    mEditId = s_kept.mEditId;
    mName->setText(s_kept.mName);
    mClicks = s_kept.mClicks;
    mPins = s_kept.mPins;
    mDesign = s_kept.mDesign;
    mCover->setValue(s_kept.mCover);
    mMouths->set(s_kept.mMouths);
    mLevelSet = s_kept.mLevelSet;
    mExitAt = s_kept.mExitAt;
    mClosedRooms = s_kept.mClosedRooms;
    s_kept = Kept();
    showDesign();
    designChanged();
    return true;
}

void WolfFloaterTunnels::showDesign()
{
    if (!mKind) return;
    mKind->setValue(mDesign.mKind == "wide" || mDesign.mKind == "rail" ? mDesign.mKind : std::string("walk"));
    mStyle->setValue(mDesign.mStyle);
    mW->setValue(mDesign.mW);
    mH->setValue(mDesign.mH);
    mPortals->set(mDesign.mPortals);
    if (mFlat) mFlat->set(mDesign.mFlat);   // [2026-10-23 subway]
}

void WolfFloaterTunnels::onStartOver()
{
    // Starting over asks first when there is a drawing to lose (in the window: a modal box would hold a quit up,
    // llappviewer.cpp idleShutdown "Wait for all modal alerts to get resolved").
    if (mClicks.size() >= 2 && !mJustBuilt)
    {
        mConfirming = true;
        updateButtons();
        return;
    }
    newDrawing();
    lookFromAbove();
    mPenWanted = true;
    setStatus(std::string(), false);
}

void WolfFloaterTunnels::onStartOverAnswer(bool yes)
{
    mConfirming = false;
    if (!yes) { updateButtons(); return; }
    const bool was_edit = editingBuilt();
    newDrawing();
    mPenWanted = true;
    setStatus(was_edit ? "Editing stopped: the built tunnel is as it was." : "The drawing was cleared.", false);
}

// [2026-10-22 simple] What the tunnel is for: Path (walking), Road (wide), Railway (wolftunnel.rs: only "rail" changes the
// slope, RAIL_GRADE_WARN). Each sets its size (Paul's brief: walking 4 m, wide 6 m, rail 6 x 6); Expert's Width and Height
// change the size and keep the use (the design's "kind", TUNNEL_DESIGN_V1.md: only the slope warning reads it).
void WolfFloaterTunnels::onKind()
{
    mDesign.mKind = mKind->getValue().asString();
    if (mDesign.mKind == "wide") { mDesign.mW = 6.f; mDesign.mH = 4.5f; }
    else if (mDesign.mKind == "rail") { mDesign.mW = 6.f; mDesign.mH = 6.f; }
    else { mDesign.mKind = "walk"; mDesign.mW = 4.f; mDesign.mH = 3.5f; }
    showDesign();
    if (keepEndsLongEnough()) setStatus(llformat("A marker was moved out so the tunnel's ends are at least %.0f m long (as wide as the tunnel).", mDesign.mW), false);
    designChanged();
}

void WolfFloaterTunnels::onStyle()
{
    mDesign.mStyle = mStyle->getValue().asString() == "rough" ? "rough" : "smooth";
    designChanged();   // (a rough roof is higher: wolftunnel.rs Air::top)
}

void WolfFloaterTunnels::onSize()
{
    mDesign.mW = llclamp((F32)mW->getValueF32(), W_MIN, W_MAX);
    mDesign.mH = llclamp((F32)mH->getValueF32(), H_MIN, H_MAX);
    if (keepEndsLongEnough()) setStatus(llformat("A marker was moved out so the tunnel's ends are at least %.0f m long (as wide as the tunnel).", mDesign.mW), false);
    designChanged();
}

bool WolfFloaterTunnels::keepEndsLongEnough()
{
    LLViewerRegion* rgn = gAgent.getRegion();
    const size_t n = mClicks.size();
    if (!rgn || n < 2) return false;
    const F32 w = rgn->getWidth(), need = mDesign.mW;
    bool moved = false;
    // `moving` goes out along the line from `from` until the piece is `need` long (wolftunnel.rs validate_design, first/last).
    auto push = [&](size_t from, size_t moving)
    {
        const LLVector2 d = mClicks[moving] - mClicks[from];
        const F32 len = d.length();
        if (len >= need || len < 1e-3f) return;
        const LLVector2 p = mClicks[from] + d * (need / len);
        if (p.mV[0] < 0.f || p.mV[1] < 0.f || p.mV[0] > w || p.mV[1] > w) return;   // (the line under Build says so)
        mClicks[moving] = p;
        moved = true;
    };
    push(0, 1);
    push(n - 2, n - 1);
    return moved;
}

void WolfFloaterTunnels::clickAt(F32 x, F32 y)
{
    if (!mEditing || s_jobBusy || mJustBuilt) return;
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return;
    WolfToolTunnel* t = WolfToolTunnel::getInstance();
    if (t->pen() == WolfToolTunnel::PEN_EXIT) { exitClickAt(x, y); return; }   // [2026-10-23 subway]
    if (t->pen() == WolfToolTunnel::PEN_ROOM)
    {
        // One room per press of Add a room, then the pen draws the route again.
        t->setPen(WolfToolTunnel::PEN_ROUTE);
        if (mClicks.size() < 2) { setStatus("Draw the tunnel first: a room goes on it.", true); return; }
        // [WOLF TUNNELS 2026-10-23] a click inside a room that is there takes it away (editing a tunnel's rooms)
        for (size_t i = 0; i < mDesign.mCaverns.size(); ++i)
        {
            const WolfTunnels::Cavern& c = mDesign.mCaverns[i];
            if (dist_vec(LLVector2(x, y), LLVector2(c.mX, c.mY)) <= c.mR)
            {
                mDesign.mCaverns.erase(mDesign.mCaverns.begin() + i);
                setStatus(llformat("Room %d was taken away.", (S32)i + 1), false);
                designChanged();
                return;
            }
        }
        const Route route(mDesign.mP);
        const std::pair<F32, F32> nt = route.nearest(x, y);
        const F32 r = llclamp((F32)mRoomR->getValueF32(), CAVERN_R_MIN, CAVERN_R_MAX);
        if (nt.first > llmax(mDesign.mW * 2.f, r)) { setStatus("Click on the tunnel's line to put a room there.", true); return; }
        LLVector2 p, u;
        route.at(nt.second, p, u);
        WolfTunnels::Cavern c;
        c.mX = r2(p.mV[0]);
        c.mY = r2(p.mV[1]);
        c.mR = r;
        // A room is taller than the tunnel, and rises with its size.
        c.mH = r2(llclamp(llmax(mDesign.mH + 1.f, c.mR * 0.8f), CAVERN_H_MIN, CAVERN_H_MAX));
        const size_t before = mDesign.mCaverns.size();
        mDesign.mCaverns.push_back(c);
        fitRooms();
        if (mDesign.mCaverns.size() == before) setStatus("A room does not fit that close to a way in or out - click further inside.", true);
        else setStatus(llformat("Room %d added (%.0f m across).", (S32)mDesign.mCaverns.size(), mDesign.mCaverns.back().mR * 2.f), false);
        designChanged();
        return;
    }
    const F32 w = rgn->getWidth();
    LLVector2 p(x, y);
    std::string note;
    if (!mClicks.empty())
    {
        const LLVector2 prev = mClicks.back(), d = p - prev;
        const F32 len = d.length();
        // The second press of a double-click lands on the point the first one made: the same click (SEG_MIN_M, the
        // generator's shortest piece).
        if (len < SEG_MIN_M) return;
        // [2026-10-22 simple] Too close: moved out along the same line to the shortest end piece the generator takes (as wide as
        // the tunnel, wolftunnel.rs validate_design), instead of refused.
        if (len < mDesign.mW)
        {
            p = prev + d * (mDesign.mW / len);
            if (p.mV[0] < 0.f || p.mV[1] < 0.f || p.mV[0] > w || p.mV[1] > w)
            {
                setStatus("That is too near the region's edge for the tunnel's end - click a little further from the edge.", true);
                return;
            }
            note = llformat("That click was moved out to %.0f m from the last one, the shortest a tunnel's end can be.", mDesign.mW);
        }
    }
    mClicks.push_back(p);
    mPins.push_back(std::numeric_limits<F32>::quiet_NaN());
    setStatus(note, false);
    designChanged();
}

void WolfFloaterTunnels::moveClick(S32 k, F32 x, F32 y, bool done)
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!mEditing || !rgn || k < 0 || (size_t)k >= mClicks.size()) return;
    const F32 w = rgn->getWidth();
    mClicks[k].set(llclamp(x, 0.f, w), llclamp(y, 0.f, w));
    if (!done) { mPreviewDirty = true; return; }
    if (keepEndsLongEnough()) setStatus(llformat("The marker was moved out so the tunnel's end is at least %.0f m long.", mDesign.mW), false);
    designChanged();
}

void WolfFloaterTunnels::undoPoint()
{
    if (mClicks.empty() || s_jobBusy || mJustBuilt) return;
    mClicks.pop_back();
    if (mPins.size() > mClicks.size()) mPins.resize(mClicks.size());
    setStatus(mClicks.empty() ? std::string("Click on the ground where the tunnel goes in.") : std::string("The last point was taken back."), false);
    designChanged();
}

void WolfFloaterTunnels::removeClick(S32 k)
{
    if (!mEditing || s_jobBusy || mJustBuilt || k < 0 || (size_t)k >= mClicks.size()) return;
    if (mClicks.size() <= 2) { setStatus("A tunnel needs its way in and its way out - drag them instead, or press Undo last point.", true); return; }
    mClicks.erase(mClicks.begin() + k);
    if ((size_t)k < mPins.size()) mPins.erase(mPins.begin() + k);
    if (mPins.size() > mClicks.size()) mPins.resize(mClicks.size());
    setStatus(k == 0 ? std::string("The way in was taken out: the next point is the way in now.")
                     : (size_t)k == mClicks.size() ? std::string("The way out was taken out: the point before it is the way out now.")
                                                     : llformat("Bend %d was taken out.", k), false);
    designChanged();
}

void WolfFloaterTunnels::insertClickNear(F32 x, F32 y)
{
    if (!mEditing || s_jobBusy || mJustBuilt) return;
    if (mClicks.size() < 2) { clickAt(x, y); return; }
    // the piece of the drawn route nearest the click (between clicked points, not the floor's own points)
    size_t best = 1;
    F32 best_d = 1e30f;
    const LLVector2 q(x, y);
    for (size_t i = 1; i < mClicks.size(); ++i)
    {
        const LLVector2 a = mClicks[i - 1], v = mClicks[i] - mClicks[i - 1];
        const F32 l2 = v.lengthSquared();
        const F32 u = l2 > 1e-9f ? llclamp(((q - a) * v) / l2, 0.f, 1.f) : 0.f;
        const F32 d = dist_vec(a + v * u, q);
        if (d < best_d) { best_d = d; best = i; }
    }
    if (best_d > llmax(mDesign.mW * 2.f, 6.f)) { setStatus("Ctrl+click on the tunnel's line to put a bend there.", true); return; }
    if (dist_vec(q, mClicks[best - 1]) < SEG_MIN_M || dist_vec(q, mClicks[best]) < SEG_MIN_M) return;
    mClicks.insert(mClicks.begin() + best, q);
    if (mPins.size() >= best) mPins.insert(mPins.begin() + best, std::numeric_limits<F32>::quiet_NaN());
    setStatus(llformat("A bend was put in between point %d and point %d - drag it where you want it.", (S32)best, (S32)best + 1), false);
    designChanged();
}

bool WolfFloaterTunnels::editingBuilt() const
{
    const WolfTunnels::Record* r = mEditId.empty() ? nullptr : findRecord(mEditId);
    return mEditing && !mJustBuilt && r && r->mBuilt.isMap();
}

void WolfFloaterTunnels::onRoomPen()
{
    WolfToolTunnel* t = WolfToolTunnel::getInstance();
    if (t->pen() == WolfToolTunnel::PEN_ROOM) { t->setPen(WolfToolTunnel::PEN_ROUTE); updateButtons(); return; }
    if (mClicks.size() < 2) { setStatus("Draw the tunnel first: a room goes on it.", true); return; }
    if (t->current()) t->setPen(WolfToolTunnel::PEN_ROOM);
    else t->begin(getHandle(), WolfToolTunnel::PEN_ROOM);
    setStatus("Click on the tunnel's line to put a room there.", false);
    updateButtons();
}

void WolfFloaterTunnels::onUndoRoom()
{
    if (mDesign.mCaverns.empty()) return;
    mDesign.mCaverns.pop_back();
    designChanged();
}

// ── [2026-10-23 subway] Keep it level, and the ways up ───────────────────────────────────────────────────────────────
// Paul: "add an option to tunneling to keep the tunnel flat, i.e. like a subway", then "with a subway they do need paths to the
// surface as well". Who and why: a region owner building a subway wants level platforms and a way up from every station.
// A way up (wolftunnel.rs TExit) is a straight ramp: from inside the tunnel (its line, or a room's middle) out across the
// tunnel's or the room's edge on a short level stub (EXIT_STUB_M past it), then up at EXIT_GRADE until its floor meets the
// ground, where its mouth opens (with a stone arch when the tunnel has them). It goes off to whichever side keeps it clear of the
// tunnel's line, the rooms and the other ways up; the rooms have one each unless closed, others are added on the line.

void WolfFloaterTunnels::placeExits()
{
    mDesign.mExits.clear();
    mExitNotes.clear();
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!mDesign.mFlat || !rgn || mDesign.mP.size() < 4 || mDesign.mZ.size() * 2 != mDesign.mP.size()) return;
    const LLSurface& land = rgn->getLand();
    const F32 w = rgn->getWidth();
    const Air main(mDesign);
    const F32 ew = llmin(EXIT_W_M, mDesign.mW);
    auto loaded = [&](const LLVector2& p) { LLSurfacePatch* pt = land.resolvePatchRegion(p.mV[0], p.mV[1]); return pt && pt->getHasReceivedData(); };
    // one way up from `foot` (inside the tunnel) towards `dir`, its level stub reaching `edge` + EXIT_STUB_M from the foot
    auto make = [&](const LLVector2& foot, LLVector2 dir, F32 edge, S32 room, std::string& why) -> bool
    {
        dir.normVec();
        const F32 zf = main.floorOn(main.route.nearest(foot.mV[0], foot.mV[1]).second);
        const LLVector2 stub = foot + dir * (edge + EXIT_STUB_M);
        F32 d = 0.f;
        LLVector2 top;
        F32 zt = zf;
        bool reached = false;
        for (; d <= 400.f; d += 0.25f)
        {
            top = stub + dir * d;
            if (top.mV[0] < 1.f || top.mV[1] < 1.f || top.mV[0] > w - 1.f || top.mV[1] > w - 1.f) { why = "it would leave the region"; return false; }
            if (!loaded(top)) { why = "this viewer has not loaded the ground there yet"; return false; }
            const F32 g = land.resolveHeightRegion(top.mV[0], top.mV[1]);
            zt = zf + EXIT_GRADE * d;
            if (zt >= g && d >= ew) { zt = g; reached = true; break; }
        }
        if (!reached) { why = "the ground is too far above it"; return false; }
        // clear of the tunnel's line, the other rooms and the other ways up, beyond its own start
        for (F32 q = 2.f; q <= d; q += 1.f)
        {
            const LLVector2 a = stub + dir * q;
            if (main.route.nearest(a.mV[0], a.mV[1]).first < main.reach() + ew * 0.5f + 0.5f) { why = "it would run into the tunnel"; return false; }
            for (size_t i = 0; i < mDesign.mCaverns.size(); ++i)
            {
                const WolfTunnels::Cavern& c = mDesign.mCaverns[i];
                if ((S32)i != room && dist_vec(a, LLVector2(c.mX, c.mY)) < c.mR + ew * 0.5f + 0.5f) { why = "it would run into a room"; return false; }
            }
            for (const WolfTunnels::Exit& o : mDesign.mExits)
            {
                const LLVector2 o0(o.mP[2], o.mP[3]), o1(o.mP[4], o.mP[5]);
                const LLVector2 v = o1 - o0;
                const F32 l2 = llmax(v.lengthSquared(), 1e-6f);
                const F32 u = llclamp(((a - o0) * v) / l2, 0.f, 1.f);
                if (dist_vec(a, o0 + v * u) < ew + EXIT_CLEAR_M) { why = "it would run into another way up"; return false; }
            }
        }
        WolfTunnels::Exit e;
        e.mP = { r2(foot.mV[0]), r2(foot.mV[1]), r2(stub.mV[0]), r2(stub.mV[1]), r2(top.mV[0]), r2(top.mV[1]) };
        e.mZ = { r2(zf), r2(zf), r2(zt) };
        e.mW = ew;
        e.mRoom = room;
        mDesign.mExits.push_back(e);
        return true;
    };
    auto both_ways = [&](const LLVector2& foot, const LLVector2& across, F32 edge, S32 room, const std::string& name)
    {
        if ((S32)mDesign.mExits.size() >= MAX_EXITS)
        {
            mExitNotes.push_back(llformat("A tunnel can have at most %d ways up: %s has none.", MAX_EXITS, name.c_str()));
            return;
        }
        std::string why1, why2;
        if (make(foot, across, edge, room, why1) || make(foot, across * -1.f, edge, room, why2)) return;
        mExitNotes.push_back(name + " has no way up: " + why1 + (why2.empty() || why2 == why1 ? std::string() : ", and on the other side " + why2) + ".");
    };
    for (size_t i = 0; i < mDesign.mCaverns.size(); ++i)
    {
        const WolfTunnels::Cavern& c = mDesign.mCaverns[i];
        bool closed = false;
        for (const LLVector2& q : mClosedRooms) if (dist_vec(q, LLVector2(c.mX, c.mY)) <= c.mR) closed = true;
        if (closed) continue;
        LLVector2 p, u;
        main.route.at(main.route.nearest(c.mX, c.mY).second, p, u);
        both_ways(LLVector2(c.mX, c.mY), LLVector2(-u.mV[1], u.mV[0]), c.mR, (S32)i, llformat("Room %d", (S32)i + 1));
    }
    for (size_t k = 0; k < mExitAt.size(); ++k)
    {
        LLVector2 p, u;
        main.route.at(llclamp(mExitAt[k], 0.f, main.route.mLen), p, u);
        both_ways(p, LLVector2(-u.mV[1], u.mV[0]), main.hw, -1, llformat("The way up you added (%d)", (S32)k + 1));
    }
}

void WolfFloaterTunnels::onFlat()
{
    mDesign.mFlat = mFlat->get();
    if (!mDesign.mFlat) { mExitAt.clear(); }
    setStatus(mDesign.mFlat ? "Level (subway): one floor height between the two ramps, and a way up from every room. Add a way up adds more."
                            : "The floor follows the ground again.", false);
    designChanged();
}

void WolfFloaterTunnels::onExitPen()
{
    WolfToolTunnel* t = WolfToolTunnel::getInstance();
    if (t->pen() == WolfToolTunnel::PEN_EXIT) { t->setPen(WolfToolTunnel::PEN_ROUTE); updateButtons(); return; }
    if (!mDesign.mFlat) { setStatus("Ways up are for level (subway) tunnels: tick Keep it level first.", true); return; }
    if (mClicks.size() < 2) { setStatus("Draw the tunnel first: a way up goes from it.", true); return; }
    if (t->current()) t->setPen(WolfToolTunnel::PEN_EXIT);
    else t->begin(getHandle(), WolfToolTunnel::PEN_EXIT);
    setStatus("Click on the tunnel's line to put a way up there, on a way up to take it away, or in a room to open or close its way up.", false);
    updateButtons();
}

void WolfFloaterTunnels::exitClickAt(F32 x, F32 y)
{
    if (!mEditing || s_jobBusy || mJustBuilt) return;
    WolfToolTunnel::getInstance()->setPen(WolfToolTunnel::PEN_ROUTE);   // one change per press, as Add a room
    const LLVector2 q(x, y);
    // in a room: its way up opens or closes
    for (size_t i = 0; i < mDesign.mCaverns.size(); ++i)
    {
        const WolfTunnels::Cavern& c = mDesign.mCaverns[i];
        const LLVector2 cc(c.mX, c.mY);
        if (dist_vec(q, cc) > c.mR) continue;
        bool closed = false;
        for (size_t k = 0; k < mClosedRooms.size(); ++k)
            if (dist_vec(mClosedRooms[k], cc) <= c.mR) { mClosedRooms.erase(mClosedRooms.begin() + k); closed = true; break; }
        if (!closed) mClosedRooms.push_back(cc);
        setStatus(closed ? llformat("Room %d has a way up again.", (S32)i + 1) : llformat("Room %d is closed: no way up from it.", (S32)i + 1), false);
        designChanged();
        return;
    }
    // on a way up added on the line: it goes
    for (size_t k = 0; k < mDesign.mExits.size(); ++k)
    {
        const WolfTunnels::Exit& e = mDesign.mExits[k];
        if (e.mRoom >= 0 || e.mP.size() < 6) continue;
        const LLVector2 a(e.mP[0], e.mP[1]), b(e.mP[4], e.mP[5]), v = b - a;
        const F32 u = llclamp(((q - a) * v) / llmax(v.lengthSquared(), 1e-6f), 0.f, 1.f);
        if (dist_vec(q, a + v * u) > e.mW) continue;
        // which added one it is: the nearest by where it starts
        const Route route(mDesign.mP);
        const F32 t = route.nearest(a.mV[0], a.mV[1]).second;
        size_t best = 0;
        for (size_t i = 1; i < mExitAt.size(); ++i) if (fabsf(mExitAt[i] - t) < fabsf(mExitAt[best] - t)) best = i;
        if (!mExitAt.empty()) mExitAt.erase(mExitAt.begin() + best);
        setStatus("That way up was taken away.", false);
        designChanged();
        return;
    }
    // on the line: a new one there
    const Route route(mDesign.mP);
    const std::pair<F32, F32> nt = route.nearest(x, y);
    if (nt.first > llmax(mDesign.mW * 2.f, 6.f)) { setStatus("Click on the tunnel's line to put a way up there.", true); return; }
    if ((S32)mDesign.mExits.size() >= MAX_EXITS) { setStatus(llformat("A tunnel can have at most %d ways up.", MAX_EXITS), true); return; }
    mExitAt.push_back(nt.second);
    const size_t before = mDesign.mExits.size();
    designChanged();
    setStatus(mDesign.mExits.size() > before ? std::string("A way up was added: it climbs to the surface from there.")
                                             : (mExitNotes.empty() ? std::string("No way up fits there.") : mExitNotes.back()), mDesign.mExits.size() <= before);
}

void WolfFloaterTunnels::onLevel()
{
    if (!mDesign.mFlat) return;
    mLevelSet = llclamp((F32)mLevel->getValueF32(), Z_MIN, Z_MAX);
    mLevelAuto->setLabel("Work it out");
    designChanged();
}

void WolfFloaterTunnels::onLevelAuto()
{
    mLevelSet = std::numeric_limits<F32>::quiet_NaN();
    mLevelAuto->setLabel("(worked out)");
    designChanged();
}

// Source: wolftunnel.rs validate_design (caverns): on the route (nearer than half the width), the whole room inside the two
// mouth cuts (its centre at least r + ROUGH_CAVERN_MAX_M from each end) and inside the region. Rooms are put back on the route
// and made smaller to fit, and taken away when they cannot (a smaller room than CAVERN_R_MIN is refused by the generator).
void WolfFloaterTunnels::fitRooms()
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (mDesign.mCaverns.empty() || !rgn) return;
    const F32 w = rgn->getWidth();
    size_t gone = 0;
    std::vector<WolfTunnels::Cavern> kept;
    const Route route(mDesign.mP);
    for (WolfTunnels::Cavern c : mDesign.mCaverns)
    {
        if (route.mPts.size() < 2) { ++gone; continue; }
        const std::pair<F32, F32> nt = route.nearest(c.mX, c.mY);
        if (nt.first > llmax(mDesign.mW * 2.f, c.mR)) { ++gone; continue; }
        LLVector2 p, u;
        route.at(nt.second, p, u);
        const F32 room = llmin(llmin(nt.second, route.mLen - nt.second) - ROUGH_CAVERN_MAX_M,
                               llmin(llmin(p.mV[0], p.mV[1]), llmin(w - p.mV[0], w - p.mV[1])));
        if (room < CAVERN_R_MIN) { ++gone; continue; }
        c.mX = r2(p.mV[0]);
        c.mY = r2(p.mV[1]);
        c.mR = r2(llmin(c.mR, room - 0.01f));
        kept.push_back(c);
    }
    mDesign.mCaverns = kept;
    if (gone) setStatus(gone == 1 ? std::string("A room was taken away: the tunnel no longer has space for it there.")
                                  : llformat("%d rooms were taken away: the tunnel no longer has space for them there.", (S32)gone), false);
}

// ── Expert: a point's depth by hand ────────────────────────────────────────────────────────────────────────────────────

S32 WolfFloaterTunnels::expertPoint() const
{
    if (!mExpertCheck || !mExpertCheck->get() || mClicks.empty()) return -1;
    return llclamp((S32)mPointNum->getValue().asInteger() - 1, 0, (S32)mClicks.size() - 1);
}

void WolfFloaterTunnels::onExpertPoint()
{
    designChanged();   // (the row and the amber marker follow)
}

void WolfFloaterTunnels::onPointDepth()
{
    const S32 k = expertPoint();
    if (k < 0) return;
    if (mPins.size() < mClicks.size()) mPins.resize(mClicks.size(), std::numeric_limits<F32>::quiet_NaN());
    mPins[k] = llclamp((F32)mPointDepth->getValueF32(), -50.f, 300.f);
    designChanged();
}

void WolfFloaterTunnels::onPointAuto()
{
    const S32 k = expertPoint();
    if (k < 0 || (size_t)k >= mPins.size()) return;
    mPins[k] = std::numeric_limits<F32>::quiet_NaN();
    designChanged();
}

// ── the design follows ─────────────────────────────────────────────────────────────────────────────────────────────────

/** The clicks changed (or the kind, the size, Expert's numbers): the floor, the preview, the one note and the buttons. */
void WolfFloaterTunnels::designChanged()
{
    mPreview = Preview();
    mPreviewDirty = false;
    mPreviewAt = LLFrameTimer::getElapsedSeconds();
    LLViewerRegion* rgn = gAgent.getRegion();
    Fit fit;
    fitRoute(rgn, mClicks, mPins, roofTop(mDesign), mCover ? llclamp((F32)mCover->getValueF32(), 0.f, 100.f) : COVER_M,
             kindGrade(mDesign), !mMouths || mMouths->get(), mDesign.mW, fit, mDesign.mFlat, mLevelSet);
    mDesign.mP = fit.p;
    mDesign.mZ = fit.z;
    // [2026-10-23 subway] Expert's level: the worked-out (or set) floor between the ramps
    if (mLevel && mDesign.mFlat && mClicks.size() >= 2) mLevel->setValue(r2(fit.level));
    mFitProblem = fit.problem;
    mBadEnd = fit.bad_end;
    fitRooms();
    // [2026-10-22 crossroads] Where the route meets itself or turns sharply, one level junction (wolftunnel.rs level, the same
    // floor the generator builds); its refusal is the drawing's one refusal when the floor itself had none.
    const Levelled lv = levelRoute(mDesign);
    if (mFitProblem.empty() && !lv.problem.empty())
    {
        mFitProblem = lv.problem;
        mBadEnd = lv.bad_end;
    }
    // [2026-10-23 subway] the ways up, worked out for this floor
    placeExits();
    const size_t n = mDesign.mP.size() / 2;
    const F32 len = WolfTunnels::routeLength(mDesign);
    mRouteInfo->setText(mClicks.empty() ? std::string() : mClicks.size() == 1 ? std::string("Way in placed")
                        : llformat("%.0f m long%s", len, mClicks.size() > 2 ? llformat(", %d bend%s", (S32)mClicks.size() - 2, mClicks.size() == 3 ? "" : "s").c_str() : ""));
    mProblem = rgn ? WolfTunnels::designProblem(mDesign, rgn->getWidth()) : std::string("You are not in a region.");
    // Expert's point row
    const S32 maxpt = llmax(1, (S32)mClicks.size());
    mPointNum->setMaxValue((F32)maxpt);
    if (mPointNum->getValue().asInteger() > maxpt) mPointNum->setValue(maxpt);
    const S32 ek = expertPoint();
    std::vector<F32> click_t(mClicks.size(), 0.f);
    for (size_t k = 1; k < mClicks.size(); ++k) click_t[k] = click_t[k - 1] + dist_vec(mClicks[k], mClicks[k - 1]);
    if (ek >= 0 && rgn && n >= 1)
    {
        const F32 g = rgn->getLand().resolveHeightRegion(mClicks[ek].mV[0], mClicks[ek].mV[1]);
        const bool pinned = (size_t)ek < mPins.size() && std::isfinite(mPins[ek]);
        mPointDepth->setValue(r2(pinned ? mPins[ek] : g - WolfTunnels::floorAt(mDesign, click_t[ek])));
        mPointAutoBtn->setLabel(pinned ? std::string("Work it out") : std::string("(worked out)"));
    }
    if (n >= 2 && rgn)
    {
        const Air air(mDesign, lv.meets);
        F32 lo = mDesign.mZ[0], hi = mDesign.mZ[0], steep = 0.f;
        for (size_t k = 0; k < n; ++k) { lo = llmin(lo, mDesign.mZ[k]); hi = llmax(hi, mDesign.mZ[k]); }
        for (size_t k = 0; k + 1 < n; ++k) steep = llmax(steep, fabsf(air.pieceGrade(k)));
        mGradeInfo->setText(llformat("Floor %.1f to %.1f m high, steepest %.0f%%, %d floor points.", lo, hi, steep * 100.f, (S32)n));
        // The preview is drawn for any route the shape can be worked out for: every piece 0.5 m or more, the size in range, the
        // points inside the region (the checks before the pieces' lengths in designProblem).
        bool previewable = mDesign.mW >= W_MIN && mDesign.mW <= W_MAX && mDesign.mH >= H_MIN && mDesign.mH <= H_MAX;
        for (size_t k = 1; k < n && previewable; ++k)
            if (air.route.mCum[k] - air.route.mCum[k - 1] < SEG_MIN_M) previewable = false;
        for (F32 v : mDesign.mP) if (v < 0.f || v > rgn->getWidth()) previewable = false;
        if (previewable)
        {
            // Cross-sections along the route: from the route outwards to where the air ends, the floor and the roof at
            // PREVIEW_ACROSS points across. Source: wolftunnel.rs Air::eval (the same shape the generator builds).
            const F32 step = llmax(1.f, len / (F32)PREVIEW_RINGS_MAX);
            const S32 rings = llmax(1, (S32)ceilf(len / step));
            F32 far_r = air.reach() + 1.f;
            for (const WolfTunnels::Cavern& c : mDesign.mCaverns) far_r = llmax(far_r, c.mR + ROUGH_CAVERN_MAX_M + c.mR + 1.f);
            // [2026-10-23 subway] the same cross-sections along every way up (through the joined air), then along the tunnel
            auto rings_along = [&](const Route& rt, F32 rlen, F32 reach_r)
            {
                const S32 nr = llmax(1, (S32)ceilf(rlen / llmax(1.f, rlen / (F32)PREVIEW_RINGS_MAX)));
                std::vector<LLVector3> pf, pc;
                for (S32 i = 0; i <= nr; ++i)
                {
                    LLVector2 p, u;
                    rt.at(rlen * (F32)i / (F32)nr, p, u);
                    const LLVector2 left(-u.mV[1], u.mV[0]);
                    auto edge = [&](F32 sign) -> F32
                    {
                        F32 inside = 0.f, q = 0.f;
                        while (q < reach_r)
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
                        const LLVector2 a = p + left * (-qr + (ql + qr) * (F32)k / (F32)PREVIEW_ACROSS);
                        const Field fd = air.eval(a.mV[0], a.mV[1]);
                        fl.emplace_back(a.mV[0], a.mV[1], fd.f);
                        ce.emplace_back(a.mV[0], a.mV[1], fd.f + fd.v);
                    }
                    if (!pf.empty())
                    {
                        for (S32 k = 0; k < PREVIEW_ACROSS; ++k)
                        {
                            mPreview.mFloor.insert(mPreview.mFloor.end(), { pf[k], pf[k + 1], fl[k + 1], pf[k], fl[k + 1], fl[k] });
                            mPreview.mRoof.insert(mPreview.mRoof.end(), { pc[k], pc[k + 1], ce[k + 1], pc[k], ce[k + 1], ce[k] });
                        }
                        for (S32 k : { 0, PREVIEW_ACROSS })
                            mPreview.mRoof.insert(mPreview.mRoof.end(), { pf[k], fl[k], ce[k], pf[k], ce[k], pc[k] });
                    }
                    pf = fl;
                    pc = ce;
                }
            };
            for (const std::shared_ptr<Air>& xa : air.xa) rings_along(xa->route, xa->route.mLen, xa->reach() + 1.f);
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
            // The notes over the ground this viewer has (wolftunnel.rs warnings() leaves the ground ones to the viewer), in plain
            // words and without coordinates (the red marks in the world show the places), the most important first.
            const LLSurface& land = rgn->getLand();
            const F32 water = rgn->getWaterHeight();
            auto loaded = [&](F32 x, F32 y) { LLSurfacePatch* pt = land.resolvePatchRegion(x, y); return pt && pt->getHasReceivedData(); };
            if (fit.steep > 0.f && mFitProblem.empty())
            {
                const char* use = mDesign.mKind == "rail" ? "a train" : "walking";
                mPreview.mWarnings.push_back(fit.pinned
                    ? llformat("The depths set in Expert make the floor a steep climb for %s (%.0f%%).", use, fit.steep * 100.f)
                    : llformat("The way out is %.0f m %s than the way in over %.0f m: a steep climb for %s (%.0f%%). Drag the Out marker further away for a gentler slope.",
                               fabsf(fit.rise), fit.rise > 0.f ? "higher" : "lower", fit.run, use, fit.steep * 100.f));
            }
            // The roof above the ground where the floor could not be any lower: down from a mouth no faster than the slope, the
            // roof is out of the ground near a way in on level land - that is the way in itself, not worth a note.
            bool out_of = false, wet = false;
            const F32 post_every = llmax(10.f, len / 20.f);
            F32 next_post = MOUTH_M;
            const F32 z0 = mDesign.mZ.front(), z1 = mDesign.mZ.back();
            for (F32 t = 0.f; t <= len; t += 2.f)
            {
                LLVector2 p, u;
                air.route.at(t, p, u);
                if (!loaded(p.mV[0], p.mV[1])) continue;
                const F32 g = land.resolveHeightRegion(p.mV[0], p.mV[1]);
                const F32 f = air.floorT(t), roof = f + air.top();
                const bool inner = t >= MOUTH_M && t <= len - MOUTH_M;
                const F32 ramp = llmax(z0 - fit.grade * t, z1 - fit.grade * (len - t));
                const bool o = inner && g < roof && f > ramp + 0.05f;
                if (o) { out_of = true; mPreview.mBreakouts.emplace_back(p.mV[0], p.mV[1], roof); }
                if (inner && g < water) wet = true;
                if (inner && t >= next_post)
                {
                    next_post = t + post_every;
                    mPreview.mPosts.emplace_back(p.mV[0], p.mV[1], g);
                    mPreview.mPosts.emplace_back(p.mV[0], p.mV[1], roof);
                    mPreview.mPostLabels.push_back(g >= roof ? llformat("%.1f m of ground over the roof", g - roof) : llformat("roof %.1f m above the ground", roof - g));
                }
            }
            if (out_of) mPreview.mWarnings.push_back("Part of the tunnel shows above the ground (red marks): the land there is too low to cover it. It can still be built; to hide it, draw it under higher ground.");
            for (size_t i = 0; i < mDesign.mCaverns.size(); ++i)
            {
                const WolfTunnels::Cavern& c = mDesign.mCaverns[i];
                if (!loaded(c.mX, c.mY)) continue;
                const F32 tc = air.route.nearest(c.mX, c.mY).second;
                if (land.resolveHeightRegion(c.mX, c.mY) < air.floorT(tc) + c.mH)
                    mPreview.mWarnings.push_back(llformat("Room %d comes out of the ground - make rooms smaller (Expert) or put it under higher ground.", (S32)i + 1));
            }
            const F32 ends[2][3] = { { mDesign.mP[0], mDesign.mP[1], z0 }, { mDesign.mP[2 * n - 2], mDesign.mP[2 * n - 1], z1 } };
            for (S32 k = 0; k < 2; ++k)
            {
                const char* name = k == 0 ? "way in" : "way out";
                if (!loaded(ends[k][0], ends[k][1])) continue;
                const F32 g = land.resolveHeightRegion(ends[k][0], ends[k][1]), z = ends[k][2];
                if (z < water - 0.1f) mPreview.mWarnings.push_back(llformat("The %s is %.1f m under the water.", name, water - z));
                if (g - z > air.top()) mPreview.mWarnings.push_back(llformat("The %s is buried: the ground there is %.1f m above its floor (Mouths at ground level is off in Expert).", name, g - z));
            }
            if (wet) mPreview.mWarnings.push_back("Part of the tunnel runs under the water - it stays dry inside.");
            for (const std::string& xn : mExitNotes) mPreview.mWarnings.push_back(xn);   // [2026-10-23 subway]
            // Source: wolftunnel.rs generate() - the same note the build gives.
            if (lv.places > 0)
                mPreview.mWarnings.push_back(llformat("The floor is made level in %d place%s where the tunnel meets itself or turns sharply, so the way through stays open; next to %s the floor is up to %.1f m lower than drawn.",
                                                      lv.places, lv.places == 1 ? "" : "s", lv.places == 1 ? "it" : "them", lv.lowered));
        }
    }
    else
    {
        mGradeInfo->setText(std::string());
    }
    // The one sentence: the refusal, else the generator's own limit, else the most important note.
    mNoteError = false;
    if (mClicks.size() >= 2 && !mFitProblem.empty()) { mNote = mFitProblem; mNoteError = true; }
    else if (mClicks.size() >= 2 && !mProblem.empty()) { mNote = mProblem; mNoteError = true; }
    else if (!mPreview.mWarnings.empty()) mNote = mPreview.mWarnings.front();
    else mNote.clear();
    updateButtons();
}

// ── Build ─────────────────────────────────────────────────────────────────────────────────────────────────────────────

void WolfFloaterTunnels::onBuild()
{
    if (!mEditing || s_jobBusy) return;
    LLViewerRegion* rgn = gAgent.getRegion();
    const std::string block = buildBlocker();
    if (!rgn || !block.empty()) { setStatus("Cannot build yet: " + block, true); return; }
    const std::string uuid = regionUuid();
    if (uuid.empty() || !WolfGrid::isOnWolfTerritoriesConfirmed()) { setStatus("This region's record is not loaded yet - wait a moment and try again.", true); return; }
    // The pen is put away until the build ends (a failed build gives it back, draw()).
    mPenWanted = false;
    // [WOLF TERRAFORM 2026-10-23] only when the pen is down for this window (it may be the Terraform window's)
    if (WolfToolTunnel::instanceExists() && WolfToolTunnel::getInstance()->hostIs(this)) WolfToolTunnel::getInstance()->cancel();
    mJustBuilt = false;
    WolfTunnels::Record rec;
    if (mEditId.empty()) { LLUUID id; id.generate(); mEditId = id.asString(); }
    rec.mId = mEditId;
    rec.mName = mName->getText().empty() ? std::string("Wolf Tunnel") : mName->getText();
    rec.mDesign = mDesign;
    s_jobBusy = true;
    s_jobOk = false;
    s_jobErrors.clear();
    s_jobStatus = "Starting...";
    s_jobProgress = 0.f;
    ++s_jobSerial;
    mProgress->setValue(0.f);
    mProgress->setVisible(true);
    const std::string cap = mCapUrl;
    const LLUUID region = rgn->getRegionID();
    const S32 max_boxes = mMax;
    mBuilding = true;
    LLCoros::instance().launch("WolfTunnelsBuild", [rec, cap, region, uuid, max_boxes]() { buildCoro(rec, cap, region, uuid, max_boxes); });
    updateButtons();
}

std::string WolfFloaterTunnels::buildBlocker() const
{
    if (!WolfGrid::isOnWolfTerritories()) return "Tunnels can be made on Wolf Territories regions only.";
    if (s_jobBusy) return "A build is running - wait for it to finish.";
    if (!mEditing) return "Press Start over to draw a tunnel.";
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return "You are not in a region.";
    if (mBusy && mRegionAtFetch.isNull()) return "Reading this region's tunnels - a moment.";
    if (mCapUrl.empty() || mRegionAtFetch != rgn->getRegionID()) return getString("str_need_cap");
    if (!mEdit) return getString("str_need_rights");
    if (mClicks.size() < 2) return mClicks.empty() ? "Click on the ground where the tunnel goes in." : "Click where the tunnel comes out.";
    if (!mFitProblem.empty()) return mFitProblem;
    return mProblem;
}

std::string WolfFloaterTunnels::nextStep() const
{
    if (!WolfGrid::isOnWolfTerritories()) return "Tunnels can be made on Wolf Territories regions only.";
    if (s_jobBusy) return "Building your tunnel - this takes a little while.";
    if (mJustBuilt) return "Built! Press Walk in to go inside - or Start over to draw another tunnel.";
    if (!mEditing) return "Press Start over to draw a tunnel.";
    // [WOLF TUNNELS 2026-10-23] editing a built tunnel says how
    if (editingBuilt() && mClicks.size() >= 2)
        return "Editing: drag a marker to move it, Ctrl+click a marker to take that bend out, Ctrl+click the line to add a bend. "
               "Then press Rebuild tunnel - or Cancel edit to leave it as it is.";
    LLViewerRegion* rgn = gAgent.getRegion();
    const bool known = rgn && mRegionAtFetch.notNull() && mRegionAtFetch == rgn->getRegionID();
    if (mCapUrl.empty() && !mBusy) return "Tunnels cannot be made on this region - the reason is below.";
    if (known && !mEdit) return "Only the region's estate owner and estate managers can make tunnels here.";
    const bool pen_here = WolfToolTunnel::instanceExists() && WolfToolTunnel::getInstance()->current();
    if (mPenWanted && !pen_here && gFloaterTools && gFloaterTools->getVisible()) return "Close the Build window to go on drawing.";
    if (pen_here && WolfToolTunnel::getInstance()->pen() == WolfToolTunnel::PEN_ROOM) return "Click on the tunnel's line to put a room there.";
    if (pen_here && WolfToolTunnel::getInstance()->pen() == WolfToolTunnel::PEN_EXIT)
        return "Click on the tunnel's line to add a way up there, on a way up to take it away, or in a room to open or close its way up.";
    if (mClicks.empty()) return "Click on the ground where the tunnel goes in.";
    if (mClicks.size() == 1) return "Now click where it comes out. (To make it bend, click the bends first, in order.)";
    return "Click again to carry it on (your last click is always the way out), or drag a marker. Then pick what it is for and press Build tunnel.";
}

void WolfFloaterTunnels::updateButtons()
{
    if (!mHint || !mBuildBtn) return;
    const bool drawing = mEditing && !s_jobBusy && !mJustBuilt;
    const WolfToolTunnel::EPen pen = WolfToolTunnel::instanceExists() ? WolfToolTunnel::getInstance()->pen() : WolfToolTunnel::PEN_NONE;
    // 1. Where (Start over's question takes the row while it asks)
    for (LLView* v : std::initializer_list<LLView*>{ mUndoPointBtn, mStartOverBtn, mTopViewBtn, mRouteInfo }) v->setVisible(!mConfirming);
    for (LLView* v : std::initializer_list<LLView*>{ mConfirmText, mConfirmYes, mConfirmNo }) v->setVisible(mConfirming);
    mUndoPointBtn->setEnabled(drawing && !mClicks.empty());
    mStartOverBtn->setEnabled(WolfGrid::isOnWolfTerritories() && !s_jobBusy);
    mTopViewBtn->setEnabled(WolfGrid::isOnWolfTerritories() && gAgent.getRegion() != nullptr);
    // 2. What
    mKind->setEnabled(drawing);
    mStyle->setEnabled(drawing);
    mRoomBtn->setEnabled(drawing && mClicks.size() >= 2);
    mRoomBtn->setToggleState(pen == WolfToolTunnel::PEN_ROOM);
    // [2026-10-23 subway]
    mFlat->setEnabled(drawing);
    mExitBtn->setEnabled(drawing && mDesign.mFlat && mClicks.size() >= 2);
    mExitBtn->setToggleState(pen == WolfToolTunnel::PEN_EXIT);
    mLevel->setEnabled(drawing && mDesign.mFlat);
    mLevelAuto->setEnabled(drawing && mDesign.mFlat && std::isfinite(mLevelSet));
    // 3. Build: always shown; when it cannot be pressed the line under it says why, else the one note, else "Ready".
    const std::string block = buildBlocker();
    const bool built_before = !mEditId.empty() && findRecord(mEditId) && findRecord(mEditId)->mBuilt.isMap();
    mBuildBtn->setEnabled(block.empty());
    mBuildBtn->setLabel(built_before ? std::string("Rebuild tunnel") : std::string("Build tunnel"));
    // [WOLF TUNNELS 2026-10-23] editing a built tunnel: Start over is "Cancel edit" and leaves it as it is
    mStartOverBtn->setLabel(editingBuilt() ? std::string("Cancel edit") : std::string("Start over"));
    mConfirmText->setText(editingBuilt() ? std::string("Stop editing? The built tunnel stays as it is.") : std::string("Clear the drawing and start again?"));
    mWalkInBtn->setVisible(mJustBuilt);
    mProgress->setVisible(s_jobBusy);
    std::string line;
    bool caution = false;
    if (s_jobBusy || mJustBuilt) line.clear();
    else if (!block.empty())
    {
        // ("Click ..." is the hint line's job; everything else that stops Build is said here)
        if (block.find("Click") != 0) { line = "Cannot build yet: " + block; caution = true; }
    }
    else if (!mNote.empty()) { line = mNote; caution = true; }
    else line = "Ready: press Build tunnel. The see-through shape shows it in the ground.";
    if (mWarnings->getText() != line) mWarnings->setText(line);
    mWarnings->setColor(caution ? WolfBurrow::AMBER : WolfBurrow::CREAM);   // [WOLF BURROW 2026-10-23] on the painted plate
    const std::string hint = nextStep();
    if (mHint->getText() != hint) mHint->setText(hint);
    // the reason Build is grey goes to the log once per change (Paul's 10-22 test left no trace of it)
    const std::string logged = mEditing && mClicks.size() >= 2 ? block : std::string();
    if (logged != mLoggedBlock)
    {
        mLoggedBlock = logged;
        if (!logged.empty()) LL_INFOS("WolfTunnels") << "Build tunnel not available: " << logged << LL_ENDL;
    }

    // Expert
    const S32 ek = expertPoint();
    for (LLUICtrl* c : std::initializer_list<LLUICtrl*>{ mName, mPortals, mMouths, mW, mH, mCover, mRoomR })
        c->setEnabled(drawing);
    mPointNum->setEnabled(drawing && !mClicks.empty());
    mPointDepth->setEnabled(drawing && ek >= 0 && !mDesign.mFlat);   // [2026-10-23 subway] a level tunnel has one level
    mPointAutoBtn->setEnabled(drawing && ek >= 0 && (size_t)ek < mPins.size() && std::isfinite(mPins[ek]));
    mUndoRoomBtn->setEnabled(drawing && !mDesign.mCaverns.empty());
    // the region's tunnels
    const bool here = mRegionAtFetch.notNull() && !mCapUrl.empty();
    const bool can = here && mEdit && !s_jobBusy;
    const WolfTunnels::Record* picked = pickedTunnel();
    mEditBtn->setEnabled(can && picked);
    mDelTunBtn->setEnabled(can && picked);
    mGoTunBtn->setEnabled(picked != nullptr && !s_jobBusy);
    // [WOLF CAVE LIGHTS 2026-10-23] a built tunnel can be lit; one with lights listed can have them taken away
    const bool lit = picked && picked->mLights.isMap() && picked->mLights["objects"].size() > 0;
    mLightUpBtn->setEnabled(can && picked && picked->mBuilt.isMap());
    mUnlightBtn->setEnabled(can && lit);
    mLightStyle->setEnabled(can && picked && picked->mBuilt.isMap());
    mLightTone->setEnabled(can && picked && picked->mBuilt.isMap());
    mListCheck->setLabel(llformat("This region's tunnels (%d)", (S32)s_records.size()));
    // Expert: the boxes
    const bool canBox = mEdit && !mBusy && mRegionAtFetch.notNull() && !s_jobBusy;
    mSaveBtn->setEnabled(canBox && s_selected != 0);
    mDeleteBtn->setEnabled(canBox && s_selected != 0);
    for (LLSpinCtrl* s : mSpin) s->setEnabled(canBox && s_selected != 0);
    mGoBtn->setEnabled(s_selected != 0 && !mBusy);
    mRefreshBtn->setEnabled(!mBusy);
    mLeftoversBtn->setEnabled(canBox && !mCapUrl.empty() && !leftoverBoxes().empty());
}

// ── the region's tunnels ──────────────────────────────────────────────────────────────────────────────────────────────

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
        cell("style", r.mDesign.mStyle == "rough" ? "Natural" : "Built");
        cell("length", llformat("%.0f m", WolfTunnels::routeLength(r.mDesign)));
        const std::string lights = WolfCaveLights::summary(r.mLights);   // [WOLF CAVE LIGHTS 2026-10-23]
        cell("built", r.mBuilt.isMap() ? (lights.empty() ? std::string("built") : "built, " + lights) : std::string("not built"));
        mTunList->addRow(rp);
    }
    if (!keep.empty()) mTunList->selectByValue(LLSD(keep));
    onPickTunnel();
    updateButtons();
}

// [WOLF CAVE LIGHTS 2026-10-23] The light choice for the picked tunnel: its own lights' style and colour when it has some; else
// the last choice made; else what suits its walls (Paul: torches for natural walls, wall lamps for built ones).
void WolfFloaterTunnels::onPickTunnel()
{
    const WolfTunnels::Record* r = pickedTunnel();
    const std::string id = r ? r->mId : std::string();
    if (id == mLightPickShown) return;
    mLightPickShown = id;
    if (!r) return;
    const WolfCaveLights::EStyle walls = WolfCaveLights::styleForWalls(r->mDesign.mStyle == "rough");
    const bool lit = r->mLights.isMap() && r->mLights.has("style");
    const WolfCaveLights::EStyle st = lit ? WolfCaveLights::styleFrom(r->mLights["style"].asString(), walls) : WolfCaveLights::savedStyle(walls);
    const bool warm = lit ? r->mLights["tone"].asString() != "cool" : WolfCaveLights::savedWarm();
    mLightStyle->setValue(WolfCaveLights::styleKey(st));
    mLightTone->setValue(warm ? "warm" : "cool");
}

void WolfFloaterTunnels::onLightUp()
{
    const WolfTunnels::Record* r = pickedTunnel();
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!r || !rgn || s_jobBusy) return;
    if (!r->mBuilt.isMap()) { setStatus("Build the tunnel first, then light it up.", true); return; }
    const WolfCaveLights::EStyle style = WolfCaveLights::styleFrom(mLightStyle->getValue().asString(), WolfCaveLights::STYLE_LAMPS);
    const bool warm = mLightTone->getValue().asString() != "cool";
    const std::vector<WolfCaveLights::Lamp> lamps = tunnelLamps(r->mDesign, style);
    if (lamps.empty()) { setStatus("No place for a lamp was found in \"" + r->mName + "\" (is it built and long enough?).", true); return; }
    const WolfTunnels::Record rec = *r;
    const LLUUID region = rgn->getRegionID();
    const std::string uuid = regionUuid();
    if (uuid.empty()) { setStatus("This region's tunnels are not loaded here - press Refresh.", true); return; }
    WolfCaveLights::confirmLightUp(rec.mName, style, (S32)lamps.size(), rec.mLights, [rec, lamps, style, warm, region, uuid](bool keep)
    {
        if (s_jobBusy) return;
        if (!gAgent.getRegion() || gAgent.getRegion()->getRegionID() != region) return;
        s_jobBusy = true;
        s_jobOk = false;
        s_jobErrors.clear();
        s_jobStatus = "Lighting up \"" + rec.mName + "\"...";
        s_jobProgress = -1.f;
        ++s_jobSerial;
        LLCoros::instance().launch("WolfTunnelsLights", [rec, lamps, style, warm, keep, region, uuid]()
        {
            lightsCoro(rec, lamps, false, style, warm, keep, region, uuid);
        });
    });
}

void WolfFloaterTunnels::onRemoveLights()
{
    const WolfTunnels::Record* r = pickedTunnel();
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!r || !rgn || s_jobBusy) return;
    const WolfTunnels::Record rec = *r;
    const LLUUID region = rgn->getRegionID();
    const std::string uuid = regionUuid();
    if (uuid.empty()) { setStatus("This region's tunnels are not loaded here - press Refresh.", true); return; }
    WolfCaveLights::confirmRemove(rec.mName, rec.mLights, [rec, region, uuid](bool keep)
    {
        if (s_jobBusy) return;
        if (!gAgent.getRegion() || gAgent.getRegion()->getRegionID() != region) return;
        s_jobBusy = true;
        s_jobOk = false;
        s_jobErrors.clear();
        s_jobStatus = "Taking the lights of \"" + rec.mName + "\" away...";
        s_jobProgress = -1.f;
        ++s_jobSerial;
        LLCoros::instance().launch("WolfTunnelsUnlight", [rec, keep, region, uuid]()
        {
            lightsCoro(rec, {}, true, WolfCaveLights::STYLE_HIDDEN, true, keep, region, uuid);
        });
    });
}

const WolfTunnels::Record* WolfFloaterTunnels::pickedTunnel() const
{
    return mTunList ? findRecord(mTunList->getSelectedValue().asString()) : nullptr;
}

// Change: the picked tunnel becomes the drawing (its clicks recovered from the stored route, clicksOf); its floor is worked out
// again, and Build again takes the old rock and boxes away first (buildCoro).
void WolfFloaterTunnels::onEditTunnel()
{
    const WolfTunnels::Record* r = pickedTunnel();
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!r || !rgn || s_jobBusy) return;
    const WolfTunnels::Record rec = *r;
    newDrawing();
    mEditId = rec.mId;
    mName->setText(rec.mName);
    mDesign = rec.mDesign;
    if (mDesign.mKind != "wide" && mDesign.mKind != "rail") mDesign.mKind = "walk";
    mClicks = clicksOf(rec.mDesign);
    mPins.assign(mClicks.size(), std::numeric_limits<F32>::quiet_NaN());
    // [2026-10-23 subway] a level tunnel keeps its level and its ways up: the ones on its line by where they start, a room
    // without one stays closed
    if (rec.mDesign.mFlat && !rec.mDesign.mZ.empty())
    {
        std::map<F32, S32> counts;   // the level: the floor most of its points share
        for (F32 z : rec.mDesign.mZ) counts[r2(z)]++;
        F32 lvl = rec.mDesign.mZ[0];
        S32 best = 0;
        for (const auto& kv : counts) if (kv.second > best) { best = kv.second; lvl = kv.first; }
        mLevelSet = lvl;
        const Route route(rec.mDesign.mP);
        for (const WolfTunnels::Exit& e : rec.mDesign.mExits)
            if (e.mRoom < 0 && e.mP.size() >= 2) mExitAt.push_back(route.nearest(e.mP[0], e.mP[1]).second);
        for (size_t i = 0; i < rec.mDesign.mCaverns.size(); ++i)
        {
            bool has = false;
            for (const WolfTunnels::Exit& e : rec.mDesign.mExits) if (e.mRoom == (S32)i) has = true;
            if (!has) mClosedRooms.emplace_back(rec.mDesign.mCaverns[i].mX, rec.mDesign.mCaverns[i].mY);
        }
    }
    // a tunnel drawn with its ends under the ground keeps them there
    const LLSurface& land = rgn->getLand();
    const size_t n = rec.mDesign.mP.size() / 2;
    if (n >= 2 && rec.mDesign.mZ.size() == n)
    {
        const F32 g0 = land.resolveHeightRegion(rec.mDesign.mP[0], rec.mDesign.mP[1]);
        const F32 g1 = land.resolveHeightRegion(rec.mDesign.mP[2 * n - 2], rec.mDesign.mP[2 * n - 1]);
        mMouths->set(!(g0 - rec.mDesign.mZ[0] > 0.5f && g1 - rec.mDesign.mZ[n - 1] > 0.5f));
    }
    showDesign();
    designChanged();
    lookFromAbove();
    mPenWanted = true;
    setStatus("\"" + rec.mName + "\" is open for editing: its floor is worked out again. Change what you like, then press Rebuild tunnel "
              "(the old one is taken away first: its rock, its holes and walkable parts, its lights). Cancel edit leaves it as it is.", false);
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
        ? "\"" + rec.mName + "\"'s rock is not in view from here. Delete the tunnel anyway, leaving the rock where it is (you can delete it by hand)? Its walkable parts are removed."
        : "Delete \"" + rec.mName + "\"? Its rock and its walkable parts are taken away; anyone inside is lifted to the surface.";
    // [WOLF CAVE LIGHTS 2026-10-23] its lamps go too; any not in view are said
    if (rec.mLights.isMap() && rec.mLights["objects"].size() > 0)
    {
        std::vector<LLUUID> roots;
        LLSD lights_unseen;
        WolfCaveLights::find(rec.mLights["objects"], roots, lights_unseen);
        args["MESSAGE"] = args["MESSAGE"].asString() + llformat(" Its %s %s taken away too", WolfCaveLights::summary(rec.mLights).c_str(),
                                                                 WolfCaveLights::lampCount(rec.mLights) == 1 ? "is" : "are")
            + (lights_unseen.size() > 0 ? llformat(" (%d light part%s not in view from here stay%s where %s: delete %s by hand).",
                                                   (S32)lights_unseen.size(), lights_unseen.size() == 1 ? "" : "s", lights_unseen.size() == 1 ? "s" : "",
                                                   lights_unseen.size() == 1 ? "it is" : "they are", lights_unseen.size() == 1 ? "it" : "them")
                                        : std::string("."));
    }
    LLHandle<LLFloater> h = getHandle();
    const std::string cap = mCapUrl;
    const LLUUID region = rgn->getRegionID();
    const std::string uuid = regionUuid();
    LLNotificationsUtil::add("GenericAlertYesCancel", args, LLSD(), [h, rec, cap, region, uuid, unseen](const LLSD& n, const LLSD& resp)
    {
        if (LLNotificationsUtil::getSelectedOption(n, resp) != 0 || s_jobBusy) return;
        if (uuid.empty()) return;
        s_jobBusy = true;
        s_jobOk = false;
        s_jobErrors.clear();
        s_jobStatus = "Deleting \"" + rec.mName + "\"...";
        s_jobProgress = -1.f;
        ++s_jobSerial;
        LLCoros::instance().launch("WolfTunnelsDelete", [rec, cap, region, uuid, unseen]() { deleteCoro(rec, cap, region, uuid, unseen); });
        if (WolfFloaterTunnels* self = static_cast<WolfFloaterTunnels*>(h.get())) self->updateButtons();
    });
}

// Walk in: to the floor a few metres inside the way in (a teleport within the region).
void WolfFloaterTunnels::walkInto(const WolfTunnels::Record& r)
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn || r.mDesign.mP.size() < 4) return;
    const Route route(r.mDesign.mP);
    LLVector2 p, u;
    const F32 t = llmin(3.f, route.mLen * 0.5f);
    route.at(t, p, u);
    const F32 z = WolfTunnels::floorAt(r.mDesign, t) + 1.f;
    gAgent.teleportViaLocation(rgn->getPosGlobalFromRegion(LLVector3(p.mV[0], p.mV[1], z)));
    mCamSaved = false;   // (the teleport puts the camera behind the avatar again)
    setStatus("Going inside \"" + r.mName + "\".", false);
}

// ── the region's walkable boxes (Expert) ──────────────────────────────────────────────────────────────────────────

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
    setStatus(READING, false);
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
    else if (!mEdit)
        setStatus(getString("str_need_rights"), true);   // (keepPen puts the pen away: penAllowed)
    else if (mStatus && mStatus->getText() == READING)
        setStatus(std::string(), false);                 // (the hint line says what to do next)
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

// [2026-10-22] Paul: "if you delete a tunnel mesh you're left with holes in the terrain". Delete in the tunnels list takes a
// tunnel's walkable parts away even when its rock is gone; this clears the parts no tunnel record lists at all.
void WolfFloaterTunnels::onRemoveLeftovers()
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn || s_jobBusy || mCapUrl.empty()) return;
    const std::vector<S32> ids = leftoverBoxes();
    if (ids.empty()) { setStatus("No leftover walkable parts here: every one belongs to a tunnel in the list.", false); return; }
    LLSD args;
    args["MESSAGE"] = llformat("Remove %d walkable part(s) that no tunnel in this region's list owns? They are what leaves holes in the "
                               "ground after a tunnel's rock is deleted by hand. A tunnel whose record never saved its parts (built "
                               "before 22 Oct) loses them too: build it again. Anyone standing in one below the ground is lifted to the surface.",
                               (S32)ids.size());
    LLHandle<LLFloater> h = getHandle();
    const std::string cap = mCapUrl;
    const LLUUID region = rgn->getRegionID();
    LLNotificationsUtil::add("GenericAlertYesCancel", args, LLSD(), [h, ids, cap, region](const LLSD& n, const LLSD& r)
    {
        if (LLNotificationsUtil::getSelectedOption(n, r) != 0 || s_jobBusy) return;
        s_jobBusy = true;
        s_jobOk = false;
        s_jobErrors.clear();
        s_jobStatus = "Removing leftover walkable parts...";
        s_jobProgress = -1.f;
        ++s_jobSerial;
        LLCoros::instance().launch("WolfTunnelsLeftovers", [ids, cap, region]() { leftoversCoro(ids, cap, region); });
        if (WolfFloaterTunnels* self = static_cast<WolfFloaterTunnels*>(h.get())) self->updateButtons();
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
// the pen
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
    // held only while the pen is down: the base tool stays the pie, so every other click is the pie's (wolftooladventureplace.h)
    LLToolMgr::getInstance()->setTransientTool(this);
}

void WolfToolTunnel::setPen(EPen pen)
{
    if (mActive && pen != PEN_NONE) mPen = pen;
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
    if (LLToolMgr::instanceExists())
    {
        LLToolMgr* tm = LLToolMgr::getInstance();
        if (tm->getCurrentTool() == this) tm->clearTransientTool();
    }
    if (was)
        if (WolfRouteHost* f = host()) f->penStopped();
}

// [WOLF TERRAFORM 2026-10-23] The pen serves any WolfRouteHost window (the Tunnels window, the Terraform window).
WolfRouteHost* WolfToolTunnel::host() const
{
    return dynamic_cast<WolfRouteHost*>(mFloater.get());
}

void WolfToolTunnel::detach()
{
    mFloater = LLHandle<LLFloater>();
    mDrag = DRAG_NONE;   // (dropDrag would put the point back into the window)
    mDragPoint = -1;
    cancel();
}

bool WolfToolTunnel::current() const
{
    return mActive && LLToolMgr::instanceExists() && LLToolMgr::getInstance()->getCurrentTool() == this;
}

bool WolfToolTunnel::shown() const
{
    // [WOLF TERRAFORM 2026-10-23] the window the pen was put down for; with none, the Tunnels window (as before)
    // [WOLF TERRAFORM 2026-10-23] the window the pen was put down for, or the Tunnels window (as before)
    auto up = [](LLFloater* f) { return f && f->getVisible() && !f->isMinimized(); };
    return up(mFloater.get()) || up(LLFloaterReg::findInstance("wolf_tunnels"));
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

// [2026-10-22] Paul: "want to draw underground". From above the ground a click picks the ground spot under the pointer. From
// under the ground (the camera in a tunnel) the ray from the camera would only meet the ground from below, so the click lands on
// a level plane at the drawing's last floor instead (the floor itself is worked out from the ground either way: fitRoute).
bool WolfToolTunnel::place(S32 x, S32 y, LLVector3& ground) const
{
    WolfRouteHost* f = host();
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!f || !rgn) return false;
    const F32 w = rgn->getWidth();
    if (cameraUnderground())
    {
        F32 px, py;
        if (!planeHit(x, y, f->planeHeight(), px, py) || px < 0.f || py < 0.f || px >= w || py >= w) return false;
        ground.set(px, py, rgn->getLand().resolveHeightRegion(px, py));
        return true;
    }
    return surface(x, y, ground);
}

// Source: wolfcave.cpp WolfToolCave::pickHandle (llviewercamera.h:75 projectPosAgentToScreen(pos_agent, out_point, clamp) in the
// scaled UI coordinates the tool's x, y are in). A clicked point's marker stands on the ground over it.
S32 WolfToolTunnel::pickMarker(S32 x, S32 y) const
{
    // [WOLF TERRAFORM 2026-10-23] the markers of the window the pen is down for, while it is drawing (for the Tunnels window:
    // editing(), as before)
    const WolfRouteHost* f = host();
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!f || !f->routeEditing() || !f->markersMovable() || !rgn) return -1;
    const std::vector<LLVector2>& c = f->clicks();
    const LLSurface& land = rgn->getLand();
    LLViewerCamera* cam = LLViewerCamera::getInstance();
    S32 best = -1;
    F32 best_px = HANDLE_PICK_PX;
    for (size_t k = 0; k < c.size(); ++k)
    {
        const LLVector3 h(c[k].mV[0], c[k].mV[1], land.resolveHeightRegion(c[k].mV[0], c[k].mV[1]));
        LLCoordGL sp;
        if (!cam->projectPosAgentToScreen(rgn->getPosAgentFromRegion(h), sp, true)) continue;
        const F32 dx = (F32)(sp.mX - x), dy = (F32)(sp.mY - y);
        const F32 px = sqrtf(dx * dx + dy * dy);
        if (px <= best_px) { best = (S32)k; best_px = px; }
    }
    return best;
}

bool WolfToolTunnel::handleAnyMouseClick(S32 x, S32 y, MASK mask, EMouseClickType clicktype, bool down)
{
    // The pen takes left clicks and double-clicks only; Alt-clicks are the camera's (lltool.cpp:193-209 getOverrideTool: the
    // pen is a transient tool, so LLToolMgr never asks for the override itself, lltoolmgr.cpp getCurrentTool); everything else
    // is LLToolPie's (wolfcave.cpp WolfToolCave the same).
    if (mActive && (clicktype == CLICK_LEFT || clicktype == CLICK_DOUBLELEFT) && down && mDrag == DRAG_NONE)
        if (LLTool* over = getOverrideTool(mask)) return over->handleAnyMouseClick(x, y, mask, clicktype, down);
    if (!mActive || (clicktype != CLICK_LEFT && clicktype != CLICK_DOUBLELEFT)) return LLToolPie::getInstance()->handleAnyMouseClick(x, y, mask, clicktype, down);
    return LLTool::handleAnyMouseClick(x, y, mask, clicktype, down);
}

bool WolfToolTunnel::handleScrollWheel(S32 x, S32 y, S32 clicks) { return LLToolPie::getInstance()->handleScrollWheel(x, y, clicks); }

bool WolfToolTunnel::handleMouseDown(S32 x, S32 y, MASK mask)
{
    if (!mActive) return false;
    WolfRouteHost* f = host();
    if (!f) { cancel(); return true; }
    // [2026-10-22 simple] A press on a marker is a click until the pointer moves DRAG_START_PX while held; then it drags the
    // point sideways (the assessment: a shaky click used to grab a handle and move the point by metres).
    const S32 k = mPen == PEN_ROUTE ? pickMarker(x, y) : -1;
    // [WOLF TUNNELS 2026-10-23] editing a route: Ctrl+click on a marker takes that bend out, Ctrl+click elsewhere puts a bend
    // on the line where it is clicked
    if (mPen == PEN_ROUTE && (mask & MASK_CONTROL))
    {
        if (k >= 0) { f->removeClick(k); return true; }
        LLVector3 ground;
        if (place(x, y, ground)) f->insertClickNear(ground.mV[VX], ground.mV[VY]);
        return true;
    }
    if (k >= 0)
    {
        mDragPoint = k;
        mDragStart = f->clicks()[k];
        mPressX = x;
        mPressY = y;
        LLVector3 at;
        if (surface(x, y, at)) { mGrabDX = mDragStart.mV[0] - at.mV[VX]; mGrabDY = mDragStart.mV[1] - at.mV[VY]; }
        else { mGrabDX = 0.f; mGrabDY = 0.f; }
        mDrag = DRAG_PENDING;
        mHover = k;
        setMouseCapture(true);   // (so the release comes here; handleHover lets go of it if the release is lost)
        return true;
    }
    LLVector3 ground;
    if (!place(x, y, ground))
    {
        f->setStatus(f->noGroundText(cameraUnderground()), true);
        return true;
    }
    f->clickAt(ground.mV[VX], ground.mV[VY]);
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
    WolfRouteHost* f = host();
    if (!f) return mActive;
    if (drag == DRAG_PENDING)
    {
        // a press on a marker that did not move: an ordinary click there
        LLVector3 ground;
        if (place(mPressX, mPressY, ground)) f->clickAt(ground.mV[VX], ground.mV[VY]);
    }
    else if (drag == DRAG_MOVE && k >= 0 && (size_t)k < f->clicks().size())
    {
        f->moveClick(k, f->clicks()[k].mV[0], f->clicks()[k].mV[1], true);
    }
    return mActive;
}

// A drag that is dropped (Esc, the pen put down, the mouse taken by another window) puts the point back as it was at the press.
void WolfToolTunnel::dropDrag()
{
    if (mDrag == DRAG_MOVE && mDragPoint >= 0)
    {
        if (WolfRouteHost* f = host())
            f->moveClick(mDragPoint, mDragStart.mV[0], mDragStart.mV[1], true);
    }
    mDrag = DRAG_NONE;
    mDragPoint = -1;
    if (hasMouseCapture()) setMouseCapture(false);
}

void WolfToolTunnel::onMouseCaptureLost()
{
    dropDrag();
}

// [2026-10-22 simple] A double-click is two clicks on one spot: the first press made the point, this second one lands on it
// (clickAt takes it as the same click). It never puts the pen away (the assessment: "Double-click to finish" did, and later
// clicks then went to the land tool).
bool WolfToolTunnel::handleDoubleClick(S32 x, S32 y, MASK mask)
{
    if (!mActive) return LLToolPie::getInstance()->handleDoubleClick(x, y, mask);
    return handleMouseDown(x, y, mask);
}

bool WolfToolTunnel::handleHover(S32 x, S32 y, MASK mask)
{
    if (!shown()) { cancel(); return false; }
    WolfRouteHost* f = host();
    // [2026-10-22 simple] A held mouse capture whose release never came (the window lost focus mid-press) is let go here: while
    // the pen holds the capture every click goes to it, menus included (llviewerwindow.cpp handleAnyMouseClick, the captor first).
    if (mDrag != DRAG_NONE && !gViewerWindow->getLeftMouseDown()) { dropDrag(); return true; }
    if (mDrag == DRAG_PENDING && f && (fabsf((F32)(x - mPressX)) >= DRAG_START_PX || fabsf((F32)(y - mPressY)) >= DRAG_START_PX))
        mDrag = DRAG_MOVE;
    if (mDrag == DRAG_MOVE && f && mDragPoint >= 0)
    {
        LLVector3 at;
        if (surface(x, y, at)) f->moveClick(mDragPoint, at.mV[VX] + mGrabDX, at.mV[VY] + mGrabDY, false);
        // Source: llcursortypes.h UI_CURSOR_SIZEALL.
        gViewerWindow->setCursor(UI_CURSOR_SIZEALL);
        return true;
    }
    mHover = mPen == PEN_ROUTE ? pickMarker(x, y) : -1;
    mHaveCursor = place(x, y, mCursor);
    // Source: llcursortypes.h UI_CURSOR_CROSS / UI_CURSOR_SIZEALL.
    gViewerWindow->setCursor(mHover >= 0 ? UI_CURSOR_SIZEALL : UI_CURSOR_CROSS);
    return true;
}

// [2026-10-22 simple] Enter and Escape never put the pen away (the assessment: they did, and the drawing looked dead). Escape
// drops a drag or the room pen, and is kept otherwise: it is View > Reset View's shortcut (menu_viewer.xml:4463-4465), which
// would end the view from above, and the current tool gets keys (llviewerwindow.cpp:3509) before the menus' plain shortcuts (:3598).
bool WolfToolTunnel::handleKey(KEY key, MASK mask)
{
    if (!mActive) return false;
    WolfRouteHost* f = host();
    if (key == KEY_ESCAPE && mask == MASK_NONE)
    {
        if (mDrag == DRAG_MOVE) { dropDrag(); if (f) f->setStatus("The marker is back where it was.", false); return true; }
        if (mPen == PEN_ROOM) { mPen = PEN_ROUTE; if (f) f->setStatus("No room added.", false); return true; }
        if (mPen == PEN_EXIT) { mPen = PEN_ROUTE; if (f) f->setStatus("No way up changed.", false); return true; }
        if (f) f->setStatus(f->escapeText(), false);
        return true;
    }
    if (f && key == KEY_BACKSPACE && mask == MASK_NONE) { f->undoPoint(); return true; }
    return false;
}

namespace
{
    /** [WOLF TERRAFORM 2026-10-23] The pen's own marks for a window's clicks (moved out of render below, unchanged, so the Tunnels
     *  window and the Terraform window draw them alike): the line on the ground along the clicks (`join`), where the next click
     *  goes (a line from the last point to the pointer when `ghost_line`, a ring of `ring_r` there) and the markers - a post on
     *  the ground at every click with a ring round its foot, sized to stay about the same on screen (wolfcave.cpp
     *  WolfToolCave::render handles); the first green, the last blue (red when `bad_end`), the rest white, `hot` (under the
     *  pointer or dragged) brighter and `picked_k` amber. */
    struct RouteMarks { std::vector<LLVector3> ground, ghost, in_m, out_m, bends, hot, picked, bad; };
    void routeMarks(LLViewerRegion* rgn, const std::vector<LLVector2>& c, bool join, bool drawing_here, bool ghost_line,
                    const LLVector3& cursor, F32 ring_r, S32 hover, S32 drag, S32 picked_k, bool bad_end, RouteMarks& m)
    {
        const LLSurface& land = rgn->getLand();
        auto gz = [&](const LLVector2& p) { return land.resolveHeightRegion(p.mV[0], p.mV[1]); };
        for (size_t k = 1; k < c.size() && join; ++k)
        {
            // a few points along each piece, so the line follows the hill
            const S32 parts = llclamp((S32)(dist_vec(c[k], c[k - 1]) / 4.f), 1, 64);
            for (S32 i = 0; i < parts; ++i)
            {
                const LLVector2 a = c[k - 1] + (c[k] - c[k - 1]) * ((F32)i / parts), b = c[k - 1] + (c[k] - c[k - 1]) * ((F32)(i + 1) / parts);
                m.ground.emplace_back(a.mV[0], a.mV[1], gz(a) + 0.15f);
                m.ground.emplace_back(b.mV[0], b.mV[1], gz(b) + 0.15f);
            }
        }
        if (drawing_here)
        {
            if (!c.empty() && ghost_line)
            {
                m.ghost.emplace_back(c.back().mV[0], c.back().mV[1], gz(c.back()) + 0.15f);
                m.ghost.emplace_back(cursor.mV[VX], cursor.mV[VY], cursor.mV[VZ] + 0.15f);
            }
            for (S32 i = 0; i < 24; ++i)
            {
                const F32 a0 = (F32)i / 24.f * F_TWO_PI, a1 = (F32)(i + 1) / 24.f * F_TWO_PI;
                m.ghost.emplace_back(cursor.mV[VX] + cosf(a0) * ring_r, cursor.mV[VY] + sinf(a0) * ring_r, cursor.mV[VZ] + 0.15f);
                m.ghost.emplace_back(cursor.mV[VX] + cosf(a1) * ring_r, cursor.mV[VY] + sinf(a1) * ring_r, cursor.mV[VZ] + 0.15f);
            }
        }
        const LLVector3 eye = LLViewerCamera::getInstance()->getOrigin();
        for (size_t k = 0; k < c.size(); ++k)
        {
            const LLVector3 foot(c[k].mV[0], c[k].mV[1], gz(c[k]));
            const F32 sz = llclamp(dist_vec(eye, rgn->getPosAgentFromRegion(foot)) * 0.02f, 0.3f, 6.f);
            const bool is_in = k == 0, is_out = c.size() >= 2 && k + 1 == c.size();
            std::vector<LLVector3>& out = (S32)k == hover || (S32)k == drag ? m.hot
                                        : (S32)k == picked_k ? m.picked
                                        : is_out && bad_end ? m.bad : is_in ? m.in_m : is_out ? m.out_m : m.bends;
            out.push_back(foot);
            out.emplace_back(foot.mV[VX], foot.mV[VY], foot.mV[VZ] + sz * 3.f);
            for (S32 i = 0; i < 16; ++i)
            {
                const F32 a0 = (F32)i / 16.f * F_TWO_PI, a1 = (F32)(i + 1) / 16.f * F_TWO_PI;
                out.emplace_back(foot.mV[VX] + cosf(a0) * sz, foot.mV[VY] + sinf(a0) * sz, foot.mV[VZ] + 0.1f);
                out.emplace_back(foot.mV[VX] + cosf(a1) * sz, foot.mV[VY] + sinf(a1) * sz, foot.mV[VZ] + 0.1f);
            }
        }
    }

    /** [WOLF TERRAFORM 2026-10-23] The hint lines at the top of the world view (moved out of WolfToolTunnel::draw, unchanged). */
    void hintLines(const std::vector<std::pair<std::string, LLColor4>>& lines)
    {
        const LLFontGL* font = LLFontGL::getFontSansSerif();
        const F32 cx = (F32)gViewerWindow->getWorldViewWidthScaled() * 0.5f;
        F32 ty = (F32)gViewerWindow->getWorldViewHeightScaled() - 60.f;
        for (const auto& h : lines)
        {
            const F32 tw = (F32)font->getWidthF32(h.first);
            WolfGameDraw::rect(cx - tw * 0.5f - 10.f, ty - 5.f, cx + tw * 0.5f + 10.f, ty + (F32)font->getLineHeight() + 5.f, LLColor4(0.f, 0.f, 0.f, 0.78f));
            font->renderUTF8(h.first, 0, cx, ty, h.second, LLFontGL::HCENTER, LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
            ty -= (F32)font->getLineHeight() + 12.f;
        }
    }
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

    // The region's walkable boxes (Expert: "Show the boxes in the world").
    LLFloater* open = LLFloaterReg::findInstance("wolf_tunnels");
    LLCheckBoxCtrl* adv = open ? open->findChild<LLCheckBoxCtrl>("advanced_check") : nullptr;
    LLCheckBoxCtrl* exp = open ? open->findChild<LLCheckBoxCtrl>("expert_check") : nullptr;
    LLCheckBoxCtrl* show = open ? open->findChild<LLCheckBoxCtrl>("show_outlines") : nullptr;
    if (rgn->getRegionID() == s_boxesRegion && adv && adv->get() && exp && exp->get() && (!show || show->get()))
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

    // [WOLF TERRAFORM 2026-10-23] Another window's drawing (the Terraform window): its own preview, then the line, where the next
    // click goes and the markers, as the tunnel's.
    WolfRouteHost* other = host();
    if (other && !dynamic_cast<WolfFloaterTunnels*>(other))
    {
        other->renderRouteExtras(rgn);
        if (other->routeEditing())
        {
            RouteMarks m;
            const bool here = mActive && mHaveCursor && mDrag != DRAG_MOVE && mHover < 0;
            routeMarks(rgn, other->clicks(), other->joinClicks(), here, other->joinClicks() && mPen == PEN_ROUTE, mCursor,
                       other->cursorRadius(), mActive ? mHover : -1, mActive ? mDragPoint : -1, -1, false, m);
            LLGLDepthTest depth(GL_FALSE);
            gGL.setLineWidth(2.f);
            gGL.color4f(1.f, 1.f, 1.f, 0.8f);
            draw(m.ground, LLRender::LINES);
            gGL.color4f(1.f, 1.f, 0.4f, 0.95f);
            draw(m.ghost, LLRender::LINES);
            gGL.setLineWidth(3.f);
            gGL.color4f(0.35f, 0.95f, 0.45f, 1.f);
            draw(m.in_m, LLRender::LINES);
            gGL.color4f(0.45f, 0.75f, 1.f, 1.f);
            draw(m.out_m, LLRender::LINES);
            gGL.color4f(1.f, 1.f, 1.f, 0.95f);
            draw(m.bends, LLRender::LINES);
            gGL.setLineWidth(5.f);
            gGL.color4f(1.f, 1.f, 0.3f, 1.f);
            draw(m.hot, LLRender::LINES);
            gGL.setLineWidth(1.f);
        }
    }

    // The tunnel being drawn: the preview tube (see-through, also through the ground so its depth shows), the route on the
    // ground and at its floor, the markers, the rooms, where the roof leaves the ground.
    WolfFloaterTunnels* f = WolfFloaterTunnels::editing();
    if (!f) return;
    const bool pen_here = host() == f;   // [WOLF TERRAFORM 2026-10-23] the pen's pointer and drag are this window's
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
    std::vector<LLVector3> route, posts, red, rooms;
    const size_t n = d.mP.size() / 2;
    const bool have_z = d.mZ.size() == n;
    const Route rt(d.mP);
    // The floor (orange) along the design's points, the line on the ground (white) along the clicks.
    for (size_t k = 1; k < n && have_z; ++k)
    {
        route.emplace_back(d.mP[2 * k - 2], d.mP[2 * k - 1], d.mZ[k - 1] + 0.05f);
        route.emplace_back(d.mP[2 * k], d.mP[2 * k + 1], d.mZ[k] + 0.05f);
    }
    const std::vector<LLVector2>& c = f->clicks();
    // Where the next click goes: from the last point to the pointer, and a ring there. The markers: green = the way in, blue =
    // the way out (red when the ends are too far apart in height), white = a bend; the one under the pointer brighter, Expert's
    // picked point amber. [WOLF TERRAFORM 2026-10-23] built by routeMarks (moved from here unchanged).
    const bool drawing_here = pen_here && mActive && mHaveCursor && mDrag != DRAG_MOVE && mHover < 0;
    RouteMarks m;
    routeMarks(rgn, c, true, drawing_here, mPen == PEN_ROUTE, mCursor, mPen == PEN_ROOM ? (F32)CAVERN_R_MIN : llmax(0.5f, d.mW * 0.5f),
               pen_here ? mHover : -1, pen_here ? mDragPoint : -1, f->expertPoint(), f->badEnd(), m);
    std::vector<LLVector3>& ground = m.ground;
    std::vector<LLVector3>& ghost = m.ghost;
    std::vector<LLVector3>& in_m = m.in_m;
    std::vector<LLVector3>& out_m = m.out_m;
    std::vector<LLVector3>& bends = m.bends;
    std::vector<LLVector3>& hot = m.hot;
    std::vector<LLVector3>& picked = m.picked;
    std::vector<LLVector3>& bad = m.bad;
    const bool expert = f->expertPoint() >= 0;
    if (expert) for (size_t k = 0; k + 1 < pv.mPosts.size(); k += 2) { posts.push_back(pv.mPosts[k]); posts.push_back(pv.mPosts[k + 1]); }
    for (const LLVector3& b : pv.mBreakouts) { red.push_back(b); red.emplace_back(b.mV[VX], b.mV[VY], b.mV[VZ] + 3.f); }
    for (const WolfTunnels::Cavern& cv : d.mCaverns)
    {
        const F32 z = (have_z ? WolfTunnels::floorAt(d, rt.nearest(cv.mX, cv.mY).second) : 0.f) + 0.05f;
        for (S32 i = 0; i < 36; ++i)
        {
            const F32 a0 = (F32)i / 36.f * F_TWO_PI, a1 = (F32)(i + 1) / 36.f * F_TWO_PI;
            rooms.emplace_back(cv.mX + cosf(a0) * cv.mR, cv.mY + sinf(a0) * cv.mR, z);
            rooms.emplace_back(cv.mX + cosf(a1) * cv.mR, cv.mY + sinf(a1) * cv.mR, z);
        }
    }
    {
        LLGLDepthTest depth(GL_FALSE);
        gGL.setLineWidth(2.f);
        gGL.color4f(1.f, 0.70f, 0.28f, 0.9f);
        draw(route, LLRender::LINES);
        gGL.color4f(1.f, 1.f, 1.f, 0.8f);
        draw(ground, LLRender::LINES);
        gGL.color4f(1.f, 1.f, 1.f, 0.45f);
        draw(posts, LLRender::LINES);
        gGL.color4f(0.55f, 0.80f, 1.f, 0.9f);
        draw(rooms, LLRender::LINES);
        gGL.color4f(1.f, 1.f, 0.4f, 0.95f);
        draw(ghost, LLRender::LINES);
        gGL.setLineWidth(4.f);
        gGL.color4f(1.f, 0.2f, 0.15f, 1.f);
        draw(red, LLRender::LINES);
        // the markers last and never hidden (wolfcave.cpp the same)
        gGL.setLineWidth(3.f);
        gGL.color4f(0.35f, 0.95f, 0.45f, 1.f);
        draw(in_m, LLRender::LINES);
        gGL.color4f(0.45f, 0.75f, 1.f, 1.f);
        draw(out_m, LLRender::LINES);
        gGL.color4f(1.f, 1.f, 1.f, 0.95f);
        draw(bends, LLRender::LINES);
        gGL.color4f(0.96f, 0.65f, 0.14f, 1.f);
        draw(picked, LLRender::LINES);
        gGL.setLineWidth(5.f);
        gGL.color4f(1.f, 0.15f, 0.1f, 1.f);
        draw(bad, LLRender::LINES);
        gGL.color4f(1.f, 1.f, 0.3f, 1.f);
        draw(hot, LLRender::LINES);
        gGL.setLineWidth(1.f);
    }
}

void WolfToolTunnel::draw()
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return;
    // [WOLF TERRAFORM 2026-10-23] another window's drawing (the Terraform window): its markers' names and what to do next
    WolfRouteHost* other = host();
    if (other && !dynamic_cast<WolfFloaterTunnels*>(other) && shown() && other->routeEditing())
    {
        const LLFontGL* font = LLFontGL::getFontSansSerif();
        LLViewerCamera* cam = LLViewerCamera::getInstance();
        const std::vector<LLVector2>& c = other->clicks();
        for (size_t k = 0; k < c.size(); ++k)
        {
            const F32 g = rgn->getLand().resolveHeightRegion(c[k].mV[0], c[k].mV[1]);
            LLCoordGL sp;
            if (!cam->projectPosAgentToScreen(rgn->getPosAgentFromRegion(LLVector3(c[k].mV[0], c[k].mV[1], g)), sp, false)) continue;
            const bool is_in = k == 0, is_out = c.size() >= 2 && k + 1 == c.size();
            const LLColor4 col = is_in ? LLColor4(0.55f, 1.f, 0.6f, 1.f) : is_out ? LLColor4(0.6f, 0.85f, 1.f, 1.f) : LLColor4(1.f, 1.f, 1.f, 0.95f);
            font->renderUTF8(other->markerLabel(k), 0, (F32)sp.mX + 10.f, (F32)sp.mY + 6.f, col, LLFontGL::LEFT, LLFontGL::BOTTOM, LLFontGL::BOLD, LLFontGL::DROP_SHADOW);
        }
        std::vector<std::pair<std::string, LLColor4>> lines;
        if (mActive && mDrag == DRAG_MOVE) lines.emplace_back("Move the marker; let go to put it there. Esc puts it back.", LLColor4::white);
        else if (mActive && mHover >= 0) lines.emplace_back("Drag the marker to move it.", LLColor4::white);
        else if (!other->routeHint().empty()) lines.emplace_back(other->routeHint(), LLColor4::white);
        hintLines(lines);
        return;
    }
    WolfFloaterTunnels* f = WolfFloaterTunnels::editing();
    if (!f) return;
    const LLFontGL* font = LLFontGL::getFontSansSerif();
    LLViewerCamera* cam = LLViewerCamera::getInstance();
    const LLSurface& land = rgn->getLand();
    // The markers' names: In and Out (and the bends' numbers; Expert: every point's depth, and the depth posts' labels).
    const std::vector<LLVector2>& c = f->clicks();
    const bool expert = f->expertPoint() >= 0;
    const WolfTunnels::Design& d = f->design();
    std::vector<F32> click_t(c.size(), 0.f);
    for (size_t k = 1; k < c.size(); ++k) click_t[k] = click_t[k - 1] + dist_vec(c[k], c[k - 1]);
    for (size_t k = 0; k < c.size(); ++k)
    {
        const F32 g = land.resolveHeightRegion(c[k].mV[0], c[k].mV[1]);
        LLCoordGL sp;
        if (!cam->projectPosAgentToScreen(rgn->getPosAgentFromRegion(LLVector3(c[k].mV[0], c[k].mV[1], g)), sp, false)) continue;
        const bool is_in = k == 0, is_out = c.size() >= 2 && k + 1 == c.size();
        std::string label = is_in ? std::string("In") : is_out ? std::string("Out") : llformat("%d", (S32)k + 1);
        if (expert && d.mZ.size() * 2 == d.mP.size() && !d.mZ.empty())
            label += llformat(" (%.1f m deep)", g - WolfTunnels::floorAt(d, click_t[k]));
        const LLColor4 col = is_out && f->badEnd() ? LLColor4(1.f, 0.35f, 0.3f, 1.f) : is_in ? LLColor4(0.55f, 1.f, 0.6f, 1.f)
                           : is_out ? LLColor4(0.6f, 0.85f, 1.f, 1.f) : LLColor4(1.f, 1.f, 1.f, 0.95f);
        font->renderUTF8(label, 0, (F32)sp.mX + 10.f, (F32)sp.mY + 6.f, col, LLFontGL::LEFT, LLFontGL::BOTTOM, LLFontGL::BOLD, LLFontGL::DROP_SHADOW);
    }
    if (expert)
    {
        const WolfFloaterTunnels::Preview& pv = f->preview();
        const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
        for (size_t k = 0; k < pv.mPostLabels.size() && 2 * k + 1 < pv.mPosts.size(); ++k)
        {
            LLCoordGL sp;
            if (!cam->projectPosAgentToScreen(rgn->getPosAgentFromRegion(pv.mPosts[2 * k]), sp, false)) continue;
            small->renderUTF8(pv.mPostLabels[k], 0, (F32)sp.mX, (F32)sp.mY + 4.f, LLColor4(1.f, 1.f, 1.f, 0.9f), LLFontGL::HCENTER, LLFontGL::BOTTOM,
                              LLFontGL::NORMAL, LLFontGL::DROP_SHADOW);
        }
    }
    // What to do next and the one note, at the top of the world view where the user is looking (wolftooladventureplace.cpp
    // draw), shown while the window is open; the note in the caution colour when it stops Build.
    std::vector<std::pair<std::string, LLColor4>> lines;
    const bool pen_here = host() == f;   // [WOLF TERRAFORM 2026-10-23] the pen's drag and pointer are this window's (as render)
    if (pen_here && mActive && mDrag == DRAG_MOVE) lines.emplace_back("Move the marker; let go to put it there. Esc puts it back.", LLColor4::white);
    else if (pen_here && mActive && mHover >= 0) lines.emplace_back("Drag the marker to move it - or just click to add a point there.", LLColor4::white);
    else lines.emplace_back(f->nextStep(), LLColor4::white);
    if (c.size() >= 2 && !f->note().empty())
        lines.emplace_back(f->note(), f->noteIsError() ? LLColor4(1.f, 0.55f, 0.5f, 1.f) : LLColor4(1.f, 0.85f, 0.45f, 1.f));
    hintLines(lines);   // [WOLF TERRAFORM 2026-10-23] moved to hintLines, unchanged
}
