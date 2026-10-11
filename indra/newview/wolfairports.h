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
class LLViewerObject;

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
    // <WolfViewer 2026-10-11> Paul: "refresh the airports window every time it opens". Fetch now, not on the
    // 10 minute clock (unless a fetch is already under way). generation() goes up with every list that arrives,
    // so a window or page showing it can fill again.
    void refreshNow();
    U32 generation() const { return mGeneration; }
    // </WolfViewer>
    bool loaded() const { return mLoaded; }
    /** The airport on this region (name, any case), or nullptr. */
    const Airport* forRegion(const std::string& region) const;
    // <WolfViewer 2026-10-10> Paul: "users should be able to search for airports ... by airport name ... it looks
    // up the airport in our database and then gets them to the right place on the region". Every airport whose
    // name or region holds the text (any case, accents ignored): names starting with it first, then a word
    // starting with it, then anywhere; nearest first within each. And one by its directory id.
    std::vector<const Airport*> search(const std::string& text, const LLVector3d& from) const;
    const Airport* byId(S32 id) const;
    const std::vector<Airport>& all() const { return mAirports; }   // <WolfViewer 2026-10-10/> the Airports window
    /** Plain lower-case ASCII for matching and for the flight computer's screen: accents dropped. */
    static std::string fold(const std::string& utf8);
    // </WolfViewer>
    /** Look for the runway round an airport among the objects the viewer has. Invalid if none. */
    // <WolfViewer 2026-10-11> a flat slab the airport's own point lies on counts as the runway (as a named one does), and the
    // longest such wins; one shorter than need_len (metres) gives way to a longer runway there if there is one. The linkset
    // rooted at skip_root (the plane itself, when looking for the runway it sits on) is never a runway.
    static Runway findRunway(const Airport& airport, F32 need_len = 0.f, const LLViewerObject* skip_root = nullptr);

private:
    void fetchCoro();
    std::vector<Airport> mAirports;
    bool mLoaded = false;
    bool mFetching = false;
    F64  mNextFetch = 0.0;
    U32  mGeneration = 0;   // <WolfViewer 2026-10-11/> lists received
};

#endif // WOLF_AIRPORTS_H
