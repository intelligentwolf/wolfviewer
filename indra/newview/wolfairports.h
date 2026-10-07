/**
 * @file wolfairports.h
 * @brief WolfViewer: the grid's airports (wolf-grid.com airportsapi.php) and finding their runways
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

#ifndef WOLF_AIRPORTS_H
#define WOLF_AIRPORTS_H

#include <string>
#include <vector>

#include "llsingleton.h"
#include "v3dmath.h"

// Paul, 2026-10-07: "autopilot should find if there is an airport on a region and head for that
// we have an airports database, so check the api and try and find the runway ... and land when it
// gets there". The airports come from the grid's airports directory (gridmanager airports.php,
// table grid.airports) through airportsapi.php: one point per airport, the teleport address its
// owner pasted, in global metres. The directory holds no runway, so the runway is found in-world
// round that point (findRunway): the longest flat, narrow prim lying there.
// Wolf Territories only: WolfFlight, the only caller, runs only there (WolfGrid::isOnWolfTerritories).
class WolfAirports : public LLSingleton<WolfAirports>
{
    LLSINGLETON(WolfAirports);
    ~WolfAirports();

public:
    struct Airport
    {
        S32         mId = 0;
        std::string mName;
        std::string mRegion;
        LLVector3d  mGlobal;     // z = the address's height, or 0 when it has none
        bool        mHasZ = false;
        bool        mExact = false;   // false: the address named only the region, mGlobal is its centre
    };
    // A runway: its centreline end to end, both ends at the surface.
    struct Runway
    {
        bool       mValid = false;
        LLVector3d mA, mB;       // global, centreline ends
        F32        mWidth = 0.f;
        F32        mTopZ = 0.f;  // the surface height
        F32 length() const;
    };

    /** Fetch the list if it has not been fetched (or is 10 minutes old). Cheap to call every frame. */
    void refresh();
    bool loaded() const { return mLoaded; }
    /** The airport on this region (name, any case), or nullptr. */
    const Airport* forRegion(const std::string& region) const;
    /** Look for the runway round an airport among the objects the viewer has. Invalid if none. */
    static Runway findRunway(const Airport& airport);

private:
    void fetchCoro();
    std::vector<Airport> mAirports;
    bool mLoaded = false;
    bool mFetching = false;
    F64  mNextFetch = 0.0;
};

#endif // WOLF_AIRPORTS_H
