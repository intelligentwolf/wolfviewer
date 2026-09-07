/**
 * @file wolfspeech.cpp
 * @brief WolfViewer: speech to text (dictation) and text to speech (chat read aloud).
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

#include "wolfspeech.h"

#include <boost/json.hpp>
#include <cmath>
#include <cstring>

#if LL_OPENAL
#include "AL/al.h"
#include "AL/alc.h"
#endif

#include "fsfloaternearbychat.h"
#include "fsnearbychatcontrol.h"
#include "fsnearbychathub.h"
#include "llchatentry.h"
#include "llagentdata.h"
#include "llaudioengine.h"
#include "llchat.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "lldir.h"
#include "llfile.h"
#include "llframetimer.h"
#include "llhttpconstants.h"
#include "llimview.h"
#include "llinstantmessage.h"
#include "llnotificationsutil.h"
#include "llsdjson.h"
#include "llviewercontrol.h"
#include "wolfgrid.h"

// Source: wolfstorm/js/ui/speech_to_text.js, js/ui/text_to_speech.js.

namespace
{
    /**
     * Inside a coroutine: POST a raw body and return the raw reply plus the HTTP status code.
     * Source: llcorehttputil.cpp trivialPostCoro; fsdata.cpp for HTTP_RESULTS_RAW.
     */
    S32 postRaw(const std::string& url, const std::string& content_type, const std::vector<U8>& body,
                LLSD::Binary& reply, std::string& error)
    {
        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
            std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfSpeech", LLCore::HttpRequest::DEFAULT_POLICY_ID);
        LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
        LLCore::HttpOptions::ptr_t options = std::make_shared<LLCore::HttpOptions>();
        options->setTimeout(40);   // whisper's own budget is 30 s (main.rs STT_TIMEOUT_SECS)
        LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
        headers->append(HTTP_OUT_HEADER_CONTENT_TYPE, content_type);
        // [SPEECH-AUTH 2026-09-07] The proxy serves Wolf Territories sessions only: it checks
        // this pair against the grid's presence service (rust_proxy speech_authorise), which
        // is what makes the grid gate real rather than a courtesy in public source.
        headers->append("X-Wolf-Agent", gAgentID.asString());
        headers->append("X-Wolf-Session", gAgentSessionID.asString());
        LLCore::BufferArray::ptr_t raw(new LLCore::BufferArray());   // intrusive pointer (fsprimfeedconnect.cpp:72)
        raw->append(body.data(), body.size());

        LLSD result = adapter->postRawAndSuspend(request, url, raw, options, headers);
        LLSD httpResults = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
        LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(httpResults);
        reply.clear();
        if (result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
        {
            reply = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
        }
        error = status ? std::string() : status.toString();
        return status.getType();
    }

    /** The "error" (or "text") field of a JSON reply, if it is one. */
    std::string jsonField(const LLSD::Binary& bytes, const char* field)
    {
        std::string text(bytes.begin(), bytes.end());
        boost::system::error_code ec;
        boost::json::value v = boost::json::parse(text, ec);
        if (ec || !v.is_object())
        {
            return std::string();
        }
        LLSD sd = LlsdFromJson(v);
        return sd.has(field) ? sd[field].asString() : std::string();
    }
}

WolfSpeech::WolfSpeech()
{
}

WolfSpeech::~WolfSpeech()
{
    closeCapture();
    for (const Played& p : mPlayed)
    {
        LLFile::remove(p.mPath);
    }
    mPlayed.clear();
}

// static
bool WolfSpeech::isAvailable()
{
    return WolfGrid::isWolfTerritories() && gAgentID.notNull();
}

// static
bool WolfSpeech::canCapture()
{
#if LL_OPENAL
    return true;
#else
    return false;
#endif
}

void WolfSpeech::idle()
{
    if (mDictating)
    {
        pollCapture();
    }
    pumpSpeech();
    if (!mPlayed.empty())
    {
        const F64 now = LLFrameTimer::getElapsedSeconds();
        for (auto it = mPlayed.begin(); it != mPlayed.end();)
        {
            if (now >= it->mDeleteAt)
            {
                LLFile::remove(it->mPath);
                it = mPlayed.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
}

// ───────────────────────────── dictation ─────────────────────────────

// Source: speech_to_text.js toggle() / start() / stop().
void WolfSpeech::toggleDictation()
{
    if (mDictating)
    {
        closeCapture();
        mDictating = false;
        resetUtterance();
        tip("Dictation off.");
        return;
    }
    if (!isAvailable())
    {
        report("Dictation is available on Wolf Territories only.");
        return;
    }
    if (!canCapture())
    {
        report("This build of the viewer cannot capture audio (no OpenAL), so dictation is unavailable.");
        return;
    }
    if (!openCapture())
    {
        report("Could not open the microphone for dictation.");
        return;
    }
    mDictating = true;
    mReportedKey.clear();
    mNoiseFloor = 0.f;
    LL_INFOS("WolfSpeech") << "dictation on" << LL_ENDL;
    tip("Dictation on: click into the nearby chat bar and speak. A pause sends the line. "
        "Audio is only captured while the chat bar has focus, and only Wolf Territories' own "
        "server hears it.");
}

bool WolfSpeech::openCapture()
{
#if LL_OPENAL
    if (mCaptureDevice)
    {
        return true;
    }
    // 16 kHz mono PCM16, the format whisper wants natively (speech_to_text.js TARGET_RATE); a
    // two-second ring is far more than one frame's worth at any frame rate.
    ALCdevice* dev = alcCaptureOpenDevice(NULL, TARGET_RATE, AL_FORMAT_MONO16, TARGET_RATE * 2);
    if (!dev)
    {
        LL_WARNS("WolfSpeech") << "alcCaptureOpenDevice failed" << LL_ENDL;
        return false;
    }
    alcCaptureStart(dev);
    mCaptureDevice = dev;
    mPending.clear();
    return true;
#else
    return false;
#endif
}

void WolfSpeech::closeCapture()
{
#if LL_OPENAL
    if (mCaptureDevice)
    {
        ALCdevice* dev = static_cast<ALCdevice*>(mCaptureDevice);
        alcCaptureStop(dev);
        alcCaptureCloseDevice(dev);
        mCaptureDevice = nullptr;
    }
#endif
    mPending.clear();
}

void WolfSpeech::pollCapture()
{
#if LL_OPENAL
    if (!mCaptureDevice)
    {
        return;
    }
    ALCdevice* dev = static_cast<ALCdevice*>(mCaptureDevice);
    ALCint available = 0;
    alcGetIntegerv(dev, ALC_CAPTURE_SAMPLES, 1, &available);
    if (available > 0)
    {
        const size_t old = mPending.size();
        mPending.resize(old + available);
        alcCaptureSamples(dev, mPending.data() + old, available);
    }
    // The voice decision is made per BLOCK_SAMPLES, the size of the buffer the browser hands
    // WolfStorm (256 ms at 16 kHz), so the two behave the same.
    while (mPending.size() >= (size_t)BLOCK_SAMPLES)
    {
        processBlock(mPending.data(), BLOCK_SAMPLES);
        mPending.erase(mPending.begin(), mPending.begin() + BLOCK_SAMPLES);
    }
#endif
}

// Source: speech_to_text.js _onAudio(ev) — one capture buffer: decide speech vs silence,
// accumulate, and flush on the pause. Only buffers while the chat bar has focus.
void WolfSpeech::processBlock(const S16* samples, S32 count)
{
    if (!focusedChatBox())
    {
        // Focus lost: discard a part-spoken utterance rather than holding it. Buffering would
        // mean a sentence spoken while looking at the world lands in chat the moment the bar is
        // next clicked — exactly the surprise this must not produce.
        if (mSpeaking)
        {
            resetUtterance();
        }
        return;
    }
    F64 sum = 0.0;
    for (S32 i = 0; i < count; ++i)
    {
        const F64 x = samples[i] / 32768.0;
        sum += x * x;
    }
    const F32 rms = (F32)sqrt(sum / (F64)count);
    const F64 now = LLFrameTimer::getElapsedSeconds();
    // Idle-level estimate (see NOISE_RATIO in the header): the first block seeds it, a
    // quieter block replaces it at once, a louder one nudges it up by NOISE_FLOOR_RISE.
    if (mNoiseFloor <= 0.f || rms < mNoiseFloor)
    {
        mNoiseFloor = rms;
    }
    else
    {
        mNoiseFloor += (rms - mNoiseFloor) * NOISE_FLOOR_RISE;
    }
    const F32 threshold = llmax(SPEECH_RMS_MIN, mNoiseFloor * NOISE_RATIO);
    if (rms >= threshold)
    {
        mSpeaking = true;
        mLastVoiceAt = now;
    }
    if (!mSpeaking)
    {
        return;      // still waiting for speech to begin
    }
    mUtterance.insert(mUtterance.end(), samples, samples + count);
    const F32 duration = (F32)mUtterance.size() / (F32)TARGET_RATE;
    const F32 silence = (F32)(now - mLastVoiceAt);
    if (silence >= SILENCE_HANGOVER_SECS || duration >= MAX_UTTERANCE_SECS)
    {
        if (duration >= MIN_UTTERANCE_SECS)
        {
            flushUtterance();
        }
        else
        {
            LL_INFOS("WolfSpeech") << "dictation: discarded " << duration << " s utterance (below "
                                   << MIN_UTTERANCE_SECS << " s)" << LL_ENDL;
        }
        resetUtterance();
    }
}

// Source: speech_to_text.js _resetUtterance()
void WolfSpeech::resetUtterance()
{
    mUtterance.clear();
    mSpeaking = false;
    mLastVoiceAt = 0.0;
}

// Source: speech_to_text.js _flush() — bound the concurrency: whisper is CPU-bound and the
// proxy answers 503 when its pool is saturated, so piling requests on achieves nothing but a
// queue of stale sentences.
void WolfSpeech::flushUtterance()
{
    if (mInflight >= MAX_INFLIGHT)
    {
        LL_INFOS("WolfSpeech") << "dictation: " << MAX_INFLIGHT << " transcriptions already in flight, utterance dropped" << LL_ENDL;
        return;
    }
    ++mInflight;
    std::vector<U8> wav = encodeWav(mUtterance);
    LL_INFOS("WolfSpeech") << "dictation: sending " << ((F32)mUtterance.size() / (F32)TARGET_RATE)
                           << " s utterance (" << wav.size() << " bytes), idle level " << mNoiseFloor
                           << ", threshold " << llmax(SPEECH_RMS_MIN, mNoiseFloor * NOISE_RATIO) << LL_ENDL;
    LLCoros::instance().launch("WolfSpeech stt", [wav]() { sttCoro(wav); });
}

// Source: speech_to_text.js _encodeWav(): canonical 44-byte RIFF/WAVE header, PCM, mono,
// 16-bit, little-endian; the samples are already 16 kHz PCM16 here so no resampling.
std::vector<U8> WolfSpeech::encodeWav(const std::vector<S16>& pcm)
{
    const U32 n = (U32)pcm.size();
    const U32 data_bytes = n * 2;
    std::vector<U8> out(44 + data_bytes);
    auto put32 = [&out](size_t o, U32 v) { out[o] = v & 0xff; out[o + 1] = (v >> 8) & 0xff; out[o + 2] = (v >> 16) & 0xff; out[o + 3] = (v >> 24) & 0xff; };
    auto put16 = [&out](size_t o, U16 v) { out[o] = v & 0xff; out[o + 1] = (v >> 8) & 0xff; };
    memcpy(&out[0], "RIFF", 4);
    put32(4, 36 + data_bytes);
    memcpy(&out[8], "WAVE", 4);
    memcpy(&out[12], "fmt ", 4);
    put32(16, 16);                     // fmt chunk size
    put16(20, 1);                      // format = PCM
    put16(22, 1);                      // channels = mono
    put32(24, TARGET_RATE);            // sample rate
    put32(28, TARGET_RATE * 2);        // byte rate = rate * channels * bytesPerSample
    put16(32, 2);                      // block align
    put16(34, 16);                     // bits per sample
    memcpy(&out[36], "data", 4);
    put32(40, data_bytes);
    // Per-utterance gain (the browser's autoGainControl equivalent): raise the peak to
    // GAIN_TARGET_PEAK, at most GAIN_MAX, so a quiet mic still gives whisper a full signal.
    S32 peak = 1;
    for (U32 i = 0; i < n; ++i)
    {
        const S32 a = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (a > peak) peak = a;
    }
    const F32 gain = llmin(GAIN_MAX, llmax(1.f, GAIN_TARGET_PEAK * 32767.f / (F32)peak));
    for (U32 i = 0; i < n; ++i)
    {
        const S32 v = llclamp((S32)(pcm[i] * gain), -32768, 32767);
        put16(44 + i * 2, (U16)(S16)v);
    }
    return out;
}

// static
void WolfSpeech::sttCoro(std::vector<U8> wav)
{
    // Source: speech_to_text.js _flush(): POST audio/wav to STT_URL; reply {success, text|error}.
    LLSD::Binary reply;
    std::string error;
    const S32 status = postRaw(std::string(WolfGrid::SPEECH_API_BASE) + "/stt", "audio/wav", wav, reply, error);
    if (status >= 200 && status < 300)
    {
        const std::string text = jsonField(reply, "text");
        const std::string err = jsonField(reply, "error");
        if (!err.empty())
        {
            WolfSpeech::instance().onTranscribed(false, status, err);
        }
        else
        {
            WolfSpeech::instance().onTranscribed(true, status, text);
        }
    }
    else
    {
        std::string msg = jsonField(reply, "error");
        if (msg.empty())
        {
            msg = "speech service returned HTTP " + std::to_string(status) + (error.empty() ? "" : " (" + error + ")");
        }
        WolfSpeech::instance().onTranscribed(false, status, msg);
    }
}

void WolfSpeech::onTranscribed(bool ok, S32 status, const std::string& text_or_error)
{
    if (mInflight > 0)
    {
        --mInflight;
    }
    if (!ok)
    {
        // 503 is "busy, try again" and must NOT switch dictation off or latch an error;
        // anything else is a real fault worth reporting once.
        LL_WARNS("WolfSpeech") << "dictation: transcription failed, HTTP " << status << ": " << text_or_error << LL_ENDL;
        if (status == 503)
        {
            tip(text_or_error);
        }
        else
        {
            reportOnce("stt-" + std::to_string(status), "Dictation: " + text_or_error);
        }
        return;
    }
    std::string text = text_or_error;
    LLStringUtil::trim(text);
    LL_INFOS("WolfSpeech") << "dictation: transcribed \"" << text << "\"" << LL_ENDL;
    if (text.empty())
    {
        return;
    }
    insertAndSend(text);
}

// The nearby chat input that currently has keyboard focus — the whole gate. Two widgets can
// be it: the bottom chat bar, an FSNearbyChatControl that registers itself with FSNearbyChat
// on focus (fsnearbychatcontrol.cpp onFocusReceived), and the nearby chat FLOATER's box,
// which is a plain LLChatEntry ("chat_box", fsfloaternearbychat.cpp:149) the hub never sees.
LLChatEntry* WolfSpeech::focusedChatBox()
{
    FSNearbyChatControl* bar = FSNearbyChat::instance().mFocusedInputEditor;
    if (bar && bar->hasFocus())
    {
        return bar;
    }
    FSFloaterNearbyChat* floater = FSFloaterNearbyChat::findInstance();
    if (floater)
    {
        LLChatEntry* box = floater->getChatBox();
        if (box && box->hasFocus())
        {
            return box;
        }
    }
    return nullptr;
}

// Source: speech_to_text.js _insertAndSend() / _send(): put the text in the box and send it —
// but only if that box STILL has focus — the same way pressing Enter does, so dictated chat is
// indistinguishable from typed chat (FSNearbyChat::sendChat is the chat bar's commit path).
void WolfSpeech::insertAndSend(const std::string& text)
{
    LLChatEntry* box = focusedChatBox();
    if (!box)
    {
        LL_INFOS("WolfSpeech") << "dictation: chat box lost focus before the text landed, line dropped" << LL_ENDL;
        return;
    }
    std::string existing = box->getText();
    std::string combined;
    if (!existing.empty() && existing.back() != ' ')
    {
        combined = existing + " " + text;
    }
    else
    {
        combined = existing + text;
    }
    box->setText(LLStringExplicit(""));
    FSNearbyChat::instance().sendChat(utf8str_to_wstring(combined), CHAT_TYPE_NORMAL);
}

// ───────────────────────────── read aloud ─────────────────────────────

// static
bool WolfSpeech::isReadingAloud()
{
    static LLCachedControl<bool> read_aloud(gSavedSettings, "WolfViewerSpeechReadAloud", false);
    return read_aloud;
}

// Source: text_to_speech.js toggle() / start() / stop().
void WolfSpeech::toggleReadAloud()
{
    if (isReadingAloud())
    {
        gSavedSettings.setBOOL("WolfViewerSpeechReadAloud", false);
        mQueue.clear();
        tip("Reading aloud off.");
        return;
    }
    if (!isAvailable())
    {
        report("Reading chat aloud is available on Wolf Territories only.");
        return;
    }
    if (!gAudiop)
    {
        report("The audio engine is off, so nothing can be read aloud.");
        return;
    }
    gSavedSettings.setBOOL("WolfViewerSpeechReadAloud", true);
    mReportedKey.clear();
    tip("Reading chat aloud: nearby chat and IMs are spoken as they arrive. Press again to stop.");
}

// Source: text_to_speech.js onNearbyChat(from, text, type) — our own lines never reach it
// there; here they are filtered by mFromID.
void WolfSpeech::onNearbyChat(const LLChat& chat)
{
    if (!isReadingAloud() || !isAvailable())
    {
        return;
    }
    if (chat.mMuted || chat.mFromID == gAgentID)
    {
        return;
    }
    if (chat.mSourceType == CHAT_SOURCE_SYSTEM || chat.mSourceType == CHAT_SOURCE_TELEPORT)
    {
        return;
    }
    if (chat.mChatType == CHAT_TYPE_START || chat.mChatType == CHAT_TYPE_STOP || chat.mChatType == CHAT_TYPE_DEBUG_MSG)
    {
        return;
    }
    const char* verb = (chat.mChatType == CHAT_TYPE_WHISPER) ? "whispers"
                     : (chat.mChatType == CHAT_TYPE_SHOUT) ? "shouts" : "says";
    enqueue(chat.mFromName + " " + verb + ". " + chat.mText);
}

// Source: text_to_speech.js onInstantMessage(from, text, isGroup) — IMs on, group chat off
// by default (settings WolfViewerSpeechReadIMs / WolfViewerSpeechReadGroups).
void WolfSpeech::onInstantMessage(const LLUUID& session_id, const std::string& from, const LLUUID& from_id, const std::string& text)
{
    if (!isReadingAloud() || !isAvailable())
    {
        return;
    }
    if (from_id == gAgentID || from == SYSTEM_FROM || from_id.isNull())
    {
        return;
    }
    LLIMModel::LLIMSession* session = LLIMModel::getInstance()->findIMSession(session_id);
    const bool is_group = session && session->isGroupSessionType();
    static LLCachedControl<bool> read_ims(gSavedSettings, "WolfViewerSpeechReadIMs", true);
    static LLCachedControl<bool> read_groups(gSavedSettings, "WolfViewerSpeechReadGroups", false);
    if (is_group)
    {
        if (!read_groups) return;
        enqueue("Group message from " + from + ". " + text);
    }
    else
    {
        if (!read_ims) return;
        enqueue("Message from " + from + ". " + text);
    }
}

// Source: text_to_speech.js _enqueue(): a burst beyond MAX_QUEUE drops the OLDEST pending
// lines, because old chat read late is noise, not information.
void WolfSpeech::enqueue(const std::string& line)
{
    const std::string said = speakable(line);
    if (said.empty())
    {
        return;
    }
    mQueue.push_back(said);
    while ((S32)mQueue.size() > MAX_QUEUE)
    {
        mQueue.pop_front();
    }
    pumpSpeech();
}

// Source: text_to_speech.js _speakable(): links become the word "link", whitespace collapses,
// a line with no letters or digits is skipped, long lines are cut at MAX_CHARS.
std::string WolfSpeech::speakable(const std::string& line)
{
    std::string s;
    s.reserve(line.size());
    size_t i = 0;
    while (i < line.size())
    {
        if (line.compare(i, 7, "http://") == 0 || line.compare(i, 8, "https://") == 0
            || line.compare(i, 13, "secondlife://") == 0)
        {
            s += " link ";
            while (i < line.size() && !isspace((unsigned char)line[i])) ++i;
            continue;
        }
        s += line[i++];
    }
    // collapse whitespace
    std::string out;
    bool in_space = false;
    bool has_word_char = false;
    for (unsigned char c : s)
    {
        if (isspace(c))
        {
            if (!in_space && !out.empty()) out += ' ';
            in_space = true;
        }
        else
        {
            in_space = false;
            out += (char)c;
            if (isalnum(c) || c >= 0x80) has_word_char = true;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (!has_word_char)
    {
        return std::string();
    }
    if (out.size() > MAX_CHARS)
    {
        out = out.substr(0, MAX_CHARS) + ".";
    }
    return out;
}

// Source: text_to_speech.js _pump(): one line at a time, in order.
void WolfSpeech::pumpSpeech()
{
    if (mSynthesising || mQueue.empty())
    {
        return;
    }
    if (!isReadingAloud())
    {
        mQueue.clear();
        return;
    }
    if (LLFrameTimer::getElapsedSeconds() < mSpeakingUntil)
    {
        return;   // the previous line is still playing
    }
    const std::string text = mQueue.front();
    mQueue.pop_front();
    mSynthesising = true;
    LLCoros::instance().launch("WolfSpeech tts", [text]() { ttsCoro(text); });
}

// static
void WolfSpeech::ttsCoro(std::string text)
{
    // Source: text_to_speech.js _say(): POST JSON {text} to TTS_URL; reply audio/wav.
    LLSD body;
    body["text"] = text;
    const std::string json = boost::json::serialize(LlsdToJson(body));
    std::vector<U8> bytes(json.begin(), json.end());
    LLSD::Binary reply;
    std::string error;
    const S32 status = postRaw(std::string(WolfGrid::SPEECH_API_BASE) + "/tts", "application/json", bytes, reply, error);
    if (status >= 200 && status < 300 && reply.size() > 44)
    {
        WolfSpeech::instance().onSynthesised(true, status, reply, std::string());
    }
    else
    {
        std::string msg = jsonField(reply, "error");
        if (msg.empty())
        {
            msg = "speech service returned HTTP " + std::to_string(status) + (error.empty() ? "" : " (" + error + ")");
        }
        WolfSpeech::instance().onSynthesised(false, status, LLSD::Binary(), msg);
    }
}

void WolfSpeech::onSynthesised(bool ok, S32 status, const LLSD::Binary& wav, const std::string& error)
{
    mSynthesising = false;
    if (!ok)
    {
        if (status == 503)
        {
            reportOnce("busy", "The speech service is busy — some lines will not be read.");
        }
        else
        {
            reportOnce("tts-" + std::to_string(status), "Reading aloud failed: " + error);
        }
        return;
    }
    if (!isReadingAloud() || !gAudiop)
    {
        return;
    }
    mReportedKey.clear();
    // Into the sound cache under a fresh UUID: LLAudioData's constructor calls
    // hasDecodedFile(uuid), which looks for <LL_PATH_FS_SOUND_CACHE>/<uuid>.dsf
    // (llaudioengine.cpp), and the OpenAL buffer loads it as a WAV (alut).
    const LLUUID id = LLUUID::generateNewID();
    const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_FS_SOUND_CACHE, id.asString()) + ".dsf";
    {
        llofstream out(path, std::ios::binary);
        if (!out.is_open())
        {
            reportOnce("cache", "Reading aloud failed: could not write to the sound cache.");
            return;
        }
        out.write(reinterpret_cast<const char*>(wav.data()), wav.size());
    }
    const F32 seconds = wavSeconds(wav);
    gAudiop->triggerSound(id, gAgentID, 1.f, LLAudioEngine::AUDIO_TYPE_UI);
    const F64 now = LLFrameTimer::getElapsedSeconds();
    // A short gap between lines reads as punctuation; the file lives well past its playback
    // so the engine's own loading can never race the delete.
    mSpeakingUntil = now + seconds + 0.25;
    mPlayed.push_back({ path, now + seconds + 30.0 });
}

// Duration of a RIFF/WAVE PCM file from its fmt and data chunks; 0 if not parseable.
F32 WolfSpeech::wavSeconds(const LLSD::Binary& wav)
{
    if (wav.size() < 12 || memcmp(wav.data(), "RIFF", 4) != 0 || memcmp(wav.data() + 8, "WAVE", 4) != 0)
    {
        return 0.f;
    }
    auto rd32 = [&wav](size_t o) -> U32 { return wav[o] | (wav[o + 1] << 8) | (wav[o + 2] << 16) | ((U32)wav[o + 3] << 24); };
    auto rd16 = [&wav](size_t o) -> U16 { return (U16)(wav[o] | (wav[o + 1] << 8)); };
    U32 rate = 0, channels = 0, bits = 0, data_bytes = 0;
    size_t o = 12;
    while (o + 8 <= wav.size())
    {
        const U32 size = rd32(o + 4);
        if (memcmp(wav.data() + o, "fmt ", 4) == 0 && o + 8 + 16 <= wav.size())
        {
            channels = rd16(o + 10);
            rate = rd32(o + 12);
            bits = rd16(o + 22);
        }
        else if (memcmp(wav.data() + o, "data", 4) == 0)
        {
            data_bytes = size;
            break;
        }
        o += 8 + size + (size & 1);
    }
    if (!rate || !channels || !bits || !data_bytes)
    {
        return 0.f;
    }
    return (F32)data_bytes / (F32)(rate * channels * (bits / 8));
}

// ───────────────────────────── feedback ─────────────────────────────

// Source: speech_to_text.js _reportOnce(): one report per distinct cause, so a repeating
// fault cannot become a toast storm.
void WolfSpeech::reportOnce(const std::string& key, const std::string& message)
{
    if (mReportedKey == key)
    {
        return;
    }
    mReportedKey = key;
    report(message);
}

// static
void WolfSpeech::report(const std::string& message)
{
    LL_WARNS("WolfSpeech") << message << LL_ENDL;
    LLSD args;
    args["MESSAGE"] = message;
    LLNotificationsUtil::add("GenericAlertOK", args);
}

// static
void WolfSpeech::tip(const std::string& message)
{
    LLSD args;
    args["MESSAGE"] = message;
    LLNotificationsUtil::add("SystemMessageTip", args);
}
