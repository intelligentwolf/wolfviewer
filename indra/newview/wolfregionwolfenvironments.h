/**
 * @file wolfregionwolfenvironments.h
 * @brief WolfViewer: Region / Estate > Wolf Environments - the region's sky, weather and real place in one tab.
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

#ifndef WOLF_REGION_WOLF_ENVIRONMENTS_H
#define WOLF_REGION_WOLF_ENVIRONMENTS_H

#include "llpanel.h"
#include "llhandle.h"
#include "lluuid.h"
#include "llenvironment.h"
#include "llframetimer.h"

#include <string>

class LLButton;
class LLComboBox;
class LLFloater;
class LLTabContainer;
class LLTextBox;
class WolfPanelRegionWeather;
class WolfPanelRegionAutoEnvironment;

/**
 * [WOLF ENVIRONMENTS 2026-11-02] Region / Estate > Wolf Environments > Sky. Ticket #448 (Jimmy Olsen): residents wanted a
 * preset sky for their region without the Wolf sky, but the Weather tab only set the weather and the only way to the sky
 * was to add an EEP by hand in the Environment tab.
 *
 *   persona/story  a region owner or estate manager wants everyone in their region to see a chosen sky - one of the
 *                  viewer's presets or an EEP setting from their inventory - set from the same tab as the weather.
 *   flow           Sky page -> choose a preset, or Use Inventory Sky... / drag an item onto the page -> Apply ->
 *                  "Saved" (or the failure, on the page and in the region's own error message).
 *
 * THE PATH (no protocol of our own): the region's ExtEnvironment capability, exactly as the stock Environment tab:
 *   inventory item  LLEnvironment::updateRegion(asset, name, NO_TRACK, -1, -1, flags)   (llpanelenvironment.cpp:994-1031
 *                   onPickerCommitted; OpenSim EnvironmentModule.cs:602-624 replaces the sky, water or whole day and
 *                   names the cycle after the item)
 *   day preset      LLEnvironment::createDayCycleFromLegacyPreset -> updateRegion(day)   (quickprefs.cpp:904 loads it the
 *                   same way for the resident's own sky; llpanelenvironment.cpp onEditCommitted sends a whole day)
 *   sky preset      createSkyFromLegacyPreset -> createDayCycleFromEnvironment(ENV_REGION, sky) -> updateRegion(day)
 *                   (what LLEnvironment::updateParcel(psky) does, llenvironment.cpp:2320-2325, with the null day it
 *                   does not check, checked here, and the cycle renamed so the Wolf modules never take it for theirs)
 *   default         LLEnvironment::resetRegion (the stock "Use Default" button, llpanelenvironment.cpp:843-857)
 * Day length and offset are sent as -1, which coroUpdateEnvironment leaves out (llenvironment.cpp), so the region keeps
 * its own. WHO MAY: LLEnvironment::canAgentUpdateRegionEnvironment (god or gAgent.canManageEstate), the courtesy copy of
 * OpenSim's CanIssueEstateCommand(agent, false) check (EnvironmentModule.cs:566).
 *
 * WHO DRAWS THE SKY: the region's day cycle name says it (the Wolf modules name theirs, WolfAutoEnvironmentModule.cs:62
 * CYCLE_NAME, WolfEnvironment.cs:67-69 WOLF_CYCLE_NAME / ON_TOP_PREFIX). When the real place or a Wolf Environment draws
 * the sky, the module keeps the owner's own sky aside (SaveCurrent) and puts it back when switched off (Restore); "on top"
 * bakes the weather over the owner's sky. The page says which, so an Apply that is drawn over within a minute is
 * expected, not a mystery.
 */
class WolfPanelRegionSky : public LLPanel
{
public:
    WolfPanelRegionSky();
    ~WolfPanelRegionSky() override;

    bool postBuild() override;
    void refresh() override;
    void draw() override;
    bool handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop, EDragAndDropType cargo_type, void* cargo_data,
                           EAcceptance* accept, std::string& tooltip_msg) override;

private:
    enum ChoiceKind { NONE, DAY_PRESET, SKY_PRESET, INVENTORY };
    enum Owner { OWN, AUTO, WOLF, ON_TOP };

    void fillPresets();
    void onPresetPicked();
    void onInventoryButton();
    void chooseItem(const LLUUID& item_id);
    void onApply();
    void onRevert();
    void onDefault();
    void showChoice();
    void updateState();
    void setStatus(const std::string& msg, bool error);
    void beginRequest(const std::string& msg, bool reset);
    void onRegionAnswered(U64 serial, bool reset);
    bool canEdit(std::string* why = nullptr) const;
    Owner skyOwner(std::string* name) const;
    LLSettingsDay::ptr_t loadDayPreset(const std::string& name, std::string& why) const;
    LLSettingsSky::ptr_t loadSkyPreset(const std::string& name, std::string& why) const;
    LLFloater* picker(bool create);
    std::string agentRegionId() const;

    ChoiceKind  mChoice = NONE;
    std::string mChoiceName;      ///< preset name, or the inventory item's name
    LLUUID      mChoiceItem;      ///< the inventory item (INVENTORY)
    std::string mChoiceRegion;    ///< the region the choice was made in
    std::string mShownRegion;     ///< the region the page last showed
    bool        mPresetsFilled = false;
    bool        mWriting = false;
    bool        mWaiting = false; ///< a request is out and has not been answered
    LLFrameTimer mTick;          ///< draw(): the once-a-second look at rights, capability and region
    bool        mWaitingIsReset = false;
    std::string mWaitingName;
    Owner       mWaitingOwner = OWN;
    F64         mWaitingSince = 0.0;
    U64         mSerial = 0;
    LLHandle<LLFloater> mPickerHandle;
    LLEnvironment::connection_t mEnvChanged;

    LLComboBox* mPreset = nullptr;
    LLTextBox*  mNow = nullptr;
    LLTextBox*  mOverride = nullptr;
    LLTextBox*  mChosen = nullptr;
    LLTextBox*  mStatus = nullptr;
    LLButton*   mApply = nullptr;
};

/**
 * [WOLF ENVIRONMENTS 2026-11-02] The tab itself: Sky, Weather (WolfPanelRegionWeather, panel_weather_controls.xml,
 * unchanged) and Real place (Jimmy Olsen's Automatic Environment, panel_region_auto_environment.xml) as pages of one
 * tab, replacing the two top-level tabs "Weather" and "Jimmy's EEP & Weather". The pages keep their own code; the
 * floater still refreshes them through weatherPanel() / autoEnvPanel().
 */
class WolfPanelRegionWolfEnvironments : public LLPanel
{
public:
    bool postBuild() override;
    void refresh() override;

    WolfPanelRegionWeather* weatherPanel() const { return mWeather; }
    WolfPanelRegionAutoEnvironment* autoEnvPanel() const { return mAutoEnv; }

private:
    LLTabContainer* mPages = nullptr;
    WolfPanelRegionSky* mSky = nullptr;
    WolfPanelRegionWeather* mWeather = nullptr;
    WolfPanelRegionAutoEnvironment* mAutoEnv = nullptr;
};

#endif // WOLF_REGION_WOLF_ENVIRONMENTS_H
