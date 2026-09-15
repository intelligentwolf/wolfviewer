/**
 * @file wolfregionweather.cpp
 * @brief WolfViewer: region and occupied-parcel weather state and editors.
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

#include "wolfregionweather.h"

#include <memory>          // std::unique_ptr, for the asset callback's handle

#include <boost/json.hpp>

#include "llagent.h"
#include "llassetstorage.h"
#include "llbutton.h"
#include "llcolorswatch.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "llcorehttputil.h"
#include "lldispatcher.h"
#include "llcoros.h"
#include "llframetimer.h"
#include "llhttpconstants.h"
#include "llfilesystem.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llnotificationsutil.h"
#include "llparcel.h"
#include "llpermissions.h"
#include "llradiogroup.h"
#include "roles_constants.h"
#include "llscrolllistctrl.h"
#include "llsdjson.h"
#include "llsliderctrl.h"
#include "lltextbox.h"
#include "lluictrlfactory.h"
#include "llviewerinventory.h"
#include "llviewerparcelmgr.h"
#include "llviewergenericmessage.h"
#include "llviewerregion.h"
#include "wolfgrid.h"
#include "wolfweather.h"

const char* WolfRegionWeather::API_URL = "https://wolfstorm.app/php/weather.php";

namespace
{
    // A region's sky is allowed to change under you, so this is short — but not so short that a
    // quiet region costs a request a second.
    constexpr F32 POLL_SECS = 60.f;
    constexpr S32 MAX_FETCHES_IN_FLIGHT = 2;

    LLSD json_to_llsd(const LLSD::Binary& bytes)
    {
        std::string text(bytes.begin(), bytes.end());
        boost::system::error_code ec;
        boost::json::value v = boost::json::parse(text, ec);
        if (ec) return LLSD();
        return LlsdFromJson(v);
    }

    std::string currentRegionId()
    {
        LLViewerRegion* rgn = gAgent.getRegion();
        return rgn ? rgn->getRegionID().asString() : std::string();
    }
}

/**
 * [WEATHER 2026-09-12] The region telling us the weather changed, now.
 *
 * wolfSetRegionWeather (Wolf_Api.cs) writes grid.weather AND sends this to everyone standing in
 * the region, so a scripted storm starts when the script says so instead of at the next poll.
 * grid.weather is still the truth — this is a shortcut, and anyone who arrives later reads the
 * stored row as usual. A malformed or unexpected push is ignored rather than trusted: it comes
 * off the wire and is not a reason to put the sky into a state the grid does not agree with.
 */
class WolfWeatherPushHandler : public LLDispatchHandler
{
public:
    bool operator()(const LLDispatcher*, const std::string& key, const LLUUID& invoice,
                    const sparam_t& strings) override
    {
        if (strings.empty()) return true;
        LLViewerRegion* rgn = gAgent.getRegion();
        // Only the region the agent is actually in may change this agent's weather.
        if (!rgn || rgn->getRegionID() != invoice)
        {
            LL_DEBUGS("WolfWeather") << "ignoring a weather push for another region" << LL_ENDL;
            return true;
        }
        boost::system::error_code ec;
        boost::json::value v = boost::json::parse(strings[0], ec);
        if (ec) { LL_WARNS("WolfWeather") << "unreadable weather push" << LL_ENDL; return true; }
        const LLSD sd = LlsdFromJson(v);
        if (!sd.isMap()) return true;

        WolfRegionWeather::instance().applyPush(sd);
        return true;
    }
};

namespace
{
    WolfWeatherPushHandler sWeatherPushHandler;
    const std::string MESSAGE_WOLF_WEATHER("WolfWeather");
}

WolfRegionWeather::WolfRegionWeather()
{
    // Registered once, for the life of the singleton. LLEnvironment does the same for its own
    // PushEnvironment message (llenvironment.cpp:973).
    if (!gGenericDispatcher.isHandlerPresent(MESSAGE_WOLF_WEATHER))
    {
        gGenericDispatcher.addHandler(MESSAGE_WOLF_WEATHER, &sWeatherPushHandler);
    }
    // Source: llagent.cpp:1142-1151 and :1178-1315. Region changes invalidate the old parcel
    // immediately; parcel changes arrive only after the destination parcel and bitmap exist.
    mRegionChangedConnection = gAgent.addRegionChangedCallback([this]() { onRegionChanged(); });
    mParcelChangedConnection = gAgent.addParcelChangedCallback([this]() { onParcelChanged(); });
    LLViewerRegion* region = gAgent.getRegion();
    LLParcel* parcel = LLViewerParcelMgr::getInstance()->getAgentParcel();
    if (region && parcel && parcel->getRegionID() == region->getRegionID()) mAwaitingParcel = false;
}

WolfRegionWeather::~WolfRegionWeather()
{
    if (mRegionChangedConnection.connected()) mRegionChangedConnection.disconnect();
    if (mParcelChangedConnection.connected()) mParcelChangedConnection.disconnect();
}

std::string WolfRegionWeather::regionTarget() const
{
    // Source: LLAgent::setRegion() (llagent.cpp:1178-1315). The visit generation distinguishes
    // A -> B -> A even though the destination UUID repeats.
    return currentRegionId() + ":" + llformat("%llu", (unsigned long long)mRegionGate.current());
}

std::string WolfRegionWeather::parcelTarget() const
{
    return currentRegionId() + ":" + llformat("%llu", (unsigned long long)mParcelGate.current())
         + ":" + mParcelId;
}

WolfWeatherSaveTarget WolfRegionWeather::regionSaveTarget() const
{
    WolfWeatherSaveTarget target;
    target.mEditorTarget = regionTarget();
    target.mRegionId = currentRegionId();
    target.mVersion = mVersion;
    target.mVisitGeneration = mRegionGate.current();
    return target;
}

WolfWeatherSaveTarget WolfRegionWeather::parcelSaveTarget() const
{
    // Source: weather.php:311-351,383-505. UUID, version and x/y are copied from the same
    // accepted GET and stay together for optimistic parcel POST.
    WolfWeatherSaveTarget target;
    target.mEditorTarget = parcelTarget();
    target.mRegionId = currentRegionId();
    target.mParcelId = mParcelId;
    target.mVersion = mParcelVersion;
    target.mX = mParcelX;
    target.mY = mParcelY;
    target.mVisitGeneration = mParcelGate.current();
    return target;
}

void WolfRegionWeather::invalidateParcel()
{
    mHaveParcel = false;
    mParcelProfile = WolfWeatherProfile();
    mParcelKind.clear();
    mParcelName.clear();
    mParcelId.clear();
    mParcelGroupOwned = false;
    mParcelApplies = false;
    mParcelVersion = 0;
    mParcelX = 0.f;
    mParcelY = 0.f;
    ++mGeneration;
    applyToWeather();
}

void WolfRegionWeather::onRegionChanged()
{
    // The region row belongs to the old region just as surely as the parcel row does. Clearing
    // both prevents the previous sky being applied while the destination requests are in flight.
    mHaveRow = false;
    mProfile = WolfWeatherProfile();
    mKind.clear();
    mRegionName.clear();
    mVersion = 0;
    mAllowParcel = true;
    mFetchedFor.clear();
    mAttemptedFor.clear();
    mAwaitingParcel = true;
    mRegionGate.advance();
    mParcelGate.advance();
    ++mRegionSaveSerial;
    ++mParcelSaveSerial;
    mSavingRegion = false;
    mSavingParcel = false;
    mLastError.clear();
    invalidateParcel();
    mNextPoll = 0.f;
}

void WolfRegionWeather::onParcelChanged()
{
    mAwaitingParcel = false;
    mParcelGate.advance();
    ++mParcelSaveSerial;
    mSavingParcel = false;
    mLastError.clear();
    invalidateParcel();
    refresh(true);
}

/**
 * Is the region IMPOSING weather? Three things have to be true: the grid knows this region, a
 * row exists with an actual kind (empty is "no opinion", which is NOT "clear"), and the owner
 * has turned it on. Otherwise the resident's own menu still decides.
 */
bool WolfRegionWeather::forcedProfile(WolfWeatherProfile& out) const
{
    if (!mHaveRow || !mProfile.mEnabled || mKind.empty()) return false;
    out = mProfile;
    out.clampAll();
    return true;
}

bool WolfRegionWeather::parcelForcedProfile(WolfWeatherProfile& out) const
{
    // Source: weather.php:206-224. `applies` derives from the current region policy and row;
    // explicit clear is an override, while disabled/unset rows inherit.
    if (!mParcelApplies || !wolfWeatherParcelApplies(mAllowParcel, mHaveParcel,
                                                     mParcelProfile.mEnabled, !mParcelKind.empty())) return false;
    out = mParcelProfile;
    out.clampAll();
    return true;
}

void WolfRegionWeather::idle()
{
    if (!WolfGrid::isWolfTerritories()) return;
    const std::string id = currentRegionId();
    if (id.empty()) return;
    // Source: LLFrameTimer and the 60-second weather polling contract above. An attempted target
    // owns the retry clock even when its first request failed; successful-target state is kept
    // separately in mFetchedFor.
    if (id != mAttemptedFor) { refresh(true); return; }
    if (LLFrameTimer::getElapsedSeconds() >= mNextPoll) refresh(false);
}

void WolfRegionWeather::refresh(bool force)
{
    if (!WolfGrid::isWolfTerritories()) return;
    const std::string id = currentRegionId();
    if (id.empty()) return;
    if (!force && id == mAttemptedFor && LLFrameTimer::getElapsedSeconds() < mNextPoll) return;
    if (mFetchesInFlight >= MAX_FETCHES_IN_FLIGHT)
    {
        mFetchPending = true;
        mFetchPendingForce = mFetchPendingForce || force;
        mAttemptedFor = id;
        mNextPoll = (F32)LLFrameTimer::getElapsedSeconds() + POLL_SECS;
        return;
    }
    bool include_parcel = !mAwaitingParcel;
    F32 x = 0.f, y = 0.f;
    if (include_parcel)
    {
        LLViewerRegion* region = gAgent.getRegion();
        if (!region) return;
        // Source: llviewerregion.cpp:2319-2324 and llagent.cpp:1463-1477. The API accepts the
        // avatar's current position in the current region, including variable-size regions.
        const LLVector3 local = region->getPosRegionFromGlobal(gAgent.getPositionGlobal());
        x = local.mV[VX];
        y = local.mV[VY];
    }
    ++mFetchesInFlight;
    mAttemptedFor = id;
    mNextPoll = (F32)LLFrameTimer::getElapsedSeconds() + POLL_SECS;
    const auto region_generation = mRegionGate.current();
    const auto parcel_generation = mParcelGate.current();
    const U64 fetch_serial = ++mNextFetchSerial;
    LLCoros::instance().launch("WolfRegionWeather", [id, include_parcel, x, y, region_generation,
                                                       parcel_generation, fetch_serial]() {
        WolfRegionWeather::fetchCoro(id, include_parcel, x, y, region_generation,
                                     parcel_generation, fetch_serial);
    });
}

// static
void WolfRegionWeather::fetchCoro(std::string region_id, bool include_parcel, F32 x, F32 y,
                                  WolfWeatherAsyncGate::generation_t region_generation,
                                  WolfWeatherAsyncGate::generation_t parcel_generation, U64 fetch_serial)
{
    // Source: weather.php:311-351. Coordinates are sent only after the agent-parcel callback;
    // %.9g round-trips the captured F32 without selecting or inventing a parcel identifier.
    std::string url = std::string(API_URL) + "?region=" + region_id;
    if (include_parcel) url += "&x=" + llformat("%.9g", (double)x) + "&y=" + llformat("%.9g", (double)y);
    LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
        std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfRegionWeather", LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
    LLCore::HttpOptions::ptr_t options = std::make_shared<LLCore::HttpOptions>();
    options->setTimeout(20);
    LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
    headers->append(HTTP_OUT_HEADER_ACCEPT, "application/json");

    LLSD result = adapter->getRawAndSuspend(request, url, options, headers);
    LLSD httpResults = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(httpResults);

    WolfRegionWeather& self = WolfRegionWeather::instance();
    // Source: LLAgent::setRegion() (llagent.cpp:1178-1315). UUID equality alone cannot identify
    // the current visit after A -> B -> A. Completion order also prevents an older concurrent
    // read from replacing a newer result or failure.
    const bool current_region = currentRegionId() == region_id
                             && self.mRegionGate.accepts(region_generation);
    if (!current_region || fetch_serial < self.mAcceptedFetchSerial)
    {
        self.finishFetch();
        return;
    }
    self.mAcceptedFetchSerial = fetch_serial;
    if (!status)
    {
        // Off grid, offline, or the service having a moment. Not worth a dialog: the user did not
        // ask for this and the region simply has no weather as far as the viewer is concerned.
        if (current_region)
        {
            self.mLastError = status.toString();
            ++self.mGeneration;
        }
        self.finishFetch();
        return;
    }
    LLSD reply;
    if (result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
    {
        reply = json_to_llsd(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary());
    }
    if (!reply.isMap() || !reply["success"].asBoolean() || !reply["regions"].isArray())
    {
        if (current_region) { self.mLastError = "The weather service returned an unreadable reply."; ++self.mGeneration; }
        self.finishFetch();
        return;
    }

    const LLSD& regions = reply["regions"];
    if (regions.size() == 0)
    {
        self.finishFetch();
        return;
    }
    const LLSD& r = regions[0];
    const S32 region_version = r["version"].asInteger();
    if (self.mHaveRow && region_version < self.mVersion) { self.finishFetch(); return; }
    self.mHaveRow    = true;
    self.mRegionName = r["name"].asString();
    self.mVersion    = region_version;
    self.mAllowParcel = !r.has("allowParcel") || r["allowParcel"].asBoolean();
    // A null kind is "the region has no opinion" — deliberately not turned into "clear". The
    // rest of the profile (the look and the sound) is always complete, because those are the
    // estate owner's art direction and apply even when the region is not forcing the weather.
    self.mKind       = (!r.has("kind") || r["kind"].isUndefined()) ? std::string() : r["kind"].asString();
    self.mProfile    = WolfWeatherProfile();
    self.mProfile.fromLLSD(r);
    self.mLastError.clear();
    if (include_parcel && self.mParcelGate.accepts(parcel_generation))
    {
        const LLSD& parcel = reply["parcel"];
        if (parcel.isMap() && parcel.has("parcel") && parcel["parcel"].isDefined())
        {
            const S32 parcel_version = parcel["version"].asInteger();
            const bool stale_parcel = self.mHaveParcel && parcel["parcel"].asString() == self.mParcelId
                                   && parcel_version < self.mParcelVersion;
            if (!stale_parcel)
            {
                self.mHaveParcel = true;
                self.mParcelId = parcel["parcel"].asString();
                self.mParcelName = parcel["name"].asString();
                self.mParcelGroupOwned = parcel["groupOwned"].asBoolean();
                self.mParcelVersion = parcel_version;
                self.mParcelKind = (!parcel.has("kind") || !parcel["kind"].isDefined()) ? std::string() : parcel["kind"].asString();
                self.mParcelProfile = WolfWeatherProfile();
                self.mParcelProfile.fromLLSD(parcel);
                // The UUID/version and these coordinates were accepted in one server response. Saves
                // must keep this tuple; fresh avatar coordinates have not proven the same parcel.
                self.mParcelX = x;
                self.mParcelY = y;
            }
        }
        else
        {
            self.invalidateParcel();
        }
    }
    // Source: weather.php:212-224. Parcel POST versions are independent and carry no region
    // policy revision, so the latest accepted region allowParcel state always gates this row.
    self.recomputeParcelApplies();
    self.mFetchedFor = region_id;
    self.mGeneration++;
    LL_INFOS("WolfWeather") << "region weather: " << (self.mKind.empty() ? "unset" : self.mKind)
                            << " level " << self.mProfile.mLevel
                            << (self.mProfile.mEnabled ? " (on)" : " (off)")
                            << " brightness " << self.mProfile.mBrightness
                            << "% density " << self.mProfile.mDensity
                            << "% sound " << (self.mProfile.mSound ? self.mProfile.mSoundPreset : "off")
                            << LL_ENDL;
    self.applyToWeather();
    self.finishFetch();
}

void WolfRegionWeather::finishFetch()
{
    if (mFetchesInFlight > 0) --mFetchesInFlight;
    if (mFetchPending && mFetchesInFlight < MAX_FETCHES_IN_FLIGHT)
    {
        const bool force = mFetchPendingForce;
        mFetchPending = false;
        mFetchPendingForce = false;
        refresh(force);
    }
}

void WolfRegionWeather::recomputeParcelApplies()
{
    mParcelApplies = wolfWeatherParcelApplies(mAllowParcel, mHaveParcel,
                                              mParcelProfile.mEnabled, !mParcelKind.empty());
}

void WolfRegionWeather::applyPush(const LLSD& row)
{
    if (!row.isMap()) return;
    // Source: weather.php:430-499. Region revisions are monotonic across GET, POST and the
    // simulator's pushed copy; an older wire message cannot roll the local row backwards.
    const S32 pushed_version = row.has("version") ? row["version"].asInteger() : 0;
    if (mHaveRow && row.has("version") && pushed_version < mVersion) return;
    mHaveRow = true;
    mKind = (!row.has("kind") || row["kind"].isUndefined()) ? std::string() : row["kind"].asString();
    if (row.has("version")) mVersion = pushed_version;
    if (row.has("allowParcel")) mAllowParcel = row["allowParcel"].asBoolean();
    mProfile = WolfWeatherProfile();
    mProfile.fromLLSD(row);
    recomputeParcelApplies();
    mLastError.clear();
    mGeneration++;
    LL_INFOS("WolfWeather") << "region weather pushed by the sim: "
                            << (mKind.empty() ? "unset" : mKind) << LL_ENDL;
    applyToWeather();
}

void WolfRegionWeather::applyToWeather()
{
    // wolfweather.cpp owns the precedence rule; this only tells it the region's answer changed.
    WolfWeather::instance().onRegionWeatherChanged();
}

U64 WolfRegionWeather::saveRegion(const WolfWeatherProfile& profile, bool allow_parcel,
                                  const WolfWeatherSaveTarget& captured_target)
{
    if (!WolfGrid::isWolfTerritories()) { mLastError = "These tools are only available on Wolf Territories Grid."; ++mGeneration; return 0; }
    if (mSavingRegion) { mLastError = "A region weather change is already saving."; ++mGeneration; return 0; }
    const std::string id = currentRegionId();
    if (id.empty() || captured_target.mEditorTarget != regionTarget()
        || captured_target.mRegionId != id
        || !mRegionGate.accepts(captured_target.mVisitGeneration))
    {
        mLastError = "The region changed before this edit could be saved.";
        ++mGeneration;
        return 0;
    }
    mSavingRegion = true;
    mLastError.clear();

    WolfWeatherProfile p = profile;
    p.clampAll();
    LLSD body = p.toLLSD();
    // Source: weather.php:383-505. Region and parcel rows have independent versions and scope
    // is explicit; allowParcel is an actual JSON boolean on region saves.
    body["scope"] = "region";
    body["region"]  = id;
    body["version"] = captured_target.mVersion;
    body["allowParcel"] = allow_parcel;
    const std::string text = boost::json::serialize(LlsdToJson(body));
    const U64 save_serial = ++mRegionSaveSerial;
    const auto generation = captured_target.mVisitGeneration;
    LLCoros::instance().launch("WolfRegionWeatherSave", [text, id, generation, save_serial]() {
        WolfRegionWeather::saveCoro(text, "region", id, std::string(), generation, save_serial);
    });
    return save_serial;
}

U64 WolfRegionWeather::saveParcel(const WolfWeatherProfile& profile,
                                  const WolfWeatherSaveTarget& captured_target)
{
    if (!WolfGrid::isWolfTerritories()) { mLastError = "These tools are only available on Wolf Territories Grid."; ++mGeneration; return 0; }
    if (mSavingParcel) { mLastError = "A parcel weather change is already saving."; ++mGeneration; return 0; }
    const std::string id = currentRegionId();
    // Source: llviewerparcelmgr.cpp:1860-1894 updates the agent-parcel record and emits
    // changeParcels() after the simulator sends the same parcel bitmap. That notification
    // advances the visit gate even when the parcel UUID is unchanged. The UUID/region pair
    // identifies the saved row; the POST version check still rejects a genuinely newer edit.
    if (!mHaveParcel || captured_target.mRegionId != id || captured_target.mParcelId != mParcelId)
    {
        mLastError = "The occupied parcel changed before this edit could be saved.";
        ++mGeneration;
        return 0;
    }
    mSavingParcel = true;
    mLastError.clear();
    WolfWeatherProfile p = profile;
    p.clampAll();
    LLSD body = p.toLLSD();
    body["scope"] = "parcel";
    body["region"] = id;
    body["x"] = captured_target.mX;
    body["y"] = captured_target.mY;
    body["version"] = captured_target.mVersion;
    const std::string parcel_id = captured_target.mParcelId;
    const std::string text = boost::json::serialize(LlsdToJson(body));
    const U64 save_serial = ++mParcelSaveSerial;
    const auto generation = captured_target.mVisitGeneration;
    LLCoros::instance().launch("WolfParcelWeatherSave", [text, id, parcel_id, generation, save_serial]() {
        WolfRegionWeather::saveCoro(text, "parcel", id, parcel_id, generation, save_serial);
    });
    return save_serial;
}

// static
void WolfRegionWeather::saveCoro(std::string body, std::string scope, std::string region_id,
                                 std::string parcel_id,
                                 WolfWeatherAsyncGate::generation_t target_generation,
                                 U64 save_serial)
{
    LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
        std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfRegionWeather", LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
    LLCore::HttpOptions::ptr_t options = std::make_shared<LLCore::HttpOptions>();
    options->setTimeout(30);
    LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
    headers->append(HTTP_OUT_HEADER_CONTENT_TYPE, "application/json");
    headers->append("X-Wolf-Agent", gAgentID.asString());
    headers->append("X-Wolf-Session", gAgentSessionID.asString());
    LLCore::BufferArray::ptr_t raw(new LLCore::BufferArray());
    raw->append(body.data(), body.size());

    LLSD result = adapter->postRawAndSuspend(request, API_URL, raw, options, headers);
    LLSD httpResults = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(httpResults);
    LLSD reply;
    if (result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
    {
        reply = json_to_llsd(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary());
    }

    WolfRegionWeather& self = WolfRegionWeather::instance();
    const bool current_save = (scope == "region") ? save_serial == self.mRegionSaveSerial
                                                   : save_serial == self.mParcelSaveSerial;
    if (scope == "region" && current_save) self.mSavingRegion = false;
    if (scope == "parcel" && current_save) self.mSavingParcel = false;
    if (!status || !reply.isMap() || !reply["success"].asBoolean())
    {
        const std::string error = reply.isMap() && reply.has("error") ? reply["error"].asString()
                                  : !status ? status.toString()
                                            : "The weather service returned an unreadable reply.";
        // User-initiated failures remain visible even if their panel was hidden while the
        // request was in flight. The current panel also renders mLastError persistently.
        LLNotificationsUtil::add("GenericAlertOK", LLSD().with("MESSAGE", "Could not save weather: " + error));
        if (current_save)
        {
            if (scope == "region") self.mRegionSaveResult.record(save_serial, false, error);
            else self.mParcelSaveResult.record(save_serial, false, error);
            self.mLastError = error;
            self.mGeneration++;
        }
        return;
    }
    // Source: weather.php:492-505. Apply a save reply only to the immutable target it was sent
    // for. The database save may have succeeded after movement, but it cannot become new-target
    // state in this viewer.
    const bool in_region = currentRegionId() == region_id;
    const bool same_region = in_region && self.mRegionGate.accepts(target_generation);
    const bool same_parcel = in_region && self.mParcelGate.accepts(target_generation)
                          && self.mParcelId == parcel_id;
    if (current_save && scope == "region" && same_region)
    {
        // Source: weather.php:451-499. Versions increase on every accepted POST. A delayed reply
        // cannot roll back a newer poll or simulator push already applied locally.
        const S32 reply_version = reply["version"].asInteger();
        if (!self.mHaveRow || reply_version >= self.mVersion)
        {
            self.mHaveRow = true;
            self.mVersion = reply_version;
            self.mKind = (!reply.has("kind") || !reply["kind"].isDefined()) ? std::string() : reply["kind"].asString();
            self.mAllowParcel = reply["allowParcel"].asBoolean();
            self.mProfile = WolfWeatherProfile();
            self.mProfile.fromLLSD(reply);
            self.recomputeParcelApplies();
        }
    }
    else if (current_save && scope == "parcel" && same_parcel)
    {
        const S32 reply_version = reply["version"].asInteger();
        if (!self.mHaveParcel || reply_version >= self.mParcelVersion)
        {
            self.mHaveParcel = true;
            self.mParcelVersion = reply_version;
            self.mParcelKind = (!reply.has("kind") || !reply["kind"].isDefined()) ? std::string() : reply["kind"].asString();
            self.mParcelName = reply["name"].asString();
            self.mParcelGroupOwned = reply["groupOwned"].asBoolean();
            self.mParcelProfile = WolfWeatherProfile();
            self.mParcelProfile.fromLLSD(reply);
            self.recomputeParcelApplies();
        }
    }
    if (current_save)
    {
        if (scope == "region") self.mRegionSaveResult.record(save_serial, true, std::string());
        else self.mParcelSaveResult.record(save_serial, true, std::string());
        self.mLastError.clear();
        self.mGeneration++;
        if ((scope == "region" && same_region) || (scope == "parcel" && same_parcel)) self.applyToWeather();
    }
}


// ═══════════════════════════════════════════════════════════════════════════════════════════
// About Land > Weather — the panel
// ═══════════════════════════════════════════════════════════════════════════════════════════

WolfPanelWeather::WolfPanelWeather(Scope scope)
:   mScope(scope),
    mPreviewOwner(WolfWeather::instance().acquirePreviewOwner())
{}

WolfPanelWeather::~WolfPanelWeather()
{
    // An unsaved edit is previewed on THIS viewer's own sky. It must not outlive the panel:
    // weather that followed you out of the window would be indistinguishable from saved
    // weather, and there would be no way to get rid of it.
    cancelPresetLoad();
    clearPreview();
    if (mPanelRegionChangedConnection.connected()) mPanelRegionChangedConnection.disconnect();
    if (mPanelParcelChangedConnection.connected()) mPanelParcelChangedConnection.disconnect();
}

bool WolfPanelWeather::postBuild()
{
    // Source: llagent.cpp parcel/region signals. Preview cleanup runs in the crossing callback,
    // before a subsequent draw can display or adopt the new target.
    mPanelRegionChangedConnection = gAgent.addRegionChangedCallback([this]() {
        cancelPresetLoad();
        clearPreview();
    });
    mPanelParcelChangedConnection = gAgent.addParcelChangedCallback([this]() {
        if (mScope == PARCEL)
        {
            cancelPresetLoad();
            clearPreview();
        }
    });
    mPreset         = getChild<LLComboBox>("weather_preset");
    mKind           = getChild<LLComboBox>("weather_kind");
    mEnabled        = getChild<LLCheckBoxCtrl>("weather_enabled");
    mUseRegion      = getChild<LLCheckBoxCtrl>("weather_use_region");
    mAllowParcel    = getChild<LLCheckBoxCtrl>("weather_allow_parcel");
    mLadder         = getChild<LLRadioGroup>("weather_level");
    mBrightness     = getChild<LLSliderCtrl>("weather_brightness");
    mTint           = getChild<LLColorSwatchCtrl>("weather_tint");
    mTintAmount     = getChild<LLSliderCtrl>("weather_tint_amount");
    mMoveSpeed      = getChild<LLSliderCtrl>("weather_move");
    mDensity        = getChild<LLSliderCtrl>("weather_density");
    mVelocity       = getChild<LLSliderCtrl>("weather_velocity");
    mSize           = getChild<LLSliderCtrl>("weather_size");
    mSound          = getChild<LLCheckBoxCtrl>("weather_sound");
    mVolume         = getChild<LLSliderCtrl>("weather_volume");
    mAmbience       = getChild<LLComboBox>("weather_ambience");
    mNearLayer      = getChild<LLComboBox>("weather_near");
    mApply          = getChild<LLButton>("weather_apply");
    mRegionLine     = getChild<LLTextBox>("weather_region_line");
    mStatus         = getChild<LLTextBox>("weather_status");
    mParticlesTitle = getChild<LLTextBox>("weather_hdr_particles");

    childSetAction("weather_preset_apply", [this](LLUICtrl*, const LLSD&) { onApplyPreset(); });
    childSetAction("weather_inv_preset",   [this](LLUICtrl*, const LLSD&) { onInventoryPreset(); });
    childSetAction("weather_reset",        [this](LLUICtrl*, const LLSD&) { onReset(); });
    childSetAction("weather_apply",        [this](LLUICtrl*, const LLSD&) { onApply(); });
    childSetAction("weather_revert",       [this](LLUICtrl*, const LLSD&) { onRevert(); });

    // Every control that changes the sky goes through one handler, so a control cannot be added
    // and quietly forget to preview or to mark the edit unsaved.
    auto changed = [this](LLUICtrl*, const LLSD&) { onControlChanged(); };
    mEnabled->setCommitCallback(changed);
    mUseRegion->setCommitCallback(changed);
    mAllowParcel->setCommitCallback(changed);
    mLadder->setCommitCallback(changed);
    mBrightness->setCommitCallback(changed);
    mTint->setCommitCallback(changed);
    mTintAmount->setCommitCallback(changed);
    mMoveSpeed->setCommitCallback(changed);
    mDensity->setCommitCallback(changed);
    mVelocity->setCommitCallback(changed);
    mSize->setCommitCallback(changed);
    mSound->setCommitCallback(changed);
    mVolume->setCommitCallback(changed);
    mAmbience->setCommitCallback(changed);
    mNearLayer->setCommitCallback(changed);
    // The KIND relabels the ladder and the particle group as well, so it has its own.
    mKind->setCommitCallback([this](LLUICtrl*, const LLSD&) { onKindChanged(); });

    mEnabled->setVisible(mScope == REGION);
    mAllowParcel->setVisible(mScope == REGION);
    mUseRegion->setVisible(mScope == PARCEL);
    adoptTarget(false);
    return true;
}

bool WolfPanelWeather::canEdit() const
{
    LLViewerRegion* region = gAgent.getRegion();
    if (!WolfGrid::isWolfTerritories() || !region) return false;
    if (region->canManageEstate()) return true;
    if (mScope == REGION) return false;
    // Source: weather.php:396-417 and llviewerparcelmgr.h:162. Courtesy-only permission uses
    // the occupied parcel, never selected land; group-owned parcels remain locked.
    LLParcel* parcel = LLViewerParcelMgr::getInstance()->getAgentParcel();
    return WolfRegionWeather::instance().haveParcel()
        && WolfRegionWeather::instance().allowParcel()
        && parcel && !parcel->getIsGroupOwned() && parcel->getOwnerID() == gAgentID;
}

std::string WolfPanelWeather::currentTarget() const
{
    return mScope == REGION ? WolfRegionWeather::instance().regionTarget()
                            : WolfRegionWeather::instance().parcelTarget();
}

WolfWeatherSaveTarget WolfPanelWeather::currentSaveTarget() const
{
    return mScope == REGION ? WolfRegionWeather::instance().regionSaveTarget()
                            : WolfRegionWeather::instance().parcelSaveTarget();
}

const WolfWeatherProfile& WolfPanelWeather::currentStored() const
{
    return mScope == REGION ? WolfRegionWeather::instance().stored()
                            : WolfRegionWeather::instance().parcelStored();
}

bool WolfPanelWeather::currentAllowParcel() const
{
    return WolfRegionWeather::instance().allowParcel();
}

bool WolfPanelWeather::dirty() const
{
    return mEdit != mStored || (mScope == REGION && mEditAllowParcel != mStoredAllowParcel);
}

void WolfPanelWeather::adoptTarget(bool announce_discard)
{
    const bool discarded = announce_discard && dirty();
    cancelPresetLoad();
    clearPreview();
    mTarget = currentTarget();
    mSaveTarget = currentSaveTarget();
    mEdit = currentStored();
    mStored = mEdit;
    mEditAllowParcel = currentAllowParcel();
    mStoredAllowParcel = mEditAllowParcel;
    mSavePending = false;
    writeControls();
    if (discarded) setStatus(getString("str_target_discarded"), false);
}

void WolfPanelWeather::cancelPresetLoad()
{
    // Source: LLView::onVisibilityChange()/isInVisibleChain() (llview.cpp:406,663-700).
    // Closing a parent does not change this panel's own visible bit, so invalidate the editor
    // session and explicitly close its picker.
    mEditGate.newSession();
    if (LLFloater* picker = mPickerHandle.get()) picker->closeFloater();
    mPickerHandle.markDead();
}

void WolfPanelWeather::markEdited()
{
    mEditGate.changed();
    ++mEditRevision;
}

void WolfPanelWeather::setStatus(const std::string& msg, bool error)
{
    if (!mStatus) return;
    mStatus->setText(msg);
    // A failure must be readable; a confirmation may be quiet.
    mStatus->setColor(error ? LLColor4(0.88f, 0.33f, 0.28f, 1.f) : LLColor4(0.79f, 0.64f, 0.15f, 1.f));
}

// ── the controls ───────────────────────────────────────────────────────────────────────────

void WolfPanelWeather::readControls()
{
    mEdit.mKind       = WolfWeatherProfile::kindFromName(mKind->getValue().asString());
    mEdit.mLevel      = mLadder->getValue().asInteger();
    mEdit.mEnabled    = (mScope == REGION) ? mEnabled->getValue().asBoolean()
                                           : !mUseRegion->getValue().asBoolean();
    if (mScope == REGION) mEditAllowParcel = mAllowParcel->getValue().asBoolean();
    mEdit.mBrightness = (S32)mBrightness->getValueF32();
    mEdit.mTint       = LLColor4(mTint->get());
    mEdit.mTintAmount = (S32)mTintAmount->getValueF32();
    mEdit.mMoveSpeed  = (S32)mMoveSpeed->getValueF32();
    mEdit.mDensity    = (S32)mDensity->getValueF32();
    mEdit.mVelocity   = (S32)mVelocity->getValueF32();
    mEdit.mSize       = (S32)mSize->getValueF32();
    mEdit.mSound      = mSound->getValue().asBoolean();
    mEdit.mVolume     = (S32)mVolume->getValueF32();
    mEdit.mSoundPreset = mAmbience->getValue().asString();
    mEdit.mSoundLayer  = mNearLayer->getValue().asString();
    mEdit.clampAll();
}

void WolfPanelWeather::writeControls()
{
    // Setting a control fires its commit callback, which would read the half-written state back
    // out over the rest of the edit. One flag, checked in onControlChanged.
    mWriting = true;
    mKind->setValue(WolfWeatherProfile::kindName(mEdit.mKind));
    mLadder->setValue(mEdit.mLevel);
    mEnabled->setValue(mEdit.mEnabled);
    mUseRegion->setValue(!mEdit.mEnabled);
    mAllowParcel->setValue(mEditAllowParcel);
    mBrightness->setValue((F32)mEdit.mBrightness);
    mTint->set(mEdit.mTint, true);
    mTintAmount->setValue((F32)mEdit.mTintAmount);
    mMoveSpeed->setValue((F32)mEdit.mMoveSpeed);
    mDensity->setValue((F32)mEdit.mDensity);
    mVelocity->setValue((F32)mEdit.mVelocity);
    mSize->setValue((F32)mEdit.mSize);
    mSound->setValue(mEdit.mSound);
    mVolume->setValue((F32)mEdit.mVolume);
    mAmbience->setValue(mEdit.mSoundPreset);
    mNearLayer->setValue(mEdit.mSoundLayer);
    mWriting = false;

    // The ladder and the particle group speak the language of whatever is falling.
    const bool snow = (mEdit.mKind == WolfWeatherProfile::SNOW);
    // A radio_group's items ARE check boxes (llradiogroup.cpp builds LLRadioCtrl from
    // LLCheckBoxCtrl), so each band's text is its check box's label.
    for (S32 n = 1; n <= 4; ++n)
    {
        LLCheckBoxCtrl* box = mLadder->findChild<LLCheckBoxCtrl>(llformat("level%d", n));
        if (!box) continue;
        box->setLabel(WolfWeatherProfile::levelLabel(snow ? WolfWeatherProfile::SNOW
                                                          : WolfWeatherProfile::RAIN, n)
                      + " \u2014 " + WolfWeatherProfile::levelDensity(n));
    }
    if (mParticlesTitle)
    {
        LLStringUtil::format_map_t args;
        args["KIND"] = (mEdit.mKind == WolfWeatherProfile::SNOW) ? "Snow"
                     : (mEdit.mKind == WolfWeatherProfile::RAIN) ? "Rain" : "None";
        mParticlesTitle->setText(getString("str_particles", args));
    }

    const bool editable = canEdit();
    static const char* const CONTROLS[] = {
        "weather_preset", "weather_preset_apply", "weather_inv_preset", "weather_kind",
        "weather_enabled", "weather_use_region", "weather_allow_parcel", "weather_level", "weather_brightness", "weather_tint",
        "weather_tint_amount", "weather_move", "weather_density", "weather_velocity",
        "weather_size", "weather_sound", "weather_volume", "weather_ambience",
        "weather_near", "weather_reset", "weather_apply",
    };
    for (const char* name : CONTROLS)
    {
        if (LLView* v = findChild<LLView>(name)) v->setEnabled(editable);
    }
    // Revert stays live for everyone: it only drops a local preview.
}

void WolfPanelWeather::updatePreview()
{
    if (!WolfGrid::isWolfTerritories()) { setStatus("These tools are only available on Wolf Territories Grid.", true); return; }
    WolfWeatherProfile p = mEdit;
    // A preview is always ON, whatever the "Apply Global Weather" box says: the box decides
    // what OTHER people get, and previewing nothing would make the slider look broken.
    p.mEnabled = true;
    WolfWeather::instance().setPreview(mPreviewOwner, &p);
}

void WolfPanelWeather::clearPreview()
{
    WolfWeather::instance().setPreview(mPreviewOwner, nullptr);
}

void WolfPanelWeather::onVisibilityChange(bool visible)
{
    if (!visible)
    {
        cancelPresetLoad();
        clearPreview();
    }
    else
    {
        // Source: LLAgent::setRegion() visit ordering (llagent.cpp:1178-1315). Adopt the current
        // generation before a hidden A -> B -> A draft can be previewed again.
        if (currentTarget() != mTarget) adoptTarget(true);
        if (dirty()) updatePreview();
    }
    LLPanel::onVisibilityChange(visible);
}

void WolfPanelWeather::onControlChanged()
{
    if (!WolfGrid::isWolfTerritories()) { setStatus("These tools are only available on Wolf Territories Grid.", true); return; }
    if (mWriting) return;
    readControls();
    markEdited();
    updatePreview();
    setStatus(LLStringUtil::null, false);
}

void WolfPanelWeather::onKindChanged()
{
    if (!WolfGrid::isWolfTerritories()) { setStatus("These tools are only available on Wolf Territories Grid.", true); return; }
    if (mWriting) return;
    const WolfWeatherProfile::Kind was = mEdit.mKind;
    readControls();
    // The tint is chosen for one kind; switching kind takes that kind's neutral colour rather
    // than leaving blue snow behind.
    if (mEdit.mKind != was) mEdit.mTint = WolfWeatherProfile::neutralColor(mEdit.mKind);
    markEdited();
    writeControls();
    updatePreview();
    setStatus(LLStringUtil::null, false);
}

// ── presets ────────────────────────────────────────────────────────────────────────────────

void WolfPanelWeather::onApplyPreset()
{
    if (!WolfGrid::isWolfTerritories()) { setStatus("These tools are only available on Wolf Territories Grid.", true); return; }
    const std::string id = mPreset->getValue().asString();
    if (id.empty()) return;
    mEdit = WolfWeatherProfile::applyPreset(mEdit, id);
    markEdited();
    writeControls();
    updatePreview();
    setStatus(getString("str_preset_loaded"), false);
}

void WolfPanelWeather::onReset()
{
    if (!WolfGrid::isWolfTerritories()) { setStatus("These tools are only available on Wolf Territories Grid.", true); return; }
    // Reset is the DEFAULTS, keeping what falls; Revert is what puts the grid's row back. Two
    // buttons that did the same thing would be worse than useless.
    WolfWeatherProfile fresh;
    fresh.mKind    = mEdit.mKind;
    fresh.mLevel   = mEdit.mLevel;
    fresh.mEnabled = mEdit.mEnabled;
    fresh.mTint    = WolfWeatherProfile::neutralColor(fresh.mKind);
    mEdit = fresh;
    markEdited();
    writeControls();
    updatePreview();
    setStatus(getString("str_reset"), false);
}

void WolfPanelWeather::onApply()
{
    if (!WolfGrid::isWolfTerritories()) { setStatus("These tools are only available on Wolf Territories Grid.", true); return; }
    if (mSavePending) { setStatus(getString("str_saving"), false); return; }
    if (!canEdit()) { setStatus(getString(mScope == REGION ? "str_read_only" : "str_read_only_parcel"), true); return; }
    readControls();
    WolfRegionWeather& rw = WolfRegionWeather::instance();
    // Source: weather.php:383-505. Apply uses the immutable row revision and accepted parcel
    // point captured when this editor baseline was adopted. A later poll cannot turn an old
    // draft into a write against a newer row.
    const U64 save_serial = (mScope == REGION)
                          ? rw.saveRegion(mEdit, mEditAllowParcel, mSaveTarget)
                          : rw.saveParcel(mEdit, mSaveTarget);
    mShownGeneration = WolfRegionWeather::instance().generation();
    if (save_serial == 0)
    {
        mSavePending = false;
        setStatus(getString("str_save_failed") + " " + rw.lastError(), true);
    }
    else
    {
        mSubmittedEditRevision = mEditRevision;
        mSubmittedSaveSerial = save_serial;
        mSubmittedProfile = mEdit;
        mSubmittedAllowParcel = mEditAllowParcel;
        mSavePending = true;
        setStatus(getString("str_saving"), false);
    }
}

void WolfPanelWeather::onRevert()
{
    adoptTarget(false);
    setStatus(LLStringUtil::null, false);
}

// ── refresh ────────────────────────────────────────────────────────────────────────────────

void WolfPanelWeather::refresh()
{
    WolfRegionWeather::instance().refresh(false);
    // onOpen(), focus and parcel callbacks can refresh outside the UI render pass.
    // Let draw() consume the new generation when the renderer has bound its shader.
}

void WolfPanelWeather::draw()
{
    if (!WolfGrid::isWolfTerritories()) { setStatus("These tools are only available on Wolf Territories Grid.", true); LLPanel::draw(); return; }
    WolfRegionWeather& rw = WolfRegionWeather::instance();
    const bool target_changed = currentTarget() != mTarget;
    if (target_changed) adoptTarget(true);
    if (rw.generation() != mShownGeneration)
    {
        mShownGeneration = rw.generation();

        const WolfWeatherSaveResult& save_result = (mScope == REGION)
                                                   ? rw.regionSaveResult() : rw.parcelSaveResult();
        const bool saving = (mScope == REGION) ? rw.savingRegion() : rw.savingParcel();
        if (mSavePending && save_result.completed(mSubmittedSaveSerial))
        {
            if (!save_result.succeeded())
            {
                mSavePending = false;
                setStatus(getString("str_save_failed") + " " + save_result.error(), true);
            }
            else
            {
                const WolfWeatherProfile fresh = currentStored();
                const bool fresh_allow = currentAllowParcel();
                // Source: weather.php:430-499. A successful POST advances exactly the submitted
                // revision once. Newer edits made while it was in flight keep that accepted
                // baseline; they are never rebound to an unrelated poll result.
                if (mEditRevision == mSubmittedEditRevision)
                {
                    mEdit = fresh;
                    mStored = fresh;
                    mEditAllowParcel = fresh_allow;
                    mStoredAllowParcel = fresh_allow;
                    mSaveTarget = currentSaveTarget();
                    clearPreview();
                    writeControls();
                }
                else
                {
                    mStored = mSubmittedProfile;
                    mStoredAllowParcel = mSubmittedAllowParcel;
                    ++mSaveTarget.mVersion;
                }
                mSavePending = false;
                setStatus(getString("str_saved"), false);
            }
        }
        else if (!mSavePending && !rw.lastError().empty())
        {
            setStatus(getString("str_save_failed") + " " + rw.lastError(), true);
        }
        else if (!mSavePending && !saving && !dirty())
        {
            const WolfWeatherProfile fresh = currentStored();
            const bool fresh_allow = currentAllowParcel();
            // A passive refresh may replace a clean baseline. While dirty, both the visual
            // baseline and its version/point tuple remain the ones editing began from.
            mEdit = fresh;
            mStored = fresh;
            mEditAllowParcel = fresh_allow;
            mStoredAllowParcel = fresh_allow;
            mSaveTarget = currentSaveTarget();
            writeControls();
        }

        LLStringUtil::format_map_t args;
        args["REGION"] = rw.onGrid() ? rw.regionName() : std::string("this region");
        args["PARCEL"] = rw.haveParcel() ? rw.parcelName() : std::string("the parcel you occupy");
        const std::string kind = (mScope == REGION) ? rw.kind() : rw.parcelKind();
        const bool available = (mScope == REGION) ? rw.onGrid() : rw.haveParcel();
        if (!available)
        {
            args["STATE"] = getString(mScope == REGION ? "str_off_grid" : "str_parcel_loading");
        }
        else if (kind.empty())
        {
            args["STATE"] = getString("str_unset");
        }
        else
        {
            const WolfWeatherProfile& p = currentStored();
            LLStringUtil::format_map_t sargs;
            sargs["KIND"]  = kind;
            sargs["LEVEL"] = (p.mKind == WolfWeatherProfile::CLEAR)
                             ? getString("str_state_clear")
                             : WolfWeatherProfile::levelLabel(p.mKind, p.mLevel);
            args["STATE"] = getString(p.mEnabled ? "str_state" : "str_state_off", sargs);
        }
        if (mRegionLine) mRegionLine->setText(getString(mScope == REGION ? "str_region" : "str_parcel", args));

        if (!target_changed && !canEdit() && mStatus && mStatus->getText().empty())
        {
            setStatus(getString(mScope == REGION ? "str_read_only" : "str_read_only_parcel"), false);
        }
    }
    LLPanel::draw();
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
// "Use Inventory Preset" — a weather profile read out of a notecard
// ═══════════════════════════════════════════════════════════════════════════════════════════

namespace
{
    /**
     * The text of a notecard asset, out of the Linden wrapper.
     *
     * Source: LLPreviewNotecard::onLoadComplete (llpreviewnotecard.cpp:410) for the "Linden text
     * version" test, and LLNotecard's own format:
     *
     *   Linden text version 2
     *   {
     *   LLEmbeddedItems version 1
     *   { count 0 }
     *   Text length <N>
     *   <the text>
     *   }
     *
     * Taking the outermost {...} would be WRONG — the wrapper's own LLEmbeddedItems block is a
     * {...} and would be read as the preset. The text is what follows "Text length <N>".
     */
    std::string notecard_text(const std::string& raw)
    {
        const std::string marker("Text length ");
        const size_t at = raw.find(marker);
        if (at == std::string::npos) return raw;      // not wrapped: plain text, use it as it is
        size_t p = at + marker.size();
        S32 len = 0;
        while (p < raw.size() && raw[p] >= '0' && raw[p] <= '9') { len = len * 10 + (raw[p] - '0'); ++p; }
        if (p < raw.size() && raw[p] == '\r') ++p;
        if (p >= raw.size() || raw[p] != '\n') return raw;
        ++p;
        if (len <= 0 || p + (size_t)len > raw.size()) return raw.substr(p);
        return raw.substr(p, (size_t)len);
    }

    /**
     * The profile out of a notecard's text. The preset is the LAST {...} in it, so a notecard
     * may carry a line of notes above the profile and still work.
     * @return false when there is no readable weather profile in it.
     */
    bool profile_from_notecard(const std::string& raw, WolfWeatherProfile& out)
    {
        const std::string body = notecard_text(raw);
        const size_t first = body.find('{');
        const size_t last = body.rfind('}');
        if (first == std::string::npos || last == std::string::npos || last <= first) return false;
        boost::system::error_code ec;
        boost::json::value v = boost::json::parse(body.substr(first, last - first + 1), ec);
        if (ec) return false;
        const LLSD sd = LlsdFromJson(v);
        if (!sd.isMap() || !sd.has("kind")) return false;
        const std::string kind = sd["kind"].asString();
        if (kind != "clear" && kind != "rain" && kind != "snow") return false;
        out.fromLLSD(sd);
        return true;
    }
}

/**
 * The notecard picker. Built straight from its XML rather than through LLFloaterReg, the way
 * LLFloaterSettingsPicker is (llsettingspicker.cpp:—), because nothing else ever opens it.
 */
class WolfWeatherPresetPicker : public LLFloater
{
public:
    typedef boost::function<void(const WolfWeatherProfile&)> picked_t;

    WolfWeatherPresetPicker(LLView* owner, picked_t cb)
    :   LLFloater(LLSD()), mOwnerHandle(owner->getHandle()), mPicked(cb)
    {
        buildFromFile("floater_wolf_weather_preset.xml");
    }

    bool postBuild() override
    {
        mList = getChild<LLScrollListCtrl>("preset_list");
        mStatus = getChild<LLTextBox>("preset_status");
        childSetAction("preset_load",   [this](LLUICtrl*, const LLSD&) { onLoad(); });
        childSetAction("preset_cancel", [this](LLUICtrl*, const LLSD&) { closeFloater(); });
        mList->setDoubleClickCallback(boost::bind(&WolfWeatherPresetPicker::onLoad, this));
        fill();
        return true;
    }

    /** The one instance at a time, so a second click re-focuses rather than stacking windows. */
    static WolfWeatherPresetPicker* open(LLView* owner, picked_t cb, LLHandle<LLFloater>& slot)
    {
        WolfWeatherPresetPicker* p = static_cast<WolfWeatherPresetPicker*>(slot.get());
        if (!p)
        {
            p = new WolfWeatherPresetPicker(owner, cb);
            slot = p->getHandle();
        }
        else
        {
            p->mOwnerHandle = owner->getHandle();
            p->mPicked = cb;
        }
        p->openFloater();
        p->setFocus(true);
        return p;
    }

private:
    struct AssetRequest
    {
        LLHandle<LLFloater> mPicker;
        WolfWeatherAsyncGate::generation_t mGeneration;
    };

    void fill()
    {
        LLViewerInventoryCategory::cat_array_t cats;
        LLViewerInventoryItem::item_array_t items;
        LLIsType is_notecard(LLAssetType::AT_NOTECARD);
        gInventory.collectDescendentsIf(gInventory.getRootFolderID(), cats, items,
                                        LLInventoryModel::EXCLUDE_TRASH, is_notecard);
        mList->deleteAllItems();
        for (const auto& item : items)
        {
            if (!item) continue;
            LLSD row;
            row["id"] = item->getUUID();
            row["columns"][0]["column"] = "name";
            row["columns"][0]["value"] = item->getName();
            mList->addElement(row);
        }
        mList->sortByColumnIndex(0, true);
        if (mList->isEmpty()) mStatus->setText(getString("str_none"));
    }

    void onLoad()
    {
        LLScrollListItem* sel = mList->getFirstSelected();
        if (!sel) return;
        LLViewerInventoryItem* item = gInventory.getItem(sel->getUUID());
        if (!item) return;
        if (!gAgent.allowOperation(PERM_COPY, item->getPermissions(), GP_OBJECT_MANIPULATE)
            && !gAgent.isGodlike())
        {
            mStatus->setText(getString("str_no_perm"));
            return;
        }
        LLStringUtil::format_map_t args;
        args["NAME"] = item->getName();
        mStatus->setText(getString("str_loading", args));

        // The reply comes back on a C callback with a void*, so the picker is identified by its
        // own handle rather than a raw pointer: the window may be gone by the time it lands.
        mLoadGate.advance();
        AssetRequest* load = new AssetRequest{getHandle(), mLoadGate.current()};
        gAssetStorage->getInvItemAsset(LLHost(),          // the agent's own inventory, not a task
                                       gAgent.getID(), gAgent.getSessionID(),
                                       item->getPermissions().getOwner(),
                                       LLUUID::null,      // no task: it is in inventory
                                       item->getUUID(), item->getAssetUUID(), item->getType(),
                                       &WolfWeatherPresetPicker::onAssetLoaded, (void*)load, true);
    }

    static void onAssetLoaded(const LLUUID& asset_uuid, LLAssetType::EType type,
                              void* user_data, S32 status, LLExtStat)
    {
        std::unique_ptr<AssetRequest> load((AssetRequest*)user_data);
        WolfWeatherPresetPicker* self = static_cast<WolfWeatherPresetPicker*>(load->mPicker.get());
        if (!self) return;                       // the picker was closed while the asset flew
        if (!self->isInVisibleChain()) return;   // its owner hid/reverted/superseded this load
        if (!self->mLoadGate.accepts(load->mGeneration)) return; // a newer card was selected
        if (status != 0)
        {
            self->mStatus->setText(self->getString("str_failed"));
            return;
        }
        LLFileSystem file(asset_uuid, type, LLFileSystem::READ);
        const S32 len = file.getSize();
        if (len <= 0) { self->mStatus->setText(self->getString("str_failed")); return; }
        std::vector<char> buffer((size_t)len + 1, 0);
        file.read((U8*)&buffer[0], len);
        buffer[(size_t)len] = 0;

        WolfWeatherProfile profile;
        if (!profile_from_notecard(std::string(&buffer[0], (size_t)len), profile))
        {
            self->mStatus->setText(self->getString("str_failed"));
            return;
        }
        if (self->mPicked) self->mPicked(profile);
        self->closeFloater();
    }

    LLHandle<LLView> mOwnerHandle;
    picked_t         mPicked;
    WolfWeatherAsyncGate mLoadGate;
    LLScrollListCtrl* mList = nullptr;
    LLTextBox*        mStatus = nullptr;
};

void WolfPanelWeather::onInventoryPreset()
{
    if (!WolfGrid::isWolfTerritories()) { setStatus("These tools are only available on Wolf Territories Grid.", true); return; }
    cancelPresetLoad();
    const LLHandle<LLView> owner = getHandle();
    const std::string target = mTarget;
    const WolfWeatherEditGate::token_t edit = mEditGate.capture();
    WolfWeatherPresetPicker::open(this,
        [owner, target, edit](const WolfWeatherProfile& p)
        {
            // Source: LLView::isInVisibleChain() (llview.cpp:406) and LLHandle semantics used by
            // the picker above. Parent-floater hiding, a changed target, Revert and a newer edit
            // all invalidate this exact completion.
            WolfPanelWeather* panel = static_cast<WolfPanelWeather*>(owner.get());
            if (!panel || !panel->isInVisibleChain() || panel->mTarget != target
                || panel->currentTarget() != target || !panel->mEditGate.accepts(edit)) return;
            if (!WolfGrid::isWolfTerritories()) { panel->setStatus("These tools are only available on Wolf Territories Grid.", true); return; }
            panel->mEdit = p;
            panel->mEdit.clampAll();
            panel->markEdited();
            panel->writeControls();
            panel->updatePreview();
            panel->setStatus(panel->getString("str_preset_loaded"), false);
        },
        mPickerHandle);
}
