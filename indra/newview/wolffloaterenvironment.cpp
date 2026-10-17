/**
 * @file wolffloaterenvironment.cpp
 * @brief WolfViewer: World > Wolf World > Wolf Environment. See wolffloaterenvironment.h.
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

#include "wolffloaterenvironment.h"

#include <cmath>
#include <sstream>
#include <boost/json.hpp>

#include "llagent.h"
#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcolorswatch.h"
#include "llcombobox.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "llenvironment.h"
#include "llframetimer.h"
#include "llhttpconstants.h"
#include "lllineeditor.h"
#include "llnotificationsutil.h"
#include "llparcel.h"
#include "llradiogroup.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "llsdjson.h"
#include "llsdserialize.h"
#include "llsettingsvo.h"
#include "llsliderctrl.h"
#include "llspinctrl.h"
#include "lltabcontainer.h"
#include "lltextbox.h"
#include "lltexturectrl.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h"
#include "wolfenvironmentstate.h"
#include "wolfgrid.h"
#include "wolfregionweather.h"

// Same host as php/auto_environment.php (WolfPanelRegionAutoEnvironment::API_URL).
const char* WolfFloaterEnvironment::API_URL = "https://wolfstorm.app/php/wolf_environment.php";
const char* WolfFloaterEnvironment::AUTO_ENV_URL = "https://wolfstorm.app/php/auto_environment.php";

namespace
{
    constexpr F64 PREVIEW_DEBOUNCE = 1.6;   // the region takes one preview per agent per 1.5 s (WolfEnvironment.cs PREVIEW_MIN_MS)
    // The capability (WolfEnvironment.cs PREVIEW_CAP), asked for in llviewerregion.cpp.
    const char* PREVIEW_CAP = "WolfEnvironmentPreview";
    LLSD sZones;
    bool sZonesAsked = false;

    LLSD json_to_llsd(const std::string& text)
    {
        boost::system::error_code ec;
        boost::json::value v = boost::json::parse(text, ec);
        return ec ? LLSD() : LlsdFromJson(v);
    }

    std::string llsd_to_json(const LLSD& v)
    {
        return boost::json::serialize(LlsdToJson(v));
    }

    LLSD raw_reply(const LLSD& result)
    {
        if (!result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW)) return LLSD();
        const LLSD::Binary& raw = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
        return json_to_llsd(std::string(raw.begin(), raw.end()));
    }

    /** "#rrggbb" <-> colour. A bad or missing string reads as white. */
    LLColor4 hex_to_color(const LLSD& v)
    {
        const std::string s = v.asString();
        if (s.size() != 7 || s[0] != '#') return LLColor4::white;
        unsigned int rgb = 0;
        std::istringstream in(s.substr(1));
        in >> std::hex >> rgb;
        if (in.fail()) return LLColor4::white;
        return LLColor4(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, 1.f);
    }

    std::string color_to_hex(const LLColor4& c)
    {
        auto b = [](F32 x) { return (int)llclamp(ll_round(x * 255.f), 0, 255); };
        return llformat("#%02x%02x%02x", b(c.mV[VRED]), b(c.mV[VGREEN]), b(c.mV[VBLUE]));
    }

    /** An integer field exactly as the web's validator wants it (is_int: a JSON integer, never 50.0). */
    LLSD as_int(F32 v) { return LLSD::Integer(ll_round(v)); }

    template<class T> T* child(LLFloater* f, const char* name) { return f->getChild<T>(name); }

    // The choices, as the format's values (php/wolf_env_lib.php WE_*) with plain labels.
    struct Choice { const char* value; const char* label; };
    const Choice PLACE_KINDS[] = { {"real", "A real place on Earth"}, {"fictional", "A fictional world"}, {"none", "No place"} };
    const Choice TIME_MODES[] = { {"real", "The place's own time"}, {"grid", "Grid time (Pacific)"}, {"zone", "A time zone"},
                                  {"fixed", "Always the same time"}, {"cycle", "A faster day"} };
    const Choice SOURCES[] = { {"live", "Real weather there"}, {"pattern", "A weather pattern"}, {"fixed", "Always the same"} };
    const Choice PATTERNS[] = { {"settled", "Settled"}, {"changeable", "Changeable"}, {"unsettled", "Unsettled"},
                                {"stormy", "Stormy"}, {"wintry", "Wintry"} };
    const Choice TYPES[] = { {"clear", "Clear"}, {"fair", "Fair"}, {"cloudy", "Cloudy"}, {"overcast", "Overcast"},
                             {"rain", "Rain"}, {"storm", "Storm"}, {"snow", "Snow"}, {"fog", "Fog"} };
    const Choice TINTS[] = { {"neutral", "Neutral"}, {"warm", "Warm"}, {"cool", "Cool"}, {"custom", "Custom"} };
    const Choice WATERS[] = { {"natural", "Follows the wind"}, {"calm", "Calm"}, {"choppy", "Choppy"}, {"stormy", "Stormy"} };
    const Choice STYLES[] = { {"natural", "As the weather makes them"}, {"wispy", "Wispy"}, {"towering", "Towering"},
                              {"bands", "Bands"}, {"sheet", "One sheet"} };
    const Choice POSITIONS[] = { {"real", "The real sky for the place"}, {"fixed", "Fixed in the sky"}, {"orbit", "Its own path"} };
    const Choice SHAPES[] = { {"circle", "A circle"}, {"rect", "A rectangle"}, {"above", "Everything above a height"} };
    const Choice PRESETS[] = { {"earth", "Earth"}, {"alien", "Alien world"}, {"sunset-world", "Sunset world"},
                               {"twin-suns", "Twin suns"}, {"toxic", "Toxic"}, {"underwater", "Underwater"},
                               {"void", "The void"}, {"custom", "Custom"} };

    template<size_t N> void fill(LLComboBox* c, const Choice (&list)[N])
    {
        c->removeall();
        for (const Choice& ch : list) c->add(ch.label, LLSD(ch.value));
    }

    /** The control a validator field path belongs to (wolf_env_lib.php field names), and its tab. */
    struct FieldCtrl { const char* prefix; const char* ctrl; const char* tab; };
    const FieldCtrl FIELD_CTRLS[] = {
        {"mode", "mode", "tab_basics"}, {"place.kind", "place_kind", "tab_basics"}, {"place.name", "place_name", "tab_basics"},
        {"place.lat", "place_lat", "tab_basics"}, {"place.lon", "place_lon", "tab_basics"}, {"time.zone", "time_zone", "tab_basics"},
        {"time.hour", "time_hour", "tab_basics"}, {"time.dayHours", "time_day_hours", "tab_basics"}, {"time", "time_mode", "tab_basics"},
        {"weather", "weather_source", "tab_basics"}, {"clouds", "clouds", "tab_basics"}, {"haze", "haze", "tab_basics"},
        {"mood", "mood_tint", "tab_basics"}, {"water", "water", "tab_basics"}, {"preset", "preset", "tab_basics"},
        {"sky.suns", "bodies", "tab_bodies"}, {"sky.moons", "bodies", "tab_bodies"}, {"sky.night.planets", "bodies", "tab_bodies"},
        {"sky", "ray_density", "tab_sky"}, {"zones", "zones", "tab_zones"},
    };
}

WolfFloaterEnvironment::WolfFloaterEnvironment(const LLSD& key) : LLFloater(key) {}

WolfFloaterEnvironment::~WolfFloaterEnvironment()
{
    // At quit the environment singleton may already be gone (onClose skips this when quitting).
    if (mPreviewOn && LLEnvironment::instanceExists()) stopPreview();
}

bool WolfFloaterEnvironment::postBuild()
{
    mScopeCombo = getChild<LLComboBox>("scope");
    mWho = getChild<LLTextBox>("who");
    mStatus = getChild<LLTextBox>("status");
    mTabs = getChild<LLTabContainer>("tabs");
    mMode = getChild<LLRadioGroup>("mode");
    mPreviewBtn = getChild<LLButton>("preview");
    mTimeOn = getChild<LLCheckBoxCtrl>("time_preview_on");
    mTime = getChild<LLSliderCtrl>("time_preview");
    mTimeText = getChild<LLTextBox>("time_preview_text");
    mTime->setEnabled(false);
    mTimeOn->setCommitCallback([this](LLUICtrl*, const LLSD&) { applyTime(); });
    mTime->setCommitCallback([this](LLUICtrl*, const LLSD&) { applyTime(); });

    fill(child<LLComboBox>(this, "preset"), PRESETS);
    fill(child<LLComboBox>(this, "place_kind"), PLACE_KINDS);
    fill(child<LLComboBox>(this, "time_mode"), TIME_MODES);
    fill(child<LLComboBox>(this, "weather_source"), SOURCES);
    fill(child<LLComboBox>(this, "weather_pattern"), PATTERNS);
    fill(child<LLComboBox>(this, "weather_type"), TYPES);
    fill(child<LLComboBox>(this, "mood_tint"), TINTS);
    fill(child<LLComboBox>(this, "water"), WATERS);
    fill(child<LLComboBox>(this, "cloud_style"), STYLES);
    fill(child<LLComboBox>(this, "body_position"), POSITIONS);
    fill(child<LLComboBox>(this, "zone_shape"), SHAPES);
    fill(child<LLComboBox>(this, "zone_weather"), TYPES);
    fill(child<LLComboBox>(this, "zone_mood_tint"), TINTS);
    fillTimeZones();

    // Every editable control commits into the draft.
    static const char* const EDITS[] = {
        "mode", "place_kind", "place_name", "place_lat", "place_lon", "time_mode", "time_zone", "time_hour", "time_day_hours",
        "weather_source", "weather_pattern", "weather_type", "clouds_auto", "clouds", "haze", "water", "mood_tint", "mood_color",
        "mood_strength", "ray_nat", "ray_color", "ray_density", "mie_nat", "mie_color", "mie_density", "ozone_nat", "ozone_color",
        "ozone_density", "mie_glow", "planet_km", "atmos_km", "stars", "star_nat", "star_color", "nebula", "nebula_color",
        "cloud_nat", "cloud_color", "cloud_glow", "cloud_style", "cloud_texture", "fog_nat", "fog_color", "fog_glow",
        "sunlight_nat", "sunlight_color", "ambient_nat", "ambient_color", "light_strength",
        "body_nat", "body_color", "body_size", "body_brightness", "body_texture", "body_position", "body_az", "body_el",
        "body_period", "body_tilt", "body_phase", "body_lit_auto", "body_lit", "body_rings", "body_ring_color",
        "zone_name", "zone_shape", "zone_x", "zone_y", "zone_radius", "zone_x2", "zone_y2", "zone_alt", "zone_edge",
        "zone_weather_on", "zone_weather", "zone_clouds_on", "zone_clouds", "zone_haze_on", "zone_haze", "zone_fog_on",
        "zone_fog", "zone_fogglow_on", "zone_fog_glow", "zone_light_on", "zone_light", "zone_lightstr_on",
        "zone_light_strength", "zone_mood_on", "zone_mood_tint", "zone_mood_color", "zone_mood_strength" };
    for (const char* name : EDITS)
    {
        getChild<LLUICtrl>(name)->setCommitCallback([this](LLUICtrl*, const LLSD&) { onChanged(); });
    }
    child<LLLineEditor>(this, "place_name")->setKeystrokeCallback([](LLLineEditor*, void* d) {
        static_cast<WolfFloaterEnvironment*>(d)->onChanged(); }, this);
    child<LLLineEditor>(this, "zone_name")->setKeystrokeCallback([](LLLineEditor*, void* d) {
        static_cast<WolfFloaterEnvironment*>(d)->onChanged(); }, this);
    child<LLLineEditor>(this, "time_zone_search")->setKeystrokeCallback([](LLLineEditor*, void* d) {
        static_cast<WolfFloaterEnvironment*>(d)->fillTimeZones(); }, this);

    getChild<LLUICtrl>("preset")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPresetPicked(); });
    mScopeCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { onScopePicked(); });
    child<LLLineEditor>(this, "place_search")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSearch(); });
    childSetAction("place_search_btn", [this](LLUICtrl*, const LLSD&) { onSearch(); });
    LLScrollListCtrl* results = child<LLScrollListCtrl>(this, "place_results");
    results->setCommitOnSelectionChange(true);
    results->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        LLScrollListItem* it = child<LLScrollListCtrl>(this, "place_results")->getFirstSelected();
        if (!it || !mResults.isArray()) return;
        const S32 i = it->getValue().asInteger();
        if (i < 0 || i >= (S32)mResults.size()) return;
        mWriting = true;
        child<LLLineEditor>(this, "place_name")->setText(mResults[i]["name"].asString());
        child<LLSpinCtrl>(this, "place_lat")->setValue(mResults[i]["lat"].asReal());
        child<LLSpinCtrl>(this, "place_lon")->setValue(mResults[i]["lon"].asReal());
        child<LLComboBox>(this, "place_kind")->setValue(LLSD("real"));
        mWriting = false;
        onChanged();
    });

    LLScrollListCtrl* bodies = child<LLScrollListCtrl>(this, "bodies");
    bodies->setCommitOnSelectionChange(true);
    bodies->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        LLScrollListItem* it = child<LLScrollListCtrl>(this, "bodies")->getFirstSelected();
        mBodyIndex = it ? it->getValue().asInteger() : -1;
        writeBody();
    });
    auto add_body = [this](const std::string& kind)
    {
        if (!canEdit()) return;
        readControls();
        LLSD& list = bodyList(kind);
        if (list.size() >= 4) { setStatus("At most 4 " + kind + ".", true); return; }
        // A new body has its own id and its own path (only the first sun / moon may follow the real sky).
        std::string base = kind == "planets" ? "planet" : kind.substr(0, kind.size() - 1);
        std::string id;
        for (S32 n = (S32)list.size() + 1; ; ++n)
        {
            id = base + std::to_string(n);
            bool used = false;
            for (const LLSD& b : llsd::inArray(list)) used = used || b["id"].asString() == id;
            if (!used) break;
        }
        LLSD b;
        b["id"] = id;
        b["color"] = kind == "planets" ? LLSD("#c09070") : LLSD();
        b["size"] = LLSD::Integer(100);
        if (kind == "planets")
        {
            b["azimuth"] = LLSD::Real(180.0); b["elevation"] = LLSD::Real(30.0);
            b["rings"] = false; b["ringColor"] = "#d0c0a0";
        }
        else
        {
            b["brightness"] = LLSD::Integer(100);
            b["texture"] = LLSD();
            LLSD pos;
            pos["mode"] = "orbit"; pos["periodHours"] = LLSD::Real(24.0); pos["tilt"] = LLSD::Real(20.0); pos["phase"] = LLSD::Real(90.0);
            b["position"] = pos;
            if (kind == "moons") b["phase"] = LLSD();
        }
        list.append(b);
        mDoc["preset"] = "custom";
        fillBodies();
        mBodyIndex = (S32)mBodyRows.size() - 1;
        child<LLScrollListCtrl>(this, "bodies")->selectNthItem(mBodyIndex);
        writeBody();
        onChanged();
    };
    childSetAction("add_sun", [add_body](LLUICtrl*, const LLSD&) { add_body("suns"); });
    childSetAction("add_moon", [add_body](LLUICtrl*, const LLSD&) { add_body("moons"); });
    childSetAction("add_planet", [add_body](LLUICtrl*, const LLSD&) { add_body("planets"); });
    childSetAction("remove_body", [this](LLUICtrl*, const LLSD&)
    {
        if (!canEdit() || mBodyIndex < 0 || mBodyIndex >= (S32)mBodyRows.size()) return;
        readControls();
        const std::string kind = mBodyRows[mBodyIndex][0].asString();
        const S32 at = mBodyRows[mBodyIndex][1].asInteger();
        LLSD& list = bodyList(kind);
        LLSD kept = LLSD::emptyArray();
        for (S32 i = 0; i < (S32)list.size(); ++i) if (i != at) kept.append(list[i]);
        list = kept;
        mDoc["preset"] = "custom";
        mBodyIndex = -1;
        fillBodies();
        writeBody();
        onChanged();
    });

    LLScrollListCtrl* zones = child<LLScrollListCtrl>(this, "zones");
    zones->setCommitOnSelectionChange(true);
    zones->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        LLScrollListItem* it = child<LLScrollListCtrl>(this, "zones")->getFirstSelected();
        mZoneIndex = it ? it->getValue().asInteger() : -1;
        writeZone();
    });
    childSetAction("zone_add", [this](LLUICtrl*, const LLSD&)
    {
        if (!canEdit()) return;
        readControls();
        LLSD& zl = mDoc["zones"];
        if (!zl.isArray()) zl = LLSD::emptyArray();
        if (zl.size() >= 16) { setStatus("At most 16 zones.", true); return; }
        std::string id;
        for (S32 n = (S32)zl.size() + 1; ; ++n)
        {
            id = "zone" + std::to_string(n);
            bool used = false;
            for (const LLSD& z : llsd::inArray(zl)) used = used || z["id"].asString() == id;
            if (!used) break;
        }
        // A new zone: a 20 m circle where the agent stands.
        const LLVector3 at = gAgent.getPositionRegion();   // <WolfViewer> zone x/y are region coordinates; agent frame may be rebased
        LLSD shape;
        shape["type"] = "circle";
        shape["x"] = LLSD::Real(llmax(0.f, at.mV[VX]));
        shape["y"] = LLSD::Real(llmax(0.f, at.mV[VY]));
        shape["radius"] = LLSD::Real(20.0);
        LLSD z;
        z["id"] = id; z["name"] = "Zone " + std::to_string(zl.size() + 1); z["shape"] = shape; z["edge"] = LLSD::Real(5.0);
        z["set"] = LLSD::emptyMap();
        zl.append(z);
        fillZones();
        mZoneIndex = (S32)zl.size() - 1;
        child<LLScrollListCtrl>(this, "zones")->selectNthItem(mZoneIndex);
        writeZone();
        onChanged();
    });
    childSetAction("zone_remove", [this](LLUICtrl*, const LLSD&)
    {
        if (!canEdit() || mZoneIndex < 0 || mZoneIndex >= (S32)mDoc["zones"].size()) return;
        readControls();
        LLSD kept = LLSD::emptyArray();
        for (S32 i = 0; i < (S32)mDoc["zones"].size(); ++i) if (i != mZoneIndex) kept.append(mDoc["zones"][i]);
        mDoc["zones"] = kept;
        mZoneIndex = -1;
        fillZones();
        writeZone();
        onChanged();
    });
    childSetAction("zone_here", [this](LLUICtrl*, const LLSD&)
    {
        const LLVector3 at = gAgent.getPositionRegion();   // <WolfViewer> zone x/y are region coordinates; agent frame may be rebased
        mWriting = true;
        child<LLSpinCtrl>(this, "zone_x")->setValue(llmax(0.f, at.mV[VX]));
        child<LLSpinCtrl>(this, "zone_y")->setValue(llmax(0.f, at.mV[VY]));
        mWriting = false;
        onChanged();
    });

    childSetAction("save", [this](LLUICtrl*, const LLSD&) { post("save"); });
    childSetAction("revert", [this](LLUICtrl*, const LLSD&) { onRevert(); });
    childSetAction("remove", [this](LLUICtrl*, const LLSD&) { onRemoveAsked(); });
    mPreviewBtn->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        if (mPreviewOn) stopPreview();
        else startPreview();
    });
    writeControls();
    return true;
}

void WolfFloaterEnvironment::onOpen(const LLSD& key)
{
    // [WOLF GRID GATE] Wolf only: anywhere else the window does not open (the menu item is gone too).
    if (!WolfGrid::isOnWolfTerritories())
    {
        WolfGrid::noteWolfOnly("Wolf Environment");   // [WOLF GRID GATE 2026-10-17/]
        closeFloater();
        return;
    }
    LLFloater::onOpen(key);
    mFetchedFor.clear();
}

void WolfFloaterEnvironment::onClose(bool app_quitting)
{
    // A preview never outlives the window (as the auto environment's preview does not outlive its tab).
    // The time slider first, so ending the draft does not run the shared sky ahead again.
    if (mTimeOn && mTimeOn->getValue().asBoolean() && !app_quitting)
    {
        mTimeOn->setValue(false);
        if (!mPreviewOn) endTime();
    }
    if (mPreviewOn && !app_quitting) stopPreview();
    if (!app_quitting) endTime();
    LLFloater::onClose(app_quitting);
}

std::string WolfFloaterEnvironment::regionId() const
{
    LLViewerRegion* r = gAgent.getRegion();
    return r ? r->getRegionID().asString() : std::string();
}

bool WolfFloaterEnvironment::canEdit() const
{
    if (!mHave || !mReply["you"].isMap()) return false;
    return mScope == "parcel" ? mReply["you"]["canEditParcel"].asBoolean() : mReply["you"]["canEditRegion"].asBoolean();
}

LLSD& WolfFloaterEnvironment::scopeEntry()
{
    return mScope == "parcel" ? mReply["parcel"]["env"] : mReply["regionEnv"];
}

LLSD& WolfFloaterEnvironment::bodyList(const std::string& kind)
{
    LLSD& sky = mDoc["sky"];
    if (kind == "planets")
    {
        LLSD& l = sky["night"]["planets"];
        if (!l.isArray()) l = LLSD::emptyArray();
        return l;
    }
    LLSD& l = sky[kind];
    if (!l.isArray()) l = LLSD::emptyArray();
    return l;
}

void WolfFloaterEnvironment::setStatus(const std::string& msg, bool error)
{
    // Errors stay until the next action (they never time out); red that reads on the dark floater.
    mStatus->setText(msg);
    mStatus->setColor(error ? LLColor4(1.f, 0.45f, 0.4f, 1.f) : LLColor4(0.79f, 0.64f, 0.15f, 1.f));
}

void WolfFloaterEnvironment::draw()
{
    if (!WolfGrid::isOnWolfTerritories())
    {
        // [WOLF GRID GATE] a hypergrid jump away closes it (and ends any preview: onClose)
        closeFloater();
        return;
    }
    const std::string id = regionId();
    LLParcel* parcel = LLViewerParcelMgr::getInstance()->getAgentParcel();
    const S32 local = parcel ? parcel->getLocalID() : -1;
    // [WOLF GRID GATE 2026-10-17] fetch() waits for this region's own word; until then the reset below must not run (it
    // would run every frame and wipe what is being edited)
    if (!mBusy && !id.empty() && WolfGrid::isOnWolfTerritoriesConfirmed())
    {
        if (id != mFetchedFor)
        {
            // a preview belongs to the region it was made in, the time slider's too
            if (mTimeOn && mTimeOn->getValue().asBoolean()) { mTimeOn->setValue(false); if (!mPreviewOn) endTime(); }
            if (mPreviewOn) stopPreview();
            mDirty = false;
            mHave = false;
            mScope = "region";
            fetch();
        }
        else if (local != mParcelLocalId && !mDirty)
        {
            fetch();                          // walked onto another parcel: read what is there
        }
    }
    if (mPreviewPending && LLFrameTimer::getElapsedSeconds() >= mPreviewDue && !mBusy) startPreview();
    // [WOLF ENV 2026-10-12] the time slider's preview ends when something else ends it (a personal sky,
    // Jimmy's tab, a region change), as wolfautoenvironment.cpp's does; the readout follows the clock
    if (timeOn() && !mPreviewOn && mTimeOwned && !WolfRegionWeather::instance().autoEnvPreviewOn())
    {
        mTimeOn->setValue(false);
        mTimeOwned = false;
        mTime->setEnabled(false);
        WolfEnvironmentState::instance().setTimeAhead(0.0);
        showTime();
    }
    static F64 s_lastTimeText = 0.0;
    const F64 t = LLFrameTimer::getElapsedSeconds();
    if (t - s_lastTimeText > 1.0) { s_lastTimeText = t; showTime(); }
    LLFloater::draw();
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
// reading
// ═══════════════════════════════════════════════════════════════════════════════════════════

void WolfFloaterEnvironment::fetch()
{
    // [WOLF GRID GATE 2026-10-17] carries our session: only once this region has said it is Wolf (draw() tries again)
    if (!WolfGrid::isOnWolfTerritoriesConfirmed()) return;
    const std::string id = regionId();
    if (id.empty()) return;
    mFetchedFor = id;
    LLParcel* parcel = LLViewerParcelMgr::getInstance()->getAgentParcel();
    mParcelLocalId = parcel ? parcel->getLocalID() : -1;
    const LLVector3 at = gAgent.getPositionRegion();   // <WolfViewer> zone x/y are region coordinates; agent frame may be rebased
    mAtX = llmax(0.f, at.mV[VX]);
    mAtY = llmax(0.f, at.mV[VY]);
    if (!mHave) setStatus(getString("str_loading"), false);
    mBusy = true;
    const std::string url = llformat("%s?region=%s&x=%.2f&y=%.2f", API_URL, id.c_str(), mAtX, mAtY);
    const LLHandle<LLFloater> h = getHandle();
    const U64 serial = ++mSerial;
    LLCoros::instance().launch("WolfEnvGet", [h, url, serial]() { getCoro(h, url, serial); });
    // the sky's clock for the time slider's readout (auto_environment.php: clockUtcOffset, clockLabel)
    const std::string clock_url = llformat("%s?region=%s", AUTO_ENV_URL, id.c_str());
    LLCoros::instance().launch("WolfEnvClock", [h, clock_url, id]() { clockCoro(h, clock_url, id); });
    if (!sZonesAsked)
    {
        sZonesAsked = true;
        LLCoros::instance().launch("WolfEnvZones", [h]() { zonesCoro(h); });
    }
}

// static
void WolfFloaterEnvironment::getCoro(LLHandle<LLFloater> handle, std::string url, U64 serial)
{
    auto adapter = std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfEnvironment", LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
    LLCore::HttpOptions::ptr_t options = WolfGrid::makeVerifiedHttpOptions();
    options->setTimeout(30);
    LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
    headers->append(HTTP_OUT_HEADER_ACCEPT, "application/json");
    headers->append("X-Wolf-Agent", gAgentID.asString());
    headers->append("X-Wolf-Session", gAgentSessionID.asString());
    LLSD result = adapter->getRawAndSuspend(request, url, options, headers);
    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
    LLSD reply = raw_reply(result);
    WolfFloaterEnvironment* self = static_cast<WolfFloaterEnvironment*>(handle.get());
    if (!self || serial != self->mSerial) { if (self) self->mBusy = false; return; }
    const bool ok = status && reply.isMap() && reply["success"].asBoolean();
    const std::string why = reply.isMap() && reply.has("error") ? reply["error"].asString() : status.toString();
    self->onGetReply(reply, ok, why);
}

void WolfFloaterEnvironment::onGetReply(const LLSD& reply, bool ok, const std::string& why)
{
    mBusy = false;
    if (!ok)
    {
        setStatus(getString("str_read_failed") + " " + why, true);
        return;
    }
    applyReply(reply);
}

// static
void WolfFloaterEnvironment::zonesCoro(LLHandle<LLFloater> handle)
{
    auto adapter = std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfEnvironment", LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
    LLCore::HttpOptions::ptr_t options = WolfGrid::makeVerifiedHttpOptions();
    options->setTimeout(20);
    LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
    headers->append(HTTP_OUT_HEADER_ACCEPT, "application/json");
    LLSD result = adapter->getRawAndSuspend(request, std::string(AUTO_ENV_URL) + "?timezones=1", options, headers);
    LLSD reply = raw_reply(result);
    if (!reply.isMap() || !reply["zones"].isArray())
    {
        sZonesAsked = false;   // asked again the next time a region is read
        LL_WARNS("WolfEnv") << "time zone list not read" << LL_ENDL;
        return;
    }
    sZones = reply["zones"];
    if (WolfFloaterEnvironment* self = static_cast<WolfFloaterEnvironment*>(handle.get()))
    {
        self->mWriting = true;
        self->fillTimeZones();
        self->mWriting = false;
    }
}

void WolfFloaterEnvironment::applyReply(const LLSD& reply)
{
    mReply = reply;
    mHave = true;
    // The scope list: the region, and the parcel the agent stands on (when the web found one).
    mWriting = true;
    mScopeCombo->removeall();
    mScopeCombo->add("This region: " + reply["region"]["name"].asString(), LLSD("region"));
    if (reply["parcel"].isMap())
        mScopeCombo->add("This parcel: " + reply["parcel"]["name"].asString(), LLSD("parcel"));
    else if (mScope == "parcel")
        mScope = "region";
    mScopeCombo->setValue(LLSD(mScope));
    mWriting = false;

    const LLSD& entry = scopeEntry();
    mVersion = mScope == "parcel" ? reply["parcel"]["version"].asInteger() : reply["regionVersion"].asInteger();
    if (!mDirty) mDoc = entry.isMap() ? entry["doc"] : reply["defaultDoc"];
    mBodyIndex = -1;
    mZoneIndex = -1;
    writeControls();
    if (!entry.isMap() && !mDirty) setStatus(getString("str_none_yet"), false);
    else if (!mDirty) setStatus(LLStringUtil::null, false);
}

void WolfFloaterEnvironment::onScopePicked()
{
    if (mWriting) return;
    const std::string want = mScopeCombo->getValue().asString();
    if (want == mScope) return;
    if (mDirty)
    {
        // Switching would throw the edit away: say so and stay.
        mWriting = true;
        mScopeCombo->setValue(LLSD(mScope));
        mWriting = false;
        setStatus("Save or Revert your changes before switching between the region and the parcel.", true);
        return;
    }
    if (mPreviewOn) stopPreview();
    mScope = want;
    applyReply(mReply);
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
// the document -> the controls
// ═══════════════════════════════════════════════════════════════════════════════════════════

void WolfFloaterEnvironment::fillTimeZones()
{
    LLComboBox* c = child<LLComboBox>(this, "time_zone");
    std::string keep = c->getValue().asString();
    if (keep.empty() && mDoc["time"]["mode"].asString() == "zone") keep = mDoc["time"]["zone"].asString();
    std::string filter = child<LLLineEditor>(this, "time_zone_search")->getText();
    LLStringUtil::replaceChar(filter, '_', ' ');
    LLStringUtil::toLower(filter);
    LLStringUtil::trim(filter);
    c->removeall();
    for (const LLSD& z : llsd::inArray(sZones))
    {
        std::string label = z.asString();
        LLStringUtil::replaceChar(label, '_', ' ');
        std::string lower = label;
        LLStringUtil::toLower(lower);
        if (!filter.empty() && lower.find(filter) == std::string::npos) continue;
        c->add(label, z);
    }
    if (keep.empty()) keep = "Europe/London";
    if (!c->getItemByValue(LLSD(keep)))
    {
        std::string label = keep;
        LLStringUtil::replaceChar(label, '_', ' ');
        c->add(label, LLSD(keep));
    }
    c->setValue(LLSD(keep));
}

void WolfFloaterEnvironment::writeControls()
{
    mWriting = true;
    const LLSD& d = mDoc;
    const std::string mode = d["mode"].asString();
    mMode->setSelectedIndex(mode == "on-top" ? 1 : mode == "off" ? 2 : 0);
    child<LLComboBox>(this, "preset")->setValue(LLSD(d.has("preset") ? d["preset"].asString() : std::string("earth")));
    const LLSD& p = d["place"];
    child<LLComboBox>(this, "place_kind")->setValue(LLSD(p["kind"].asString().empty() ? std::string("real") : p["kind"].asString()));
    child<LLLineEditor>(this, "place_name")->setText(p["name"].asString());
    child<LLSpinCtrl>(this, "place_lat")->setValue(p["lat"].asReal());
    child<LLSpinCtrl>(this, "place_lon")->setValue(p["lon"].asReal());
    const LLSD& t = d["time"];
    child<LLComboBox>(this, "time_mode")->setValue(LLSD(t["mode"].asString().empty() ? std::string("real") : t["mode"].asString()));
    if (t["mode"].asString() == "zone") { fillTimeZones(); child<LLComboBox>(this, "time_zone")->setValue(t["zone"]); }
    child<LLSliderCtrl>(this, "time_hour")->setValue((F32)(t.has("hour") ? t["hour"].asReal() : 12.0));
    child<LLSpinCtrl>(this, "time_day_hours")->setValue(t.has("dayHours") ? t["dayHours"].asReal() : 4.0);
    const LLSD& w = d["weather"];
    child<LLComboBox>(this, "weather_source")->setValue(LLSD(w["source"].asString().empty() ? std::string("live") : w["source"].asString()));
    child<LLComboBox>(this, "weather_pattern")->setValue(LLSD(w.has("pattern") ? w["pattern"].asString() : std::string("changeable")));
    child<LLComboBox>(this, "weather_type")->setValue(LLSD(w.has("type") ? w["type"].asString() : std::string("fair")));
    const bool autoCloud = !d.has("clouds") || d["clouds"].isUndefined();
    child<LLCheckBoxCtrl>(this, "clouds_auto")->set(autoCloud);
    child<LLSliderCtrl>(this, "clouds")->setValue((F32)(autoCloud ? 50.0 : d["clouds"].asReal()));
    child<LLSliderCtrl>(this, "haze")->setValue((F32)(d["haze"].asReal()));
    child<LLComboBox>(this, "water")->setValue(LLSD(d["water"]["look"].asString().empty() ? std::string("natural") : d["water"]["look"].asString()));
    child<LLComboBox>(this, "mood_tint")->setValue(LLSD(d["mood"]["tint"].asString().empty() ? std::string("neutral") : d["mood"]["tint"].asString()));
    child<LLColorSwatchCtrl>(this, "mood_color")->set(hex_to_color(d["mood"]["color"]));
    child<LLSliderCtrl>(this, "mood_strength")->setValue((F32)(d["mood"]["strength"].asReal()));

    // the sky block
    const LLSD& s = d["sky"];
    const LLSD& a = s["atmosphere"];
    auto natural = [this](const char* chk, const char* sw, const LLSD& v)
    {
        child<LLCheckBoxCtrl>(this, chk)->set(v.isUndefined());
        if (!v.isUndefined()) child<LLColorSwatchCtrl>(this, sw)->set(hex_to_color(v));
    };
    natural("ray_nat", "ray_color", a["rayleigh"]);
    natural("mie_nat", "mie_color", a["mie"]);
    natural("ozone_nat", "ozone_color", a["ozone"]);
    child<LLSliderCtrl>(this, "ray_density")->setValue((F32)(a.has("rayleighDensity") ? a["rayleighDensity"].asReal() : 100.0));
    child<LLSliderCtrl>(this, "mie_density")->setValue((F32)(a.has("mieDensity") ? a["mieDensity"].asReal() : 100.0));
    child<LLSliderCtrl>(this, "ozone_density")->setValue((F32)(a.has("ozoneDensity") ? a["ozoneDensity"].asReal() : 100.0));
    child<LLSliderCtrl>(this, "mie_glow")->setValue((F32)(a.has("mieAnisotropy") ? a["mieAnisotropy"].asReal() : 80.0));
    child<LLSpinCtrl>(this, "planet_km")->setValue(a.has("planetRadiusKm") ? a["planetRadiusKm"].asReal() : 6360.0);
    child<LLSpinCtrl>(this, "atmos_km")->setValue(a.has("atmosphereKm") ? a["atmosphereKm"].asReal() : 60.0);
    const LLSD& n = s["night"];
    child<LLSliderCtrl>(this, "stars")->setValue((F32)(n.has("stars") ? n["stars"].asReal() : 100.0));
    natural("star_nat", "star_color", n["starColor"]);
    child<LLSliderCtrl>(this, "nebula")->setValue((F32)(n["nebula"].asReal()));
    child<LLColorSwatchCtrl>(this, "nebula_color")->set(hex_to_color(n.has("nebulaColor") ? n["nebulaColor"] : LLSD("#6040a0")));
    const LLSD& c = s["clouds"];
    natural("cloud_nat", "cloud_color", c["color"]);
    child<LLSliderCtrl>(this, "cloud_glow")->setValue((F32)(c["emission"].asReal()));
    child<LLComboBox>(this, "cloud_style")->setValue(LLSD(c["style"].asString().empty() ? std::string("natural") : c["style"].asString()));
    child<LLTextureCtrl>(this, "cloud_texture")->setImageAssetID(c["texture"].isUndefined() ? LLUUID::null : c["texture"].asUUID());
    natural("fog_nat", "fog_color", s["fog"]["color"]);
    child<LLSliderCtrl>(this, "fog_glow")->setValue((F32)(s["fog"]["glow"].asReal()));
    natural("sunlight_nat", "sunlight_color", s["light"]["sun"]);
    natural("ambient_nat", "ambient_color", s["light"]["ambient"]);
    child<LLSliderCtrl>(this, "light_strength")->setValue((F32)(s["light"].has("strength") ? s["light"]["strength"].asReal() : 100.0));
    mWriting = false;

    fillBodies();
    writeBody();
    fillZones();
    writeZone();
    updateVisibility();
    showNow();

    // Who may change it, and the estate's parcel switch.
    std::string who;
    if (mHave)
    {
        who = canEdit() ? getString(mScope == "parcel" ? "str_can_parcel" : "str_can_region")
                        : getString(mScope == "parcel" ? "str_cannot_parcel" : "str_cannot_region");
        if (mScope == "parcel" && !mReply["estateAllowsParcel"].asBoolean()) who += " " + getString("str_no_parcel_env");
    }
    mWho->setText(who);
}

void WolfFloaterEnvironment::fillBodies()
{
    LLScrollListCtrl* list = child<LLScrollListCtrl>(this, "bodies");
    list->deleteAllItems();
    mBodyRows = LLSD::emptyArray();
    const LLSD& s = mDoc["sky"];
    auto add = [&](const std::string& kind, const LLSD& l, const char* word)
    {
        for (S32 i = 0; i < (S32)l.size(); ++i)
        {
            const LLSD& b = l[i];
            std::string where = kind == "planets" ? "fixed in the sky"
                              : b["position"]["mode"].asString() == "real" ? "the real sky"
                              : b["position"]["mode"].asString() == "fixed" ? "fixed in the sky" : "its own path";
            LLSD row = LLSD::emptyArray();
            row.append(kind);
            row.append(i);
            list->addSimpleElement(llformat("%s %d (%s)", word, i + 1, where.c_str()), ADD_BOTTOM, LLSD((S32)mBodyRows.size()));
            mBodyRows.append(row);
        }
    };
    add("suns", s["suns"], "Sun");
    add("moons", s["moons"], "Moon");
    add("planets", s["night"]["planets"], "Planet");
    if (mBodyIndex >= 0 && mBodyIndex < (S32)mBodyRows.size()) list->selectNthItem(mBodyIndex);
    else mBodyIndex = -1;
}

void WolfFloaterEnvironment::writeBody()
{
    mWriting = true;
    const bool have = mBodyIndex >= 0 && mBodyIndex < (S32)mBodyRows.size();
    std::string kind;
    LLSD b;
    if (have)
    {
        kind = mBodyRows[mBodyIndex][0].asString();
        b = bodyList(kind)[mBodyRows[mBodyIndex][1].asInteger()];
    }
    const bool planet = kind == "planets";
    child<LLCheckBoxCtrl>(this, "body_nat")->set(!planet && b["color"].isUndefined());
    if (!b["color"].isUndefined()) child<LLColorSwatchCtrl>(this, "body_color")->set(hex_to_color(b["color"]));
    LLSliderCtrl* size = child<LLSliderCtrl>(this, "body_size");
    size->setMinValue(planet ? 1.f : 25.f);
    size->setMaxValue(planet ? 400.f : 2000.f);
    size->setValue((F32)(b.has("size") ? b["size"].asReal() : 100.0));
    child<LLSliderCtrl>(this, "body_brightness")->setValue((F32)(b.has("brightness") ? b["brightness"].asReal() : 100.0));
    child<LLTextureCtrl>(this, "body_texture")->setImageAssetID(b["texture"].isUndefined() ? LLUUID::null : b["texture"].asUUID());
    const LLSD& pos = b["position"];
    const std::string pmode = planet ? std::string("fixed") : (pos["mode"].asString().empty() ? std::string("real") : pos["mode"].asString());
    child<LLComboBox>(this, "body_position")->setValue(LLSD(pmode));
    child<LLSpinCtrl>(this, "body_az")->setValue(planet ? b["azimuth"].asReal() : pos["azimuth"].asReal());
    child<LLSpinCtrl>(this, "body_el")->setValue(planet ? b["elevation"].asReal() : (pos.has("elevation") ? pos["elevation"].asReal() : 30.0));
    child<LLSpinCtrl>(this, "body_period")->setValue(pos.has("periodHours") ? pos["periodHours"].asReal() : 24.0);
    child<LLSpinCtrl>(this, "body_tilt")->setValue(pos["tilt"].asReal());
    child<LLSpinCtrl>(this, "body_phase")->setValue(pos["phase"].asReal());
    child<LLCheckBoxCtrl>(this, "body_lit_auto")->set(b["phase"].isUndefined());
    child<LLSliderCtrl>(this, "body_lit")->setValue((F32)(b["phase"].isUndefined() ? 50.0 : b["phase"].asReal()));
    child<LLCheckBoxCtrl>(this, "body_rings")->set(b["rings"].asBoolean());
    child<LLColorSwatchCtrl>(this, "body_ring_color")->set(hex_to_color(b.has("ringColor") ? b["ringColor"] : LLSD("#d0c0a0")));
    mWriting = false;
    updateVisibility();
}

void WolfFloaterEnvironment::fillZones()
{
    LLScrollListCtrl* list = child<LLScrollListCtrl>(this, "zones");
    list->deleteAllItems();
    const LLSD& zl = mDoc["zones"];
    for (S32 i = 0; i < (S32)zl.size(); ++i)
    {
        const LLSD& z = zl[i];
        const std::string type = z["shape"]["type"].asString();
        std::string what = type == "above" ? llformat("above %.0f m", z["shape"]["altitude"].asReal())
                         : type == "rect" ? std::string("rectangle") : llformat("circle, %.0f m", z["shape"]["radius"].asReal());
        list->addSimpleElement(llformat("%s (%s)", z["name"].asString().empty() ? z["id"].asString().c_str() : z["name"].asString().c_str(),
                                        what.c_str()), ADD_BOTTOM, LLSD(i));
    }
    if (mZoneIndex >= 0 && mZoneIndex < (S32)zl.size()) list->selectNthItem(mZoneIndex);
    else mZoneIndex = -1;
}

void WolfFloaterEnvironment::writeZone()
{
    mWriting = true;
    LLSD z;
    if (mZoneIndex >= 0 && mZoneIndex < (S32)mDoc["zones"].size()) z = mDoc["zones"][mZoneIndex];
    const LLSD& sh = z["shape"];
    const LLSD& set = z["set"];
    child<LLLineEditor>(this, "zone_name")->setText(z["name"].asString());
    child<LLComboBox>(this, "zone_shape")->setValue(LLSD(sh["type"].asString().empty() ? std::string("circle") : sh["type"].asString()));
    const bool rect = sh["type"].asString() == "rect";
    child<LLSpinCtrl>(this, "zone_x")->setValue(rect ? sh["x1"].asReal() : sh["x"].asReal());
    child<LLSpinCtrl>(this, "zone_y")->setValue(rect ? sh["y1"].asReal() : sh["y"].asReal());
    child<LLSpinCtrl>(this, "zone_radius")->setValue(sh.has("radius") ? sh["radius"].asReal() : 20.0);
    child<LLSpinCtrl>(this, "zone_x2")->setValue(sh["x2"].asReal());
    child<LLSpinCtrl>(this, "zone_y2")->setValue(sh["y2"].asReal());
    child<LLSpinCtrl>(this, "zone_alt")->setValue(sh.has("altitude") ? sh["altitude"].asReal() : 1000.0);
    child<LLSpinCtrl>(this, "zone_edge")->setValue(z["edge"].asReal());
    child<LLCheckBoxCtrl>(this, "zone_weather_on")->set(set.has("weather"));
    child<LLComboBox>(this, "zone_weather")->setValue(LLSD(set.has("weather") ? set["weather"]["type"].asString() : std::string("fog")));
    child<LLCheckBoxCtrl>(this, "zone_clouds_on")->set(set.has("clouds"));
    child<LLSliderCtrl>(this, "zone_clouds")->setValue((F32)(set.has("clouds") ? set["clouds"].asReal() : 50.0));
    child<LLCheckBoxCtrl>(this, "zone_haze_on")->set(set.has("haze"));
    child<LLSliderCtrl>(this, "zone_haze")->setValue((F32)(set.has("haze") ? set["haze"].asReal() : 50.0));
    child<LLCheckBoxCtrl>(this, "zone_fog_on")->set(set.has("fogColor"));
    child<LLColorSwatchCtrl>(this, "zone_fog")->set(hex_to_color(set.has("fogColor") ? set["fogColor"] : LLSD("#8090a0")));
    child<LLCheckBoxCtrl>(this, "zone_fogglow_on")->set(set.has("fogGlow"));
    child<LLSliderCtrl>(this, "zone_fog_glow")->setValue((F32)(set.has("fogGlow") ? set["fogGlow"].asReal() : 20.0));
    child<LLCheckBoxCtrl>(this, "zone_light_on")->set(set.has("light"));
    child<LLColorSwatchCtrl>(this, "zone_light")->set(hex_to_color(set.has("light") ? set["light"] : LLSD("#ffd0a0")));
    child<LLCheckBoxCtrl>(this, "zone_lightstr_on")->set(set.has("lightStrength"));
    child<LLSliderCtrl>(this, "zone_light_strength")->setValue((F32)(set.has("lightStrength") ? set["lightStrength"].asReal() : 100.0));
    child<LLCheckBoxCtrl>(this, "zone_mood_on")->set(set.has("mood"));
    child<LLComboBox>(this, "zone_mood_tint")->setValue(LLSD(set.has("mood") ? set["mood"]["tint"].asString() : std::string("cool")));
    child<LLColorSwatchCtrl>(this, "zone_mood_color")->set(hex_to_color(set.has("mood") ? set["mood"]["color"] : LLSD("#ffffff")));
    child<LLSliderCtrl>(this, "zone_mood_strength")->setValue((F32)(set.has("mood") ? set["mood"]["strength"].asReal() : 50.0));
    mWriting = false;
    updateVisibility();
}

/** Only what applies is shown or enabled (Hick: fewer choices at a time). */
void WolfFloaterEnvironment::updateVisibility()
{
    const bool edit = canEdit() && !mBusy;
    const std::string kind = child<LLComboBox>(this, "place_kind")->getValue().asString();
    const std::string tmode = child<LLComboBox>(this, "time_mode")->getValue().asString();
    const std::string src = child<LLComboBox>(this, "weather_source")->getValue().asString();
    const bool real = kind == "real";
    auto show = [this](const char* n, bool v) { getChildView(n)->setVisible(v); };
    show("place_search", real); show("place_search_btn", real); show("place_results", real); show("place_lon", real);
    show("place_lat", kind != "none"); show("place_name", kind != "none"); show("place_name_lbl", kind != "none");
    show("time_zone", tmode == "zone"); show("time_zone_search", tmode == "zone");
    show("time_hour", tmode == "fixed"); show("time_hour_text", tmode == "fixed");
    show("time_day_hours", tmode == "cycle");
    show("weather_pattern", src == "pattern"); show("weather_type", src == "fixed");
    const F32 hour = child<LLSliderCtrl>(this, "time_hour")->getValueF32();
    child<LLTextBox>(this, "time_hour_text")->setText(llformat("%02d:%02d there", (int)hour, (int)ll_round((hour - (int)hour) * 60.f) % 60));

    // Suns, moons, planets: the fields their kind has.
    const bool haveBody = mBodyIndex >= 0 && mBodyIndex < (S32)mBodyRows.size();
    const std::string bkind = haveBody ? mBodyRows[mBodyIndex][0].asString() : std::string();
    const bool first = haveBody && mBodyRows[mBodyIndex][1].asInteger() == 0;
    const bool planet = bkind == "planets";
    const std::string pmode = child<LLComboBox>(this, "body_position")->getValue().asString();
    for (const char* n : { "body_color_lbl", "body_nat", "body_color", "body_size", "body_hdr" }) show(n, haveBody);
    show("body_brightness", haveBody && !planet); show("body_texture", haveBody && !planet);
    show("body_pos_lbl", haveBody && !planet); show("body_position", haveBody && !planet);
    show("body_az", haveBody && (planet || pmode == "fixed")); show("body_el", haveBody && (planet || pmode == "fixed"));
    show("body_period", haveBody && !planet && pmode == "orbit"); show("body_tilt", haveBody && !planet && pmode == "orbit");
    show("body_phase", haveBody && !planet && pmode == "orbit");
    show("body_lit_auto", haveBody && bkind == "moons"); show("body_lit", haveBody && bkind == "moons");
    show("body_rings", haveBody && planet); show("body_ring_color", haveBody && planet);
    show("body_nat", haveBody && !planet);
    // Only the first sun / moon may take the real sky (the web refuses more).
    if (LLComboBox* pc = child<LLComboBox>(this, "body_position")) pc->setEnabledByValue(LLSD("real"), first);

    // Zones: the fields their shape has.
    const bool haveZone = mZoneIndex >= 0 && mZoneIndex < (S32)mDoc["zones"].size();
    const std::string shape = child<LLComboBox>(this, "zone_shape")->getValue().asString();
    for (const char* n : { "zone_name_lbl", "zone_name", "zone_shape", "zone_set_hdr", "zone_weather_on", "zone_weather",
                           "zone_clouds_on", "zone_clouds", "zone_haze_on", "zone_haze", "zone_fog_on", "zone_fog",
                           "zone_fogglow_on", "zone_fog_glow", "zone_light_on", "zone_light", "zone_lightstr_on",
                           "zone_light_strength", "zone_mood_on", "zone_mood_tint", "zone_mood_color", "zone_mood_strength" })
        show(n, haveZone);
    show("zone_x", haveZone && shape != "above"); show("zone_y", haveZone && shape != "above");
    show("zone_radius", haveZone && shape == "circle"); show("zone_x2", haveZone && shape == "rect"); show("zone_y2", haveZone && shape == "rect");
    show("zone_alt", haveZone && shape == "above"); show("zone_here", haveZone && shape != "above"); show("zone_edge", haveZone && shape != "above");
    child<LLSpinCtrl>(this, "zone_x")->setLabel(shape == "rect" ? std::string("From East (m)") : std::string("East (m)"));
    child<LLSpinCtrl>(this, "zone_y")->setLabel(shape == "rect" ? std::string("From North") : std::string("North (m)"));

    // Read-only for anyone who may not save; the tabs stay browsable.
    static const char* const EDITABLE[] = {
        "mode", "preset", "place_kind", "place_search", "place_search_btn", "place_results", "place_name", "place_lat", "place_lon",
        "time_mode", "time_zone", "time_zone_search", "time_hour", "time_day_hours", "weather_source", "weather_pattern", "weather_type",
        "clouds_auto", "clouds", "haze", "water", "mood_tint", "mood_color", "mood_strength", "ray_nat", "ray_color", "ray_density",
        "mie_nat", "mie_color", "mie_density", "ozone_nat", "ozone_color", "ozone_density", "mie_glow", "planet_km", "atmos_km",
        "stars", "star_nat", "star_color", "nebula", "nebula_color", "cloud_nat", "cloud_color", "cloud_glow", "cloud_style",
        "cloud_texture", "fog_nat", "fog_color", "fog_glow", "sunlight_nat", "sunlight_color", "ambient_nat", "ambient_color",
        "light_strength", "add_sun", "add_moon", "add_planet", "remove_body", "body_nat", "body_color", "body_size",
        "body_brightness", "body_texture", "body_position", "body_az", "body_el", "body_period", "body_tilt", "body_phase",
        "body_lit_auto", "body_lit", "body_rings", "body_ring_color", "zone_add", "zone_remove", "zone_name", "zone_shape",
        "zone_x", "zone_y", "zone_radius", "zone_x2", "zone_y2", "zone_alt", "zone_here", "zone_edge", "zone_weather_on",
        "zone_weather", "zone_clouds_on", "zone_clouds", "zone_haze_on", "zone_haze", "zone_fog_on", "zone_fog",
        "zone_fogglow_on", "zone_fog_glow", "zone_light_on", "zone_light", "zone_lightstr_on", "zone_light_strength",
        "zone_mood_on", "zone_mood_tint", "zone_mood_color", "zone_mood_strength", "save", "revert" };
    for (const char* n : EDITABLE) getChildView(n)->setEnabled(edit);
    // the dependent controls
    getChildView("clouds")->setEnabled(edit && !child<LLCheckBoxCtrl>(this, "clouds_auto")->get());
    const bool customMood = child<LLComboBox>(this, "mood_tint")->getValue().asString() == "custom";
    getChildView("mood_color")->setEnabled(edit && customMood);
    getChildView("mood_strength")->setEnabled(edit && child<LLComboBox>(this, "mood_tint")->getValue().asString() != "neutral");
    for (const auto& pr : { std::make_pair("ray_nat", "ray_color"), std::make_pair("mie_nat", "mie_color"),
                            std::make_pair("ozone_nat", "ozone_color"), std::make_pair("star_nat", "star_color"),
                            std::make_pair("cloud_nat", "cloud_color"), std::make_pair("fog_nat", "fog_color"),
                            std::make_pair("sunlight_nat", "sunlight_color"), std::make_pair("ambient_nat", "ambient_color"),
                            std::make_pair("body_nat", "body_color") })
        getChildView(pr.second)->setEnabled(edit && !child<LLCheckBoxCtrl>(this, pr.first)->get());
    if (planet) getChildView("body_color")->setEnabled(edit);
    getChildView("body_lit")->setEnabled(edit && !child<LLCheckBoxCtrl>(this, "body_lit_auto")->get());
    getChildView("zone_mood_color")->setEnabled(edit && child<LLComboBox>(this, "zone_mood_tint")->getValue().asString() == "custom");
    // Remove only when there is something stored here; Preview whenever Wolf is on in the draft.
    getChildView("remove")->setEnabled(edit && scopeEntry().isMap());
    mPreviewBtn->setEnabled(mHave && canEdit() && (mPreviewOn || mMode->getSelectedIndex() != 2));
    mPreviewBtn->setToggleState(mPreviewOn);
    mPreviewBtn->setLabel(mPreviewOn ? std::string("Stop preview") : std::string("Preview"));
}

void WolfFloaterEnvironment::showNow()
{
    LLTextBox* now = child<LLTextBox>(this, "now");
    if (!mHave) { now->setText(LLStringUtil::null); return; }
    const LLSD& entry = scopeEntry();
    if (!entry.isMap()) { now->setText(LLStringUtil::null); return; }
    const LLSD& o = entry["obs"];
    std::string text = "Saved: " + std::string(entry["doc"]["mode"].asString() == "off" ? "off" : "on") + ".";
    if (o.isMap())
    {
        text += llformat(" Weather there now: %s, cloud %.0f%%, wind %.1f m/s.", o["condition"].asString().c_str(),
                         o["cloud"].asReal(), o["windSpeed"].asReal());
        if (o["stale"].asBoolean()) text += " (The weather service has not answered for hours.)";
    }
    else if (entry["doc"]["weather"]["source"].asString() == "live")
    {
        text += " Waiting for the first weather report for this place.";
    }
    now->setText(text);
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
// the controls -> the document
// ═══════════════════════════════════════════════════════════════════════════════════════════

void WolfFloaterEnvironment::readControls()
{
    LLSD& d = mDoc;
    d["format"] = "wolf-environment";
    d["version"] = LLSD::Integer(1);
    const S32 m = mMode->getSelectedIndex();
    d["mode"] = m == 1 ? "on-top" : m == 2 ? "off" : "wolf";
    d["preset"] = child<LLComboBox>(this, "preset")->getValue().asString();

    const std::string kind = child<LLComboBox>(this, "place_kind")->getValue().asString();
    LLSD place;
    place["kind"] = kind;
    if (kind != "none")
    {
        place["name"] = child<LLLineEditor>(this, "place_name")->getText();
        place["lat"] = LLSD::Real(child<LLSpinCtrl>(this, "place_lat")->getValueF32());
        if (kind == "real") place["lon"] = LLSD::Real(child<LLSpinCtrl>(this, "place_lon")->getValueF32());
    }
    d["place"] = place;

    const std::string tmode = child<LLComboBox>(this, "time_mode")->getValue().asString();
    LLSD time;
    time["mode"] = tmode;
    if (tmode == "zone") time["zone"] = child<LLComboBox>(this, "time_zone")->getValue().asString();
    if (tmode == "fixed") time["hour"] = LLSD::Real(child<LLSliderCtrl>(this, "time_hour")->getValueF32());
    if (tmode == "cycle") time["dayHours"] = LLSD::Real(child<LLSpinCtrl>(this, "time_day_hours")->getValueF32());
    d["time"] = time;

    const std::string src = child<LLComboBox>(this, "weather_source")->getValue().asString();
    LLSD w;
    w["source"] = src;
    if (src == "pattern") w["pattern"] = child<LLComboBox>(this, "weather_pattern")->getValue().asString();
    if (src == "fixed") w["type"] = child<LLComboBox>(this, "weather_type")->getValue().asString();
    d["weather"] = w;

    d["clouds"] = child<LLCheckBoxCtrl>(this, "clouds_auto")->get() ? LLSD() : as_int(child<LLSliderCtrl>(this, "clouds")->getValueF32());
    d["haze"] = as_int(child<LLSliderCtrl>(this, "haze")->getValueF32());
    d["water"]["look"] = child<LLComboBox>(this, "water")->getValue().asString();
    d["mood"]["tint"] = child<LLComboBox>(this, "mood_tint")->getValue().asString();
    d["mood"]["color"] = color_to_hex(child<LLColorSwatchCtrl>(this, "mood_color")->get());
    d["mood"]["strength"] = as_int(child<LLSliderCtrl>(this, "mood_strength")->getValueF32());

    LLSD& s = d["sky"];
    auto colour = [this](const char* chk, const char* sw) -> LLSD
    {
        return child<LLCheckBoxCtrl>(this, chk)->get() ? LLSD() : LLSD(color_to_hex(child<LLColorSwatchCtrl>(this, sw)->get()));
    };
    LLSD& a = s["atmosphere"];
    a["rayleigh"] = colour("ray_nat", "ray_color");
    a["rayleighDensity"] = as_int(child<LLSliderCtrl>(this, "ray_density")->getValueF32());
    a["mie"] = colour("mie_nat", "mie_color");
    a["mieDensity"] = as_int(child<LLSliderCtrl>(this, "mie_density")->getValueF32());
    a["ozone"] = colour("ozone_nat", "ozone_color");
    a["ozoneDensity"] = as_int(child<LLSliderCtrl>(this, "ozone_density")->getValueF32());
    a["mieAnisotropy"] = as_int(child<LLSliderCtrl>(this, "mie_glow")->getValueF32());
    a["planetRadiusKm"] = as_int(child<LLSpinCtrl>(this, "planet_km")->getValueF32());
    a["atmosphereKm"] = as_int(child<LLSpinCtrl>(this, "atmos_km")->getValueF32());
    LLSD& n = s["night"];
    n["stars"] = as_int(child<LLSliderCtrl>(this, "stars")->getValueF32());
    n["starColor"] = colour("star_nat", "star_color");
    n["nebula"] = as_int(child<LLSliderCtrl>(this, "nebula")->getValueF32());
    n["nebulaColor"] = color_to_hex(child<LLColorSwatchCtrl>(this, "nebula_color")->get());
    if (!n["planets"].isArray()) n["planets"] = LLSD::emptyArray();
    LLSD& c = s["clouds"];
    c["color"] = colour("cloud_nat", "cloud_color");
    c["emission"] = as_int(child<LLSliderCtrl>(this, "cloud_glow")->getValueF32());
    c["style"] = child<LLComboBox>(this, "cloud_style")->getValue().asString();
    const LLUUID ct = child<LLTextureCtrl>(this, "cloud_texture")->getImageAssetID();
    c["texture"] = ct.isNull() ? LLSD() : LLSD(ct.asString());
    s["fog"]["color"] = colour("fog_nat", "fog_color");
    s["fog"]["glow"] = as_int(child<LLSliderCtrl>(this, "fog_glow")->getValueF32());
    s["light"]["sun"] = colour("sunlight_nat", "sunlight_color");
    s["light"]["ambient"] = colour("ambient_nat", "ambient_color");
    s["light"]["strength"] = as_int(child<LLSliderCtrl>(this, "light_strength")->getValueF32());
    if (!s["suns"].isArray()) s["suns"] = LLSD::emptyArray();
    if (!s["moons"].isArray()) s["moons"] = LLSD::emptyArray();
    readBody();
    readZone();
    if (!d["zones"].isArray()) d["zones"] = LLSD::emptyArray();
}

void WolfFloaterEnvironment::readBody()
{
    if (mBodyIndex < 0 || mBodyIndex >= (S32)mBodyRows.size()) return;
    const std::string kind = mBodyRows[mBodyIndex][0].asString();
    LLSD& b = bodyList(kind)[mBodyRows[mBodyIndex][1].asInteger()];
    const bool planet = kind == "planets";
    b["color"] = (!planet && child<LLCheckBoxCtrl>(this, "body_nat")->get()) ? LLSD()
               : LLSD(color_to_hex(child<LLColorSwatchCtrl>(this, "body_color")->get()));
    b["size"] = as_int(child<LLSliderCtrl>(this, "body_size")->getValueF32());
    if (planet)
    {
        b["azimuth"] = LLSD::Real(child<LLSpinCtrl>(this, "body_az")->getValueF32());
        b["elevation"] = LLSD::Real(child<LLSpinCtrl>(this, "body_el")->getValueF32());
        b["rings"] = (bool)child<LLCheckBoxCtrl>(this, "body_rings")->get();
        b["ringColor"] = color_to_hex(child<LLColorSwatchCtrl>(this, "body_ring_color")->get());
        return;
    }
    b["brightness"] = as_int(child<LLSliderCtrl>(this, "body_brightness")->getValueF32());
    const LLUUID tex = child<LLTextureCtrl>(this, "body_texture")->getImageAssetID();
    b["texture"] = tex.isNull() ? LLSD() : LLSD(tex.asString());
    const std::string pmode = child<LLComboBox>(this, "body_position")->getValue().asString();
    LLSD pos;
    pos["mode"] = pmode;
    if (pmode == "fixed")
    {
        pos["azimuth"] = LLSD::Real(child<LLSpinCtrl>(this, "body_az")->getValueF32());
        pos["elevation"] = LLSD::Real(child<LLSpinCtrl>(this, "body_el")->getValueF32());
    }
    else if (pmode == "orbit")
    {
        pos["periodHours"] = LLSD::Real(child<LLSpinCtrl>(this, "body_period")->getValueF32());
        pos["tilt"] = LLSD::Real(child<LLSpinCtrl>(this, "body_tilt")->getValueF32());
        pos["phase"] = LLSD::Real(child<LLSpinCtrl>(this, "body_phase")->getValueF32());
    }
    b["position"] = pos;
    if (kind == "moons")
        b["phase"] = child<LLCheckBoxCtrl>(this, "body_lit_auto")->get() ? LLSD() : as_int(child<LLSliderCtrl>(this, "body_lit")->getValueF32());
}

void WolfFloaterEnvironment::readZone()
{
    if (mZoneIndex < 0 || mZoneIndex >= (S32)mDoc["zones"].size()) return;
    LLSD& z = mDoc["zones"][mZoneIndex];
    z["name"] = child<LLLineEditor>(this, "zone_name")->getText();
    const std::string type = child<LLComboBox>(this, "zone_shape")->getValue().asString();
    LLSD sh;
    sh["type"] = type;
    const F32 x = child<LLSpinCtrl>(this, "zone_x")->getValueF32(), y = child<LLSpinCtrl>(this, "zone_y")->getValueF32();
    if (type == "circle") { sh["x"] = LLSD::Real(x); sh["y"] = LLSD::Real(y); sh["radius"] = LLSD::Real(child<LLSpinCtrl>(this, "zone_radius")->getValueF32()); }
    else if (type == "rect")
    {
        sh["x1"] = LLSD::Real(x); sh["y1"] = LLSD::Real(y);
        sh["x2"] = LLSD::Real(child<LLSpinCtrl>(this, "zone_x2")->getValueF32());
        sh["y2"] = LLSD::Real(child<LLSpinCtrl>(this, "zone_y2")->getValueF32());
    }
    else sh["altitude"] = as_int(child<LLSpinCtrl>(this, "zone_alt")->getValueF32());
    z["shape"] = sh;
    z["edge"] = LLSD::Real(child<LLSpinCtrl>(this, "zone_edge")->getValueF32());
    LLSD set = LLSD::emptyMap();
    if (child<LLCheckBoxCtrl>(this, "zone_weather_on")->get())
    {
        LLSD zw;
        zw["source"] = "fixed";
        zw["type"] = child<LLComboBox>(this, "zone_weather")->getValue().asString();
        set["weather"] = zw;
    }
    if (child<LLCheckBoxCtrl>(this, "zone_clouds_on")->get()) set["clouds"] = as_int(child<LLSliderCtrl>(this, "zone_clouds")->getValueF32());
    if (child<LLCheckBoxCtrl>(this, "zone_haze_on")->get()) set["haze"] = as_int(child<LLSliderCtrl>(this, "zone_haze")->getValueF32());
    if (child<LLCheckBoxCtrl>(this, "zone_fog_on")->get()) set["fogColor"] = color_to_hex(child<LLColorSwatchCtrl>(this, "zone_fog")->get());
    if (child<LLCheckBoxCtrl>(this, "zone_fogglow_on")->get()) set["fogGlow"] = as_int(child<LLSliderCtrl>(this, "zone_fog_glow")->getValueF32());
    if (child<LLCheckBoxCtrl>(this, "zone_light_on")->get()) set["light"] = color_to_hex(child<LLColorSwatchCtrl>(this, "zone_light")->get());
    if (child<LLCheckBoxCtrl>(this, "zone_lightstr_on")->get()) set["lightStrength"] = as_int(child<LLSliderCtrl>(this, "zone_light_strength")->getValueF32());
    if (child<LLCheckBoxCtrl>(this, "zone_mood_on")->get())
    {
        LLSD m;
        m["tint"] = child<LLComboBox>(this, "zone_mood_tint")->getValue().asString();
        m["color"] = color_to_hex(child<LLColorSwatchCtrl>(this, "zone_mood_color")->get());
        m["strength"] = as_int(child<LLSliderCtrl>(this, "zone_mood_strength")->getValueF32());
        set["mood"] = m;
    }
    z["set"] = set;
}

void WolfFloaterEnvironment::onChanged()
{
    if (mWriting || !mHave) return;
    readControls();
    // A change to the sky block makes the look "custom" (the presets are only starting points).
    mDirty = true;
    fillBodies();
    fillZones();
    updateVisibility();
    setStatus(getString("str_unsaved"), false);
    if (mPreviewOn)
    {
        // a live preview follows the edit, at most as often as the region takes previews
        mPreviewPending = true;
        mPreviewDue = LLFrameTimer::getElapsedSeconds() + PREVIEW_DEBOUNCE;
    }
}

void WolfFloaterEnvironment::onPresetPicked()
{
    if (mWriting || !mHave) return;
    const std::string preset = child<LLComboBox>(this, "preset")->getValue().asString();
    if (preset != "custom" && mReply["presets"].has(preset))
    {
        readControls();
        mDoc["sky"] = mReply["presets"][preset];
        mDoc["preset"] = preset;
        mBodyIndex = -1;
        writeControls();
    }
    onChanged();
}

void WolfFloaterEnvironment::onRevert()
{
    mDirty = false;
    child<LLScrollListCtrl>(this, "place_results")->deleteAllItems();
    applyReply(mReply);
    if (mPreviewOn) stopPreview();
}

void WolfFloaterEnvironment::onRemoveAsked()
{
    if (!canEdit() || !scopeEntry().isMap()) return;
    // Removing deletes the stored settings: an alert dialog the owner must answer (not a toast).
    LLSD args;
    LLStringUtil::format_map_t map;
    map["[WHAT]"] = mScope == "parcel" ? std::string("this parcel") : std::string("this region");
    std::string msg = getString("str_remove_ask");
    LLStringUtil::format(msg, map);
    args["MESSAGE"] = msg;
    const LLHandle<LLFloater> h = getHandle();
    LLNotificationsUtil::add("GenericAlertYesCancel", args, LLSD(), [h](const LLSD& n, const LLSD& r)
    {
        if (LLNotificationsUtil::getSelectedOption(n, r) != 0) return;
        if (WolfFloaterEnvironment* self = static_cast<WolfFloaterEnvironment*>(h.get())) self->post("remove");
    });
}

void WolfFloaterEnvironment::onSearch()
{
    if (!canEdit() || mBusy) return;   // never over a read in flight (it would be dropped as stale)
    const std::string q = child<LLLineEditor>(this, "place_search")->getText();
    if (q.size() < 2) { setStatus(getString("str_search_short"), true); return; }
    LLSD body;
    body["action"] = "search";
    body["q"] = q;
    setStatus(getString("str_searching"), false);
    const LLHandle<LLFloater> h = getHandle();
    const std::string text = llsd_to_json(body);
    const U64 serial = ++mSerial;
    mBusy = true;
    LLCoros::instance().launch("WolfEnvSearch", [h, text, serial]() { postCoro(h, AUTO_ENV_URL, text, "search", serial); });
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
// writing
// ═══════════════════════════════════════════════════════════════════════════════════════════

void WolfFloaterEnvironment::post(const std::string& action)
{
    if (!canEdit()) { setStatus(getString(mScope == "parcel" ? "str_cannot_parcel" : "str_cannot_region"), true); return; }
    if (mBusy) return;
    readControls();
    LLSD body;
    body["action"] = action;
    body["region"] = regionId();
    body["scope"] = mScope;
    body["x"] = LLSD::Real(mAtX);
    body["y"] = LLSD::Real(mAtY);
    body["version"] = LLSD::Integer(mVersion);
    if (action != "remove") body["doc"] = mDoc;
    mBusy = true;
    if (action == "save") setStatus(getString("str_saving"), false);
    updateVisibility();
    const LLHandle<LLFloater> h = getHandle();
    const std::string text = llsd_to_json(body);
    const U64 serial = ++mSerial;
    LLCoros::instance().launch("WolfEnvPost", [h, text, action, serial]() { postCoro(h, API_URL, text, action, serial); });
}

// static
void WolfFloaterEnvironment::postCoro(LLHandle<LLFloater> handle, std::string url, std::string body, std::string kind, U64 serial)
{
    auto adapter = std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfEnvironment", LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
    LLCore::HttpOptions::ptr_t options = WolfGrid::makeVerifiedHttpOptions();
    options->setTimeout(40);
    // Never retried by the HTTP layer: a slow first answer must not send a save twice (the auto
    // environment's "Conflict" of 2026-10-03, wolfautoenvironment.cpp postCoro).
    options->setRetries(0);
    LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
    headers->append(HTTP_OUT_HEADER_CONTENT_TYPE, "application/json");
    headers->append(HTTP_OUT_HEADER_ACCEPT, "application/json");
    headers->append("X-Wolf-Agent", gAgentID.asString());
    headers->append("X-Wolf-Session", gAgentSessionID.asString());
    LLCore::BufferArray::ptr_t raw(new LLCore::BufferArray());
    raw->append(body.data(), body.size());
    LLSD result = adapter->postRawAndSuspend(request, url, raw, options, headers);
    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
    LLSD reply = raw_reply(result);
    const bool ok = status && reply.isMap() && reply["success"].asBoolean();
    const std::string why = reply.isMap() && reply.has("error") ? reply["error"].asString()
                          : !status ? status.toString() : std::string("the service returned an unreadable reply");
    WolfFloaterEnvironment* self = static_cast<WolfFloaterEnvironment*>(handle.get());
    if (!self)
    {
        // A save the owner asked for must not fail silently, even with the window closed.
        if (!ok && (kind == "save" || kind == "remove"))
            LLNotificationsUtil::add("GenericAlertOK", LLSD().with("MESSAGE", "Could not save the Wolf Environment: " + why));
        return;
    }
    if (serial != self->mSerial && kind != "save" && kind != "remove") { self->mBusy = false; return; }
    // 409: changed elsewhere since it was read (wolf_environment.php's version check)
    const bool conflict = !ok && status == LLCore::HttpStatus(HTTP_CONFLICT);
    self->onPostReply(kind, reply, ok, conflict, why);
}

void WolfFloaterEnvironment::onPostReply(const std::string& kind, const LLSD& reply, bool ok, bool conflict, const std::string& why)
{
    mBusy = false;
    if (kind == "search")
    {
        if (!ok) { setStatus(getString("str_search_failed") + " " + why, true); updateVisibility(); return; }
        mResults = reply["results"];
        LLScrollListCtrl* list = child<LLScrollListCtrl>(this, "place_results");
        list->deleteAllItems();
        for (S32 i = 0; i < (S32)mResults.size(); ++i)
            list->addSimpleElement(llformat("%s  (%.4f, %.4f)", mResults[i]["name"].asString().c_str(),
                                            mResults[i]["lat"].asReal(), mResults[i]["lon"].asReal()), ADD_BOTTOM, LLSD(i));
        setStatus(getString(mResults.size() ? "str_pick" : "str_none_found"), mResults.size() == 0);
        updateVisibility();
        return;
    }
    if (kind == "check")
    {
        if (!ok)
        {
            mPreviewOn = false;
            if (reply.has("errors")) showErrors(reply["errors"]);
            else setStatus(getString("str_preview_failed") + " " + why, true);
            updateVisibility();
            return;
        }
        LLViewerRegion* region = gAgent.getRegion();
        const std::string cap = region ? region->getCapability(PREVIEW_CAP) : std::string();
        if (cap.empty())
        {
            mPreviewOn = false;
            setStatus(getString("str_preview_no_cap"), true);
            updateVisibility();
            return;
        }
        LLSD body;
        body["entry"] = reply["entry"];
        mPreviewEntry = reply["entry"];
        const std::string text = llsd_to_json(body);
        const LLHandle<LLFloater> h = getHandle();
        const U64 serial = ++mPreviewSerial;
        mPreviewBusy = true;   // its own flag: a stopped preview must never leave the window busy
        LLCoros::instance().launch("WolfEnvPreview", [h, cap, text, serial]() { previewCoro(h, cap, text, serial); });
        return;
    }
    // save / remove
    if (!ok)
    {
        if (conflict)
        {
            // Saved from elsewhere since this was read: reload it, keep the edit, say so.
            const LLSD keep = mDoc;
            setStatus(getString("str_conflict"), true);
            mDirty = true;
            mDoc = keep;
            fetch();
            return;
        }
        if (reply.has("errors")) showErrors(reply["errors"]);
        else setStatus(getString("str_save_failed") + " " + why, true);
        updateVisibility();
        return;
    }
    mDirty = false;
    if (mPreviewOn) stopPreview();
    applyReply(reply);
    setStatus(getString(kind == "remove" ? "str_removed" : "str_saved"), false);
    // The weather tab and the moon read `autoEnv` from weather.php: ask again now.
    WolfRegionWeather::instance().refresh(true);
    // [WOLF ENV 2026-10-12 fix] and the renderer's own copy of the document (wolfenvironmentstate.cpp),
    // which otherwise waits up to REFETCH_SECS (120 s) before the native sky follows the save
    WolfEnvironmentState::instance().refresh();
}

/** Every problem the web found, listed, and the first one's control focused on its tab. */
void WolfFloaterEnvironment::showErrors(const LLSD& errors)
{
    std::string text = getString("str_fix");
    std::string firstField;
    for (const LLSD& e : llsd::inArray(errors))
    {
        text += "\n- " + e["message"].asString();
        if (firstField.empty()) firstField = e["field"].asString();
    }
    setStatus(text, true);
    for (const FieldCtrl& fc : FIELD_CTRLS)
    {
        if (firstField.compare(0, strlen(fc.prefix), fc.prefix) != 0) continue;
        mTabs->selectTabByName(fc.tab);
        if (LLUICtrl* c = findChild<LLUICtrl>(fc.ctrl)) c->setFocus(true);
        break;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
// the preview: the draft, baked by the region, on this screen only
// ═══════════════════════════════════════════════════════════════════════════════════════════

void WolfFloaterEnvironment::startPreview()
{
    mPreviewPending = false;
    if (!canEdit() || mBusy || mPreviewBusy)
    {
        if (mPreviewOn) mPreviewPending = true;   // try again once the window is free
        return;
    }
    // The auto environment's own time preview uses ENV_LOCAL too: one preview at a time.
    if (WolfRegionWeather::instance().autoEnvPreviewOn()) WolfRegionWeather::instance().setAutoEnvPreview(false, 0.0);
    mPreviewOn = true;
    post("check");
}

// static
void WolfFloaterEnvironment::previewCoro(LLHandle<LLFloater> handle, std::string cap, std::string body, U64 serial)
{
    auto adapter = std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfEnvironment", LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
    LLCore::HttpOptions::ptr_t options = std::make_shared<LLCore::HttpOptions>();
    options->setTimeout(30);
    options->setRetries(0);
    LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
    headers->append(HTTP_OUT_HEADER_CONTENT_TYPE, "application/json");
    headers->append(HTTP_OUT_HEADER_ACCEPT, "application/llsd+xml");
    LLCore::BufferArray::ptr_t raw(new LLCore::BufferArray());
    raw->append(body.data(), body.size());
    LLSD result = adapter->postRawAndSuspend(request, cap, raw, options, headers);
    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
    LLSD reply;
    if (result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
    {
        const LLSD::Binary& bytes = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
        std::istringstream in(std::string(bytes.begin(), bytes.end()));
        LLSDSerialize::fromXML(reply, in);
    }
    WolfFloaterEnvironment* self = static_cast<WolfFloaterEnvironment*>(handle.get());
    if (!self) return;
    if (serial == self->mPreviewSerial) self->mPreviewBusy = false;
    if (serial != self->mPreviewSerial || !self->mPreviewOn) { self->updateVisibility(); return; }   // stopped (or redone) while it baked
    if (!status || !reply.isMap() || !reply["success"].asBoolean())
    {
        self->mPreviewOn = false;
        self->setStatus(self->getString("str_preview_failed") + " " +
                        (reply.isMap() && reply.has("message") ? reply["message"].asString() : status.toString()), true);
        self->updateVisibility();
        return;
    }
    // Source: llenvironment.cpp EnvironmentInfo::extract (:2416-2440) — the same reading of an
    // environment message the viewer uses for the region's own (buildFromEnvironmentMessage, day
    // length, day offset), then shown the way the auto environment's preview is
    // (wolfregionweather.cpp setAutoEnvPreview: ENV_LOCAL, selected instantly).
    const LLSD& env = reply["environment"];
    LLSettingsDay::ptr_t day = LLSettingsVODay::buildFromEnvironmentMessage(env["day_cycle"]);
    if (!day)
    {
        self->mPreviewOn = false;
        self->setStatus(self->getString("str_preview_failed") + " the viewer could not read the baked sky", true);
        self->updateVisibility();
        return;
    }
    LLEnvironment& e = LLEnvironment::instance();
    self->mPreviewDay = day;
    self->mPreviewDayLength = env["day_length"].asInteger();
    self->mPreviewDayOffset = env["day_offset"].asInteger();
    // [WOLF ENV 2026-10-12] the draft at the time slider's moment (0 ahead when it is off)
    e.setEnvironment(LLEnvironment::ENV_LOCAL, day, LLSettingsDay::Seconds(self->mPreviewDayLength),
                     LLSettingsDay::Seconds(self->mPreviewDayOffset + (S32)ll_round((F32)self->timeAhead())));
    e.setSelectedEnvironment(LLEnvironment::ENV_LOCAL, LLEnvironment::TRANSITION_FAST);
    WolfEnvironmentState::instance().setTimeAhead(self->timeAhead());
    // The native renderer (phases 2-4) draws the same draft while it is previewed.
    WolfEnvironmentState::instance().setPreviewEntry(self->mPreviewEntry);
    self->setStatus(self->getString("str_previewing"), false);
    self->updateVisibility();
}

void WolfFloaterEnvironment::stopPreview()
{
    mPreviewOn = false;
    mPreviewPending = false;
    mPreviewBusy = false;   // any answer still on its way is now stale (serial) and ignored
    ++mPreviewSerial;
    if (WolfEnvironmentState::instanceExists())
        WolfEnvironmentState::instance().setPreviewEntry(LLSD());   // the native renderer stops drawing the draft
    // Source: llviewermenu.cpp:828-829 (as setAutoEnvPreview): clearing ENV_LOCAL and selecting it
    // returns to the shared environment.
    LLEnvironment& e = LLEnvironment::instance();
    e.clearEnvironment(LLEnvironment::ENV_LOCAL);
    e.setSelectedEnvironment(LLEnvironment::ENV_LOCAL, LLEnvironment::TRANSITION_INSTANT);
    if (mStatus && mStatus->getText() == getString("str_previewing")) setStatus(LLStringUtil::null, false);
    mPreviewDay.reset();
    // [WOLF ENV 2026-10-12] the time slider stays: the shared sky, still that far ahead
    if (timeOn()) applyTime();
    if (mPreviewBtn) updateVisibility();
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
// [WOLF ENV 2026-10-12] the time test slider. Paul: "can you add a test slider like on Jimmys eep".
// Source: wolfautoenvironment.cpp onPreviewChanged / showPreviewTime / onVisibilityChange and
// panel_region_auto_environment.xml ae_preview (0 .. 23.75 h in quarter hours): the sky's own baked
// cycle run ahead on ENV_LOCAL (WolfRegionWeather::setAutoEnvPreview), this viewer only, nothing saved,
// ended by closing the window or a region change. While the draft is previewed it is the DRAFT's cycle
// that runs ahead (both are an ENV_LOCAL day: one at a time, so the slider moves whichever is shown).
// The native renderer follows: it takes the sun and moon from that cycle, and its own clock-placed
// bodies from WolfEnvironmentState::timeAhead (wolfenvironmentrender.cpp skyNow).
// ═══════════════════════════════════════════════════════════════════════════════════════════

bool WolfFloaterEnvironment::timeOn() const
{
    return mTimeOn && mTimeOn->getValue().asBoolean();
}

F64 WolfFloaterEnvironment::timeAhead() const
{
    return timeOn() && mTime ? (F64)mTime->getValueF32() * 3600.0 : 0.0;
}

void WolfFloaterEnvironment::applyTime()
{
    const bool on = timeOn();
    mTime->setEnabled(on);
    const F64 ahead = timeAhead();
    LLEnvironment& e = LLEnvironment::instance();
    if (mPreviewOn && mPreviewDay)
    {
        e.setEnvironment(LLEnvironment::ENV_LOCAL, mPreviewDay, LLSettingsDay::Seconds(mPreviewDayLength),
                         LLSettingsDay::Seconds(mPreviewDayOffset + (S32)ll_round((F32)ahead)));
        e.setSelectedEnvironment(LLEnvironment::ENV_LOCAL, LLEnvironment::TRANSITION_INSTANT);
        WolfEnvironmentState::instance().setTimeAhead(ahead);
    }
    else if (on)
    {
        WolfRegionWeather::instance().setAutoEnvPreview(true, ahead, true);
        mTimeOwned = WolfRegionWeather::instance().autoEnvPreviewOn();
        WolfEnvironmentState::instance().setTimeAhead(mTimeOwned ? ahead : 0.0);
        if (!mTimeOwned)
        {
            // no baked cycle here yet (the region's environment has not arrived): nothing to run ahead
            mTimeOn->setValue(false);
            mTime->setEnabled(false);
        }
    }
    else
    {
        endTime();
    }
    showTime();
}

void WolfFloaterEnvironment::endTime()
{
    if (mTime) mTime->setEnabled(false);
    if (WolfEnvironmentState::instanceExists()) WolfEnvironmentState::instance().setTimeAhead(0.0);
    if (mPreviewOn && mPreviewDay)
    {
        // the draft goes back to now
        LLEnvironment& e = LLEnvironment::instance();
        e.setEnvironment(LLEnvironment::ENV_LOCAL, mPreviewDay, LLSettingsDay::Seconds(mPreviewDayLength),
                         LLSettingsDay::Seconds(mPreviewDayOffset));
        e.setSelectedEnvironment(LLEnvironment::ENV_LOCAL, LLEnvironment::TRANSITION_INSTANT);
    }
    else if (mTimeOwned && WolfRegionWeather::instance().autoEnvPreviewOn())
    {
        // only the preview this window started (never Jimmy's tab's own)
        WolfRegionWeather::instance().setAutoEnvPreview(false, 0.0);
    }
    mTimeOwned = false;
    showTime();
}

/** As showPreviewTime: "Showing Sat 23:00, Grid time (13 h 30 min ahead) - only on your screen." in the
 *  sky's own clock (auto_environment.php clockUtcOffset now / clockUtcOffset12h 12 h on, clockLabel). */
void WolfFloaterEnvironment::showTime()
{
    if (!mTimeText) return;
    const std::string mode = mDoc.isMap() ? mDoc["time"]["mode"].asString() : std::string();
    if (mode == "fixed")
    {
        mTimeText->setText(std::string("This sky is always the same time: the slider changes nothing."));
        return;
    }
    if (!mClockCfg.isMap())
    {
        mTimeText->setText(timeOn() ? llformat("Showing %.2f h ahead - only on your screen.", timeAhead() / 3600.0) : std::string());
        return;
    }
    const F64 ahead = timeAhead();
    const bool has_clock = mClockCfg.has("clockUtcOffset");
    const S32 offset = (ahead < 43200.0 ? mClockCfg[has_clock ? "clockUtcOffset" : "gridUtcOffset"]
                                        : mClockCfg[has_clock ? "clockUtcOffset12h" : "gridUtcOffset12h"]).asInteger();
    const std::string label = mClockCfg.has("clockLabel") ? mClockCfg["clockLabel"].asString() : std::string("Grid time");
    const time_t local = (time_t)(LLDate::now().secondsSinceEpoch() + ahead + offset);
    struct tm parts;
#if LL_WINDOWS
    gmtime_s(&parts, &local);
#else
    gmtime_r(&local, &parts);
#endif
    char clock[32];
    strftime(clock, sizeof(clock), "%a %H:%M", &parts);
    if (timeOn())
    {
        const S32 mins = (S32)(ahead / 60.0 + 0.5);
        std::string text = llformat("Showing %s, %s (%d h %02d min ahead) - only on your screen.", clock, label.c_str(), mins / 60, mins % 60);
        if (mode == "cycle") text += " (A faster day: this is real time ahead.)";
        if (mPreviewOn) text += " Your draft.";
        mTimeText->setText(text);
    }
    else
    {
        mTimeText->setText(llformat("%s now: %s.", label.c_str(), clock));
    }
}

// static
void WolfFloaterEnvironment::clockCoro(LLHandle<LLFloater> handle, std::string url, std::string region_id)
{
    auto adapter = std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfEnvironment", LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
    LLCore::HttpOptions::ptr_t options = WolfGrid::makeVerifiedHttpOptions();
    options->setTimeout(30);
    LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
    headers->append(HTTP_OUT_HEADER_ACCEPT, "application/json");
    LLSD result = adapter->getRawAndSuspend(request, url, options, headers);
    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
    LLSD reply = raw_reply(result);
    WolfFloaterEnvironment* self = static_cast<WolfFloaterEnvironment*>(handle.get());
    if (!self || region_id != self->regionId()) return;
    // A failure only leaves the readout without a clock (it then says how far ahead): logged, not shown.
    if (!status || !reply.isMap() || !reply["success"].asBoolean() || !reply["config"].isMap())
    {
        LL_WARNS("WolfEnv") << "the sky clock for the time slider was not read: " << status.toString() << LL_ENDL;
        return;
    }
    self->mClockCfg = reply["config"];
    self->mClockFor = region_id;
    self->showTime();
}
