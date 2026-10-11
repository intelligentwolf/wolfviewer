/**
 * @file wolfspeech.h
 * @brief WolfViewer: speech to text (dictation) and text to speech (chat read aloud),
 *        on Wolf Territories' own servers, Wolf Territories only.
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

#ifndef WOLF_SPEECH_H
#define WOLF_SPEECH_H

#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "llsd.h"
#include "llsingleton.h"
#include "lluuid.h"

class LLChat;
class LLChatEntry;

// Source: wolfstorm/js/ui/speech_to_text.js SpeechToText and js/ui/text_to_speech.js
// TextToSpeech — the same gates, the same numbers, the same two server endpoints.
//
// DICTATION. The microphone is captured through OpenAL (alcCaptureOpenDevice, 16 kHz mono
// PCM16 — the rate whisper wants natively) and split into utterances by the same RMS voice
// detector WolfStorm uses: a block counts as speech above SPEECH_RMS, an utterance ends after
// SILENCE_HANGOVER_SECS of quiet or at MAX_UTTERANCE_SECS, shorter than MIN_UTTERANCE_SECS is
// discarded. The utterance is sent as a WAV to the WolfStorm proxy's POST /stt (a load balancer
// over whisper.cpp servers) and the text is put into the nearby chat bar and SENT — a pause is
// the send key, as in WolfStorm. AUDIO IS ONLY BUFFERED WHILE THE NEARBY CHAT BAR HAS FOCUS:
// that is the privacy gate, and the reason nothing is uploaded while you walk around or talk
// in voice. Focus is checked again when the transcription lands, because it is asynchronous.
//
// READ ALOUD. Nearby chat from others and incoming IMs are queued and spoken one at a time:
// each line goes to POST /tts (piper, en_GB "alan"), the WAV comes back, is written into the
// viewer's sound cache under a fresh UUID and triggered as a UI sound — the audio engine finds
// a decoded file in its cache and plays it without any asset fetch (llaudioengine.cpp
// LLAudioData::LLAudioData → hasDecodedFile). The voice is the same one WolfStorm uses, so a
// region sounds the same in both viewers, and nothing goes to a third party.
//
// Both are offered on Wolf Territories only (WolfGrid::isWolfTerritories): they send audio
// and chat to Wolf Territories' own servers, which is only right for that grid.
class WolfSpeech : public LLSingleton<WolfSpeech>
{
    LLSINGLETON(WolfSpeech);
    ~WolfSpeech();

public:
    /** On Wolf Territories, logged in. */
    static bool isAvailable();
    /** Built with OpenAL (the ReleaseOS builds are). */
    static bool canCapture();

    /** Every frame from LLAppViewer::idle(). */
    void idle();

    // Dictation (toolbar / menu toggle).
    void toggleDictation();
    bool isDictating() const { return mDictating; }

    // Read aloud (toolbar / menu toggle; the state is the setting WolfViewerSpeechReadAloud).
    void toggleReadAloud();
    static bool isReadingAloud();

    // <WolfViewer 2026-10-11> TOWER (Paul: "is there someway of having audio announcements from the tower?" ->
    // "we have text to speech could we use that" -> "IMPLEMENT IT default tower on"). The autopilot's tower calls and
    // height callouts (wolfflight.cpp towerSay / towerCallouts) through the same /tts voice, spoken whether or not
    // chat is read aloud, ahead of any chat waiting. Every phrase synthesised for the tower is kept (mTowerCache)
    // so a repeat plays at once; the height callouts are fetched as the approach starts (prefetch), so "fifty ...
    // thirty ... ten" are not a second late. Setting WolfTowerVoice (default on); Wolf Territories only, as all of
    // this class.
    static bool towerOn();
    /** Say a tower line: next after the line playing now; a cached phrase plays without a fetch. */
    void announce(const std::string& text);
    /** Synthesise a tower phrase into the cache without playing it. */
    void prefetch(const std::string& text);
    // </WolfViewer>

    /** FSFloaterNearbyChat::addMessage — a nearby chat line as it is displayed. */
    void onNearbyChat(const LLChat& chat);
    /** LLIMModel::processAddingMessage — an instant message as it is displayed. */
    void onInstantMessage(const LLUUID& session_id, const std::string& from, const LLUUID& from_id, const std::string& text);

    // Source: speech_to_text.js — static constants, same values.
    static constexpr U32 TARGET_RATE = 16000;
    /** One ScriptProcessorNode buffer there: the unit the voice decision is made on. */
    static constexpr S32 BLOCK_SAMPLES = 4096;
    static constexpr F32 SPEECH_RMS = 0.015f;
    static constexpr F32 SILENCE_HANGOVER_SECS = 0.7f;
    static constexpr F32 MIN_UTTERANCE_SECS = 0.35f;
    static constexpr F32 MAX_UTTERANCE_SECS = 15.f;
    static constexpr S32 MAX_INFLIGHT = 2;
    /** Raw OpenAL capture has none of the browser's noise suppression or automatic gain, so
     *  the fixed RMS threshold alone is not enough: a quiet room on an ordinary USB mic
     *  measured 0.020 idle on 2026-09-06, above SPEECH_RMS, and every pause looked like speech.
     *  Speech is therefore ALSO required to rise NOISE_RATIO above a running estimate of the
     *  idle level (drops immediately, climbs slowly so speech itself cannot lift it). */
    static constexpr F32 NOISE_RATIO = 3.0f;
    static constexpr F32 NOISE_FLOOR_RISE = 0.02f;
    /** MEASURED 2026-09-06 (Paul's USB mic, WolfViewer.log): idle level 0.0010–0.0016 and
     *  speech blocks mostly UNDER SPEECH_RMS, so only the first block of every sentence
     *  counted and each utterance was cut at 1.024 s. The browser's automatic gain is what
     *  lets WolfStorm use 0.015; raw capture uses this lower floor with the relative test
     *  above, and the utterance is gain-normalised before upload (encodeWav). */
    static constexpr F32 SPEECH_RMS_MIN = 0.004f;
    static constexpr F32 GAIN_TARGET_PEAK = 0.9f;
    static constexpr F32 GAIN_MAX = 8.f;
    // Source: text_to_speech.js
    static constexpr S32 MAX_QUEUE = 6;
    static constexpr size_t MAX_CHARS = 400;

private:
    // dictation
    bool openCapture();
    void closeCapture();
    void pollCapture();
    void processBlock(const S16* samples, S32 count);
    void resetUtterance();
    void flushUtterance();
    static std::vector<U8> encodeWav(const std::vector<S16>& pcm);
    static void sttCoro(std::vector<U8> wav);
    void onTranscribed(bool ok, S32 status, const std::string& text_or_error);
    /** The nearby chat input that has keyboard focus: the bottom chat bar (FSNearbyChatControl,
     *  tracked by FSNearbyChat) or the nearby chat floater's own box (a plain LLChatEntry,
     *  fsfloaternearbychat.cpp:149 "chat_box"). Null when neither has it. */
    static LLChatEntry* focusedChatBox();
    void insertAndSend(const std::string& text);

    // read aloud
    void enqueue(const std::string& line);
    void pumpSpeech();
    // <WolfViewer 2026-10-11> what a synthesis is for: chat read aloud, a tower line to play, a tower phrase to cache
    enum ESynth { SYNTH_CHAT = 0, SYNTH_TOWER, SYNTH_PREFETCH };
    static void ttsCoro(std::string text, S32 kind);
    void onSynthesised(bool ok, S32 status, const LLSD::Binary& wav, const std::string& error, const std::string& text, S32 kind);
    void playSound(const LLUUID& id, F32 seconds);
    static std::string speakable(const std::string& line);
    static F32 wavSeconds(const LLSD::Binary& wav);

    // feedback
    void reportOnce(const std::string& key, const std::string& message);
    static void report(const std::string& message);
    static void tip(const std::string& message);

    // capture state (the device is an ALCdevice*, kept untyped so this header needs no AL)
    void*            mCaptureDevice = nullptr;
    bool             mDictating = false;
    std::vector<S16> mPending;      // captured, not yet a whole block
    std::vector<S16> mUtterance;    // the utterance being accumulated
    bool             mSpeaking = false;
    F64              mLastVoiceAt = 0.0;
    F32              mNoiseFloor = 0.f;
    S32              mInflight = 0;
    std::string      mReportedKey;

    // read-aloud state
    std::deque<std::string> mQueue;
    bool                    mSynthesising = false;
    F64                     mSpeakingUntil = 0.0;
    struct Played
    {
        std::string mPath;
        F64         mDeleteAt;
        LLUUID      mId;        // <WolfViewer 2026-09-26/> the engine's LLAudioData key, dropped with the file
    };
    std::vector<Played>     mPlayed;
    // <WolfViewer 2026-10-11> tower: lines waiting, and every tower phrase synthesised this run (kept on disk
    // until the viewer quits; a handful of short WAVs)
    std::deque<std::string> mTowerQueue;
    struct Cached
    {
        LLUUID      mId;
        std::string mPath;
        F32         mSeconds = 0.f;
    };
    std::map<std::string, Cached> mTowerCache;
    std::set<std::string>   mPrefetching;
    // </WolfViewer>
};

#endif // WOLF_SPEECH_H
