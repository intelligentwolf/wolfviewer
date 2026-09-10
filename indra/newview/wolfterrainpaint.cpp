/**
 * @file wolfterrainpaint.cpp
 * @brief WolfViewer: painted ground textures — roads and tracks drawn on the terrain (Build > Paint).
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
#include <array>
#include <cmath>

#include "wolfterrainpaint.h"

#include <boost/json.hpp>

#include "llagent.h"
#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "llfloatertools.h"
#include "llframetimer.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llhttpconstants.h"
#include "llimagegl.h"
#include "llnotificationsutil.h"
#include "llrender.h"
#include "llsdjson.h"
#include "llsliderctrl.h"
#include "llspinctrl.h"
#include "llsurface.h"
#include "lltextbox.h"
#include "lltexturectrl.h"
#include "lltoolcomp.h"
#include "lltoolmgr.h"
#include "lluictrlfactory.h"
#include "llviewercontrol.h"
#include "llviewerparceloverlay.h"
#include "llviewerregion.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "llviewertexturelist.h"
#include "llviewerwindow.h"
#include "llvlcomposition.h"
#include "llworld.h"
#include "llviewerobjectlist.h"
#include "llvowater.h"
#include "m3math.h"
#include "pipeline.h"
#include "wolfgrid.h"

// Source: wolfstorm/js/world/terrain_paint.js TerrainPaint.API — one host owns the data for every viewer.
const char* WolfTerrainPaint::API_URL = "https://wolfstorm.app/php/terrain_paint.php";

static LLPanelInjector<WolfPanelTerrainPaint> t_wolf_panel_terrain_paint("wolf_panel_terrain_paint");

namespace
{
    constexpr F32 TAU = 6.2831853f;

    // Region handles are (x << 32) | y in metres (indra/llmath/v3dmath.h from_region_handle).
    U64 handle_of(S32 x, S32 y) { return ((U64)(U32)x << 32) | (U64)(U32)y; }
    void handle_xy(U64 handle, S32& x, S32& y) { x = (S32)(handle >> 32); y = (S32)(handle & 0xffffffffULL); }

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

    F32 round2(F32 v) { return roundf(v * 100.f) / 100.f; }
}

// IEEE 754 binary16, round to nearest even — the same numbers THREE.DataUtils.toHalfFloat writes.
U16 WolfTerrainPaint::f32_to_f16(F32 v)
{
    U32 x; memcpy(&x, &v, 4);
    const U16 sign = (U16)((x >> 16) & 0x8000);
    S32 exp = (S32)((x >> 23) & 0xff);
    U32 mant = x & 0x7fffff;
    if (exp == 0xff) return (U16)(sign | 0x7c00 | (mant ? 0x200 : 0));
    S32 e = exp - 127 + 15;
    if (e >= 0x1f) return (U16)(sign | 0x7c00);
    if (e <= 0)
    {
        if (e < -10) return sign;
        mant |= 0x800000;
        const S32 shift = 14 - e;
        U32 hm = mant >> shift;
        if ((mant >> (shift - 1)) & 1) ++hm;
        return (U16)(sign | hm);
    }
    U32 hv = sign | ((U32)e << 10) | (mant >> 13);
    if (mant & 0x1000) ++hv;
    return (U16)hv;
}

F32 WolfTerrainPaint::f16_to_f32(U16 hv)
{
    const F32 sign = (hv & 0x8000) ? -1.f : 1.f;
    const S32 exp = (hv >> 10) & 0x1f;
    const S32 mant = hv & 0x3ff;
    if (exp == 0) return sign * (F32)mant * ldexpf(1.f, -24);
    if (exp == 0x1f) return mant ? NAN : sign * INFINITY;
    return sign * (1.f + (F32)mant / 1024.f) * ldexpf(1.f, exp - 15);
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// WolfTerrainPaint — records, layers, the brush, save
// ═══════════════════════════════════════════════════════════════════════════════════════

WolfTerrainPaint::WolfTerrainPaint() {}
WolfTerrainPaint::~WolfTerrainPaint()
{
    for (auto& kv : mLayers) releaseLayer(kv.second);
}

F32 WolfTerrainPaint::clampWidth(F32 w)   { return std::isfinite(w) ? llclamp(w, MIN_WIDTH, MAX_WIDTH) : 4.f; }
F32 WolfTerrainPaint::clampOpacity(F32 o) { return std::isfinite(o) ? llclamp(o, 0.05f, 1.f) : 1.f; }
F32 WolfTerrainPaint::clampScale(F32 s)   { return std::isfinite(s) ? llclamp(s, MIN_SCALE, MAX_SCALE) : 4.f; }

// Source: terrain_paint.js TerrainPaint.strokeKey — the same rounding, so a stroke saved by one
// viewer is "the same stroke" to the other on a 409 merge.
std::string WolfTerrainPaint::strokeKey(const Stroke& s)
{
    std::string k = llformat("%d|%.2f|%.2f|", s.mSlot, round2(s.mWidth), round2(s.mOpacity));
    for (F32 v : s.mPoints) k += llformat("%.2f,", round2(v));
    if (s.mSlot == -2)
    {
        k += "|z:";
        for (F32 v : s.mZ) k += llformat("%.2f,", round2(v));
        k += llformat("|d:%.2f", round2(s.mDepth));
    }
    return k;
}

namespace
{
    F32 point_seg_d2(F32 x, F32 y, F32 sx, F32 sy, F32 ex, F32 ey)
    {
        const F32 vx = ex - sx, vy = ey - sy, l2 = vx * vx + vy * vy;
        F32 t = l2 > 1e-9f ? ((x - sx) * vx + (y - sy) * vy) / l2 : 0.f;
        t = llclamp(t, 0.f, 1.f);
        const F32 qx = x - (sx + t * vx), qy = y - (sy + t * vy);
        return qx * qx + qy * qy;
    }
    F32 cross2(F32 ox, F32 oy, F32 px, F32 py, F32 qx, F32 qy) { return (px - ox) * (qy - oy) - (py - oy) * (qx - ox); }
    /** Squared distance between segments a-b and c-d (0 when they cross). terrain_paint_water.js _segDist2. */
    F32 seg_seg_d2(F32 ax, F32 ay, F32 bx, F32 by, F32 cx, F32 cy, F32 dx, F32 dy)
    {
        const F32 d1 = cross2(cx, cy, dx, dy, ax, ay), d2 = cross2(cx, cy, dx, dy, bx, by);
        const F32 d3 = cross2(ax, ay, bx, by, cx, cy), d4 = cross2(ax, ay, bx, by, dx, dy);
        if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0))) return 0.f;
        return llmin(llmin(point_seg_d2(ax, ay, cx, cy, dx, dy), point_seg_d2(bx, by, cx, cy, dx, dy)),
                     llmin(point_seg_d2(cx, cy, ax, ay, bx, by), point_seg_d2(dx, dy, ax, ay, bx, by)));
    }
    /** A polyline as segments; a single point is a zero-length segment. */
    void segments_of(const std::vector<F32>& p, std::vector<std::array<F32, 4>>& out)
    {
        out.clear();
        if (p.size() == 2) out.push_back({ p[0], p[1], p[0], p[1] });
        for (size_t k = 2; k + 1 < p.size(); k += 2) out.push_back({ p[k - 2], p[k - 1], p[k], p[k + 1] });
    }
}

// Any water SEGMENT within (we + ww)/2 of any erase segment. Source: terrain_paint_water.js intersects.
bool WolfTerrainPaint::waterIntersects(const Stroke& water, const Stroke& erase)
{
    const F32 r = (clampWidth(water.mWidth) + clampWidth(erase.mWidth)) * 0.5f;
    const F32 r2 = r * r;
    std::vector<std::array<F32, 4>> ws, es;
    segments_of(water.mPoints, ws);
    segments_of(erase.mPoints, es);
    for (const auto& a : ws)
        for (const auto& b : es)
            if (seg_seg_d2(a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]) <= r2) return true;
    return false;
}

void WolfTerrainPaint::simplifyWater(Stroke& s, F32 tol)
{
    const size_t n = s.mPoints.size() / 2;
    if (n <= 2 || s.mZ.size() != n) return;
    std::vector<U8> keep(n, 0);
    keep[0] = keep[n - 1] = 1;
    std::vector<std::pair<size_t, size_t>> stack;
    stack.push_back({ 0, n - 1 });
    const std::vector<F32>& p = s.mPoints;
    while (!stack.empty())
    {
        const auto ab = stack.back(); stack.pop_back();
        const size_t a = ab.first, b = ab.second;
        const F32 ax = p[a * 2], ay = p[a * 2 + 1], bx = p[b * 2], by = p[b * 2 + 1];
        const F32 vx = bx - ax, vy = by - ay, l2 = vx * vx + vy * vy;
        size_t worst = 0; bool found = false; F32 worst_d = tol * tol;
        for (size_t i = a + 1; i < b; ++i)
        {
            const F32 x = p[i * 2], y = p[i * 2 + 1];
            F32 d2;
            if (l2 < 1e-9f) { const F32 dx = x - ax, dy = y - ay; d2 = dx * dx + dy * dy; }
            else
            {
                const F32 t = llclamp(((x - ax) * vx + (y - ay) * vy) / l2, 0.f, 1.f);
                const F32 dx = x - (ax + t * vx), dy = y - (ay + t * vy); d2 = dx * dx + dy * dy;
            }
            if (d2 > worst_d) { worst_d = d2; worst = i; found = true; }
        }
        if (found) { keep[worst] = 1; stack.push_back({ a, worst }); stack.push_back({ worst, b }); }
    }
    std::vector<F32> op, oz;
    for (size_t i = 0; i < n; ++i) if (keep[i]) { op.push_back(p[i * 2]); op.push_back(p[i * 2 + 1]); oz.push_back(s.mZ[i]); }
    s.mPoints = op;
    s.mZ = oz;
}

F64 WolfTerrainPaint::lastFetchAgeSecs() const
{
    return mLastFetchAt > 0.0 ? (F64)LLFrameTimer::getElapsedSeconds() - mLastFetchAt : 1e9;
}

// ── fetch ──────────────────────────────────────────────────────────────────────────────

std::vector<U64> WolfTerrainPaint::neighbourHandles() const
{
    // Source: wolfwavezones.cpp neighbourHandles — the same ring, var-region aware.
    std::vector<U64> out;
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return out;
    S32 x, y;
    handle_xy(rgn->getHandle(), x, y);
    const S32 sx = (S32)rgn->getWidth(), sy = (S32)rgn->getWidth();
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

void WolfTerrainPaint::refresh()
{
    if (mFetching) return;
    std::vector<U64> handles = neighbourHandles();
    if (handles.empty()) return;
    mFetching = true;
    mNextRefresh = LLFrameTimer::getElapsedSeconds() + REFRESH_SECS;
    LLCoros::instance().launch("WolfTerrainPaint fetch", [handles]() { WolfTerrainPaint::instance().fetchCoro(handles); });
}

WolfTerrainPaint::Stroke WolfTerrainPaint::parseStroke(const LLSD& s, bool& ok)
{
    Stroke st;
    ok = false;
    if (!s.isMap() || !s["p"].isArray()) return st;
    st.mSlot = llclamp(s["t"].asInteger(), -2, SLOTS - 1);
    st.mWidth = clampWidth(s.has("w") ? (F32)s["w"].asReal() : 4.f);
    st.mOpacity = clampOpacity(s.has("o") ? (F32)s["o"].asReal() : 1.f);
    const LLSD& p = s["p"];
    for (S32 i = 0; i + 1 < (S32)p.size(); i += 2)
    {
        const F32 x = (F32)p[i].asReal(), y = (F32)p[i + 1].asReal();
        if (!std::isfinite(x) || !std::isfinite(y)) continue;
        st.mPoints.push_back(x);
        st.mPoints.push_back(y);
    }
    ok = st.mPoints.size() >= 2;
    if (st.mSlot == -2)
    {
        // Water: a surface height per point, the last one repeated where the record is short.
        const LLSD& z = s["z"];
        for (size_t i = 0; i < st.mPoints.size() / 2; ++i)
        {
            F32 zv = (z.isArray() && i < (size_t)z.size()) ? (F32)z[(S32)i].asReal() : (st.mZ.empty() ? 20.f : st.mZ.back());
            if (!std::isfinite(zv)) zv = st.mZ.empty() ? 20.f : st.mZ.back();
            st.mZ.push_back(zv);
        }
        const F32 d = s.has("d") ? (F32)s["d"].asReal() : WATER_DEPTH_DEFAULT;
        st.mDepth = std::isfinite(d) ? llclamp(d, 0.05f, 20.f) : WATER_DEPTH_DEFAULT;
    }
    return st;
}

LLSD WolfTerrainPaint::strokeToLLSD(const Stroke& s)
{
    LLSD out;
    out["t"] = s.mSlot;
    out["w"] = (F64)round2(s.mWidth);
    out["o"] = (F64)round2(s.mOpacity);
    LLSD p = LLSD::emptyArray();
    for (F32 v : s.mPoints) p.append((F64)round2(v));
    out["p"] = p;
    if (s.mSlot == -2)
    {
        LLSD z = LLSD::emptyArray();
        for (F32 v : s.mZ) z.append((F64)round2(v));
        out["z"] = z;
        out["d"] = (F64)round2(s.mDepth);
    }
    return out;
}

WolfTerrainPaint::Record WolfTerrainPaint::parseRecord(const LLSD& r)
{
    Record rec;
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
        const LLSD& tex = r["layout"]["textures"];
        for (S32 i = 0; i < SLOTS; ++i)
        {
            if (tex.isArray() && i < (S32)tex.size() && tex[i].isMap() && tex[i].has("id"))
            {
                rec.mTextures[i].mId.set(tex[i]["id"].asString());
                rec.mTextures[i].mScale = clampScale(tex[i].has("scale") ? (F32)tex[i]["scale"].asReal() : 4.f);
                rec.mTextures[i].mWorld = !(tex[i].has("mode") && tex[i]["mode"].asString() == "road");
            }
        }
        for (const LLSD& s : llsd::inArray(r["layout"]["strokes"]))
        {
            bool ok = false;
            Stroke st = parseStroke(s, ok);
            if (ok) rec.mStrokes.push_back(st);
        }
    }
    return rec;
}

// Source: wolfwavezones.cpp fetchCoro — the same adapter shape (getRawAndSuspend, JSON body).
void WolfTerrainPaint::fetchCoro(std::vector<U64> handles)
{
    std::string url = std::string(API_URL) + "?handles=";
    for (size_t i = 0; i < handles.size(); ++i)
    {
        if (i) url += ",";
        url += std::to_string(handles[i]);
    }
    LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
        std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfTerrainPaint", LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
    LLCore::HttpOptions::ptr_t options = std::make_shared<LLCore::HttpOptions>();
    options->setTimeout(20);
    LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
    headers->append(HTTP_OUT_HEADER_ACCEPT, "application/json");

    LLSD result = adapter->getRawAndSuspend(request, url, options, headers);
    LLSD httpResults = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(httpResults);
    mFetching = false;
    {
        LLViewerRegion* rgn_now = gAgent.getRegion();
        mFetchedForHandle = rgn_now ? rgn_now->getHandle() : 0;
    }
    if (!status)
    {
        mLastError = status.toString();
        LL_WARNS("WolfTerrainPaint") << "fetch failed: " << mLastError << LL_ENDL;
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
        LL_WARNS("WolfTerrainPaint") << "fetch: unexpected reply" << LL_ENDL;
        return;
    }
    mByHandle.clear();
    for (const LLSD& r : llsd::inArray(reply["regions"]))
    {
        Record rec = parseRecord(r);
        if (rec.mHandle) mByHandle[rec.mHandle] = rec;
    }
    // A clean working copy must not shadow the newer record; one from a region we have left
    // cannot be saved anywhere (terrain_paint.js refresh, same rule).
    if (mEdit && ((!mEdit->mDirty && !mLiveStroke) || mEdit->mHandle != mFetchedForHandle))
    {
        if (mEdit->mDirty) LL_WARNS("WolfTerrainPaint") << "unsaved ground paint for " << mEdit->mName << " dropped: the region changed" << LL_ENDL;
        mLiveStroke = false;
        mEdit.reset();
    }
    mLastError.clear();
    mLastFetchAt = LLFrameTimer::getElapsedSeconds();
    LL_INFOS("WolfTerrainPaint") << "paint records for " << mByHandle.size() << " regions" << LL_ENDL;
}

const WolfTerrainPaint::Record* WolfTerrainPaint::current() const
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return nullptr;
    auto it = mByHandle.find(rgn->getHandle());
    return it == mByHandle.end() ? nullptr : &it->second;
}

// ── layers ─────────────────────────────────────────────────────────────────────────────

bool WolfTerrainPaint::layoutFor(U64 handle, const Slot*& textures, const std::vector<Stroke>*& strokes, std::string& key) const
{
    if (mEdit && mEdit->mHandle == handle)
    {
        key = llformat("edit:%u:%d", mEdit->mRev, mEdit->mEnabled ? 1 : 0);
        if (!mEdit->mEnabled) return false;
        textures = mEdit->mTextures;
        strokes = &mEdit->mStrokes;
        return true;
    }
    auto it = mByHandle.find(handle);
    if (it == mByHandle.end()) { key = "none"; return false; }
    const Record& r = it->second;
    key = llformat("v:%d:%d", r.mVersion, r.mEnabled ? 1 : 0);
    if (!r.mStored || !r.mEnabled || r.mStrokes.empty()) return false;
    textures = r.mTextures;
    strokes = &r.mStrokes;
    return true;
}

WolfTerrainPaint::Layer& WolfTerrainPaint::layerFor(LLViewerRegion* regionp)
{
    Layer& L = mLayers[regionp->getHandle()];
    L.mHandle = regionp->getHandle();
    return L;
}

void WolfTerrainPaint::ensureBuffers(Layer& L, LLViewerRegion* regionp)
{
    const S32 sx = (S32)regionp->getWidth(), sy = (S32)regionp->getWidth();
    const bool is_main = regionp == gAgent.getRegion();
    const S32 max_side = is_main ? MAP_MAX_PX : MAP_MAX_PX_NEIGHBOUR;
    F32 ppm = MAP_PX_PER_M;
    while ((F32)llmax(sx, sy) * ppm > (F32)max_side) ppm *= 0.5f;
    const S32 w = llmax(1, ll_round(sx * ppm)), h = llmax(1, ll_round(sy * ppm));
    if (!L.mMap.empty() && L.mW == w && L.mH == h && L.mSizeX == sx && L.mSizeY == sy) return;
    if (L.mTex) { LLImageGL::deleteTextures(1, &L.mTex); L.mTex = 0; }
    L.mW = w; L.mH = h; L.mPpm = ppm; L.mSizeX = sx; L.mSizeY = sy;
    L.mMap.assign((size_t)w * h * 4, 0);
    L.mSDist.clear(); L.mSAlong.clear(); L.mSLat.clear();
    L.mSBoxSet = false;
    L.mLive = false;
    L.mDirtyGL = true;
}

void WolfTerrainPaint::ensureScratch(Layer& L)
{
    const size_t n = (size_t)L.mW * L.mH;
    if (L.mSDist.size() == n) return;
    L.mSDist.assign(n, std::numeric_limits<F32>::infinity());
    L.mSAlong.assign(n, 0.f);
    L.mSLat.assign(n, 0.f);
    L.mSBoxSet = false;
}

void WolfTerrainPaint::clearScratch(Layer& L)
{
    if (!L.mSBoxSet || L.mSDist.empty()) { L.mSBoxSet = false; return; }
    const Box& b = L.mSBox;
    for (S32 j = b.y0; j < b.y1; ++j)
    {
        std::fill(L.mSDist.begin() + (size_t)j * L.mW + b.x0, L.mSDist.begin() + (size_t)j * L.mW + b.x1, std::numeric_limits<F32>::infinity());
    }
    L.mSBoxSet = false;
}

void WolfTerrainPaint::killWater(Layer& L)
{
    for (LLPointer<LLVOWater>& w : L.mWater)
    {
        if (w.notNull() && !w->isDead()) gObjectList.killObject(w);
    }
    L.mWater.clear();
    L.mWaterKey.clear();
}

/**
 * The water strokes that survive the erase strokes, in order, as one LLVOWater plane per
 * segment (fswolfwater.cpp ensureSurface: transform first, then createObject; the plane's
 * X axis runs along the segment and tilts with the heights so a river runs downhill).
 * Source: terrain_paint_water.js surviving/_ribbon — keep the rules in step.
 */
void WolfTerrainPaint::rebuildWater(Layer& L, LLViewerRegion* regionp, const std::vector<Stroke>& strokes)
{
    killWater(L);
    std::vector<const Stroke*> water;
    for (const Stroke& s : strokes)
    {
        if (s.mSlot == -2) { water.push_back(&s); continue; }
        if (s.mSlot == -1)
        {
            for (S32 i = (S32)water.size() - 1; i >= 0; --i)
            {
                if (waterIntersects(*water[i], s)) water.erase(water.begin() + i);
            }
        }
    }
    for (const Stroke* sp : water)
    {
        const Stroke& s = *sp;
        const F32 w = clampWidth(s.mWidth);
        const size_t n = s.mPoints.size() / 2;
        if (n == 0 || s.mZ.size() != n) continue;
        const size_t segs = llmax((size_t)1, n - 1);
        for (size_t i = 0; i < segs; ++i)
        {
            if ((S32)L.mWater.size() >= MAX_WATER_PLANES)
            {
                LL_WARNS("WolfTerrainPaint") << "water: " << MAX_WATER_PLANES << " planes reached in region " << L.mHandle << " — the remaining water strokes are not drawn" << LL_ENDL;
                return;
            }
            const size_t j = llmin(i + 1, n - 1);
            F32 ax = s.mPoints[i * 2], ay = s.mPoints[i * 2 + 1], az = s.mZ[i];
            F32 bx = s.mPoints[j * 2], by = s.mPoints[j * 2 + 1], bz = s.mZ[j];
            if (n == 1) { bx = ax + w; by = ay; bz = az; ax -= w * 0.5f; bx -= w * 0.5f; }   // a dab: a square
            LLVector3 along(bx - ax, by - ay, bz - az);
            const F32 len = along.length();
            if (len < 0.01f) continue;
            along /= len;
            // Round caps like the painted band: extend each end by half the width.
            const LLVector3 flat(along.mV[VX], along.mV[VY], 0.f);
            LLVector3 x_axis = along;
            LLVector3 y_axis = LLVector3(0.f, 0.f, 1.f) % x_axis;   // left of travel
            if (y_axis.lengthSquared() < 1e-6f) continue;
            y_axis.normalize();
            LLVector3 z_axis = x_axis % y_axis;
            z_axis.normalize();
            LLMatrix3 m;
            m.setRows(x_axis, y_axis, z_axis);
            LLQuaternion rot(m);
            const LLVector3 mid((ax + bx) * 0.5f, (ay + by) * 0.5f, (az + bz) * 0.5f);
            LLVOWater* waterp = (LLVOWater*)gObjectList.createObjectViewer(LLViewerObject::LL_VO_WATER, regionp);
            if (!waterp) return;
            waterp->setRotation(rot);
            waterp->setPositionAgent(regionp->getPosAgentFromRegion(mid));
            waterp->setScale(LLVector3(len + w, w, 0.f));
            waterp->setBoundedWaterDepth(llclamp(s.mDepth, 0.05f, 20.f));
            gPipeline.createObject(waterp);
            gObjectList.updateActive(waterp);
            L.mWater.push_back(waterp);
        }
    }
}

std::string WolfTerrainPaint::texSig(const std::string& key, const Slot* textures, const std::vector<Stroke>& strokes)
{
    S32 n = 0; size_t pts = 0;
    for (const Stroke& s : strokes) if (s.mSlot != -2) { ++n; pts += s.mPoints.size(); }
    // The edit REVISION is dropped: it moves on every water stroke, undo and toggle, none of
    // which should rebake the map (the counts and tiles catch the ones that should). terrain_paint.js same.
    std::string base = key;
    if (key.rfind("edit:", 0) == 0) base = "edit:" + key.substr(key.find_last_of(':') + 1);
    std::string sig = base + llformat("|%d|%d|", n, (S32)pts);
    for (S32 i = 0; i < SLOTS; ++i) sig += textures[i].mId.isNull() ? "0," : llformat("%.3f%c,", clampScale(textures[i].mScale), textures[i].mWorld ? 'w' : 'r');
    return sig;
}

void WolfTerrainPaint::releaseLayer(Layer& L)
{
    killWater(L);
    L.mTexSig.clear();
    if (L.mTex) { LLImageGL::deleteTextures(1, &L.mTex); L.mTex = 0; }
    L.mMap.clear();
    L.mSDist.clear(); L.mSAlong.clear(); L.mSLat.clear();
    L.mSBoxSet = false;
    L.mHasPaint = false;
    L.mBaking = false;
    L.mPending.clear();
    L.mLive = false;
    L.mDirtyGL = false;
    for (S32 i = 0; i < SLOTS; ++i) { L.mUsed[i] = false; L.mPalette[i] = nullptr; }
    L.mW = L.mH = 0;
}

void WolfTerrainPaint::setPalette(Layer& L, const Slot* textures, bool& bakeChanged)
{
    bakeChanged = false;
    for (S32 i = 0; i < SLOTS; ++i)
    {
        const Slot& s = textures[i];
        const F32 scale = s.mId.isNull() ? 4.f : clampScale(s.mScale);
        const bool world = s.mId.isNull() ? true : s.mWorld;
        if (scale != L.mScales[i]) { L.mScales[i] = scale; if (L.mUsed[i]) bakeChanged = true; }
        if (world != L.mWorld[i]) { L.mWorld[i] = world; if (L.mUsed[i]) bakeChanged = true; }
        if (s.mId.isNull())
        {
            L.mPalette[i] = nullptr;
            continue;
        }
        if (L.mPalette[i].notNull() && L.mPalette[i]->getID() == s.mId) continue;
        // Source: llvlcomposition.cpp:75 — a terrain texture is fetched at BOOST_TERRAIN so it
        // arrives at full detail and stays resident.
        LLPointer<LLViewerFetchedTexture> tex = LLViewerTextureManager::getFetchedTexture(s.mId, FTT_DEFAULT, true, LLGLTexture::BOOST_TERRAIN);
        if (tex.notNull())
        {
            tex->setBoostLevel(LLGLTexture::BOOST_TERRAIN);
            tex->setAddressMode(LLTexUnit::TAM_WRAP);
        }
        L.mPalette[i] = tex;
    }
}

void WolfTerrainPaint::startBake(Layer& L, const std::vector<Stroke>& strokes)
{
    L.mLive = false;
    std::fill(L.mMap.begin(), L.mMap.end(), 0);
    for (S32 i = 0; i < SLOTS; ++i) L.mUsed[i] = false;
    for (const Stroke& s : strokes) if (s.mSlot >= 0 && s.mSlot < SLOTS) L.mUsed[s.mSlot] = true;
    L.mPending.clear();
    for (const Stroke& s : strokes) if (s.mSlot != -2) L.mPending.push_back(s);
    L.mHasPaint = !L.mPending.empty();
    L.mPendingIndex = 0;
    L.mBaking = !L.mPending.empty();
    L.mDirtyGL = true;
}

// Source: terrain_paint.js rebake — chunked so a long list does not stall the frame.
void WolfTerrainPaint::bakeSlice(Layer& L, F64 budget_secs)
{
    if (!L.mBaking) return;
    ensureScratch(L);
    const F64 t0 = LLFrameTimer::getElapsedSeconds();
    while (L.mPendingIndex < L.mPending.size() && (F64)LLFrameTimer::getElapsedSeconds() - t0 < budget_secs)
    {
        rasterStroke(L, L.mPending[L.mPendingIndex]);
        ++L.mPendingIndex;
    }
    L.mDirtyGL = true;
    if (L.mPendingIndex >= L.mPending.size())
    {
        L.mBaking = false;
        L.mPending.clear();
        ++mBakes;
        if (L.mHandle != (gAgent.getRegion() ? gAgent.getRegion()->getHandle() : 0))
        {
            // A neighbour is not painted on: its scratch (12 bytes a texel) can go.
            L.mSDist.clear(); L.mSAlong.clear(); L.mSLat.clear();
            L.mSBoxSet = false;
        }
    }
}

WolfTerrainPaint::Box WolfTerrainPaint::bbox(const Layer& L, const F32* pts, size_t n, F32 w) const
{
    Box b;
    F32 minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
    for (size_t k = 0; k + 1 < n; k += 2)
    {
        const F32 x = pts[k] * L.mPpm, y = pts[k + 1] * L.mPpm;
        minX = llmin(minX, x); maxX = llmax(maxX, x);
        minY = llmin(minY, y); maxY = llmax(maxY, y);
    }
    if (minX > maxX) return b;
    const F32 pad = w * L.mPpm * 0.5f + 2.f;
    b.x0 = llmax(0, (S32)floorf(minX - pad));
    b.y0 = llmax(0, (S32)floorf(minY - pad));
    b.x1 = llmin(L.mW, (S32)ceilf(maxX + pad));
    b.y1 = llmin(L.mH, (S32)ceilf(maxY + pad));
    return b;
}

void WolfTerrainPaint::unionBox(Box& a, bool& a_set, const Box& b)
{
    if (b.empty()) return;
    if (!a_set) { a = b; a_set = true; return; }
    a.x0 = llmin(a.x0, b.x0); a.y0 = llmin(a.y0, b.y0);
    a.x1 = llmax(a.x1, b.x1); a.y1 = llmax(a.y1, b.y1);
}

/**
 * Source: terrain_paint.js _rasterSegment — every pixel within the round-capped band records the
 * segment it is NEAREST to (distance, metres along the stroke = cum + the projection, and the
 * across-fraction: 0 at the left edge of travel, 1 at the right). Keep in step.
 */
bool WolfTerrainPaint::rasterSegment(Layer& L, F32 x0, F32 y0, F32 x1, F32 y1, F32 w, F32 cum, Box& out)
{
    const F32 seg[4] = { x0, y0, x1, y1 };
    out = bbox(L, seg, 4, w);
    if (out.empty()) return false;
    const F32 ppm = L.mPpm;
    const S32 W = L.mW;
    const F32 dx = x1 - x0, dy = y1 - y0;
    const F32 len2 = dx * dx + dy * dy;
    const F32 len = sqrtf(len2);
    const F32 R = w * 0.5f;
    for (S32 j = out.y0; j < out.y1; ++j)
    {
        const F32 py = ((F32)j + 0.5f) / ppm;
        const F32 ry = py - y0;
        for (S32 i = out.x0; i < out.x1; ++i)
        {
            const F32 px = ((F32)i + 0.5f) / ppm;
            const F32 rx = px - x0;
            F32 t = 0.f, nx = rx, ny = ry, lateral;
            if (len2 > 1e-9f)
            {
                t = llclamp((rx * dx + ry * dy) / len2, 0.f, 1.f);
                nx = rx - t * dx; ny = ry - t * dy;
                lateral = (dx * ry - dy * rx) / len;   // signed: left of travel positive
            }
            else
            {
                lateral = ry;                           // a dab: no direction, face north
            }
            const F32 dist = sqrtf(nx * nx + ny * ny);
            if (dist > R) continue;
            const size_t p = (size_t)j * W + i;
            // Ties stay with the segment processed first (the earlier one along the stroke).
            if (dist >= L.mSDist[p]) continue;
            L.mSDist[p] = dist;
            L.mSAlong[p] = cum + t * len;
            L.mSLat[p] = lateral;
        }
    }
    unionBox(L.mSBox, L.mSBoxSet, out);
    return true;
}

/**
 * Source: terrain_paint.js _composite — coverage feathers over the outer 12% of the half-width
 * (at least one map pixel); a later stroke covers an earlier one where its coverage is at least
 * the old one's; an erase stroke scales the old coverage down. Texel: R,G = cos,sin of the
 * along-phase (2π · along / tile), B = (slot + across)/4, A = coverage.
 */
/**
 * Source: terrain_paint.js _composite — coverage feathers over the outer 12% of the half-width
 * (at least one map pixel); a later stroke covers an earlier one where its coverage is at least
 * the old one's; an erase stroke scales the old coverage down. Texel (RGBA16F): R,G =
 * (0.25 + 0.2·slot)·(cos φ, sin φ), φ = 2π·along/tile (0 for a world-grid slot); B = metres
 * across from the left edge of travel / tile (0 for a world-grid slot); A = coverage.
 */
void WolfTerrainPaint::composite(Layer& L, const Box& bb, const Stroke& s, const std::vector<U16>& src, std::vector<U16>& dst)
{
    const S32 W = L.mW;
    const F32 w = clampWidth(s.mWidth);
    const F32 o = clampOpacity(s.mOpacity);
    const F32 R = w * 0.5f;
    const F32 fe = llmax(1.f / L.mPpm, R * 0.12f);
    const bool erase = s.mSlot < 0;
    const F32 tile = erase ? 4.f : L.mScales[s.mSlot];
    const bool world = !erase && L.mWorld[s.mSlot];
    const F32 radius = erase ? 0.f : 0.25f + 0.2f * (F32)s.mSlot;
    const bool same = &src == &dst;
    for (S32 j = bb.y0; j < bb.y1; ++j)
    {
        for (S32 i = bb.x0; i < bb.x1; ++i)
        {
            const size_t p = (size_t)j * W + i, di = p * 4;
            const F32 dist = L.mSDist[p];
            F32 c = 0.f;
            if (dist <= R)
            {
                c = llmin((R - dist) / fe, 1.f);
                c = c * c * (3.f - 2.f * c) * o;
            }
            if (c <= 0.002f)
            {
                if (!same) { dst[di] = src[di]; dst[di + 1] = src[di + 1]; dst[di + 2] = src[di + 2]; dst[di + 3] = src[di + 3]; }
                continue;
            }
            const F32 old_cov = f16_to_f32(src[di + 3]);
            if (erase)
            {
                if (!same) { dst[di] = src[di]; dst[di + 1] = src[di + 1]; dst[di + 2] = src[di + 2]; }
                dst[di + 3] = f32_to_f16(old_cov * (1.f - c));
                continue;
            }
            if (c + 1.f / 255.f < old_cov)
            {
                if (!same) { dst[di] = src[di]; dst[di + 1] = src[di + 1]; dst[di + 2] = src[di + 2]; dst[di + 3] = src[di + 3]; }
                continue;
            }
            const F32 ph = world ? 0.f : (L.mSAlong[p] / tile) * TAU;
            dst[di]     = f32_to_f16(radius * cosf(ph));
            dst[di + 1] = f32_to_f16(radius * sinf(ph));
            dst[di + 2] = f32_to_f16(world ? 0.f : (R - L.mSLat[p]) / tile);   // left edge of travel 0, right edge w/tile
            dst[di + 3] = f32_to_f16(c);
        }
    }
}

bool WolfTerrainPaint::rasterPolyline(Layer& L, const std::vector<F32>& p, F32 w, Box& out)
{
    bool set = false; Box seg; out = Box();
    F32 cum = 0.f;
    if (p.size() == 2)
    {
        if (rasterSegment(L, p[0], p[1], p[0], p[1], w, 0.f, seg)) unionBox(out, set, seg);
        return set;
    }
    for (size_t k = 2; k + 1 < p.size(); k += 2)
    {
        if (rasterSegment(L, p[k - 2], p[k - 1], p[k], p[k + 1], w, cum, seg)) unionBox(out, set, seg);
        cum += sqrtf((p[k] - p[k - 2]) * (p[k] - p[k - 2]) + (p[k + 1] - p[k - 1]) * (p[k + 1] - p[k - 1]));
    }
    return set;
}

void WolfTerrainPaint::rasterStroke(Layer& L, const Stroke& s)
{
    if (s.mPoints.size() < 2) return;
    ensureScratch(L);
    clearScratch(L);
    Box box;
    if (rasterPolyline(L, s.mPoints, clampWidth(s.mWidth), box)) composite(L, box, s, L.mMap, L.mMap);
}

bool WolfTerrainPaint::layerReady(const Layer& L, LLViewerRegion* regionp) const
{
    // A bake in progress still shows (the map fills in stroke by stroke, as terrain_paint.js
    // uploads its live copy every 150 ms); only the palette and the ground gate it.
    if (!L.mHasPaint || L.mMap.empty()) return false;
    for (S32 i = 0; i < SLOTS; ++i)
    {
        if (!L.mUsed[i]) continue;
        if (L.mPalette[i].isNull() || L.mPalette[i]->isMissingAsset() || !L.mPalette[i]->hasGLTexture()) return false;
    }
    // "Drawn after the normal terrain textures have rezzed": the region's own detail textures
    // must be in (lldrawpoolterrain.cpp binds compp->mDetailTextures[i]).
    LLVLComposition* compp = regionp->getComposition();
    if (compp)
    {
        for (S32 i = 0; i < LLVLComposition::ASSET_COUNT; ++i)
        {
            LLViewerFetchedTexture* d = compp->mDetailTextures[i];
            if (d && !d->hasGLTexture()) return false;
        }
    }
    return true;
}

// Source: wolfwaterfield.cpp upload — a raw GL texture through LLImageGL::setManualImage; RGBA16F
// half floats here, bilinear with no mips (the texels carry an angle, a radius code and a
// coordinate a mip would average).
void WolfTerrainPaint::upload(Layer& L)
{
    if (L.mMap.empty()) return;
    if (!L.mTex) LLImageGL::generateTextures(1, &L.mTex);
    gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, L.mTex);
    LLImageGL::setManualImage(LLTexUnit::getInternalType(LLTexUnit::TT_TEXTURE), 0, GL_RGBA16F,
                              L.mW, L.mH, GL_RGBA, GL_HALF_FLOAT, L.mMap.data(), false);
    gGL.getTexUnit(0)->setTextureFilteringOption(LLTexUnit::TFO_BILINEAR);
    gGL.getTexUnit(0)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    L.mDirtyGL = false;
    L.mLastUpload = LLFrameTimer::getElapsedSeconds();
}

void WolfTerrainPaint::bind(LLGLSLShader* shader, LLViewerRegion* regionp)
{
    static LLStaticHashedString s_on("wolf_paint_on");
    static LLStaticHashedString s_size("wolf_paint_size");
    if (!shader || !regionp) return;
    auto it = mLayers.find(regionp->getHandle());
    Layer* L = it == mLayers.end() ? nullptr : &it->second;
    if (!L || !layerReady(*L, regionp))
    {
        shader->uniform1f(s_on, 0.f);
        return;
    }
    // Upload when dirty; while a bake is still running, at most every 150 ms (a 2048² map is 16 MB).
    if (!L->mTex || (L->mDirtyGL && (!L->mBaking || (F64)LLFrameTimer::getElapsedSeconds() - L->mLastUpload > 0.15))) upload(*L);
    S32 unit = shader->enableTexture(LLShaderMgr::WOLF_PAINT_MAP);
    if (unit > -1) gGL.getTexUnit(unit)->bindManual(LLTexUnit::TT_TEXTURE, L->mTex);
    for (S32 i = 0; i < SLOTS; ++i)
    {
        S32 u = shader->enableTexture((S32)LLShaderMgr::WOLF_PAINT_TEX0 + i);
        if (u < 0) continue;
        LLViewerTexture* tex = (L->mPalette[i].notNull() && L->mPalette[i]->hasGLTexture())
            ? (LLViewerTexture*)L->mPalette[i].get() : (LLViewerTexture*)LLViewerFetchedTexture::sDefaultDiffuseImagep.get();
        gGL.getTexUnit(u)->bind(tex);
        gGL.getTexUnit(u)->setTextureAddressMode(LLTexUnit::TAM_WRAP);
    }
    shader->uniform1f(s_on, 1.f);
    shader->uniform2f(s_size, regionp->getWidth(), regionp->getWidth());
    // World-grid slots: tile size and the region-origin phase of each tile so the texture
    // continues across a region border (lldrawpoolterrain.cpp:262-263, as the detail offset).
    static LLStaticHashedString s_mode("wolf_paint_mode");
    static LLStaticHashedString s_tile("wolf_paint_tile");
    static LLStaticHashedString s_offx("wolf_paint_offx");
    static LLStaticHashedString s_offy("wolf_paint_offy");
    const LLVector3d origin = regionp->getOriginGlobal();
    F32 mode[4], tile[4], offx[4], offy[4];
    for (S32 i = 0; i < SLOTS; ++i)
    {
        const F32 t = llmax(L->mScales[i], 0.01f);
        mode[i] = L->mWorld[i] ? 1.f : 0.f;
        tile[i] = t;
        offx[i] = (F32)fmod(origin.mdV[VX], (F64)t);
        offy[i] = (F32)fmod(origin.mdV[VY], (F64)t);
    }
    shader->uniform4fv(s_mode, 1, mode);
    shader->uniform4fv(s_tile, 1, tile);
    shader->uniform4fv(s_offx, 1, offx);
    shader->uniform4fv(s_offy, 1, offy);
}

void WolfTerrainPaint::unbind(LLGLSLShader* shader)
{
    if (!shader) return;
    shader->disableTexture(LLShaderMgr::WOLF_PAINT_MAP);
    for (S32 i = 0; i < SLOTS; ++i) shader->disableTexture((S32)LLShaderMgr::WOLF_PAINT_TEX0 + i);
}

void WolfTerrainPaint::idle()
{
    if (!WolfGrid::isWolfTerritories()) return;
    LLViewerRegion* agent_rgn = gAgent.getRegion();
    if (!agent_rgn) return;
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (agent_rgn->getHandle() != mFetchedForHandle || now >= mNextRefresh) refresh();

    // Every region in the world with a record gets a layer. The paint MAP rebakes (a slice per
    // frame) only when a texture stroke or a tile changed (texSig); the water planes rebuild
    // when anything changed (key); regions that left the world lose theirs.
    std::set<U64> alive;
    for (LLViewerRegion* rgn : LLWorld::getInstance()->getRegionList())
    {
        if (!rgn || !rgn->isAlive()) continue;
        alive.insert(rgn->getHandle());
        const Slot* textures = nullptr;
        const std::vector<Stroke>* strokes = nullptr;
        std::string key;
        const bool has = layoutFor(rgn->getHandle(), textures, strokes, key);
        if (!has)
        {
            auto it = mLayers.find(rgn->getHandle());
            if (it != mLayers.end() && it->second.mKey != key) { releaseLayer(it->second); it->second.mKey = key; }
            continue;
        }
        Layer& L = layerFor(rgn);
        L.mKey = key;
        const std::string sig = texSig(key, textures, *strokes);
        if (L.mTexSig != sig)
        {
            L.mTexSig = sig;
            ensureBuffers(L, rgn);
            bool scale_changed = false;
            setPalette(L, textures, scale_changed);
            startBake(L, *strokes);
        }
        if (L.mWaterKey != key)
        {
            rebuildWater(L, rgn, *strokes);
            L.mWaterKey = key;
        }
        if (L.mBaking) bakeSlice(L, 0.006);
        // Keep the palette wanted at full detail while it is on the ground.
        for (S32 i = 0; i < SLOTS; ++i)
        {
            if (L.mUsed[i] && L.mPalette[i].notNull()) L.mPalette[i]->addTextureStats(1024.f * 1024.f);
        }
    }
    for (auto it = mLayers.begin(); it != mLayers.end();)
    {
        if (alive.count(it->first) == 0) { releaseLayer(it->second); it = mLayers.erase(it); }
        else ++it;
    }
}

// ── rights (courtesy copy of the service's rule) ───────────────────────────────────────

bool WolfTerrainPaint::canEditAll() const
{
    // Source: llviewerregion.cpp canManageEstate — godlike || estate manager || region owner.
    return gAgent.canManageEstate();
}

// Source: php/terrain_paint.php paint_brush_on_own_land — the square of half-side w/2 around
// the point (centre, four axis points, four corners) must be the agent's own parcel tiles.
bool WolfTerrainPaint::canPaintAt(F32 x, F32 y, F32 w) const
{
    if (canEditAll()) return true;
    LLViewerRegion* rgn = gAgent.getRegion();
    LLViewerParcelOverlay* overlay = rgn ? rgn->getParcelOverlay() : nullptr;
    if (!overlay) return false;
    const F32 h = clampWidth(w) * 0.5f;
    const F32 xs[9] = { x, x - h, x + h, x,     x,     x - h, x + h, x - h, x + h };
    const F32 ys[9] = { y, y,     y,     y - h, y + h, y - h, y - h, y + h, y + h };
    for (S32 i = 0; i < 9; ++i)
    {
        if (xs[i] < 0.f || ys[i] < 0.f || xs[i] >= rgn->getWidth() || ys[i] >= rgn->getWidth()) return false;
        if (!overlay->isOwnedSelf(LLVector3(xs[i], ys[i], 0.f))) return false;
    }
    return true;
}

bool WolfTerrainPaint::canPaintSegment(F32 x0, F32 y0, F32 x1, F32 y1, F32 w) const
{
    if (canEditAll()) return true;
    const F32 len = sqrtf((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
    const S32 steps = llmax(1, (S32)ceilf(len / 2.f));   // half a land tile, as the service walks it
    for (S32 i = 1; i <= steps; ++i)
    {
        const F32 f = (F32)i / (F32)steps;
        if (!canPaintAt(x0 + (x1 - x0) * f, y0 + (y1 - y0) * f, w)) return false;
    }
    return true;
}

// ── editing ────────────────────────────────────────────────────────────────────────────

WolfTerrainPaint::Edit* WolfTerrainPaint::beginEdit()
{
    if (mEdit) return mEdit.get();
    const Record* r = current();
    if (!r) return nullptr;
    std::unique_ptr<Edit> ed(new Edit());
    ed->mHandle = r->mHandle;
    ed->mUuid = r->mUuid;
    ed->mName = r->mName;
    ed->mBaseVersion = r->mVersion;
    for (S32 i = 0; i < SLOTS; ++i) ed->mTextures[i] = r->mTextures[i];
    ed->mStrokes = r->mStrokes;
    for (const Stroke& s : r->mStrokes) ed->mBaseKeys.insert(strokeKey(s));
    ed->mEnabled = r->mEnabled;
    mEdit = std::move(ed);
    return mEdit.get();
}

void WolfTerrainPaint::endEdit()
{
    mLiveStroke = false;
    mEdit.reset();   // idle() sees the key change and rebakes from the stored record
}

void WolfTerrainPaint::editChanged()
{
    if (!mEdit) return;
    mEdit->mDirty = true;
    ++mEdit->mRev;
}

void WolfTerrainPaint::editPaletteChanged()
{
    if (!mEdit) return;
    mEdit->mDirty = true;
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return;
    Layer& L = layerFor(rgn);
    bool scale_changed = false;
    setPalette(L, mEdit->mTextures, scale_changed);
    // The along-phase is baked with the tile: a tile change shows up in texSig (idle rebakes).
    ++mEdit->mRev;
}

bool WolfTerrainPaint::beginStroke(S32 slot, F32 w, F32 o, F32 x, F32 y, F32 level, F32 depth)
{
    Edit* ed = beginEdit();
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!ed || !rgn || !ed->mEnabled) return false;
    if (slot >= 0 && ed->mTextures[slot].mId.isNull()) return false;
    if (slot == -2)
    {
        // Water: collect the points with the ground height + level; the planes appear on release.
        mLiveWater = Stroke();
        mLiveWater.mSlot = -2;
        mLiveWater.mWidth = clampWidth(w);
        mLiveWater.mOpacity = 1.f;
        mLiveWater.mLevel = llclamp(level, 0.f, 10.f);
        mLiveWater.mDepth = llclamp(depth, 0.05f, 20.f);
        mLiveWater.mPoints = { round2(x), round2(y) };
        mLiveWater.mZ = { round2(rgn->getLand().resolveHeightRegion(x, y) + mLiveWater.mLevel) };
        mBandAnchors = { round2(x), round2(y) };
        mBandRaw.clear();
        mBandHasCursor = false;
        mBandTol = llmax(0.3f, clampWidth(w) * 0.12f);
        mLiveIsWater = true;
        mLiveStroke = true;
        return true;
    }
    mLiveIsWater = false;
    mBandAnchors = { round2(x), round2(y) };
    mBandRaw.clear();
    mBandHasCursor = false;
    mBandTol = llmax(0.3f, clampWidth(w) * 0.12f);
    Layer& L = layerFor(rgn);
    ensureBuffers(L, rgn);
    // A bake still in progress: the drag would composite onto a half-built picture, and
    // finishing it here would stall the mouse-down for seconds on a heavily painted region.
    // The tool reports "still loading" (agentLayerBaking) and the user tries again.
    if (L.mBaking) return false;
    bool scale_changed = false;
    setPalette(L, ed->mTextures, scale_changed);
    L.mHasPaint = true;
    if (slot >= 0) L.mUsed[slot] = true;
    ensureScratch(L);
    clearScratch(L);
    L.mLive = true;
    L.mLiveStroke = Stroke();
    L.mLiveStroke.mSlot = slot;
    L.mLiveStroke.mWidth = clampWidth(w);
    L.mLiveStroke.mOpacity = clampOpacity(o);
    L.mLiveStroke.mPoints = { round2(x), round2(y) };
    L.mLiveBoxSet = false;
    // Two buffers, as terrain_paint.js: mLiveBase is the committed picture under the drag; the
    // whole stroke is re-rasterised over it on every update (the rubber-band segments move
    // until a corner is fixed), and an erase never erases twice.
    L.mLiveBase = L.mMap;
    updateLiveStroke(L);
    mLiveStroke = true;
    return true;
}

/** Re-rasterise the live stroke over the committed picture; pixels it no longer reaches go back. */
void WolfTerrainPaint::updateLiveStroke(Layer& L)
{
    if (!L.mLive) return;
    ensureScratch(L);
    clearScratch(L);
    Box box; bool set = false;
    const bool has = rasterPolyline(L, L.mLiveStroke.mPoints, clampWidth(L.mLiveStroke.mWidth), box);
    Box redo; bool redo_set = false;
    if (has) unionBox(redo, redo_set, box);
    if (L.mLiveBoxSet) unionBox(redo, redo_set, L.mLiveBox);
    if (redo_set) composite(L, redo, L.mLiveStroke, L.mLiveBase, L.mMap);
    L.mLiveBox = box; L.mLiveBoxSet = has; (void)set;
    L.mDirtyGL = true;
}

bool WolfTerrainPaint::agentLayerBaking() const
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return false;
    auto it = mLayers.find(rgn->getHandle());
    return it != mLayers.end() && it->second.mBaking;
}

/**
 * The hand moved to (x, y). The stroke stays a straight line from its last corner to the
 * cursor; a corner is fixed at the previous cursor position when the hand's path since the
 * last corner has left that line by more than the tolerance — straight, even segments, not
 * the jitter of the mouse. Source: terrain_paint.js extendStroke — keep in step.
 */
void WolfTerrainPaint::extendStroke(F32 x, F32 y)
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn || !mLiveStroke) return;
    const F32 cx = round2(x), cy = round2(y);
    std::vector<F32>& a = mBandAnchors;
    if (a.size() < 2) return;
    const F32 ax = a[a.size() - 2], ay = a[a.size() - 1];
    F32 max_d = 0.f;
    for (size_t k = 0; k + 1 < mBandRaw.size(); k += 2)
    {
        max_d = llmax(max_d, sqrtf(point_seg_d2(mBandRaw[k], mBandRaw[k + 1], ax, ay, cx, cy)));
    }
    if (max_d > mBandTol && mBandHasCursor)
    {
        a.push_back(mBandCursor[0]);
        a.push_back(mBandCursor[1]);
        mBandRaw.clear();
    }
    mBandRaw.push_back(cx); mBandRaw.push_back(cy);
    mBandCursor[0] = cx; mBandCursor[1] = cy; mBandHasCursor = true;
    std::vector<F32> pts(a);
    pts.push_back(cx); pts.push_back(cy);
    if (mLiveIsWater)
    {
        mLiveWater.mPoints = pts;
        mLiveWater.mZ.clear();
        for (size_t k = 0; k + 1 < pts.size(); k += 2)
            mLiveWater.mZ.push_back(round2(rgn->getLand().resolveHeightRegion(pts[k], pts[k + 1]) + mLiveWater.mLevel));
        return;
    }
    auto it = mLayers.find(rgn->getHandle());
    if (it == mLayers.end() || !it->second.mLive) return;
    Layer& L = it->second;
    L.mLiveStroke.mPoints = pts;
    updateLiveStroke(L);
}

const WolfTerrainPaint::Stroke* WolfTerrainPaint::endStroke()
{
    const bool was_live = mLiveStroke;
    mLiveStroke = false;
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn || !mEdit) return nullptr;
    if (mLiveIsWater)
    {
        mLiveIsWater = false;
        if (!was_live) return nullptr;
        simplifyWater(mLiveWater, WATER_SIMPLIFY_M);
        mEdit->mStrokes.push_back(mLiveWater);
        editChanged();   // idle rebuilds the water planes; the paint map's texSig is unchanged
        return &mEdit->mStrokes.back();
    }
    auto it = mLayers.find(rgn->getHandle());
    if (it == mLayers.end() || !it->second.mLive) return nullptr;
    Layer& L = it->second;
    // Final, clean rasterisation of the stroke as it stands over the committed picture.
    updateLiveStroke(L);
    L.mLive = false;
    L.mLiveBase.clear();
    L.mLiveBase.shrink_to_fit();
    if (L.mLiveStroke.mSlot == -1 && !canEditAll())
    {
        // An erase removes every water stroke it crosses (rebuildWater): a parcel owner may
        // only do that to water wholly on their own land — the service refuses otherwise.
        std::vector<const Stroke*> water;
        for (const Stroke& s : mEdit->mStrokes)
        {
            if (s.mSlot == -2) { water.push_back(&s); continue; }
            if (s.mSlot == -1)
                for (S32 i = (S32)water.size() - 1; i >= 0; --i) if (waterIntersects(*water[i], s)) water.erase(water.begin() + i);
        }
        for (const Stroke* ws : water)
        {
            if (!waterIntersects(*ws, L.mLiveStroke)) continue;
            bool own = canPaintAt(ws->mPoints[0], ws->mPoints[1], ws->mWidth);
            for (size_t k = 2; own && k + 1 < ws->mPoints.size(); k += 2)
                own = canPaintSegment(ws->mPoints[k - 2], ws->mPoints[k - 1], ws->mPoints[k], ws->mPoints[k + 1], ws->mWidth);
            if (!own)
            {
                mLastRefusal = "That erase would remove water on land you do not own — nothing erased.";
                editChanged();   // rebake from the strokes as they are: the drag's pixels go
                return nullptr;
            }
        }
    }
    mEdit->mStrokes.push_back(L.mLiveStroke);
    mEdit->mDirty = true;
    // The layer already holds this stroke: advance the revision and stamp the layer with it
    // so idle() does not rebake the lot.
    ++mEdit->mRev;
    L.mKey = llformat("edit:%u:%d", mEdit->mRev, mEdit->mEnabled ? 1 : 0);
    L.mTexSig = texSig(L.mKey, mEdit->mTextures, mEdit->mStrokes);
    L.mDirtyGL = true;
    // idle() still rebuilds the water planes for the new key (an erase may have crossed one).
    return &mEdit->mStrokes.back();
}

// ── save ───────────────────────────────────────────────────────────────────────────────

void WolfTerrainPaint::save()
{
    if (!mEdit) { notify("Nothing to save."); return; }
    const Record* r = current();
    if (!r || r->mHandle != mEdit->mHandle) { notify("You have left the region this paint belongs to."); return; }
    if (mSaving) return;
    if ((S32)mEdit->mStrokes.size() > MAX_STROKES) { notify(llformat("Too many strokes (%d max) — undo or clear some.", MAX_STROKES)); return; }
    size_t points = 0;
    for (const Stroke& s : mEdit->mStrokes) points += s.mPoints.size() / 2;
    if ((S32)points > MAX_POINTS) { notify(llformat("Too much paint (%d points max) — undo or clear some.", MAX_POINTS)); return; }
    const LLSD layout = editLayoutLLSD();
    mSaving = true;
    const std::string uuid = mEdit->mUuid;
    const bool enabled = mEdit->mEnabled;
    const S32 version = mEdit->mBaseVersion;
    LLCoros::instance().launch("WolfTerrainPaint save", [uuid, layout, enabled, version]()
    {
        WolfTerrainPaint::instance().saveCoro(uuid, layout, enabled, version, false);
    });
}

LLSD WolfTerrainPaint::editLayoutLLSD() const
{
    LLSD layout;
    LLSD tex = LLSD::emptyArray();
    LLSD strokes = LLSD::emptyArray();
    if (mEdit)
    {
        for (S32 i = 0; i < SLOTS; ++i)
        {
            if (mEdit->mTextures[i].mId.isNull()) tex.append(LLSD());
            else tex.append(LLSD().with("id", mEdit->mTextures[i].mId.asString()).with("scale", (F64)mEdit->mTextures[i].mScale).with("mode", mEdit->mTextures[i].mWorld ? "world" : "road"));
        }
        for (const Stroke& s : mEdit->mStrokes) strokes.append(strokeToLLSD(s));
    }
    layout["textures"] = tex;
    layout["strokes"] = strokes;
    return layout;
}

// Source: wolfwavezones.cpp saveCoro — agent + session ids the service verifies against the
// grid's presence service; no secret is carried by this (public) viewer.
void WolfTerrainPaint::saveCoro(std::string region_uuid, LLSD layout, bool enabled, S32 version, bool retried)
{
    LLSD body;
    body["region"] = region_uuid;
    body["layout"] = layout;
    body["enabled"] = enabled;
    body["version"] = version;
    const std::string text = boost::json::serialize(LlsdToJson(body));

    LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
        std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfTerrainPaint", LLCore::HttpRequest::DEFAULT_POLICY_ID);
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
    // 409: someone saved first. Take their record, put THIS session's added strokes on top of
    // it (mergeOnto, the terrain_paint.js rule) and try once more.
    if (!retried && status.isHttpStatus() && status.getType() == 409 && mEdit)
    {
        LL_INFOS("WolfTerrainPaint") << "save: version conflict, refetching, merging and retrying once" << LL_ENDL;
        mFetching = true;
        // fetchCoro drops a non-dirty edit; ours is dirty (it has changes) so it survives.
        fetchCoro(neighbourHandles());
        const Record* r = current();
        if (r && r->mUuid == region_uuid && mEdit)
        {
            mergeOnto(*r);
            // ONE retry, with retried = true: going through save() would start again at
            // retried = false and two editors saving at once could loop for ever.
            saveCoro(region_uuid, editLayoutLLSD(), mEdit->mEnabled, mEdit->mBaseVersion, true);
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
            msg += " (" + std::to_string(reply["refusedCount"].asInteger()) + " refused)";
        }
        mLastError = msg;
        notify("Could not save the ground paint: " + msg);
        return;
    }
    mLastError.clear();
    const LLSD& r = reply["region"];
    Record rec = parseRecord(r);
    if (rec.mHandle) mByHandle[rec.mHandle] = rec;
    mLiveStroke = false;
    mEdit.reset();   // idle() rebakes from the stored record
    notify("Ground paint saved for " + rec.mName + " — everyone in the region now sees it.");
}

// Source: terrain_paint.js _mergeOnto — theirs + our additions − our removals; their palette
// where they set a slot, ours where they left it empty.
void WolfTerrainPaint::mergeOnto(const Record& theirs)
{
    if (!mEdit) return;
    Edit& ed = *mEdit;
    std::set<std::string> our_keys;
    for (const Stroke& s : ed.mStrokes) our_keys.insert(strokeKey(s));
    std::set<std::string> removed_by_us;
    for (const std::string& k : ed.mBaseKeys) if (!our_keys.count(k)) removed_by_us.insert(k);
    std::vector<Stroke> added;
    for (const Stroke& s : ed.mStrokes) if (!ed.mBaseKeys.count(strokeKey(s))) added.push_back(s);
    std::vector<Stroke> merged;
    for (const Stroke& s : theirs.mStrokes) if (!removed_by_us.count(strokeKey(s))) merged.push_back(s);
    for (S32 i = 0; i < SLOTS; ++i)
    {
        if (theirs.mTextures[i].mId.notNull()) ed.mTextures[i] = theirs.mTextures[i];
    }
    const size_t before = merged.size() + added.size();
    for (const Stroke& s : added) merged.push_back(s);
    merged.erase(std::remove_if(merged.begin(), merged.end(), [&](const Stroke& s) { return s.mSlot >= 0 && ed.mTextures[s.mSlot].mId.isNull(); }), merged.end());
    ed.mDropped = (S32)(before - merged.size());
    ed.mStrokes = merged;
    ed.mBaseVersion = theirs.mVersion;
    ed.mBaseKeys.clear();
    for (const Stroke& s : theirs.mStrokes) ed.mBaseKeys.insert(strokeKey(s));
    editChanged();
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// WolfToolTerrainPaint — the brush over the 3-D view
// ═══════════════════════════════════════════════════════════════════════════════════════

WolfToolTerrainPaint::WolfToolTerrainPaint() : LLTool(std::string("TerrainPaint")) {}

F32 WolfToolTerrainPaint::brushWidth() const
{
    static LLCachedControl<F32> width(gSavedSettings, "WolfTerrainPaintBrushWidth", 4.f);
    return WolfTerrainPaint::clampWidth((F32)width);
}

F32 WolfToolTerrainPaint::brushOpacity() const
{
    // The panel's slider is TRANSPARENCY (Paul: "add transparency level"); the stroke stores opacity.
    static LLCachedControl<F32> transparency(gSavedSettings, "WolfTerrainPaintBrushTransparency", 0.f);
    return WolfTerrainPaint::clampOpacity(1.f - llclamp((F32)transparency, 0.f, 90.f) / 100.f);
}

// The ground under the pointer, in the AGENT region's metres (a stroke belongs to one region).
bool WolfToolTerrainPaint::hit(S32 x, S32 y, F32& rx, F32& ry, F32& rz) const
{
    LLVector3d spot;
    if (!gViewerWindow->mousePointOnLandGlobal(x, y, &spot)) return false;
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return false;
    const LLVector3 pos = rgn->getPosRegionFromGlobal(spot);
    if (pos.mV[VX] < 0.f || pos.mV[VY] < 0.f || pos.mV[VX] >= rgn->getWidth() || pos.mV[VY] >= rgn->getWidth()) return false;
    rx = pos.mV[VX]; ry = pos.mV[VY]; rz = pos.mV[VZ];
    return true;
}

bool WolfToolTerrainPaint::handleMouseDown(S32 x, S32 y, MASK mask)
{
    F32 rx, ry, rz;
    if (!hit(x, y, rx, ry, rz)) return false;
    WolfTerrainPaint& tp = WolfTerrainPaint::instance();
    const F32 w = brushWidth();
    if (!tp.canPaintAt(rx, ry, w))
    {
        mMessage = "You can only paint on land you own.";
        return true;
    }
    const S32 slot = mErase ? -1 : mSlot;
    static LLCachedControl<F32> water_level(gSavedSettings, "WolfTerrainPaintWaterLevel", WolfTerrainPaint::WATER_LEVEL_DEFAULT);
    static LLCachedControl<F32> water_depth(gSavedSettings, "WolfTerrainPaintWaterDepth", WolfTerrainPaint::WATER_DEPTH_DEFAULT);
    if (!tp.beginStroke(slot, w, brushOpacity(), rx, ry, (F32)water_level, (F32)water_depth))
    {
        WolfTerrainPaint::Edit* ed = tp.edit();
        if (tp.agentLayerBaking()) mMessage = "The region's paint is still loading — try again in a moment.";
        else if (ed && !ed->mEnabled) mMessage = "Tick \"Use this paint\" before painting.";
        else if (slot >= 0 && ed && ed->mTextures[slot].mId.isNull()) mMessage = llformat("Choose a texture for slot %d first.", slot + 1);
        else mMessage = "Could not start the stroke — the region record is not ready.";
        return true;
    }
    mDragging = true;
    mStoppedOnce = false;
    mLastX = rx; mLastY = ry;
    mMouseX = x; mMouseY = y;
    setMouseCapture(true);
    return true;
}

bool WolfToolTerrainPaint::handleHover(S32 x, S32 y, MASK mask)
{
    mMouseX = x; mMouseY = y;
    mGotHover = true;
    gViewerWindow->setCursor(UI_CURSOR_TOOLLAND);
    if (!mDragging || !hasMouseCapture()) return true;
    F32 rx, ry, rz;
    if (!hit(x, y, rx, ry, rz)) return true;
    const F32 w = brushWidth();
    // Sample the drag no closer than a fraction of the brush (keeps the stroke small).
    const F32 step = llmax(0.25f, w * 0.15f);
    if (sqrtf((rx - mLastX) * (rx - mLastX) + (ry - mLastY) * (ry - mLastY)) < step) return true;
    if (!WolfTerrainPaint::instance().canPaintSegment(mLastX, mLastY, rx, ry, w))
    {
        // The brush stops at the edge of the caller's land: end the stroke here.
        if (!mStoppedOnce) { mStoppedOnce = true; mMessage = "The stroke stopped at land you do not own."; }
        finishStroke();
        return true;
    }
    mLastX = rx; mLastY = ry;
    WolfTerrainPaint::instance().extendStroke(rx, ry);
    return true;
}

void WolfToolTerrainPaint::finishStroke()
{
    if (!mDragging) return;
    mDragging = false;
    const WolfTerrainPaint::Stroke* s = WolfTerrainPaint::instance().endStroke();
    if (!s)
    {
        const std::string refusal = WolfTerrainPaint::instance().takeRefusal();
        if (!refusal.empty()) mMessage = refusal;
    }
    if (s)
    {
        const WolfTerrainPaint::Edit* ed = WolfTerrainPaint::instance().edit();
        mMessage = llformat("%s of %d point%s laid — %d in all. Save to keep it.", s->mSlot == -2 ? "Water" : "Stroke",
                            (S32)(s->mPoints.size() / 2), s->mPoints.size() == 2 ? "" : "s", ed ? (S32)ed->mStrokes.size() : 0);
    }
    if (hasMouseCapture()) setMouseCapture(false);
}

bool WolfToolTerrainPaint::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (!hasMouseCapture()) return false;
    finishStroke();
    return true;
}

void WolfToolTerrainPaint::onMouseCaptureLost()
{
    mDragging = false;
    WolfTerrainPaint::instance().endStroke();
}

bool WolfToolTerrainPaint::handleKey(KEY key, MASK mask)
{
    if (key == KEY_ESCAPE)
    {
        finishStroke();
        LLToolset* ts = LLToolMgr::getInstance()->getCurrentToolset();
        if (ts) ts->selectTool(LLToolCompTranslate::getInstance());
        return true;
    }
    return LLTool::handleKey(key, mask);
}

void WolfToolTerrainPaint::handleSelect()
{
    if (gFloaterTools) gFloaterTools->setStatusText("paintland");
}

void WolfToolTerrainPaint::handleDeselect()
{
    finishStroke();
    mGotHover = false;
}

// A ring on the ground the width of the brush, in the paint colour (Source: lltoolbrush.cpp
// render/renderOverlay for the gGL line-drawing shape; no shader bind of its own).
void WolfToolTerrainPaint::render()
{
    if (!mGotHover) return;
    mGotHover = false;
    F32 rx, ry, rz;
    if (!hit(mMouseX, mMouseY, rx, ry, rz)) return;
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return;
    const LLSurface& land = rgn->getLand();
    const F32 r = brushWidth() * 0.5f;
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    LLGLDepthTest depth(GL_TRUE);
    gGL.color4f(1.f, 0.70f, 0.28f, mDragging ? 1.f : 0.85f);
    gGL.begin(LLRender::LINES);
    const S32 N = 48;
    for (S32 k = 0; k < N; ++k)
    {
        for (S32 e = 0; e < 2; ++e)
        {
            const F32 a = (F32)(k + e) / (F32)N * TAU;
            const F32 px = llclamp(rx + r * cosf(a), 0.f, rgn->getWidth() - 0.01f);
            const F32 py = llclamp(ry + r * sinf(a), 0.f, rgn->getWidth() - 0.01f);
            const F32 pz = land.resolveHeightRegion(px, py) + 0.15f;
            const LLVector3 agent = rgn->getPosAgentFromRegion(LLVector3(px, py, pz));
            gGL.vertex3fv(agent.mV);
        }
    }
    gGL.end();
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// WolfPanelTerrainPaint — Build > Paint
// ═══════════════════════════════════════════════════════════════════════════════════════

WolfPanelTerrainPaint::WolfPanelTerrainPaint() : LLPanel() {}

bool WolfPanelTerrainPaint::postBuild()
{
    mNote    = getChild<LLTextBox>("paint_note");
    mStatus  = getChild<LLTextBox>("paint_status");
    mCount   = getChild<LLTextBox>("paint_count");
    for (S32 i = 0; i < WolfTerrainPaint::SLOTS; ++i)
    {
        mTex[i]       = getChild<LLTextureCtrl>(llformat("paint_tex_%d", i));
        mScale[i]     = getChild<LLSpinCtrl>(llformat("paint_scale_%d", i));
        mUse[i]       = getChild<LLButton>(llformat("paint_use_%d", i));
        mClearSlot[i] = getChild<LLButton>(llformat("paint_clear_%d", i));
        mModeSlot[i]  = getChild<LLButton>(llformat("paint_mode_%d", i));
        mModeSlot[i]->setCommitCallback(boost::bind(&WolfPanelTerrainPaint::onModeSlot, this, i));
        mTex[i]->setAllowNoTexture(true);
        mTex[i]->setCommitCallback(boost::bind(&WolfPanelTerrainPaint::onTexture, this, i));
        mScale[i]->setCommitCallback(boost::bind(&WolfPanelTerrainPaint::onScale, this, i));
        mUse[i]->setCommitCallback(boost::bind(&WolfPanelTerrainPaint::onUseSlot, this, i));
        mClearSlot[i]->setCommitCallback(boost::bind(&WolfPanelTerrainPaint::onClearSlot, this, i));
    }
    mUseWater     = getChild<LLButton>("paint_use_water");
    mWaterLevel   = getChild<LLSpinCtrl>("paint_water_level");
    mWaterDepth   = getChild<LLSpinCtrl>("paint_water_depth");
    mUseWater->setCommitCallback(boost::bind(&WolfPanelTerrainPaint::onUseSlot, this, -2));
    mWaterLevel->setCommitCallback(boost::bind(&WolfPanelTerrainPaint::onWaterParam, this));
    mWaterDepth->setCommitCallback(boost::bind(&WolfPanelTerrainPaint::onWaterParam, this));
    mWaterLevel->set(gSavedSettings.getF32("WolfTerrainPaintWaterLevel"));
    mWaterDepth->set(gSavedSettings.getF32("WolfTerrainPaintWaterDepth"));
    mWidth        = getChild<LLSliderCtrl>("paint_width");
    mTransparency = getChild<LLSliderCtrl>("paint_transparency");
    mErase        = getChild<LLCheckBoxCtrl>("paint_erase");
    mArm          = getChild<LLButton>("paint_arm");
    mEnabled      = getChild<LLCheckBoxCtrl>("paint_enabled");
    mUndo         = getChild<LLButton>("paint_undo");
    mClear        = getChild<LLButton>("paint_clear");
    mSave         = getChild<LLButton>("paint_save");
    mWidth->setCommitCallback(boost::bind(&WolfPanelTerrainPaint::onWidth, this));
    mTransparency->setCommitCallback(boost::bind(&WolfPanelTerrainPaint::onTransparency, this));
    mErase->setCommitCallback(boost::bind(&WolfPanelTerrainPaint::onErase, this));
    mArm->setCommitCallback(boost::bind(&WolfPanelTerrainPaint::onArm, this));
    mEnabled->setCommitCallback(boost::bind(&WolfPanelTerrainPaint::onEnabled, this));
    mUndo->setCommitCallback(boost::bind(&WolfPanelTerrainPaint::onUndo, this));
    mClear->setCommitCallback(boost::bind(&WolfPanelTerrainPaint::onClear, this));
    getChild<LLButton>("paint_revert")->setCommitCallback(boost::bind(&WolfPanelTerrainPaint::onRevert, this));
    mSave->setCommitCallback(boost::bind(&WolfPanelTerrainPaint::onSave, this));
    mWidth->setValue(gSavedSettings.getF32("WolfTerrainPaintBrushWidth"));
    mTransparency->setValue(gSavedSettings.getF32("WolfTerrainPaintBrushTransparency"));
    onUseSlot(0);
    return true;
}

bool WolfPanelTerrainPaint::armed() const
{
    return LLToolMgr::getInstance()->getCurrentTool() == WolfToolTerrainPaint::getInstance();
}

void WolfPanelTerrainPaint::setStatus(const std::string& msg, bool error)
{
    if (!mStatus) return;
    mStatus->setText(msg);
    mStatus->setColor(error ? LLColor4(1.f, 0.54f, 0.54f, 1.f) : LLColor4(0.75f, 0.78f, 0.85f, 1.f));
}

void WolfPanelTerrainPaint::draw()
{
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (now >= mNextPoll)
    {
        mNextPoll = now + 0.25;
        WolfTerrainPaint& tp = WolfTerrainPaint::instance();
        WolfToolTerrainPaint* tool = WolfToolTerrainPaint::getInstance();
        const std::string msg = tool->takeMessage();
        if (!msg.empty()) setStatus(msg, msg.find("Save to keep") == std::string::npos);
        LLViewerRegion* rgn = gAgent.getRegion();
        const U64 handle = rgn ? rgn->getHandle() : 0;
        const WolfTerrainPaint::Record* r = tp.current();
        const WolfTerrainPaint::Edit* ed = tp.edit();
        const bool fetching = tp.fetching();
        const bool is_armed = armed();
        if ((mWasSaving && !tp.saving()) || (r && (handle != mShownHandle || r->mVersion != mShownVersion))
            || (!r && mShownHandle != 0) || fetching != mShownFetching || is_armed != mShownArmed
            || (ed && ed->mRev != mShownRev) || (!ed && mShownRev != 0xffffffff))
        {
            mShownFetching = fetching;
            mShownArmed = is_armed;
            refresh();
        }
    }
    LLPanel::draw();
}

void WolfPanelTerrainPaint::refresh()
{
    if (gDisconnected) return;
    WolfTerrainPaint& tp = WolfTerrainPaint::instance();
    const WolfTerrainPaint::Record* r = tp.current();
    LLViewerRegion* rgn = gAgent.getRegion();
    const U64 handle = rgn ? rgn->getHandle() : 0;
    if (mWasSaving && !tp.saving())
    {
        mWasSaving = false;
        if (tp.lastError().empty()) { setStatus(getString("str_saved"), false); mShownVersion = -1; }
        else setStatus(getString("str_save_failed") + " " + tp.lastError(), true);
    }
    const bool is_armed = armed();
    mArm->setLabel(getString(is_armed ? "str_stop" : "str_arm"));
    mArm->setToggleState(is_armed);
    if (!r)
    {
        if (mNote) mNote->setText(getString(tp.fetching() ? "str_fetching" : "str_off_grid"));
        for (S32 i = 0; i < WolfTerrainPaint::SLOTS; ++i) { mTex[i]->setEnabled(false); mScale[i]->setEnabled(false); mUse[i]->setEnabled(false); mClearSlot[i]->setEnabled(false); mModeSlot[i]->setEnabled(false); }
        mUseWater->setEnabled(false);
        mArm->setEnabled(false); mSave->setEnabled(false); mUndo->setEnabled(false); mClear->setEnabled(false); mEnabled->setEnabled(false);
        if (mCount) mCount->setText(LLStringUtil::null);
        mShownHandle = 0; mShownVersion = -1; mShownRev = 0xffffffff;
        if (!tp.fetching() && tp.fetchedFor() != handle) tp.refresh();
        return;
    }
    if (handle != mShownHandle && !tp.fetching() && tp.lastFetchAgeSecs() > 10.0) tp.refresh();
    rebuild();
    mShownHandle = handle;
    mShownVersion = r->mVersion;
    mShownRev = tp.edit() ? tp.edit()->mRev : 0xffffffff;
}

void WolfPanelTerrainPaint::rebuild()
{
    WolfTerrainPaint& tp = WolfTerrainPaint::instance();
    const WolfTerrainPaint::Record* r = tp.current();
    WolfTerrainPaint::Edit* ed = tp.beginEdit();
    if (!r || !ed) return;
    const bool all = tp.canEditAll();
    LLViewerRegion* rgn = gAgent.getRegion();
    const bool overlay_known = rgn && rgn->getParcelOverlay();
    for (S32 i = 0; i < WolfTerrainPaint::SLOTS; ++i)
    {
        const WolfTerrainPaint::Slot& s = ed->mTextures[i];
        mTex[i]->setEnabled(true);
        mTex[i]->setImageAssetID(s.mId);
        mScale[i]->setEnabled(s.mId.notNull());
        mScale[i]->set(s.mId.notNull() ? s.mScale : 4.f);
        mUse[i]->setEnabled(true);
        S32 used = 0;
        for (const WolfTerrainPaint::Stroke& st : ed->mStrokes) if (st.mSlot == i) ++used;
        mUse[i]->setLabel(used ? llformat("%d (%d)", i + 1, used) : llformat("%d", i + 1));
        mClearSlot[i]->setEnabled(s.mId.notNull());
        mModeSlot[i]->setEnabled(s.mId.notNull());
        mModeSlot[i]->setToggleState(s.mId.notNull() && s.mWorld);
    }
    S32 water = 0;
    for (const WolfTerrainPaint::Stroke& st : ed->mStrokes) if (st.mSlot == -2) ++water;
    mUseWater->setEnabled(true);
    mUseWater->setLabel(water ? llformat("Water (%d)", water) : std::string("Water"));
    mEnabled->set(ed->mEnabled);
    mEnabled->setEnabled(all);
    mArm->setEnabled(ed->mEnabled);
    mUndo->setEnabled(!ed->mStrokes.empty());
    mClear->setEnabled(!ed->mStrokes.empty());
    mSave->setEnabled(!tp.saving());
    if (mCount) mCount->setText(llformat("%d stroke%s%s", (S32)ed->mStrokes.size(), ed->mStrokes.size() == 1 ? "" : "s", ed->mDirty ? " (unsaved)" : ""));
    LLStringUtil::format_map_t args;
    args["[REGION]"] = r->mName;
    args["[STATE]"] = r->mStored ? llformat("saved paint v%d", r->mVersion) : std::string("nothing saved yet");
    mNote->setText(getString(all ? "str_note_all" : (overlay_known ? "str_note_parcel" : "str_note_no_overlay"), args));
}

void WolfPanelTerrainPaint::onTexture(S32 slot)
{
    WolfTerrainPaint::Edit* ed = WolfTerrainPaint::instance().beginEdit();
    if (!ed) return;
    const LLUUID id = mTex[slot]->getImageAssetID();
    if (id.isNull()) { onClearSlot(slot); return; }
    ed->mTextures[slot].mId = id;
    if (ed->mTextures[slot].mScale <= 0.f) ed->mTextures[slot].mScale = 4.f;
    WolfTerrainPaint::instance().editPaletteChanged();
    onUseSlot(slot);
    setStatus(llformat("Texture %d set — loading it for the ground.", slot + 1), false);
    rebuild();
}

void WolfPanelTerrainPaint::onScale(S32 slot)
{
    WolfTerrainPaint::Edit* ed = WolfTerrainPaint::instance().beginEdit();
    if (!ed || ed->mTextures[slot].mId.isNull()) return;
    ed->mTextures[slot].mScale = WolfTerrainPaint::clampScale(mScale[slot]->get());
    mScale[slot]->set(ed->mTextures[slot].mScale);
    WolfTerrainPaint::instance().editPaletteChanged();
    rebuild();
}

void WolfPanelTerrainPaint::onClearSlot(S32 slot)
{
    WolfTerrainPaint::Edit* ed = WolfTerrainPaint::instance().beginEdit();
    if (!ed) return;
    S32 used = 0;
    for (const WolfTerrainPaint::Stroke& st : ed->mStrokes) if (st.mSlot == slot) ++used;
    if (used)
    {
        setStatus(llformat("Texture %d is under %d stroke%s — erase or undo them first.", slot + 1, used, used == 1 ? "" : "s"), true);
        mTex[slot]->setImageAssetID(ed->mTextures[slot].mId);
        return;
    }
    ed->mTextures[slot] = WolfTerrainPaint::Slot();
    WolfTerrainPaint::instance().editPaletteChanged();
    rebuild();
}

// "Grid" toggled: the slot tiles on the world grid (like the ground) instead of following the stroke.
void WolfPanelTerrainPaint::onModeSlot(S32 slot)
{
    WolfTerrainPaint::Edit* ed = WolfTerrainPaint::instance().beginEdit();
    if (!ed || ed->mTextures[slot].mId.isNull()) { if (mModeSlot[slot]) mModeSlot[slot]->setToggleState(false); return; }
    ed->mTextures[slot].mWorld = mModeSlot[slot]->getToggleState();
    WolfTerrainPaint::instance().editPaletteChanged();
    setStatus(llformat("Texture %d now tiles %s.", slot + 1, ed->mTextures[slot].mWorld ? "on the world grid" : "along the stroke"), false);
    rebuild();
}

void WolfPanelTerrainPaint::onUseSlot(S32 slot)
{
    WolfToolTerrainPaint::getInstance()->setSlot(slot);
    for (S32 i = 0; i < WolfTerrainPaint::SLOTS; ++i) if (mUse[i]) mUse[i]->setToggleState(i == slot);
    if (mUseWater) mUseWater->setToggleState(slot == -2);
}

void WolfPanelTerrainPaint::onWaterParam()
{
    gSavedSettings.setF32("WolfTerrainPaintWaterLevel", llclamp(mWaterLevel->get(), 0.f, 10.f));
    gSavedSettings.setF32("WolfTerrainPaintWaterDepth", llclamp(mWaterDepth->get(), 0.05f, 20.f));
}

void WolfPanelTerrainPaint::onWidth()
{
    gSavedSettings.setF32("WolfTerrainPaintBrushWidth", WolfTerrainPaint::clampWidth(mWidth->getValueF32()));
}

void WolfPanelTerrainPaint::onTransparency()
{
    gSavedSettings.setF32("WolfTerrainPaintBrushTransparency", llclamp(mTransparency->getValueF32(), 0.f, 90.f));
}

void WolfPanelTerrainPaint::onErase()
{
    WolfToolTerrainPaint::getInstance()->setErase(mErase->get());
}

void WolfPanelTerrainPaint::onArm()
{
    LLToolset* ts = LLToolMgr::getInstance()->getCurrentToolset();
    if (!ts) return;
    if (armed())
    {
        ts->selectTool(LLToolCompTranslate::getInstance());
        setStatus("", false);
    }
    else
    {
        WolfTerrainPaint::Edit* ed = WolfTerrainPaint::instance().beginEdit();
        if (!ed) { setStatus(getString("str_off_grid"), true); return; }
        if (!ed->mEnabled) { setStatus("Tick \"Use this paint\" before painting.", true); return; }
        ts->selectTool(WolfToolTerrainPaint::getInstance());
        setStatus(getString("str_armed"), false);
    }
    refresh();
}

void WolfPanelTerrainPaint::onEnabled()
{
    WolfTerrainPaint& tp = WolfTerrainPaint::instance();
    WolfTerrainPaint::Edit* ed = tp.beginEdit();
    if (!ed) return;
    ed->mEnabled = mEnabled->get();
    if (!ed->mEnabled && armed()) onArm();
    tp.editChanged();
    setStatus(ed->mEnabled ? "Showing the paint — Save to keep it." : "Paint switched off (kept, not shown) — Save to keep it.", false);
    rebuild();
}

void WolfPanelTerrainPaint::onUndo()
{
    WolfTerrainPaint& tp = WolfTerrainPaint::instance();
    WolfTerrainPaint::Edit* ed = tp.edit();
    if (!ed || ed->mStrokes.empty()) return;
    ed->mStrokes.pop_back();
    tp.editChanged();
    setStatus("Last stroke removed.", false);
    rebuild();
}

void WolfPanelTerrainPaint::onClear()
{
    WolfTerrainPaint& tp = WolfTerrainPaint::instance();
    WolfTerrainPaint::Edit* ed = tp.edit();
    if (!ed || ed->mStrokes.empty()) return;
    const S32 n = (S32)ed->mStrokes.size();
    ed->mStrokes.clear();
    tp.editChanged();
    setStatus(llformat("%d stroke%s cleared — Save to keep it, Revert to get them back.", n, n == 1 ? "" : "s"), false);
    rebuild();
}

void WolfPanelTerrainPaint::onRevert()
{
    if (armed()) onArm();
    WolfTerrainPaint::instance().endEdit();
    mShownVersion = -1;
    refresh();
    setStatus("Reverted to the saved paint.", false);
}

void WolfPanelTerrainPaint::onSave()
{
    if (armed()) onArm();
    WolfTerrainPaint& tp = WolfTerrainPaint::instance();
    setStatus(getString("str_saving"), false);
    mWasSaving = true;
    mSave->setEnabled(false);
    tp.save();
}
