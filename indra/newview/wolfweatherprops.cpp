/**
 * @file wolfweatherprops.cpp
 * @brief WolfViewer: Weather Props - an umbrella in the rain, a snow shovel in the snow, a leaf rake in autumn (ticket 847).
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

#include "wolfweatherprops.h"

#include <cctype>

#include "llagent.h"
#include "llappearancemgr.h"
#include "llcoros.h"
#include "lldate.h"
#include "llframetimer.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llinventorymodelbackgroundfetch.h"
#include "llnotificationsutil.h"
#include "llstartup.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "llviewerjointattachment.h"
#include "llviewerobject.h"
#include "llvoavatarself.h"
#include "wolfcave.h"
#include "wolfgrid.h"
#include "wolfleavesseason.h"
#include "wolfregionweather.h"
#include "wolfweather.h"
#include "wolfweatherprofile.h"

namespace
{
    // Source: php/wolfprops.php (both web hosts; the same host and path as the Fishing give, wolffishingoverlay.cpp API_URL)
    const char* API_URL = "https://wolfstorm.app/php/wolfprops.php";
    constexpr F32 API_TIMEOUT_S = 30.f;                  // wolffishingoverlay.cpp API_TIMEOUT_S
    // Source: build_weatherprops.py DESC_PREFIX; php/wolfprops_lib.php WPR_DESC_PREFIX
    const std::string DESC_PREFIX = "wolfprop:";
    // Source: build_weatherprops.py FOLDER_NAME; php/wolfprops_items.php 'folder' (wolfprops_lib.php gives into Objects > it)
    const std::string FOLDER_NAME = "Weather Props";
    // Source: avatar_lad.xml attachment_point id="6" name="Right Hand"; OpenSim SLUtil.cs:660 "ATTACH_RHAND" // 6;
    // build_weatherprops.py ATTACH_RHAND (each prop's root State / LastAttachPoint)
    constexpr S32 RIGHT_HAND = 6;
    constexpr F64 CHECK_S = 2.0;          // CHOSEN: how often the rules are checked (a few comparisons, no network)
    constexpr F64 SETTLE_S = 20.0;        // CHOSEN: a change must hold this long, and changes are at least this far apart
    constexpr F64 SEASON_S = 60.0;        // CHOSEN: the season worked out again (wolfleaves.cpp SEASON_S is the leaves' own)
    constexpr F64 WEAR_WAIT_S = 30.0;     // CHOSEN: a wear not seen on by then has failed
    constexpr F64 GIVE_WAIT_S = 60.0;     // CHOSEN: a given prop not seen in the inventory by then is reported
    constexpr F64 REFETCH_S = 5.0;        // CHOSEN: while waiting, the props' folders are asked for again this often
    constexpr F64 RESCAN_S = 30.0;        // CHOSEN: a whole-inventory scan that found nothing is repeated at most this often
                                          // unless the inventory's item count changes (wolffishingoverlay.cpp RodWatch)

    /** The items a scan finds: an object item (not a link) whose description names `kind` (wolffishingoverlay.cpp RodItems). */
    class PropItems : public LLInventoryCollectFunctor
    {
    public:
        explicit PropItems(WolfWeatherProps::EKind k) : mKind(k) {}
        bool operator()(LLInventoryCategory*, LLInventoryItem* item) override
        {
            if (!item || item->getIsLinkType() || item->getType() != LLAssetType::AT_OBJECT
                || item->getInventoryType() != LLInventoryType::IT_OBJECT) return false;
            return WolfWeatherProps::descKind(item->getDescription()) == mKind;
        }
    private:
        WolfWeatherProps::EKind mKind;
    };

    /** The login's inventory fetch has nothing left to do (wolffishingoverlay.cpp backgroundFetchIdle). */
    bool backgroundFetchIdle()
    {
        const LLInventoryModelBackgroundFetch& f = LLInventoryModelBackgroundFetch::instance();
        return f.isEverythingFetched() || (f.inventoryFetchStarted() && !f.folderFetchActive() && f.isBulkFetchProcessingComplete());
    }

    /** Ask the region for a folder's contents again (wolffishingoverlay.cpp refetchFolder). */
    void refetch(const LLUUID& id)
    {
        if (id.isNull()) return;
        if (LLViewerInventoryCategory* cat = gInventory.getCategory(id))
        {
            cat->setVersion(LLViewerInventoryCategory::VERSION_UNKNOWN);
            cat->fetch();
        }
    }

    const char* weatherWords(WolfWeatherProps::EKind k)
    {
        switch (k)
        {
            case WolfWeatherProps::UMBRELLA: return "It is raining here";
            case WolfWeatherProps::SHOVEL:   return "It is snowing here";
            case WolfWeatherProps::RAKE:     return "It is autumn here";
            default:                         return "";
        }
    }
}

WolfWeatherProps::WolfWeatherProps() {}
WolfWeatherProps::~WolfWeatherProps() {}

// static
const char* WolfWeatherProps::keyOf(EKind k)
{
    switch (k)
    {
        case UMBRELLA: return "umbrella";
        case SHOVEL:   return "shovel";
        case RAKE:     return "rake";
        default:       return "";
    }
}

// static
const char* WolfWeatherProps::wordOf(EKind k)
{
    switch (k)
    {
        case UMBRELLA: return "umbrella";
        case SHOVEL:   return "snow shovel";
        case RAKE:     return "leaf rake";
        default:       return "";
    }
}

// static - Source: php/wolfprops_lib.php wpr_desc_key (the same rule, so the grid never gives a prop the viewer then cannot find)
WolfWeatherProps::EKind WolfWeatherProps::descKind(const std::string& description)
{
    std::string d = description;
    LLStringUtil::trimHead(d);
    LLStringUtil::toLower(d);
    if (d.compare(0, DESC_PREFIX.size(), DESC_PREFIX) != 0) return NONE;
    for (S32 i = UMBRELLA; i < KIND_COUNT; ++i)
    {
        const std::string want = DESC_PREFIX + keyOf((EKind)i);
        if (d.compare(0, want.size(), want) != 0) continue;
        if (d.size() == want.size()) return (EKind)i;
        const unsigned char next = (unsigned char)d[want.size()];
        if (!std::isalnum(next) && next != '_') return (EKind)i;
    }
    return NONE;
}

// static
bool WolfWeatherProps::enabled()
{
    static LLCachedControl<bool> on(gSavedSettings, "WolfViewerWeatherProps", false);
    return on;
}

// static
bool WolfWeatherProps::kindAllowed(EKind k)
{
    static LLCachedControl<bool> umbrella(gSavedSettings, "WolfViewerWeatherPropsUmbrella", true);
    static LLCachedControl<bool> shovel(gSavedSettings, "WolfViewerWeatherPropsShovel", true);
    static LLCachedControl<bool> rake(gSavedSettings, "WolfViewerWeatherPropsRake", true);
    switch (k)
    {
        case UMBRELLA: return umbrella;
        case SHOVEL:   return shovel;
        case RAKE:     return rake;
        default:       return false;
    }
}

void WolfWeatherProps::setStatus(const std::string& text, bool error)
{
    mStatus = text;
    mStatusError = error;
}

/** The season at this region's place, as the falling leaves work it out (wolfleaves.cpp:161-191 updateSeason). */
WolfWeatherProps::EKind WolfWeatherProps::season()
{
    const F64 now = LLFrameTimer::getTotalSeconds();
    if (mSeasonKind < 0 || now - mSeasonAt >= SEASON_S)
    {
        mSeasonAt = now;
        F64 lat = 0.0;
        bool place = false;
        F64 when = LLDate::now().secondsSinceEpoch();
        if (WolfRegionWeather::instanceExists())
        {
            const WolfRegionWeather& rw = WolfRegionWeather::instance();
            place = rw.autoEnvPlaceLat(lat);
            when = rw.autoEnvSkyTime();
        }
        mSeasonKind = (S32)WolfLeavesSeason::at(place, lat, WolfLeavesSeason::dayOfYearUTC(when)).kind;
    }
    return mSeasonKind == (S32)WolfLeavesSeason::AUTUMN ? RAKE : NONE;
}

/** The prop the weather and season call for here (ticked or not: the caller says why nothing is worn), NONE when sitting. */
WolfWeatherProps::EKind WolfWeatherProps::wantedKind(bool& sitting, EKind& weather_kind)
{
    weather_kind = NONE;
    if (WolfWeather::instanceExists())
    {
        // Source: wolfweather.cpp WolfWeather::apply() - mActiveSource "region", "parcel" (a parcel's setting or a parcel
        // prim's wolfrain / wolfsnow), "hurricane": weather everyone on this spot sees. Never "menu" (the resident's own
        // Weather menu, or the land's "no opinion"), "preview" (About Land > Weather's unsaved edit) or "off" (the switch).
        const WolfWeather& w = WolfWeather::instance();
        const std::string& src = w.activeSource();
        if (src == "region" || src == "parcel" || src == "hurricane")
        {
            const S32 kind = w.activeProfile().mKind;
            if (kind == WolfWeatherProfile::RAIN) weather_kind = UMBRELLA;
            else if (kind == WolfWeatherProfile::SNOW) weather_kind = SHOVEL;
        }
    }
    if (weather_kind == NONE) weather_kind = season();
    sitting = isAgentAvatarValid() && gAgentAvatarp->isSitting();
    if (sitting) return NONE;
    return weather_kind;
}

/** The prop item for `k`, outside the Trash: the one found last time if it still is one, else a whole scan (only when the
 *  inventory changed since a scan that found nothing, or RESCAN_S on). */
LLViewerInventoryItem* WolfWeatherProps::findProp(EKind k)
{
    auto it = mFound.find((S32)k);
    if (it != mFound.end())
    {
        LLViewerInventoryItem* item = gInventory.getItem(it->second);
        const LLUUID trash = gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH);
        if (item && item->isFinished() && descKind(item->getDescription()) == k
            && !(trash.notNull() && gInventory.isObjectDescendentOf(item->getUUID(), trash)))
        {
            return item;
        }
        mFound.erase(it);
    }
    const F64 now = LLFrameTimer::getTotalSeconds();
    const S32 count = gInventory.getItemCount();
    if (count == mScanCount && now - mScanAt < RESCAN_S) return nullptr;
    mScanCount = count;
    mScanAt = now;
    LLInventoryModel::cat_array_t cats;
    LLInventoryModel::item_array_t items;
    PropItems match(k);
    gInventory.collectDescendentsIf(gInventory.getRootFolderID(), cats, items, LLInventoryModel::EXCLUDE_TRASH, match);
    LLViewerInventoryItem* first = nullptr;
    for (const LLPointer<LLViewerInventoryItem>& p : items)
    {
        if (p.isNull() || !p->isFinished()) continue;
        if (get_is_item_worn(p->getUUID())) { first = p.get(); break; }
        if (!first) first = p.get();
    }
    if (first)
    {
        mFound[(S32)k] = first->getUUID();
        mScanCount = -1;
    }
    return first;
}

void WolfWeatherProps::takeOff(const LLUUID& item, const char* why)
{
    if (item.isNull() || !get_is_item_worn(item)) return;
    LL_INFOS("WolfWeatherProps") << "taking off " << item << ": " << why << LL_ENDL;
    mRemoving = item;
    mLastAction = LLFrameTimer::getTotalSeconds();
    LLAppearanceMgr::instance().removeItemFromAvatar(item);
}

void WolfWeatherProps::stopAll(const char* why)
{
    // only a prop this viewer put on (or found on at login): never one the resident wore by hand
    for (const LLUUID& id : mManaged)
    {
        if (get_is_item_worn(id)) takeOff(id, why);
    }
    mManaged.clear();
    mOurItem.setNull();
    mOurKind = NONE;
    mOurConfirmed = false;
    mPending = NONE;
    mDeclined = NONE;
    mWearFailed = NONE;
    // a give under way is forgotten (giveCoro checks the generation); a failed one may be asked again next time
    ++mGiveGen;
    mGive = GIVE_IDLE;
}

void WolfWeatherProps::retry()
{
    if (mGive != GIVE_FAILED) return;
    mGive = GIVE_IDLE;
    mScanCount = -1;
    mNextCheck = 0.0;
    setStatus("Trying again...");
}

void WolfWeatherProps::idle()
{
    const F64 now = LLFrameTimer::getTotalSeconds();
    if (now < mNextCheck) return;
    mNextCheck = now + CHECK_S;
    if (LLStartUp::getStartupState() < STATE_STARTED || !isAgentAvatarValid()) return;

    const bool active = WolfGrid::isOnWolfTerritories() && enabled();
    if (!active)
    {
        if (mWasActive)
        {
            // switched off, or left Wolf Territories: what we put on comes off, once; then nothing runs (inert)
            stopAll(WolfGrid::isOnWolfTerritories() ? "the switch is off" : "left Wolf Territories");
            mWasActive = false;
        }
        setStatus(WolfGrid::isOnWolfTerritories() ? "Weather props are switched off." : "Weather props work on Wolf Territories regions only.");
        return;
    }
    mWasActive = true;
    if (!mLoginScanned && !backgroundFetchIdle())
    {
        // the inventory must be known before a worn prop can be told from anything else in the hand (loginScan below)
        setStatus("Waiting for your inventory to load...");
        return;
    }

    // what is worn now: our prop's state, any prop in the right hand, and anything else there
    LLUUID worn_prop;                     // a prop (by description) worn anywhere
    EKind worn_kind = NONE;
    bool hand_busy = false;               // something that is not a prop in the right hand
    for (const auto& ap : gAgentAvatarp->mAttachmentPoints)
    {
        if (!ap.second) continue;
        for (const LLPointer<LLViewerObject>& obj : ap.second->mAttachedObjects)
        {
            if (obj.isNull()) continue;
            const LLUUID& item_id = obj->getAttachmentItemID();
            const LLViewerInventoryItem* item = gInventory.getItem(item_id);
            const EKind k = item ? descKind(item->getDescription()) : NONE;
            if (k != NONE)
            {
                if (worn_prop.isNull()) { worn_prop = item_id; worn_kind = k; }
            }
            else if (ap.first == RIGHT_HAND)
            {
                hand_busy = true;
            }
        }
    }
    if (!mLoginScanned)
    {
        // a prop worn at the first check (once the inventory has loaded) is taken as ours: put on by this feature before the
        // last logout (OpenSim keeps a worn attachment over a logout)
        mLoginScanned = true;
        if (worn_prop.notNull()) mManaged.insert(worn_prop);
    }

    // our last wear: on now, taken off by the resident, or never came on
    if (mOurItem.notNull())
    {
        const bool on = get_is_item_worn(mOurItem);
        if (on)
        {
            mOurConfirmed = true;
        }
        else if (mRemoving == mOurItem)
        {
            mOurItem.setNull();
            mOurKind = NONE;
            mOurConfirmed = false;
            mRemoving.setNull();
        }
        else if (mOurConfirmed)
        {
            LL_INFOS("WolfWeatherProps") << "the resident took the " << wordOf(mOurKind) << " off: not again until the weather changes" << LL_ENDL;
            mDeclined = mOurKind;
            mManaged.erase(mOurItem);
            mOurItem.setNull();
            mOurKind = NONE;
            mOurConfirmed = false;
        }
        else if (now - mOurAskedAt >= WEAR_WAIT_S)
        {
            LL_INFOS("WolfWeatherProps") << "the " << wordOf(mOurKind) << " did not go on" << LL_ENDL;
            mWearFailed = mOurKind;
            mManaged.erase(mOurItem);
            mOurItem.setNull();
            mOurKind = NONE;
        }
    }
    if (mRemoving.notNull() && !get_is_item_worn(mRemoving)) mRemoving.setNull();

    bool sitting = false;
    EKind weather = NONE;
    const EKind called = wantedKind(sitting, weather);
    EKind want = kindAllowed(called) ? called : NONE;
    if (want != mPending)
    {
        mPending = want;
        mPendingSince = now;
        // a change of weather: a prop taken off by hand, or one that would not go on, may come again
        if (mDeclined != want) mDeclined = NONE;
        if (mWearFailed != want) mWearFailed = NONE;
    }
    const bool settled = now - mPendingSince >= SETTLE_S;
    const bool can_act = now - mLastAction >= SETTLE_S;
    const EKind target = (want == mDeclined || want == mWearFailed) ? NONE : want;
    const std::string what = wordOf(want);

    // 1. a prop on that is not the one wanted: off, if it is one we manage
    if (worn_prop.notNull() && worn_kind != target)
    {
        if (mManaged.count(worn_prop))
        {
            if (settled && can_act) takeOff(worn_prop, "the weather changed");
            setStatus(target == NONE ? std::string("Putting your ") + wordOf(worn_kind) + " away in a moment."
                                     : std::string("Changing to your ") + wordOf(target) + " in a moment.");
            return;
        }
        // one the resident wore themselves: it stays, and nothing is added beside it
        setStatus(std::string("You are holding your ") + wordOf(worn_kind) + " yourself, so Weather Props leaves it be.");
        return;
    }

    // 2. why nothing is wanted
    if (target == NONE)
    {
        if (sitting) setStatus("You are sitting down, so no props just now.");
        else if (called != NONE && !kindAllowed(called)) setStatus(std::string(weatherWords(called)) + ", but the " + wordOf(called) + " is switched off below.");
        else if (want != NONE && want == mDeclined) setStatus(std::string("You took your ") + what + " off; it comes back with the next change of weather.");
        else if (want != NONE && want == mWearFailed)
            setStatus(std::string("Your ") + what + " could not be put on just now. It is tried again at the next change of weather.", true);
        else setStatus("Nothing to carry just now: no rain or snow here, and it is not autumn. Props follow the weather of the land you are on.");
        return;
    }

    // 3. already on
    if (worn_kind == target)
    {
        setStatus(std::string(weatherWords(target)) + ": " + (target == UMBRELLA ? "your umbrella is up." : "you have your " + what + "."));
        return;
    }
    if (mOurItem.notNull() && mOurKind == target)
    {
        setStatus(std::string("Putting your ") + what + " in your hand...");
        return;
    }
    if (hand_busy)
    {
        setStatus(std::string("Your right hand is holding something else, so no ") + what + ".");
        return;
    }
    if (!settled || !can_act)
    {
        setStatus(std::string(weatherWords(target)) + ": your " + what + " comes out in a moment.");
        return;
    }

    // 4. find it, or have the grid give the props, and put it on
    LLViewerInventoryItem* item = findProp(target);
    if (!item)
    {
        if (mGive == GIVE_FAILED) return;   // the status says why; Try again in the window (retry())
        if (mGive == GIVE_ASKING) { setStatus("Getting your free weather props from the grid..."); return; }
        if (mGive == GIVE_WAITING)
        {
            if (now - mGiveAt >= GIVE_WAIT_S)
            {
                mGive = GIVE_FAILED;
                setStatus("Your weather props are in your inventory (Objects > " + FOLDER_NAME + "), but this viewer cannot see them "
                          "yet. Press Try again, or log out and back in.", true);
                return;
            }
            if (now >= mRefetchAt) { refetchPropFolders(); mRefetchAt = now + REFETCH_S; mScanCount = -1; }
            setStatus("Your weather props are on their way into your inventory...");
            return;
        }
        if (!backgroundFetchIdle())
        {
            setStatus(std::string("Looking in your inventory for your ") + what + "...");
            return;
        }
        askGrid();
        return;
    }
    LL_INFOS("WolfWeatherProps") << "putting on the " << what << " (" << item->getUUID() << ")" << LL_ENDL;
    mOurItem = item->getUUID();
    mOurKind = target;
    mOurConfirmed = false;
    mOurAskedAt = now;
    mLastAction = now;
    mManaged.insert(mOurItem);
    if (mGive == GIVE_WAITING) mGive = GIVE_IDLE;
    setStatus(std::string("Putting your ") + what + " in your hand...");
    // add, never replace: anything else worn stays (the right hand was checked empty above)
    LLAppearanceMgr::instance().wearItemOnAvatar(mOurItem, true, false);
}

void WolfWeatherProps::refetchPropFolders()
{
    // Objects (a folder new to this login appears under it), the folder the grid named, and every "Weather Props" under
    // Objects (wolffishingoverlay.cpp rodCoro's waits)
    const LLUUID objects = gInventory.findUserDefinedCategoryUUIDForType(LLFolderType::FT_OBJECT);
    refetch(objects);
    refetch(mGiveFolder);
    if (objects.isNull()) return;
    LLInventoryModel::cat_array_t* cats = nullptr;
    LLInventoryModel::item_array_t* items = nullptr;
    gInventory.getDirectDescendentsOf(objects, cats, items);
    if (!cats) return;
    for (const LLPointer<LLViewerInventoryCategory>& c : *cats)
    {
        if (c.notNull() && LLStringUtil::compareInsensitive(c->getName(), FOLDER_NAME) == 0) refetch(c->getUUID());
    }
}

void WolfWeatherProps::askGrid()
{
    mGive = GIVE_ASKING;
    const U32 gen = ++mGiveGen;
    setStatus("Getting your free weather props from the grid...");
    LL_INFOS("WolfWeatherProps") << "asking the grid for the weather props" << LL_ENDL;
    LLCoros::instance().launch("WolfWeatherPropsGive", [gen]() { giveCoro(gen); });
}

// static
void WolfWeatherProps::giveCoro(U32 gen)
{
    // Source: wolffishingoverlay.cpp rodCoro step 2 (the same request and reply reading); php/wolfprops.php answers
    const std::string body("{\"action\":\"give\"}");
    LLSD result = WolfCaves::serviceRequest(API_URL, &body, API_TIMEOUT_S);
    LLSD reply;
    std::string transport;
    const S32 status = WolfCaves::serviceReply(result, reply, transport);
    if (!instanceExists()) return;
    WolfWeatherProps& self = instance();
    if (self.mGiveGen != gen || self.mGive != GIVE_ASKING) return;   // switched off, or left Wolf, meanwhile
    if (status != 200 || !reply.isMap() || !reply["success"].asBoolean())
    {
        const std::string why = reply.isMap() && reply.has("error") ? reply["error"].asString()
            : (transport.empty() ? llformat("the grid answered HTTP %d.", status) : transport);
        LL_INFOS("WolfWeatherProps") << "give refused (" << status << "): " << why << LL_ENDL;
        self.onGiveReply(false, why, LLUUID::null);
        return;
    }
    // Source: wolfprops_lib.php wpr_give -> {have: true, given: []} | {have: false, given: [...], folderId, folderNew, delivered}
    LL_INFOS("WolfWeatherProps") << "give: " << (reply["have"].asBoolean() ? "the grid says you have them"
                                                 : llformat("%d given", (S32)reply["given"].size()))
                                 << " folder " << reply["folderId"].asString() << LL_ENDL;
    self.onGiveReply(true, std::string(), reply["folderId"].asUUID());
}

void WolfWeatherProps::onGiveReply(bool ok, const std::string& error, const LLUUID& folder)
{
    if (!ok)
    {
        mGive = GIVE_FAILED;
        const std::string msg = "Your weather props could not be fetched: " + error;
        setStatus(msg + " Press Try again in Weather Props.", true);
        if (!mGiveAlerted)
        {
            // the window may be closed: say it once a session where it is seen (wolfvehiclerez.cpp says a failure the same way)
            mGiveAlerted = true;
            LLNotificationsUtil::add("GenericAlertOK", LLSD().with("MESSAGE", "Weather Props: " + msg));
        }
        return;
    }
    mGive = GIVE_WAITING;
    mGiveAt = LLFrameTimer::getTotalSeconds();
    mGiveFolder = folder;
    mRefetchAt = 0.0;
    mScanCount = -1;
    mNextCheck = 0.0;
}
