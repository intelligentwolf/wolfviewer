/**
 * @file wolfregionwolfenvironments.cpp
 * @brief WolfViewer: Region / Estate > Wolf Environments (see wolfregionwolfenvironments.h).
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

#include "wolfregionwolfenvironments.h"

#include <algorithm>

#include "llagent.h"
#include "llbutton.h"
#include "llcombobox.h"
#include "lldir.h"
#include "llfloaterreg.h"
#include "llframetimer.h"
#include "llinventorymodel.h"
#include "llnotificationsutil.h"
#include "llsettingspicker.h"
#include "llstring.h"
#include "lltabcontainer.h"
#include "lltextbox.h"
#include "llviewerinventory.h"
#include "llviewerregion.h"
#include "wolfautoenvironment.h"
#include "wolfregionweather.h"

namespace
{
    // Source: WolfAutoEnvironmentModule.cs:62 CYCLE_NAME (also wolfregionweather.cpp:544 WOLF_AUTO_CYCLE_NAME)
    const std::string AUTO_CYCLE_NAME("Jimmy Olsen's Automatic Environment");
    // Source: WolfEnvironment.cs:67 WOLF_CYCLE_NAME, :69 ON_TOP_PREFIX ("on top" keeps the owner's cycle name after it, :935)
    const std::string WOLF_CYCLE_NAME("Wolf Environment");
    const std::string ON_TOP_PREFIX("Wolf Environment on ");
    // The region has not confirmed an ExtEnvironment PUT/DELETE in this long: say so rather than "Applying..." for ever.
    // A refusal is shown by the region's own WLRegionApplyFail alert (llenvironment.cpp coroUpdateEnvironment) and
    // otherwise only by this silence.
    constexpr F64 ANSWER_WAIT_SECS = 15.0;

    bool caseless_less(const std::string& a, const std::string& b)
    {
        std::string la(a), lb(b);
        LLStringUtil::toLower(la);
        LLStringUtil::toLower(lb);
        return la < lb;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
// Sky
// ═══════════════════════════════════════════════════════════════════════════════════════════

WolfPanelRegionSky::WolfPanelRegionSky() = default;

WolfPanelRegionSky::~WolfPanelRegionSky()
{
    if (mEnvChanged.connected()) mEnvChanged.disconnect();
    // Source: llpanelenvironment.cpp:240-256 onVisibilityChange closes its picker with the panel
    if (LLFloater* p = mPickerHandle.get()) p->closeFloater();
}

bool WolfPanelRegionSky::postBuild()
{
    mPreset   = getChild<LLComboBox>("sky_preset");
    mNow      = getChild<LLTextBox>("sky_now");
    mOverride = getChild<LLTextBox>("sky_override");
    mChosen   = getChild<LLTextBox>("sky_chosen");
    mStatus   = getChild<LLTextBox>("sky_status");
    mApply    = getChild<LLButton>("sky_apply");

    mPreset->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPresetPicked(); });
    childSetAction("sky_inventory", [this](LLUICtrl*, const LLSD&) { onInventoryButton(); });
    childSetAction("sky_apply",     [this](LLUICtrl*, const LLSD&) { onApply(); });
    childSetAction("sky_revert",    [this](LLUICtrl*, const LLSD&) { onRevert(); });
    childSetAction("sky_default",   [this](LLUICtrl*, const LLSD&) { onDefault(); });
    childSetAction("sky_goto_real_place", [this](LLUICtrl*, const LLSD&)
    {
        // the page's own tab container (panel_region_wolf_environments.xml wolf_env_pages)
        if (LLTabContainer* pages = dynamic_cast<LLTabContainer*>(getParent()))
        {
            pages->selectTabByName("panel_region_auto_environment");
        }
    });
    childSetAction("sky_open_wolf_env", [](LLUICtrl*, const LLSD&) { LLFloaterReg::showInstance("wolf_environment"); });

    // Source: llpanelenvironment.cpp:201 - the region's environment arriving or changing. THIS is how a change is
    // confirmed: OpenSim answers the PUT/DELETE with {success} and no "environment" map (EnvironmentModule.cs:455, 661,
    // 679), so LLEnvironment never calls the update callback (llenvironment.cpp coroUpdateEnvironment only applies a
    // reply that has one). The region then sends RegionInfo (EnvironmentModule.cs:289 WindlightRefresh ->
    // HandleRegionInfoRequest), which re-requests the region's environment (llenvironment.cpp:1026) and lands here.
    mEnvChanged = LLEnvironment::instance().setEnvironmentChanged([this](LLEnvironment::EnvSelection_t env, S32)
    {
        if (env != LLEnvironment::ENV_REGION) return;
        if (mWaiting)
        {
            // an apply is confirmed when the region's cycle carries the chosen name (OpenSim names it after day_name,
            // ViewerEnvironment.cs:417-418; a preset's day is named before it is sent); a reset by any new region day
            LLSettingsDay::ptr_t day = LLEnvironment::instance().getEnvironmentDay(LLEnvironment::ENV_REGION);
            if (mWaitingIsReset || (day && day->getName() == mWaitingName))
            {
                onRegionAnswered(mSerial, mWaitingIsReset);
                return;
            }
        }
        updateState();
    });

    fillPresets();
    showChoice();
    updateState();
    return true;
}

void WolfPanelRegionSky::fillPresets()
{
    mWriting = true;
    mPreset->removeall();
    mPreset->add(getString("str_preset_none"), LLSD(std::string()));
#ifdef OPENSIM
    // Source: llenvironment.cpp:933-982 loadLegacyPresets - the viewer's own skies and day cycles (app_settings and the
    // resident's user_settings windlight folders), the same lists Quick Preferences offers (quickprefs.cpp:285-360).
    const LLEnvironment& env = LLEnvironment::instance();
    std::vector<std::string> days(env.mLegacyDayCycles.begin(), env.mLegacyDayCycles.end());
    std::vector<std::string> skies(env.mLegacySkies.begin(), env.mLegacySkies.end());
    std::sort(days.begin(), days.end(), caseless_less);
    days.erase(std::unique(days.begin(), days.end()), days.end());
    std::sort(skies.begin(), skies.end(), caseless_less);
    skies.erase(std::unique(skies.begin(), skies.end()), skies.end());
    if (!days.empty())
    {
        mPreset->add(getString("str_group_days"), LLSD(std::string()), EAddPosition::ADD_BOTTOM, false);
        for (const std::string& name : days) mPreset->add(name, LLSD("day:" + name));
    }
    if (!skies.empty())
    {
        mPreset->add(getString("str_group_skies"), LLSD(std::string()), EAddPosition::ADD_BOTTOM, false);
        for (const std::string& name : skies) mPreset->add(name, LLSD("sky:" + name));
    }
    mPresetsFilled = !days.empty() || !skies.empty();
#endif
    mPreset->selectFirstItem();
    mWriting = false;
}

void WolfPanelRegionSky::onPresetPicked()
{
    if (mWriting) return;
    const std::string v = mPreset->getValue().asString();
    if (v.size() > 4 && (v.compare(0, 4, "day:") == 0 || v.compare(0, 4, "sky:") == 0))
    {
        mChoice = v.compare(0, 4, "day:") == 0 ? DAY_PRESET : SKY_PRESET;
        mChoiceName = v.substr(4);
        mChoiceItem.setNull();
        mChoiceRegion = agentRegionId();
        setStatus(std::string(), false);
    }
    else
    {
        // "Choose a preset..." again: forget a preset choice (an inventory choice stays)
        if (mChoice == DAY_PRESET || mChoice == SKY_PRESET) mChoice = NONE;
    }
    showChoice();
    updateState();
}

LLFloater* WolfPanelRegionSky::picker(bool create)
{
    // Source: llpanelenvironment.cpp:407-422 getSettingsPicker
    LLFloaterSettingsPicker* p = static_cast<LLFloaterSettingsPicker*>(mPickerHandle.get());
    if (!p && create)
    {
        p = new LLFloaterSettingsPicker(this, LLUUID::null);
        mPickerHandle = p->getHandle();
        LLHandle<LLPanel> h = getHandle();
        p->setCommitCallback([h](LLUICtrl*, const LLSD& data)
        {
            if (WolfPanelRegionSky* self = static_cast<WolfPanelRegionSky*>(h.get())) self->chooseItem(data["ItemId"].asUUID());
        });
    }
    return p;
}

void WolfPanelRegionSky::onInventoryButton()
{
    std::string why;
    if (!canEdit(&why))
    {
        setStatus(why, true);
        return;
    }
    // Source: llpanelenvironment.cpp:881-896 onBtnSelect - every settings type, as the stock region tab
    if (LLFloaterSettingsPicker* p = static_cast<LLFloaterSettingsPicker*>(picker(true)))
    {
        p->setSettingsFilter(LLSettingsType::ST_NONE);
        p->setSettingsItemId(mChoice == INVENTORY ? mChoiceItem : LLUUID::null);
        p->openFloater();
        p->setFocus(true);
    }
}

void WolfPanelRegionSky::chooseItem(const LLUUID& item_id)
{
    LLViewerInventoryItem* item = gInventory.getItem(item_id);
    if (!item)
    {
        setStatus(getString("str_item_missing"), true);
        return;
    }
    if (item->getType() != LLAssetType::AT_SETTINGS)
    {
        setStatus(getString("str_not_settings"), true);
        return;
    }
    mChoice = INVENTORY;
    mChoiceItem = item_id;
    mChoiceName = item->getName();
    mChoiceRegion = agentRegionId();
    mWriting = true;
    mPreset->selectFirstItem();
    mWriting = false;
    setStatus(std::string(), false);
    showChoice();
    updateState();
}

bool WolfPanelRegionSky::handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop, EDragAndDropType cargo_type,
                                           void* cargo_data, EAcceptance* accept, std::string& tooltip_msg)
{
    if (cargo_type != DAD_SETTINGS)
    {
        return LLPanel::handleDragAndDrop(x, y, mask, drop, cargo_type, cargo_data, accept, tooltip_msg);
    }
    // Source: llpanelenvironment.cpp:1181-1219 LLSettingsDropTarget::handleDragAndDrop - the whole page is the target
    std::string why;
    LLViewerInventoryItem* item = cargo_data ? static_cast<LLViewerInventoryItem*>(cargo_data) : nullptr;
    if (!canEdit(&why) || !item || !gInventory.getItem(item->getUUID()))
    {
        *accept = ACCEPT_NO;
        if (!why.empty()) tooltip_msg = why;
        return true;
    }
    *accept = ACCEPT_YES_COPY_SINGLE;
    if (drop) chooseItem(item->getUUID());
    return true;
}

void WolfPanelRegionSky::showChoice()
{
    if (!mChosen) return;
    if (mChoice == NONE)
    {
        mChosen->setText(getString("str_nothing"));
        return;
    }
    LLStringUtil::format_map_t args;
    args["[NAME]"] = mChoiceName;
    args["[KIND]"] = getString(mChoice == DAY_PRESET ? "str_kind_day" : mChoice == SKY_PRESET ? "str_kind_sky" : "str_kind_inventory");
    mChosen->setText(getString("str_chosen", args));
}

std::string WolfPanelRegionSky::agentRegionId() const
{
    LLViewerRegion* region = gAgent.getRegion();
    return region ? region->getRegionID().asString() : std::string();
}

bool WolfPanelRegionSky::canEdit(std::string* why) const
{
    LLEnvironment& env = LLEnvironment::instance();
    // Source: llenvironment.cpp:1143-1146 isExtendedEnvironmentEnabled - coroUpdateEnvironment returns in silence
    // without the capability, so it is checked here where the page can say so
    if (!env.isExtendedEnvironmentEnabled())
    {
        if (why) *why = getString("str_no_cap");
        return false;
    }
    // Source: llenvironment.cpp:1138-1141 canAgentUpdateRegionEnvironment (the stock region tab's canEdit,
    // llfloaterregioninfo.cpp:208); OpenSim checks CanIssueEstateCommand (EnvironmentModule.cs:566)
    if (!env.canAgentUpdateRegionEnvironment())
    {
        if (why) *why = getString("str_read_only");
        return false;
    }
    return true;
}

WolfPanelRegionSky::Owner WolfPanelRegionSky::skyOwner(std::string* name) const
{
    LLSettingsDay::ptr_t day = LLEnvironment::instance().getEnvironmentDay(LLEnvironment::ENV_REGION);
    const std::string n = day ? day->getName() : std::string();
    if (name) *name = n;
    // Source: WolfAutoEnvironmentModule.cs:205-215 - a region Wolf Environment is baked before the automatic one is looked at
    if (n == WOLF_CYCLE_NAME) return WOLF;
    if (n.compare(0, ON_TOP_PREFIX.size(), ON_TOP_PREFIX) == 0)
    {
        if (name) *name = n.substr(ON_TOP_PREFIX.size());
        return ON_TOP;
    }
    // weather.php autoEnv (wolfregionweather.h autoEnvSky): enabled && skyOn, the module's own test (WolfAutoEnvironmentModule.cs:218)
    if (n == AUTO_CYCLE_NAME || WolfRegionWeather::instance().autoEnvSky()) return AUTO;
    return OWN;
}

void WolfPanelRegionSky::updateState()
{
    if (!mNow) return;

    const std::string region = agentRegionId();
    if (region != mShownRegion)
    {
        if (mChoice != NONE && mChoiceRegion != region)
        {
            mChoice = NONE;
            mWriting = true;
            mPreset->selectFirstItem();
            mWriting = false;
            showChoice();
            if (!mShownRegion.empty()) setStatus(getString("str_region_changed"), false);
        }
        mShownRegion = region;
    }

    // what the region shows now
    std::string name;
    const Owner owner = skyOwner(&name);
    const bool have_day = (bool)LLEnvironment::instance().getEnvironmentDay(LLEnvironment::ENV_REGION);
    LLStringUtil::format_map_t args;
    if (!have_day && owner != AUTO)
    {
        mNow->setText(getString("str_now_unknown"));
    }
    else
    {
        std::string what;
        LLStringUtil::format_map_t inner;
        switch (owner)
        {
        case AUTO:
            inner["[PLACE]"] = WolfRegionWeather::instance().autoEnvPlace();
            what = getString("str_now_auto", inner);
            break;
        case WOLF:
            what = getString("str_now_wolf");
            break;
        case ON_TOP:
            inner["[NAME]"] = name.empty() ? getString("str_now_default") : name;
            what = getString("str_now_on_top", inner);
            break;
        default:
            what = name.empty() ? getString("str_now_default") : name;
            break;
        }
        args["[NAME]"] = what;
        mNow->setText(getString("str_now", args));
    }

    // who draws over an applied sky, and where that is changed
    std::string over;
    if (owner == AUTO)
    {
        LLStringUtil::format_map_t a;
        a["[PLACE]"] = WolfRegionWeather::instance().autoEnvPlace();
        over = getString("str_override_auto", a);
    }
    else if (owner == WOLF) over = getString("str_override_wolf");
    else if (owner == ON_TOP) over = getString("str_override_on_top");
    mOverride->setText(over);
    mOverride->setVisible(!over.empty());
    getChildView("sky_goto_real_place")->setVisible(owner == AUTO);
    getChildView("sky_open_wolf_env")->setVisible(owner == WOLF || owner == ON_TOP);

    std::string why;
    const bool editable = canEdit(&why);
    mPreset->setEnabled(editable && !mWaiting && mPresetsFilled);
    getChildView("sky_inventory")->setEnabled(editable && !mWaiting);
    mApply->setEnabled(editable && !mWaiting && mChoice != NONE);
    getChildView("sky_revert")->setEnabled(!mWaiting && mChoice != NONE);
    getChildView("sky_default")->setEnabled(editable && !mWaiting);
    const std::string shown = mStatus->getText();
    const bool reason_shown = shown == getString("str_read_only") || shown == getString("str_no_cap");
    if (!editable && !mWaiting && (shown.empty() || reason_shown)) setStatus(why, false);   // never over another message
    else if (editable && !mWaiting && reason_shown)
    {
        setStatus(std::string(), false);   // the reason no longer holds (rights or the capability arrived)
    }
}

void WolfPanelRegionSky::refresh()
{
    updateState();
}

void WolfPanelRegionSky::draw()
{
    if (mWaiting && LLFrameTimer::getElapsedSeconds() - mWaitingSince > ANSWER_WAIT_SECS)
    {
        mWaiting = false;   // a late answer is still shown (onRegionAnswered checks the serial, not this flag)
        setStatus(getString("str_no_answer"), true);
        updateState();
    }
    else if (mTick.getElapsedTimeF32() > 1.f)
    {
        // the rights, the capability and the region underfoot change without telling this page
        mTick.reset();
        updateState();
    }
    LLPanel::draw();
}

void WolfPanelRegionSky::setStatus(const std::string& msg, bool error)
{
    if (!mStatus) return;
    mStatus->setText(msg);
    // Same colours as the Weather and Real place pages (wolfregionweather.cpp / wolfautoenvironment.cpp:185 setStatus)
    mStatus->setColor(error ? LLColor4(0.88f, 0.33f, 0.28f, 1.f) : LLColor4(0.79f, 0.64f, 0.15f, 1.f));
}

void WolfPanelRegionSky::beginRequest(const std::string& msg, bool reset)
{
    ++mSerial;
    mWaiting = true;
    mWaitingIsReset = reset;
    mWaitingSince = LLFrameTimer::getElapsedSeconds();
    setStatus(msg, false);
    updateState();
}

LLSettingsDay::ptr_t WolfPanelRegionSky::loadDayPreset(const std::string& name, std::string& why) const
{
    // Source: quickprefs.cpp:904 - the file in app_settings, then (llenvironment.cpp:936 loads both) user_settings
    LLSD messages;
    LLSettingsDay::ptr_t day = LLEnvironment::createDayCycleFromLegacyPreset(
        gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "windlight", "days", name + ".xml"), messages);
    if (!day)
    {
        messages = LLSD();
        day = LLEnvironment::createDayCycleFromLegacyPreset(
            gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "windlight", "days", name + ".xml"), messages);
    }
    if (!day) why = messages["REASONS"].asString();
    return day;
}

LLSettingsSky::ptr_t WolfPanelRegionSky::loadSkyPreset(const std::string& name, std::string& why) const
{
    // Source: quickprefs.cpp:844
    LLSD messages;
    LLSettingsSky::ptr_t sky = LLEnvironment::createSkyFromLegacyPreset(
        gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "windlight", "skies", name + ".xml"), messages);
    if (!sky)
    {
        messages = LLSD();
        sky = LLEnvironment::createSkyFromLegacyPreset(
            gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "windlight", "skies", name + ".xml"), messages);
    }
    if (!sky) why = messages["REASONS"].asString();
    return sky;
}

void WolfPanelRegionSky::onApply()
{
    std::string why;
    if (!canEdit(&why))
    {
        setStatus(why, true);
        return;
    }
    if (mChoice == NONE || mWaiting) return;
    if (mChoiceRegion != agentRegionId())
    {
        updateState();   // clears the choice and says why
        return;
    }

    LLHandle<LLPanel> h = getHandle();
    LLEnvironment& env = LLEnvironment::instance();
    mWaitingName = mChoiceName;
    mWaitingOwner = skyOwner(nullptr);
    const U64 serial = mSerial + 1;   // beginRequest's
    auto answered = [h, serial](S32, LLEnvironment::EnvironmentInfo::ptr_t)
    {
        if (WolfPanelRegionSky* self = static_cast<WolfPanelRegionSky*>(h.get())) self->onRegionAnswered(serial, false);
    };

    if (mChoice == INVENTORY)
    {
        LLViewerInventoryItem* item = gInventory.getItem(mChoiceItem);
        if (!item)
        {
            setStatus(getString("str_item_missing"), true);
            return;
        }
        // Source: llpanelenvironment.cpp:1004-1028 onPickerCommitted(item, track) - permissions to flags, whole day
        U32 flags = 0;
        if (!item->getPermissions().allowOperationBy(PERM_MODIFY, gAgent.getID())) flags |= LLSettingsBase::FLAG_NOMOD;
        if (!item->getPermissions().allowOperationBy(PERM_TRANSFER, gAgent.getID())) flags |= LLSettingsBase::FLAG_NOTRANS;
        beginRequest(getString("str_applying"), false);
        env.updateRegion(item->getAssetUUID(), item->getName(), LLEnvironment::NO_TRACK, -1, -1, flags,
                         LLEnvironment::altitudes_vect_t(), answered);
        return;
    }

    LLSettingsDay::ptr_t day;
    if (mChoice == DAY_PRESET)
    {
        day = loadDayPreset(mChoiceName, why);
    }
    else
    {
        LLSettingsSky::ptr_t sky = loadSkyPreset(mChoiceName, why);
        if (sky)
        {
            // Source: llenvironment.cpp:2320-2325 updateParcel(psky): the region's day with this sky in place of its skies.
            // createDayCycleFromEnvironment answers null while the region's day has not arrived (llenvironment.cpp:2770),
            // which updateParcel(psky) would then dereference - checked here.
            day = env.createDayCycleFromEnvironment(LLEnvironment::ENV_REGION, sky);
            if (!day)
            {
                setStatus(getString("str_no_region_day"), true);
                return;
            }
            day->setFlag(sky->getFlags());
        }
    }
    if (!day)
    {
        LLStringUtil::format_map_t args;
        args["[NAME]"] = mChoiceName;
        args["[WHY]"] = why;
        setStatus(getString("str_preset_failed", args), true);
        return;
    }
    // The cycle is named after the preset: a clone of the region's day keeps the Wolf modules' name otherwise, and they
    // would take the owner's sky for their own (WolfEnvironment.cs:605-611 IsOursEnv) and throw it away on Restore.
    day->setName(mChoiceName);
    beginRequest(getString("str_applying"), false);
    env.updateRegion(day, -1, -1, LLEnvironment::altitudes_vect_t(), answered);
}

void WolfPanelRegionSky::onRegionAnswered(U64 serial, bool reset)
{
    if (serial != mSerial) return;
    mWaiting = false;
    if (reset)
    {
        setStatus(getString("str_reset_done"), false);
    }
    else
    {
        LLStringUtil::format_map_t args;
        args["[NAME]"] = mWaitingName;
        if (mWaitingOwner == AUTO || mWaitingOwner == WOLF)
        {
            args["[WHO]"] = getString(mWaitingOwner == AUTO ? "str_who_auto" : "str_who_wolf");
            setStatus(getString("str_saved_kept", args), false);
        }
        else
        {
            setStatus(getString("str_saved", args), false);
        }
        mChoice = NONE;
        mWriting = true;
        mPreset->selectFirstItem();
        mWriting = false;
        showChoice();
    }
    updateState();
}

void WolfPanelRegionSky::onRevert()
{
    mChoice = NONE;
    mChoiceItem.setNull();
    mWriting = true;
    mPreset->selectFirstItem();
    mWriting = false;
    setStatus(std::string(), false);
    showChoice();
    updateState();
}

void WolfPanelRegionSky::onDefault()
{
    std::string why;
    if (!canEdit(&why))
    {
        setStatus(why, true);
        return;
    }
    // Source: llpanelenvironment.cpp:843-857 onBtnDefault (confirm, then resetParcel(INVALID_PARCEL_ID) = resetRegion)
    LLHandle<LLPanel> h = getHandle();
    LLNotificationsUtil::add("WolfRegionSkyConfirmDefault", LLSD(), LLSD(), [h](const LLSD& notif, const LLSD& resp)
    {
        if (LLNotificationsUtil::getSelectedOption(notif, resp) != 0) return;
        WolfPanelRegionSky* self = static_cast<WolfPanelRegionSky*>(h.get());
        if (!self || self->mWaiting) return;
        self->beginRequest(self->getString("str_resetting"), true);
        const U64 serial = self->mSerial;
        LLEnvironment::instance().resetRegion([h, serial](S32, LLEnvironment::EnvironmentInfo::ptr_t)
        {
            if (WolfPanelRegionSky* s = static_cast<WolfPanelRegionSky*>(h.get())) s->onRegionAnswered(serial, true);
        });
    });
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
// the tab
// ═══════════════════════════════════════════════════════════════════════════════════════════

bool WolfPanelRegionWolfEnvironments::postBuild()
{
    mPages = getChild<LLTabContainer>("wolf_env_pages");

    // Source: llfloaterregioninfo.cpp:313-323 (before this change) - each page built from its own file, then added
    mSky = new WolfPanelRegionSky;
    mSky->buildFromFile("panel_region_sky.xml");
    mPages->addTabPanel(LLTabContainer::TabPanelParams().panel(mSky).select_tab(true));

    mWeather = new WolfPanelRegionWeather;
    mWeather->buildFromFile("panel_weather_controls.xml");
    mPages->addTabPanel(mWeather);

    mAutoEnv = new WolfPanelRegionAutoEnvironment;
    mAutoEnv->buildFromFile("panel_region_auto_environment.xml");
    mPages->addTabPanel(mAutoEnv);
    return true;
}

void WolfPanelRegionWolfEnvironments::refresh()
{
    if (mSky) mSky->refresh();
    if (mWeather) mWeather->refresh();
    if (mAutoEnv) mAutoEnv->refresh();
}
