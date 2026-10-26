/**
 * @file wolfadultcontent.cpp
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

#include "llviewerprecompiledheaders.h"

#include "wolfadultcontent.h"

#include <boost/json.hpp>

#include "llagent.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "llframetimer.h"
#include "llhttpconstants.h"
#include "llnotificationsutil.h"
#include "llregionhandle.h"
#include "llsdjson.h"
#include "llsdutil.h"         // llsd::inArray
#include "lltrans.h"
#include "llviewerregion.h"
#include "llweb.h"
#include "llworld.h"
#include "wolfgrid.h"

namespace
{
    // Source: PROTOCOL.md §3b / §6 - the public list (no login, Cache-Control max-age=300).
    const char* const LIST_URL = "https://wolfstorm.app/php/wolf_adult_content.php?action=regions";
    // Source: PROTOCOL.md §3b "Ask at most once every 5 minutes".
    const F64 REFRESH_SECS = 300.0;
    const F64 RETRY_SECS = 300.0;       // a failed read waits as long: never more than once every 5 minutes
    // Source: PROTOCOL.md §6 - used when no region has sent WolfAgeCheckURL / WolfRegionTypesURL (§5: "or
    // https://www.wolf-grid.com/index.php?f=verifyage when you do not have it").
    const char* const DEFAULT_AGE_CHECK_URL = "https://www.wolf-grid.com/index.php?f=verifyage";
    const char* const DEFAULT_REGION_TYPES_URL = "https://www.wolf-grid.com/index.php?f=regiontypes";
    const char* const REGION_TYPE_URL = "https://www.wolf-grid.com/index.php?f=regiontype";

    // Source: PROTOCOL.md §5 - match on the marker text, not the whole string (the region name is inside the resident
    // text, and the viewer may see it after "/" (LLClientView SendAgentAlertMessage) or inside a teleport reason).
    const std::string MARKER_RESIDENT(" is an Adult Content region. Verify your age at wolf-grid.com");
    const std::string MARKER_MOVED("This is an Adult Content region and your account hasn't completed an age check");
    const std::string TEXT_VISITOR("Adult Content regions are only open to Wolf Territories residents who have completed "
                                   "an age check. Visitors from other grids can't enter.");

    // Source: wolfairports.cpp json_to_llsd (the same reply handling).
    LLSD json_to_llsd(const LLSD::Binary& bytes)
    {
        std::string text(bytes.begin(), bytes.end());
        boost::system::error_code ec;
        boost::json::value v = boost::json::parse(text, ec);
        if (ec) return LLSD();
        return LlsdFromJson(v);
    }

    // Only links to wolf-grid.com are opened from what a region sends (WolfAgeCheckURL / WolfRegionTypesURL): a region
    // is not trusted to send the resident to any other site from a "Verify my age" button.
    bool is_wolf_grid_page(const std::string& url)
    {
        static const std::string A("https://www.wolf-grid.com/");
        static const std::string B("https://wolf-grid.com/");
        return url.compare(0, A.size(), A) == 0 || url.compare(0, B.size(), B) == 0;
    }
}

// Source: skins/default/textures/textures.xml Parcel_AC_Light / Parcel_AC_Dark (added with this file).
const char* const WolfAdultContent::ICON_LIGHT = "Parcel_AC_Light";
const char* const WolfAdultContent::ICON_DARK = "Parcel_AC_Dark";

WolfAdultContent::WolfAdultContent()
{
}

WolfAdultContent::~WolfAdultContent()
{
}

// static
const std::string& WolfAdultContent::typeName()
{
    // strings.xml SIM_ACCESS_ADULT_CONTENT = "Adult Content" (PROTOCOL.md §1). Read once, as accessToString does
    // (llviewerregion.cpp <FS:ND> "Prevent querying LLTrans each frame").
    static const std::string name = LLTrans::getString("SIM_ACCESS_ADULT_CONTENT");
    return name;
}

// static
WolfAdultContent::ESays WolfAdultContent::regionSays(const LLViewerRegion* region, bool& adult_content)
{
    // Source: PROTOCOL.md §3a - OpenSimExtras "WolfAdultContent" (bool); a missing key is unknown, not General.
    // Source: lfsimfeaturehandler.cpp setSupportedFeatures - info["OpenSimExtras"] of getSimulatorFeatures.
    if (!region || !region->simulatorFeaturesReceived())
    {
        return ESays::NotYet;
    }
    LLSD info;
    region->getSimulatorFeatures(info);
    const LLSD extras = info.has("OpenSimExtras") ? info["OpenSimExtras"] : LLSD();
    // The region's OWN grid must be Wolf's (WolfGrid::isOnWolfRegion: only its own GridURL counts).
    if (!extras.has("GridURL") || !WolfGrid::isWolfHost(extras["GridURL"].asString()))
    {
        return ESays::NotWolf;
    }
    if (!extras.has("WolfAdultContent"))
    {
        return ESays::NoPlugin;   // PROTOCOL.md §9: not gated there - never marked Adult Content
    }
    adult_content = extras["WolfAdultContent"].asBoolean();
    return ESays::Said;
}

bool WolfAdultContent::listContains(F64 global_x, F64 global_y)
{
    // The list holds Wolf Territories' regions in Wolf Territories' map coordinates: only on that grid, standing on one
    // of its regions (a hypergrid trip can change the map server, LFSimFeatureHandler mMapServerURL).
    if (!WolfGrid::isOnWolfTerritories())
    {
        return false;
    }
    refresh();
    for (const Area& a : mAreas)
    {
        if (global_x >= (F64)a.mMinX && global_x < (F64)a.mMaxX && global_y >= (F64)a.mMinY && global_y < (F64)a.mMaxY)
        {
            return true;
        }
    }
    return false;
}

// static
bool WolfAdultContent::isAdultContentRegion(const LLViewerRegion* region)
{
    if (!region)
    {
        return false;
    }
    bool adult_content = false;
    const LLVector3d origin = region->getOriginGlobal();
    switch (regionSays(region, adult_content))
    {
    case ESays::NoPlugin:
    case ESays::NotWolf:
        return false;
    case ESays::Said:
        // SimulatorFeatures are sent on arrival only (PROTOCOL.md §3a notes), so after an owner switches Adult Content on
        // or off they are stale until the next arrival. The list (§3b, every 5 minutes, the grid record) is fresher:
        // once it has arrived it decides for a region that runs the plugin.
        if (getInstance()->listLoaded())
        {
            return getInstance()->listContains(origin.mdV[VX] + 1.0, origin.mdV[VY] + 1.0);
        }
        return adult_content;
    case ESays::NotYet:
    default:
        return getInstance()->listContains(origin.mdV[VX] + 1.0, origin.mdV[VY] + 1.0);
    }
}

// static
bool WolfAdultContent::isAdultContentAt(U64 region_handle)
{
    if (LLWorld::instanceExists())
    {
        if (LLViewerRegion* region = LLWorld::getInstance()->getRegionFromHandle(region_handle))
        {
            return isAdultContentRegion(region);
        }
    }
    U32 x = 0, y = 0;
    from_region_handle(region_handle, &x, &y);   // Source: llregionhandle.h:122 - global metres of the SW corner
    return getInstance()->listContains((F64)x + 1.0, (F64)y + 1.0);
}

// static
bool WolfAdultContent::isAdultContentAtGlobal(const LLVector3d& pos_global)
{
    if (LLWorld::instanceExists())
    {
        if (LLViewerRegion* region = LLWorld::getInstance()->getRegionFromPosGlobal(pos_global))
        {
            return isAdultContentRegion(region);
        }
    }
    return getInstance()->listContains(pos_global.mdV[VX], pos_global.mdV[VY]);
}

// static
std::string WolfAdultContent::accessString(U8 sim_access, bool adult_content)
{
    // A region that is down keeps "Offline" (SIM_ACCESS_DOWN) whatever the list says.
    if (adult_content && sim_access != SIM_ACCESS_DOWN)
    {
        return typeName();
    }
    return LLViewerRegion::accessToString(sim_access);
}

// static
std::string WolfAdultContent::accessIcon(U8 sim_access, bool adult_content)
{
    if (adult_content && sim_access != SIM_ACCESS_DOWN)
    {
        return ICON_LIGHT;
    }
    return LLViewerRegion::getAccessIcon(sim_access);
}

WolfAdultContent::EVerified WolfAdultContent::ageVerified() const
{
    // The state is about THIS login's Wolf Territories account: nothing on another grid's login.
    return WolfGrid::isWolfTerritories() ? mVerified : EVerified::Unknown;
}

bool WolfAdultContent::ageVerifiedFromThisRegion() const
{
    const LLViewerRegion* region = gAgent.getRegion();
    return WolfGrid::isWolfTerritories() && region && mVerified != EVerified::Unknown
        && region->getRegionID() == mVerifiedRegion;
}

std::string WolfAdultContent::ageCheckURL() const
{
    return mAgeCheckURL.empty() ? std::string(DEFAULT_AGE_CHECK_URL) : mAgeCheckURL;
}

std::string WolfAdultContent::regionTypesURL() const
{
    return mRegionTypesURL.empty() ? std::string(DEFAULT_REGION_TYPES_URL) : mRegionTypesURL;
}

// static
std::string WolfAdultContent::regionTypeURL(const LLUUID& region_id)
{
    // Source: PROTOCOL.md §6 - "...?f=regiontype" (list) and "...?f=regiontype&region={REGION UUID}".
    std::string url(REGION_TYPE_URL);
    if (region_id.notNull())
    {
        url += "&region=" + region_id.asString();
    }
    return url;
}

void WolfAdultContent::onSimulatorFeatures(const LLViewerRegion* region, const LLSD& extras)
{
    // Source: PROTOCOL.md §3a - WolfAgeVerified is the requesting agent's own state; WolfAgeCheckURL /
    // WolfRegionTypesURL the pages. Only from a Wolf region (its own GridURL), and only for a Wolf login.
    if (!region || !WolfGrid::isWolfTerritories() || !extras.has("GridURL")
        || !WolfGrid::isWolfHost(extras["GridURL"].asString()))
    {
        return;
    }
    if (extras.has("WolfAgeVerified"))
    {
        mVerified = extras["WolfAgeVerified"].asBoolean() ? EVerified::Verified : EVerified::NotVerified;
        mVerifiedRegion = region->getRegionID();
    }
    if (extras.has("WolfAgeCheckURL") && is_wolf_grid_page(extras["WolfAgeCheckURL"].asString()))
    {
        mAgeCheckURL = extras["WolfAgeCheckURL"].asString();
    }
    if (extras.has("WolfRegionTypesURL") && is_wolf_grid_page(extras["WolfRegionTypesURL"].asString()))
    {
        mRegionTypesURL = extras["WolfRegionTypesURL"].asString();
    }
}

// static
WolfAdultContent::ERefusal WolfAdultContent::classify(const std::string& text)
{
    if (text.find(TEXT_VISITOR) != std::string::npos)
    {
        return ERefusal::Visitor;
    }
    if (text.find(MARKER_MOVED) != std::string::npos)
    {
        return ERefusal::Moved;
    }
    if (text.find(MARKER_RESIDENT) != std::string::npos)
    {
        return ERefusal::Resident;
    }
    return ERefusal::NotAdultContent;
}

// static
bool WolfAdultContent::showRefusal(const std::string& text, std::function<void()> on_closed)
{
    const ERefusal kind = classify(text);
    if (kind == ERefusal::NotAdultContent)
    {
        return false;
    }
    if (kind != ERefusal::Visitor && !WolfGrid::isWolfTerritories())
    {
        return false;   // only a Wolf Territories resident has the "Verify my age" route
    }

    // The server's own words, without the "/" an agent alert carries (llviewermessage.cpp process_alert_core).
    std::string message = text;
    if (!message.empty() && message[0] == '/')
    {
        message.erase(0, 1);
    }
    LLStringUtil::trim(message);
    LLSD args;
    args["MESSAGE"] = message;

    if (kind == ERefusal::Visitor)
    {
        // Paul 2026-10-26: visitors have no route, so no verify button.
        LLNotificationsUtil::add("WolfAdultContentVisitor", args, LLSD(),
            [on_closed](const LLSD&, const LLSD&) { if (on_closed) on_closed(); });
        return true;
    }

    const std::string url = instanceExists() ? getInstance()->ageCheckURL() : std::string(DEFAULT_AGE_CHECK_URL);
    LLNotificationsUtil::add("WolfAdultContentRefused", args, LLSD(),
        [url, on_closed](const LLSD& notification, const LLSD& response)
        {
            // notifications.xml WolfAdultContentRefused: index 0 "Verify my age", 1 "Close".
            if (LLNotificationsUtil::getSelectedOption(notification, response) == 0)
            {
                LLWeb::loadURLExternal(url);
            }
            if (on_closed) on_closed();
        });
    return true;
}

void WolfAdultContent::refresh()
{
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (mFetching || now < mNextFetch || !WolfGrid::isOnWolfTerritories())
    {
        return;
    }
    mFetching = true;
    mNextFetch = now + RETRY_SECS;   // moved on to REFRESH_SECS by a good reply
    LLCoros::instance().launch("WolfAdultContent fetch", []() { WolfAdultContent::instance().fetchCoro(); });
}

// Source: wolfairports.cpp fetchCoro (the same adapter, options and reply handling); PROTOCOL.md §3b reply shape.
void WolfAdultContent::fetchCoro()
{
    LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
        std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfAdultContent", LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
    LLCore::HttpOptions::ptr_t options = WolfGrid::makeVerifiedHttpOptions();
    options->setTimeout(20);
    LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
    headers->append(HTTP_OUT_HEADER_ACCEPT, "application/json");

    LLSD result = adapter->getRawAndSuspend(request, LIST_URL, options, headers);
    mFetching = false;
    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(
        result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
    if (!status)
    {
        LL_WARNS("WolfAdultContent") << "Adult Content list: " << status.toString() << LL_ENDL;
        return;
    }
    LLSD reply;
    if (result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
    {
        reply = json_to_llsd(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary());
    }
    if (!reply.isMap() || !reply["success"].asBoolean() || !reply["regions"].isArray())
    {
        LL_WARNS("WolfAdultContent") << "Adult Content list: unexpected reply" << LL_ENDL;
        return;
    }
    mNextFetch = LLFrameTimer::getElapsedSeconds() + REFRESH_SECS;
    // Source: PROTOCOL.md §3b "Cache the answer by version": the same version is the same list.
    const std::string version = reply["version"].asString();
    if (!version.empty() && version == mVersion)
    {
        return;
    }
    std::vector<Area> areas;
    for (const LLSD& r : llsd::inArray(reply["regions"]))
    {
        if (areas.size() >= 20000) break;   // a sane bound on what one reply may hold
        // Source: PROTOCOL.md §3b - x, y the south-west corner in region units (metres / 256); size_x, size_y metres.
        const S64 x = r["x"].asInteger(), y = r["y"].asInteger();
        S64 sx = r["size_x"].asInteger(), sy = r["size_y"].asInteger();
        if (sx <= 0) sx = 256;
        if (sy <= 0) sy = 256;
        // Global metres fit in 32 bits (a region handle holds 32 bits each way, llregionhandle.h to_region_handle).
        if (x < 0 || y < 0 || sx > 65536 || sy > 65536 || x * 256 + sx > 4294967295LL || y * 256 + sy > 4294967295LL)
        {
            continue;
        }
        Area a;
        a.mMinX = (U32)(x * 256);
        a.mMinY = (U32)(y * 256);
        a.mMaxX = (U32)(x * 256 + sx);
        a.mMaxY = (U32)(y * 256 + sy);
        areas.push_back(a);
    }
    mAreas.swap(areas);
    mVersion = version;
    ++mGeneration;
    LL_INFOS("WolfAdultContent") << "Adult Content list: " << mAreas.size() << " regions (version " << mVersion << ")"
                                 << LL_ENDL;
}
