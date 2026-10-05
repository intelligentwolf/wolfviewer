/**
 * @file llpanellandaudio.h
 * @brief Allows configuration of "audio" for a land parcel.
 *
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

#ifndef LLPANELLANDAUDIO_H
#define LLPANELLANDAUDIO_H

#include "lllineeditor.h"
#include "llbutton.h"
#include <boost/signals2.hpp>
#include "llpanel.h"
#include "llparcelselection.h"
#include "lluifwd.h"    // widget pointer types

class LLPanelLandAudio
    :   public LLPanel
{
public:
    LLPanelLandAudio(LLSafeHandle<LLParcelSelection>& parcelp);
    /*virtual*/ ~LLPanelLandAudio();
    /*virtual*/ bool postBuild();
    void refresh();

private:
    static void onCommitAny(LLUICtrl* ctrl, void *userdata);
    void onBtnCopyToClipboard();
    // <WolfViewer 2026-10-05> the music URL line, the Radio Stations floater, the 10 Wolf
    // Territories stations and the user's own 10 presets (wolfradiostations.h).
    void onBtnClearMusic();
    void onWolfStationSet(S32 index);
    void onPresetCommit(S32 index);
    void onPresetSet(S32 index);
    void refreshStations();
    static void migrateSavedStreams();

private:
    LLCheckBoxCtrl* mCheckSoundLocal;
    LLCheckBoxCtrl* mCheckParcelEnableVoice;
    LLCheckBoxCtrl* mCheckEstateDisabledVoice;
    LLCheckBoxCtrl* mCheckParcelVoiceLocal;
    LLLineEditor* mMusicURLEdit;
    LLButton* mBtnStreamCopyToClipboard;
    LLButton* mBtnClearMusic;
    LLButton* mBtnRadioStations;
    static constexpr S32 ROWS = 10;
    LLButton* mWolfStationSet[ROWS];
    LLTextBox* mWolfStationName[ROWS];
    LLLineEditor* mPresetEdit[ROWS];
    LLButton* mPresetSet[ROWS];
    boost::signals2::scoped_connection mStationsChanged;
    LLCheckBoxCtrl* mCheckAVSoundAny;
    LLCheckBoxCtrl* mCheckAVSoundGroup;
    LLCheckBoxCtrl* mCheckObscureMOAP;

    LLSafeHandle<LLParcelSelection>&    mParcel;
};

#endif
