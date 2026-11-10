/**
 * @file wolfvoicemessage.cpp
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

#include "llviewerprecompiledheaders.h"

#include "wolfvoicemessage.h"

#include <cstring>
#include <functional>

#include <boost/json.hpp>
#include <boost/regex.hpp>

#if LL_OPENAL
#include "AL/al.h"
#include "AL/alc.h"
#endif

#include "fsfloaterim.h"
#include "llagent.h"
#include "llaudioengine.h"
#include "llcommandhandler.h"
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
#include "llstartup.h"
#include "rlvactions.h"
#include "wolfgrid.h"
#include "wolfspeech.h"

// Threads: everything here runs on the main thread (idle, UI callbacks, and coroutines resumed on the main loop).

namespace
{
    // Source: wolfaimode.cpp SERVICE_URL - the WolfStorm web host every WolfViewer call to a wolfstorm/php service uses.
    const char* const SERVICE_URL = "https://wolfstorm.app/php/voicemsg.php";

    // Source: wolfstorm/php/voicemsg_lib.php vm_im_text() - the exact line the server makes, so a resident's own text that
    // merely contains the link is never rewritten. The microphone is U+1F3A4 in UTF-8.
    const boost::regex LINE_PATTERN(
        "^\xF0\x9F\x8E\xA4 Voice message \\(([0-9]{1,2}:[0-9]{2})\\) - plays in WolfViewer and WolfStorm, or listen at "
        "https://www\\.wolf-grid\\.com/voicemessage\\.php\\?m=([A-Za-z0-9_-]{22}) \\(log in to the website first\\)\\. "
        "Kept for ([0-9]{1,3}) days\\.$");
    // Source: voicemsg_lib.php VM_ID_PATTERN
    const boost::regex ID_PATTERN("^[A-Za-z0-9_-]{22}$");

    /** A JSON body as LLSD (empty when it is not JSON). */
    LLSD parseJson(const std::string& text)
    {
        boost::system::error_code ec;
        boost::json::value v = boost::json::parse(text, ec);
        if (ec || !v.is_object())
        {
            return LLSD();
        }
        return LlsdFromJson(v);
    }

    /** The reply of a request: the raw body when there is one, else the error body (llcorehttputil.cpp httpStatus
     *  ["error_body"]) - wolfaimode.cpp WolfAIMode::request reads both the same way. */
    std::vector<U8> replyBytes(const LLSD& result)
    {
        std::vector<U8> out;
        if (result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
        {
            const LLSD::Binary& b = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
            out.assign(b.begin(), b.end());
        }
        const LLSD& http = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
        if (out.empty() && http.has("error_body"))
        {
            const std::string& e = http["error_body"].asStringRef();
            out.assign(e.begin(), e.end());
        }
        return out;
    }

    /** The person-readable failure for a request that did not succeed. */
    std::string failureText(S32 status, const LLSD& reply, const char* what)
    {
        if (reply.isMap() && reply.has("error") && !reply["error"].asString().empty() && status == 200)
        {
            return reply["error"].asString();   // a refusal the server wrote for a person (HTTP 200, success false)
        }
        if (status == 401 || status == 403)
        {
            return std::string("Your Wolf Territories login could not be checked, so the ") + what
                 + " did not go through. Try again in a moment, or log out and back in.";
        }
        if (status == 0)
        {
            return std::string("The voice message service could not be reached, so the ") + what
                 + " did not go through. Check your internet connection and try again.";
        }
        return std::string("The voice message service answered with an error (HTTP ") + std::to_string(status) + "), so the "
             + what + " did not go through. Try again in a moment.";
    }

    LLCore::HttpHeaders::ptr_t sessionHeaders(const char* accept)
    {
        // Source: wolfspeech.cpp postRaw / wolfaimode.cpp - the session pair on a host-verified, non-redirecting request.
        LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
        headers->append(HTTP_OUT_HEADER_ACCEPT, accept);
        headers->append("X-Wolf-Agent", gAgentID.asString());
        headers->append("X-Wolf-Session", gAgentSessionID.asString());
        return headers;
    }

    typedef std::function<void(bool ok, const std::string& text, const std::string& error)> sent_fn;
    typedef std::function<void(bool ok, const std::vector<U8>& wav, const std::string& error)> fetched_fn;

    void sendCoro(LLUUID to, std::vector<U8> wav, sent_fn done)
    {
        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
            std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfVoiceMessage send", LLCore::HttpRequest::DEFAULT_POLICY_ID);
        LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
        LLCore::HttpOptions::ptr_t options = WolfGrid::makeVerifiedHttpOptions();
        options->setTimeout(40);   // upload of up to 1.9 MB plus the server's Opus encode (VM_FFMPEG_TIMEOUT 20 s)
        options->setRetries(0);    // a retry could store the clip twice
        LLCore::HttpHeaders::ptr_t headers = sessionHeaders("application/json");
        headers->append(HTTP_OUT_HEADER_CONTENT_TYPE, "audio/wav");
        LLCore::BufferArray::ptr_t raw(new LLCore::BufferArray());   // intrusive pointer (wolfspeech.cpp postRaw)
        raw->append(wav.data(), wav.size());
        const std::string url = std::string(SERVICE_URL) + "?action=send&to=" + to.asString();
        LLSD result = adapter->postRawAndSuspend(request, url, raw, options, headers);
        LLCore::HttpStatus st = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
        const S32 status = st.isHttpStatus() ? (S32)st.getType() : (st ? 200 : 0);
        const std::vector<U8> bytes = replyBytes(result);
        const LLSD reply = parseJson(std::string(bytes.begin(), bytes.end()));
        const std::string text = reply.isMap() ? reply["text"].asString() : std::string();
        const bool ok = status == 200 && reply.isMap() && reply["success"].asBoolean() && WolfVoiceMessage::isVoiceMessageLine(text);
        if (!ok)
        {
            LL_WARNS("WolfVoiceMessage") << "send: HTTP " << status << " " << st.toString() << LL_ENDL;
        }
        done(ok, text, ok ? std::string() : failureText(status, reply, "voice message"));
    }

    void fetchCoro(std::string id, fetched_fn done)
    {
        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
            std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfVoiceMessage fetch", LLCore::HttpRequest::DEFAULT_POLICY_ID);
        LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
        LLCore::HttpOptions::ptr_t options = WolfGrid::makeVerifiedHttpOptions();
        options->setTimeout(30);
        options->setRetries(1);    // a read: safe to repeat
        LLCore::HttpHeaders::ptr_t headers = sessionHeaders("audio/wav, application/json");
        const std::string url = std::string(SERVICE_URL) + "?action=get&m=" + id;
        LLSD result = adapter->getRawAndSuspend(request, url, options, headers);
        LLCore::HttpStatus st = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
        const S32 status = st.isHttpStatus() ? (S32)st.getType() : (st ? 200 : 0);
        std::vector<U8> bytes = replyBytes(result);
        // Source: voicemsg.php - a clip is the only answer that is not JSON; it starts "RIFF" (vm_wav_header).
        if (status == 200 && bytes.size() > 44 && memcmp(bytes.data(), "RIFF", 4) == 0)
        {
            done(true, bytes, std::string());
            return;
        }
        LL_WARNS("WolfVoiceMessage") << "fetch " << id << ": HTTP " << status << " " << st.toString() << ", " << bytes.size() << " bytes" << LL_ENDL;
        done(false, std::vector<U8>(), failureText(status, parseJson(std::string(bytes.begin(), bytes.end())), "voice message"));
    }
}

// =====================================================================================================================
// secondlife:///app/wolfvoicemsg/play/<id> - the Play link in a one-to-one conversation (displayText)
// =====================================================================================================================

class WolfVoiceMessageHandler : public LLCommandHandler
{
public:
    // Source: wolffloaterchat.cpp WolfChatHandler - a link in chat, honoured only when clicked.
    WolfVoiceMessageHandler() : LLCommandHandler("wolfvoicemsg", UNTRUSTED_CLICK_ONLY) {}

    bool handle(const LLSD& params, const LLSD& query_map, const std::string& grid, LLMediaCtrl* web) override
    {
        if (LLStartUp::getStartupState() < STATE_STARTED) return true;
        if (params.size() < 2 || params[0].asString() != "play") return false;
        const std::string id = params[1].asString();
        if (!boost::regex_match(id, ID_PATTERN)) return false;
        if (!WolfVoiceMessage::isAvailable())
        {
            WolfGrid::noteWolfOnly("Voice messages");
            return true;
        }
        WolfVoiceMessage::instance().play(id);
        return true;
    }
};
WolfVoiceMessageHandler gWolfVoiceMessageHandler;

// =====================================================================================================================

WolfVoiceMessage::WolfVoiceMessage()
{
}

WolfVoiceMessage::~WolfVoiceMessage()
{
    closeCapture();
    for (const Played& p : mPlayed)
    {
        LLFile::remove(p.mPath);
    }
    mPlayed.clear();
}

// static
bool WolfVoiceMessage::isAvailable()
{
    // Source: wolfspeech.cpp isAvailable / canCapture - the login grid (the server checks the session with Wolf presence).
    return WolfGrid::isWolfTerritories() && gAgentID.notNull() && WolfSpeech::canCapture();
}

F32 WolfVoiceMessage::recordedSeconds() const
{
    return (F32)mPcm.size() / (F32)WolfSpeech::TARGET_RATE;
}

void WolfVoiceMessage::setState(EState state)
{
    mState = state;
    if (state == IDLE)
    {
        mSessionID.setNull();
        mOtherID.setNull();
    }
    mStateChanged();
}

bool WolfVoiceMessage::startRecording(const LLUUID& session_id, const LLUUID& other_id)
{
    if (mState == SENDING)
    {
        report("Your last voice message is still being sent - wait a moment, then try again.");
        return false;
    }
    if (mState == RECORDING)
    {
        report("You are already recording a voice message in another conversation. Send it or cancel it first.");
        return false;
    }
    if (!isAvailable())
    {
        report("Voice messages work on Wolf Territories only.");
        return false;
    }
    if (other_id.isNull() || other_id == gAgentID)
    {
        report("Voice messages are for conversations with one other resident.");
        return false;
    }
    // [RLVa] the same restriction a typed IM to this resident is under (fsfloaterim.cpp sendMsg)
    if ((RlvActions::hasBehaviour(RLV_BHVR_SENDIM) || RlvActions::hasBehaviour(RLV_BHVR_SENDIMTO)) && !RlvActions::canSendIM(other_id))
    {
        report("You cannot send messages to this resident at the moment (RLV restriction).");
        return false;
    }
    if (WolfSpeech::instanceExists() && WolfSpeech::instance().isDictating())
    {
        report("Dictation is using the microphone. Turn dictation off, then press Voice again.");
        return false;
    }
    if (!openCapture())
    {
        report("The microphone could not be opened. Check it is plugged in and that no other program is using it, then try again.");
        return false;
    }
    mPcm.clear();
    mPcm.reserve((size_t)(WolfSpeech::TARGET_RATE * (MAX_SECONDS + 1.f)));
    mSessionID = session_id;
    mOtherID = other_id;
    LL_INFOS("WolfVoiceMessage") << "recording for " << other_id << LL_ENDL;
    setState(RECORDING);
    return true;
}

void WolfVoiceMessage::cancel()
{
    if (mState != RECORDING)
    {
        return;
    }
    closeCapture();
    mPcm.clear();
    LL_INFOS("WolfVoiceMessage") << "recording cancelled" << LL_ENDL;
    setState(IDLE);
}

void WolfVoiceMessage::stopAndSend()
{
    if (mState != RECORDING)
    {
        return;
    }
    pollCapture();   // whatever the device holds up to now
    closeCapture();
    if (mPcm.size() > (size_t)(WolfSpeech::TARGET_RATE * MAX_SECONDS))
    {
        mPcm.resize((size_t)(WolfSpeech::TARGET_RATE * MAX_SECONDS));
    }
    const F32 seconds = recordedSeconds();
    if (seconds < MIN_SECONDS)
    {
        mPcm.clear();
        setState(IDLE);
        report("That voice message was too short to send. Press Voice, speak, then press Send.");
        return;
    }
    // Nothing reached the microphone (muted, unplugged, wrong input): the loudest sample is below the level dictation
    // treats as the quietest speech (wolfspeech.h SPEECH_RMS_MIN, as a 16-bit amplitude).
    S32 peak = 0;
    for (S16 v : mPcm)
    {
        const S32 a = v < 0 ? -(S32)v : (S32)v;
        if (a > peak) peak = a;
    }
    if ((F32)peak < WolfSpeech::SPEECH_RMS_MIN * 32767.f)
    {
        mPcm.clear();
        setState(IDLE);
        report("No sound came from your microphone, so nothing was sent. Check it is plugged in, not muted, and chosen as "
               "the input in your computer's sound settings.");
        return;
    }
    std::vector<U8> wav = WolfSpeech::wav16k(mPcm);   // the dictation WAV: 16 kHz mono PCM16, peak-normalised
    mPcm.clear();
    mPcm.shrink_to_fit();
    const LLUUID to = mOtherID;
    LL_INFOS("WolfVoiceMessage") << "sending " << seconds << " s (" << wav.size() << " bytes)" << LL_ENDL;
    setState(SENDING);
    LLCoros::instance().launch("WolfVoiceMessage send", [to, wav]()
    {
        sendCoro(to, wav, [](bool ok, const std::string& text, const std::string& error)
        {
            if (WolfVoiceMessage::instanceExists())
            {
                WolfVoiceMessage::instance().onSent(ok, text, error);
            }
        });
    });
}

void WolfVoiceMessage::onSent(bool ok, const std::string& text, const std::string& error)
{
    const LLUUID session_id = mSessionID;
    const LLUUID other_id = mOtherID;
    setState(IDLE);
    if (!ok)
    {
        report(error);
        return;
    }
    // Through the IM window when it is open (RLVa filter, a not-yet-initialised session's queue - fsfloaterim.cpp sendMsg),
    // straight to the IM model otherwise.
    if (FSFloaterIM* im = FSFloaterIM::findInstance(session_id))
    {
        im->sendMsg(text);
    }
    else
    {
        LLIMModel::sendMessage(text, session_id, other_id, IM_NOTHING_SPECIAL);
    }
}

void WolfVoiceMessage::play(const std::string& id)
{
    if (mFetching.count(id))
    {
        return;   // already on its way
    }
    if (!gAudiop)
    {
        report("Sound is switched off in this viewer, so the voice message cannot be played.");
        return;
    }
    mFetching.insert(id);
    tip("Playing the voice message...");
    LLCoros::instance().launch("WolfVoiceMessage fetch", [id]()
    {
        fetchCoro(id, [id](bool ok, const std::vector<U8>& wav, const std::string& error)
        {
            if (WolfVoiceMessage::instanceExists())
            {
                WolfVoiceMessage::instance().onFetched(id, ok, wav, error);
            }
        });
    });
}

void WolfVoiceMessage::onFetched(const std::string& id, bool ok, const std::vector<U8>& wav, const std::string& error)
{
    mFetching.erase(id);
    if (!ok)
    {
        report(error);
        return;
    }
    if (!gAudiop)
    {
        report("Sound is switched off in this viewer, so the voice message cannot be played.");
        return;
    }
    // Source: wolfspeech.cpp onSynthesised - into the sound cache under a fresh UUID; LLAudioData finds the decoded file
    // (<LL_PATH_FS_SOUND_CACHE>/<uuid>.dsf, llaudioengine.cpp hasDecodedFile) and plays it without an asset fetch.
    const LLUUID sound_id = LLUUID::generateNewID();
    const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_FS_SOUND_CACHE, sound_id.asString()) + ".dsf";
    {
        llofstream out(path, std::ios::binary);
        if (!out.is_open())
        {
            report("The voice message could not be played: the viewer's sound cache could not be written.");
            return;
        }
        out.write(reinterpret_cast<const char*>(wav.data()), (std::streamsize)wav.size());
    }
    // Source: voicemsg_lib.php vm_wav_header - 44-byte header, 16 kHz mono PCM16.
    const F64 seconds = (F64)(wav.size() - 44) / (F64)(WolfSpeech::TARGET_RATE * 2);
    gAudiop->triggerSound(sound_id, gAgentID, 1.f, LLAudioEngine::AUDIO_TYPE_UI);   // wolfspeech.cpp playSound
    mPlayed.push_back({ path, sound_id, LLFrameTimer::getElapsedSeconds() + seconds + 30.0 });
    LL_INFOS("WolfVoiceMessage") << "playing " << id << " (" << seconds << " s)" << LL_ENDL;
}

void WolfVoiceMessage::idle()
{
    if (mState == RECORDING)
    {
        if (!isAvailable())
        {
            cancel();
        }
        else
        {
            pollCapture();
            if (recordedSeconds() >= MAX_SECONDS)
            {
                tip("That is one minute, the longest a voice message can be - sending it now.");
                stopAndSend();
            }
        }
    }
    if (!mPlayed.empty())
    {
        const F64 now = LLFrameTimer::getElapsedSeconds();
        for (auto it = mPlayed.begin(); it != mPlayed.end();)
        {
            if (now >= it->mDeleteAt)
            {
                // Source: wolfspeech.cpp idle - drop the engine's LLAudioData with the file, or its map grows a line a clip.
                if (gAudiop)
                {
                    gAudiop->removeAudioData(it->mId);
                }
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

bool WolfVoiceMessage::openCapture()
{
#if LL_OPENAL
    if (mCaptureDevice)
    {
        return true;
    }
    // Source: wolfspeech.cpp openCapture - 16 kHz mono PCM16, a two-second ring (polled every frame).
    ALCdevice* dev = alcCaptureOpenDevice(NULL, WolfSpeech::TARGET_RATE, AL_FORMAT_MONO16, WolfSpeech::TARGET_RATE * 2);
    if (!dev)
    {
        LL_WARNS("WolfVoiceMessage") << "alcCaptureOpenDevice failed" << LL_ENDL;
        return false;
    }
    alcCaptureStart(dev);
    mCaptureDevice = dev;
    return true;
#else
    return false;
#endif
}

void WolfVoiceMessage::closeCapture()
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
}

void WolfVoiceMessage::pollCapture()
{
#if LL_OPENAL
    if (!mCaptureDevice)
    {
        return;
    }
    // Source: wolfspeech.cpp pollCapture
    ALCdevice* dev = static_cast<ALCdevice*>(mCaptureDevice);
    ALCint available = 0;
    alcGetIntegerv(dev, ALC_CAPTURE_SAMPLES, 1, &available);
    if (available > 0)
    {
        const size_t old = mPcm.size();
        mPcm.resize(old + (size_t)available);
        alcCaptureSamples(dev, mPcm.data() + old, available);
    }
#endif
}

// static
bool WolfVoiceMessage::isVoiceMessageLine(const std::string& text, std::string* id, std::string* length)
{
    boost::smatch m;
    if (!boost::regex_match(text, m, LINE_PATTERN))
    {
        return false;
    }
    if (length) *length = m[1].str();
    if (id) *id = m[2].str();
    return true;
}

// static
std::string WolfVoiceMessage::displayText(const std::string& text)
{
    std::string id, length;
    if (!isVoiceMessageLine(text, &id, &length))
    {
        return text;
    }
    // A labelled SLapp link (llurlentry.cpp LLUrlEntrySLLabel "[secondlife://... label]") shows as "▶ Play".
    return "\xF0\x9F\x8E\xA4 Voice message (" + length + ")   [secondlife:///app/wolfvoicemsg/play/" + id + " \xE2\x96\xB6 Play]";
}

// static
void WolfVoiceMessage::report(const std::string& message)
{
    // Source: wolfspeech.cpp report - an alert the resident has to acknowledge, so a failure is never missed.
    LL_WARNS("WolfVoiceMessage") << message << LL_ENDL;
    LLSD args;
    args["MESSAGE"] = message;
    LLNotificationsUtil::add("GenericAlertOK", args);
}

// static
void WolfVoiceMessage::tip(const std::string& message)
{
    LLSD args;
    args["MESSAGE"] = message;
    LLNotificationsUtil::add("SystemMessageTip", args);
}
