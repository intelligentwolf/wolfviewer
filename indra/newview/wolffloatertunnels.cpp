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

#include "llagent.h"
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
    // The kinds' sizes (Paul's brief: walk tunnel 4 x 4 x 3.5 m, rail 6 x 6 x 6 m): width, height, and the length of a box made
    // by a click without a drag.
    const F32 KIND_W[2] = { 4.f, 6.f }, KIND_H[2] = { 3.5f, 6.f }, KIND_L[2] = { 4.f, 6.f };
    // Source: UNDERGROUND_PATCH.md "z0 must be a metre or two BELOW the floor": the box's floor sits this far below the ground
    // where the drag began.
    const F32 FLOOR_BELOW = 1.f;
    // Source: WolfTunnelsModule.cs MIN_SIZE / MAX_SIZE (the region refuses others; the tool keeps to them first).
    const F32 MIN_SIZE = 0.5f, MAX_SIZE = 512.f;
    // A drag shorter than this (metres) is a click.
    const F32 MIN_DRAG = 1.f;
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
// the window
// =====================================================================================================================

WolfFloaterTunnels::WolfFloaterTunnels(const LLSD& key) : LLFloater(key) {}

WolfFloaterTunnels::~WolfFloaterTunnels()
{
    if (WolfToolTunnel::instanceExists()) WolfToolTunnel::getInstance()->cancel();
}

bool WolfFloaterTunnels::postBuild()
{
    mList = getChild<LLScrollListCtrl>("tunnel_list");
    mList->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSelect(); });
    static const char* SPINS[7] = { "centre_x", "centre_y", "centre_z", "size_l", "size_w", "size_h", "turn" };
    for (S32 i = 0; i < 7; ++i) mSpin[i] = getChild<LLSpinCtrl>(SPINS[i]);
    mWalkBtn = getChild<LLButton>("draw_walk_btn");
    mRailBtn = getChild<LLButton>("draw_rail_btn");
    mSaveBtn = getChild<LLButton>("save_btn");
    mDeleteBtn = getChild<LLButton>("delete_btn");
    mGoBtn = getChild<LLButton>("go_btn");
    mRefreshBtn = getChild<LLButton>("refresh_btn");
    mShowCheck = getChild<LLCheckBoxCtrl>("show_outlines");
    mStatus = getChild<LLTextBox>("status");
    mInfo = getChild<LLTextBox>("info");
    mWalkBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onDraw(K_WALK); });
    mRailBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onDraw(K_RAIL); });
    mSaveBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onSave(); });
    mDeleteBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onDelete(); });
    mGoBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onGoInside(); });
    mRefreshBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { refresh(); });
    updateButtons();
    return true;
}

void WolfFloaterTunnels::onOpen(const LLSD& key)
{
    // [WOLF GRID GATE] checked once, on open (no polling): Tunnels are WolfSim's.
    if (!WolfGrid::isOnWolfTerritories())
    {
        mEdit = false;
        s_boxes.clear();
        fillList();
        setStatus("Tunnels work on Wolf Territories regions only.", true);
        updateButtons();
        return;
    }
    refresh();
}

void WolfFloaterTunnels::onClose(bool app_quitting)
{
    if (WolfToolTunnel::instanceExists()) WolfToolTunnel::getInstance()->cancel();
}

void WolfFloaterTunnels::draw()
{
    // the region changed under an open window (teleport, crossing): what it shows belongs to the old one
    LLViewerRegion* rgn = gAgent.getRegion();
    if (rgn && mRegionAtFetch.notNull() && rgn->getRegionID() != mRegionAtFetch && !mBusy)
    {
        mRegionAtFetch.setNull();
        if (WolfToolTunnel::instanceExists()) WolfToolTunnel::getInstance()->cancel();
        refresh();
    }
    LLFloater::draw();
}

void WolfFloaterTunnels::setStatus(const std::string& text, bool error)
{
    if (!mStatus) return;
    mStatus->setText(text);
    mStatus->setColor(LLUIColorTable::instance().getColor(error ? "AlertCautionTextColor" : "LabelTextColor"));
}

void WolfFloaterTunnels::refresh()
{
    LLViewerRegion* rgn = gAgent.getRegion();
    const std::string url = rgn ? rgn->getCapability(CAP) : std::string();
    if (url.empty())
    {
        s_boxes.clear();
        mEdit = false;
        fillList();
        setStatus("This region has no Tunnels yet (it needs the WolfSim update). Ask the region owner.", true);
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
        self->setStatus("The region did not answer (" + status.toString() + "). Press Refresh to try again.", true);
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
    const S32 before = (S32)s_boxes.size();
    mEdit = answer["edit"].asBoolean();
    mMax = answer["max"].asInteger();
    WolfTunnels::setFromAnswer(answer, mRegionAtFetch);   // the same store the terrain and the camera read
    // a new box is selected at once, so it can be fine-tuned
    if (from_post && (S32)s_boxes.size() == before + 1 && !s_boxes.empty()) s_selected = s_boxes.back().mId;
    fillList();
    if (answer.has("message"))
        setStatus(answer["message"].asString(), true);
    else if (from_post)
        setStatus("Saved to the region. " + std::to_string(s_boxes.size()) + " tunnel box(es) here.", false);
    else
        setStatus(mEdit ? (s_boxes.empty() ? "No tunnels here yet. Draw one: press on the ground at its mouth and drag along where it runs."
                                           : std::to_string(s_boxes.size()) + " tunnel box(es) here.")
                        : "You can see this region's tunnels; only the estate owner and managers can change them.", false);
    updateButtons();
}

void WolfFloaterTunnels::fillList()
{
    if (!mList) return;
    mList->deleteAllItems();
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
        mList->addRow(rp);
    }
    if (mInfo) mInfo->setText(mMax > 0 ? llformat("%d of %d boxes", (S32)s_boxes.size(), mMax) : std::string());
    selectId(s_selected);
}

void WolfFloaterTunnels::selectId(S32 id)
{
    s_selected = 0;
    for (const WolfTunnels::Box& b : s_boxes)
    {
        if (b.mId != id) continue;
        s_selected = id;
        mList->selectByValue(LLSD(id));
        mList->scrollToShowSelected();
        const F32 v[7] = { b.mCentre.mV[VX], b.mCentre.mV[VY], b.mCentre.mV[VZ], b.mSize.mV[VX], b.mSize.mV[VY], b.mSize.mV[VZ], b.mYawDeg };
        for (S32 i = 0; i < 7; ++i) mSpin[i]->setValue((F64)v[i]);
    }
    if (!s_selected) mList->deselectAllItems(true);
    updateButtons();
}

void WolfFloaterTunnels::onSelect()
{
    selectId(mList->getSelectedValue().asInteger());
}

void WolfFloaterTunnels::updateButtons()
{
    if (!mWalkBtn) return;
    const bool can = mEdit && !mBusy && mRegionAtFetch.notNull();
    const bool full = mMax > 0 && (S32)s_boxes.size() >= mMax;
    mWalkBtn->setEnabled(can && !full);
    mRailBtn->setEnabled(can && !full);
    mSaveBtn->setEnabled(can && s_selected != 0);
    mDeleteBtn->setEnabled(can && s_selected != 0);
    for (LLSpinCtrl* s : mSpin) s->setEnabled(can && s_selected != 0);
    mGoBtn->setEnabled(s_selected != 0 && !mBusy);
    mRefreshBtn->setEnabled(!mBusy);
}

void WolfFloaterTunnels::onDraw(EKind kind)
{
    WolfToolTunnel::getInstance()->begin(getHandle(), kind);
    setStatus(kind == K_RAIL ? "Rail tunnel: press on the ground at the tunnel's mouth and drag along where it runs. Escape cancels."
                             : "Walking tunnel: press on the ground at the tunnel's mouth and drag along where it runs. Escape cancels.", false);
}

void WolfFloaterTunnels::addDrawn(const WolfTunnels::Box& b)
{
    LLSD body;
    body["op"] = "add";
    body["tunnel"] = boxToLLSD(b);
    setStatus("Saving the new tunnel...", false);
    post(body);
}

void WolfFloaterTunnels::onSave()
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

void WolfFloaterTunnels::onDelete()
{
    if (!s_selected) return;
    const S32 id = s_selected;
    LLHandle<LLFloater> h = getHandle();
    LLSD args;
    args["MESSAGE"] = llformat("Delete tunnel box #%d? Anyone standing in it below the ground is lifted to the surface.", id);
    LLNotificationsUtil::add("GenericAlertYesCancel", args, LLSD(), [h, id](const LLSD& n, const LLSD& r)
    {
        if (LLNotificationsUtil::getSelectedOption(n, r) != 0) return;
        WolfFloaterTunnels* self = static_cast<WolfFloaterTunnels*>(h.get());
        if (!self) return;
        LLSD body;
        body["op"] = "remove";
        body["id"] = id;
        s_selected = 0;
        self->setStatus(llformat("Deleting tunnel box #%d...", id), false);
        self->post(body);
    });
}

void WolfFloaterTunnels::onGoInside()
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn || !s_selected) return;
    for (const WolfTunnels::Box& b : s_boxes)
    {
        if (b.mId != s_selected) continue;
        // a metre above the box's floor (the floor is FLOOR_BELOW under the tunnel's own floor when drawn here)
        const LLVector3 p(b.mCentre.mV[VX], b.mCentre.mV[VY], b.mCentre.mV[VZ] - b.mSize.mV[VZ] * 0.5f + FLOOR_BELOW + 0.5f);
        gAgent.teleportViaLocation(rgn->getPosGlobalFromRegion(p));
        setStatus(llformat("Going inside tunnel box #%d.", b.mId), false);
        return;
    }
}

// =====================================================================================================================
// the draw tool
// =====================================================================================================================

WolfToolTunnel::WolfToolTunnel() : LLTool(std::string("WolfTunnel")) {}

void WolfToolTunnel::begin(LLHandle<LLFloater> floater, S32 kind)
{
    cancel();
    mFloater = floater;
    mKind = llclamp(kind, 0, 1);
    mActive = true;
    // held only while drawing: the base tool stays the pie, so every other click is the pie's (wolftooladventureplace.h)
    LLToolMgr::getInstance()->setTransientTool(this);
}

void WolfToolTunnel::cancel()
{
    if (hasMouseCapture()) setMouseCapture(false);
    mActive = false;
    mDragging = false;
    LLToolMgr* tm = LLToolMgr::getInstance();
    if (tm->getCurrentTool() == this) tm->clearTransientTool();
}

bool WolfToolTunnel::shown() const
{
    WolfFloaterTunnels* f = static_cast<WolfFloaterTunnels*>(mFloater.get());
    if (f && f->getVisible() && !f->isMinimized()) return true;
    // the outlines also show with the window open but no draw begun
    LLFloater* open = LLFloaterReg::findInstance("wolf_tunnels");
    if (!open || !open->getVisible() || open->isMinimized()) return false;
    LLCheckBoxCtrl* check = open->findChild<LLCheckBoxCtrl>("show_outlines");
    return !check || check->get();
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

WolfTunnels::Box WolfToolTunnel::drawnBox() const
{
    WolfTunnels::Box b;
    const F32 w = KIND_W[mKind], h = KIND_H[mKind];
    F32 dx = mEnd.mV[VX] - mStart.mV[VX], dy = mEnd.mV[VY] - mStart.mV[VY];
    F32 len = sqrtf(dx * dx + dy * dy);
    F32 yaw;
    if (len < MIN_DRAG)
    {
        // a click: one default length, facing the way the camera looks
        const LLVector3 at = LLViewerCamera::getInstance()->getAtAxis();
        yaw = atan2f(at.mV[VY], at.mV[VX]);
        len = KIND_L[mKind];
        b.mCentre.set(mStart.mV[VX] + cosf(yaw) * len * 0.5f, mStart.mV[VY] + sinf(yaw) * len * 0.5f, 0.f);
    }
    else
    {
        yaw = atan2f(dy, dx);
        len = llmin(len, MAX_SIZE);
        b.mCentre.set(mStart.mV[VX] + cosf(yaw) * len * 0.5f, mStart.mV[VY] + sinf(yaw) * len * 0.5f, 0.f);
    }
    // the floor a metre under the ground where the drag began (UNDERGROUND_PATCH.md), the box rising h above it
    b.mCentre.mV[VZ] = mStart.mV[VZ] - FLOOR_BELOW + h * 0.5f;
    b.mSize.set(len, w, h);
    b.mYawDeg = wrap360(yaw * RAD_TO_DEG);
    return b;
}

bool WolfToolTunnel::handleAnyMouseClick(S32 x, S32 y, MASK mask, EMouseClickType clicktype, bool down)
{
    if (!mActive || clicktype != CLICK_LEFT) return LLToolPie::getInstance()->handleAnyMouseClick(x, y, mask, clicktype, down);
    return LLTool::handleAnyMouseClick(x, y, mask, clicktype, down);
}

bool WolfToolTunnel::handleScrollWheel(S32 x, S32 y, S32 clicks) { return LLToolPie::getInstance()->handleScrollWheel(x, y, clicks); }

bool WolfToolTunnel::handleMouseDown(S32 x, S32 y, MASK mask)
{
    if (!mActive) return false;
    WolfFloaterTunnels* f = static_cast<WolfFloaterTunnels*>(mFloater.get());
    LLVector3 p;
    if (!surface(x, y, p))
    {
        if (f) f->setStatus("Press on this region's ground, where the tunnel begins.", true);
        return true;
    }
    mStart = mEnd = p;
    mDragging = true;
    setMouseCapture(true);
    return true;
}

bool WolfToolTunnel::handleHover(S32 x, S32 y, MASK mask)
{
    if (!shown()) { cancel(); return false; }
    if (mDragging)
    {
        LLVector3 p;
        if (surface(x, y, p)) mEnd = p;
    }
    gViewerWindow->setCursor(UI_CURSOR_CROSS);
    return true;
}

bool WolfToolTunnel::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (!mDragging) return mActive;
    mDragging = false;
    if (hasMouseCapture()) setMouseCapture(false);
    LLVector3 p;
    if (surface(x, y, p)) mEnd = p;
    const WolfTunnels::Box b = drawnBox();
    cancel();
    if (WolfFloaterTunnels* f = static_cast<WolfFloaterTunnels*>(mFloater.get())) f->addDrawn(b);
    return true;
}

bool WolfToolTunnel::handleKey(KEY key, MASK mask)
{
    if (key == KEY_ESCAPE && mActive)
    {
        cancel();
        if (WolfFloaterTunnels* f = static_cast<WolfFloaterTunnels*>(mFloater.get())) f->setStatus("Drawing cancelled - nothing was changed.", false);
        return true;
    }
    return false;
}

void WolfToolTunnel::onMouseCaptureLost()
{
    mDragging = false;
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

// Source: wolftooladventureplace.cpp WolfToolAdventurePlace::render (lines through gGL, depth-tested, not written).
void WolfToolTunnel::render()
{
    if (!shown()) return;
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return;
    const bool here = rgn->getRegionID() == s_boxesRegion;
    std::vector<LLVector3> rest, sel, live;
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    if (here)
    {
        for (const WolfTunnels::Box& b : s_boxes)
        {
            if (dist_vec(rgn->getPosAgentFromRegion(b.mCentre), cam) > CULL_M + b.mSize.magVec()) continue;
            boxEdges(rgn, b, b.mId == s_selected ? sel : rest);
        }
    }
    if (mDragging) boxEdges(rgn, drawnBox(), live);
    if (rest.empty() && sel.empty() && live.empty()) return;
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    LLGLEnable blend(GL_BLEND);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    // the boxes are under the ground: drawn through it (no depth test) so an owner can see where they run
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
    lines(rest, 1.5f, 0.49f, 0.83f, 0.99f, 0.55f);   // ice: the region's tunnels
    lines(sel, 3.f, 0.96f, 0.65f, 0.14f, 0.95f);     // amber: the one picked in the list (its numbers are in the window too)
    lines(live, 3.f, 1.f, 1.f, 1.f, 1.f);            // white: the box being drawn
    gGL.setLineWidth(1.f);
}

void WolfToolTunnel::draw()
{
    if (!mActive) return;
    // what the tool wants, at the top of the world view (wolftooladventureplace.cpp draw)
    const LLFontGL* f = LLFontGL::getFontSansSerifSmall();
    std::string h = mKind == 1 ? "Rail tunnel" : "Walking tunnel";
    if (mDragging)
    {
        const WolfTunnels::Box b = drawnBox();
        h += llformat(": %.0f m long, turned %.0f\xC2\xB0. Release to save it.", b.mSize.mV[VX], b.mYawDeg);
    }
    else
    {
        h += ": press on the ground at the tunnel's mouth and drag along where it runs. Escape cancels.";
    }
    const F32 tw = (F32)f->getWidthF32(h);
    const F32 cx = (F32)gViewerWindow->getWorldViewWidthScaled() * 0.5f, ty = (F32)gViewerWindow->getWorldViewHeightScaled() - 60.f;
    WolfGameDraw::rect(cx - tw * 0.5f - 8.f, ty - 4.f, cx + tw * 0.5f + 8.f, ty + (F32)f->getLineHeight() + 4.f, LLColor4(0.f, 0.f, 0.f, 0.75f));
    f->renderUTF8(h, 0, cx, ty, LLColor4::white, LLFontGL::HCENTER, LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
}
