/**
 * @file media_plugin_base.cpp
 * @brief Media plugin base class for LLMedia API plugin system
 *
 * All plugins should be a subclass of MediaPluginBase.
 *
 * @cond
 * $LicenseInfo:firstyear=2008&license=viewerlgpl$
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
 * @endcond
 */

#include "linden_common.h"
#include "media_plugin_base.h"
#include "wolfpluginaudio.h"    // [W111 VIDEO 2026-11-24]

#include <algorithm>
#include <cstring>
#include <iostream>


// TODO: Make sure that the only symbol exported from this library is LLPluginInitEntryPoint
////////////////////////////////////////////////////////////////////////////////
/// Media plugin constructor.
///
/// @param[in] host_send_func Function for sending messages from plugin to plugin loader shell
/// @param[in] host_user_data Message data for messages from plugin to plugin loader shell

MediaPluginBase::MediaPluginBase(
    LLPluginInstance::sendMessageFunction host_send_func,
    void *host_user_data )
{
    mHostSendFunction = host_send_func;
    mHostUserData = host_user_data;
    mDeleteMe = false;
    mPixels = 0;
    mWidth = 0;
    mHeight = 0;
    mTextureWidth = 0;
    mTextureHeight = 0;
    mDepth = 0;
    mStatus = STATUS_NONE;
}

// [W111 VIDEO 2026-11-24] out of line: ~unique_ptr<WolfPluginAudioWriter> needs the complete type
MediaPluginBase::~MediaPluginBase()
{
}

/**
 * Converts current media status enum value into string (STATUS_LOADING into "loading", etc.)
 *
 * @return Media status string ("loading", "playing", "paused", etc)
 *
 */
std::string MediaPluginBase::statusString()
{
    std::string result;

    switch(mStatus)
    {
        case STATUS_LOADING:    result = "loading";     break;
        case STATUS_LOADED:     result = "loaded";      break;
        case STATUS_ERROR:      result = "error";       break;
        case STATUS_PLAYING:    result = "playing";     break;
        case STATUS_PAUSED:     result = "paused";      break;
        case STATUS_DONE:       result = "done";        break;
        default:
            // keep the empty string
        break;
    }

    return result;
}

/**
 * Set media status.
 *
 * @param[in] status Media status (STATUS_LOADING, STATUS_PLAYING, STATUS_PAUSED, etc)
 *
 */
void MediaPluginBase::setStatus(EStatus status)
{
    if(mStatus != status)
    {
        mStatus = status;
        sendStatus();
    }
}


/**
 * Receive message from plugin loader shell.
 *
 * @param[in] message_string Message string
 * @param[in] user_data Message data
 *
 */
void MediaPluginBase::staticReceiveMessage(const char *message_string, void **user_data)
{
    MediaPluginBase *self = (MediaPluginBase*)*user_data;

    if(self != NULL)
    {
        // [W111 VIDEO 2026-11-24] the viewer's "sound through the viewer" choice comes in the media "init" message;
        // take it before the plugin's own receiveMessage handles "init" (and makes its player or browser).
        // The cheap text test keeps every other message from being parsed twice.
        if (!self->mWolfAudio && message_string && strstr(message_string, WolfPluginAudio::KEY_THROUGH_VIEWER))
        {
            self->wolfAudioInit(message_string);
        }
        self->receiveMessage(message_string);

        // If the plugin has processed the delete message, delete it.
        if(self->mDeleteMe)
        {
            delete self;
            *user_data = NULL;
        }
    }
}

/**
 * Send message to plugin loader shell.
 *
 * @param[in] message Message data being sent to plugin loader shell
 *
 */
void MediaPluginBase::sendMessage(const LLPluginMessage &message)
{
    std::string output = message.generate();
    mHostSendFunction(output.c_str(), &mHostUserData);
}

/**
 * Notifies plugin loader shell that part of display area needs to be redrawn.
 *
 * @param[in] left Left X coordinate of area to redraw (0,0 is at top left corner)
 * @param[in] top Top Y coordinate of area to redraw (0,0 is at top left corner)
 * @param[in] right Right X-coordinate of area to redraw (0,0 is at top left corner)
 * @param[in] bottom Bottom Y-coordinate of area to redraw (0,0 is at top left corner)
 *
 */
void MediaPluginBase::setDirty(int left, int top, int right, int bottom)
{
    LLPluginMessage message(LLPLUGIN_MESSAGE_CLASS_MEDIA, "updated");

    message.setValueS32("left", left);
    message.setValueS32("top", top);
    message.setValueS32("right", right);
    message.setValueS32("bottom", bottom);

    sendMessage(message);
}

/**
 * Sends "media_status" message to plugin loader shell ("loading", "playing", "paused", etc.)
 *
 */
void MediaPluginBase::sendStatus()
{
    LLPluginMessage message(LLPLUGIN_MESSAGE_CLASS_MEDIA, "media_status");

    message.setValue("status", statusString());

    sendMessage(message);
}


// ─── [W111 VIDEO 2026-11-24] sound through the viewer (see media_plugin_base.h, llplugin/wolfpluginaudio.h) ───────

void MediaPluginBase::wolfAudioInit(const char* message_string)
{
    LLPluginMessage message;
    if (message.parse(message_string) < 0)
    {
        return;
    }
    if (message.getClass() != LLPLUGIN_MESSAGE_CLASS_MEDIA || message.getName() != "init"
        || !message.hasValue(WolfPluginAudio::KEY_THROUGH_VIEWER)
        || !message.getValueBoolean(WolfPluginAudio::KEY_THROUGH_VIEWER))
    {
        return;
    }
    const std::string name = message.getValue(WolfPluginAudio::KEY_SHM_NAME);
    const S32 size = message.getValueS32(WolfPluginAudio::KEY_SHM_SIZE);
    std::unique_ptr<WolfPluginAudioWriter> writer(new WolfPluginAudioWriter);
    if (size > 0 && writer->attach(name, (size_t)size))
    {
        mWolfAudio = std::move(writer);
    }
    else
    {
        // The plugin plays its own sound, as before; the viewer still sends "set_volume", so nothing is lost.
        std::cerr << "media plugin: could not attach the viewer's sound ring '" << name << "'; playing sound here" << std::endl;
    }
}

bool MediaPluginBase::wolfAudioThroughViewer() const
{
    return mWolfAudio != nullptr;
}

void MediaPluginBase::wolfAudioFormat(int channels, int sample_rate)
{
    std::lock_guard<std::mutex> lock(mWolfAudioMutex);
    mWolfAudioChannels = channels;
    mWolfAudioRate = sample_rate;
    if ((channels != 1 && channels != 2) || sample_rate < (int)WolfPluginAudio::MIN_RATE
        || sample_rate > (int)WolfPluginAudio::MAX_RATE)
    {
        if (!mWolfAudioFormatWarned)
        {
            mWolfAudioFormatWarned = true;
            std::cerr << "media plugin: sound format " << channels << " ch " << sample_rate
                      << " Hz is not one the viewer takes (1-2 ch, 8000-384000 Hz); dropped" << std::endl;
        }
    }
}

void MediaPluginBase::wolfAudioPush(const float* interleaved, size_t frames)
{
    wolfAudioWrite(interleaved, frames, WolfPluginAudio::NO_DUE);
}

void MediaPluginBase::wolfAudioPushAt(const float* interleaved, size_t frames, int64_t due_us)
{
    wolfAudioWrite(interleaved, frames, due_us);    // WolfPluginAudio::NO_DUE (0) = as soon as possible
}

void MediaPluginBase::wolfAudioWrite(const float* interleaved, size_t frames, int64_t due_us)
{
    if (!mWolfAudio || !interleaved || frames == 0)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(mWolfAudioMutex);
    const int channels = mWolfAudioChannels;
    const int rate = mWolfAudioRate;
    if ((channels != 1 && channels != 2) || rate < (int)WolfPluginAudio::MIN_RATE || rate > (int)WolfPluginAudio::MAX_RATE)
    {
        return;
    }
    size_t done = 0;
    while (done < frames)
    {
        const size_t n = std::min<size_t>(frames - done, WolfPluginAudio::MAX_RECORD_FRAMES);
        const float* stereo = interleaved + done * (size_t)channels;
        if (channels == 1)
        {
            // Mono plays the same on both sides (the ring is always stereo).
            mWolfAudioScratch.resize(n * 2);
            for (size_t i = 0; i < n; ++i)
            {
                mWolfAudioScratch[i * 2] = mWolfAudioScratch[i * 2 + 1] = stereo[i];
            }
            stereo = mWolfAudioScratch.data();
        }
        // A split block's later parts are due later by the frames before them.
        const int64_t due = due_us == WolfPluginAudio::NO_DUE ? due_us : due_us + (int64_t)(done * 1000000 / (size_t)rate);
        mWolfAudio->write(WolfPluginAudio::KIND_DATA, (uint32_t)rate, due, stereo, (uint32_t)n);
        done += n;
    }
}

void MediaPluginBase::wolfAudioFlush()
{
    if (mWolfAudio)
    {
        mWolfAudio->write(WolfPluginAudio::KIND_FLUSH, 0, WolfPluginAudio::NO_DUE, nullptr, 0);
    }
}

void MediaPluginBase::wolfAudioPause()
{
    if (mWolfAudio && !mWolfAudioPaused.exchange(true))
    {
        if (!mWolfAudio->write(WolfPluginAudio::KIND_PAUSE, 0, WolfPluginAudio::nowUs(), nullptr, 0))
        {
            mWolfAudioPaused.store(false);      // not delivered (ring full): the viewer is not paused
        }
    }
}

void MediaPluginBase::wolfAudioResume()
{
    if (mWolfAudio && mWolfAudioPaused.exchange(false))
    {
        if (!mWolfAudio->write(WolfPluginAudio::KIND_RESUME, 0, WolfPluginAudio::nowUs(), nullptr, 0))
        {
            mWolfAudioPaused.store(true);       // not delivered: the viewer is still paused
        }
    }
}

#if LL_WINDOWS
# define LLSYMEXPORT __declspec(dllexport)
#else
# define LLSYMEXPORT __attribute__ ((visibility("default")))
#endif

extern "C"
{
    LLSYMEXPORT int LLPluginInitEntryPoint(LLPluginInstance::sendMessageFunction host_send_func, void *host_user_data, LLPluginInstance::sendMessageFunction *plugin_send_func, void **plugin_user_data);
}

/**
 * Plugin initialization and entry point. Establishes communication channel for messages between plugin and plugin loader shell.  TODO:DOC - Please check!
 *
 * @param[in] host_send_func Function for sending messages from plugin to plugin loader shell
 * @param[in] host_user_data Message data for messages from plugin to plugin loader shell
 * @param[out] plugin_send_func Function for plugin to receive messages from plugin loader shell
 * @param[out] plugin_user_data Pointer to plugin instance
 *
 * @return int, where 0=success
 *
 */
LLSYMEXPORT int
LLPluginInitEntryPoint(LLPluginInstance::sendMessageFunction host_send_func, void *host_user_data, LLPluginInstance::sendMessageFunction *plugin_send_func, void **plugin_user_data)
{
    return init_media_plugin(host_send_func, host_user_data, plugin_send_func, plugin_user_data);
}

#ifdef WIN32
int WINAPI DllEntryPoint( HINSTANCE hInstance, unsigned long reason, void* params )
{
    return 1;
}
#endif
