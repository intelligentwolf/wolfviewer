/**
 * @file wolfmapchart.cpp
 * @brief World Map chart illustrations on Wolf Territories (see wolfmapchart.h).
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

#include "wolfmapchart.h"

#include "indra_constants.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "llhttpconstants.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llsdjson.h"
#include "llsdutil.h"
#include "llui.h"
#include "lluiimage.h"
#include "llviewercontrol.h"
#include "llworldmapview.h"
#include "wolfgrid.h"
#include <boost/json.hpp>

namespace
{
    // [CHART ART 2026-10-31] php/map_chart_art.php on wolfstorm.app, the host the map's box comes from
    // (wolfmapglobe.cpp EXTENT_URL). Public, read-only; the server caches it for an hour.
    const char* CHART_URL = "https://wolfstorm.app/php/map_chart_art.php";
    constexpr F64 REFRESH_S = 600.0;    // the server's own cache time: a new region clears its art within ~20 min
    constexpr F64 RETRY_S = 600.0;      // after a failure
    // A picture is drawn only while it is this wide on screen (the server picks its sizes for this range), and none
    // once a region is more than MAX_PPR pixels across ("hidden when zoomed in close").
    constexpr F32 MIN_PX = 32.f, MAX_PX = 240.f, MAX_PPR = 20.f;
    constexpr S32 MAX_DRAWN = 12;       // a handful of quads a frame, whatever the answer holds
    constexpr size_t MAX_ITEMS = 2100;  // map_chart_art.php: 3 sizes x CA_MAX_PER_TIER (700)
    constexpr F32 ALPHA = 0.9f;
    // OpenSim's world is 65536 regions of 256 m a side (wolfmapglobe.cpp WORLD_M).
    constexpr F64 WORLD_M = 16777216.0;

    // Source: wolfmapglobe.cpp json_to_llsd - the service answers JSON, not LLSD.
    LLSD json_to_llsd(const LLSD::Binary& bytes)
    {
        boost::system::error_code ec;
        boost::json::value v = boost::json::parse(std::string(bytes.begin(), bytes.end()), ec);
        return ec ? LLSD() : LlsdFromJson(v);
    }

    const char* const KIND_NAMES[] = { "serpent", "whale", "galleon", "sloop", "brigantine" };
    const char* const KIND_IMAGES[] = { "Wolf_Chart_Serpent", "Wolf_Chart_Whale", "Wolf_Chart_Galleon",
                                        "Wolf_Chart_Sloop", "Wolf_Chart_Brigantine" };
}

// static
bool WolfMapChart::settingOn()
{
    // Per account (settings_per_account.xml WolfMapChartArt, on by default). Before an account's settings are in,
    // the default.
    return !gSavedPerAccountSettings.controlExists("WolfMapChartArt") || gSavedPerAccountSettings.getBOOL("WolfMapChartArt");
}

// static
void WolfMapChart::setSettingOn(bool on)
{
    if (gSavedPerAccountSettings.controlExists("WolfMapChartArt"))
    {
        gSavedPerAccountSettings.setBOOL("WolfMapChartArt", on);
    }
}

// static
bool WolfMapChart::shown()
{
    return WolfGrid::isOnWolfTerritories() && settingOn();
}

void WolfMapChart::requestIfNeeded()
{
    if (mFetching || !WolfGrid::isOnWolfTerritoriesConfirmed())   // fetched only once THIS region says it is Wolf's
    {
        return;
    }
    const F64 now = LLTimer::getElapsedSeconds();
    if (mFetchedAt >= 0.0 && now - mFetchedAt < (mHaveItems ? REFRESH_S : RETRY_S))
    {
        return;
    }
    mFetching = true;
    mFetchedAt = now;
    LLCoros::instance().launch("WolfMapChart", []()
    {
        WolfMapChart::instance().fetchCoro();
    });
}

// Source: wolfmapglobe.cpp fetchExtentCoro - the same adapter shape (getRawAndSuspend, JSON body, verified TLS).
void WolfMapChart::fetchCoro()
{
    LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
        std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfMapChart", LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
    LLCore::HttpOptions::ptr_t options = WolfGrid::makeVerifiedHttpOptions();
    options->setTimeout(20);
    LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
    headers->append(HTTP_OUT_HEADER_ACCEPT, "application/json");

    LLSD result = adapter->getRawAndSuspend(request, CHART_URL, options, headers);
    LLSD http_results = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(http_results);
    mFetching = false;
    mFetchedAt = LLTimer::getElapsedSeconds();
    if (!status)
    {
        LL_WARNS("WolfMapChart") << "chart art read failed: " << status.toString() << LL_ENDL;
        return;
    }
    LLSD reply;
    if (result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
    {
        reply = json_to_llsd(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary());
    }
    if (!reply.isMap() || !reply["success"].asBoolean() || !reply["items"].isArray())
    {
        LL_WARNS("WolfMapChart") << "chart art read: unexpected reply" << LL_ENDL;
        return;
    }
    std::vector<Item> items;
    for (const LLSD& it : llsd::inArray(reply["items"]))
    {
        if (items.size() >= MAX_ITEMS) break;
        const std::string k = it["k"].asString();
        S32 kind = -1;
        for (S32 i = 0; i < KIND_COUNT; ++i)
        {
            if (k == KIND_NAMES[i]) { kind = i; break; }
        }
        const F64 x = it["x"].asReal(), y = it["y"].asReal(), w = it["w"].asReal(), h = it["h"].asReal();
        if (kind < 0 || !(x >= 0.0 && x <= WORLD_M && y >= 0.0 && y <= WORLD_M && w > 0.0 && h > 0.0
                          && w <= WORLD_M && h <= WORLD_M))
        {
            continue;   // an unknown kind (a newer server) or nonsense: skipped
        }
        items.push_back({ (EKind)kind, LLVector3d(x, y, 0.0), (F32)w, (F32)h });
    }
    mItems.swap(items);
    mHaveItems = true;
}

// Looked up each time (a map lookup in LLUIImageList), never held in a static: a static image pointer would outlive the
// texture list at shutdown (LLWorldMapView::cleanupClass clears its own images for the same reason).
LLUIImagePtr WolfMapChart::image(EKind kind)
{
    return LLUI::getUIImage(KIND_IMAGES[kind]);
}

void WolfMapChart::drawItem(const Item& item, F32 x, F32 y, F32 w_px, F32 h_px, F32 alpha)
{
    LLUIImagePtr img = image(item.mKind);
    if (img.isNull()) return;
    img->draw(ll_round(x - w_px * 0.5f), ll_round(y - h_px * 0.5f), ll_round(w_px), ll_round(h_px),
              LLColor4(1.f, 1.f, 1.f, ALPHA * alpha));
}

void WolfMapChart::drawFlat(LLWorldMapView& view, F32 pixels_per_region)
{
    if (!shown()) return;
    requestIfNeeded();
    if (mItems.empty() || pixels_per_region > MAX_PPR) return;
    const F32 ppm = pixels_per_region / REGION_WIDTH_METERS;
    const LLRect rect = view.getRect();
    const F32 width = (F32)rect.getWidth(), height = (F32)rect.getHeight();
    const F32 alpha = view.getDrawContext().mAlpha;
    S32 drawn = 0;
    for (const Item& item : mItems)
    {
        const F32 w_px = item.mW * ppm, h_px = item.mH * ppm;
        if (w_px < MIN_PX || w_px > MAX_PX) continue;
        const LLVector3 p = view.globalPosToView(item.mCentre);
        if (p.mV[VX] + w_px < 0.f || p.mV[VX] - w_px > width || p.mV[VY] + h_px < 0.f || p.mV[VY] - h_px > height)
        {
            continue;   // off screen
        }
        drawItem(item, p.mV[VX], p.mV[VY], w_px, h_px, alpha);
        if (++drawn >= MAX_DRAWN) break;
    }
}

void WolfMapChart::drawGlobe(const WolfMapGlobe::View& v)
{
    if (!shown()) return;
    requestIfNeeded();
    if (mItems.empty() || v.ppr > MAX_PPR) return;
    S32 drawn = 0;
    const F32 ppm = v.ppr / REGION_WIDTH_METERS;
    for (const Item& item : mItems)
    {
        // Cheap first: the globe never shows a thing wider than the flat map at its centre, and only up to a few times
        // narrower near the rim - skip what cannot be in range before projecting anything.
        const F32 flat_w = item.mW * ppm;
        if (flat_w < MIN_PX || flat_w > MAX_PX * 4.f) continue;
        // The width it has on the globe there: the distance between its left and right edges, projected.
        F32 cx, cy, lx, ly, rx, ry, shade = 1.f;
        if (!WolfMapGlobe::project(v, item.mCentre, cx, cy, &shade)) continue;   // round the back
        const LLVector3d half(item.mW * 0.5, 0.0, 0.0);
        if (!WolfMapGlobe::project(v, item.mCentre - half, lx, ly) || !WolfMapGlobe::project(v, item.mCentre + half, rx, ry))
        {
            continue;
        }
        const F32 w_px = sqrtf((rx - lx) * (rx - lx) + (ry - ly) * (ry - ly));
        if (w_px < MIN_PX || w_px > MAX_PX) continue;
        const F32 h_px = w_px * item.mH / item.mW;
        if (cx + w_px < 0.f || cx - w_px > v.width || cy + h_px < 0.f || cy - h_px > v.height) continue;
        drawItem(item, cx, cy, w_px, h_px, llclamp(shade, 0.3f, 1.f));
        if (++drawn >= MAX_DRAWN) break;
    }
}

void WolfMapChart::drawCompass(S32 width, S32 height, F32 alpha)
{
    if (!shown()) return;
    LLUIImagePtr rose = LLUI::getUIImage("Wolf_Chart_Compass");
    if (rose.isNull()) return;
    // Top-right, under the "NE" direction label (llworldmapview.cpp updateDirections: the corner labels sit PAD = 2 px
    // from the edges, one text line high). The map never turns, so north is always up. It is drawn UNDER the map
    // (llworldmapview.cpp: before the tiles on the flat map, before the pictures, pins and markers on the globe), so it
    // never hides a region, a pin or the agent: it shows through the sea.
    constexpr S32 SIZE = 64, INSET = 8, LABEL_GAP = 22;
    if (width < SIZE * 3 || height < SIZE * 3) return;   // a tiny map: no room for decoration
    rose->draw(width - SIZE - INSET, height - SIZE - LABEL_GAP, SIZE, SIZE, LLColor4(1.f, 1.f, 1.f, alpha));
}
