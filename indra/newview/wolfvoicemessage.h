/**
 * @file wolfvoicemessage.h
 * @brief WolfViewer: voice messages in one-to-one instant messages (ticket 631).
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

#ifndef WOLF_VOICEMESSAGE_H
#define WOLF_VOICEMESSAGE_H

#include <set>
#include <string>
#include <vector>

#include <boost/signals2.hpp>

#include "llsingleton.h"
#include "lluuid.h"

// [VOICE MESSAGES 2026-11-10] Ticket 631, Jimmy Olsen: "Add a button for short voice messages? (maybe up to 1min)". Paul
// approved. Design: firestorm/region-terrain-work/im-translate-voice-2026-11-10/README.md.
//
// RECORD: the Voice button in a one-to-one IM window (fsfloaterim.cpp). The microphone is opened through OpenAL exactly as
// dictation opens it (wolfspeech.cpp openCapture: 16 kHz mono PCM16) only while the button is pressed in, the button then
// reads "Send 0:07" beside a Cancel button; Send (or 1:00) uploads the WAV (WolfSpeech::wav16k) to php/voicemsg.php
// action=send with the session pair, and the line the server answers with is sent as the IM.
// PLAY: an IM line made by the server (vm_im_text, matched exactly by LINE_PATTERN) is shown in a one-to-one conversation
// as "🎤 Voice message (0:12) ▶ Play", the play link being secondlife:///app/wolfvoicemsg/play/<id> (click only). The clip
// is fetched with the session pair (only the sender and the recipient are given it), written into the sound cache and
// played as a UI sound at the listener (wolfspeech.cpp playSound), and the file is deleted afterwards.
// Wolf Territories logins only (the server checks the session with Wolf presence).
class WolfVoiceMessage : public LLSingleton<WolfVoiceMessage>
{
    LLSINGLETON(WolfVoiceMessage);
    ~WolfVoiceMessage();

public:
    // Source: wolfstorm/php/voicemsg_lib.php VM_MIN_SECONDS / VM_MAX_SECONDS.
    static constexpr F32 MIN_SECONDS = 1.f;
    static constexpr F32 MAX_SECONDS = 60.f;

    /** Logged in to Wolf Territories, with a viewer built with OpenAL. */
    static bool isAvailable();

    enum EState { IDLE = 0, RECORDING, SENDING };
    EState getState() const { return mState; }
    /** The conversation being recorded for or sent to (null when IDLE). */
    const LLUUID& getSessionID() const { return mSessionID; }
    F32 recordedSeconds() const;

    /** Start recording for this one-to-one conversation; false (and a visible message) if it cannot. */
    bool startRecording(const LLUUID& session_id, const LLUUID& other_id);
    /** Stop and upload; the IM is sent when the server has stored it. */
    void stopAndSend();
    /** Stop and throw the recording away. */
    void cancel();

    /** Fetch and play a clip (the secondlife:///app/wolfvoicemsg/play/<id> link). */
    void play(const std::string& id);

    /** Every frame from LLAppViewer::idle() (once the singleton exists). */
    void idle();

    /** True for the exact line the server makes (vm_im_text); the id and length text are returned when asked. */
    static bool isVoiceMessageLine(const std::string& text, std::string* id = nullptr, std::string* length = nullptr);
    /** The line as a one-to-one conversation shows it: the Play link in place of the web link. Others are unchanged. */
    static std::string displayText(const std::string& text);

    typedef boost::signals2::signal<void()> state_signal_t;
    boost::signals2::connection setStateChangedCallback(const state_signal_t::slot_type& cb) { return mStateChanged.connect(cb); }

private:
    bool openCapture();
    void closeCapture();
    void pollCapture();
    void setState(EState state);
    void onSent(bool ok, const std::string& text, const std::string& error);
    void onFetched(const std::string& id, bool ok, const std::vector<U8>& wav, const std::string& error);
    static void report(const std::string& message);
    static void tip(const std::string& message);

    EState           mState = IDLE;
    LLUUID           mSessionID;
    LLUUID           mOtherID;
    void*            mCaptureDevice = nullptr;   // ALCdevice*, untyped so this header needs no AL (wolfspeech.h)
    std::vector<S16> mPcm;
    std::set<std::string> mFetching;
    struct Played
    {
        std::string mPath;
        LLUUID      mId;
        F64         mDeleteAt = 0.0;
    };
    std::vector<Played> mPlayed;
    state_signal_t   mStateChanged;
};

#endif // WOLF_VOICEMESSAGE_H
