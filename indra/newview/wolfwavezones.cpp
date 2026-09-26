/**
 * @file wolfwavezones.cpp
 * @brief WolfViewer: per-region wave zones — the layout set in About Land > Waves.
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
#include <algorithm>
#include <cmath>

#include "wolfwavezones.h"

#include <boost/json.hpp>

#include "llagent.h"
#include "llagentcamera.h"
#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "llfocusmgr.h"
#include "llframetimer.h"
#include "llhttpconstants.h"
#include "llnotificationsutil.h"
#include "llrender2dutils.h"
#include "llsdjson.h"
#include "llselectmgr.h"
#include "llsliderctrl.h"
#include "lltextbox.h"
#include "lltool.h"
#include "lltoolmgr.h"
#include "llviewerwindow.h"
#include "llviewershadermgr.h"
#include "lluictrlfactory.h"
#include "llparcel.h"            // PARCEL_GRID_STEP_METERS
#include "llimage.h"
#include "llsurface.h"
#include "llviewerparceloverlay.h"
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "llworld.h"
#include "llworldmap.h"        // <WolfViewer 2026-09-26/> map tiles under the painter on huge regions
#include "llworldmipmap.h"
#include "wolfgrid.h"
#include "wolfnearbyregions.h"
#include "wolfwaterfield.h"
#include "wolfwavebrush.h"

// Source: wolfstorm/js/world/wave_zones.js API — one host owns the data for every viewer.
const char* WolfWaveZones::API_URL = "https://wolfstorm.app/php/waves.php";

static LLDefaultChildRegistry::Register<WolfWavePainter> r_wolf_wave_painter("wolf_wave_painter");

namespace
{
    // Source: wave_zones.js ENERGY.
    F32 energy_of(char z)
    {
        switch (z)
        {
            case 's': return 1.0f;
            case 'm': return 0.35f;   // [2026-09-10] small waves
            case 'c': return 0.15f;
            case 'x': return 0.0f;
            default:  return WolfWaveZones::OPEN_ENERGY;
        }
    }

    // Region handles are (x << 32) | y in metres (indra/llmath/v3dmath.h from_region_handle).
    void handle_xy(U64 handle, S32& x, S32& y)
    {
        x = (S32)(handle >> 32);
        y = (S32)(handle & 0xffffffffULL);
    }

    /** Parse a JSON body into LLSD, or an undefined LLSD when it is not JSON. */
    LLSD json_to_llsd(const LLSD::Binary& bytes)
    {
        std::string text(bytes.begin(), bytes.end());
        boost::system::error_code ec;
        boost::json::value v = boost::json::parse(text, ec);
        if (ec) return LLSD();
        return LlsdFromJson(v);
    }

    void notify(const std::string& msg)
    {
        LLNotificationsUtil::add("GenericAlertOK", LLSD().with("MESSAGE", msg));
    }
}

F32 WolfWaveZones::energyOf(char z) { return energy_of(z); }

// Source: php/waves.php waves_cell() — the same rule, so a payload without `cell` still agrees.
S32 WolfWaveZones::cellFor(S32 sizeX, S32 sizeY)
{
    S32 cell = CELL_M;
    const S32 edge = llmax(llmax(sizeX, sizeY), 256);
    while (edge / cell > MAX_CELLS_EDGE) cell *= 2;
    return cell;
}

// <WolfViewer 2026-09-26> From the 16 m paint cell on every region (was the region's own cell).
// Source: wave_zones.js bake() texel rule.
S32 WolfWaveZones::texelM(F32 span_m)
{
    S32 texel = PAINT_CELL_M;
    while (span_m / (F32)texel > 768.f) texel *= 2;
    return texel;
}

// <WolfViewer 2026-09-26> Layout v2 tiles. Source: wave_zones.js setTileCell / tilesToJSON /
// storedTiles / zoneOf, php/waves.php waves_tiles_clean.
bool WolfWaveZones::setTileCell(Tiles& tiles, S32 fx, S32 fy, char z)
{
    if (fx < 0 || fy < 0) return false;
    std::string& t = tiles[tileKey(fx >> 4, fy >> 4)];
    if (t.size() != (size_t)TILE_LEN) t.assign(TILE_LEN, '.');
    char& c = t[((fy & 15) << 4) | (fx & 15)];
    if (c == z) return false;
    c = z;
    return true;
}

LLSD WolfWaveZones::tilesToLLSD(const Tiles& tiles)
{
    LLSD out = LLSD::emptyMap();   // {} on the wire even with nothing painted: the service refuses null
    for (const auto& kv : tiles)
    {
        const std::string& t = kv.second;
        if (t.size() != (size_t)TILE_LEN || t.find_first_not_of('.') == std::string::npos) continue;
        const bool uniform = t.find_first_not_of(t[0]) == std::string::npos;
        out[std::to_string(kv.first % 65536u) + "," + std::to_string(kv.first / 65536u)] = uniform ? std::string(1, t[0]) : t;
    }
    return out;
}

WolfWaveZones::Tiles WolfWaveZones::parseTiles(const LLSD& layout, S32 pw, S32 ph)
{
    Tiles out;
    if (layout["v"].asInteger() != 2)
    {
        LL_WARNS("WolfWaveZones") << "layout is not v2; its painted cells are not shown" << LL_ENDL;
        return out;
    }
    const LLSD& tiles = layout["tiles"];
    if (!tiles.isMap()) return out;   // {} can arrive as an empty map or undefined: nothing painted
    const U32 tw = (U32)((pw + TILE_CELLS - 1) / TILE_CELLS), th = (U32)((ph + TILE_CELLS - 1) / TILE_CELLS);
    for (LLSD::map_const_iterator it = tiles.beginMap(); it != tiles.endMap(); ++it)
    {
        const std::string& key = it->first;
        const size_t comma = key.find(',');
        if (key.size() > 11 || comma == std::string::npos || comma == 0 || comma + 1 >= key.size()
            || key.find_first_not_of("0123456789,") != std::string::npos || key.find(',', comma + 1) != std::string::npos) continue;
        if (!it->second.isString()) continue;
        const U32 tx = (U32)std::stoul(key.substr(0, comma)), ty = (U32)std::stoul(key.substr(comma + 1));
        if (tx >= tw || ty >= th) continue;
        std::string cells = it->second.asString();
        if (cells.size() == 1) cells.assign(TILE_LEN, cells[0]);
        else if (cells.size() != (size_t)TILE_LEN) continue;
        if (cells.find_first_not_of("somcx.") != std::string::npos) continue;
        out[tileKey((S32)tx, (S32)ty)] = cells;
    }
    return out;
}

char WolfWaveZones::Source::at(S32 fx, S32 fy) const
{
    if (mTiles)
    {
        auto it = mTiles->find(tileKey(fx >> 4, fy >> 4));
        if (it != mTiles->end() && it->second.size() == (size_t)TILE_LEN)
        {
            const char c = it->second[((fy & 15) << 4) | (fx & 15)];
            if (c != '.') return c;
        }
    }
    const S32 ax = fx * PAINT_CELL_M / mAutoCell, ay = fy * PAINT_CELL_M / mAutoCell;
    const size_t k = (size_t)ay * mAutoW + ax;
    return k < mAuto.size() ? mAuto[k] : 'o';
}

// Source: wolfstorm wave_zones.js WaveZones.zoneScale — same numbers, keep in step.
F32 WolfWaveZones::zoneScale(F32 e, F32 amplitude, F32 calm_ripple, F32 small_scale)
{
    const F32 a = llmax(amplitude, 0.02f);
    const F32 calm_floor = llclamp(calm_ripple / a, 0.f, 1.f);
    const F32 small = llclamp(small_scale, calm_floor, 1.f);
    if (e <= 0.f) return 0.f;
    if (e < 0.15f) return calm_floor * (e / 0.15f);
    if (e < 0.35f) return calm_floor + (small - calm_floor) * ((e - 0.15f) / 0.20f);
    if (e < 0.55f) return small + (1.f - small) * ((e - 0.35f) / 0.20f);
    return 1.f;
}

F64 WolfWaveZones::lastFetchAgeSecs() const
{
    // explicit F64: getElapsedSeconds() is an LLUnitImplicit and the ternary needs one type
    return mLastFetchAt > 0.0 ? (F64)LLFrameTimer::getElapsedSeconds() - mLastFetchAt : 1e9;
}

WolfWaveZones::WolfWaveZones()
{
    // Source: LLAgent::setRegion emits on each transition, including leaving and returning
    // to the same UUID while an older request is suspended.
    mRegionChangedConnection = gAgent.addRegionChangedCallback([this]() { ++mFetchGeneration; });
}

WolfWaveZones::~WolfWaveZones()
{
    if (mRegionChangedConnection.connected()) mRegionChangedConnection.disconnect();
}

// ── fetch ──────────────────────────────────────────────────────────────────────────────

std::vector<U64> WolfWaveZones::neighbourHandles() const
{
    // <WolfViewer 2026-09-23> The regions the viewer is actually connected to, the agent's own
    // first, nearest next - not a guessed ring of corner offsets, which missed any neighbour
    // not sitting on one of them (wolfnearbyregions.h). 24 = the service's own cap
    // (php/waves.php WAVES_MAX_HANDLES; wt_regions_by_handles fails the whole request above it).
    return WolfNearbyRegions::handles(24);
}

void WolfWaveZones::idle()
{
    // Other grids get the automatic layout: nothing is asked of the Wolf Territories service.
    if (!WolfGrid::isWolfTerritories()) return;
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return;
    const F64 now = LLFrameTimer::getElapsedSeconds();
    // <WolfViewer 2026-09-23> ...or when a neighbour connects or drops: after a crossing the
    // fetch goes out before the neighbours have all connected, and a region that connects
    // later would otherwise wait REFRESH_SECS for its waves. The neighbour set is sorted from the
    // whole region list, so it is compared once a second rather than every frame.
    bool neighbours_changed = false;
    if (now >= mNextNeighbourCheck)
    {
        mNextNeighbourCheck = now + 1.0;
        // An empty set (no region yet) makes refresh() return before recording it.
        const std::vector<U64> handles = neighbourHandles();
        neighbours_changed = !handles.empty() && handles != mRequestedHandles;
    }
    if (rgn->getHandle() != mFetchedForHandle || now >= mNextRefresh || neighbours_changed)
    {
        refresh();
    }
}

void WolfWaveZones::refresh()
{
    if (mFetching) return;
    const LLViewerRegion* region = gAgent.getRegion();
    if (!region) return;
    std::vector<U64> handles = neighbourHandles();
    if (handles.empty()) return;
    mFetching = true;
    mRequestedHandles = handles;
    mNextRefresh = LLFrameTimer::getElapsedSeconds() + REFRESH_SECS;
    const U64 requested_handle = region->getHandle();
    const U64 generation = mFetchGeneration;
    LLCoros::instance().launch("WolfWaveZones fetch", [handles, requested_handle, generation]() {
        WolfWaveZones::instance().fetchCoro(handles, requested_handle, generation);
    });
}

// Source: wolfspeech.cpp postRaw for the adapter shape; llcorehttputil.h getRawAndSuspend.
void WolfWaveZones::fetchCoro(std::vector<U64> handles, U64 requested_handle, U64 generation)
{
    std::string url = std::string(API_URL) + "?v=2&handles=";   // <WolfViewer 2026-09-26/> layout v2
    for (size_t i = 0; i < handles.size(); ++i)
    {
        if (i) url += ",";
        url += std::to_string(handles[i]);
    }
    LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
        std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfWaveZones", LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
    LLCore::HttpOptions::ptr_t options = std::make_shared<LLCore::HttpOptions>();
    options->setTimeout(20);
    LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
    headers->append(HTTP_OUT_HEADER_ACCEPT, "application/json");
    // <WolfViewer 2026-09-22> Identify ourselves on READS too, not only on writes.
    // wt_authenticate() already validates this pair against the grid presence service
    // (METHOD getagent), so a session from any other grid cannot satisfy it. Reads have
    // always been anonymous, which let anyone pull Wolf Territories' saved layouts.
    // Sending them now costs nothing -- the service ignores headers it does not require
    // -- and is what lets the service START requiring them once enough viewers carry
    // them. Enforcing before then would break every older viewer ON our own grid.
    headers->append("X-Wolf-Agent", gAgentID.asString());
    headers->append("X-Wolf-Session", gAgentSessionID.asString());

    LLSD result = adapter->getRawAndSuspend(request, url, options, headers);
    LLSD httpResults = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(httpResults);
    mFetching = false;
    // The attempted region is the request's target, not the avatar's location on completion.
    // Reject an invalidated visit before it replaces destination records or errors.
    const LLViewerRegion* region_now = gAgent.getRegion();
    if (generation != mFetchGeneration || !region_now || region_now->getHandle() != requested_handle)
    {
        mFetchedForHandle = 0;
        return;
    }
    // Failed current-target reads remain throttled on REFRESH_SECS, never every frame.
    mFetchedForHandle = requested_handle;
    if (!status)
    {
        mLastError = status.toString();
        LL_WARNS("WolfWaveZones") << "fetch failed: " << mLastError << LL_ENDL;
        return;
    }
    LLSD reply;
    if (result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
    {
        reply = json_to_llsd(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary());
    }
    if (!reply.isMap() || !reply["success"].asBoolean() || !reply["regions"].isArray())
    {
        mLastError = "bad reply";
        LL_WARNS("WolfWaveZones") << "fetch: unexpected reply" << LL_ENDL;
        return;
    }
    const auto previous = std::move(mByHandle);
    mByHandle.clear();
    for (const LLSD& r : llsd::inArray(reply["regions"]))
    {
        Region rec;
        rec.mUuid = r["region"].asString();
        rec.mName = r["name"].asString();
        rec.mHandle = std::strtoull(r["handle"].asString().c_str(), nullptr, 10);
        rec.mSizeX = llmax(256, r["sizeX"].asInteger());
        rec.mSizeY = llmax(256, r["sizeY"].asInteger());
        // [2026-09-10] the service's cell for this region (php/waves.php waves_cell); its own
        // rule if an older service omits it.
        rec.mCell = r.has("cell") ? llmax(CELL_M, r["cell"].asInteger()) : cellFor(rec.mSizeX, rec.mSizeY);
        rec.mVersion = r["version"].asInteger();
        rec.mEnabled = r["enabled"].asBoolean();
        if (r["layout"].isMap())
        {
            rec.mStored = true;
            rec.mTiles = parseTiles(r["layout"], rec.pw(), rec.ph());
            rec.mParams = r["layout"]["params"];
        }
        if (rec.mHandle)
        {
            // A GET started before a successful POST must not roll the accepted row back.
            const auto old = previous.find(rec.mHandle);
            mByHandle[rec.mHandle] = old != previous.end() && old->second.mVersion > rec.mVersion ? old->second : rec;
        }
    }
    mLastError.clear();
    mLastFetchAt = LLFrameTimer::getElapsedSeconds();
    LL_INFOS("WolfWaveZones") << "layouts for " << mByHandle.size() << " regions" << LL_ENDL;
    // The water bakes the zone span with its fields: make it do so again now.
    WolfWaterField::instance().invalidate();
}

// ── layouts ────────────────────────────────────────────────────────────────────────────

const WolfWaveZones::Region* WolfWaveZones::current() const
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return nullptr;
    auto it = mByHandle.find(rgn->getHandle());
    return it == mByHandle.end() ? nullptr : &it->second;
}

const WolfWaveZones::Tiles* WolfWaveZones::paintedFor(const Region& r) const
{
    auto pv = mPreview.find(r.mHandle);
    if (pv != mPreview.end()) return pv->second.mEnabled ? &pv->second.mTiles : nullptr;
    return r.mEnabled ? &r.mTiles : nullptr;
}

WolfWaveZones::Source WolfWaveZones::sourceFor(const Region& r) const
{
    Source src;
    src.mTiles = paintedFor(r);
    src.mAuto = defaultZones(r);
    src.mAutoW = r.w();
    src.mAutoCell = r.mCell;
    src.mW = r.pw();
    src.mH = r.ph();
    return src;
}

// Source: wave_zones.js defaultZones — the automatic layout. A cell is SEA ('o': full swell,
// wind sea, breakers, swash) when its water connects, through water, to the region edge AND
// open water (exposure >= 0.5) lies within COAST_REACH_M of it; every other water cell is
// ENCLOSED ('m': small waves, smallScale of the open sea, same direction, no breakers and no
// swash foam — waterV.glsl shoreGate): a dug lake, a pond, a river or a narrow inlet. Land
// cells take the class of the water they touch (sea wins), so the blur in fill() cannot dim
// the sea at its own shoreline and the breakers still roll in.
//
// Connectivity, NOT the exposure alone: exposure is distance to the nearest land, so it is
// below 0.3 for ALL water within ~56 m of any shore — which classed the whole near-shore band
// of an open coast as enclosed and, with the foam gate, removed the breakers that live there
// (Paul 09-10: "still no waves rolling into the shore"). The flood fill starts from every
// water cell on the region border (the void and the neighbours are sea) and walks through
// water cells; heights are the region's own drawn surface (LLSurface::resolveHeightRegion,
// five samples per cell, any below the water level = water). A region whose terrain is not
// loaded (a neighbour not yet in LLWorld) falls back to the exposure rule.
// [2026-09-10, Paul] "a lake in a region doesn't have waves unless they define it" (the white
// foamy lake in Jimmy Olsen's screenshot), "the water is flat and boring" (first cut: all
// off), "water enclosed by land also needs character and direction" (second cut: flat lakes).
std::string WolfWaveZones::defaultZones(const Region& r) const
{
    static constexpr F32 COAST_REACH_M = 96.f;
    static constexpr F32 OPEN_EXPOSURE = 0.5f;
    const S32 w = r.w(), h = r.h();
    const F32 cell = (F32)r.mCell;
    LLViewerRegion* agent_rgn = gAgent.getRegion();
    const WolfWaterField::Field* fld = agent_rgn ? WolfWaterField::instance().get(agent_rgn) : nullptr;
    F32 ox = 0.f, oy = 0.f;
    if (agent_rgn && r.mHandle != agent_rgn->getHandle())
    {
        S32 ax, ay, bx, by;
        handle_xy(r.mHandle, ax, ay);
        handle_xy(agent_rgn->getHandle(), bx, by);
        ox = (F32)(ax - bx);
        oy = (F32)(ay - by);
    }
    std::vector<F32> expo((size_t)w * h, 1.f);
    for (S32 cy = 0; cy < h; ++cy)
        for (S32 cx = 0; cx < w; ++cx)
            expo[(size_t)cy * w + cx] = fld ? WolfWaterField::exposureAt(*fld, ox + (cx + 0.5f) * cell, oy + (cy + 0.5f) * cell) : 1.f;

    LLViewerRegion* rgn = LLWorld::getInstance()->getRegionFromHandle(r.mHandle);
    std::string out((size_t)w * h, 'o');
    if (!rgn)
    {
        // No terrain for this region: the exposure rule alone.
        for (size_t k = 0; k < out.size(); ++k) out[k] = expo[k] >= 0.3f ? 'o' : 'm';
        return out;
    }
    const LLSurface& land = rgn->getLand();
    const F32 wl = rgn->getWaterHeight();
    std::vector<U8> water((size_t)w * h, 0);
    for (S32 cy = 0; cy < h; ++cy)
    {
        for (S32 cx = 0; cx < w; ++cx)
        {
            const F32 mx = (cx + 0.5f) * cell, my = (cy + 0.5f) * cell, q = cell * 0.25f;
            const F32 sx[5] = { mx, mx - q, mx + q, mx - q, mx + q };
            const F32 sy[5] = { my, my - q, my - q, my + q, my + q };
            for (S32 i = 0; i < 5; ++i)
            {
                if (land.resolveHeightRegion(sx[i], sy[i]) < wl - 0.05f) { water[(size_t)cy * w + cx] = 1; break; }
            }
        }
    }
    // Flood fill from the border: sea is what the void can reach through water.
    std::vector<U8> sea((size_t)w * h, 0);
    std::vector<S32> stack;
    auto seed = [&](S32 cx, S32 cy) { const size_t k = (size_t)cy * w + cx; if (water[k] && !sea[k]) { sea[k] = 1; stack.push_back((S32)k); } };
    for (S32 cx = 0; cx < w; ++cx) { seed(cx, 0); seed(cx, h - 1); }
    for (S32 cy = 0; cy < h; ++cy) { seed(0, cy); seed(w - 1, cy); }
    while (!stack.empty())
    {
        const S32 k = stack.back(); stack.pop_back();
        const S32 cx = k % w, cy = k / w;
        if (cx > 0)     seed(cx - 1, cy);
        if (cx < w - 1) seed(cx + 1, cy);
        if (cy > 0)     seed(cx, cy - 1);
        if (cy < h - 1) seed(cx, cy + 1);
    }
    // Sea cells with open water within reach are 'o'; every other water cell is 'm'.
    const S32 reach = llmax(1, (S32)ceilf(COAST_REACH_M / cell));
    for (S32 cy = 0; cy < h; ++cy)
    {
        for (S32 cx = 0; cx < w; ++cx)
        {
            const size_t k = (size_t)cy * w + cx;
            if (!water[k]) continue;
            bool open = false;
            if (sea[k])
            {
                for (S32 dy = -reach; dy <= reach && !open; ++dy)
                    for (S32 dx = -reach; dx <= reach; ++dx)
                    {
                        const S32 nx = cx + dx, ny = cy + dy;
                        if (nx < 0 || ny < 0 || nx >= w || ny >= h) { open = true; break; }   // past the border: the void sea
                        if (sea[(size_t)ny * w + nx] && expo[(size_t)ny * w + nx] >= OPEN_EXPOSURE) { open = true; break; }
                    }
            }
            out[k] = open ? 'o' : 'm';
        }
    }
    // Land cells: the class of the water they touch, sea winning; inland land stays 'o'
    // (nothing is drawn there, and it must not dim a neighbouring sea cell in the blur).
    std::string land_out(out);
    for (S32 cy = 0; cy < h; ++cy)
    {
        for (S32 cx = 0; cx < w; ++cx)
        {
            const size_t k = (size_t)cy * w + cx;
            if (water[k]) continue;
            bool near_sea = false, near_enclosed = false;
            for (S32 dy = -1; dy <= 1; ++dy)
                for (S32 dx = -1; dx <= 1; ++dx)
                {
                    const S32 nx = cx + dx, ny = cy + dy;
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    const size_t j = (size_t)ny * w + nx;
                    if (!water[j]) continue;
                    if (out[j] == 'o') near_sea = true; else near_enclosed = true;
                }
            land_out[k] = (near_sea || !near_enclosed) ? 'o' : 'm';
        }
    }
    return land_out;
}

void WolfWaveZones::fill(LLViewerRegion* regionp, F32 x0, F32 y0, F32 sx, F32 sy, S32 w, S32 h, std::vector<F32>& out) const
{
    out.assign((size_t)w * h, OPEN_ENERGY);
    if (!regionp || mByHandle.empty()) return;
    S32 bx, by;
    handle_xy(regionp->getHandle(), bx, by);
    // <WolfViewer 2026-09-26> Each region's 16 m painted cells over its automatic layout (Source).
    struct Span { F32 x, y, sx, sy; Source src; };
    std::vector<Span> spans;
    for (const auto& kv : mByHandle)
    {
        const Region& r = kv.second;
        S32 ax, ay;
        handle_xy(r.mHandle, ax, ay);
        spans.push_back({ (F32)(ax - bx), (F32)(ay - by), (F32)r.mSizeX, (F32)r.mSizeY, sourceFor(r) });
    }
    const F32 tx = sx / w, ty = sy / h;
    const F32 rw = regionp->getWidth(), rh = regionp->getWidth();
    const Span* mine = nullptr;
    for (const Span& s : spans) if (s.x == 0.f && s.y == 0.f) { mine = &s; break; }
    if (mine)
    {
        size_t surf = 0;
        if (mine->src.mTiles)
            for (const auto& t : *mine->src.mTiles) surf += std::count(t.second.begin(), t.second.end(), 's');
        const bool previewed = mPreview.find(regionp->getHandle()) != mPreview.end();
        LL_INFOS("WolfWaveZones") << "fill for " << regionp->getName() << ": " << (mine->src.mTiles ? mine->src.mTiles->size() : 0)
                                  << " painted tiles, " << surf << " painted surf cells, source "
                                  << (previewed ? "PREVIEW" : "stored/default") << LL_ENDL;
    }
    const S32 per = llmax(1, (S32)ceilf(tx / (F32)PAINT_CELL_M));
    for (S32 j = 0; j < h; ++j)
    {
        const F32 py = y0 + (j + 0.5f) * ty;
        for (S32 i = 0; i < w; ++i)
        {
            const F32 px = x0 + (i + 0.5f) * tx;
            bool covered = false;
            for (const Span& s : spans)
            {
                if (px < s.x || py < s.y || px >= s.x + s.sx || py >= s.y + s.sy) continue;
                // <WolfViewer 2026-09-18> A texel wider than the cell (texelM doubles the cell
                // until the 3x3 span fits 768 texels: 256 m on a 200x200 region, 64 m cells)
                // used to read ONE cell at its centre, so a one-cell surf stroke was sampled
                // away and "literally nothing happens" when painting. Read every cell under
                // the texel and keep the highest energy: a painted surf cell shows through.
                // (A single OFF cell inside open sea is lost at that scale; surf is what a
                // designer paints on a coast.) wave_zones.js bake() same.
                const S32 cx0 = (S32)floorf((px - s.x - tx * 0.5f) / PAINT_CELL_M), cy0 = (S32)floorf((py - s.y - ty * 0.5f) / PAINT_CELL_M);
                F32 best = -1.f;
                for (S32 cy = llmax(0, cy0); cy < llmin(s.src.mH, cy0 + per); ++cy)
                    for (S32 cx = llmax(0, cx0); cx < llmin(s.src.mW, cx0 + per); ++cx)
                        best = llmax(best, energy_of(s.src.at(cx, cy)));
                out[(size_t)j * w + i] = best < 0.f ? OPEN_ENERGY : best;
                covered = true;
                break;
            }
            // Space no region covers (the void sea round the region) is OPEN water (Paul
            // 09-10: waves "only at the outside" of a region) — except beyond a SURF edge cell,
            // which it continues so the surf painted along the edge rolls in from far out
            // instead of fading over the last texel before the border ([SURF 2026-09-07],
            // Paul: "waves from the edge of the region"). Off / calm / small edge cells do NOT
            // reach out: the 48 m blur below is the ramp from the open sea outside to the
            // painted cell inside. wave_zones.js bake() same.
            if (!covered && mine)
            {
                const F32 qx = llclamp(px, 0.f, rw - 0.5f), qy = llclamp(py, 0.f, rh - 0.5f);
                if (mine->src.at((S32)(qx / PAINT_CELL_M), (S32)(qy / PAINT_CELL_M)) == 's') out[(size_t)j * w + i] = energy_of('s');
            }
        }
    }
    // [SURF rev3] 3x3 box blur (48 m footprint): a bilinear read of 16 m cells stepped the
    // crest height every cell and the surf came out TERRACED (Paul's Sandbox screenshot,
    // 09-07). A designer paints cells; the sea needs a 48 m ramp between them. wave_zones.js same.
    std::vector<F32> src(out);
    for (S32 j = 0; j < h; ++j)
    {
        for (S32 i = 0; i < w; ++i)
        {
            F32 acc = 0.f; S32 n = 0;
            for (S32 dj = -1; dj <= 1; ++dj)
            {
                const S32 jj = j + dj; if (jj < 0 || jj >= h) continue;
                for (S32 di = -1; di <= 1; ++di)
                {
                    const S32 ii = i + di; if (ii < 0 || ii >= w) continue;
                    acc += src[(size_t)jj * w + ii]; ++n;
                }
            }
            out[(size_t)j * w + i] = acc / (F32)n;
        }
    }
}

F32 WolfWaveZones::energyAt(F32 rx, F32 ry) const
{
    LLViewerRegion* rgn = gAgent.getRegion();
    const WolfWaterField::Field* fld = rgn ? WolfWaterField::instance().get(rgn) : nullptr;
    return fld ? WolfWaterField::zoneAt(*fld, rx, ry) : OPEN_ENERGY;
}

// ── rights (courtesy copy of the service's rule) ──────────────────────────────────────

// <WolfViewer 2026-09-18> Source: wave_zones.js cellHasWater. A cell is water if ANY of five
// points in it is under the water - the centre and the four quarter points. A centre-only test
// refused every shoreline cell on a 256 m grid, which is exactly where surf is painted.
bool WolfWaveZones::cellHasWater(LLViewerRegion* regionp, S32 cx, S32 cy, S32 cell)
{
    if (!regionp) return false;
    const F32 wl = regionp->getWaterHeight();
    const LLSurface& land = regionp->getLand();
    static const F32 pts[5][2] = { { 0.5f, 0.5f }, { 0.25f, 0.25f }, { 0.75f, 0.25f }, { 0.25f, 0.75f }, { 0.75f, 0.75f } };
    for (const auto& f : pts)
    {
        if (land.resolveHeightRegion((cx + f[0]) * cell, (cy + f[1]) * cell) < wl) return true;
    }
    return false;
}
// </WolfViewer>

bool WolfWaveZones::canEditAll() const
{
    // Source: llviewerregion.cpp canManageEstate — godlike || estate manager || region owner.
    return gAgent.canManageEstate();
}

bool WolfWaveZones::cellEditable(S32 cx, S32 cy) const
{
    if (canEditAll()) return true;
    LLViewerRegion* rgn = gAgent.getRegion();
    LLViewerParcelOverlay* overlay = rgn ? rgn->getParcelOverlay() : nullptr;
    if (!overlay) return false;
    // Every 4 m parcel tile under the cell must be the agent's own
    // (llviewerparceloverlay.cpp isOwnedSelf: PARCEL_SELF at that tile).
    const S32 cell = PAINT_CELL_M;   // <WolfViewer 2026-09-26/> the 16 m paint cell on every region
    const S32 per = cell / (S32)PARCEL_GRID_STEP_METERS;
    for (S32 ty = 0; ty < per; ++ty)
    {
        for (S32 tx = 0; tx < per; ++tx)
        {
            const LLVector3 pos((F32)(cx * cell + tx * (S32)PARCEL_GRID_STEP_METERS) + 2.f,
                                (F32)(cy * cell + ty * (S32)PARCEL_GRID_STEP_METERS) + 2.f, 0.f);
            if (!overlay->isOwnedSelf(pos)) return false;
        }
    }
    return true;
}

void WolfWaveZones::preview(const Tiles& tiles, bool enabled)
{
    if (!WolfGrid::isWolfTerritories()) { notify("Sorry, this function is only available on Wolf Territories Grid."); return; }
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return;
    ++mPreviewRevision;
    Preview& pv = mPreview[rgn->getHandle()];
    pv.mEnabled = enabled;
    if (enabled) pv.mTiles = tiles; else pv.mTiles.clear();
    LL_INFOS("WolfWaveZones") << "preview set for handle " << rgn->getHandle() << ": "
                              << (enabled ? std::to_string(tiles.size()) + " painted tiles" : std::string("automatic layout")) << LL_ENDL;
    WolfWaterField::instance().invalidate();
}

void WolfWaveZones::previewParams(const LLSD& params)
{
    if (!WolfGrid::isWolfTerritories()) { notify("Sorry, this function is only available on Wolf Territories Grid."); return; }
    LLViewerRegion* region = gAgent.getRegion();
    if (!region) return;
    // <WolfViewer 2026-09-21> Most of these are uniforms read every frame through params() and
    // need no rebake. The surf WAVELENGTH is not: the exposure bake integrates the surf's
    // optical path against it (wolfwaterfield.cpp), so moving that slider — or the height
    // slider, since the wavelength is max(surfLength, 12 * surfHeight) — leaves the baked path
    // describing a different wave from the one being drawn. Rebake, but only when the
    // wavelength actually moved; a rebake per slider tick on every other control would be a
    // hitch for nothing. WolfWaterField paces itself at MIN_REBAKE_SECS regardless.
    const F32 k0_before = WolfWaterField::surfK0();
    ++mPreviewRevision;
    mPreviewParamsFor = region->getHandle();
    mPreviewParams = params;
    if (WolfWaterField::surfK0() != k0_before)
    {
        WolfWaterField::instance().invalidate();
    }
}

const LLSD& WolfWaveZones::params() const
{
    static const LLSD none;
    LLViewerRegion* region = gAgent.getRegion();
    if (region && region->getHandle() == mPreviewParamsFor && mPreviewParams.isMap()) return mPreviewParams;
    const Region* r = current();
    return r ? r->mParams : none;
}

void WolfWaveZones::clearPreview()
{
    ++mPreviewRevision;
    mPreviewParamsFor = 0;
    mPreviewParams = LLSD();
    if (mPreview.empty()) return;
    mPreview.clear();
    LL_INFOS("WolfWaveZones") << "preview cleared" << LL_ENDL;
    WolfWaterField::instance().invalidate();
}

// ── save ───────────────────────────────────────────────────────────────────────────────

bool WolfWaveZones::save(const SaveTarget& target, const Tiles& tiles, const LLSD& params, bool enabled)
{
    // Source: WolfGrid login identity; the server separately verifies the captured request.
    if (!WolfGrid::isWolfTerritories()) { mLastSaveError = "Sorry, this function is only available on Wolf Territories Grid."; notify(mLastSaveError); return false; }
    const Region* r = current();
    if (!r || r->mUuid != target.mUuid || r->mHandle != target.mHandle)
    {
        mLastSaveError = "The region changed. Reopen Waves before saving.";
        notify(mLastSaveError);
        return false;
    }
    if (mSaving) { mLastSaveError = "A wave layout is already saving."; return false; }
    mSaving = true;
    mLastSaveError.clear();
    const U64 preview_revision = mPreviewRevision;
    // Source: php/waves.php region/version checks and browser WaveZones.save: preserve the
    // editor's version across polling and its region across coroutine suspension/retry.
    LLCoros::instance().launch("WolfWaveZones save", [target, tiles, params, enabled, preview_revision]()
    {
        WolfWaveZones::instance().saveCoro(target, tiles, params, enabled, preview_revision, false);
    });
    return true;
}

// Source: wolfspeech.cpp postRaw — the agent and session ids the service verifies against
// the grid's presence service; no secret is carried by this (public) viewer.
void WolfWaveZones::saveCoro(SaveTarget target, Tiles tiles, LLSD params, bool enabled, U64 preview_revision, bool retried)
{
    LLSD body;
    body["region"] = target.mUuid;
    // <WolfViewer 2026-09-26> layout v2: the painted 16 m tiles (php/waves.php WAVES_PAINT_CELL_M)
    body["layout"] = LLSD().with("v", 2).with("tiles", tilesToLLSD(tiles)).with("params", params);
    body["enabled"] = enabled;
    body["version"] = target.mVersion;
    const std::string text = boost::json::serialize(LlsdToJson(body));

    LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
        std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfWaveZones", LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
    LLCore::HttpOptions::ptr_t options = std::make_shared<LLCore::HttpOptions>();
    options->setTimeout(30);
    LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
    headers->append(HTTP_OUT_HEADER_CONTENT_TYPE, "application/json");
    headers->append("X-Wolf-Agent", gAgentID.asString());
    headers->append("X-Wolf-Session", gAgentSessionID.asString());
    LLCore::BufferArray::ptr_t raw(new LLCore::BufferArray());
    raw->append(text.data(), text.size());

    LLSD result = adapter->postRawAndSuspend(request, API_URL, raw, options, headers);
    LLSD httpResults = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(httpResults);
    LLSD reply;
    if (result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
    {
        reply = json_to_llsd(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary());
    }
    // [2026-09-10] 409 = the service holds a newer version than the one this viewer fetched
    // (a save from WolfStorm or another editor since; Jimmy Olsen at WolfFest: "cant save;
    // change settings from wave ... Conflict", even as a god). Fetch the current record and
    // put THIS edit on top of it, once — the same rule as wave_zones.js save(). The layout the
    // user painted is kept; only the version number is refreshed.
    if (!retried && status.isHttpStatus() && status.getType() == 409)
    {
        LL_INFOS("WolfWaveZones") << "save: version conflict, refetching and retrying once" << LL_ENDL;
        // Source: php/waves.php:162-167 accepts an exact UUID read. A conflict reload must
        // not publish current-region state or substitute the destination after movement.
        LLSD reload = adapter->getRawAndSuspend(request, std::string(API_URL) + "?v=2&region=" + target.mUuid, options, headers);
        const LLCore::HttpStatus reload_status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(
            reload[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
        LLSD fresh;
        if (reload.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
            fresh = json_to_llsd(reload[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary());
        if (reload_status && fresh["success"].asBoolean() && fresh["regions"].isArray()
            && fresh["regions"].size() == 1)
        {
            const LLSD& row = fresh["regions"][0];
            if (row["region"].asString() == target.mUuid
                && std::strtoull(row["handle"].asString().c_str(), nullptr, 10) == target.mHandle)
            {
                target.mVersion = row["version"].asInteger();
                saveCoro(target, tiles, params, enabled, preview_revision, true);
                return;
            }
        }
    }
    mSaving = false;
    if (!status || !reply.isMap() || !reply["success"].asBoolean())
    {
        std::string msg = reply.isMap() && reply.has("error") ? reply["error"].asString()
                                                              : (status ? std::string("unexpected reply") : status.toString());
        if (reply.isMap() && reply.has("refusedCount"))
        {
            msg += " (" + std::to_string(reply["refusedCount"].asInteger()) + " cells refused)";
        }
        mLastError = msg;
        mLastSaveError = msg;
        notify("Could not save the wave layout: " + msg);
        return;
    }
    const LLSD& r = reply["region"];
    if (r["region"].asString() != target.mUuid
        || std::strtoull(r["handle"].asString().c_str(), nullptr, 10) != target.mHandle)
    {
        mLastSaveError = "The grid returned a different region for the wave save.";
        notify(mLastSaveError);
        return;
    }
    mLastError.clear();
    mLastSaveError.clear();
    mLastSavedVersion = r["version"].asInteger();
    auto it = mByHandle.find(target.mHandle);
    if (it != mByHandle.end() && it->second.mVersion <= mLastSavedVersion)
    {
        Region& rec = it->second;
        rec.mVersion = r["version"].asInteger();
        rec.mEnabled = r["enabled"].asBoolean();
        if (r["layout"].isMap())
        {
            rec.mStored = true;
            rec.mTiles = parseTiles(r["layout"], rec.pw(), rec.ph());
            rec.mParams = r["layout"]["params"];
        }
    }
    // Source: browser WaveZones.save previewRevision guard. Newer edits, hiding, Revert,
    // and crossings all change this revision; a late result cannot remove their previews.
    if (mPreviewRevision == preview_revision)
    {
        mPreview.erase(target.mHandle);
        if (mPreviewParamsFor == target.mHandle) { mPreviewParams = LLSD(); mPreviewParamsFor = 0; }
    }
    WolfWaterField::instance().invalidate();
    notify("Wave layout saved for " + r["name"].asString() + " — everyone in the region now sees it.");
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// WolfWavePainter
// ═══════════════════════════════════════════════════════════════════════════════════════

WolfWavePainter::WolfWavePainter(const Params& p) : LLUICtrl(p) {}

void WolfWavePainter::setLayout(const WolfWaveZones::Source& src, const WolfWaveZones::Tiles& tiles, bool all)
{
    mSrc = src;
    mTiles = tiles;
    mSrc.mTiles = &mTiles;
    mHasLayout = true;
    mDirty = false;
    // <WolfViewer 2026-09-26> The overlay: at most 512 texels an edge, each mZoneK x mZoneK cells.
    mZoneK = 1;
    while ((mSrc.mW + mZoneK - 1) / mZoneK > 512 || (mSrc.mH + mZoneK - 1) / mZoneK > 512) mZoneK *= 2;
    mZoneTW = (mSrc.mW + mZoneK - 1) / mZoneK;
    mZoneTH = (mSrc.mH + mZoneK - 1) / mZoneK;
    // Locked areas, sampled once per layout at each texel centre (the parcel overlay does not
    // change under us). Shading only: the brush checks every cell it paints (cellEditable) and
    // the service checks again.
    mLockedTexels.assign((size_t)mZoneTW * mZoneTH, 0);
    if (!all)
    {
        LLViewerRegion* rgn = gAgent.getRegion();
        LLViewerParcelOverlay* overlay = rgn ? rgn->getParcelOverlay() : nullptr;
        const F32 texel_m = (F32)(mZoneK * WolfWaveZones::PAINT_CELL_M);
        for (S32 j = 0; j < mZoneTH; ++j)
            for (S32 i = 0; i < mZoneTW; ++i)
            {
                const LLVector3 pos(((F32)i + 0.5f) * texel_m, ((F32)j + 0.5f) * texel_m, 0.f);
                mLockedTexels[(size_t)j * mZoneTW + i] = (!overlay || !overlay->isOwnedSelf(pos)) ? 1 : 0;
            }
    }
    mZonesStale = true;
}

void WolfWavePainter::clearLayout()
{
    mHasLayout = false;
    mTiles.clear();
    mSrc = WolfWaveZones::Source();
    mLockedTexels.clear();
    mDirty = false;
    mZonesStale = true;
}

// Fractional: a 16-cell region in a 224 px box gets 14 px cells; Ireland's 28,800 cells are
// far below a pixel each — the same box, scaled to fit (Paul: "on massive regions scale the
// drawing down"). At that size the box is an overview and the in-world brush is the tool.
F32 WolfWavePainter::cellPx() const
{
    return llmin((F32)getRect().getWidth() / (F32)mSrc.mW, (F32)getRect().getHeight() / (F32)mSrc.mH);
}

// Local y is bottom-up: row 0 (the region's south edge) is at the bottom. Region metres out.
bool WolfWavePainter::pointAt(S32 x, S32 y, F32& mx, F32& my) const
{
    const F32 px = cellPx();
    if (!mHasLayout || px <= 0.f) return false;
    mx = (F32)x / px * (F32)WolfWaveZones::PAINT_CELL_M;
    my = (F32)y / px * (F32)WolfWaveZones::PAINT_CELL_M;
    return mx >= 0.f && my >= 0.f && mx < (F32)(mSrc.mW * WolfWaveZones::PAINT_CELL_M) && my < (F32)(mSrc.mH * WolfWaveZones::PAINT_CELL_M);
}

// The region's terrain under the grid (Paul: "the terrain drawn on it so the user can see
// where they are drawing"): water blue by depth, land sand -> grass -> earth -> rock -> snow by
// height above the water, hill-shaded from the north-west. Heights are the drawn surface,
// LLSurface::resolveHeightRegion (0 where a patch has not arrived yet, which reads as deep
// water); rebuilt every 5 s while the tab is up, since terraforming changes them. Same ramp as
// wolfstorm land_waves_tab.js terrainUnderlay(). Row 0 of the raw image is the region's south
// edge, which gl_draw_scaled_image's default uv rect puts at the bottom.
void WolfWavePainter::refreshTerrain()
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return;
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (mTerrainTex.notNull() && mTerrainHandle == rgn->getHandle() && now - mTerrainBuiltAt < 5.0) return;
    const S32 N = llclamp(mSrc.mW * 4, 64, 512);
    if (mTerrainRaw.isNull() || mTerrainN != N)
    {
        mTerrainRaw = new LLImageRaw((U16)N, (U16)N, 3);
        mTerrainN = N;
        mTerrainTex = nullptr;
    }
    const F32 width = rgn->getWidth();
    const F32 wh = rgn->getWaterHeight();
    const LLSurface& land = rgn->getLand();
    std::vector<F32> hgt((size_t)N * N);
    for (S32 j = 0; j < N; ++j)
    {
        const F32 my = ((F32)j + 0.5f) / (F32)N * width;
        for (S32 i = 0; i < N; ++i)
        {
            const F32 mx = ((F32)i + 0.5f) / (F32)N * width;
            hgt[(size_t)j * N + i] = land.resolveHeightRegion(mx, my);
        }
    }
    U8* d = mTerrainRaw->getData();
    const F32 m_per_px = width / (F32)N;
    auto lerp3 = [](const F32* a, const F32* b, F32 t, F32* o) { for (S32 c = 0; c < 3; ++c) o[c] = a[c] + (b[c] - a[c]) * t; };
    static const F32 SHALLOW[3] = { 70, 150, 195 }, DEEP[3] = { 12, 34, 88 };
    static const F32 SAND[3] = { 196, 180, 124 }, GRASS[3] = { 92, 142, 72 }, EARTH[3] = { 122, 100, 70 };
    static const F32 ROCK[3] = { 142, 142, 146 }, SNOW[3] = { 232, 232, 238 };
    for (S32 j = 0; j < N; ++j)
    {
        for (S32 i = 0; i < N; ++i)
        {
            const size_t k = (size_t)j * N + i;
            const F32 h = hgt[k];
            F32 rgb[3];
            if (h < wh)
            {
                lerp3(SHALLOW, DEEP, llclamp((wh - h) / 12.f, 0.f, 1.f), rgb);
            }
            else
            {
                const F32 a = h - wh;
                if (a < 2.f)       lerp3(SAND, GRASS, a / 2.f, rgb);
                else if (a < 30.f) lerp3(GRASS, EARTH, (a - 2.f) / 28.f, rgb);
                else if (a < 80.f) lerp3(EARTH, ROCK, (a - 30.f) / 50.f, rgb);
                else               lerp3(ROCK, SNOW, llclamp((a - 80.f) / 60.f, 0.f, 1.f), rgb);
                const F32 he = hgt[(size_t)j * N + llmin(i + 1, N - 1)], hw = hgt[(size_t)j * N + llmax(i - 1, 0)];
                const F32 hn = hgt[(size_t)llmin(j + 1, N - 1) * N + i], hs = hgt[(size_t)llmax(j - 1, 0) * N + i];
                const F32 gx = (he - hw) / (2.f * m_per_px), gy = (hn - hs) / (2.f * m_per_px);
                const F32 shade = llclamp(0.82f + 0.35f * (-gx * 0.7f + gy * 0.7f) / sqrtf(1.f + gx * gx + gy * gy) + 0.18f, 0.f, 1.f);
                for (F32& c : rgb) c *= shade;
            }
            d[k * 3] = (U8)llclamp((S32)rgb[0], 0, 255);
            d[k * 3 + 1] = (U8)llclamp((S32)rgb[1], 0, 255);
            d[k * 3 + 2] = (U8)llclamp((S32)rgb[2], 0, 255);
        }
    }
    // Source: llnetmap.cpp createObjectImage / setSubImage — a local texture from a raw image,
    // re-uploaded in place on later rebuilds.
    if (mTerrainTex.isNull())
    {
        mTerrainTex = LLViewerTextureManager::getLocalTexture(mTerrainRaw.get(), false);
    }
    else
    {
        mTerrainTex->setSubImage(mTerrainRaw, 0, 0, N, N);
    }
    mTerrainHandle = rgn->getHandle();
    mTerrainBuiltAt = now;
}

// <WolfViewer 2026-09-26> The zones as one texture: each texel is the zone of the 16 m cell at
// its centre (painted, else automatic), tinted as the old per-cell squares were; when a texel
// holds several cells every painted cell is then put on its texel, so a one-dab stroke on a
// huge region still shows. Rebuilt at most ten times a second while painting. Source:
// land_waves_tab.js draw() (the same tints) and refreshTerrain() here for the texture.
// <WolfViewer 2026-09-26> Paul: the painter showed a grey square on Ireland ("it should be the
// whole island's terrain"). A region wider than 12 x 256 m has its terrain streamed by view
// distance only, with no whole-region sweep (OpenSimWolf TerrainModule.cs AddRegion,
// TerrainData.UsesPagedStorage: width > 12 * Constants.RegionSize), so the viewer never holds
// the whole island's heights. The world map does: draw its tiles instead, at the farthest level
// that covers the region in at most 16 tiles an edge (Ireland, 1800 regions: level 8, 128
// regions a tile, 15 x 15). Tiles are placed and cropped as LLWorldMapView::drawMipmapLevel
// does (SW corner at grid * REGION_WIDTH_METERS, v = 0 at the south edge).
bool WolfWavePainter::drawMapUnderlay(S32 gw, S32 gh)
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return false;
    const F64 width = (F64)rgn->getWidth();
    if (width <= 12.0 * REGION_WIDTH_METERS) return false;

    const S32 regions = (S32)ceil(width / REGION_WIDTH_METERS);
    S32 level = 1;
    while (level < LLWorldMipmap::MAP_LEVELS && (regions + (1 << (level - 1)) - 1) / (1 << (level - 1)) > 16) ++level;
    const F64 tile_m = (F64)REGION_WIDTH_METERS * (F64)(1 << (level - 1));

    const LLVector3d origin = rgn->getOriginGlobal();
    const F64 x0 = origin.mdV[VX], y0 = origin.mdV[VY];
    const F64 x1 = x0 + width, y1 = y0 + width;
    const F64 sx = (F64)gw / width, sy = (F64)gh / width;

    LLWorldMap* world_map = LLWorldMap::getInstance();
    LLGLSUIDefault gls_ui;
    gGL.color4f(1.f, 1.f, 1.f, 1.f);
    for (F64 ty = floor(y0 / tile_m) * tile_m; ty < y1; ty += tile_m)
    {
        for (F64 tx = floor(x0 / tile_m) * tile_m; tx < x1; tx += tile_m)
        {
            U32 grid_x, grid_y;
            LLWorldMipmap::globalToMipmap(tx, ty, level, &grid_x, &grid_y);
            LLPointer<LLViewerFetchedTexture> tile = world_map->getObjectsTile(grid_x, grid_y, level, true);
            if (tile.isNull() || !tile->hasGLTexture()) continue;
            // The part of this tile inside the region, in metres, then in the tile's 0..1 and the box's pixels.
            const F64 gx0 = (F64)grid_x * REGION_WIDTH_METERS, gy0 = (F64)grid_y * REGION_WIDTH_METERS;
            const F64 cx0 = llmax(gx0, x0), cx1 = llmin(gx0 + tile_m, x1);
            const F64 cy0 = llmax(gy0, y0), cy1 = llmin(gy0 + tile_m, y1);
            if (cx1 <= cx0 || cy1 <= cy0) continue;
            const F32 u0 = (F32)((cx0 - gx0) / tile_m), u1 = (F32)((cx1 - gx0) / tile_m);
            const F32 v0 = (F32)((cy0 - gy0) / tile_m), v1 = (F32)((cy1 - gy0) / tile_m);
            const F32 left = (F32)((cx0 - x0) * sx), right = (F32)((cx1 - x0) * sx);
            const F32 bottom = (F32)((cy0 - y0) * sy), top = (F32)((cy1 - y0) * sy);
            gGL.getTexUnit(0)->bind(tile.get());
            tile->setAddressMode(LLTexUnit::TAM_CLAMP);
            gGL.begin(LLRender::TRIANGLES);
            gGL.texCoord2f(u0, v1); gGL.vertex3f(left, top, 0.f);
            gGL.texCoord2f(u0, v0); gGL.vertex3f(left, bottom, 0.f);
            gGL.texCoord2f(u1, v0); gGL.vertex3f(right, bottom, 0.f);
            gGL.texCoord2f(u0, v1); gGL.vertex3f(left, top, 0.f);
            gGL.texCoord2f(u1, v0); gGL.vertex3f(right, bottom, 0.f);
            gGL.texCoord2f(u1, v1); gGL.vertex3f(right, top, 0.f);
            gGL.end();
        }
    }
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    return true;
}

void WolfWavePainter::refreshZones()
{
    if (!mHasLayout || mZoneTW <= 0 || mZoneTH <= 0) return;
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (!mZonesStale || (mZoneTex.notNull() && now - mZonesBuiltAt < 0.1)) return;
    if (mZoneRaw.isNull() || mZoneRaw->getWidth() != mZoneTW || mZoneRaw->getHeight() != mZoneTH)
    {
        mZoneRaw = new LLImageRaw((U16)mZoneTW, (U16)mZoneTH, 4);
        mZoneTex = nullptr;
    }
    U8* d = mZoneRaw->getData();
    auto put = [&](S32 i, S32 j, char z)
    {
        // Source: land_waves_tab.js TINTS; OFF has none, the terrain shows through.
        F32 r = 0.f, g = 0.f, b = 0.f, a = 0.f;
        switch (z)
        {
            case 's': r = 0.31f; g = 0.64f; b = 1.0f;  a = 0.72f; break;
            case 'o': r = 0.17f; g = 0.44f; b = 0.71f; a = 0.66f; break;
            case 'm': r = 0.51f; g = 0.77f; b = 0.93f; a = 0.58f; break;
            case 'c': r = 0.44f; g = 0.76f; b = 0.64f; a = 0.55f; break;
            default: break;
        }
        const size_t k = (size_t)j * mZoneTW + i;
        if (mLockedTexels[k])   // black at 0.45 over the tint
        {
            const F32 out = a + 0.45f * (1.f - a);
            const F32 f = out > 0.f ? a * 0.55f / out : 0.f;
            r *= f; g *= f; b *= f; a = out;
        }
        d[k * 4]     = (U8)ll_round(r * 255.f);
        d[k * 4 + 1] = (U8)ll_round(g * 255.f);
        d[k * 4 + 2] = (U8)ll_round(b * 255.f);
        d[k * 4 + 3] = (U8)ll_round(a * 255.f);
    };
    for (S32 j = 0; j < mZoneTH; ++j)
        for (S32 i = 0; i < mZoneTW; ++i)
            put(i, j, mSrc.at(llmin(mSrc.mW - 1, i * mZoneK + mZoneK / 2), llmin(mSrc.mH - 1, j * mZoneK + mZoneK / 2)));
    if (mZoneK > 1)
    {
        for (const auto& kv : mTiles)
        {
            const S32 tx = (S32)(kv.first % 65536u), ty = (S32)(kv.first / 65536u);
            for (S32 n = 0; n < WolfWaveZones::TILE_LEN && n < (S32)kv.second.size(); ++n)
            {
                const char z = kv.second[n];
                if (z == '.') continue;
                const S32 fx = tx * WolfWaveZones::TILE_CELLS + (n & 15), fy = ty * WolfWaveZones::TILE_CELLS + (n >> 4);
                if (fx < mSrc.mW && fy < mSrc.mH) put(fx / mZoneK, fy / mZoneK, z);
            }
        }
    }
    // Source: llnetmap.cpp createObjectImage / setSubImage, as refreshTerrain(). Point filtering
    // keeps a cell a crisp square when the box scales the texture up.
    if (mZoneTex.isNull())
    {
        mZoneTex = LLViewerTextureManager::getLocalTexture(mZoneRaw.get(), false);
        mZoneTex->setFilteringOption(LLTexUnit::TFO_POINT);
    }
    else
    {
        mZoneTex->setSubImage(mZoneRaw, 0, 0, mZoneTW, mZoneTH);
    }
    mZonesStale = false;
    mZonesBuiltAt = now;
}

void WolfWavePainter::draw()
{
    const F32 px = cellPx();
    const S32 gw = ll_round(mSrc.mW * px), gh = ll_round(mSrc.mH * px);
    gl_rect_2d(0, getRect().getHeight(), getRect().getWidth(), 0, LLColor4(0.04f, 0.06f, 0.12f, 1.f));
    if (!drawMapUnderlay(gw, gh))   // <WolfViewer 2026-09-26/>
    {
        refreshTerrain();
        if (mTerrainTex.notNull())
        {
            gl_draw_scaled_image(0, 0, gw, gh, mTerrainTex);
        }
    }
    if (mHasLayout)
    {
        refreshZones();
        if (mZoneTex.notNull())
        {
            // The last texel column / row can hold fewer than mZoneK cells: scale by texels, not cells.
            gl_draw_scaled_image(0, 0, ll_round(mZoneTW * mZoneK * px), ll_round(mZoneTH * mZoneK * px), mZoneTex);
        }
        if (px >= 8.f)
        {
            const LLColor4 line(1.f, 1.f, 1.f, 0.08f);
            for (S32 x = 0; x <= mSrc.mW; ++x) gl_line_2d(ll_round(x * px), 0, ll_round(x * px), gh, line);
            for (S32 y = 0; y <= mSrc.mH; ++y) gl_line_2d(0, ll_round(y * px), gw, ll_round(y * px), line);
        }
    }
    LLUICtrl::draw();
}

bool WolfWavePainter::paintCell(S32 cx, S32 cy)
{
    if (!getEnabled() || !mHasLayout || cx < 0 || cy < 0 || cx >= mSrc.mW || cy >= mSrc.mH) return false;
    if (!WolfWaveZones::setTileCell(mTiles, cx, cy, mBrush)) return false;
    mDirty = true;
    mZonesStale = true;
    return true;
}

void WolfWavePainter::stroke(S32 x, S32 y)
{
    F32 mx, my;
    if (!pointAt(x, y, mx, my)) { mPrevious = false; return; }
    if (mOnStroke) mOnStroke(mPrevious ? mLastX : mx, mPrevious ? mLastY : my, mx, my);
    mLastX = mx; mLastY = my; mPrevious = true;
}

bool WolfWavePainter::handleMouseDown(S32 x, S32 y, MASK mask)
{
    if (!getEnabled()) return LLUICtrl::handleMouseDown(x, y, mask);
    mPainting = true;
    mPrevious = false;
    gFocusMgr.setMouseCapture(this);
    stroke(x, y);
    return true;
}

bool WolfWavePainter::handleHover(S32 x, S32 y, MASK mask)
{
    if (mPainting && hasMouseCapture())
    {
        stroke(x, y);
        return true;
    }
    return LLUICtrl::handleHover(x, y, mask);
}

bool WolfWavePainter::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (hasMouseCapture())
    {
        mPainting = false;
        mPrevious = false;
        gFocusMgr.setMouseCapture(NULL);
        return true;
    }
    return LLUICtrl::handleMouseUp(x, y, mask);
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// WolfPanelLandWaves — Region / Estate > Waves
// ═══════════════════════════════════════════════════════════════════════════════════════

// Source: WolfToolTerrainPaint's capture/render lifecycle and LLToolPipette's transient tool.
// This tool owns no draft: both the map and the world edit the panel's versioned working copy.
class WolfToolWavePaint : public LLTool
{
public:
    explicit WolfToolWavePaint(WolfPanelLandWaves& panel) : LLTool("WavePaint"), mPanel(panel) {}
    bool active() const { return mActive; }
    bool isAlwaysRendered() override { return true; }
    void handleSelect() override
    {
        mActive = true;
        if (mPanel.mWorldPaint) mPanel.mWorldPaint->setToggleState(true);
    }
    void handleDeselect() override
    {
        mActive = false;
        mHover = false;
        mPrevious = false;
        if (hasMouseCapture()) setMouseCapture(false);
        if (mPanel.mWorldPaint) mPanel.mWorldPaint->setToggleState(false);
    }
    void onMouseCaptureLost() override { mPrevious = false; }
    bool handleKey(KEY key, MASK mask) override
    {
        if (key != KEY_ESCAPE) return LLTool::handleKey(key, mask);
        mPanel.stopWorldBrush();
        mPanel.setStatus("Water brush stopped. Save to keep your changes.", false);
        return true;
    }
    bool handleMouseDown(S32 x, S32 y, MASK mask) override
    {
        mPrevious = false;
        setMouseCapture(true);
        dab(x, y);
        return true;
    }
    bool handleMouseUp(S32 x, S32 y, MASK mask) override
    {
        if (hasMouseCapture()) { dab(x, y); setMouseCapture(false); }
        mPrevious = false;
        return true;
    }
    bool handleDoubleClick(S32 x, S32 y, MASK mask) override { return handleMouseDown(x, y, mask); }
    bool handleRightMouseDown(S32 x, S32 y, MASK mask) override
    {
        mPanel.stopWorldBrush();
        mPanel.setStatus("Water brush stopped. Save to keep your changes.", false);
        return true;
    }
    bool handleHover(S32 x, S32 y, MASK mask) override
    {
        mMouseX = x; mMouseY = y;
        mHover = true;
        gViewerWindow->setCursor(UI_CURSOR_TOOLLAND);
        if (hasMouseCapture()) dab(x, y);
        return true;
    }
    void render() override
    {
        if (!mActive || !mHover) return;
        mHover = false;
        LLVector3 hit;
        if (!waterHit(mMouseX, mMouseY, hit)) return;
        const auto* row = WolfWaveZones::instance().current();
        LLViewerRegion* region = gAgent.getRegion();
        if (!row || !region) return;
        // <WolfViewer 2026-09-26> The 16 m paint cell on every region: the same ring everywhere.
        const F32 radius = mPanel.mBrushDiameter->getValueF32() * (F32)WolfWaveZones::PAINT_CELL_M * 0.5f;
        // Source: WolfToolTerrainPaint::render, 48-segment brush ring. Flush before restoring
        // the shader: the About Land crash was geometry flushed after its shader was unbound.
        LLGLSLShader* previous = LLGLSLShader::sCurBoundShaderPtr;
        gDebugProgram.bind();
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        LLGLDepthTest depth(GL_FALSE, GL_FALSE);
        gGL.color4f(0.31f, 0.64f, 1.f, 1.f);
        gGL.begin(LLRender::LINES);
        for (S32 k = 0; k < 48; ++k)
            for (S32 end = 0; end < 2; ++end)
            {
                const F32 angle = (F32)(k + end) / 48.f * F_TWO_PI;
                const LLVector3 point = region->getPosAgentFromRegion(LLVector3(
                    hit.mV[VX] + radius * cosf(angle), hit.mV[VY] + radius * sinf(angle), hit.mV[VZ]));
                gGL.vertex3fv(point.mV);
            }
        gGL.end();
        gGL.flush();
        if (previous) previous->bind(); else LLGLSLShader::unbind();
    }
private:
    bool waterHit(S32 x, S32 y, LLVector3& hit) const
    {
        if (!mActive || gDisconnected || !mPanel.isInVisibleChain() || !mPanel.targetCurrent() ||
            !mPanel.mEnabled->get()) return false;
        LLViewerRegion* region = gAgent.getRegion();
        const auto* row = WolfWaveZones::instance().current();
        if (!region || !row) return false;
        // Source: LLViewerWindow::mousePointOnPlaneGlobal. Reject its parallel-ray fallback;
        // sky clicks must not produce an approximate point on the sea.
        if (fabsf(gViewerWindow->mouseDirectionGlobal(x, y).mV[VZ]) < 0.00001f) return false;
        LLVector3d point;
        if (!gViewerWindow->mousePointOnPlaneGlobal(point, x, y,
            region->getPosGlobalFromRegion(LLVector3(0.f, 0.f, region->getWaterHeight())), LLVector3::z_axis)) return false;
        hit = region->getPosRegionFromGlobal(point);
        if (!std::isfinite(hit.mV[VX]) || !std::isfinite(hit.mV[VY]) ||
            hit.mV[VX] < 0.f || hit.mV[VY] < 0.f || hit.mV[VX] >= row->mSizeX || hit.mV[VY] >= row->mSizeY ||
            region->getLand().resolveHeightRegion(hit.mV[VX], hit.mV[VY]) >= region->getWaterHeight()) return false;
        LLVector3d terrain;
        const LLVector3d camera = gAgentCamera.getCameraPositionGlobal();
        if (gViewerWindow->mousePointOnLandGlobal(x, y, &terrain) &&
            (terrain - camera).lengthSquared() < (point - camera).lengthSquared()) return false;
        return true;
    }
    void dab(S32 x, S32 y)
    {
        LLVector3 hit;
        if (!waterHit(x, y, hit))
        {
            mPrevious = false;
            mPanel.setStatus("Point at water in this region to paint.", true);
            return;
        }
        const F32 px = hit.mV[VX], py = hit.mV[VY];
        mPanel.paintStroke(mPrevious ? mLastX : px, mPrevious ? mLastY : py, px, py, true);
        mLastX = px; mLastY = py; mPrevious = true;
    }
    WolfPanelLandWaves& mPanel;
    bool mActive = false, mPrevious = false, mHover = false;
    S32 mMouseX = 0, mMouseY = 0;
    F32 mLastX = 0.f, mLastY = 0.f;
};

// Source: llfloaterregioninfo.cpp:244-321. Region panels are constructed directly and build a
// standalone XUI file; wave permissions still consult the occupied parcel at paint time.
WolfPanelLandWaves::WolfPanelLandWaves() : LLPanel() {}

WolfPanelLandWaves::~WolfPanelLandWaves()
{
    stopWorldBrush();
    if (mRegionChangedConnection.connected()) mRegionChangedConnection.disconnect();
    WolfWaveZones::instance().clearPreview();
}

bool WolfPanelLandWaves::targetCurrent() const
{
    const LLViewerRegion* region = gAgent.getRegion();
    const WolfWaveZones::Region* row = WolfWaveZones::instance().current();
    return region && row && mTarget.mHandle == region->getHandle() && mTarget.mUuid == row->mUuid;
}

void WolfPanelLandWaves::invalidateTarget()
{
    stopWorldBrush();
    const bool discarded = mDirty;
    WolfWaveZones::instance().clearPreview();
    mTarget = WolfWaveZones::SaveTarget();
    mDirty = false;
    ++mEditRevision;
    mWasSaving = false;
    mShownHandle = 0;
    mShownVersion = -1;
    mNextPoll = 0.0;
    mBakeWaitUntil = 0.0;
    if (mPainter) { mPainter->clearDirty(); mPainter->setEnabled(false); }
    if (mSave) mSave->setEnabled(false);
    if (discarded) setStatus("You moved to another region. The unsaved wave preview was discarded.", true);
}

void WolfPanelLandWaves::onVisibilityChange(bool visible)
{
    // Source: LLView::onVisibilityChange propagates ancestor visibility separately from the
    // child's own visible bit. Hiding the owner must end its world preview immediately.
    if (!visible) { stopWorldBrush(); WolfWaveZones::instance().clearPreview(); mBakeWaitUntil = 0.0; }
    else if (mPainter)
    {
        if (!targetCurrent()) { invalidateTarget(); refresh(); }
        else if (mDirty) previewEdit();
    }
    LLPanel::onVisibilityChange(visible);
}

bool WolfPanelLandWaves::postBuild()
{
    // Source: LLAgent::setRegion region callback: invalidate even across A->B->A while hidden.
    mRegionChangedConnection = gAgent.addRegionChangedCallback([this]() { invalidateTarget(); });
    mPainter     = getChild<WolfWavePainter>("waves_painter");
    mWorldPaint  = getChild<LLButton>("waves_world_paint");
    mBrushDiameter = getChild<LLSliderCtrl>("waves_brush_diameter");
    mWorldTool = new WolfToolWavePaint(*this);
    mWorldPaint->setCommitCallback(boost::bind(&WolfPanelLandWaves::toggleWorldBrush, this));
    mStatus      = getChild<LLTextBox>("waves_status");
    mNote        = getChild<LLTextBox>("waves_note");
    mSurfHeight  = getChild<LLSliderCtrl>("waves_surf_height");
    mSetInterval = getChild<LLSliderCtrl>("waves_set_interval");
    mCalmRipple  = getChild<LLSliderCtrl>("waves_calm_ripple");
    mSmallScale  = getChild<LLSliderCtrl>("waves_small_scale");
    mEnabled     = getChild<LLCheckBoxCtrl>("waves_enabled");
    mSave        = getChild<LLButton>("waves_save");
    mBrushS      = getChild<LLButton>("waves_brush_s");
    mBrushO      = getChild<LLButton>("waves_brush_o");
    mBrushM      = getChild<LLButton>("waves_brush_m");
    mBrushC      = getChild<LLButton>("waves_brush_c");
    mBrushX      = getChild<LLButton>("waves_brush_x");

    mBrushS->setCommitCallback(boost::bind(&WolfPanelLandWaves::onBrush, this, 's'));
    mBrushO->setCommitCallback(boost::bind(&WolfPanelLandWaves::onBrush, this, 'o'));
    mBrushM->setCommitCallback(boost::bind(&WolfPanelLandWaves::onBrush, this, 'm'));
    mBrushC->setCommitCallback(boost::bind(&WolfPanelLandWaves::onBrush, this, 'c'));
    mBrushX->setCommitCallback(boost::bind(&WolfPanelLandWaves::onBrush, this, 'x'));
    mSave->setCommitCallback(boost::bind(&WolfPanelLandWaves::onSave, this));
    getChild<LLButton>("waves_revert")->setCommitCallback(boost::bind(&WolfPanelLandWaves::onRevert, this));
    getChild<LLButton>("waves_default")->setCommitCallback(boost::bind(&WolfPanelLandWaves::onDefault, this));
    mSurfHeight->setCommitCallback(boost::bind(&WolfPanelLandWaves::onParamChanged, this));
    mSetInterval->setCommitCallback(boost::bind(&WolfPanelLandWaves::onParamChanged, this));
    mCalmRipple->setCommitCallback(boost::bind(&WolfPanelLandWaves::onParamChanged, this));
    mSmallScale->setCommitCallback(boost::bind(&WolfPanelLandWaves::onParamChanged, this));
    mEnabled->setCommitCallback(boost::bind(&WolfPanelLandWaves::onParamChanged, this));

    // <WolfViewer 2026-09-26> The map paints with the same brush (diameter in 16 m cells) as the
    // water; it may paint land cells as before, the in-world brush only water.
    mPainter->setStrokeCallback([this](F32 ax, F32 ay, F32 bx, F32 by) { paintStroke(ax, ay, bx, by, false); });
    onBrush('s');
    return true;
}

void WolfPanelLandWaves::toggleWorldBrush()
{
    if (mWorldTool->active()) { stopWorldBrush(); setStatus("Water brush stopped. Save to keep your changes.", false); return; }
    if (gDisconnected || !WolfGrid::isWolfTerritories() || !targetCurrent())
    {
        mWorldPaint->setToggleState(false);
        setStatus("The region's wave layout is not ready. Wait for it to load before painting.", true);
        return;
    }
    if (!ensureEnabled())   // <WolfViewer 2026-09-20/> ticks the box for a region holder, explains to a parcel owner
    {
        mWorldPaint->setToggleState(false);
        return;
    }
    // Source: LLViewerWindow::renderSelections uses a HUD projection while HUDs are
    // selected. Water painting starts in world space and has no object selection.
    LLSelectMgr::getInstance()->deselectAll();
    LLToolMgr::getInstance()->setTransientTool(mWorldTool);
    mWorldPaint->setToggleState(true);
    setStatus("Drag on water to paint. Press Esc or Paint on water to stop; Save to keep changes.", false);
}

void WolfPanelLandWaves::stopWorldBrush()
{
    if (!mWorldTool) return;
    if (mWorldTool->hasMouseCapture()) mWorldTool->setMouseCapture(false);
    if (LLToolMgr::instanceExists() && LLToolMgr::getInstance()->getCurrentTool() == mWorldTool.get())
        LLToolMgr::getInstance()->clearTransientTool();
    mWorldTool->handleDeselect();
}

bool WolfPanelLandWaves::paintStroke(F32 ax, F32 ay, F32 bx, F32 by, bool waterOnly)
{
    if (gDisconnected || !targetCurrent() || !isInVisibleChain() || !mPainter || !mPainter->hasLayout()) return false;
    if (!ensureEnabled()) return false;   // <WolfViewer 2026-09-20/> ticks the box for a region holder, explains to a parcel owner
    WolfWaveZones& wz = WolfWaveZones::instance();
    LLViewerRegion* region = gAgent.getRegion();
    bool changed = false, refused = false;
    WolfWaveBrush::visit(mPainter->cellsW(), mPainter->cellsH(), WolfWaveZones::PAINT_CELL_M, (S32)mBrushDiameter->getValueF32(),
        ax, ay, bx, by,
        [&](S32 cx, S32 cy)
        {
            if (!wz.cellEditable(cx, cy)) { refused = true; return; }
            // <WolfViewer 2026-09-18/> five points, not the centre
            if (waterOnly && !WolfWaveZones::cellHasWater(region, cx, cy, WolfWaveZones::PAINT_CELL_M)) return;
            changed = mPainter->paintCell(cx, cy) || changed;
        });
    if (changed) onParamChanged();
    if (refused) setStatus(getString("str_locked_cell"), true);
    return changed;
}

void WolfPanelLandWaves::onBrush(char z)
{
    if (!WolfGrid::isWolfTerritories()) { setStatus("Sorry, this function is only available on Wolf Territories Grid.", true); return; }
    if (mPainter) mPainter->setBrush(z);
    mBrushS->setToggleState(z == 's');
    mBrushO->setToggleState(z == 'o');
    mBrushM->setToggleState(z == 'm');
    mBrushC->setToggleState(z == 'c');
    mBrushX->setToggleState(z == 'x');
}

void WolfPanelLandWaves::setStatus(const std::string& msg, bool error)
{
    mStatusError = error;
    if (!mStatus) return;
    mStatus->setText(msg);
    mStatus->setColor(error ? LLColor4(1.f, 0.54f, 0.54f, 1.f) : LLColor4(0.75f, 0.78f, 0.85f, 1.f));
}

void WolfPanelLandWaves::armBakeConfirm()
{
    // Source: wolfstorm land_waves_tab.js _wavesPreview — the tab confirms the water took the
    // preview, so "nothing changed" is a fact rather than a guess.
    mBakeMark = WolfWaterField::instance().bakeCount();
    mBakeWaitUntil = LLFrameTimer::getElapsedSeconds() + 3.0;
}

void WolfPanelLandWaves::draw()
{
    if (gDisconnected) stopWorldBrush();
    if (!WolfGrid::isWolfTerritories()) { refresh(); LLPanel::draw(); return; }
    if (mTarget.mHandle && !targetCurrent()) { invalidateTarget(); refresh(); }
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (mBakeWaitUntil > 0.0 && !mStatusError && !mWasSaving)
    {
        WolfWaterField& wf = WolfWaterField::instance();
        if (wf.bakeCount() > mBakeMark)
        {
            mBakeWaitUntil = 0.0;
            setStatus("Applied to the water: " + std::to_string(wf.lastSurfTexels()) + " surf cells in the baked span.", false);
        }
        else if (now > mBakeWaitUntil)
        {
            mBakeWaitUntil = 0.0;
            setStatus("The water has not rebaked yet.", true);
        }
    }
    // Twice a second: a fetch that landed, a save that finished, a region change.
    if (now >= mNextPoll)
    {
        mNextPoll = now + 0.5;
        WolfWaveZones& wz = WolfWaveZones::instance();
        LLViewerRegion* rgn = gAgent.getRegion();
        const U64 handle = rgn ? rgn->getHandle() : 0;
        const WolfWaveZones::Region* r = wz.current();
        const bool fetching = wz.fetching();
        if ((mWasSaving && !wz.saving()) || (r && (handle != mShownHandle || r->mVersion != mShownVersion))
            || (!r && mShownHandle != 0) || fetching != mShownFetching)
        {
            mShownFetching = fetching;
            refresh();
        }
    }
    LLPanel::draw();
}

void WolfPanelLandWaves::refresh()
{
    if (mTarget.mHandle && !targetCurrent()) invalidateTarget();
    if (!WolfGrid::isWolfTerritories())
    {
        setStatus("Sorry, this function is only available on Wolf Territories Grid.", true);
        if (mNote) mNote->setText(std::string("Sorry, this function is only available on Wolf Territories Grid."));
        if (mSave) mSave->setEnabled(false);
        if (mPainter) mPainter->setEnabled(false);
        return;
    }
    if (gDisconnected) return;
    WolfWaveZones& wz = WolfWaveZones::instance();
    const WolfWaveZones::Region* r = wz.current();
    LLViewerRegion* rgn = gAgent.getRegion();
    const U64 handle = rgn ? rgn->getHandle() : 0;
    // A save just finished: show its result and reload the layout.
    if (mWasSaving && !wz.saving())
    {
        mWasSaving = false;
        if (wz.lastSaveError().empty())
        {
            mTarget.mVersion = wz.lastSavedVersion();
            if (mEditRevision == mSubmittedRevision)
            {
                mDirty = false;
                if (mPainter) mPainter->clearDirty();
                mShownVersion = -1;
                setStatus(getString("str_saved"), false);
            }
            else setStatus("Saved the submitted layout. You have newer unsaved changes.", false);
        }
        else setStatus(getString("str_save_failed") + " " + wz.lastSaveError(), true);
    }
    if (mSave) mSave->setEnabled(r != nullptr && !wz.saving());
    if (!r)
    {
        if (mNote) mNote->setText(getString(wz.fetching() ? "str_fetching" : "str_off_grid"));
        if (mPainter) { mPainter->setEnabled(false); mPainter->clearLayout(); }
        mShownHandle = 0;
        mShownVersion = -1;
        // Never asked about this region (the floater opened before the handshake): ask once.
        // A region the grid does not know stays "automatic" — fetchedFor() already names it,
        // so the half-second poll in draw() cannot turn into a fetch loop.
        if (!wz.fetching() && wz.fetchedFor() != handle) wz.refresh();
        return;
    }
    // [2026-09-10] First look at this region in the tab: if the record is more than ten
    // seconds old, ask the grid again, so the version the Save carries is current (a save
    // from WolfStorm or another editor since the login would otherwise make the first save a
    // 409, which saveCoro now retries once anyway). The half-second poll rebuilds when the
    // fresh record lands.
    if (handle != mShownHandle && !wz.fetching() && wz.lastFetchAgeSecs() > 10.0)
    {
        wz.refresh();
    }
    // Only rebuild the working copy when the region or its stored version changed — and never
    // over an edit in progress: a newer version arriving while the user paints is noted, and
    // the Save puts the painted layout on top of it (saveCoro's 409 retry).
    const bool changed = handle != mShownHandle || r->mVersion != mShownVersion
        || (mPainter && !mPainter->dirty()
            && (!mPainter->hasLayout() || mPainter->cellsW() != r->pw() || mPainter->cellsH() != r->ph()));
    if (changed)
    {
        if (mDirty && handle == mShownHandle)
        {
            if (!mStatusError && !mWasSaving) setStatus(getString("str_newer_on_grid"), false);
            mShownVersion = r->mVersion;
            return;
        }
        rebuild();
        mShownHandle = handle;
        mShownVersion = r->mVersion;
    }
}

void WolfPanelLandWaves::rebuild()
{
    WolfWaveZones& wz = WolfWaveZones::instance();
    const WolfWaveZones::Region* r = wz.current();
    if (!r || !mPainter) return;
    mWriting = true;
    mTarget = { r->mUuid, r->mHandle, r->mVersion };
    mDirty = false;
    const bool all = wz.canEditAll();
    // <WolfViewer 2026-09-20/> stored cells even while switched off; 2026-09-26: 16 m tiles
    mPainter->setLayout(wz.sourceFor(*r), wz.editorTilesFor(*r), all);
    mPainter->setEnabled(true);
    const LLSD& p = r->mParams;
    // explicit F32: LLSliderCtrl::setValue(F32) and MSVC's C4244 is an error on the CI
    mSurfHeight->setValue((F32)(p.has("surfHeight") ? p["surfHeight"].asReal() : 3.0));
    mSetInterval->setValue((F32)(p.has("setInterval") ? p["setInterval"].asReal() : 90.0));
    mCalmRipple->setValue((F32)(p.has("calmRipple") ? p["calmRipple"].asReal() * 100.0 : 3.0));
    mSmallScale->setValue((F32)(p.has("smallScale") ? p["smallScale"].asReal() * 100.0 : WolfWaveZones::SMALL_SCALE_DEFAULT * 100.0));
    mEnabled->set(r->mEnabled);
    mSurfHeight->setEnabled(all);
    mSetInterval->setEnabled(all);
    mCalmRipple->setEnabled(all);
    mSmallScale->setEnabled(all);
    mEnabled->setEnabled(all);
    getChild<LLButton>("waves_default")->setEnabled(all);
    LLStringUtil::format_map_t args;
    args["[REGION]"] = r->mName;
    args["[W]"] = std::to_string(r->pw());
    args["[H]"] = std::to_string(r->ph());
    args["[CELL]"] = std::to_string(WolfWaveZones::PAINT_CELL_M);
    args["[STATE]"] = r->mStored ? getString("str_stored") : getString("str_automatic");
    mNote->setText(getString(all ? "str_note_all" : "str_note_parcel", args));
    mWriting = false;
}

LLSD WolfPanelLandWaves::paramsFromControls() const
{
    LLSD p;
    p["surfHeight"] = mSurfHeight->getValueF32();
    p["setInterval"] = (S32)mSetInterval->getValueF32();
    p["calmRipple"] = mCalmRipple->getValueF32() / 100.f;
    p["smallScale"] = mSmallScale->getValueF32() / 100.f;   // [2026-09-10] slider in percent of the open sea
    return p;
}

void WolfPanelLandWaves::onParamChanged()
{
    if (mWriting) return;
    if (!WolfGrid::isWolfTerritories()) { setStatus("Sorry, this function is only available on Wolf Territories Grid.", true); return; }
    if (!targetCurrent()) { invalidateTarget(); refresh(); return; }
    if (!mEnabled->get()) stopWorldBrush();
    // Live in the water until Save or Revert, like the brush (WolfStorm land_waves_tab.js).
    WolfWaveZones& wz = WolfWaveZones::instance();
    if (!wz.current() || !mPainter) return;
    mDirty = true;
    ++mEditRevision;
    setStatus("", false);
    previewEdit();
}

void WolfPanelLandWaves::previewEdit()
{
    if (!targetCurrent() || !isInVisibleChain()) return;
    WolfWaveZones& wz = WolfWaveZones::instance();
    wz.previewParams(paramsFromControls());
    // Off = the automatic layout (open sea full waves, enclosed water small waves): preview that, so the switch is visible at once.
    wz.preview(mPainter->tiles(), mEnabled->get());
    armBakeConfirm();
}

void WolfPanelLandWaves::onSave()
{
    if (!WolfGrid::isWolfTerritories()) { setStatus("Sorry, this function is only available on Wolf Territories Grid.", true); return; }
    if (!mPainter) return;
    if (!targetCurrent()) { invalidateTarget(); refresh(); setStatus("The region changed. Review this region's layout before saving.", true); return; }
    if (mWasSaving) return;
    WolfWaveZones& wz = WolfWaveZones::instance();
    mSubmittedRevision = mEditRevision;
    mWasSaving = wz.save(mTarget, mPainter->tiles(), paramsFromControls(), mEnabled->get());
    mSave->setEnabled(!mWasSaving);
    setStatus(mWasSaving ? getString("str_saving") : getString("str_save_failed") + " " + wz.lastSaveError(), !mWasSaving);
}

void WolfPanelLandWaves::onRevert()
{
    stopWorldBrush();
    WolfWaveZones::instance().clearPreview();
    ++mEditRevision;
    mDirty = false;
    mWasSaving = false;
    mBakeWaitUntil = 0.0;
    setStatus("", false);
    if (mPainter) mPainter->clearDirty();
    mShownVersion = -1;
    refresh();
}

// [2026-09-10] "Reset to automatic" switches the saved layout OFF (the "Use this layout" box)
// rather than copying the automatic cells into the painter: a copy saved as a layout froze the
// automatic rule of that moment for good (Paul's WT Atlantic 131 kept a flat first-cut default
// as its saved layout — "its flat and boring, i set the water back to automatic"). With the
// box off the grid stores enabled=0 and every viewer computes the automatic layout live.
// [2026-09-26] Reset also clears the painted cells (see onDefault); unticking the box keeps them.
// <WolfViewer 2026-09-20/> see wolfwavezones.h
bool WolfPanelLandWaves::ensureEnabled()
{
    if (!mEnabled) return false;
    if (mEnabled->get()) return true;
    if (!WolfWaveZones::instance().canEditAll())
    {
        setStatus(getString("str_layout_off_parcel"), true);
        return false;
    }
    mEnabled->set(true);
    mDirty = true;
    setStatus(getString("str_layout_on_for_paint"), false);
    return true;
}

void WolfPanelLandWaves::onDefault()
{
    if (!WolfGrid::isWolfTerritories()) { setStatus("Sorry, this function is only available on Wolf Territories Grid.", true); return; }
    if (!targetCurrent()) { invalidateTarget(); refresh(); return; }
    WolfWaveZones& wz = WolfWaveZones::instance();
    const WolfWaveZones::Region* r = wz.current();
    if (!r || !mPainter || !mEnabled) return;
    // <WolfViewer 2026-09-26> Paul: "reset to automatic should clear any customisations". The
    // kept cells came back with the next brush stroke (ensureEnabled ticks the box again), so
    // Reset empties the painter's tiles too and Save stores enabled=0 with none. Unticking
    // "Use this layout" is still the way to switch painting off and keep it.
    mPainter->setLayout(wz.sourceFor(*r), WolfWaveZones::Tiles(), wz.canEditAll());
    // </WolfViewer>
    mEnabled->set(false);
    onParamChanged();   // previews the automatic layout at once (enabled off -> defaultZones)
    setStatus(getString("str_default_loaded"), false);
}
