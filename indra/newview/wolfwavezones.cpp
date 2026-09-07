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
#include "llviewerparceloverlay.h"
#include "llviewerregion.h"
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

// Source: wave_zones.js defaultZones — surf in the outer SURF_BAND_M where the water is open,
// calm where land is close on all sides (exposure < 0.3), open elsewhere. The exposure field
// is the agent region's (it spans the neighbours), so a neighbour's cells are offset into it.
std::string WolfWaveZones::defaultZones(const Region& r) const
{
    const S32 w = r.w(), h = r.h();
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
            const F32 mx = (cx + 0.5f) * CELL_M, my = (cy + 0.5f) * CELL_M;
            const F32 e = fld ? WolfWaterField::exposureAt(*fld, ox + mx, oy + my) : 1.f;
            // [Paul 09-07] The automatic layout has NO surf: "surf is a special thing" a designer
            // paints. Calm where land closes in (rivers, harbours), open elsewhere. wave_zones.js same.
            if (e < 0.3f) out += 'c';
            else out += 'o';
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
    struct Span { F32 x, y, sx, sy; S32 w; std::string zones; };
    std::vector<Span> spans;
    for (const auto& kv : mByHandle)
    {
        const Region& r = kv.second;
        S32 ax, ay;
        handle_xy(r.mHandle, ax, ay);
        spans.push_back({ (F32)(ax - bx), (F32)(ay - by), (F32)r.mSizeX, (F32)r.mSizeY, r.w(), zonesFor(r) });
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
                const S32 cx = (S32)((px - s.x) / CELL_M), cy = (S32)((py - s.y) / CELL_M);
                const size_t k = (size_t)cy * s.w + cx;
                out[(size_t)j * w + i] = k < s.zones.size() ? energy_of(s.zones[k]) : OPEN_ENERGY;
                covered = true;
                break;
            }
            // [SURF 2026-09-07] Space no region covers (the void sea round the region) carries
            // on from this region's nearest edge cell, so surf painted along the edge rolls in
            // from far out instead of fading over the last texel before the border (Paul:
            // "waves from the edge of the region"; wave_zones.js bake does the same).
            if (!covered && mine)
            {
                const F32 qx = llclamp(px, 0.f, rw - 0.5f), qy = llclamp(py, 0.f, rh - 0.5f);
                const S32 cx = (S32)(qx / CELL_M), cy = (S32)(qy / CELL_M);
                const size_t k = (size_t)cy * mine->w + cx;
                if (k < mine->zones.size()) out[(size_t)j * w + i] = energy_of(mine->zones[k]);
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
    // Every 4 m parcel tile under the 16 m cell must be the agent's own
    // (llviewerparceloverlay.cpp isOwnedSelf: PARCEL_SELF at that tile).
    const S32 per = CELL_M / (S32)PARCEL_GRID_STEP_METERS;
    for (S32 ty = 0; ty < per; ++ty)
    {
        for (S32 tx = 0; tx < per; ++tx)
        {
            const LLVector3 pos((F32)(cx * CELL_M + tx * (S32)PARCEL_GRID_STEP_METERS) + 2.f,
                                (F32)(cy * CELL_M + ty * (S32)PARCEL_GRID_STEP_METERS) + 2.f, 0.f);
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
        WolfWaveZones::instance().saveCoro(uuid, zones, params, enabled, version);
    });
}

// Source: wolfspeech.cpp postRaw — the agent and session ids the service verifies against
// the grid's presence service; no secret is carried by this (public) viewer.
void WolfWaveZones::saveCoro(std::string region_uuid, std::string zones, LLSD params, bool enabled, S32 version)
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

S32 WolfWavePainter::cellPx() const
{
    return llmax(1, llmin(getRect().getWidth() / mW, getRect().getHeight() / mH));
}

bool WolfWavePainter::cellAt(S32 x, S32 y, S32& cx, S32& cy) const
{
    const S32 px = cellPx();
    cx = x / px;
    cy = y / px;   // local y is bottom-up: row 0 (the region's south edge) is at the bottom
    return cx >= 0 && cy >= 0 && cx < mW && cy < mH;
}

void WolfWavePainter::draw()
{
    const S32 px = cellPx();
    gl_rect_2d(0, getRect().getHeight(), getRect().getWidth(), 0, LLColor4(0.04f, 0.06f, 0.12f, 1.f));
    for (S32 cy = 0; cy < mH; ++cy)
    {
        for (S32 cx = 0; cx < mW; ++cx)
        {
            const size_t k = (size_t)cy * mW + cx;
            LLColor4 c;
            switch (mZones[k])
            {
                case 's': c = LLColor4(0.31f, 0.64f, 1.0f, 1.f); break;
                case 'c': c = LLColor4(0.44f, 0.76f, 0.64f, 1.f); break;
                case 'x': c = LLColor4(0.27f, 0.27f, 0.33f, 1.f); break;
                default:  c = LLColor4(0.17f, 0.44f, 0.71f, 1.f); break;
            }
            const S32 left = cx * px, bottom = cy * px;
            gl_rect_2d(left, bottom + px, left + px, bottom, c);
            if (mLocked[k])
            {
                gl_rect_2d(left, bottom + px, left + px, bottom, LLColor4(0.f, 0.f, 0.f, 0.45f));
            }
            if (px >= 8)
            {
                gl_rect_2d(left, bottom + px, left + px, bottom, LLColor4(1.f, 1.f, 1.f, 0.08f), false);
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

WolfPanelLandWaves::WolfPanelLandWaves(LLParcelSelectionHandle& parcel) : LLPanel(), mParcel(parcel) {}

bool WolfPanelLandWaves::postBuild()
{
    mPainter     = getChild<WolfWavePainter>("waves_painter");
    mStatus      = getChild<LLTextBox>("waves_status");
    mNote        = getChild<LLTextBox>("waves_note");
    mSurfHeight  = getChild<LLSliderCtrl>("waves_surf_height");
    mSetInterval = getChild<LLSliderCtrl>("waves_set_interval");
    mCalmRipple  = getChild<LLSliderCtrl>("waves_calm_ripple");
    mEnabled     = getChild<LLCheckBoxCtrl>("waves_enabled");
    mSave        = getChild<LLButton>("waves_save");
    mBrushS      = getChild<LLButton>("waves_brush_s");
    mBrushO      = getChild<LLButton>("waves_brush_o");
    mBrushC      = getChild<LLButton>("waves_brush_c");
    mBrushX      = getChild<LLButton>("waves_brush_x");

    mBrushS->setCommitCallback(boost::bind(&WolfPanelLandWaves::onBrush, this, 's'));
    mBrushO->setCommitCallback(boost::bind(&WolfPanelLandWaves::onBrush, this, 'o'));
    mBrushC->setCommitCallback(boost::bind(&WolfPanelLandWaves::onBrush, this, 'c'));
    mBrushX->setCommitCallback(boost::bind(&WolfPanelLandWaves::onBrush, this, 'x'));
    mSave->setCommitCallback(boost::bind(&WolfPanelLandWaves::onSave, this));
    getChild<LLButton>("waves_revert")->setCommitCallback(boost::bind(&WolfPanelLandWaves::onRevert, this));
    getChild<LLButton>("waves_default")->setCommitCallback(boost::bind(&WolfPanelLandWaves::onDefault, this));
    mSurfHeight->setCommitCallback(boost::bind(&WolfPanelLandWaves::onParamChanged, this));
    mSetInterval->setCommitCallback(boost::bind(&WolfPanelLandWaves::onParamChanged, this));
    mCalmRipple->setCommitCallback(boost::bind(&WolfPanelLandWaves::onParamChanged, this));
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
    // Only rebuild the working copy when the region or its stored version changed, so a
    // refresh while painting does not throw the edits away.
    if (handle != mShownHandle || r->mVersion != mShownVersion || (mPainter && !mPainter->dirty() && mPainter->zones().size() != (size_t)(r->w() * r->h())))
    {
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
    mSurfHeight->setValue(p.has("surfHeight") ? p["surfHeight"].asReal() : 3.0);
    mSetInterval->setValue(p.has("setInterval") ? p["setInterval"].asReal() : 90.0);
    mCalmRipple->setValue(p.has("calmRipple") ? p["calmRipple"].asReal() * 100.0 : 3.0);
    mEnabled->set(r->mEnabled);
    mSurfHeight->setEnabled(all);
    mSetInterval->setEnabled(all);
    mCalmRipple->setEnabled(all);
    mEnabled->setEnabled(all);
    getChild<LLButton>("waves_default")->setEnabled(all);
    LLStringUtil::format_map_t args;
    args["[REGION]"] = r->mName;
    args["[W]"] = std::to_string(w);
    args["[H]"] = std::to_string(h);
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
    return p;
}

void WolfPanelLandWaves::onParamChanged()
{
    // Live in the water until Save or Revert, like the brush (WolfStorm land_waves_tab.js).
    WolfWaveZones& wz = WolfWaveZones::instance();
    if (!wz.current() || !mPainter) return;
    wz.previewParams(paramsFromControls());
    // Off = the automatic layout: preview that, so the switch is visible at once.
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
