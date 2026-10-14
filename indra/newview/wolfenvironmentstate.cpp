/**
 * @file wolfenvironmentstate.cpp
 * @brief WolfViewer: the Wolf Environment in force where the agent is. See wolfenvironmentstate.h.
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

#include "wolfenvironmentstate.h"

#include <cmath>
#include <sstream>
#include <boost/json.hpp>

#include "llagent.h"
#include "llenvironment.h"   // [2026-10-14] wolfEnvironmentRegion
#include "llviewercontrol.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "llframetimer.h"
#include "llhttpconstants.h"
#include "llparcel.h"
#include "llsdjson.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h"
#include "wolffloaterenvironment.h"
#include "wolfgrid.h"

namespace
{
    constexpr F64 REFETCH_SECS = 120.0;

    /** A finite number in [lo, hi], or `fallback` (NaN, infinities and non-numbers included).
     *  Source: WolfEnvironment.cs Num. */
    F32 num(const LLSD& v, F32 lo, F32 hi, F32 fallback)
    {
        if (!(v.isReal() || v.isInteger())) return fallback;
        const F64 d = v.asReal();
        if (!std::isfinite(d)) return fallback;
        return (F32)llclamp(d, (F64)lo, (F64)hi);
    }

    /** "#rrggbb" -> 0..1 colour. Source: WolfEnvironment.cs Colour. */
    bool colour(const LLSD& v, LLColor3& out)
    {
        if (!v.isString()) return false;
        const std::string s = v.asString();
        if (s.size() != 7 || s[0] != '#') return false;
        unsigned int rgb = 0;
        std::istringstream in(s.substr(1));
        in >> std::hex >> rgb;
        if (in.fail()) return false;
        out.set(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f);
        return true;
    }

    std::string str(const LLSD& v, const std::string& fallback, std::initializer_list<const char*> allowed)
    {
        if (!v.isString()) return fallback;
        const std::string s = v.asString();
        for (const char* a : allowed) if (s == a) return s;
        return fallback;
    }

    WolfEnvBody body(const LLSD& m, bool moon, bool planet)
    {
        WolfEnvBody b;
        b.mId = m["id"].asString();
        b.mHasColor = colour(m["color"], b.mColor);
        b.mSize = planet ? num(m["size"], 1, 400, 100) / 100.f : num(m["size"], 25, 2000, 100) / 100.f;
        b.mBrightness = num(m["brightness"], 0, 400, 100) / 100.f;
        if (m["texture"].isString()) b.mTexture.set(m["texture"].asString(), false);
        if (planet)
        {
            b.mMode = WolfEnvBody::FIXED;
            b.mAzimuth = num(m["azimuth"], 0, 360, 180);
            b.mElevation = num(m["elevation"], -90, 90, 30);
            b.mRings = m["rings"].asBoolean();
            colour(m["ringColor"], b.mRingColor);
            return b;
        }
        const LLSD& p = m["position"];
        const std::string mode = str(p["mode"], "real", { "real", "fixed", "orbit" });
        b.mMode = mode == "fixed" ? WolfEnvBody::FIXED : mode == "orbit" ? WolfEnvBody::ORBIT : WolfEnvBody::REAL;
        b.mAzimuth = num(p["azimuth"], 0, 360, 0);
        b.mElevation = num(p["elevation"], -90, 90, 45);
        b.mPeriodHours = num(p["periodHours"], 0.5f, 1000, 24);
        b.mTilt = num(p["tilt"], 0, 90, 0);
        b.mPhase = num(p["phase"], 0, 360, 0);
        if (moon && (m["phase"].isReal() || m["phase"].isInteger()))
        {
            b.mHasLit = true;
            b.mLit = num(m["phase"], 0, 100, 50) / 100.f;
        }
        return b;
    }

    void mood(const LLSD& m, LLColor3& tint, F32& strength)
    {
        const std::string t = str(m["tint"], "neutral", { "neutral", "warm", "cool", "custom" });
        // Source: WolfEnvironment.cs ReadMood (warm / cool are our fixed tints)
        if (t == "warm") tint.set(1.0f, 0.82f, 0.62f);
        else if (t == "cool") tint.set(0.66f, 0.82f, 1.0f);
        else if (t == "custom") { if (!colour(m["color"], tint)) tint = LLColor3::white; }
        else tint = LLColor3::white;
        strength = t == "neutral" ? 0.f : num(m["strength"], 0, 100, 0) / 100.f;
    }
}

F32 WolfEnvZone::strengthAt(const LLVector3& p) const
{
    if (mShape == ABOVE)
    {
        // an altitude zone holds from its height up, fading in over the soft edge below it
        if (p.mV[VZ] >= mAltitude) return 1.f;
        if (mEdge <= 0.f) return 0.f;
        return llclamp(1.f - (mAltitude - p.mV[VZ]) / mEdge, 0.f, 1.f);
    }
    F32 outside;
    if (mShape == CIRCLE)
    {
        const F32 dx = p.mV[VX] - mX, dy = p.mV[VY] - mY;
        outside = sqrtf(dx * dx + dy * dy) - mRadius;
    }
    else
    {
        const F32 dx = llmax(llmax(mX1 - p.mV[VX], 0.f), p.mV[VX] - mX2);
        const F32 dy = llmax(llmax(mY1 - p.mV[VY], 0.f), p.mV[VY] - mY2);
        outside = sqrtf(dx * dx + dy * dy);
    }
    if (outside <= 0.f) return 1.f;
    if (mEdge <= 0.f) return 0.f;
    return llclamp(1.f - outside / mEdge, 0.f, 1.f);
}

// static
WolfEnvDoc WolfEnvDoc::fromEntry(const LLSD& entry)
{
    WolfEnvDoc d;
    const LLSD& m = entry["doc"];
    if (!m.isMap()) return d;
    d.mValid = true;
    const std::string mode = str(m["mode"], "off", { "wolf", "on-top", "off" });
    d.mMode = mode == "wolf" ? WOLF : mode == "on-top" ? ON_TOP : OFF;
    d.mHasClouds = m["clouds"].isReal() || m["clouds"].isInteger();
    d.mClouds = num(m["clouds"], 0, 100, 0) / 100.f;
    d.mHaze = num(m["haze"], 0, 100, 0) / 100.f;
    mood(m["mood"], d.mTint, d.mTintStrength);

    const LLSD& s = m["sky"];
    const LLSD& a = s["atmosphere"];
    d.mHasRayleigh = colour(a["rayleigh"], d.mRayleigh);
    d.mHasMie = colour(a["mie"], d.mMie);
    d.mHasOzone = colour(a["ozone"], d.mOzone);
    d.mRayleighDensity = num(a["rayleighDensity"], 0, 400, 100) / 100.f;
    d.mMieDensity = num(a["mieDensity"], 0, 400, 100) / 100.f;
    d.mMieAnisotropy = num(a["mieAnisotropy"], 0, 95, 80) / 100.f;
    d.mOzoneDensity = num(a["ozoneDensity"], 0, 400, 100) / 100.f;
    d.mPlanetKm = num(a["planetRadiusKm"], 1000, 32000, 6360);
    d.mAtmosphereKm = num(a["atmosphereKm"], 10, 500, 60);
    // An absent list is Earth's (one real sun, one real moon); an empty list is none.
    if (s["suns"].isArray()) { for (const LLSD& b : llsd::inArray(s["suns"])) if (d.mSuns.size() < 4 && b.isMap()) d.mSuns.push_back(body(b, false, false)); }
    else d.mSuns.push_back(WolfEnvBody());
    if (s["moons"].isArray()) { for (const LLSD& b : llsd::inArray(s["moons"])) if (d.mMoons.size() < 4 && b.isMap()) d.mMoons.push_back(body(b, true, false)); }
    else d.mMoons.push_back(WolfEnvBody());
    for (size_t i = 1; i < d.mSuns.size(); ++i) if (d.mSuns[i].mMode == WolfEnvBody::REAL) d.mSuns[i].mMode = WolfEnvBody::ORBIT;
    for (size_t i = 1; i < d.mMoons.size(); ++i) if (d.mMoons[i].mMode == WolfEnvBody::REAL) d.mMoons[i].mMode = WolfEnvBody::ORBIT;
    const LLSD& n = s["night"];
    d.mHasStarColor = colour(n["starColor"], d.mStarColor);
    d.mNebula = num(n["nebula"], 0, 100, 0) / 100.f;
    colour(n["nebulaColor"], d.mNebulaColor);
    if (n["planets"].isArray()) for (const LLSD& b : llsd::inArray(n["planets"])) if (d.mPlanets.size() < 4 && b.isMap()) d.mPlanets.push_back(body(b, false, true));
    const LLSD& c = s["clouds"];
    d.mHasCloudColor = colour(c["color"], d.mCloudColor);
    d.mCloudEmission = num(c["emission"], 0, 100, 0) / 100.f;
    d.mCloudStyle = str(c["style"], "natural", { "natural", "wispy", "towering", "bands", "sheet" });
    d.mHasFogColor = colour(s["fog"]["color"], d.mFogColor);
    d.mFogGlow = num(s["fog"]["glow"], 0, 100, 0) / 100.f;

    S32 above = 0;
    for (const LLSD& z : llsd::inArray(m["zones"]))
    {
        if (!z.isMap() || !z["shape"].isMap() || d.mZones.size() >= 16) continue;
        WolfEnvZone zn;
        zn.mId = z["id"].asString();
        zn.mName = z["name"].asString();
        const std::string shape = str(z["shape"]["type"], "", { "circle", "rect", "above" });
        if (shape.empty() || zn.mId.empty()) continue;
        zn.mShape = shape == "rect" ? WolfEnvZone::RECT : shape == "above" ? WolfEnvZone::ABOVE : WolfEnvZone::CIRCLE;
        if (zn.mShape == WolfEnvZone::ABOVE && ++above > 3) continue;
        const LLSD& sh = z["shape"];
        zn.mX = num(sh["x"], 0, 1e6f, 0); zn.mY = num(sh["y"], 0, 1e6f, 0); zn.mRadius = num(sh["radius"], 0.1f, 1e6f, 1);
        zn.mX1 = num(sh["x1"], 0, 1e6f, 0); zn.mY1 = num(sh["y1"], 0, 1e6f, 0); zn.mX2 = num(sh["x2"], 0, 1e6f, 0); zn.mY2 = num(sh["y2"], 0, 1e6f, 0);
        zn.mAltitude = num(sh["altitude"], 100, 4000, 1000);
        zn.mEdge = num(z["edge"], 0, 256, 0);
        const LLSD& set = z["set"];
        if (set.has("clouds")) { zn.mHasClouds = true; zn.mClouds = num(set["clouds"], 0, 100, 0) / 100.f; }
        if (set.has("haze")) { zn.mHasHaze = true; zn.mHaze = num(set["haze"], 0, 100, 0) / 100.f; }
        zn.mHasFogColor = colour(set["fogColor"], zn.mFogColor);
        if (set.has("fogGlow")) { zn.mHasFogGlow = true; zn.mFogGlow = num(set["fogGlow"], 0, 100, 0) / 100.f; }
        zn.mHasLight = colour(set["light"], zn.mLight);
        if (set.has("lightStrength")) { zn.mHasLightStrength = true; zn.mLightStrength = num(set["lightStrength"], 0, 400, 100) / 100.f; }
        if (set["mood"].isMap()) { zn.mHasMood = true; mood(set["mood"], zn.mTint, zn.mTintStrength); }
        d.mZones.push_back(zn);
    }

    // the weather this scope shows (the same observation the region baked; auto_env_lib.php shape)
    const LLSD& o = entry["obs"];
    if (o.isMap() && (o["cloud"].isReal() || o["cloud"].isInteger()))
    {
        d.mHaveWeather = true;
        d.mWeatherCloud = num(o["cloud"], 0, 100, 30) / 100.f;
        d.mWeatherLow = num(o["cloudLow"], 0, 100, 0) / 100.f;
        d.mWeatherMid = num(o["cloudMedium"], 0, 100, 0) / 100.f;
        d.mWeatherHigh = num(o["cloudHigh"], 0, 100, 0) / 100.f;
        d.mWeatherFog = num(o["fog"], 0, 100, 0) / 100.f;
        d.mWindSpeed = num(o["windSpeed"], 0, 60, 3);
        d.mWindFrom = num(o["windFrom"], 0, 360, 240);
        d.mWeatherKind = str(o["kind"], "clear", { "clear", "rain", "snow" });
        d.mWeatherLevel = (S32)num(o["level"], 1, 4, 2);
        // [WolfViewer step 3] the genus mapping's inputs (a synthetic observation has temperature null)
        d.mHaveTemperature = o["temperature"].isReal() || o["temperature"].isInteger();
        d.mTemperature = num(o["temperature"], -60, 60, 15);
        d.mHumidity = num(o["humidity"], 0, 100, 60);
        d.mPrecipitation = num(o["precipitation"], 0, 200, 0);
        d.mThunderChance = num(o["thunderChance"], 0, 100, 0);
        d.mWeatherSymbol = o["symbol"].isString() ? o["symbol"].asString() : std::string();
        d.mHourly.clear();
        const LLSD& hourly = o["hourly"];
        if (hourly.isArray())
        {
            for (LLSD::array_const_iterator it = hourly.beginArray(); it != hourly.endArray() && d.mHourly.size() < 48; ++it)
            {
                const LLSD& row = *it;   // [unix time, cloud, low, medium, high, fog, humidity, wind speed, wind from, symbol]
                if (!row.isArray() || row.size() < 5 || !(row[0].isReal() || row[0].isInteger())) continue;
                WolfEnvDoc::HourlyRow h;
                h.t = row[0].asReal();
                h.low = num(row[2], 0, 100, 0) / 100.f; h.medium = num(row[3], 0, 100, 0) / 100.f; h.high = num(row[4], 0, 100, 0) / 100.f;
                d.mHourly.push_back(h);
            }
        }
    }
    return d;
}

WolfEnvironmentState::WolfEnvironmentState() = default;
WolfEnvironmentState::~WolfEnvironmentState() = default;

const WolfEnvDoc& WolfEnvironmentState::active() const
{
    if (!WolfGrid::isOnWolfTerritories()) return mNone;
    if (mPreview.mValid) return mPreview;
    if (mUseParcel && mParcel.mValid && mParcel.mMode != WolfEnvDoc::OFF) return mParcel;
    if (mRegion.mValid && mRegion.mMode != WolfEnvDoc::OFF) return mRegion;
    // [WOLF ENV 2026-10-12] Paul: the native sky on EVERY Wolf region - a region without a document of its
    // own is drawn from its automatic environment (its place, clock and live weather), in mode wolf. Only
    // with the opt-in WolfEnvironmentNativeSky (off: exactly as before - no document, the EEP)
    static LLCachedControl<bool> native_sky(gSavedSettings, "WolfEnvironmentNativeSky", false);
    if (native_sky && !mRegion.mValid && mAuto.mValid && mAuto.mMode == WolfEnvDoc::WOLF) return mAuto;
    // [WolfViewer round 13] Paul: "Use Wolf Environment on all regions" (WolfEnvironmentAllRegions, default on since w66 - settings.xml; the Wolf
    // Environment floater): a region with no document of its own and no automatic one is drawn natively from its ordinary
    // EEP sky alone - the round-9 mapping reads its cloud_shadow, haze and cloud_scale (wolfenvironmentrender.cpp
    // updateLight), the sun and moon are the EEP's (REAL), no weather (takram's default layers under the EEP's coverage).
    // Wolf Territories only, as the rest of the native sky (tierNow).
    static LLCachedControl<bool> all_regions(gSavedSettings, "WolfEnvironmentAllRegions", false);
    if (native_sky && all_regions)
    {
        if (!mEepOnly.mValid)
        {
            WolfEnvDoc& e = mEepOnly;
            e.mValid = true;
            e.mMode = WolfEnvDoc::WOLF;
            e.mSuns.assign(1, WolfEnvBody());    // REAL: the EEP sky's sun (updateLight getSunDirection)
            e.mMoons.assign(1, WolfEnvBody());   // REAL: the EEP sky's moon
        }
        return mEepOnly;
    }
    return mNone;
}

void WolfEnvironmentState::setPreviewEntry(const LLSD& entry)
{
    mPreview = entry.isMap() ? WolfEnvDoc::fromEntry(entry) : WolfEnvDoc();
}

void WolfEnvironmentState::refresh()
{
    mNextFetch = 0.0;
    mLastFetch = -1.0e9;
    if (mBusy)
    {
        // fetchCoro drops a reply whose serial is not the latest (and then leaves mBusy alone), so the
        // stale read is forgotten here and idle() may start the new one at once
        ++mSerial;
        mBusy = false;
    }
}

void WolfEnvironmentState::idle()
{
    if (!WolfGrid::isOnWolfTerritories())
    {
        if (mRegion.mValid || mParcel.mValid || mPreview.mValid)
        {
            // [WOLF GRID GATE] off Wolf nothing of it stays (a hypergrid jump away)
            mRegion = mParcel = mPreview = mAuto = WolfEnvDoc();
            mFetchedFor.clear();
            mDocsFor.clear();
        }
        return;
    }
    // [WolfViewer 2026-10-14] the region underneath when flying past the agent region's edge (open sea, visiting land), as
    // the EEP sky (LLEnvironment::wolfEnvironmentRegion): Paul, "the environment didnt change as i went over each region"
    LLViewerRegion* region = LLEnvironment::wolfEnvironmentRegion();
    if (!region) return;
    const std::string id = region->getRegionID().asString();
    if (mBusy)
    {
        // [WolfViewer 2026-10-14] audit S2: a read still in flight for the region left behind (up to its 30 s timeout) held the
        // next region's read back; it is forgotten (fetchCoro drops a reply whose serial is old) and the new one starts now
        if (id == mFetchedFor) return;
        ++mSerial;
        mBusy = false;
    }
    LLParcel* parcel = LLViewerParcelMgr::getInstance()->getAgentParcel();
    const S32 local = parcel ? parcel->getLocalID() : -1;
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (id == mFetchedFor && local == mParcelLocalId && now < mNextFetch) return;
    // a parcel change on the same region is read at most every 5 s (an edge walk flickers the id)
    if (id == mFetchedFor && now - mLastFetch < 5.0) return;
    // [WolfViewer 2026-10-14] audit S1: a new region's documents replace the old ones when its reply arrives (applyReply), not
    // before - clearing them here dropped the sky to the EEP-only document and takram's default layers for the length of the
    // read, then snapped to the new document. If that read fails, the old region's are cleared then (fetchCoro).
    mLastFetch = now;
    mFetchedFor = id;
    mParcelLocalId = local;
    mNextFetch = now + REFETCH_SECS;
    const LLVector3 at = region->getPosRegionFromGlobal(gAgent.getPositionGlobal());   // [2026-10-14] in that region (was agent coords)
    // held inside the region: over open sea with no region below, the position is kilometres outside it and the server
    // answered 400 Bad Request every 20 s (Paul's log 10-14 12:45-12:48, y 4735 .. 10791)
    const F32 edge = llmax(region->getWidth() - 0.01f, 0.f);
    const std::string url = llformat("%s?region=%s&x=%.2f&y=%.2f", WolfFloaterEnvironment::API_URL, id.c_str(),
                                     llclamp(at.mV[VX], 0.f, edge), llclamp(at.mV[VY], 0.f, edge));
    mBusy = true;
    const U64 serial = ++mSerial;
    LLCoros::instance().launch("WolfEnvState", [url, id, serial]() { WolfEnvironmentState::fetchCoro(url, id, serial); });
}

// static
void WolfEnvironmentState::fetchCoro(std::string url, std::string region_id, U64 serial)
{
    auto adapter = std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfEnvironmentState", LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
    LLCore::HttpOptions::ptr_t options = WolfGrid::makeVerifiedHttpOptions();
    options->setTimeout(30);
    LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
    headers->append(HTTP_OUT_HEADER_ACCEPT, "application/json");
    LLSD result = adapter->getRawAndSuspend(request, url, options, headers);
    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
    LLSD reply;
    if (result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
    {
        const LLSD::Binary& raw = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
        boost::system::error_code ec;
        boost::json::value v = boost::json::parse(std::string(raw.begin(), raw.end()), ec);
        if (!ec) reply = LlsdFromJson(v);
    }
    if (!WolfEnvironmentState::instanceExists()) return;
    WolfEnvironmentState& self = WolfEnvironmentState::instance();
    if (serial != self.mSerial) return;
    self.mBusy = false;
    if (!status || !reply.isMap() || !reply["success"].asBoolean())
    {
        // The renderer keeps what it had (or today's EEP sky): nothing on screen depends on this
        // read succeeding, so it is logged, and read again sooner.
        LL_WARNS("WolfEnv") << "Wolf Environment not read: "
                            << (!status ? status.toString() : std::string("the reply was not a success")) << LL_ENDL;
        self.mNextFetch = LLFrameTimer::getElapsedSeconds() + 20.0;
        // [WolfViewer 2026-10-14] the documents still in force belong to the region left behind (idle keeps them until this
        // region's reply): they must not stay over a region whose own could not be read
        if (self.mDocsFor != region_id)
        {
            self.mRegion = self.mParcel = self.mAuto = WolfEnvDoc();
            self.mDocsFor = region_id;
        }
        return;
    }
    self.applyReply(reply, region_id);
}

void WolfEnvironmentState::applyReply(const LLSD& reply, const std::string& region_id)
{
    // the agent may have moved on while this was in flight: a reply for another region is dropped
    LLViewerRegion* now = LLEnvironment::wolfEnvironmentRegion();   // [2026-10-14] the region underneath, as idle()
    if (region_id != mFetchedFor || !now || now->getRegionID().asString() != region_id) return;
    mRegion = reply["regionEnv"].isMap() ? WolfEnvDoc::fromEntry(reply["regionEnv"]) : WolfEnvDoc();
    mParcel = reply["parcel"].isMap() && reply["parcel"]["env"].isMap() ? WolfEnvDoc::fromEntry(reply["parcel"]["env"]) : WolfEnvDoc();
    // the automatic environment's document (absent from an older server: no auto doc, as before)
    mAuto = reply["autoEnv"].isMap() ? WolfEnvDoc::fromEntry(reply["autoEnv"]) : WolfEnvDoc();
    // A parcel's sky is only shown when the estate shows parcel environments (EnvironmentModule.cs:487,706).
    mUseParcel = reply["estateAllowsParcel"].asBoolean();
    mDocsFor = region_id;   // [2026-10-14]
}
