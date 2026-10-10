/**
 * @file wolfsuitcase.cpp
 * @brief WolfViewer: inventory on a hypergrid trip. See wolfsuitcase.h.
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

#include "wolfsuitcase.h"

#include "llagent.h"
#include "llframetimer.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llinventoryobserver.h"
#include "llnotificationsutil.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "llviewernetwork.h"
#include "llviewerregion.h"
#include "lfsimfeaturehandler.h"

namespace
{
    // Setting WolfSuitcaseArrivals (per account): what arrived in My Suitcase while away, kept across a relog
    // abroad, at most this many.
    const size_t MAX_ARRIVALS = 500;

    // The host of a grid address, lower case: "http://grid.example.org:8002/" -> "grid.example.org"
    // (as WolfGrid::isWolfHost reads one, wolfgrid.h).
    std::string host_of(std::string url)
    {
        LLStringUtil::toLower(url);
        const size_t scheme = url.find("://");
        if (scheme != std::string::npos) url.erase(0, scheme + 3);
        const size_t end = url.find_first_of(":/");
        if (end != std::string::npos) url.erase(end);
        return url;
    }

    // Source: llfoldertype.h:104 FT_MY_SUITCASE = 100; OpenSim HGSuitcaseInventoryService.cs:160-161 type 100,
    // a child of the root - findCategoryUUIDForType searches the root's children (llinventorymodel.cpp).
    LLUUID suitcase()
    {
        return gInventory.findCategoryUUIDForType(LLFolderType::FT_MY_SUITCASE);
    }

    // The region underfoot has said which grid it belongs to (SimulatorFeatures GridURL, lfsimfeaturehandler).
    bool region_known()
    {
        const LLViewerRegion* region = gAgent.getRegion();
        if (!region || !LFSimFeatureHandler::instanceExists())
        {
            return false;
        }
        const LFSimFeatureHandler& f = LFSimFeatureHandler::instance();
        return f.featuresRegionID() == region->getRegionID() && f.gridURLFromRegion();
    }

    void record(const LLUUID& id)
    {
        LLSD list = gSavedPerAccountSettings.getLLSD("WolfSuitcaseArrivals");
        if (!list.isArray()) list = LLSD::emptyArray();
        for (const LLSD& e : llsd::inArray(list))
        {
            if (e.asUUID() == id) return;
        }
        if (list.size() >= MAX_ARRIVALS) return;
        list.append(id);
        gSavedPerAccountSettings.setLLSD("WolfSuitcaseArrivals", list);
    }

    // While away: everything new inside My Suitcase is noted (made, bought, taken, given).
    class SuitcaseWatch : public LLInventoryObserver
    {
    public:
        void changed(U32 mask) override
        {
            if (!(mask & LLInventoryObserver::ADD) || !WolfSuitcase::onForeignGrid())
            {
                return;
            }
            const LLUUID box = suitcase();
            if (box.isNull())
            {
                return;
            }
            for (const LLUUID& id : gInventory.getAddedIDs())
            {
                const LLViewerInventoryItem* item = gInventory.getItem(id);
                if (item && !item->getIsLinkType() && gInventory.isObjectDescendentOf(id, box))
                {
                    record(id);
                }
            }
        }
    };
    SuitcaseWatch* sWatch = nullptr;

    // Back home: each noted item still in the suitcase goes to its normal folder; worn ones stay (they are on).
    void bring_home()
    {
        LLSD list = gSavedPerAccountSettings.getLLSD("WolfSuitcaseArrivals");
        if (!list.isArray() || list.size() == 0)
        {
            return;
        }
        const LLUUID box = suitcase();
        LLSD later = LLSD::emptyArray();
        S32 moved = 0;
        for (const LLSD& e : llsd::inArray(list))
        {
            const LLUUID id = e.asUUID();
            LLViewerInventoryItem* item = gInventory.getItem(id);
            if (!item)
            {
                later.append(id);   // not fetched yet: next time
                continue;
            }
            if (box.isNull() || !gInventory.isObjectDescendentOf(id, box) || item->getIsLinkType() || get_is_item_worn(id))
            {
                continue;           // moved already, a link, or worn: leave it
            }
            const LLUUID home = gInventory.findCategoryUUIDForType(LLFolderType::assetTypeToFolderType(item->getType()));
            if (home.isNull() || gInventory.isObjectDescendentOf(home, box))
            {
                continue;
            }
            change_item_parent(id, home);
            ++moved;
        }
        gSavedPerAccountSettings.setLLSD("WolfSuitcaseArrivals", later);
        if (moved > 0)
        {
            LLSD args;
            args["MESSAGE"] = llformat("Welcome home: %d thing%s you got on your trip moved from My Suitcase into your inventory.",
                                       moved, moved == 1 ? "" : "s");
            LLNotificationsUtil::add("SystemMessageTip", args);
        }
        LL_INFOS("WolfSuitcase") << "home: " << moved << " moved out of My Suitcase, " << later.size() << " still to load" << LL_ENDL;
    }
}

namespace WolfSuitcase
{
    bool onForeignGrid()
    {
        LLGridManager* gm = LLGridManager::getInstance();
        if (!gm || !gm->isInOpenSim() || !region_known())
        {
            return false;
        }
        const std::string here = host_of(LFSimFeatureHandler::instance().hyperGridURL());
        return !here.empty() && here != host_of(gm->getGrid()) && here != host_of(gm->getGatekeeper());
    }

    LLUUID landmarkFolder()
    {
        if (!onForeignGrid())
        {
            return gInventory.findCategoryUUIDForType(LLFolderType::FT_LANDMARK);
        }
        // Source: HGSuitcaseInventoryService.cs:196 - the suitcase has its own "Landmarks" (type 3)
        const LLUUID box = suitcase();
        if (box.isNull())
        {
            return LLUUID::null;
        }
        const LLUUID lm = gInventory.findCategoryUUIDForTypeInRoot(LLFolderType::FT_LANDMARK, box);
        return lm.notNull() ? lm : box;
    }

    void idle()
    {
        static F64 next = 0.0;
        const F64 now = LLFrameTimer::getElapsedSeconds();
        if (now < next || !gInventory.isInventoryUsable())
        {
            return;
        }
        next = now + 2.0;
        if (!sWatch)
        {
            sWatch = new SuitcaseWatch();
            gInventory.addObserver(sWatch);
        }
        if (region_known() && !onForeignGrid())
        {
            bring_home();
        }
    }
}
