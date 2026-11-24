/**
 * @file wolffloaterrecorder.h
 * @brief WolfViewer: the Video Recorder window (floater_wolf_recorder.xml). The recording itself: wolfrecorder.h.
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

#ifndef WOLF_FLOATER_RECORDER_H
#define WOLF_FLOATER_RECORDER_H

#include "llfloater.h"
#include "llframetimer.h"
#include "llpointer.h"

class LLButton;
class LLCheckBoxCtrl;
class LLComboBox;
class LLScrollContainer;
class LLSliderCtrl;
class LLTextBox;
class LLViewerTexture;
namespace WolfRecorder { struct Report; }

// [W111 RECORDER UI 2026-11-24] Paul: "make the video recorder floater look like a proper video recording tool". Modelled
// on camcorders and OBS Studio: a dark monitor (live preview, REC light, running timecode), big round transport buttons
// (Record/Stop, Pause/Resume), a level meter beside each sound source. The monitor and the meters are drawn by the
// window itself (draw); the buttons are WolfRecorderTransportButton (wolffloaterrecorder.cpp, widget
// "wolf_recorder_transport_button"). While a recording of the whole window runs, the window is kept out of the video
// (WolfRecorder::holdBackFromVideo).
class WolfFloaterRecorder : public LLFloater
{
public:
    WolfFloaterRecorder(const LLSD& key);
    ~WolfFloaterRecorder() override;
    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;

    /** The folder chooser's answer (LLDirPickerThread, main thread); the window may have closed meanwhile. */
    static void onFolderPicked(const std::vector<std::string>& dirs, std::string proposed);

private:
    void onRecord();
    void onPause();
    void refresh() override;   // overrides LLFloater::refresh() (clang -Winconsistent-missing-override on macOS)
    void refreshStatic();      // what changes only on a setting change: notes, folder
    void refreshSpace(const WolfRecorder::Report& st);   // the free space and the time left (every 2 s)
    void updatePreview();
    void updateMeters();
    void drawMonitor();
    void drawMeter(int source, const LLRect& rect);
    void followKeyboardFocus();

    LLButton* mRecordBtn = nullptr;
    LLButton* mPauseBtn = nullptr;
    LLButton* mCodecBtn = nullptr;
    LLButton* mPlayBtn = nullptr;
    LLButton* mSavedFolderBtn = nullptr;
    LLTextBox* mStatus = nullptr;
    LLTextBox* mMessage = nullptr;
    LLTextBox* mCodecNote = nullptr;
    LLTextBox* mFolderText = nullptr;
    LLTextBox* mWorldNote = nullptr;
    LLTextBox* mVoiceNote = nullptr;
    LLTextBox* mSpaceNote = nullptr;
    LLComboBox* mSize = nullptr;
    LLComboBox* mFps = nullptr;
    LLComboBox* mLimitCombo = nullptr;
    LLView* mMonitor = nullptr;
    LLScrollContainer* mScroll = nullptr;   // [W111 RECORDER UI 2026-11-24] everything below the transport buttons
    LLView* mSettingsPanel = nullptr;       // (Paul: "make sure it scrolls")
    const void* mLastFocus = nullptr;       // the keyboard focus last seen (compared only, never used)
    LLView* mMeterViews[3] = {};
    F64 mNextStatic = 0.0;   // LLFrameTimer::getElapsedSeconds() time (F64: a float loses the seconds after a long session)
    F64 mNextSpace = 0.0;
    std::string mStartError;   // a start refused by WolfRecorder::start, shown until the next press
    std::string mSavedFile;    // the file Play opens (the last one saved)
    U64 mSavedBytes = 0;       // its size on disk

    // the monitor: what the last refresh found
    S32 mState = 0;            // WolfRecorder::State as a number
    bool mPaused = false, mSaved = false, mMicOn = false, mVoiceOn = false;
    F64 mSeconds = 0.0, mLimitSecs = 0.0;
    std::string mDetail;       // the line under the timecode

    // the preview picture
    LLPointer<LLViewerTexture> mPreviewTex;
    U32 mPreviewSerial = 0;
    std::vector<U8> mPreviewRGB;

    // the level meters, in dBFS (shown value and peak hold)
    F32 mMeterDb[3] = { -90.f, -90.f, -90.f };
    F32 mHoldDb[3] = { -90.f, -90.f, -90.f };
    F64 mHoldUntil[3] = {};
    LLFrameTimer mMeterTimer;
};

#endif // WOLF_FLOATER_RECORDER_H
