/**
 * @file wolfmapchart.h
 * @brief World Map chart illustrations on Wolf Territories: a compass rose, and sea serpents, whales and sailing ships
 *        printed in the open sea (WolfViewer 2026-10-31).
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

#ifndef WOLF_MAPCHART_H
#define WOLF_MAPCHART_H

#include "llsingleton.h"
#include "llpointer.h"
#include "v3dmath.h"
#include "wolfmapglobe.h"
#include "lluiimage.h"   // LLUIImagePtr
#include <string>
#include <vector>

class LLWorldMapView;

// Paul 2026-10-31: "could we add illustrations to the map like a sea monster and a compass", "and a couple of ships".
// The World Map as an old explorer's chart, tastefully:
//   - a compass rose fixed in the top-right corner of the map view. The map never turns (llworldmapview.cpp: "No compass
//     rotation" - only the tilt, about the horizontal axis; the globe keeps north up at the centre of the view), so the
//     rose always points north up.
//   - pictures in the OPEN SEA, as if printed on the map: where they go is decided by the server
//     (wolfstorm.app/php/map_chart_art.php: deterministic, away from every region by a margin, a few per zoom), so they
//     never cover a region, and they are drawn straight after the map tiles - under region names, pins, the agent and
//     every marker - so nothing clickable is ever under them. Each is shown only while it is 32-240 px wide, and none
//     when zoomed in close (more than MAX_PPR pixels per region).
// Wolf Territories only: shown with WolfGrid::isOnWolfTerritories() (the band's and the Experiences button's test), the
// positions fetched only with isOnWolfTerritoriesConfirmed(); nothing is drawn or fetched elsewhere. The World Map's
// "Chart illustrations" box turns them off (per account, WolfMapChartArt; on by default).
// The art is original, drawn for Wolf Territories (skins/default/textures/wolfmap/chart_*.png, textures.xml
// Wolf_Chart_*).
class WolfMapChart : public LLSingleton<WolfMapChart>
{
    LLSINGLETON_EMPTY_CTOR(WolfMapChart);
public:
    // Shown now: on Wolf Territories and the account's setting on.
    static bool shown();
    static bool settingOn();
    static void setSettingOn(bool on);

    // The flat map (inside its tilt, straight after the tiles). pixels_per_region = the map's scale.
    void drawFlat(LLWorldMapView& view, F32 pixels_per_region);
    // The globe (after drawGlobe), projected like everything else on it.
    void drawGlobe(const WolfMapGlobe::View& v);
    // The compass rose, in the view's top-right corner (view coordinates, untilted), drawn under the map's content.
    void drawCompass(S32 width, S32 height, F32 alpha);

private:
    enum EKind { SERPENT, WHALE, GALLEON, SLOOP, BRIGANTINE, KIND_COUNT };
    struct Item
    {
        EKind      mKind;
        LLVector3d mCentre;   // global metres
        F32        mW, mH;    // metres
    };
    void requestIfNeeded();
    void fetchCoro();
    LLUIImagePtr image(EKind kind);
    void drawItem(const Item& item, F32 x, F32 y, F32 w_px, F32 h_px, F32 alpha);

    std::vector<Item> mItems;
    bool  mFetching = false;
    F64   mFetchedAt = -1.0;
    bool  mHaveItems = false;
};

#endif
