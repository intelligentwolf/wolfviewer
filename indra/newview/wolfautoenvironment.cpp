/**
 * @file wolfautoenvironment.cpp
 * @brief WolfViewer: Region / Estate > Jimmy Olsen's Automatic Environment.
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

#include "wolfautoenvironment.h"

#include <cmath>
#include <boost/json.hpp>

#include "llagent.h"
#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "llenvironment.h"
#include "llframetimer.h"
#include "llhttpconstants.h"
#include "lllineeditor.h"
#include "llnotificationsutil.h"
#include "llscrolllistctrl.h"
#include "llsdjson.h"
#include "llsliderctrl.h"
#include "llspinctrl.h"
#include "lldate.h"
#include <ctime>
#include "lltextbox.h"
#include "llviewerregion.h"
#include "wolfgrid.h"
#include "wolfregionweather.h"

// Same host as php/weather.php (WolfRegionWeather::API_URL).
const char* WolfPanelRegionAutoEnvironment::API_URL = "https://wolfstorm.app/php/auto_environment.php";

namespace
{
    constexpr F64 FETCH_SECS = 60.0;   // the place's weather changes on MET's own clock (tens of minutes)

    LLSD json_to_llsd(const LLSD::Binary& bytes)
    {
        std::string text(bytes.begin(), bytes.end());
        boost::system::error_code ec;
        boost::json::value v = boost::json::parse(text, ec);
        if (ec) return LLSD();
        return LlsdFromJson(v);
    }

    /** Degrees from north, clockwise, of a direction in region axes (+x east, +y north). */
    F64 azimuth_of(const LLVector3& d)
    {
        F64 a = std::atan2((F64)d.mV[VX], (F64)d.mV[VY]) * 180.0 / F_PI;
        return a < 0 ? a + 360.0 : a;
    }

    const char* compass(F64 deg)
    {
        static const char* const P[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
        return P[((int)std::floor((deg + 22.5) / 45.0)) & 7];
    }

    /** "partlycloudy_day" -> "partly cloudy". MET symbol codes: github.com/metno/weathericons legend.csv. */
    std::string symbol_words(std::string s)
    {
        for (const char* suffix : { "_day", "_night", "_polartwilight" })
        {
            const std::string sfx(suffix);
            if (s.size() > sfx.size() && s.compare(s.size() - sfx.size(), sfx.size(), sfx) == 0)
                s.erase(s.size() - sfx.size());
        }
        static const char* const WORDS[] = { "clearsky", "clear sky", "partlycloudy", "partly cloudy",
            "showersandthunder", " showers and thunder", "andthunder", " and thunder", "showers", " showers",
            "heavy", "heavy ", "light", "light ", "sleet", "sleet", "snow", "snow", "rain", "rain" };
        for (size_t i = 0; i + 1 < sizeof(WORDS) / sizeof(WORDS[0]); i += 2)
        {
            const std::string from(WORDS[i]);
            size_t at = s.find(from);
            if (at != std::string::npos) s.replace(at, from.size(), WORDS[i + 1]);
        }
        return s;
    }
}

WolfPanelRegionAutoEnvironment::WolfPanelRegionAutoEnvironment() = default;
WolfPanelRegionAutoEnvironment::~WolfPanelRegionAutoEnvironment() = default;

bool WolfPanelRegionAutoEnvironment::postBuild()
{
    mEnabled = getChild<LLCheckBoxCtrl>("ae_enabled");
    mSky     = getChild<LLCheckBoxCtrl>("ae_sky");
    mWeather = getChild<LLCheckBoxCtrl>("ae_weather");
    mSearch  = getChild<LLLineEditor>("ae_search");
    mResults = getChild<LLScrollListCtrl>("ae_results");
    mPlace   = getChild<LLLineEditor>("ae_place");
    mLat     = getChild<LLSpinCtrl>("ae_lat");
    mLon     = getChild<LLSpinCtrl>("ae_lon");
    mNow     = getChild<LLTextBox>("ae_now");
    mStatus  = getChild<LLTextBox>("ae_status");
    mPreviewOn   = getChild<LLCheckBoxCtrl>("ae_preview_on");
    mPreview     = getChild<LLSliderCtrl>("ae_preview");
    mPreviewTime = getChild<LLTextBox>("ae_preview_time");
    mPreviewOn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPreviewChanged(); });
    mPreview->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPreviewChanged(); });

    auto changed = [this](LLUICtrl*, const LLSD&) { onChanged(); };
    mEnabled->setCommitCallback(changed);
    mSky->setCommitCallback(changed);
    mWeather->setCommitCallback(changed);
    mPlace->setCommitCallback(changed);
    mPlace->setKeystrokeCallback([](LLLineEditor*, void* data) {
        static_cast<WolfPanelRegionAutoEnvironment*>(data)->onChanged(); }, this);
    mLat->setCommitCallback(changed);
    mLon->setCommitCallback(changed);
    mSearch->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSearch(); });
    // One click on a result picks it (not only a double click).
    mResults->setCommitOnSelectionChange(true);
    mResults->setCommitCallback([this](LLUICtrl*, const LLSD&) { onResultPicked(); });
    childSetAction("ae_search_btn", [this](LLUICtrl*, const LLSD&) { onSearch(); });
    childSetAction("ae_apply",      [this](LLUICtrl*, const LLSD&) { onApply(); });
    childSetAction("ae_revert",     [this](LLUICtrl*, const LLSD&) { onRevert(); });
    writeControls();
    return true;
}

std::string WolfPanelRegionAutoEnvironment::regionId() const
{
    LLViewerRegion* rgn = gAgent.getRegion();
    return rgn ? rgn->getRegionID().asString() : std::string();
}

bool WolfPanelRegionAutoEnvironment::canEdit() const
{
    // The courtesy copy of auto_environment.php's rule: the region owner, or a god.
    // `regionOwner` from auto_environment.php is the REGION owner (robust.regions.owner_uuid);
    // LLViewerRegion::getOwner() is the handshake's SimOwner, which OpenSim fills with the
    // ESTATE owner (LLClientView.cs:907), so it cannot answer this.
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!WolfGrid::isWolfTerritories() || !rgn || !mHave) return false;
    return mRegionOwner == gAgentID || gAgent.isGodlike();
}

void WolfPanelRegionAutoEnvironment::setStatus(const std::string& msg, bool error)
{
    if (!mStatus) return;
    mStatus->setText(msg);
    // Same colours as the Weather tab (wolfregionweather.cpp setStatus): a failure must be readable.
    mStatus->setColor(error ? LLColor4(0.88f, 0.33f, 0.28f, 1.f) : LLColor4(0.79f, 0.64f, 0.15f, 1.f));
}

void WolfPanelRegionAutoEnvironment::refresh()
{
    mNextFetch = 0.0;
}

void WolfPanelRegionAutoEnvironment::draw()
{
    if (!WolfGrid::isWolfTerritories())
    {
        setStatus("Sorry, this function is only available on Wolf Territories Grid.", true);
        LLPanel::draw();
        return;
    }
    const std::string id = regionId();
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (!id.empty() && (id != mFetchedFor || now >= mNextFetch) && !mBusy)
    {
        if (id != mFetchedFor)
        {
            // A different region: what was shown and any unsaved edit belonged to the old one.
            mHave = false;
            mDirty = false;
            mConfig = LLSD();
            mObs = LLSD();
            writeControls();
        }
        fetch();
    }
    if (now >= mNextNow)
    {
        mNextNow = now + 2.0;
        showNow();
        showPreviewTime();
        // The preview ends when something else ends it (a region change, a personal sky).
        if (mPreviewOn->getValue().asBoolean() && !WolfRegionWeather::instance().autoEnvPreviewOn())
        {
            mPreviewOn->setValue(false);
            mPreview->setEnabled(false);
        }
    }
    LLPanel::draw();
}

void WolfPanelRegionAutoEnvironment::writeControls()
{
    mWriting = true;
    const bool have = mHave && mConfig.isMap();
    mEnabled->setValue(have && mConfig["enabled"].asBoolean());
    mSky->setValue(!have || mConfig["skyOn"].asBoolean());
    mWeather->setValue(!have || mConfig["weatherOn"].asBoolean());
    mPlace->setText(have ? mConfig["placeName"].asString() : std::string());
    mLat->setValue(have ? mConfig["lat"].asReal() : 0.0);
    mLon->setValue(have ? mConfig["lon"].asReal() : 0.0);
    mWriting = false;

    // Preview time is this viewer's own view, so anyone may use it while the region follows a place.
    const bool can_preview = mHave && mConfig["enabled"].asBoolean() && mConfig["skyOn"].asBoolean();
    mPreviewOn->setEnabled(can_preview);
    mPreview->setEnabled(can_preview && mPreviewOn->getValue().asBoolean());
    if (!can_preview && mPreviewOn->getValue().asBoolean())
    {
        mPreviewOn->setValue(false);
        WolfRegionWeather::instance().setAutoEnvPreview(false, 0.0);
    }

    const bool editable = canEdit() && !mBusy;
    for (const char* name : { "ae_enabled", "ae_sky", "ae_weather", "ae_search", "ae_search_btn",
                              "ae_results", "ae_place", "ae_lat", "ae_lon", "ae_apply" })
    {
        if (LLView* v = findChild<LLView>(name)) v->setEnabled(editable);
    }
    if (mHave && !canEdit() && mStatus && mStatus->getText().empty())
    {
        setStatus(getString("str_read_only"), false);
    }
}

void WolfPanelRegionAutoEnvironment::showNow()
{
    if (!mNow) return;
    std::string text;
    if (!mHave)
    {
        text = getString("str_loading");
    }
    else
    {
        const bool on = mConfig["enabled"].asBoolean();
        const std::string place = mConfig["placeName"].asString();
        text = on ? ("Following " + place + ".") : ("Off. " + place + " is shown as a suggestion.");
        if (mObs.isMap())
        {
            const LLSD& o = mObs;
            text += llformat("\nThere now: %s, %.0f°C, cloud %.0f%% (low %.0f / mid %.0f / high %.0f), "
                             "wind %.1f m/s from the %s, humidity %.0f%%.",
                             symbol_words(o["symbol"].asString()).c_str(), o["temperature"].asReal(),
                             o["cloud"].asReal(), o["cloudLow"].asReal(), o["cloudMedium"].asReal(),
                             o["cloudHigh"].asReal(), o["windSpeed"].asReal(),
                             compass(o["windFrom"].asReal()), o["humidity"].asReal());
            text += "\nUpdated " + o["fetchedAt"].asString() + (o["stale"].asBoolean() ? " (stale: the weather service has not answered for hours)" : "");
        }
        else if (on)
        {
            text += "\nWaiting for the first weather report for this place.";
        }
    }
    // The sky this viewer is drawing right now — what the region's environment says.
    LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
    if (sky)
    {
        const LLVector3 sun = sky->getSunDirection();
        const LLVector3 moon = sky->getMoonDirection();
        const F64 sun_alt = std::asin(llclamp((F64)sun.mV[VZ], -1.0, 1.0)) * 180.0 / F_PI;
        const F64 moon_alt = std::asin(llclamp((F64)moon.mV[VZ], -1.0, 1.0)) * 180.0 / F_PI;
        // Lit fraction from the sun-moon angle: (1 - cos elongation) / 2 (Meeus ch. 48).
        const F64 lit = (1.0 - (F64)(sun * moon)) * 50.0;
        text += llformat("\nSky here: sun %.0f° up at %.0f° (%s), moon %.0f° up at %.0f° (%s), %.0f%% lit.",
                         sun_alt, azimuth_of(sun), compass(azimuth_of(sun)),
                         moon_alt, azimuth_of(moon), compass(azimuth_of(moon)), lit);
    }
    mNow->setText(text);
}

void WolfPanelRegionAutoEnvironment::onPreviewChanged()
{
    const bool on = mPreviewOn->getValue().asBoolean();
    mPreview->setEnabled(on);
    WolfRegionWeather::instance().setAutoEnvPreview(on, on ? (F64)mPreview->getValueF32() * 3600.0 : 0.0);
    showPreviewTime();
}

/** "Showing Sat 23:00 grid time (13 h 30 min ahead)". [GRID TIME 2026-10-03] Paul: "the weather
 *  should be at grid time not local time" — the sky shows the place at the GRID's (Pacific)
 *  wall-clock time, so that is the clock this reads in. auto_environment.php gives the grid
 *  zone's UTC offset now and 12 h from now. */
void WolfPanelRegionAutoEnvironment::showPreviewTime()
{
    if (!mPreviewTime) return;
    if (!mHave || !mConfig.isMap()) { mPreviewTime->setText(LLStringUtil::null); return; }
    const F64 ahead = WolfRegionWeather::instance().autoEnvTimeOffset();
    const S32 offset = (ahead < 43200.0 ? mConfig["gridUtcOffset"] : mConfig["gridUtcOffset12h"]).asInteger();
    const time_t local = (time_t)(LLDate::now().secondsSinceEpoch() + ahead + offset);
    struct tm parts;
#if LL_WINDOWS
    gmtime_s(&parts, &local);
#else
    gmtime_r(&local, &parts);
#endif
    char clock[32];
    strftime(clock, sizeof(clock), "%a %H:%M", &parts);
    const std::string place = mConfig["placeName"].asString();
    if (WolfRegionWeather::instance().autoEnvPreviewOn())
    {
        const S32 mins = (S32)(ahead / 60.0 + 0.5);
        mPreviewTime->setText(llformat("Showing %s grid time (%d h %02d min ahead) - only on your screen.",
                                       clock, mins / 60, mins % 60));
    }
    else
    {
        mPreviewTime->setText(llformat("Grid time %s: the sky shows %s as it looks at %s there.",
                                       clock, place.c_str(), clock));
    }
}

void WolfPanelRegionAutoEnvironment::onVisibilityChange(bool visible)
{
    // A preview never outlives the tab, as the Weather tab's own preview does not
    // (wolfregionweather.cpp WolfPanelWeather::onVisibilityChange): a sky that stayed hours
    // ahead after closing the window would be indistinguishable from the real one.
    if (!visible && mPreviewOn && mPreviewOn->getValue().asBoolean())
    {
        mPreviewOn->setValue(false);
        mPreview->setEnabled(false);
        WolfRegionWeather::instance().setAutoEnvPreview(false, 0.0);
    }
    LLPanel::onVisibilityChange(visible);
}

void WolfPanelRegionAutoEnvironment::onChanged()
{
    if (mWriting) return;
    mDirty = true;
    setStatus(getString("str_unsaved"), false);
}

void WolfPanelRegionAutoEnvironment::onRevert()
{
    mDirty = false;
    mResults->deleteAllItems();
    setStatus(LLStringUtil::null, false);
    writeControls();
}

void WolfPanelRegionAutoEnvironment::onResultPicked()
{
    LLScrollListItem* item = mResults->getFirstSelected();
    if (!item) return;
    const S32 i = item->getValue().asInteger();
    if (!mResultData.isArray() || i < 0 || i >= (S32)mResultData.size()) return;
    const LLSD& r = mResultData[i];
    mWriting = true;
    mPlace->setText(r["name"].asString());
    mLat->setValue(r["lat"].asReal());
    mLon->setValue(r["lon"].asReal());
    mWriting = false;
    onChanged();
}

void WolfPanelRegionAutoEnvironment::onSearch()
{
    if (!canEdit()) { setStatus(getString("str_read_only"), true); return; }
    const std::string q = mSearch->getText();
    if (q.size() < 2) { setStatus(getString("str_search_short"), true); return; }
    if (mBusy) return;
    LLSD body;
    body["action"] = "search";
    body["q"] = q;
    mBusy = true;
    setStatus(getString("str_searching"), false);
    const std::string text = boost::json::serialize(LlsdToJson(body));
    const LLHandle<LLPanel> handle = getHandle();
    const std::string id = regionId();
    const U64 serial = ++mSerial;
    LLCoros::instance().launch("WolfAutoEnvSearch", [handle, text, id, serial]() {
        WolfPanelRegionAutoEnvironment::postCoro(handle, text, id, true, serial);
    });
}

void WolfPanelRegionAutoEnvironment::onApply()
{
    if (!canEdit()) { setStatus(getString("str_read_only"), true); return; }
    if (mBusy) { setStatus(getString("str_saving"), false); return; }
    LLSD body;
    body["region"]    = regionId();
    body["version"]   = mConfig["isDefault"].asBoolean() ? 0 : mConfig["version"].asInteger();
    body["enabled"]   = mEnabled->getValue().asBoolean();
    body["skyOn"]     = mSky->getValue().asBoolean();
    body["weatherOn"] = mWeather->getValue().asBoolean();
    body["placeName"] = mPlace->getText();
    body["lat"]       = mLat->getValue().asReal();
    body["lon"]       = mLon->getValue().asReal();
    mBusy = true;
    setStatus(getString("str_saving"), false);
    const std::string text = boost::json::serialize(LlsdToJson(body));
    const LLHandle<LLPanel> handle = getHandle();
    const std::string id = regionId();
    const U64 serial = ++mSerial;
    LLCoros::instance().launch("WolfAutoEnvSave", [handle, text, id, serial]() {
        WolfPanelRegionAutoEnvironment::postCoro(handle, text, id, false, serial);
    });
}

void WolfPanelRegionAutoEnvironment::fetch()
{
    const std::string id = regionId();
    if (id.empty()) return;
    mFetchedFor = id;
    mNextFetch = LLFrameTimer::getElapsedSeconds() + FETCH_SECS;
    const LLHandle<LLPanel> handle = getHandle();
    const U64 serial = ++mSerial;
    LLCoros::instance().launch("WolfAutoEnvFetch", [handle, id, serial]() {
        WolfPanelRegionAutoEnvironment::fetchCoro(handle, id, serial);
    });
}

void WolfPanelRegionAutoEnvironment::applyReply(const LLSD& reply)
{
    mConfig = reply["config"];
    mObs = reply["obs"];
    mRegionOwner = reply.has("regionOwner") ? reply["regionOwner"].asUUID() : LLUUID::null;
    mHave = mConfig.isMap();
    showNow();
}

// static
void WolfPanelRegionAutoEnvironment::fetchCoro(LLHandle<LLPanel> handle, std::string region_id, U64 serial)
{
    LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
        std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfAutoEnvironment", LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
    // Same verified options and identity headers as the weather read (wolfregionweather.cpp fetchCoro).
    LLCore::HttpOptions::ptr_t options = WolfGrid::makeVerifiedHttpOptions();
    options->setTimeout(20);
    LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
    headers->append(HTTP_OUT_HEADER_ACCEPT, "application/json");
    headers->append("X-Wolf-Agent", gAgentID.asString());
    headers->append("X-Wolf-Session", gAgentSessionID.asString());
    LLSD result = adapter->getRawAndSuspend(request, std::string(API_URL) + "?region=" + region_id, options, headers);
    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(
        result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
    LLSD reply;
    if (result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
        reply = json_to_llsd(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary());

    WolfPanelRegionAutoEnvironment* self = static_cast<WolfPanelRegionAutoEnvironment*>(handle.get());
    if (!self || self->regionId() != region_id) return;          // closed, or we moved on
    if (!status || !reply.isMap() || !reply["success"].asBoolean())
    {
        const std::string why = reply.isMap() && reply.has("error") ? reply["error"].asString() : status.toString();
        self->setStatus(self->getString("str_read_failed") + " " + why, true);
        return;
    }
    // Never let an older read replace newer settings (a save's own reply, or a later read):
    // a stale version would make the next Apply fail with a conflict.
    if (self->mBusy) return;
    if (self->mHave && reply["config"]["version"].asInteger() < self->mConfig["version"].asInteger()) return;
    self->applyReply(reply);
    if (!self->mDirty) self->writeControls();
}

// static
void WolfPanelRegionAutoEnvironment::postCoro(LLHandle<LLPanel> handle, std::string body, std::string region_id,
                                              bool search, U64 serial)
{
    LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
        std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfAutoEnvironment", LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
    LLCore::HttpOptions::ptr_t options = WolfGrid::makeVerifiedHttpOptions();
    options->setTimeout(30);
    // A save is never retried by the HTTP layer: a slow first answer made it send the same save
    // twice, and the second copy failed the version check ("Conflict", Paul 10-03).
    options->setRetries(0);
    LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
    headers->append(HTTP_OUT_HEADER_CONTENT_TYPE, "application/json");
    headers->append("X-Wolf-Agent", gAgentID.asString());
    headers->append("X-Wolf-Session", gAgentSessionID.asString());
    LLCore::BufferArray::ptr_t raw(new LLCore::BufferArray());
    raw->append(body.data(), body.size());
    LLSD result = adapter->postRawAndSuspend(request, API_URL, raw, options, headers);
    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(
        result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
    LLSD reply;
    if (result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
        reply = json_to_llsd(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary());

    const bool ok = status && reply.isMap() && reply["success"].asBoolean();
    const std::string why = reply.isMap() && reply.has("error") ? reply["error"].asString()
                          : !status ? status.toString() : std::string("the service returned an unreadable reply");
    WolfPanelRegionAutoEnvironment* self = static_cast<WolfPanelRegionAutoEnvironment*>(handle.get());
    // 409: the settings were saved from somewhere else since this tab read them
    // (auto_environment.php version check). Reload them, keep this edit, and say so.
    const bool conflict = !ok && !search && status == LLCore::HttpStatus(HTTP_CONFLICT);
    if (!ok && !search && !conflict)
    {
        // A save the owner asked for must not fail silently, even if the floater was closed.
        LLNotificationsUtil::add("GenericAlertOK", LLSD().with("MESSAGE",
            "Could not save Jimmy Olsen's Automatic Environment: " + why));
    }
    if (!self) return;
    self->mBusy = false;
    if (self->regionId() != region_id) return;
    if (conflict)
    {
        self->mHave = false;                     // take whatever version the grid has now
        self->mNextFetch = 0.0;
        self->setStatus(self->getString("str_conflict"), true);
        return;
    }
    if (!ok)
    {
        self->setStatus(self->getString(search ? "str_search_failed" : "str_save_failed") + " " + why, true);
        if (!search) self->mNextFetch = 0.0;     // re-read: a 409 means someone else saved
        self->writeControls();
        return;
    }
    if (search)
    {
        self->mResultData = reply["results"];
        self->mResults->deleteAllItems();
        for (S32 i = 0; i < (S32)self->mResultData.size(); ++i)
        {
            const LLSD& r = self->mResultData[i];
            self->mResults->addSimpleElement(llformat("%s  (%.4f, %.4f)", r["name"].asString().c_str(),
                                                      r["lat"].asReal(), r["lon"].asReal()),
                                             ADD_BOTTOM, LLSD(i));
        }
        self->setStatus(self->mResultData.size() ? self->getString("str_pick") : self->getString("str_none_found"),
                        self->mResultData.size() == 0);
        self->writeControls();
        return;
    }
    self->applyReply(reply);
    self->mDirty = false;
    self->writeControls();
    self->setStatus(self->getString("str_saved"), false);
    // The weather tab and the moon read `autoEnv` from weather.php; ask again now.
    WolfRegionWeather::instance().refresh(true);
}
