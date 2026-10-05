/**
 * @file llpanellandaudio.cpp
 * @brief Allows configuration of "media" for a land parcel,
 *   for example movies, web pages, and audio.
 *
 * $LicenseInfo:firstyear=2007&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "llpanellandaudio.h"

// viewer includes
#include "llmimetypes.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h"
#include "lluictrlfactory.h"

// library includes
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "llfloaterurlentry.h"
#include "llfocusmgr.h"
//#include "lllineeditor.h" // <FS:CR> FIRE-593 - Unused since we use a combobox instead
#include "llparcel.h"
#include "lltextbox.h"
#include "llradiogroup.h"
#include "llspinctrl.h"
#include "llsdutil.h"
#include "lltexturectrl.h"
#include "roles_constants.h"
#include "llscrolllistctrl.h"

// Firestorm includes
#include "llviewercontrol.h"    // <FS:CR> FIRE-593 - Needed for gSavedSettings where we store our media list
#include "llclipboard.h"
// WolfViewer includes
#include "llfloaterreg.h"
#include "llnotificationsutil.h"
#include "wolfradiostations.h"

// Values for the parcel voice settings radio group
enum
{
    kRadioVoiceChatEstate = 0,
    kRadioVoiceChatPrivate = 1,
    kRadioVoiceChatDisable = 2
};

//---------------------------------------------------------------------------
// LLPanelLandAudio
//---------------------------------------------------------------------------

LLPanelLandAudio::LLPanelLandAudio(LLParcelSelectionHandle& parcel)
:   LLPanel(/*std::string("land_media_panel")*/), mParcel(parcel)
{
}


// virtual
LLPanelLandAudio::~LLPanelLandAudio()
{
}


bool LLPanelLandAudio::postBuild()
{
    mCheckSoundLocal = getChild<LLCheckBoxCtrl>("check sound local");
    childSetCommitCallback("check sound local", onCommitAny, this);

    mCheckParcelEnableVoice = getChild<LLCheckBoxCtrl>("parcel_enable_voice_channel");
    childSetCommitCallback("parcel_enable_voice_channel", onCommitAny, this);

    // This one is always disabled so no need for a commit callback
    mCheckEstateDisabledVoice = getChild<LLCheckBoxCtrl>("parcel_enable_voice_channel_is_estate_disabled");

    mCheckParcelVoiceLocal = getChild<LLCheckBoxCtrl>("parcel_enable_voice_channel_local");
    childSetCommitCallback("parcel_enable_voice_channel_local", onCommitAny, this);

    // <WolfViewer 2026-10-05> Paul: "this is terrible ... i want to replace this, 10 preset URL with
    // button to set on the land another button that says Radio Stations". The land's stream is a
    // plain line (Enter commits it), with copy and clear; below it the Radio Stations floater, the
    // 10 Wolf Territories stations and the user's own 10 presets (wolfradiostations.h).
    mMusicURLEdit = getChild<LLLineEditor>("music_url");
    mMusicURLEdit->setCommitOnFocusLost(false);
    childSetCommitCallback("music_url", onCommitAny, this);

    mBtnStreamCopyToClipboard = getChild<LLButton>("stream_copy_btn");
    mBtnStreamCopyToClipboard->setCommitCallback(boost::bind(&LLPanelLandAudio::onBtnCopyToClipboard, this));
    mBtnClearMusic = getChild<LLButton>("music_clear_btn");
    mBtnClearMusic->setCommitCallback(boost::bind(&LLPanelLandAudio::onBtnClearMusic, this));
    mBtnRadioStations = getChild<LLButton>("radio_stations_btn");
    mBtnRadioStations->setCommitCallback([](LLUICtrl*, const LLSD&) { LLFloaterReg::showInstance("wolf_radio"); });

    migrateSavedStreams();
    for (S32 i = 0; i < ROWS; ++i)
    {
        mWolfStationSet[i] = getChild<LLButton>(llformat("wolf_station_set_%d", i));
        mWolfStationSet[i]->setCommitCallback(boost::bind(&LLPanelLandAudio::onWolfStationSet, this, i));
        mWolfStationName[i] = getChild<LLTextBox>(llformat("wolf_station_%d", i));
        mPresetEdit[i] = getChild<LLLineEditor>(llformat("my_preset_%d", i));
        mPresetEdit[i]->setCommitOnFocusLost(true);
        mPresetEdit[i]->setCommitCallback(boost::bind(&LLPanelLandAudio::onPresetCommit, this, i));
        mPresetEdit[i]->setText(WolfRadioStations::preset(i));
        mPresetSet[i] = getChild<LLButton>(llformat("my_preset_set_%d", i));
        mPresetSet[i]->setCommitCallback(boost::bind(&LLPanelLandAudio::onPresetSet, this, i));
    }
    mStationsChanged = WolfRadioStations::instance().onChanged([this]() { refreshStations(); });
    WolfRadioStations::instance().load();
    refreshStations();
    // </WolfViewer>

    mCheckAVSoundAny = getChild<LLCheckBoxCtrl>("all av sound check");
    childSetCommitCallback("all av sound check", onCommitAny, this);

    mCheckAVSoundGroup = getChild<LLCheckBoxCtrl>("group av sound check");
    childSetCommitCallback("group av sound check", onCommitAny, this);

    mCheckObscureMOAP = getChild<LLCheckBoxCtrl>("obscure_moap");
    childSetCommitCallback("obscure_moap", onCommitAny, this);

    return true;
}


// public
void LLPanelLandAudio::refresh()
{
    LLParcel *parcel = mParcel->getParcel();

    if (!parcel)
    {
        clearCtrls();
        // <WolfViewer 2026-10-05> clearCtrls() disables EVERY control; the radio and the user's own
        // presets need no parcel (Paul: "none of the buttons work"), only the Set buttons do.
        mBtnRadioStations->setEnabled(true);
        for (S32 i = 0; i < ROWS; ++i) mPresetEdit[i]->setEnabled(true);
        refreshStations();
        // </WolfViewer>
    }
    else
    {
        // something selected, hooray!

        // Display options
        bool can_change_media = LLViewerParcelMgr::isParcelModifiableByAgent(parcel, GP_LAND_CHANGE_MEDIA);

        mCheckSoundLocal->set( parcel->getSoundLocal() );
        mCheckSoundLocal->setEnabled( can_change_media );

        bool allow_voice = parcel->getParcelFlagAllowVoice();

        LLViewerRegion* region = LLViewerParcelMgr::getInstance()->getSelectionRegion();
        if (region && region->isVoiceEnabled())
        {
            mCheckEstateDisabledVoice->setVisible(false);

            mCheckParcelEnableVoice->setVisible(true);
            mCheckParcelEnableVoice->setEnabled( can_change_media );
            mCheckParcelEnableVoice->set(allow_voice);

            mCheckParcelVoiceLocal->setEnabled( can_change_media && allow_voice );
        }
        else
        {
            // Voice disabled at estate level, overrides parcel settings
            // Replace the parcel voice checkbox with a disabled one
            // labelled with an explanatory message
            mCheckEstateDisabledVoice->setVisible(true);

            mCheckParcelEnableVoice->setVisible(false);
            mCheckParcelEnableVoice->setEnabled(false);
            mCheckParcelVoiceLocal->setEnabled(false);
        }

        mCheckParcelEnableVoice->set(allow_voice);
        mCheckParcelVoiceLocal->set(!parcel->getParcelFlagUseEstateVoiceChannel());

        // <WolfViewer 2026-10-05> the land's stream, and who may change it. Every new control is
        // re-enabled here: an earlier refresh without a parcel ran clearCtrls(), which disabled them all.
        if (!mMusicURLEdit->hasFocus()) mMusicURLEdit->setText(parcel->getMusicURL());
        mMusicURLEdit->setEnabled( can_change_media );
        mBtnStreamCopyToClipboard->setEnabled(true);
        mBtnClearMusic->setEnabled(can_change_media);
        mBtnRadioStations->setEnabled(true);
        for (S32 i = 0; i < ROWS; ++i)
        {
            mPresetEdit[i]->setEnabled(true);
            mPresetSet[i]->setEnabled(can_change_media && !mPresetEdit[i]->getText().empty());
        }
        refreshStations();
        // </WolfViewer>

        bool can_change_av_sounds = LLViewerParcelMgr::isParcelModifiableByAgent(parcel, GP_LAND_OPTIONS) && parcel->getHaveNewParcelLimitData();
        mCheckAVSoundAny->set(parcel->getAllowAnyAVSounds());
        mCheckAVSoundAny->setEnabled(can_change_av_sounds);

        mCheckAVSoundGroup->set(parcel->getAllowGroupAVSounds() || parcel->getAllowAnyAVSounds());  // On if "Everyone" is on
        mCheckAVSoundGroup->setEnabled(can_change_av_sounds && !parcel->getAllowAnyAVSounds());     // Enabled if "Everyone" is off

        mCheckObscureMOAP->set(parcel->getObscureMOAP());
        mCheckObscureMOAP->setEnabled(can_change_media);
    }
}
// static
void LLPanelLandAudio::onCommitAny(LLUICtrl*, void *userdata)
{
    LLPanelLandAudio *self = (LLPanelLandAudio *)userdata;

    LLParcel* parcel = self->mParcel->getParcel();
    if (!parcel)
    {
        return;
    }

    // Extract data from UI
    bool sound_local        = self->mCheckSoundLocal->get();
    std::string music_url = self->mMusicURLEdit->getText();

    bool voice_enabled = self->mCheckParcelEnableVoice->get();
    bool voice_estate_chan = !self->mCheckParcelVoiceLocal->get();

    bool any_av_sound       = self->mCheckAVSoundAny->get();
    bool group_av_sound     = true;     // If set to "Everyone" then group is checked as well
    if (!any_av_sound)
    {   // If "Everyone" is off, use the value from the checkbox
        group_av_sound = self->mCheckAVSoundGroup->get();
    }

    bool obscure_moap = self->mCheckObscureMOAP->get();

    // Remove leading/trailing whitespace (common when copying/pasting)
    LLStringUtil::trim(music_url);

    // <FS> Add leading http:// if not already present
    if (!music_url.empty() && music_url.find("://") == std::string::npos)
    {
        music_url.insert(0, "http://");
    }
    // </FS>

    // Push data into current parcel
    parcel->setParcelFlag(PF_ALLOW_VOICE_CHAT, voice_enabled);
    parcel->setParcelFlag(PF_USE_ESTATE_VOICE_CHAN, voice_estate_chan);
    parcel->setParcelFlag(PF_SOUND_LOCAL, sound_local);
    parcel->setMusicURL(music_url);
    parcel->setAllowAnyAVSounds(any_av_sound);
    parcel->setAllowGroupAVSounds(group_av_sound);
    parcel->setObscureMOAP(obscure_moap);

    // Send current parcel data upstream to server
    LLViewerParcelMgr::getInstance()->sendParcelPropertiesUpdate( parcel );

    // Might have changed properties, so let's redraw!
    self->refresh();
}

void LLPanelLandAudio::onBtnCopyToClipboard()
{
    std::string music_url = mMusicURLEdit->getText();
    LLStringUtil::trim(music_url);

    if (!music_url.empty())
    {
        LLClipboard::instance().copyToClipboard(utf8str_to_wstring(music_url), 0, static_cast<S32>(music_url.size()));
    }
}

// ---- <WolfViewer 2026-10-05> stations and presets -----------------------------------------

void LLPanelLandAudio::onBtnClearMusic()
{
    mMusicURLEdit->setText(LLStringUtil::null);
    onCommitAny(mMusicURLEdit, this);
}

void LLPanelLandAudio::refreshStations()
{
    LLParcel* parcel = mParcel->getParcel();
    const bool can_change = parcel && LLViewerParcelMgr::isParcelModifiableByAgent(parcel, GP_LAND_CHANGE_MEDIA);
    WolfRadioStations& st = WolfRadioStations::instance();
    const LLSD& featured = st.featured();
    for (S32 i = 0; i < ROWS; ++i)
    {
        const bool have = st.isLoaded() && i < (S32)featured.size();
        std::string label;
        if (have) label = featured[i]["name"].asString();
        else if (i == 0) label = st.isLoading() ? "Loading stations..." : st.lastError();
        mWolfStationName[i]->setText(label);
        mWolfStationName[i]->setToolTip(have ? featured[i]["url"].asString() : std::string());
        mWolfStationSet[i]->setVisible(have);
        mWolfStationSet[i]->setEnabled(have && can_change);
    }
}

void LLPanelLandAudio::onWolfStationSet(S32 index)
{
    const LLSD& featured = WolfRadioStations::instance().featured();
    if (index < 0 || index >= (S32)featured.size()) return;
    if (!WolfRadioStations::setLandMusic(featured[index]["url"].asString()))
    {
        LLNotificationsUtil::add("GenericAlert", LLSD().with("MESSAGE", "You can't change the music on this land."));
    }
}

void LLPanelLandAudio::onPresetCommit(S32 index)
{
    std::string url = mPresetEdit[index]->getText();
    LLStringUtil::trim(url);
    if (url != WolfRadioStations::preset(index)) WolfRadioStations::setPreset(index, url);
    LLParcel* parcel = mParcel->getParcel();
    mPresetSet[index]->setEnabled(!url.empty() && parcel && LLViewerParcelMgr::isParcelModifiableByAgent(parcel, GP_LAND_CHANGE_MEDIA));
}

void LLPanelLandAudio::onPresetSet(S32 index)
{
    onPresetCommit(index);
    std::string url = mPresetEdit[index]->getText();
    LLStringUtil::trim(url);
    if (url.empty()) return;
    if (!WolfRadioStations::setLandMusic(url))
    {
        LLNotificationsUtil::add("GenericAlert", LLSD().with("MESSAGE", "You can't change the music on this land."));
    }
}

// static
void LLPanelLandAudio::migrateSavedStreams()
{
    // The old Firestorm list of saved streams (FSStreamList "audio") becomes the first presets,
    // once, so nobody loses the addresses they had saved.
    if (gSavedSettings.getBOOL("WolfMusicPresetsMigrated")) return;
    LLSD old = gSavedSettings.getLLSD("FSStreamList");
    S32 slot = 0;
    for (LLSD::array_const_iterator it = old["audio"].beginArray(); it != old["audio"].endArray() && slot < WolfRadioStations::PRESET_COUNT; ++it)
    {
        std::string url = it->asString();
        LLStringUtil::trim(url);
        if (url.empty()) continue;
        while (slot < WolfRadioStations::PRESET_COUNT && !WolfRadioStations::preset(slot).empty()) ++slot;
        if (slot >= WolfRadioStations::PRESET_COUNT) break;
        WolfRadioStations::setPreset(slot++, url);
    }
    gSavedSettings.setBOOL("WolfMusicPresetsMigrated", true);
}
