/**
 * @file wolffloaterweatherreport.cpp
 * @brief WolfViewer: Jimmy's Weather Report. See wolffloaterweatherreport.h.
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

#include "wolffloaterweatherreport.h"

#include <cmath>
#include <ctime>

#include <boost/json.hpp>

#include "llagent.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "llenvironment.h"
#include "llframetimer.h"
#include "llsdjson.h"
#include "llsettingsdaycycle.h"
#include "lltextbox.h"
#include "llviewerregion.h"
#include "wolfgrid.h"
#include "wolfweather.h"

namespace
{
    // The same public read the Region / Estate tab makes (wolfautoenvironment.cpp API_URL).
    const char* API_URL = "https://wolfstorm.app/php/auto_environment.php";
    const F64 REFRESH_SECONDS = 30.0;

    std::string hhmm(time_t t)
    {
        struct tm g;
#if LL_WINDOWS
        gmtime_s(&g, &t);
#else
        gmtime_r(&t, &g);
#endif
        return llformat("%02d:%02d", g.tm_hour, g.tm_min);
    }

    std::string utc_text(S32 off)
    {
        const S32 a = off < 0 ? -off : off;
        std::string s = llformat("UTC%s%d", off < 0 ? "-" : "+", a / 3600);
        if ((a % 3600) / 60) s += llformat(":%02d", (a % 3600) / 60);
        return s;
    }

    std::string compass(F64 deg)
    {
        static const char* n[16] = { "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE", "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW" };
        return n[(S32)floor(fmod(deg + 11.25 + 360.0, 360.0) / 22.5) % 16];
    }
}

WolfFloaterWeatherReport::WolfFloaterWeatherReport(const LLSD& key) : LLFloater(key)
{
}

bool WolfFloaterWeatherReport::postBuild()
{
    mReport = getChild<LLTextBox>("report");
    mSky = getChild<LLTextBox>("sky");
    mStatus = getChild<LLTextBox>("status");
    return true;
}

void WolfFloaterWeatherReport::onOpen(const LLSD& key)
{
    LLFloater::onOpen(key);
    mNextFetch = 0.0;
    mFetchedFor.clear();
}

void WolfFloaterWeatherReport::draw()
{
    LLViewerRegion* region = gAgent.getRegion();
    const std::string id = region ? region->getRegionID().asString() : std::string();
    const F64 now = LLFrameTimer::getTotalSeconds();
    if (!id.empty() && !mBusy && (id != mFetchedFor || now >= mNextFetch))
    {
        mNextFetch = now + REFRESH_SECONDS;
        fetch();
    }
    showSky();
    LLFloater::draw();
}

void WolfFloaterWeatherReport::fetch()
{
    LLViewerRegion* region = gAgent.getRegion();
    if (!region) return;
    if (!WolfGrid::isOnWolfTerritories())
    {
        // The request carries this login's agent and session: only ever to Wolf's own site.
        mStatus->setText(std::string("The weather report is for Wolf Territories regions."));
        return;
    }
    mBusy = true;
    const std::string id = region->getRegionID().asString();
    mFetchedFor = id;
    const LLHandle<LLFloater> h = getHandle();
    LLCoros::instance().launch("WolfWeatherReport", [h, id]() { fetchCoro(h, id); });
}

// static
void WolfFloaterWeatherReport::fetchCoro(LLHandle<LLFloater> handle, std::string region_id)
{
    // Source: wolfautoenvironment.cpp fetchCoro - the same verified options and identity headers.
    LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
        std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfWeatherReport", LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
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
    {
        const LLSD::Binary& raw = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
        boost::system::error_code ec;
        boost::json::value v = boost::json::parse(std::string(raw.begin(), raw.end()), ec);
        if (!ec) reply = LlsdFromJson(v);
    }
    WolfFloaterWeatherReport* self = static_cast<WolfFloaterWeatherReport*>(handle.get());
    if (!self) return;
    self->mBusy = false;
    if (!status || !reply.isMap() || !reply["success"].asBoolean())
    {
        self->mStatus->setText("Could not read the weather: " + (reply.isMap() && reply.has("error") ? reply["error"].asString() : status.toString()));
        return;
    }
    self->show(reply);
}

void WolfFloaterWeatherReport::show(const LLSD& reply)
{
    mReply = reply;
    const LLSD& cfg = reply["config"];
    const LLSD& obs = reply["obs"];
    if (!cfg["enabled"].asBoolean())
    {
        mReport->setText(std::string("This region does not follow a real place's weather.\n\n"
                                     "The region owner can turn it on in World > Region Details > Jimmy's EEP & Weather."));
        mStatus->setText(std::string());
        return;
    }
    std::string t;
    t += "Weather for " + cfg["placeName"].asString() + ":\n";
    if (!obs.isMap())
    {
        t += "(no reading yet - it is fetched while someone is on the region)\n";
        mReport->setText(t);
        return;
    }
    t += (obs["condition"].asString().empty() ? std::string("Unknown") : obs["condition"].asString()) + "\n";
    if (obs.has("temperature") && obs["temperature"].isReal())
    {
        const F64 c = obs["temperature"].asReal();
        t += llformat("Temperature: %.1f °C (%.1f °F)\n", c, c * 9.0 / 5.0 + 32.0);
    }
    if (obs.has("humidity") && obs["humidity"].isReal()) t += llformat("Humidity: %d%%\n", (S32)llround(obs["humidity"].asReal()));
    if (obs.has("pressure") && obs["pressure"].isReal()) t += llformat("Pressure: %d hPa\n", (S32)llround(obs["pressure"].asReal()));
    if (obs.has("windSpeed") && obs["windSpeed"].isReal())
    {
        t += llformat("Wind: %.1f m/s", obs["windSpeed"].asReal());
        if (obs.has("windFrom") && obs["windFrom"].isReal())
            t += llformat(" from %d° (%s)", (S32)llround(obs["windFrom"].asReal()), compass(obs["windFrom"].asReal()).c_str());
        t += "\n";
    }
    const S32 off = cfg["utcOffset"].asInteger();
    t += "Local time there: " + hhmm(time(nullptr) + off) + " [" + utc_text(off) + "]\n";
    if (cfg.has("clockLabel") && cfg["clockLabel"].asString() != "Time there")
    {
        const S32 coff = cfg["clockUtcOffset"].asInteger();
        t += "The sky follows: " + cfg["clockLabel"].asString() + " (" + hhmm(time(nullptr) + coff) + ")\n";
    }
    t += "Source: MET Norway (yr.no), CC BY 4.0";
    if (obs["stale"].asBoolean()) t += " - the last reading is old";
    mReport->setText(t);
    mStatus->setText(obs["fetchedAt"].asString().empty() ? std::string() : "Read " + obs["fetchedAt"].asString());
}

// The sky the region shows now: the region's day-cycle frame for this moment. WolfAutoEnvironment
// names each frame "AutoSkyNN <Jimmy's sky>" (DayLength 86400, DayOffset 0, so the cycle position
// is the UTC time of day). Sun height from the sky the viewer is drawing.
void WolfFloaterWeatherReport::showSky()
{
    std::string t;
    LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
    if (sky)
    {
        const LLVector3 sun = sky->getSunDirection();
        const F32 elev = asinf(llclamp(sun.mV[VZ], -1.f, 1.f)) * RAD_TO_DEG;
        t += llformat("Sun height: %d°", (S32)llround(elev));
    }
    const LLSD& cfg = mReply["config"];
    if (cfg.isMap() && cfg.has("lon"))
    {
        // Source: Jimmy's slave sunElevationDeg - solar time = UTC + longitude / 15.
        const time_t now = time(nullptr);
        F64 solar = fmod((F64)(now % 86400) / 3600.0 + cfg["lon"].asReal() / 15.0 + 48.0, 24.0);
        t += llformat("  |  solar time %.2fh", solar);
        t += cfg["lat"].asReal() < 0.0 ? "  |  southern hemisphere" : "  |  northern hemisphere";
    }
    std::string using_sky;
    LLSettingsDay::ptr_t day = LLEnvironment::instance().getCurrentDay();
    if (day)
    {
        const LLSettingsDay::CycleTrack_t& track = day->getCycleTrackConst(1);
        const F32 frac = (F32)((F64)(time(nullptr) % 86400) / 86400.0);
        LLSettingsBase::ptr_t best;
        for (const auto& kv : track)
        {
            if ((F32)kv.first <= frac) best = kv.second;
        }
        if (!best && !track.empty()) best = track.rbegin()->second;
        if (best)
        {
            const std::string name = best->getName();
            const size_t sp = name.find(' ');
            if (name.rfind("AutoSky", 0) == 0) using_sky = sp != std::string::npos ? name.substr(sp + 1) : std::string("the earlier look (no weather code yet)");
        }
    }
    t += "\nSky in use: " + (using_sky.empty() ? std::string("not the automatic sky (the region or parcel has its own)") : using_sky);
    if (WolfWeather::instance().activeProfile().mAurora > 0)
        t += llformat("\nAurora level: %d%%", WolfWeather::instance().activeProfile().mAurora);
    else
        t += "\nAurora: none tonight";
    mSky->setText(t);
}
