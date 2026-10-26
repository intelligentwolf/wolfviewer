/**
 * @file wolfadultcontent.h
 * @brief WolfViewer: the Adult Content region type (badges, the age check status, refusal dialogs)
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

#ifndef WOLF_ADULT_CONTENT_H
#define WOLF_ADULT_CONTENT_H

#include <functional>
#include <string>
#include <vector>

#include "llsingleton.h"
#include "llsd.h"
#include "lluuid.h"
#include "v3dmath.h"

class LLViewerRegion;

// [ADULT CONTENT 2026-10-26] Paul: four region types, General / Moderate / Adult / Adult Content. Adult Content needs a
// completed age check (verified 18+) to enter; hypergrid visitors are always refused.
// Source: region-terrain-work/adult-content/PROTOCOL.md (version 1, 2026-10-26) and
// region-terrain-work/online-safety/OSA_COMPLIANCE_PLAN.md §5.
//
// THE SERVER IS THE GATE (PROTOCOL.md top). Nothing here allows or refuses anything: it only names the type where a
// region's maturity shows, says what the resident's own age check status is, and turns the server's refusal text into
// a dialog with a "Verify my age" button. A stock viewer sees an ordinary Adult region with "age-verified residents only"
// ticked (PROTOCOL.md §2).
//
// Where the type comes from (PROTOCOL.md §3):
//  - a region the viewer has capabilities for: SimulatorFeatures > OpenSimExtras "WolfAdultContent" (bool) and
//    "WolfRegionType" (string), from the wolf-adult-content plugin. A missing key is "unknown", never "General" (§3a).
//  - any other region (map, search, landmarks, teleport offers): the public list
//    https://wolfstorm.app/php/wolf_adult_content.php?action=regions, asked at most once every 5 minutes (§3b).
// Wolf Territories only: the list is read only when this viewer is logged in to Wolf Territories and stands on one of
// its regions (WolfGrid::isOnWolfTerritories - the map and the positions are that grid's), and a region's own keys count
// only when its own GridURL is Wolf's (WolfGrid::isWolfHost). Every other grid shows exactly what it showed before.
class WolfAdultContent : public LLSingleton<WolfAdultContent>
{
    LLSINGLETON(WolfAdultContent);
    ~WolfAdultContent();

public:
    // Source: PROTOCOL.md §1 - the exact name residents see (capital A and C).
    static const std::string& typeName();             // "Adult Content"
    // The badge: its own icon, distinct from Adult's Parcel_R_* (textures.xml Parcel_AC_Light / Parcel_AC_Dark).
    static const char* const ICON_LIGHT;
    static const char* const ICON_DARK;

    // --- what a region is (display only) ---
    /** True when this region is Adult Content: its own SimulatorFeatures say so, or (when it has not said) the list. */
    static bool isAdultContentRegion(const LLViewerRegion* region);
    /** By region handle (the map's squares, teleport offers): a region the viewer is connected to first, then the list. */
    static bool isAdultContentAt(U64 region_handle);
    /** By a global position (place and landmark profiles, search results). */
    static bool isAdultContentAtGlobal(const LLVector3d& pos_global);
    /** The maturity word for a region: "Adult Content" instead of "Adult" when adult_content (else the stock word). */
    static std::string accessString(U8 sim_access, bool adult_content);
    /** The maturity icon (light background) for a region: ICON_LIGHT when adult_content (else the stock icon). */
    static std::string accessIcon(U8 sim_access, bool adult_content);

    // --- the resident's own age check (PROTOCOL.md §4) ---
    enum class EVerified { Unknown, Verified, NotVerified };
    /** SimulatorFeatures "WolfAgeVerified" from the last Wolf region that sent it this session (it is per agent and
        sent only on arrival, so it can be stale until the next region change - §3a notes). Unknown when no Wolf region
        has said, or this is not a Wolf Territories login. */
    EVerified ageVerified() const;
    /** True when the region the agent stands on now sent "WolfAgeVerified" (ageVerified() is then this region's word). */
    bool ageVerifiedFromThisRegion() const;

    // --- links (PROTOCOL.md §6) ---
    std::string ageCheckURL() const;        // WolfAgeCheckURL, or the default when no region has sent it
    std::string regionTypesURL() const;     // WolfRegionTypesURL, or the default
    static std::string regionTypeURL(const LLUUID& region_id);   // the owner's page for one region

    // --- refusals (PROTOCOL.md §5) ---
    enum class ERefusal { NotAdultContent, Resident, Moved, Visitor };
    /** Which of the server's texts this is (marker match, so a prefix such as "/" or "Teleport failed" may surround it). */
    static ERefusal classify(const std::string& text);
    /** If text is one of the server's Adult Content texts, show the Adult Content dialog for it and return true;
        otherwise do nothing and return false (the caller shows the text as it always did). The resident texts are
        handled only on a Wolf Territories login (only they have the "Verify my age" route); the visitor text anywhere,
        with no verify button (Paul 2026-10-26: visitors have no route). on_closed, when given, runs when the dialog
        closes (the login screen waits for it). */
    static bool showRefusal(const std::string& text, std::function<void()> on_closed = nullptr);

    /** Called by LFSimFeatureHandler::setSupportedFeatures with the agent's region's OpenSimExtras. */
    void onSimulatorFeatures(const LLViewerRegion* region, const LLSD& extras);

    /** Ask for the public list if it is 5 minutes old (never more often). Cheap; called by the lookups. */
    void refresh();
    /** Goes up with every list that arrives, so a window showing badges can fill again. */
    U32 generation() const { return mGeneration; }

private:
    struct Area
    {
        U32 mMinX = 0, mMinY = 0;   // global metres, south-west corner
        U32 mMaxX = 0, mMaxY = 0;   // global metres, exclusive
    };
    bool listContains(F64 global_x, F64 global_y);
    void fetchCoro();
    enum class ESays { NotYet, NotWolf, NoPlugin, Said };
    static ESays regionSays(const LLViewerRegion* region, bool& adult_content);
    bool listLoaded() const { return mGeneration > 0; }

    std::vector<Area> mAreas;
    std::string mVersion;
    bool mFetching = false;
    F64  mNextFetch = 0.0;
    U32  mGeneration = 0;

    EVerified mVerified = EVerified::Unknown;
    LLUUID    mVerifiedRegion;      // the region that last sent WolfAgeVerified
    std::string mAgeCheckURL;
    std::string mRegionTypesURL;
};

#endif // WOLF_ADULT_CONTENT_H
