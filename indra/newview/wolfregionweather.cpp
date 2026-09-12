/**
 * @file wolfregionweather.cpp
 * @brief WolfViewer: the region's weather — About Land > Weather.
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

void WolfRegionWeather::idle()
{
    if (!WolfGrid::isWolfTerritories()) return;
    const std::string id = currentRegionId();
    if (id.empty()) return;
    // A region change re-asks at once; otherwise the poll clock decides.
    if (id != mFetchedFor) { refresh(true); return; }
    if (LLFrameTimer::getElapsedSeconds() >= mNextPoll) refresh(false);
}

void WolfRegionWeather::refresh(bool force)
{
    if (mFetching) return;
    if (!WolfGrid::isWolfTerritories()) return;
    const std::string id = currentRegionId();
    if (id.empty()) return;
    if (!force && id == mFetchedFor && LLFrameTimer::getElapsedSeconds() < mNextPoll) return;
    mFetching = true;
    mNextPoll = (F32)LLFrameTimer::getElapsedSeconds() + POLL_SECS;
    LLCoros::instance().launch("WolfRegionWeather", [id]() { WolfRegionWeather::fetchCoro(id); });
}

// static
void WolfRegionWeather::fetchCoro(std::string region_id)
{
    const std::string url = std::string(API_URL) + "?region=" + region_id;
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
    self.mFetching = false;
    self.mFetchedFor = region_id;
    if (!status)
    {
        // Off grid, offline, or the service having a moment. Not worth a dialog: the user did not
        // ask for this and the region simply has no weather as far as the viewer is concerned.
        self.mLastError = status.toString();
        return;
    }
    LLSD reply;
    if (result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
    {
        reply = json_to_llsd(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary());
    }
    if (!reply.isMap() || !reply["success"].asBoolean() || !reply["regions"].isArray()) return;

    const LLSD& regions = reply["regions"];
    if (regions.size() == 0) return;
    const LLSD& r = regions[0];
    self.mHaveRow    = true;
    self.mRegionName = r["name"].asString();
    self.mVersion    = r["version"].asInteger();
    // A null kind is "the region has no opinion" — deliberately not turned into "clear". The
    // rest of the profile (the look and the sound) is always complete, because those are the
    // estate owner's art direction and apply even when the region is not forcing the weather.
    self.mKind       = (!r.has("kind") || r["kind"].isUndefined()) ? std::string() : r["kind"].asString();
    self.mProfile    = WolfWeatherProfile();
    self.mProfile.fromLLSD(r);
    self.mLastError.clear();
    self.mGeneration++;
    LL_INFOS("WolfWeather") << "region weather: " << (self.mKind.empty() ? "unset" : self.mKind)
                            << " level " << self.mProfile.mLevel
                            << (self.mProfile.mEnabled ? " (on)" : " (off)")
                            << " brightness " << self.mProfile.mBrightness
                            << "% density " << self.mProfile.mDensity
                            << "% sound " << (self.mProfile.mSound ? self.mProfile.mSoundPreset : "off")
                            << LL_ENDL;
    self.applyToWeather();
}

void WolfRegionWeather::applyPush(const LLSD& row)
{
    if (!row.isMap()) return;
    mHaveRow = true;
    mKind = (!row.has("kind") || row["kind"].isUndefined()) ? std::string() : row["kind"].asString();
    if (row.has("version")) mVersion = row["version"].asInteger();
    mProfile = WolfWeatherProfile();
    mProfile.fromLLSD(row);
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

void WolfRegionWeather::save(const WolfWeatherProfile& profile)
{
    if (mSaving) return;
    const std::string id = currentRegionId();
    if (id.empty()) return;
    mSaving = true;
    mLastError.clear();

    WolfWeatherProfile p = profile;
    p.clampAll();
    LLSD body = p.toLLSD();
    body["region"]  = id;
    body["version"] = mVersion;
    const std::string text = boost::json::serialize(LlsdToJson(body));
    LLCoros::instance().launch("WolfRegionWeatherSave", [text]() { WolfRegionWeather::saveCoro(text); });
}

// static
void WolfRegionWeather::saveCoro(std::string body)
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
    self.mSaving = false;
    if (!status || !reply.isMap() || !reply["success"].asBoolean())
    {
        self.mLastError = reply.isMap() && reply.has("error") ? reply["error"].asString()
                                                              : status.toString();
        self.mGeneration++;
        return;
    }
    // The POST answers with the WHOLE stored row, so believe that rather than our own idea of
    // what we sent — and rather than waiting for the next poll to show it.
    self.mHaveRow = true;
    self.mVersion = reply["version"].asInteger();
    self.mKind    = (!reply.has("kind") || reply["kind"].isUndefined()) ? std::string()
                                                                        : reply["kind"].asString();
    self.mProfile = WolfWeatherProfile();
    self.mProfile.fromLLSD(reply);
    self.mLastError.clear();
    self.mGeneration++;
    self.applyToWeather();
}


// ═══════════════════════════════════════════════════════════════════════════════════════════
// About Land > Weather — the panel
// ═══════════════════════════════════════════════════════════════════════════════════════════

WolfPanelLandWeather::WolfPanelLandWeather(LLParcelSelectionHandle& /*parcel*/)
{
}

WolfPanelLandWeather::~WolfPanelLandWeather()
{
    // An unsaved edit is previewed on THIS viewer's own sky. It must not outlive the panel:
    // weather that followed you out of the window would be indistinguishable from saved
    // weather, and there would be no way to get rid of it.
    WolfWeather::instance().setPreview(nullptr);
}

bool WolfPanelLandWeather::postBuild()
{
    mPreset         = getChild<LLComboBox>("weather_preset");
    mKind           = getChild<LLComboBox>("weather_kind");
    mEnabled        = getChild<LLCheckBoxCtrl>("weather_enabled");
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

    mEdit = WolfRegionWeather::instance().stored();
    mStored = mEdit;
    writeControls();
    return true;
}

/** Region-level rights, the same rule php/weather.php enforces (wt_holds_region). */
bool WolfPanelLandWeather::canEdit() const
{
    LLViewerRegion* region = gAgent.getRegion();
    return region && region->canManageEstate();
}

void WolfPanelLandWeather::setStatus(const std::string& msg, bool error)
{
    if (!mStatus) return;
    mStatus->setText(msg);
    // A failure must be readable; a confirmation may be quiet.
    mStatus->setColor(error ? LLColor4(0.88f, 0.33f, 0.28f, 1.f) : LLColor4(0.79f, 0.64f, 0.15f, 1.f));
}

// ── the controls ───────────────────────────────────────────────────────────────────────────

void WolfPanelLandWeather::readControls()
{
    mEdit.mKind       = WolfWeatherProfile::kindFromName(mKind->getValue().asString());
    mEdit.mLevel      = mLadder->getValue().asInteger();
    mEdit.mEnabled    = mEnabled->getValue().asBoolean();
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

void WolfPanelLandWeather::writeControls()
{
    // Setting a control fires its commit callback, which would read the half-written state back
    // out over the rest of the edit. One flag, checked in onControlChanged.
    mWriting = true;
    mKind->setValue(WolfWeatherProfile::kindName(mEdit.mKind));
    mLadder->setValue(mEdit.mLevel);
    mEnabled->setValue(mEdit.mEnabled);
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
        "weather_enabled", "weather_level", "weather_brightness", "weather_tint",
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

void WolfPanelLandWeather::updatePreview()
{
    WolfWeatherProfile p = mEdit;
    // A preview is always ON, whatever the "Apply Global Weather" box says: the box decides
    // what OTHER people get, and previewing nothing would make the slider look broken.
    p.mEnabled = true;
    WolfWeather::instance().setPreview(&p);
}

void WolfPanelLandWeather::onControlChanged()
{
    if (mWriting) return;
    readControls();
    updatePreview();
    setStatus(LLStringUtil::null, false);
}

void WolfPanelLandWeather::onKindChanged()
{
    if (mWriting) return;
    const WolfWeatherProfile::Kind was = mEdit.mKind;
    readControls();
    // The tint is chosen for one kind; switching kind takes that kind's neutral colour rather
    // than leaving blue snow behind.
    if (mEdit.mKind != was) mEdit.mTint = WolfWeatherProfile::neutralColor(mEdit.mKind);
    writeControls();
    updatePreview();
    setStatus(LLStringUtil::null, false);
}

// ── presets ────────────────────────────────────────────────────────────────────────────────

void WolfPanelLandWeather::onApplyPreset()
{
    const std::string id = mPreset->getValue().asString();
    if (id.empty()) return;
    mEdit = WolfWeatherProfile::applyPreset(mEdit, id);
    writeControls();
    updatePreview();
    setStatus(getString("str_preset_loaded"), false);
}

void WolfPanelLandWeather::onReset()
{
    // Reset is the DEFAULTS, keeping what falls; Revert is what puts the grid's row back. Two
    // buttons that did the same thing would be worse than useless.
    WolfWeatherProfile fresh;
    fresh.mKind    = mEdit.mKind;
    fresh.mLevel   = mEdit.mLevel;
    fresh.mEnabled = mEdit.mEnabled;
    fresh.mTint    = WolfWeatherProfile::neutralColor(fresh.mKind);
    mEdit = fresh;
    writeControls();
    updatePreview();
    setStatus(getString("str_reset"), false);
}

void WolfPanelLandWeather::onApply()
{
    readControls();
    WolfRegionWeather::instance().save(mEdit);
    mShownGeneration = WolfRegionWeather::instance().generation();
    setStatus(getString("str_saving"), false);
}

void WolfPanelLandWeather::onRevert()
{
    WolfWeather::instance().setPreview(nullptr);
    mEdit = WolfRegionWeather::instance().stored();
    mStored = mEdit;
    writeControls();
    setStatus(LLStringUtil::null, false);
}

// ── refresh ────────────────────────────────────────────────────────────────────────────────

void WolfPanelLandWeather::refresh()
{
    WolfRegionWeather::instance().refresh(false);
    draw();
}

void WolfPanelLandWeather::draw()
{
    WolfRegionWeather& rw = WolfRegionWeather::instance();
    if (rw.generation() != mShownGeneration)
    {
        mShownGeneration = rw.generation();

        if (!rw.lastError().empty())
        {
            setStatus(getString("str_save_failed") + " " + rw.lastError(), true);
        }
        else if (!rw.saving())
        {
            // The grid's row changed. Take it — but NEVER over an edit in progress: redrawing
            // under someone's hand as they drag a slider is worse than showing a stale value.
            const WolfWeatherProfile fresh = rw.stored();
            if (mEdit == mStored)
            {
                mEdit = fresh;
                mStored = fresh;
                writeControls();
                if (!WolfWeather::instance().activeSource().empty()
                    && WolfWeather::instance().activeSource() == "preview")
                {
                    updatePreview();
                }
            }
            else
            {
                mStored = fresh;
            }
            if (mStatus && mStatus->getText() == getString("str_saving"))
            {
                setStatus(getString("str_saved"), false);
            }
        }

        LLStringUtil::format_map_t args;
        args["REGION"] = rw.onGrid() ? rw.regionName() : std::string("this region");
        if (!rw.onGrid())
        {
            args["STATE"] = getString("str_off_grid");
        }
        else if (rw.kind().empty())
        {
            args["STATE"] = getString("str_unset");
        }
        else
        {
            const WolfWeatherProfile& p = rw.stored();
            LLStringUtil::format_map_t sargs;
            sargs["KIND"]  = rw.kind();
            sargs["LEVEL"] = (p.mKind == WolfWeatherProfile::CLEAR)
                             ? getString("str_state_clear")
                             : WolfWeatherProfile::levelLabel(p.mKind, p.mLevel);
            args["STATE"] = getString(p.mEnabled ? "str_state" : "str_state_off", sargs);
        }
        if (mRegionLine) mRegionLine->setText(getString("str_region", args));

        if (!canEdit() && mStatus && mStatus->getText().empty())
        {
            setStatus(getString("str_read_only"), false);
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
        p->openFloater();
        p->setFocus(true);
        return p;
    }

private:
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
        LLHandle<LLFloater>* handle = new LLHandle<LLFloater>(getHandle());
        gAssetStorage->getInvItemAsset(LLHost(),          // the agent's own inventory, not a task
                                       gAgent.getID(), gAgent.getSessionID(),
                                       item->getPermissions().getOwner(),
                                       LLUUID::null,      // no task: it is in inventory
                                       item->getUUID(), item->getAssetUUID(), item->getType(),
                                       &WolfWeatherPresetPicker::onAssetLoaded, (void*)handle, true);
    }

    static void onAssetLoaded(const LLUUID& asset_uuid, LLAssetType::EType type,
                              void* user_data, S32 status, LLExtStat)
    {
        std::unique_ptr<LLHandle<LLFloater>> handle((LLHandle<LLFloater>*)user_data);
        WolfWeatherPresetPicker* self = static_cast<WolfWeatherPresetPicker*>(handle->get());
        if (!self) return;                       // the picker was closed while the asset flew
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
    LLScrollListCtrl* mList = nullptr;
    LLTextBox*        mStatus = nullptr;
};

void WolfPanelLandWeather::onInventoryPreset()
{
    WolfWeatherPresetPicker::open(this,
        [this](const WolfWeatherProfile& p)
        {
            mEdit = p;
            mEdit.clampAll();
            writeControls();
            updatePreview();
            setStatus(getString("str_preset_loaded"), false);
        },
        mPickerHandle);
}
