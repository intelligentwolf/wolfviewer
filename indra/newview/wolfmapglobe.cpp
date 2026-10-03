/**
 * @file wolfmapglobe.cpp
 * @brief World map on Wolf Territories: animated water and the globe (see wolfmapglobe.h).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Copyright (C) 2026, IntelligentWolf Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "wolfmapglobe.h"

#include "indra_constants.h"
#include "llagent.h"
#include "llglstates.h"
#include "llimage.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "lluiimage.h"
#include "llviewercontrol.h"
#include "llviewertexture.h"
#include "llworldmap.h"
#include "llworldmipmap.h"
#include "wolfgrid.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "llhttpconstants.h"
#include "llsdjson.h"
#include <boost/json.hpp>

#include <cmath>

namespace
{
    // Source: wolfstorm/php/map_extent.php — the whole map's box (non-cancelled grid.regions).
    const char* EXTENT_URL = "https://wolfstorm.app/php/map_extent.php";
    // The service caches its answer for ten minutes (Cache-Control max-age=600); ask as often.
    constexpr F64 EXTENT_REFRESH_S = 600.0;
    // After a failed read, try again after this long.
    constexpr F64 EXTENT_RETRY_S = 60.0;

    // The globe's size on screen, in halves of the view's shorter side: at and below GLOBE_FULL it
    // is the whole globe, at and above GLOBE_FLAT the flat map; in between it bends. At 6 the
    // view sees ~1/12 of the globe's width, which is nearly flat already. The whole globe is
    // shown at 0.84 (the slider's limit, globeZoom), leaving room for the atmosphere.
    constexpr F32 GLOBE_FULL = 1.0f;
    constexpr F32 GLOBE_FLAT = 6.0f;
    constexpr F32 GLOBE_FIT = 0.84f;
    // Double-click on the globe: fly in to twice the flat map's threshold.
    constexpr F32 FLY_IN_TIMES_FLAT = 2.0f;

    // Colours made see-through in the tiles (alpha 0 within KEY_NEAR, solid from KEY_FAR).
    struct KeyColour { F32 r, g, b; };
    constexpr KeyColour KEY_COLOURS[] = {
        { 0x11, 0x2D, 0x54 },   // MapColorWater "#112D54", the grid's region water (regions3 inis, 10-02)
        { 29, 72, 96 },         // MapImageService.cs:64 m_Watercolor, empty space in zoomed-out tiles
        { 0x1D, 0x47, 0x5F },   // MapColorWater default "#1D475F" (ShadedMapTileRenderer.cs:54)
    };
    // In RGB units. JPEG noise on flat water measured within ~3 (ocean tile 5040,4960: 70% of
    // pixels exactly #112D54, the rest of the water within 2-3); land is far further off.
    constexpr F32 KEY_NEAR = 10.f;
    constexpr F32 KEY_FAR = 26.f;
    // Tiles made see-through per frame (65,536 pixels each), so a zoom-out does not stall.
    constexpr S32 KEYED_PER_FRAME = 6;
    // A tile whose pixels were never kept (it loaded before the map was on this grid) is drawn
    // as it is after this long, rather than not at all.
    constexpr F64 KEYED_GIVE_UP_S = 3.0;

    // Water: on screen about this many pixels per repeat, at any zoom (two octaves cross-faded).
    constexpr F64 WATER_PERIOD_PX = 256.0;
    constexpr F32 WATER_FPS = 8.f;

    // The mean of the water frames, which is #112D54 (make_water.py), for the flat background
    // the globe's space fades from.
    const LLColor4 WATER_MEAN(0x11 / 255.f, 0x2D / 255.f, 0x54 / 255.f, 1.f);
    const LLColor4 SPACE(0.010f, 0.015f, 0.040f, 1.f);
    const LLColor4 ATMOSPHERE(0.45f, 0.70f, 1.00f, 1.f);

    F32 smooth(F32 u)
    {
        u = llclamp(u, 0.f, 1.f);
        return u * u * (3.f - 2.f * u);
    }

    // Projected distance from the view centre, in pixels, of a point s metres away.
    F64 projectedRadius(F64 s, F64 k, F64 t, F64 radius)
    {
        return k * ((1.0 - t) * s + t * radius * sin(s / radius));
    }

    // How far from the centre (metres) the visible part reaches: the front hemisphere at most
    // while there is any globe, and no further than covers the view's corners.
    F64 visibleReach(const WolfMapGlobe::View& v)
    {
        const F64 k = v.ppr / 256.0;
        const F64 half_diag = 0.5 * sqrt((F64)v.width * v.width + (F64)v.height * v.height) + 8.0;
        if (v.t <= 0.f)
        {
            return half_diag / k;
        }
        const F64 s_max = F_PI_BY_TWO * v.radius;
        if (projectedRadius(s_max, k, v.t, v.radius) <= half_diag)
        {
            return s_max;
        }
        F64 lo = 0.0, hi = s_max;
        for (int i = 0; i < 40; ++i)
        {
            const F64 mid = 0.5 * (lo + hi);
            (projectedRadius(mid, k, v.t, v.radius) < half_diag ? lo : hi) = mid;
        }
        return hi;
    }

    U64 tileKey(S32 level, U32 x, U32 y)
    {
        return ((U64)(level & 0xff) << 56) | ((U64)(x & 0x0fffffff) << 28) | (U64)(y & 0x0fffffff);
    }

    // Source: wolfmapoverlays.cpp json_to_llsd — the service answers JSON, not LLSD.
    LLSD json_to_llsd(const LLSD::Binary& bytes)
    {
        std::string text(bytes.begin(), bytes.end());
        boost::system::error_code ec;
        boost::json::value v = boost::json::parse(text, ec);
        if (ec) return LLSD();
        return LlsdFromJson(v);
    }
}

bool WolfMapGlobe::active() const
{
    static LLCachedControl<bool> on(gSavedSettings, "WolfMapGlobe", true);
    return on && WolfGrid::isWolfTerritories();
}

// ── the map's box ────────────────────────────────────────────────────────────────────────

void WolfMapGlobe::setViewSize(S32 width, S32 height)
{
    if (width > 0 && height > 0)
    {
        mViewWidth = width;
        mViewHeight = height;
    }
    if (!active() || mFetching)
    {
        return;
    }
    const F64 now = LLTimer::getElapsedSeconds();
    if (now - mFetchedAt < (mHaveExtent ? EXTENT_REFRESH_S : EXTENT_RETRY_S))
    {
        return;
    }
    mFetching = true;
    LLCoros::instance().launch("WolfMapGlobe extent", []()
    {
        WolfMapGlobe::instance().fetchExtentCoro();
    });
}

// Source: wolfmapoverlays.cpp fetchCoro — the same adapter shape (getRawAndSuspend, JSON body).
void WolfMapGlobe::fetchExtentCoro()
{
    LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
        std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfMapGlobe", LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
    LLCore::HttpOptions::ptr_t options = WolfGrid::makeVerifiedHttpOptions();
    options->setTimeout(20);
    LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
    headers->append(HTTP_OUT_HEADER_ACCEPT, "application/json");

    LLSD result = adapter->getRawAndSuspend(request, EXTENT_URL, options, headers);
    LLSD httpResults = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(httpResults);
    mFetching = false;
    mFetchedAt = LLTimer::getElapsedSeconds();
    if (!status)
    {
        LL_WARNS("WolfMapGlobe") << "map box read failed: " << status.toString() << LL_ENDL;
        return;
    }
    LLSD reply;
    if (result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
    {
        reply = json_to_llsd(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary());
    }
    if (!reply.isMap() || !reply["success"].asBoolean())
    {
        LL_WARNS("WolfMapGlobe") << "map box read: unexpected reply" << LL_ENDL;
        return;
    }
    const F64 x0 = reply["x0"].asReal(), y0 = reply["y0"].asReal(), x1 = reply["x1"].asReal(), y1 = reply["y1"].asReal();
    // A box the map could be in: OpenSim's world is 65536 regions of 256 m a side
    // (map_overlays.php OVL_MAX_COORD), and at least one region big.
    constexpr F64 WORLD_M = 16777216.0;
    if (!(x0 >= 0.0 && y0 >= 0.0 && x1 <= WORLD_M && y1 <= WORLD_M &&
          x1 - x0 >= REGION_WIDTH_METERS && y1 - y0 >= REGION_WIDTH_METERS))
    {
        LL_WARNS("WolfMapGlobe") << "map box read: box out of range " << x0 << "," << y0 << " " << x1 << "," << y1 << LL_ENDL;
        return;
    }
    if (!mHaveExtent || x0 != mX0 || y0 != mY0 || x1 != mX1 || y1 != mY1)
    {
        mX0 = x0; mY0 = y0; mX1 = x1; mY1 = y1;
        mRadius = llmax(x1 - x0, y1 - y0) / F_PI;
        mHaveExtent = true;
        ++mExtentGen;
        LL_INFOS("WolfMapGlobe") << "map " << (x1 - x0) / 1000.0 << " x " << (y1 - y0) / 1000.0
                                 << " km (" << reply["regions"].asInteger() << " regions), globe radius "
                                 << mRadius / 1000.0 << " km" << LL_ENDL;
    }
}

// ── zoom ─────────────────────────────────────────────────────────────────────────────────

F32 WolfMapGlobe::pprForScreenRadius(F32 half_views) const
{
    // The globe's radius on screen is ppr / 256 * R pixels.
    const F64 half_view = 0.5 * llmax(1, llmin(mViewWidth, mViewHeight));
    return (F32)(half_views * half_view * 256.0 / mRadius);
}

F32 WolfMapGlobe::amount(F32 ppr) const
{
    if (!active() || !mHaveExtent)
    {
        return 0.f;
    }
    const F32 full = pprForScreenRadius(GLOBE_FULL), flat = pprForScreenRadius(GLOBE_FLAT);
    if (ppr >= flat)
    {
        return 0.f;
    }
    if (ppr <= full)
    {
        return 1.f;
    }
    return smooth((log2f(flat) - log2f(ppr)) / (log2f(flat) - log2f(full)));
}

F32 WolfMapGlobe::globeZoom(S32 width, S32 height) const
{
    // LLWorldMapView::zoomFromScale(ppr) = log2(ppr / 256); the globe's radius on screen is
    // GLOBE_FIT halves of the shorter side.
    const F64 screen_radius = GLOBE_FIT * 0.5 * llmax(1, llmin(width, height));
    return (F32)log2(screen_radius / mRadius);
}

F32 WolfMapGlobe::flyInZoom() const
{
    return log2f(FLY_IN_TIMES_FLAT * pprForScreenRadius(GLOBE_FLAT) / 256.f);
}

F32 WolfMapGlobe::regionZoom(F32 size_x_m, F32 size_y_m, S32 width, S32 height) const
{
    // Paul 10-02: "when i search for a region it should zoom down to that region".
    const F32 regions_across = llmax(size_x_m, size_y_m, (F32)REGION_WIDTH_METERS) / REGION_WIDTH_METERS;
    F32 ppr = 0.6f * (F32)llmax(1, llmin(width, height)) / regions_across;
    if (mHaveExtent)
    {
        ppr = llmax(ppr, pprForScreenRadius(GLOBE_FLAT));
    }
    return log2f(ppr / 256.f);
}

LLVector3d WolfMapGlobe::clampCentre(const LLVector3d& c) const
{
    LLVector3d out = c;
    if (mHaveExtent)
    {
        out.mdV[VX] = llclamp(out.mdV[VX], mX0, mX1);
        out.mdV[VY] = llclamp(out.mdV[VY], mY0, mY1);
    }
    return out;
}

// ── projection ───────────────────────────────────────────────────────────────────────────

// static
bool WolfMapGlobe::project(const View& v, const LLVector3d& global, F32& x, F32& y, F32* shade)
{
    const F64 dx = global.mdV[VX] - v.centre.mdV[VX];
    const F64 dy = global.mdV[VY] - v.centre.mdV[VY];
    const F64 k = v.ppr / 256.0;
    const F64 s = sqrt(dx * dx + dy * dy);
    const F64 a = s / v.radius;
    if (v.t > 0.f && a > F_PI_BY_TWO)
    {
        return false;
    }
    const F64 f = (s < 1e-3) ? 1.0 : (1.0 - v.t) + v.t * v.radius * sin(a) / s;
    x = (F32)(v.width * 0.5 + dx * k * f);
    y = (F32)(v.height * 0.5 + dy * k * f);
    if (shade)
    {
        // Lit from the viewer: full at the centre, darker towards the rim.
        *shade = 1.f - v.t * (1.f - (F32)(0.35 + 0.65 * cos(a)));
    }
    return true;
}

// static
bool WolfMapGlobe::unproject(const View& v, F32 x, F32 y, LLVector3d& global)
{
    const F64 ox = x - v.width * 0.5, oy = y - v.height * 0.5;
    const F64 r = sqrt(ox * ox + oy * oy);
    const F64 k = v.ppr / 256.0;
    global = v.centre;
    if (r < 1e-3)
    {
        return true;
    }
    F64 s;
    if (v.t <= 0.f)
    {
        s = r / k;
    }
    else
    {
        const F64 s_max = F_PI_BY_TWO * v.radius;
        if (r > projectedRadius(s_max, k, v.t, v.radius))
        {
            return false;   // off the edge of the globe, out in space
        }
        F64 lo = 0.0, hi = s_max;   // projectedRadius rises over [0, s_max]
        for (int i = 0; i < 50; ++i)
        {
            const F64 mid = 0.5 * (lo + hi);
            (projectedRadius(mid, k, v.t, v.radius) < r ? lo : hi) = mid;
        }
        s = 0.5 * (lo + hi);
    }
    global.mdV[VX] += ox / r * s;
    global.mdV[VY] += oy / r * s;
    return true;
}

// ── see-through tiles ────────────────────────────────────────────────────────────────────

void WolfMapGlobe::prepareTile(LLViewerFetchedTexture* tile) const
{
    if (tile && active())
    {
        tile->forceToSaveRawImage(0);
    }
}

LLViewerTexture* WolfMapGlobe::keyedTile(LLViewerFetchedTexture* tile, S32 level, U32 grid_x, U32 grid_y)
{
    if (!tile)
    {
        return nullptr;
    }
    const F64 now = LLTimer::getElapsedSeconds();
    if (mKeyedFrame != LLFrameTimer::getFrameCount())
    {
        mKeyedFrame = LLFrameTimer::getFrameCount();
        mKeyedThisFrame = 0;
        // Now and then, forget tiles not drawn for two minutes.
        if (mKeyedFrame % 600 == 0)
        {
            for (auto it = mKeyed.begin(); it != mKeyed.end();)
            {
                it = (now - it->second.last_used > 120.0) ? mKeyed.erase(it) : std::next(it);
            }
        }
    }

    Keyed& e = mKeyed[tileKey(level, grid_x, grid_y)];
    if (e.source.get() != tile)
    {
        e = Keyed();
        e.source = tile;
        e.first_seen = now;
    }
    e.last_used = now;

    const S32 saved_level = tile->hasSavedRawImage() ? tile->getSavedRawImageLevel() : -1;
    if (saved_level >= 0 && (e.texture.isNull() || saved_level < e.raw_level) && mKeyedThisFrame < KEYED_PER_FRAME)
    {
        LLPointer<LLImageRaw> raw = tile->getSavedRawImage();
        if (raw.notNull())
        {
            LLImageDataSharedLock lock(raw);
            const S32 w = raw->getWidth(), h = raw->getHeight(), c = raw->getComponents();
            const U8* in = raw->getData();
            if (in && w > 0 && h > 0 && c >= 3)
            {
                LLPointer<LLImageRaw> out = new LLImageRaw((U16)w, (U16)h, 4);
                U8* o = out->getData();
                if (o)
                {
                    for (S32 i = 0, n = w * h; i < n; ++i, in += c, o += 4)
                    {
                        F32 nearest = F32_MAX;
                        for (const KeyColour& kc : KEY_COLOURS)
                        {
                            const F32 dr = in[0] - kc.r, dg = in[1] - kc.g, db = in[2] - kc.b;
                            nearest = llmin(nearest, dr * dr + dg * dg + db * db);
                        }
                        const F32 d = sqrtf(nearest);
                        o[0] = in[0];
                        o[1] = in[1];
                        o[2] = in[2];
                        o[3] = (U8)ll_round(255.f * llclamp((d - KEY_NEAR) / (KEY_FAR - KEY_NEAR), 0.f, 1.f));
                    }
                    e.texture = LLViewerTextureManager::getLocalTexture(out.get(), false);
                    e.raw_level = saved_level;
                    ++mKeyedThisFrame;
                }
            }
        }
        // Full resolution made: the decoded copy is no longer needed.
        if (saved_level == 0 && e.texture.notNull())
        {
            tile->destroySavedRawImage();
        }
    }
    if (e.texture.notNull())
    {
        return e.texture.get();
    }
    // Pixels never kept (loaded before this grid's map was on): ask again, and after a while show
    // the tile as it is.
    if (saved_level < 0)
    {
        prepareTile(tile);
        if (tile->hasGLTexture() && now - e.first_seen > KEYED_GIVE_UP_S)
        {
            return tile;
        }
    }
    return nullptr;
}

// ── water ────────────────────────────────────────────────────────────────────────────────

void WolfMapGlobe::loadWater()
{
    if (mWaterLoaded)
    {
        return;
    }
    mWaterLoaded = true;
    for (S32 i = 0; i < WATER_FRAMES; ++i)
    {
        // skins/default/textures/wolfmap/water_NN.png (make_water.py)
        mWater[i] = LLViewerTextureManager::getFetchedTextureFromFile(llformat("wolfmap/water_%02d.png", i),
                                                                      FTT_LOCAL_FILE, true, LLGLTexture::BOOST_UI);
    }
}

// Draws the water as four layers over a mesh: two sizes (so the ripples stay ~256 px whatever the
// zoom, the next size up fading in as you zoom out) times two frames (cross-faded, so the loop
// runs smoothly at 8 frames a second). The vertices are given by fill(), which emits triangles of
// (global point, screen x, screen y, shade).
template <typename Fill>
static void drawWaterLayers(const WolfMapGlobe::View& v, LLPointer<LLViewerFetchedTexture>* frames, S32 frame_count, Fill fill)
{
    const F64 k = v.ppr / 256.0;
    const F64 octave = log2(WATER_PERIOD_PX / k);
    const F64 o0 = floor(octave);
    const F32 fine_weight = 1.f - (F32)(octave - o0);     // the smaller ripples, fading out
    const F64 periods[2] = { exp2(o0 + 1.0), exp2(o0) };  // coarse first (opaque), fine on top
    const F32 weights[2] = { 1.f, fine_weight };

    const F64 tf = LLTimer::getElapsedSeconds() * WATER_FPS;
    const S32 base_frame = (S32)fmod(floor(tf), (F64)frame_count);
    const F32 fade = (F32)(tf - floor(tf));

    for (S32 layer = 0; layer < 2; ++layer)
    {
        if (weights[layer] <= 0.01f)
        {
            continue;
        }
        const F64 period = periods[layer];
        // The two sizes run half a loop apart, so they never move in step.
        const S32 frame_a = (base_frame + layer * (frame_count / 2)) % frame_count;
        const S32 frame_b = (frame_a + 1) % frame_count;
        // Keep texture coordinates small: whole repeats of the centre are dropped.
        const F64 u0 = fmod(v.centre.mdV[VX] / period, 1.0);
        const F64 v0 = fmod(v.centre.mdV[VY] / period, 1.0);
        for (S32 pass = 0; pass < 2; ++pass)
        {
            LLViewerFetchedTexture* tex = frames[pass ? frame_b : frame_a].get();
            if (!tex || !tex->hasGLTexture())
            {
                continue;
            }
            const F32 alpha = weights[layer] * (pass ? fade : 1.f);
            if (alpha <= 0.01f)
            {
                continue;
            }
            tex->setAddressMode(LLTexUnit::TAM_WRAP);
            gGL.getTexUnit(0)->bind(tex);
            gGL.begin(LLRender::TRIANGLES);
            fill([&](const LLVector3d& g, F32 x, F32 y, F32 shade)
            {
                gGL.color4f(shade, shade, shade, alpha);
                gGL.texCoord2f((F32)(u0 + (g.mdV[VX] - v.centre.mdV[VX]) / period),
                               (F32)(v0 + (g.mdV[VY] - v.centre.mdV[VY]) / period));
                gGL.vertex2f(x, y);
            });
            gGL.end();
        }
    }
}

void WolfMapGlobe::drawFlatWater(const View& v)
{
    loadWater();
    LLGLSUIDefault gls_ui;
    // Under the animation (and while its frames load): the water's own colour.
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.color4fv(WATER_MEAN.mV);
    gl_rect_2d(0, (S32)v.height, (S32)v.width, 0);

    const F64 k = v.ppr / 256.0;
    auto corner = [&](F32 x, F32 y)
    {
        LLVector3d g = v.centre;
        g.mdV[VX] += (x - v.width * 0.5) / k;
        g.mdV[VY] += (y - v.height * 0.5) / k;
        return g;
    };
    const LLVector3d bl = corner(0.f, 0.f), br = corner(v.width, 0.f), tl = corner(0.f, v.height), tr = corner(v.width, v.height);
    drawWaterLayers(v, mWater, WATER_FRAMES, [&](auto&& emit)
    {
        emit(tl, 0.f, v.height, 1.f);
        emit(bl, 0.f, 0.f, 1.f);
        emit(br, v.width, 0.f, 1.f);
        emit(tl, 0.f, v.height, 1.f);
        emit(br, v.width, 0.f, 1.f);
        emit(tr, v.width, v.height, 1.f);
    });
}

// The water over the globe: rings round the centre out to the visible reach, each vertex put
// where project() puts its global point.
void WolfMapGlobe::drawWaterMesh(const View& v)
{
    constexpr S32 RINGS = 24, SEGMENTS = 64;
    const F64 reach = visibleReach(v);
    struct P { LLVector3d g; F32 x, y, shade; bool ok; };
    std::vector<P> grid((RINGS + 1) * (SEGMENTS + 1));
    for (S32 i = 0; i <= RINGS; ++i)
    {
        const F64 s = reach * i / RINGS;
        for (S32 j = 0; j <= SEGMENTS; ++j)
        {
            const F64 th = F_TWO_PI * j / SEGMENTS;
            P& p = grid[i * (SEGMENTS + 1) + j];
            p.g = v.centre;
            p.g.mdV[VX] += s * cos(th);
            p.g.mdV[VY] += s * sin(th);
            p.ok = project(v, p.g, p.x, p.y, &p.shade);
        }
    }
    auto fill = [&](auto&& emit)
    {
        for (S32 i = 0; i < RINGS; ++i)
        {
            for (S32 j = 0; j < SEGMENTS; ++j)
            {
                const P& a = grid[i * (SEGMENTS + 1) + j];
                const P& b = grid[i * (SEGMENTS + 1) + j + 1];
                const P& c = grid[(i + 1) * (SEGMENTS + 1) + j];
                const P& d = grid[(i + 1) * (SEGMENTS + 1) + j + 1];
                if (!(a.ok && b.ok && c.ok && d.ok))
                {
                    continue;
                }
                emit(a.g, a.x, a.y, a.shade);
                emit(c.g, c.x, c.y, c.shade);
                emit(d.g, d.x, d.y, d.shade);
                emit(a.g, a.x, a.y, a.shade);
                emit(d.g, d.x, d.y, d.shade);
                emit(b.g, b.x, b.y, b.shade);
            }
        }
    };
    // Paul 10-02: "the map should start covered in water till the tiles appear". The globe is
    // water-coloured, lit the same, before the animation's frames have loaded.
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.begin(LLRender::TRIANGLES);
    fill([&](const LLVector3d&, F32 x, F32 y, F32 shade)
    {
        gGL.color4f(WATER_MEAN.mV[VRED] * shade, WATER_MEAN.mV[VGREEN] * shade, WATER_MEAN.mV[VBLUE] * shade, 1.f);
        gGL.vertex2f(x, y);
    });
    gGL.end();
    drawWaterLayers(v, mWater, WATER_FRAMES, fill);
}

// ── space (2026-10-03) ───────────────────────────────────────────────────────────────────

// The sky, made once from a fixed seed so it is the same sky every time, and the same sky as
// WolfStorm's. Source: wolfstorm js/world/map_globe.js MapGlobe._makeSky — ported call for call
// (the random numbers are drawn in the same order, so the stars land in the same places):
// stars with real-star brightness falloff and colours, a few bright ones with a halo, the Milky
// Way as faint stars and soft glow with a dark dust lane, and three faint nebulae.
void WolfMapGlobe::makeSky()
{
    mSkyMade = true;
    mSkyAdd.clear();
    mSkyOver.clear();

    // mulberry32, as JavaScript computes it with Math.imul and >>> (32-bit wrap-around).
    U32 seed = 0x5751F00Du;
    auto rnd = [&seed]() -> F64
    {
        seed += 0x6D2B79F5u;
        U32 t = seed;
        t = (t ^ (t >> 15)) * (t | 1u);
        t ^= t + (t ^ (t >> 7)) * (t | 61u);
        return (F64)(t ^ (t >> 14)) / 4294967296.0;
    };
    auto gauss = [&rnd]() -> F64
    {
        F64 u = 0.0;
        while (u == 0.0)
        {
            u = rnd();
        }
        const F64 r = sqrt(-2.0 * log(u));
        return r * cos(2.0 * F_PI * rnd());
    };
    auto onSphere = [&rnd](F64 out[3])
    {
        const F64 z = rnd() * 2.0 - 1.0;
        const F64 a = rnd() * 2.0 * F_PI;
        const F64 r = sqrt(1.0 - z * z);
        out[0] = r * cos(a);
        out[1] = r * sin(a);
        out[2] = z;
    };
    // Star colours by spectral class (approximate blackbody tints) and how common each is among
    // stars bright enough to see.
    struct StarClass { F64 weight; F32 c[3]; };
    static const StarClass CLASSES[] = {
        { 0.10, { 0.66f, 0.76f, 1.00f } },   // O/B blue-white
        { 0.22, { 0.86f, 0.90f, 1.00f } },   // A white
        { 0.33, { 1.00f, 0.97f, 0.90f } },   // F/G yellow-white
        { 0.23, { 1.00f, 0.84f, 0.64f } },   // K orange
        { 0.12, { 1.00f, 0.70f, 0.52f } },   // M orange-red
    };
    auto colour = [&rnd]() -> const F32*
    {
        F64 r = rnd();
        for (const StarClass& sc : CLASSES)
        {
            if ((r -= sc.weight) <= 0.0)
            {
                return sc.c;
            }
        }
        return CLASSES[2].c;
    };
    auto push = [](std::vector<SkyPoint>& list, const F64 d[3], F64 size, const F32 c[3], F64 a)
    {
        SkyPoint p;
        for (int i = 0; i < 3; ++i)
        {
            p.d[i] = (F32)d[i];
            p.c[i] = c[i];
        }
        p.size = (F32)size;
        p.a = (F32)a;
        list.push_back(p);
    };

    // Field stars: many faint, few bright (brightness ~ u^3).
    for (int i = 0; i < 2400; ++i)
    {
        const F64 b = pow(rnd(), 3.2);
        F64 d[3];
        onSphere(d);
        const F32* c = colour();
        push(mSkyAdd, d, 1.2 + 2.6 * b, c, 0.25 + 0.75 * llmin(1.0, b * 1.6));
    }
    // A handful of bright ones with a soft halo.
    for (int i = 0; i < 14; ++i)
    {
        F64 d[3];
        onSphere(d);
        const F32* c = colour();
        const F64 core_size = 3.6 + 1.6 * rnd();
        push(mSkyAdd, d, core_size, c, 1.0);
        const F64 halo_size = 10.0 + 6.0 * rnd();
        push(mSkyAdd, d, halo_size, c, 0.16);
    }

    // The Milky Way: a great circle tilted across the sky, densest and brightest towards one side
    // (the galactic centre). Aimed so the band crosses the view at the map's middle (the view
    // looks along +z; band(-pi/2, 0) is nearly +z for this tilt), the core just off to one side.
    F64 n[3] = { 0.6, -0.78, 0.15 };
    {
        const F64 l = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        for (F64& x : n) x /= l;
    }
    F64 e1[3] = { n[1], -n[0], 0.0 };
    {
        const F64 l = sqrt(e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2]);
        for (F64& x : e1) x /= l;
    }
    const F64 e2[3] = { n[1] * e1[2] - n[2] * e1[1], n[2] * e1[0] - n[0] * e1[2], n[0] * e1[1] - n[1] * e1[0] };
    auto band = [&](F64 lon, F64 lat, F64 out[3])
    {
        const F64 cl = cos(lat);
        for (int i = 0; i < 3; ++i)
        {
            out[i] = cl * (cos(lon) * e1[i] + sin(lon) * e2[i]) + sin(lat) * n[i];
        }
    };
    const F64 core_lon = -1.1;
    auto near_core = [core_lon](F64 lon)
    {
        return fabs(fmod(fmod(lon - core_lon, 2.0 * F_PI) + 3.0 * F_PI, 2.0 * F_PI) - F_PI);
    };
    static const F32 BAND_COOL[3] = { 0.95f, 0.93f, 1.0f };
    static const F32 BAND_WARM[3] = { 1.0f, 0.92f, 0.80f };
    for (int i = 0; i < 9000; ++i)
    {
        const F64 lon = rnd() * 2.0 * F_PI;
        const F64 core = exp(-pow(near_core(lon) / 0.9, 2.0));
        if (rnd() > 0.35 + 0.65 * core)
        {
            continue;
        }
        const F64 lat = gauss() * (0.07 + 0.08 * core);
        F64 d[3];
        band(lon, lat, d);
        const F64 size = 1.0 + 0.9 * rnd();
        const F32* c = rnd() < 0.5 ? BAND_COOL : BAND_WARM;
        const F64 a = 0.18 + 0.30 * rnd() * (0.5 + core);
        push(mSkyAdd, d, size, c, a);
    }
    // Its glow: soft, very faint, large.
    for (int i = 0; i < 1100; ++i)
    {
        const F64 lon = rnd() * 2.0 * F_PI;
        const F64 core = exp(-pow(near_core(lon) / 0.9, 2.0));
        const F64 lat = gauss() * (0.05 + 0.06 * core);
        const F64 warm = core * 0.5 + rnd() * 0.3;
        F64 d[3];
        band(lon, lat, d);
        const F64 size = 40.0 + 70.0 * rnd() + 60.0 * core;
        const F32 c[3] = { (F32)(0.78 + 0.22 * warm), (F32)(0.80 + 0.10 * warm), (F32)(1.0 - 0.18 * warm) };
        push(mSkyAdd, d, size, c, 0.022 + 0.04 * core);
    }
    // The dark dust lane down the bright middle: drawn over the glow in the space colour.
    for (int i = 0; i < 160; ++i)
    {
        const F64 lon = core_lon + gauss() * 0.9;
        const F64 lat = gauss() * 0.018 + 0.01 * sin(lon * 3.0);
        F64 d[3];
        band(lon, lat, d);
        const F64 size = 18.0 + 26.0 * rnd();
        push(mSkyOver, d, size, SPACE.mV, 0.12);
    }
    // Nebulae: three faint coloured clouds away from the band.
    static const F32 NEB[3][3] = { { 1.0f, 0.45f, 0.62f }, { 0.42f, 0.80f, 0.85f }, { 0.70f, 0.50f, 1.0f } };
    for (const auto& c : NEB)
    {
        F64 centre[3];
        onSphere(centre);
        for (int i = 0; i < 26; ++i)
        {
            F64 d[3];
            for (int k = 0; k < 3; ++k)
            {
                d[k] = centre[k] + gauss() * 0.035;
            }
            const F64 l = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            for (F64& x : d) x /= l;
            const F64 size = 22.0 + 40.0 * rnd();
            push(mSkyAdd, d, size, c, 0.022 + 0.02 * rnd());
        }
    }

    // The soft round dot each point is drawn with: alpha exp(-4 r^2) inside the unit circle
    // (map_globe.js's point shader, r^2 = |p - centre|^2 * 4).
    constexpr S32 DOT = 32;
    LLPointer<LLImageRaw> raw = new LLImageRaw(DOT, DOT, 4);
    U8* o = raw->getData();
    if (o)
    {
        for (S32 y = 0; y < DOT; ++y)
        {
            for (S32 x = 0; x < DOT; ++x, o += 4)
            {
                const F32 dx = (x + 0.5f) / DOT - 0.5f, dy = (y + 0.5f) / DOT - 0.5f;
                const F32 r2 = (dx * dx + dy * dy) * 4.f;
                o[0] = o[1] = o[2] = 255;
                o[3] = (r2 > 1.f) ? 0 : (U8)ll_round(255.f * expf(-r2 * 4.f));
            }
        }
        mStarDot = LLViewerTextureManager::getLocalTexture(raw.get(), false);
    }
}

// The sky behind the globe. It turns as the globe is spun: the centre's offset from the map's
// middle, as an angle round the globe (scaled so the far sky drifts at about the land's pace),
// seen through a camera looking at the globe. Source: map_globe.js MapGlobe._drawSpace (screen y
// grows up here, down there).
void WolfMapGlobe::drawSpace(const View& v, F32 alpha)
{
    if (alpha <= 0.01f || !mHaveExtent)
    {
        return;
    }
    if (!mSkyMade)
    {
        makeSky();
    }
    if (mStarDot.isNull())
    {
        return;
    }
    const F64 mid_x = 0.5 * (mX0 + mX1), mid_y = 0.5 * (mY0 + mY1);
    const F64 yaw = (v.centre.mdV[VX] - mid_x) / v.radius * 0.35;
    const F64 pitch = (v.centre.mdV[VY] - mid_y) / v.radius * 0.35;
    const F32 cyw = (F32)cos(yaw), syw = (F32)sin(yaw), cp = (F32)cos(pitch), sp = (F32)sin(pitch);
    const F32 f = 0.8f * llmax(v.width, v.height);
    const F32 W = v.width, H = v.height;

    auto draw = [&](const std::vector<SkyPoint>& list)
    {
        gGL.getTexUnit(0)->bind(mStarDot);
        gGL.begin(LLRender::TRIANGLES);
        for (const SkyPoint& s : list)
        {
            const F32 x = s.d[0], y = s.d[1], z = s.d[2];
            const F32 x1 = x * cyw - z * syw, z1 = x * syw + z * cyw;
            const F32 y2 = y * cp - z1 * sp, z2 = y * sp + z1 * cp;
            if (z2 < 0.05f)
            {
                continue;
            }
            const F32 sx = W * 0.5f + f * x1 / z2, sy = H * 0.5f + f * y2 / z2;
            const F32 h = s.size * 0.5f;
            if (sx < -h || sy < -h || sx > W + h || sy > H + h)
            {
                continue;
            }
            gGL.color4f(s.c[0], s.c[1], s.c[2], s.a * alpha);
            gGL.texCoord2f(0.f, 1.f); gGL.vertex2f(sx - h, sy + h);
            gGL.texCoord2f(0.f, 0.f); gGL.vertex2f(sx - h, sy - h);
            gGL.texCoord2f(1.f, 0.f); gGL.vertex2f(sx + h, sy - h);
            gGL.texCoord2f(0.f, 1.f); gGL.vertex2f(sx - h, sy + h);
            gGL.texCoord2f(1.f, 0.f); gGL.vertex2f(sx + h, sy - h);
            gGL.texCoord2f(1.f, 1.f); gGL.vertex2f(sx + h, sy + h);
        }
        gGL.end();
    };
    gGL.blendFunc(LLRender::BF_SOURCE_ALPHA, LLRender::BF_ONE);                    // starlight adds up
    draw(mSkyAdd);
    gGL.blendFunc(LLRender::BF_SOURCE_ALPHA, LLRender::BF_ONE_MINUS_SOURCE_ALPHA);  // the dust lane covers
    draw(mSkyOver);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
}

// ── the globe ────────────────────────────────────────────────────────────────────────────

void WolfMapGlobe::drawTiles(const View& v)
{
    LLWorldMap* world_map = LLWorldMap::getInstance();
    world_map->equalizeBoostLevels();

    const S32 level = LLWorldMipmap::scaleToLevel(v.ppr);
    const S32 regions_per_tile = 1 << (level - 1);
    const F64 tile_m = (F64)REGION_WIDTH_METERS * regions_per_tile;
    const F64 reach = visibleReach(v);

    // Tiles overlapping the visible reach, on the grid.
    const F64 x0 = llmax(v.centre.mdV[VX] - reach, mX0), x1 = llmin(v.centre.mdV[VX] + reach, mX1);
    const F64 y0 = llmax(v.centre.mdV[VY] - reach, mY0), y1 = llmin(v.centre.mdV[VY] + reach, mY1);
    if (x0 > x1 || y0 > y1)
    {
        return;
    }
    // Curved tiles are cut into cells; on the flat map one quad is exact.
    const S32 cells = (v.t > 0.02f) ? 4 : 1;

    for (F64 ty = floor(y0 / tile_m) * tile_m; ty < y1; ty += tile_m)
    {
        for (F64 tx = floor(x0 / tile_m) * tile_m; tx < x1; tx += tile_m)
        {
            // Skip tiles wholly beyond the reach (the square's corners).
            const F64 nx = llclamp(v.centre.mdV[VX], tx, tx + tile_m) - v.centre.mdV[VX];
            const F64 ny = llclamp(v.centre.mdV[VY], ty, ty + tile_m) - v.centre.mdV[VY];
            if (nx * nx + ny * ny > reach * reach)
            {
                continue;
            }
            const U32 grid_x = (U32)(tx / REGION_WIDTH_METERS);
            const U32 grid_y = (U32)(ty / REGION_WIDTH_METERS);
            LLPointer<LLViewerFetchedTexture> source = world_map->getObjectsTile(grid_x, grid_y, level, true);
            LLViewerTexture* tex = keyedTile(source.get(), level, grid_x, grid_y);
            if (!tex)
            {
                continue;
            }

            struct P { F32 x, y, shade; bool ok; };
            P p[5][5];
            for (S32 i = 0; i <= cells; ++i)
            {
                for (S32 j = 0; j <= cells; ++j)
                {
                    LLVector3d g(tx + tile_m * j / cells, ty + tile_m * i / cells, 0.0);
                    P& q = p[i][j];
                    q.ok = project(v, g, q.x, q.y, &q.shade);
                }
            }
            tex->setAddressMode(LLTexUnit::TAM_CLAMP);
            gGL.getTexUnit(0)->bind(tex);
            gGL.begin(LLRender::TRIANGLES);
            for (S32 i = 0; i < cells; ++i)
            {
                for (S32 j = 0; j < cells; ++j)
                {
                    const P& a = p[i][j];          // bottom left
                    const P& b = p[i][j + 1];      // bottom right
                    const P& c = p[i + 1][j];      // top left
                    const P& d = p[i + 1][j + 1];  // top right
                    if (!(a.ok && b.ok && c.ok && d.ok))
                    {
                        continue;
                    }
                    const F32 u_a = (F32)j / cells, u_b = (F32)(j + 1) / cells;
                    const F32 v_a = (F32)i / cells, v_c = (F32)(i + 1) / cells;
                    auto emit = [&](const P& q, F32 u, F32 w)
                    {
                        gGL.color4f(q.shade, q.shade, q.shade, 1.f);
                        gGL.texCoord2f(u, w);
                        gGL.vertex2f(q.x, q.y);
                    };
                    // Same winding and texture orientation as LLWorldMapView::drawMipmapLevel.
                    emit(c, u_a, v_c); emit(a, u_a, v_a); emit(b, u_b, v_a);
                    emit(c, u_a, v_c); emit(b, u_b, v_a); emit(d, u_b, v_c);
                }
            }
            gGL.end();
        }
    }
}

void WolfMapGlobe::drawMarker(const View& v, const LLVector3d& global, LLUIImage* image)
{
    F32 x, y, shade;
    if (!image || !project(v, global, x, y, &shade))
    {
        return;
    }
    image->draw(ll_round(x - image->getWidth() / 2.f), ll_round(y - image->getHeight() / 2.f));
}

void WolfMapGlobe::drawGlobe(const View& v, LLUIImage* home_image, LLUIImage* you_image)
{
    loadWater();
    LLGLSUIDefault gls_ui;
    const F64 k = v.ppr / 256.0;

    // Space, fading in from the water's colour as the map curves away.
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.color4fv(lerp(WATER_MEAN, SPACE, smooth(v.t * 1.5f)).mV);
    gl_rect_2d(0, (S32)v.height, (S32)v.width, 0);
    // <WolfViewer 2026-10-03/> The stars come out with it.
    drawSpace(v, smooth(v.t * 1.5f));

    // The atmosphere: a soft ring just outside the globe's rim.
    const F32 rim = (F32)projectedRadius(F_PI_BY_TWO * v.radius, k, v.t, v.radius);
    const F32 cx = v.width * 0.5f, cy = v.height * 0.5f;
    if (v.t > 0.f && rim < 4.f * llmax(v.width, v.height))
    {
        constexpr S32 SEG = 96;
        const F32 outer = rim * 1.06f + 6.f;
        gGL.begin(LLRender::TRIANGLES);
        for (S32 j = 0; j < SEG; ++j)
        {
            const F32 a0 = F_TWO_PI * j / SEG, a1 = F_TWO_PI * (j + 1) / SEG;
            const F32 c0 = cosf(a0), s0 = sinf(a0), c1 = cosf(a1), s1 = sinf(a1);
            const LLColor4 in(ATMOSPHERE.mV[VRED], ATMOSPHERE.mV[VGREEN], ATMOSPHERE.mV[VBLUE], 0.55f * v.t);
            const LLColor4 out(ATMOSPHERE.mV[VRED], ATMOSPHERE.mV[VGREEN], ATMOSPHERE.mV[VBLUE], 0.f);
            gGL.color4fv(in.mV);  gGL.vertex2f(cx + rim * c0, cy + rim * s0);
            gGL.color4fv(out.mV); gGL.vertex2f(cx + outer * c0, cy + outer * s0);
            gGL.color4fv(out.mV); gGL.vertex2f(cx + outer * c1, cy + outer * s1);
            gGL.color4fv(in.mV);  gGL.vertex2f(cx + rim * c0, cy + rim * s0);
            gGL.color4fv(out.mV); gGL.vertex2f(cx + outer * c1, cy + outer * s1);
            gGL.color4fv(in.mV);  gGL.vertex2f(cx + rim * c1, cy + rim * s1);
        }
        gGL.end();
    }

    drawWaterMesh(v);
    drawTiles(v);

    gGL.color4f(1.f, 1.f, 1.f, 1.f);
    LLVector3d home;
    if (home_image && gAgent.getHomePosGlobal(&home))
    {
        drawMarker(v, home, home_image);
    }
    drawMarker(v, gAgent.getPositionGlobal(), you_image);
}
