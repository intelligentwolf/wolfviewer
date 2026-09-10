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

#include "wolfwavezones.h"

#include <boost/json.hpp>

#include "llagent.h"
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
#include "llsliderctrl.h"
#include "lltextbox.h"
#include "lluictrlfactory.h"
#include "llparcel.h"            // PARCEL_GRID_STEP_METERS
#include "llimage.h"
#include "llsurface.h"
#include "llviewerparceloverlay.h"
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "llworld.h"
#include "wolfgrid.h"
#include "wolfwaterfield.h"

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
    U64 handle_of(S32 x, S32 y)
    {
        return ((U64)(U32)x << 32) | (U64)(U32)y;
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

S32 WolfWaveZones::texelM(LLViewerRegion* regionp, F32 span_m) const
{
    S32 cell = CELL_M;
    if (regionp)
    {
        auto it = mByHandle.find(regionp->getHandle());
        cell = it != mByHandle.end() ? it->second.mCell : cellFor((S32)regionp->getWidth(), (S32)regionp->getWidth());
    }
    while (span_m / (F32)cell > 768.f) cell *= 2;
    return cell;
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

WolfWaveZones::WolfWaveZones() {}
WolfWaveZones::~WolfWaveZones() {}

// ── fetch ──────────────────────────────────────────────────────────────────────────────

std::vector<U64> WolfWaveZones::neighbourHandles() const
{
    std::vector<U64> out;
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return out;
    S32 x, y;
    handle_xy(rgn->getHandle(), x, y);
    const S32 sx = (S32)rgn->getWidth(), sy = (S32)rgn->getWidth();
    // Source: wave_zones.js neighbourHandles — a var region's western or southern neighbour
    // sits its OWN size away, so both the 256 m step and this region's size are asked for.
    const S32 dxs[] = { 0, sx, -256, -sx, 256 };
    const S32 dys[] = { 0, sy, -256, -sy, 256 };
    for (S32 dx : dxs)
    {
        for (S32 dy : dys)
        {
            const S32 nx = x + dx, ny = y + dy;
            if (nx < 0 || ny < 0) continue;
            const U64 h = handle_of(nx, ny);
            if (std::find(out.begin(), out.end(), h) == out.end()) out.push_back(h);
            if (out.size() >= 24) return out;
        }
    }
    return out;
}

void WolfWaveZones::idle()
{
    // Other grids get the automatic layout: nothing is asked of the Wolf Territories service.
    if (!WolfGrid::isWolfTerritories()) return;
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return;
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (rgn->getHandle() != mFetchedForHandle || now >= mNextRefresh)
    {
        refresh();
    }
}

void WolfWaveZones::refresh()
{
    if (mFetching) return;
    std::vector<U64> handles = neighbourHandles();
    if (handles.empty()) return;
    mFetching = true;
    mNextRefresh = LLFrameTimer::getElapsedSeconds() + REFRESH_SECS;
    LLCoros::instance().launch("WolfWaveZones fetch", [handles]() { WolfWaveZones::instance().fetchCoro(handles); });
}

// Source: wolfspeech.cpp postRaw for the adapter shape; llcorehttputil.h getRawAndSuspend.
void WolfWaveZones::fetchCoro(std::vector<U64> handles)
{
    std::string url = std::string(API_URL) + "?handles=";
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

    LLSD result = adapter->getRawAndSuspend(request, url, options, headers);
    LLSD httpResults = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(httpResults);
    mFetching = false;
    // Whatever the answer, this region has been asked about: idle() retries on the
    // REFRESH_SECS clock, never every frame.
    {
        LLViewerRegion* rgn_now = gAgent.getRegion();
        mFetchedForHandle = rgn_now ? rgn_now->getHandle() : 0;
    }
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
            rec.mZones = r["layout"]["zones"].asString();
            rec.mParams = r["layout"]["params"];
        }
        if (rec.mHandle) mByHandle[rec.mHandle] = rec;
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

std::string WolfWaveZones::zonesFor(const Region& r) const
{
    auto pv = mPreview.find(r.mHandle);
    if (pv != mPreview.end())
    {
        if ((S32)pv->second.size() == r.w() * r.h()) return pv->second;
        LL_WARNS("WolfWaveZones") << "preview for " << r.mName << " has " << pv->second.size() << " cells, record wants "
                                  << r.w() * r.h() << " — ignored" << LL_ENDL;
    }
    if (r.mStored && r.mEnabled && (S32)r.mZones.size() == r.w() * r.h()) return r.mZones;
    return defaultZones(r);
}

// Source: wave_zones.js defaultZones — OPEN waves round the region (the outer SURF_BAND_M of
// cells where the exposure field says that water faces the open sea; a lake dug at the edge,
// with land between it and the void, reads as sheltered and stays flat) and every inner cell
// OFF. [2026-09-10, Paul] "NO waves inside the region only at the outside of it ... a lake in
// a region doesn't have waves unless they define it", then "put the waves back round the
// region automatically if there are no user defined settings". (Until 09-10 the whole interior
// was calm/open from the exposure field, which put waves on every dug lake — Jimmy Olsen's
// WolfFest screenshots.) The exposure field is the agent region's (it spans the neighbours),
// so a neighbour's cells are offset into it.
std::string WolfWaveZones::defaultZones(const Region& r) const
{
    const S32 w = r.w(), h = r.h();
    const F32 cell = (F32)r.mCell;
    const F32 band = llmax(SURF_BAND_M, cell);
    std::string out;
    out.reserve((size_t)w * h);
    LLViewerRegion* rgn = gAgent.getRegion();
    const WolfWaterField::Field* fld = rgn ? WolfWaterField::instance().get(rgn) : nullptr;
    F32 ox = 0.f, oy = 0.f;
    if (rgn && r.mHandle != rgn->getHandle())
    {
        S32 ax, ay, bx, by;
        handle_xy(r.mHandle, ax, ay);
        handle_xy(rgn->getHandle(), bx, by);
        ox = (F32)(ax - bx);
        oy = (F32)(ay - by);
    }
    for (S32 cy = 0; cy < h; ++cy)
    {
        for (S32 cx = 0; cx < w; ++cx)
        {
            const F32 mx = (cx + 0.5f) * cell, my = (cy + 0.5f) * cell;
            const bool edge = mx < band || my < band || mx > (F32)r.mSizeX - band || my > (F32)r.mSizeY - band;
            if (!edge) { out += 'x'; continue; }
            const F32 e = fld ? WolfWaterField::exposureAt(*fld, ox + mx, oy + my) : 1.f;
            out += (e >= 0.3f) ? 'o' : 'x';
        }
    }
    return out;
}

void WolfWaveZones::fill(LLViewerRegion* regionp, F32 x0, F32 y0, F32 sx, F32 sy, S32 w, S32 h, std::vector<F32>& out) const
{
    out.assign((size_t)w * h, OPEN_ENERGY);
    if (!regionp || mByHandle.empty()) return;
    S32 bx, by;
    handle_xy(regionp->getHandle(), bx, by);
    struct Span { F32 x, y, sx, sy; S32 w; S32 cell; std::string zones; };
    std::vector<Span> spans;
    for (const auto& kv : mByHandle)
    {
        const Region& r = kv.second;
        S32 ax, ay;
        handle_xy(r.mHandle, ax, ay);
        spans.push_back({ (F32)(ax - bx), (F32)(ay - by), (F32)r.mSizeX, (F32)r.mSizeY, r.w(), r.mCell, zonesFor(r) });
    }
    const F32 tx = sx / w, ty = sy / h;
    const F32 rw = regionp->getWidth(), rh = regionp->getWidth();
    const Span* mine = nullptr;
    for (const Span& s : spans) if (s.x == 0.f && s.y == 0.f) { mine = &s; break; }
    if (mine)
    {
        const size_t surf = std::count(mine->zones.begin(), mine->zones.end(), 's');
        const bool previewed = mPreview.find(regionp->getHandle()) != mPreview.end();
        LL_INFOS("WolfWaveZones") << "fill for " << regionp->getName() << ": " << mine->zones.size() << " cells, "
                                  << surf << " surf, source " << (previewed ? "PREVIEW" : "stored/default") << LL_ENDL;
    }
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
                const S32 cx = (S32)((px - s.x) / s.cell), cy = (S32)((py - s.y) / s.cell);
                const size_t k = (size_t)cy * s.w + cx;
                out[(size_t)j * w + i] = k < s.zones.size() ? energy_of(s.zones[k]) : OPEN_ENERGY;
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
                const S32 cx = (S32)(qx / mine->cell), cy = (S32)(qy / mine->cell);
                const size_t k = (size_t)cy * mine->w + cx;
                if (k < mine->zones.size() && mine->zones[k] == 's') out[(size_t)j * w + i] = energy_of('s');
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
    const Region* cur = current();
    const S32 cell = cur ? cur->mCell : CELL_M;
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

void WolfWaveZones::preview(const std::string& zones)
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return;
    mPreview[rgn->getHandle()] = zones;
    const Region* r = current();
    LL_INFOS("WolfWaveZones") << "preview set for handle " << rgn->getHandle() << ": " << zones.size()
                              << " cells (record " << (r ? r->mHandle : 0) << ", " << (r ? r->w() * r->h() : 0)
                              << " expected)" << LL_ENDL;
    WolfWaterField::instance().invalidate();
}

void WolfWaveZones::previewParams(const LLSD& params)
{
    // Uniforms, read every frame by lldrawpoolwater.cpp through params(): no rebake needed.
    mPreviewParams = params;
}

const LLSD& WolfWaveZones::params() const
{
    static const LLSD none;
    if (mPreviewParams.isMap()) return mPreviewParams;
    const Region* r = current();
    return r ? r->mParams : none;
}

void WolfWaveZones::clearPreview()
{
    mPreviewParams = LLSD();
    if (mPreview.empty()) return;
    mPreview.clear();
    LL_INFOS("WolfWaveZones") << "preview cleared" << LL_ENDL;
    WolfWaterField::instance().invalidate();
}

// ── save ───────────────────────────────────────────────────────────────────────────────

void WolfWaveZones::save(const std::string& zones, const LLSD& params, bool enabled)
{
    const Region* r = current();
    if (!r) { notify("This region is not on the Wolf Territories grid."); return; }
    if (mSaving) return;
    mSaving = true;
    const std::string uuid = r->mUuid;
    const S32 version = r->mVersion;
    LLCoros::instance().launch("WolfWaveZones save", [uuid, zones, params, enabled, version]()
    {
        WolfWaveZones::instance().saveCoro(uuid, zones, params, enabled, version, false);
    });
}

// Source: wolfspeech.cpp postRaw — the agent and session ids the service verifies against
// the grid's presence service; no secret is carried by this (public) viewer.
void WolfWaveZones::saveCoro(std::string region_uuid, std::string zones, LLSD params, bool enabled, S32 version, bool retried)
{
    LLSD body;
    body["region"] = region_uuid;
    body["layout"] = LLSD().with("zones", zones).with("params", params);
    body["enabled"] = enabled;
    body["version"] = version;
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
        mFetching = true;
        fetchCoro(neighbourHandles());
        const Region* r = current();
        if (r && r->mUuid == region_uuid)
        {
            saveCoro(region_uuid, zones, params, enabled, r->mVersion, true);
            return;
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
        notify("Could not save the wave layout: " + msg);
        return;
    }
    mLastError.clear();
    const LLSD& r = reply["region"];
    auto it = mByHandle.find(std::strtoull(r["handle"].asString().c_str(), nullptr, 10));
    if (it != mByHandle.end())
    {
        Region& rec = it->second;
        rec.mVersion = r["version"].asInteger();
        rec.mEnabled = r["enabled"].asBoolean();
        if (r["layout"].isMap())
        {
            rec.mStored = true;
            rec.mZones = r["layout"]["zones"].asString();
            rec.mParams = r["layout"]["params"];
        }
        mPreview.erase(rec.mHandle);
    }
    mPreviewParams = LLSD();
    WolfWaterField::instance().invalidate();
    notify("Wave layout saved for " + r["name"].asString() + " — everyone in the region now sees it.");
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// WolfWavePainter
// ═══════════════════════════════════════════════════════════════════════════════════════

WolfWavePainter::WolfWavePainter(const Params& p) : LLUICtrl(p) {}

void WolfWavePainter::setLayout(S32 w, S32 h, const std::string& zones, const std::vector<bool>& locked)
{
    mW = llmax(1, w);
    mH = llmax(1, h);
    mZones = zones;
    if ((S32)mZones.size() != mW * mH) mZones.assign((size_t)mW * mH, 'o');
    mLocked = locked;
    if ((S32)mLocked.size() != mW * mH) mLocked.assign((size_t)mW * mH, false);
    mDirty = false;
}

// Fractional: a 16-cell region in a 224 px box gets 14 px cells, a 256-cell one 0.875 px —
// the same box, scaled to fit (Paul: "on massive regions scale the drawing down").
F32 WolfWavePainter::cellPx() const
{
    return llmax(0.5f, llmin((F32)getRect().getWidth() / (F32)mW, (F32)getRect().getHeight() / (F32)mH));
}

bool WolfWavePainter::cellAt(S32 x, S32 y, S32& cx, S32& cy) const
{
    const F32 px = cellPx();
    cx = (S32)floorf((F32)x / px);
    cy = (S32)floorf((F32)y / px);   // local y is bottom-up: row 0 (the region's south edge) is at the bottom
    return cx >= 0 && cy >= 0 && cx < mW && cy < mH;
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
    const S32 N = llclamp(mW * 4, 64, 512);
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

void WolfWavePainter::draw()
{
    const F32 px = cellPx();
    const S32 gw = ll_round(mW * px), gh = ll_round(mH * px);
    gl_rect_2d(0, getRect().getHeight(), getRect().getWidth(), 0, LLColor4(0.04f, 0.06f, 0.12f, 1.f));
    refreshTerrain();
    if (mTerrainTex.notNull())
    {
        gl_draw_scaled_image(0, 0, gw, gh, mTerrainTex);
    }
    for (S32 cy = 0; cy < mH; ++cy)
    {
        for (S32 cx = 0; cx < mW; ++cx)
        {
            const size_t k = (size_t)cy * mW + cx;
            // Tints over the terrain; an OFF cell has none, the terrain shows through.
            // Source: land_waves_tab.js COLOURS.
            bool tint = true;
            LLColor4 c;
            switch (mZones[k])
            {
                case 's': c = LLColor4(0.31f, 0.64f, 1.0f, 0.72f); break;
                case 'o': c = LLColor4(0.17f, 0.44f, 0.71f, 0.66f); break;
                case 'm': c = LLColor4(0.51f, 0.77f, 0.93f, 0.58f); break;
                case 'c': c = LLColor4(0.44f, 0.76f, 0.64f, 0.55f); break;
                default:  tint = false; break;
            }
            // Snapped edges shared with the neighbours: no seams at fractional cell sizes.
            const S32 left = ll_round(cx * px), right = ll_round((cx + 1) * px);
            const S32 bottom = ll_round(cy * px), top = ll_round((cy + 1) * px);
            if (right <= left || top <= bottom) continue;
            if (tint) gl_rect_2d(left, top, right, bottom, c);
            if (mLocked[k])
            {
                gl_rect_2d(left, top, right, bottom, LLColor4(0.f, 0.f, 0.f, 0.45f));
            }
            if (px >= 8.f)
            {
                gl_rect_2d(left, top, right, bottom, LLColor4(1.f, 1.f, 1.f, 0.08f), false);
            }
        }
    }
    LLUICtrl::draw();
}

void WolfWavePainter::paint(S32 x, S32 y)
{
    S32 cx, cy;
    if (!cellAt(x, y, cx, cy)) return;
    const size_t k = (size_t)cy * mW + cx;
    if (mLocked[k])
    {
        if (!mRefusedThisStroke && mOnRefused) { mRefusedThisStroke = true; mOnRefused(); }
        return;
    }
    if (mZones[k] == mBrush) return;
    mZones[k] = mBrush;
    mDirty = true;
    if (mOnPaint) mOnPaint();
}

bool WolfWavePainter::handleMouseDown(S32 x, S32 y, MASK mask)
{
    if (!getEnabled()) return LLUICtrl::handleMouseDown(x, y, mask);
    mPainting = true;
    mRefusedThisStroke = false;
    gFocusMgr.setMouseCapture(this);
    paint(x, y);
    return true;
}

bool WolfWavePainter::handleHover(S32 x, S32 y, MASK mask)
{
    if (mPainting && hasMouseCapture())
    {
        paint(x, y);
        return true;
    }
    return LLUICtrl::handleHover(x, y, mask);
}

bool WolfWavePainter::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (hasMouseCapture())
    {
        mPainting = false;
        gFocusMgr.setMouseCapture(NULL);
        return true;
    }
    return LLUICtrl::handleMouseUp(x, y, mask);
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// WolfPanelLandWaves — About Land > Waves
// ═══════════════════════════════════════════════════════════════════════════════════════

// The parcel handle is what the other About Land panels take; this one reads the parcel through
// LLViewerParcelMgr at paint time, so it is accepted for the shared factory and not stored
// (clang's -Wunused-private-field is an error on the mac CI).
WolfPanelLandWaves::WolfPanelLandWaves(LLParcelSelectionHandle& /*parcel*/) : LLPanel() {}

bool WolfPanelLandWaves::postBuild()
{
    mPainter     = getChild<WolfWavePainter>("waves_painter");
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

    mPainter->setPaintCallback([this]() { WolfWaveZones::instance().preview(mPainter->zones()); armBakeConfirm(); });
    mPainter->setRefusedCallback([this]() { setStatus(getString("str_locked_cell"), true); });
    onBrush('s');
    return true;
}

void WolfPanelLandWaves::onBrush(char z)
{
    if (mPainter) mPainter->setBrush(z);
    mBrushS->setToggleState(z == 's');
    mBrushO->setToggleState(z == 'o');
    mBrushM->setToggleState(z == 'm');
    mBrushC->setToggleState(z == 'c');
    mBrushX->setToggleState(z == 'x');
}

void WolfPanelLandWaves::setStatus(const std::string& msg, bool error)
{
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
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (mBakeWaitUntil > 0.0)
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
    if (gDisconnected) return;
    WolfWaveZones& wz = WolfWaveZones::instance();
    const WolfWaveZones::Region* r = wz.current();
    LLViewerRegion* rgn = gAgent.getRegion();
    const U64 handle = rgn ? rgn->getHandle() : 0;
    // A save just finished: show its result and reload the layout.
    if (mWasSaving && !wz.saving())
    {
        mWasSaving = false;
        if (wz.lastError().empty()) { setStatus(getString("str_saved"), false); mShownVersion = -1; }
        else setStatus(getString("str_save_failed") + " " + wz.lastError(), true);
    }
    if (mSave) mSave->setEnabled(r != nullptr && !wz.saving());
    if (!r)
    {
        if (mNote) mNote->setText(getString(wz.fetching() ? "str_fetching" : "str_off_grid"));
        if (mPainter) { mPainter->setEnabled(false); mPainter->setLayout(16, 16, std::string(256, 'o'), {}); }
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
        || (mPainter && !mPainter->dirty() && mPainter->zones().size() != (size_t)(r->w() * r->h()));
    if (changed)
    {
        if (mPainter && mPainter->dirty() && handle == mShownHandle)
        {
            setStatus(getString("str_newer_on_grid"), false);
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
    const S32 w = r->w(), h = r->h();
    const bool all = wz.canEditAll();
    std::vector<bool> locked((size_t)w * h, false);
    if (!all)
    {
        for (S32 cy = 0; cy < h; ++cy)
            for (S32 cx = 0; cx < w; ++cx)
                locked[(size_t)cy * w + cx] = !wz.cellEditable(cx, cy);
    }
    mPainter->setLayout(w, h, wz.zonesFor(*r), locked);
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
    args["[W]"] = std::to_string(w);
    args["[H]"] = std::to_string(h);
    args["[CELL]"] = std::to_string(r->mCell);
    args["[STATE]"] = r->mStored ? getString("str_stored") : getString("str_automatic");
    mNote->setText(getString(all ? "str_note_all" : "str_note_parcel", args));
    setStatus("", false);
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
    // Live in the water until Save or Revert, like the brush (WolfStorm land_waves_tab.js).
    WolfWaveZones& wz = WolfWaveZones::instance();
    if (!wz.current() || !mPainter) return;
    wz.previewParams(paramsFromControls());
    // Off = the automatic layout (waves round the edge, flat inside): preview that, so the switch is visible at once.
    wz.preview(mEnabled->get() ? mPainter->zones() : wz.defaultZones(*wz.current()));
    armBakeConfirm();
}

void WolfPanelLandWaves::onSave()
{
    if (!mPainter) return;
    WolfWaveZones& wz = WolfWaveZones::instance();
    setStatus(getString("str_saving"), false);
    mWasSaving = true;
    mSave->setEnabled(false);
    wz.save(mPainter->zones(), paramsFromControls(), mEnabled->get());
}

void WolfPanelLandWaves::onRevert()
{
    WolfWaveZones::instance().clearPreview();
    if (mPainter) mPainter->clearDirty();
    mShownVersion = -1;
    refresh();
}

void WolfPanelLandWaves::onDefault()
{
    WolfWaveZones& wz = WolfWaveZones::instance();
    const WolfWaveZones::Region* r = wz.current();
    if (!r || !mPainter) return;
    std::vector<bool> none((size_t)r->w() * r->h(), false);
    mPainter->setLayout(r->w(), r->h(), wz.defaultZones(*r), none);
    wz.preview(mPainter->zones());
    setStatus(getString("str_default_loaded"), false);
    armBakeConfirm();
}
