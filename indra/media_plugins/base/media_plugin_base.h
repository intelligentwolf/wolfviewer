/**
 * @file media_plugin_base.h
 * @brief Media plugin base class for LLMedia API plugin system
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

#include "llplugininstance.h"
#include "llpluginmessage.h"
#include "llpluginmessageclasses.h"

#include <atomic>   // [W111 VIDEO 2026-11-24]
#include <memory>
#include <mutex>
#include <vector>

class WolfPluginAudioWriter;


class MediaPluginBase
{
public:
    MediaPluginBase(LLPluginInstance::sendMessageFunction host_send_func, void *host_user_data);
   /** Media plugin destructor. */
    virtual ~MediaPluginBase();

   /** Handle received message from plugin loader shell. */
    virtual void receiveMessage(const char *message_string) = 0;

    static void staticReceiveMessage(const char *message_string, void **user_data);

    // [W111 VIDEO 2026-11-24] SOUND THROUGH THE VIEWER (ticket 1033, Paul's decision "B"; llplugin/wolfpluginaudio.h).
    // When the viewer asks for it (the boolean "wolf_audio_through_viewer" in the media "init" message, which this base
    // reads before the plugin's own receiveMessage sees "init", so before any player or browser exists), the plugin
    // hands its decoded sound to the viewer, which plays it in its OpenAL engine, and plays nothing itself.
    // While wolfAudioThroughViewer() is true the plugin must NOT apply "set_volume" or "wolf_audio_device" itself: the
    // viewer applies the volume (and its own output device) to the sound it plays. When it is false - the viewer did
    // not ask, or the ring could not be attached - the plugin plays its sound itself exactly as before.
    // A plugin that never calls wolfAudioPush simply keeps playing itself: the viewer still sends "set_volume".
    /** True when the viewer asked for the plugin's PCM and the ring is attached; fixed before any media is created. */
    bool wolfAudioThroughViewer() const;
    /** (Re)announce the format of the following blocks: 1 (mono, played on both sides) or 2 (stereo) channels,
     *  8000-384000 Hz. Other channel counts are refused (logged once) and their blocks dropped: ask the decoder for
     *  stereo. Any thread. */
    void wolfAudioFormat(int channels, int sample_rate);
    /** Interleaved float frames in the announced format, to be heard as soon as possible, in order. Any thread of the
     *  plugin; only copies (never waits for the viewer). */
    void wolfAudioPush(const float* interleaved, size_t frames);
    /** Same, with the time the first frame should be heard, on WolfPluginAudio::nowUs()'s clock (lip-sync);
     *  WolfPluginAudio::NO_DUE (0) = as soon as possible, as wolfAudioPush. */
    void wolfAudioPushAt(const float* interleaved, size_t frames, int64_t due_us);
    /** Stream stopped / seek / new media: drop what is queued in the viewer. */
    void wolfAudioFlush();
    /** Playback paused / resumed: the viewer holds what it has queued and carries on from there. Each is sent only
     *  when it changes something (a resume without a pause before it is not sent), so a plugin may call them freely. */
    void wolfAudioPause();
    void wolfAudioResume();

protected:

   /** Plugin status. */
    typedef enum
    {
        STATUS_NONE,
        STATUS_LOADING,
        STATUS_LOADED,
        STATUS_ERROR,
        STATUS_PLAYING,
        STATUS_PAUSED,
        STATUS_DONE
    } EStatus;

   /** Plugin shared memory. */
    class SharedSegmentInfo
    {
    public:
      /** Shared memory address. */
        void *mAddress;
      /** Shared memory size. */
        size_t mSize;
    };

    void sendMessage(const LLPluginMessage &message);
    void sendStatus();
    std::string statusString();
    void setStatus(EStatus status);

    /// Note: The quicktime plugin overrides this to add current time and duration to the message.
    virtual void setDirty(int left, int top, int right, int bottom);

   /** Map of shared memory names to shared memory. */
    typedef std::map<std::string, SharedSegmentInfo> SharedSegmentMap;


   /** Function to send message from plugin to plugin loader shell. */
    LLPluginInstance::sendMessageFunction mHostSendFunction;
   /** Message data being sent to plugin loader shell by mHostSendFunction. */
    void *mHostUserData;
   /** Flag to delete plugin instance (self). */
    bool mDeleteMe;
   /** Pixel array to display. TODO:DOC are pixels always 24-bit RGB format, aligned on 32-bit boundary? Also: calling this a pixel array may be misleading since 1 pixel > 1 char. */
    unsigned char* mPixels;
   /** TODO:DOC what's this for -- does a texture have its own piece of shared memory? updated on size_change_request, cleared on shm_remove */
    std::string mTextureSegmentName;
   /** Width of plugin display in pixels. */
    int mWidth;
   /** Height of plugin display in pixels. */
    int mHeight;
   /** Width of plugin texture. */
    int mTextureWidth;
   /** Height of plugin texture. */
    int mTextureHeight;
   /** Pixel depth (pixel size in bytes). */
    int mDepth;
   /** Current status of plugin. */
    EStatus mStatus;
   /** Map of shared memory segments. */
    SharedSegmentMap mSharedSegments;

private:
    // [W111 VIDEO 2026-11-24] see wolfAudioThroughViewer
    void wolfAudioInit(const char* message_string);
    void wolfAudioWrite(const float* interleaved, size_t frames, int64_t due_us);
    std::unique_ptr<WolfPluginAudioWriter> mWolfAudio;      // set once, on "init", before the plugin sees "init"
    std::mutex          mWolfAudioMutex;                     // the format and the stereo scratch below
    int                 mWolfAudioChannels = 2;
    int                 mWolfAudioRate = 48000;
    bool                mWolfAudioFormatWarned = false;
    std::atomic<bool>   mWolfAudioPaused{false};
    std::vector<float>  mWolfAudioScratch;

};

/** The plugin <b>must</b> define this function to create its instance.
 * It should look something like this:
 * @code
 * {
 *    MediaPluginFoo *self = new MediaPluginFoo(host_send_func, host_user_data);
 *    *plugin_send_func = MediaPluginFoo::staticReceiveMessage;
 *    *plugin_user_data = (void*)self;
 *
 *    return 0;
 * }
 * @endcode
 */
int init_media_plugin(
    LLPluginInstance::sendMessageFunction host_send_func,
    void *host_user_data,
    LLPluginInstance::sendMessageFunction *plugin_send_func,
    void **plugin_user_data);


