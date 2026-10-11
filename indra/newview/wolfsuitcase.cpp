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

#include <ctime>
#include <map>
#include <set>
#include <vector>

#include "llagent.h"
#include "llframetimer.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llinventorymodelbackgroundfetch.h"
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

    // <WolfViewer 2026-10-11> Jaedyn (Discord): "when I returned to the home grid it removed everything out of the folders
    // and tossed them into the objects folder ... it pulled single items out of the folder they came in ... complete
    // avatars are just a jumbled mess". Two faults: (1) every ADD inside the suitcase while away was noted, and the
    // suitcase's own contents also arrive as ADDs as they load abroad, so things already in it were taken for new;
    // (2) each noted item went home on its own to its type's folder, so a folder got from the trip was torn apart.
    // Now: only items CREATED during the trip are noted (inventory creation date at or after the trip's start, less 10
    // minutes for clocks; WolfSuitcaseTripStart, per account, set when the agent is first seen abroad), and a folder
    // goes home WHOLE (to the top of the inventory, where a given folder lands) when everything in it came on the trip;
    // a folder that mixes trip things with things already packed stays in the suitcase, untouched.
    // Only for a foreign grid's clock running a little behind ours; kept short so an outfit copied in just before
    // leaving is not taken for a trip arrival (missing an arrival is the safe failure: it just stays in the suitcase).
    const time_t CLOCK_SLACK_SECS = 120;

    // Made during the trip that started at trip_start (0: no trip known, so nothing is).
    bool from_trip(const LLViewerInventoryItem* item, U32 trip_start)
    {
        return item && trip_start != 0 && item->getCreationDate() > 0
            && item->getCreationDate() >= (time_t)trip_start - CLOCK_SLACK_SECS;
    }

    void record(const LLUUID& id)
    {
        const LLViewerInventoryItem* item = gInventory.getItem(id);
        if (!from_trip(item, gSavedPerAccountSettings.getU32("WolfSuitcaseTripStart")))
        {
            return;   // already packed before the trip (or no date to tell by): not an arrival
        }
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

    // Every folder in the tree has loaded (its own count matches what the viewer holds).
    bool tree_loaded(const LLUUID& folder)
    {
        if (!gInventory.isCategoryComplete(folder))
        {
            return false;
        }
        LLInventoryModel::cat_array_t cats;
        LLInventoryModel::item_array_t items;
        gInventory.collectDescendents(folder, cats, items, LLInventoryModel::EXCLUDE_TRASH);
        for (const LLPointer<LLViewerInventoryCategory>& c : cats)
        {
            if (!gInventory.isCategoryComplete(c->getUUID()))
            {
                return false;
            }
        }
        return true;
    }

    // A folder still loading is asked for and waited on this many passes (2 s apart) before it is left in the suitcase.
    const S32 MAX_LOAD_TRIES = 60;

    // Back home: a loose noted item in the suitcase goes to its normal folder; a folder from the trip goes home whole;
    // worn things stay (they are on), and so does a folder that mixes trip things with things already packed.
    void bring_home()
    {
        LLSD list = gSavedPerAccountSettings.getLLSD("WolfSuitcaseArrivals");
        if (!list.isArray() || list.size() == 0)
        {
            return;
        }
        // A list with no trip start was made by an older WolfViewer, which noted everything that loaded in the suitcase,
        // packed things too: nothing in it can be trusted, so it is dropped and the suitcase left as it is.
        const U32 trip_start = gSavedPerAccountSettings.getU32("WolfSuitcaseTripStart");
        if (trip_start == 0)
        {
            LL_INFOS("WolfSuitcase") << "home: " << list.size() << " noted by an older version dropped, suitcase left as it is" << LL_ENDL;
            gSavedPerAccountSettings.setLLSD("WolfSuitcaseArrivals", LLSD::emptyArray());
            return;
        }
        static std::map<LLUUID, S32> load_tries;   // folder -> passes it has been waited on
        const LLUUID box = suitcase();
        const LLUUID root = gInventory.getRootFolderID();
        std::set<LLUUID> arrivals;
        for (const LLSD& e : llsd::inArray(list))
        {
            // the date test again, for anything noted before this check was in record()
            const LLUUID id = e.asUUID();
            const LLViewerInventoryItem* item = gInventory.getItem(id);
            if (!item || from_trip(item, trip_start))
            {
                arrivals.insert(id);   // (not fetched yet: tested once it is)
            }
        }
        LLSD later = LLSD::emptyArray();
        std::set<LLUUID> folders_done;   // top folders already moved or kept this pass
        std::set<LLUUID> waiting;        // top folders still loading this pass
        S32 moved = 0, moved_folders = 0, kept = 0;
        for (const LLUUID& id : arrivals)
        {
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
            // The item's folders from the one it is in up to the one directly under the suitcase (empty: the item lies
            // loose in the suitcase itself).
            std::vector<LLUUID> chain;
            for (LLUUID p = item->getParentUUID(); p.notNull() && p != box; )
            {
                chain.push_back(p);
                const LLViewerInventoryCategory* pc = gInventory.getCategory(p);
                p = pc ? pc->getParentUUID() : LLUUID::null;
            }
            // The folder that goes home whole: the one directly under the suitcase; or, when that is one of the suitcase's
            // own typed folders (its Clothing, Objects, Landmarks..., HGSuitcaseInventoryService.cs:196), the pilot's
            // folder directly inside it - "Suitcase/Clothing/Avatar X" goes home as Avatar X, not item by item.
            LLUUID top = chain.empty() ? LLUUID::null : chain.back();
            LLViewerInventoryCategory* top_cat = top.notNull() ? gInventory.getCategory(top) : nullptr;
            if (top_cat && top_cat->getPreferredType() != LLFolderType::FT_NONE && chain.size() >= 2)
            {
                top = chain[chain.size() - 2];
                top_cat = gInventory.getCategory(top);
            }
            // Loose, or directly in one of the suitcase's own typed folders: to the item's normal folder, as before.
            if (!top_cat || top_cat->getPreferredType() != LLFolderType::FT_NONE)
            {
                const LLUUID home = gInventory.findCategoryUUIDForType(LLFolderType::assetTypeToFolderType(item->getType()));
                if (home.isNull() || gInventory.isObjectDescendentOf(home, box))
                {
                    continue;
                }
                change_item_parent(id, home);
                ++moved;
                continue;
            }
            // In a folder of the pilot's own: the whole folder, once, if everything in it came on the trip.
            if (waiting.count(top))
            {
                later.append(id);   // its folder is still loading (below): keep it noted with the rest
                continue;
            }
            if (folders_done.count(top))
            {
                continue;
            }
            if (!tree_loaded(top))
            {
                // the folder is still loading: ask for all of it, and decide when it is here - or, after
                // MAX_LOAD_TRIES, leave it in the suitcase rather than wait for ever
                if (++load_tries[top] <= MAX_LOAD_TRIES)
                {
                    waiting.insert(top);
                    if (load_tries[top] == 1)
                    {
                        LLInventoryModelBackgroundFetch::instance().start(top, true);
                    }
                    later.append(id);
                }
                else
                {
                    folders_done.insert(top);
                    LL_WARNS("WolfSuitcase") << "home: folder " << top << " never finished loading, left in My Suitcase" << LL_ENDL;
                    load_tries.erase(top);
                    ++kept;
                }
                continue;
            }
            load_tries.erase(top);
            LLInventoryModel::cat_array_t cats;
            LLInventoryModel::item_array_t items;
            gInventory.collectDescendents(top, cats, items, LLInventoryModel::EXCLUDE_TRASH);
            bool all_new = true;
            for (const LLPointer<LLViewerInventoryItem>& it : items)
            {
                if (!it->getIsLinkType() && !arrivals.count(it->getUUID()))
                {
                    all_new = false;
                    break;
                }
            }
            folders_done.insert(top);
            if (all_new && root.notNull())
            {
                gInventory.changeCategoryParent(top_cat, root, false);
                // changeCategoryParent refuses some folders without a word (llinventorymodel.cpp: locked AO, bridge and
                // favourites folders): count only what actually moved
                if (top_cat->getParentUUID() == root)
                {
                    ++moved_folders;
                }
                else
                {
                    ++kept;
                }
            }
            else
            {
                ++kept;
            }
        }
        gSavedPerAccountSettings.setLLSD("WolfSuitcaseArrivals", later);
        if (moved > 0 || moved_folders > 0 || kept > 0)
        {
            std::string msg = "Welcome home:";
            if (moved_folders > 0)
                msg += llformat(" %d folder%s from your trip moved, whole, from My Suitcase to the top of your inventory.",
                                moved_folders, moved_folders == 1 ? "" : "s");
            if (moved > 0)
                msg += llformat(" %d thing%s you got on your trip moved from My Suitcase into your inventory.",
                                moved, moved == 1 ? "" : "s");
            if (kept > 0)
                msg += llformat(" %d folder%s left in My Suitcase as %s: only a folder where everything came from the trip is moved.",
                                kept, kept == 1 ? "" : "s", kept == 1 ? "it is" : "they are");
            LLSD args;
            args["MESSAGE"] = msg;
            LLNotificationsUtil::add("SystemMessageTip", args);
        }
        LL_INFOS("WolfSuitcase") << "home: " << moved << " items and " << moved_folders << " folders moved out of My Suitcase, "
                                 << kept << " mixed folders left, " << later.size() << " still to load" << LL_ENDL;
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
        // <WolfViewer 2026-10-11> the trip's start, for telling new things from packed ones (record); cleared at home once
        // everything noted has been dealt with
        const bool abroad = onForeignGrid();
        if (abroad && gSavedPerAccountSettings.getU32("WolfSuitcaseTripStart") == 0)
        {
            gSavedPerAccountSettings.setU32("WolfSuitcaseTripStart", (U32)time(nullptr));
        }
        if (region_known() && !abroad)
        {
            bring_home();
            const LLSD left = gSavedPerAccountSettings.getLLSD("WolfSuitcaseArrivals");
            if ((!left.isArray() || left.size() == 0) && gSavedPerAccountSettings.getU32("WolfSuitcaseTripStart") != 0)
            {
                gSavedPerAccountSettings.setU32("WolfSuitcaseTripStart", 0);
            }
        }
    }
}
