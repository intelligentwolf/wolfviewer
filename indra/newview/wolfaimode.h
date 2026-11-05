/**
 * @file wolfaimode.h
 * @brief WolfViewer: AI Mode - the resident's own avatar gets an AI personality that hears and speaks over normal voice.
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

#ifndef WOLF_AIMODE_H
#define WOLF_AIMODE_H

#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "llsd.h"
#include "llsingleton.h"
#include "lluuid.h"

class LLChat;
class LLVOAvatar;

// [AI MODE 2026-11-05] Paul (owner), after standing beside his own avatar in VR: "what if you can just make your avatar AI so
// if i'm on voice they hear me but they might hear lone as well? could he speak and hear over normal voice?" and "just a menu
// item to turn him on and off is fine". Wolf World > AI Mode (Wolf Territories only, gone elsewhere).
//
// HEARS: the resident's microphone as it goes into voice (llwebrtc capture tap - only while the mic is open, so only what the
// resident says out loud to the world), the voices the viewer plays (llwebrtc listen tap; who spoke is the voice participant
// who was speaking most during the utterance), and local chat from avatars. Speech is cut into utterances by WolfSpeech's
// detector numbers and turned into words by the proxy's /stt (whisper). AI: php/aimode.php "reply" (WolfNPC's brain, charged
// to the resident's Wolf Credits at the AI's real cost + 10%). SPEAKS: piper (the chosen voice, made by the brain) into the
// resident's OWN outgoing voice (llwebrtc LLWebRTCSpeechSource: the microphone is held while he talks, and the voice path is
// kept open for him even when the resident is muted - LLWebRTCVoiceClient::wolfSetSpeechOpen), played for the resident at
// his own ears (the same loudness wherever he is driven), and written in local chat as "(AI) ..." so people without voice, or who cannot hear, read it.
// TURNS: answers when named (the avatar's first name), when the resident talks and nobody else is near or they are already
// talking, or when someone he just answered goes on; joins in on his own at most once in CHIME_EVERY_SECS after a natural
// gap; never while anyone is speaking; one request at a time, at most MAX_PER_MINUTE replies a minute.
// HONEST: the region marks the avatar (the wolf-aitag plugin sets its name-tag title to TAG_TITLE, seen in every viewer;
// WolfViewer draws "<Name> (AI)"), the first time he speaks to someone he says he is <Name>'s AI (the brain), every line is
// in chat as "(AI) ...". OFF stops everything: taps and speech source removed, nothing sent, answers still on their way
// dropped, the tag cleared.
// NEVER MOVES THE AVATAR: the resident drives him (Paul 2026-11-05: "he doesnt need to follow me in vr because i'll just
// drive him around ... he is my avatar").
class WolfAIMode : public LLSingleton<WolfAIMode>
{
    LLSINGLETON(WolfAIMode);
    ~WolfAIMode();

public:
    /** The name-tag title the wolf-aitag region plugin gives an avatar in AI Mode (region-terrain-work/ai-mode-2026-11-05/
     *  aitag/src/WolfAITagPlugin.cs TITLE). LLVOAvatar draws "<name> (AI)" for it. */
    static constexpr const char* TAG_TITLE = "AI Mode";
    /** The channel the viewer tells the region on (the plugin's CHANNEL); negative, so it goes as ScriptDialogReply. */
    static constexpr S32 TAG_CHANNEL = -7461350;

    /** One ear (the resident's microphone, or the voices played): see wolfaimode.cpp. */
    struct Ear;

    static bool isAvailable();
    bool isOn() const { return mOn; }
    /** The menu item: on asks the brain (open) and turns on, or opens the setup window the first time; off stops all. */
    void toggle();
    void turnOff(const std::string& why = std::string());
    /** The setup window saved: turn on now. */
    void turnOnAfterSetup();

    /** Every frame from LLAppViewer::idle(). */
    void idle();
    /** Before voice goes at quit (llappviewer.cpp, next to WolfVRAudio::shutdown). */
    void shutdown();
    /** FSFloaterNearbyChat::addMessage, a live line (as WolfSpeech::onNearbyChat). */
    void onNearbyChat(const LLChat& chat);

    /** The brain's POST (php/aimode.php): done(ok, reply, error, code) on the main thread. */
    typedef std::function<void(bool, const LLSD&, const std::string&, const std::string&)> api_callback_t;
    static void api(const std::string& action, const LLSD& body, api_callback_t done);

    // Turn-taking numbers (see the class comment). Chosen for natural conversation, not taken from another system.
    static constexpr F32 MIN_GAP_SECS = 3.f;           // after he finishes, before he may start again
    static constexpr S32 MAX_PER_MINUTE = 8;
    static constexpr F32 QUIET_BEFORE_SPEAKING = 0.8f; // nobody speaking for this long before he starts
    static constexpr F32 GIVE_UP_WAITING = 12.f;       // a reply that cannot find a quiet moment in this long is dropped
    static constexpr F32 CONVERSATION_SECS = 45.f;     // the resident still talking with him
    static constexpr F32 FOLLOW_ON_SECS = 30.f;        // someone he just answered, going on
    static constexpr F32 CHIME_EVERY_SECS = 180.f;
    static constexpr F32 CHIME_GAP_SECS = 4.f;
    static constexpr F32 EARSHOT_M = 20.f;             // OpenSim ChatModule m_saydistance default (ChatModule.cs:51 = 20)
    static constexpr size_t MAX_HEARD = 6;             // aimode_lib.php AIM_MAX_HEARD
    static constexpr size_t MAX_HISTORY = 12;          // aimode_lib.php AIM_MAX_HISTORY

private:
    struct Line
    {
        LLUUID      mWho;
        std::string mName;
        std::string mText;
        std::string mVia;   // voice | mic | chat
        bool        mAddressed = false;
        F64         mAt = 0.0;
    };
    void turnOn();
    void hookVoice(bool on);
    void sendTag(bool on);
    void pumpEars();
    void pumpEar(Ear& ear, bool mic, bool got);
    void endUtterance(Ear& ear, bool mic);
    void heard(Line line);
    bool addressedToMe(const Line& l) const;
    void maybeAsk();
    void ask(bool addressed);
    void onReply(U32 gen, bool ok, const LLSD& reply, const std::string& error, const std::string& code);
    void onVoiceClip(U32 gen, const std::string& say, const std::vector<U8>& wav);
    void pumpSpeaking();
    void startSpeaking();
    void finishSpeaking();
    bool someoneSpeaking() const;
    std::string myFirstName() const;
    static std::string cleanHeard(const std::string& text);

    bool mOn = false;
    bool mOpening = false;          // an `open` on its way
    U32  mGen = 0;                  // bumped by every on/off: answers of an older generation are dropped
    std::string mFirst;             // the avatar's first name (what people call him)
    std::deque<Line> mHeard;        // heard since the last request went
    std::deque<LLSD> mHistory;      // the conversation so far {me, name, text}
    bool mAsking = false;
    S32  mSttInflight = 0;
    F64  mLastReplyAt = -1e9;       // when he last finished speaking
    F64  mLastChimeAt = -1e9;
    LLUUID mLastPartner;            // who he answered last
    std::deque<F64> mReplyTimes;    // starts of his last replies (MAX_PER_MINUTE)
    S32  mUnaddressedSince = 0;

    // speaking
    enum class Say { IDLE, WAITING, OPENING, PLAYING, CLOSING };
    Say  mSay = Say::IDLE;
    std::string mSayText;
    std::vector<S16> mSayPcm;       // 48 kHz mono
    F32  mSaySeconds = 0.f;
    std::vector<U8> mSayWav;        // the clip as fetched, for the resident's own playback
    F64  mSayReadyAt = 0.0;
    F64  mSayPhaseAt = 0.0;
    bool mVoiceOpened = false;      // wolfSetSpeechOpen(true) was asked
    F32  mServerPeak = 0.f;         // the voice server's peak measure of our sound while he spoke (the log's proof)
    std::string mLocalPath;         // the sound cache file playing for the resident
    struct Played { LLUUID mId; F64 mAt = 0.0; };
    std::vector<std::pair<std::string, Played>> mToDelete;   // the sound cache file, its audio entry, when to drop them

    // hooks and tag
    F64  mHookRefresh = 0.0;
    void* mHookedDev = nullptr;
    F64  mTagCheckAt = 0.0;

    F64  mStatAt = 0.0;
};

#endif // WOLF_AIMODE_H
