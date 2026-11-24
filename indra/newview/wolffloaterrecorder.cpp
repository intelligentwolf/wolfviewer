/**
 * @file wolffloaterrecorder.cpp
 * @brief WolfViewer: the Video Recorder window. See wolffloaterrecorder.h and floater_wolf_recorder.xml.
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
#include "wolffloaterrecorder.h"

#include <filesystem>

#include "fsyspath.h"
#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "lldir.h"
#include "lldirpicker.h"
#include "llfloaterreg.h"
#include "llframetimer.h"
#include "llsliderctrl.h"
#include "lltextbox.h"
#include "lluicolortable.h"
#include "llviewercontrol.h"
#include "wolfrecorder.h"
#include "wolfvrstream.h"

namespace
{
    std::string clock(F64 secs)
    {
        const S32 s = (S32)secs;
        return llformat("%d:%02d", s / 60, s % 60);
    }

    std::string sizeText(U64 bytes)
    {
        if (bytes >= 1024ull * 1024 * 1024) return llformat("%.1f GB", (F64)bytes / (1024.0 * 1024.0 * 1024.0));
        return llformat("%.0f MB", (F64)bytes / (1024.0 * 1024.0));
    }

    // a combo's chosen value as a number; its items carry the setting's own numbers (floater_wolf_recorder.xml)
    void bindNumberCombo(LLComboBox* combo, const char* setting)
    {
        combo->setValue(LLSD(std::to_string(gSavedSettings.getS32(setting))));
        combo->setCommitCallback([setting](LLUICtrl* ctrl, const LLSD&) {
            gSavedSettings.setS32(setting, atoi(ctrl->getValue().asString().c_str()));
        });
    }
}

WolfFloaterRecorder::WolfFloaterRecorder(const LLSD& key) : LLFloater(key) {}

bool WolfFloaterRecorder::postBuild()
{
    mRecordBtn = getChild<LLButton>("record_btn");
    mCodecBtn = getChild<LLButton>("codec_btn");
    mTimer = getChild<LLTextBox>("timer");
    mStatus = getChild<LLTextBox>("status");
    mMessage = getChild<LLTextBox>("message");
    mCodecNote = getChild<LLTextBox>("codec_note");
    mFolderText = getChild<LLTextBox>("folder_text");
    mWorldNote = getChild<LLTextBox>("world_note");
    mVoiceNote = getChild<LLTextBox>("voice_note");
    mSpaceNote = getChild<LLTextBox>("space_note");
    mSize = getChild<LLComboBox>("size_combo");
    mFps = getChild<LLComboBox>("fps_combo");
    mLimit = getChild<LLComboBox>("limit_combo");
    mWorld = getChild<LLCheckBoxCtrl>("world_check");

    mRecordBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onRecord(); });
    getChild<LLButton>("open_folder_btn")->setCommitCallback([](LLUICtrl*, const LLSD&) { WolfRecorder::openFolder(); });
    getChild<LLButton>("change_folder_btn")->setCommitCallback([](LLUICtrl*, const LLSD&) {
        (new LLDirPickerThread(&WolfFloaterRecorder::onFolderPicked, WolfRecorder::folder()))->getFile();
    });
    mCodecBtn->setCommitCallback([](LLUICtrl*, const LLSD&) { WolfVRStream::downloadCodec(); });
    bindNumberCombo(mSize, "WolfRecorderSize");
    bindNumberCombo(mFps, "WolfRecorderFps");
    bindNumberCombo(mLimit, "WolfRecorderMaxMinutes");
    for (const char* name : { "world_check", "voice_check", "mic_check", "show_ui_check" })
    {
        getChild<LLUICtrl>(name)->setCommitCallback([this](LLUICtrl*, const LLSD&) { mNextStatic = 0.f; });
    }
    return true;
}

void WolfFloaterRecorder::onOpen(const LLSD& key)
{
    mNextStatic = 0.f;
    refresh();
}

// static
void WolfFloaterRecorder::onFolderPicked(const std::vector<std::string>& dirs, std::string proposed)
{
    if (dirs.empty() || dirs[0].empty()) return;
    gSavedSettings.setString("WolfRecorderFolder", dirs[0]);
    if (WolfFloaterRecorder* f = LLFloaterReg::findTypedInstance<WolfFloaterRecorder>("wolf_recorder"))
    {
        f->mNextStatic = 0.f;
    }
}

void WolfFloaterRecorder::onRecord()
{
    mStartError.clear();
    if (WolfRecorder::isRecording())
    {
        WolfRecorder::stop();
    }
    else
    {
        std::string error;
        if (!WolfRecorder::start(error)) mStartError = error;
    }
    refresh();
}

void WolfFloaterRecorder::draw()
{
    refresh();
    LLFloater::draw();
}

void WolfFloaterRecorder::refreshStatic()
{
    // world sound: available this session, or why not
    std::string world_note;
    if (WolfRecorder::worldSoundNeedsRestart()) world_note = getString("world_restart");
    else if (gSavedSettings.getBOOL("WolfRecorderSoundWorld") && !WolfRecorder::worldSoundAvailable())
    {
#if LL_OPENAL
        world_note = getString("world_unavailable");
#else
        world_note = getString("world_fmod");
#endif
    }
    mWorldNote->setText(world_note);
    // the privacy note only matters while voice is ticked
    mVoiceNote->setVisible(gSavedSettings.getBOOL("WolfRecorderSoundVoice"));

    const std::string folder = WolfRecorder::folder();
    mFolderText->setText(folder);
    mFolderText->setToolTip(folder);

    // free space, and roughly how long a recording at this size fits in it
    std::error_code ec;
    fsyspath probe(folder);
    while (!probe.empty() && !std::filesystem::exists(probe, ec) && probe.has_parent_path() && probe.parent_path() != probe)
    {
        probe = fsyspath(probe.parent_path());   // a folder not made yet: its nearest existing parent's disk
    }
    const std::filesystem::space_info si = std::filesystem::space(probe, ec);
    if (!ec)
    {
        const bool hd = gSavedSettings.getS32("WolfRecorderSize") >= 1080;
        const bool smooth = gSavedSettings.getS32("WolfRecorderFps") >= 60;
        const F64 bytes_per_min = ((hd ? 10000.0 : 6000.0) * (smooth ? 1.5 : 1.0) + 160.0) * 1000.0 / 8.0 * 60.0;
        LLStringUtil::format_map_t args;
        args["[FREE]"] = sizeText((U64)si.available);
        args["[MIN]"] = llformat("%.0f", (F64)si.available / bytes_per_min);
        mSpaceNote->setText(getString("space_free", args));
    }
    else
    {
        mSpaceNote->setText(LLStringUtil::null);
    }
    mNextStatic = LLFrameTimer::getElapsedSeconds() + 2.0;
}

void WolfFloaterRecorder::refresh()
{
    if (!mRecordBtn) return;
    const WolfRecorder::Report st = WolfRecorder::status();
    const bool busy = st.state != WolfRecorder::State::IDLE;
    const bool recording = st.state == WolfRecorder::State::STARTING || st.state == WolfRecorder::State::RECORDING;

    mRecordBtn->setLabel(getString(recording ? "stop_label" : (busy ? "saving_label" : "record_label")));
    mRecordBtn->setEnabled(st.state != WolfRecorder::State::FINISHING);
    mTimer->setText(recording ? clock(st.seconds) : (busy ? clock(st.seconds) : "0:00"));

    std::string status;
    switch (st.state)
    {
    case WolfRecorder::State::IDLE:
        if (st.saved && !st.file.empty())
        {
            LLStringUtil::format_map_t args;
            args["[FILE]"] = gDirUtilp->getBaseFileName(st.file);
            args["[SIZE]"] = sizeText(st.bytes);
            status = getString("saved_as", args);
        }
        else status = getString("ready");
        break;
    case WolfRecorder::State::STARTING:
        status = getString("starting");
        break;
    case WolfRecorder::State::RECORDING:
        if (st.limit_seconds - st.seconds <= 30.0)
        {
            LLStringUtil::format_map_t args;
            args["[END]"] = clock(st.limit_seconds);
            status = getString("near_end", args);
        }
        else
        {
            LLStringUtil::format_map_t args, dargs;
            args["[FPS]"] = llformat("%.0f", st.fps);
            dargs["[N]"] = llformat("%u", st.dropped);
            args["[DROPPED]"] = st.dropped ? getString("dropped", dargs) : std::string();
            status = getString("recording", args);
        }
        break;
    case WolfRecorder::State::FINISHING:
        status = getString("finishing");
        break;
    }
    mStatus->setText(status);

    // a problem (stays until the next press) or a note; "Problem:" in words as well as colour (WCAG 1.4.1)
    const std::string error = !mStartError.empty() ? mStartError : st.error;
    if (!error.empty())
    {
        mMessage->setText("Problem: " + error);
        mMessage->setColor(LLUIColorTable::instance().getColor("AlertCautionTextColor"));
    }
    else
    {
        mMessage->setText(st.notice == "Saved." ? std::string() : st.notice);
        mMessage->setColor(LLUIColorTable::instance().getColor("LabelTextColor"));
    }

    // the Cisco codec: offered only when there is something to download and the encoders found nothing else
    const int codec = WolfVRStream::codecState();
    const bool offer_codec = (codec == 0 || codec == 2) && !error.empty() && error.find("video encoder") != std::string::npos;
    mCodecBtn->setVisible(offer_codec);
    mCodecBtn->setEnabled(codec == 0);
    mCodecNote->setVisible(offer_codec);   // Cisco's binary licence, condition 3: the attribution where it is controlled
    if (offer_codec || codec == 2)
    {
        const std::string codec_msg = WolfVRStream::codecMessage();   // the download's own plain-words answer
        if (!codec_msg.empty()) mMessage->setText(mMessage->getText() + "\n" + codec_msg);
    }

    // settings are fixed for a recording (the levels stay live)
    for (const char* name : { "size_combo", "fps_combo", "show_ui_check", "world_check", "voice_check", "mic_check",
                              "change_folder_btn", "limit_combo" })
    {
        getChild<LLUICtrl>(name)->setEnabled(!busy);
    }
    if (!busy && LLFrameTimer::getElapsedSeconds() >= mNextStatic) refreshStatic();
}
