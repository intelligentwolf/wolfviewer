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

class LLButton;
class LLCheckBoxCtrl;
class LLComboBox;
class LLSliderCtrl;
class LLTextBox;

class WolfFloaterRecorder : public LLFloater
{
public:
    WolfFloaterRecorder(const LLSD& key);
    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

    /** The folder chooser's answer (LLDirPickerThread, main thread); the window may have closed meanwhile. */
    static void onFolderPicked(const std::vector<std::string>& dirs, std::string proposed);

private:
    void onRecord();
    void refresh() override;   // overrides LLFloater::refresh() (clang -Winconsistent-missing-override on macOS)
    void refreshStatic();      // what changes only on a setting change: notes, folder, free space

    LLButton* mRecordBtn = nullptr;
    LLButton* mCodecBtn = nullptr;
    LLTextBox* mTimer = nullptr;
    LLTextBox* mStatus = nullptr;
    LLTextBox* mMessage = nullptr;
    LLTextBox* mCodecNote = nullptr;
    LLTextBox* mFolderText = nullptr;
    LLTextBox* mWorldNote = nullptr;
    LLTextBox* mVoiceNote = nullptr;
    LLTextBox* mSpaceNote = nullptr;
    LLComboBox* mSize = nullptr;
    LLComboBox* mFps = nullptr;
    LLComboBox* mLimit = nullptr;
    LLCheckBoxCtrl* mWorld = nullptr;
    F32 mNextStatic = 0.f;
    std::string mStartError;   // a start refused by WolfRecorder::start, shown until the next press
};

#endif // WOLF_FLOATER_RECORDER_H
