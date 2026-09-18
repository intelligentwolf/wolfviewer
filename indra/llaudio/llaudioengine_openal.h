/**
 * @file audioengine_openal.cpp
 * @brief implementation of audio engine using OpenAL
 * support as a OpenAL 3D implementation
 *
 *
 * $LicenseInfo:firstyear=2002&license=viewerlgpl$
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
 */


#ifndef LL_AUDIOENGINE_OPENAL_H
#define LL_AUDIOENGINE_OPENAL_H

#include "llaudioengine.h"
#include "lllistener_openal.h"
#include "llwindgen.h"
#include <atomic>   // <WolfViewer 2026-09-13> the device-change flag set from OpenAL's thread
#include <map>      // <WolfViewer 2026-09-18> the output-device list
#include <string>
#include <vector>

class LLAudioEngine_OpenAL : public LLAudioEngine
{
    public:
        LLAudioEngine_OpenAL();
        virtual ~LLAudioEngine_OpenAL();

        virtual bool init(void *user_data, const std::string &app_title);
        virtual std::string getDriverName(bool verbose);
        virtual LLStreamingAudioInterface* createDefaultStreamingAudioImpl() const { return nullptr; }
        virtual void allocateListener();

        virtual void shutdown();
        // <WolfViewer 2026-09-13> Every frame: base idle, then the output-device watch below.
        virtual void idle();
        virtual void setDeviceWatchEnabled(bool enabled);   // "virtual", not "override": clang -Winconsistent-missing-override (-Werror on macOS) wants the whole class one way
        // <WolfViewer 2026-09-18> OUTPUT DEVICE SELECTION (Benny Moonstone, via Paul: "missing ... the
        // selecting of the sound output like it was in the preferences in the original FS").
        // Firestorm's Preferences > Sound & Media picker (FSOutputDeviceUUID, FSPanelPreferenceSounds,
        // LLAudioEngine::setDevice / getDevices / OnOutputDeviceListChanged) only had an FMOD
        // implementation, and WolfViewer ships OpenAL on every platform, so the panel said
        // "unavailable". OpenAL Soft enumerates playback devices (ALC_ENUMERATE_ALL_EXT,
        // alc.h ALC_ALL_DEVICES_SPECIFIER) and alcReopenDeviceSOFT can open a NAMED device on the
        // live context - the same call the hot-plug watch below uses with NULL for the default.
        // A device's UUID is generated from its name (LLUUID::generate = a hash), so the saved
        // setting picks the same device next session; the null UUID is "the system default".
        virtual output_device_map_t getDevices();
        virtual void setDevice(const LLUUID& device_uuid);

        void setInternalGain(F32 gain);

        LLAudioBuffer* createBuffer();
        LLAudioChannel* createChannel();

        /*virtual*/ bool initWind();
        /*virtual*/ void cleanupWind();
        /*virtual*/ void updateWind(LLVector3 direction, F32 camera_altitude);

    private:
        // <WolfViewer 2026-09-13> AUDIO DEVICE HOT-PLUG (Owl Eyes, 09-13: "if sound had been
        // broken (plugout/plugin headset) the sound is not restored ... needs a relog").
        //
        // alutInit() opens the DEFAULT playback device once (alutInit -> alcOpenDevice(NULL)) and
        // nothing ever revisits that choice. When the headset the device was opened on is pulled,
        // the device's backend stream is gone; when it comes back, OpenAL is still bound to the
        // dead stream (or to whatever the system fell back to), so the viewer stays mute until it
        // is restarted.
        //
        // The shipped OpenAL Soft (autobuild.xml openal 1.24.2-r1, all three platforms) carries
        // two extensions that address exactly this, both in packages/include/AL/alext.h:
        //   ALC_SOFT_system_events  (alext.h:723) — a callback when the default playback device
        //                            changes or a device is added / removed;
        //   ALC_SOFT_reopen_device  (alext.h:571) — alcReopenDeviceSOFT(device, NULL, NULL)
        //                            re-opens the CURRENT default device on the same ALCdevice,
        //                            keeping the context, sources and buffers.
        // Plus ALC_EXT_disconnect (alext.h:158): ALC_CONNECTED reads 0 once the device is gone —
        // the fallback for a backend that raises no events.
        //
        // The event callback runs on OpenAL's own thread: it only raises the flag. The reopen
        // happens in idle(), on the main thread, like every other call into the engine.
        void   initDeviceWatch(ALCdevice* device);
        void   shutdownDeviceWatch();
        void   watchDevice();
        void   reopenDevice(const char* why);
        // <WolfViewer 2026-09-18> device selection (see setDevice above)
        bool   enumerateDevices(bool announce);      // rebuild mDeviceNames; true when the list changed
        const char* selectedDeviceName() const;      // NULL = the default device
        std::map<LLUUID, std::string> mDeviceNames;  // uuid (hash of the name) -> ALC device name
        std::string               mDeviceListSig;    // the last enumeration, as one string
        LLUUID                    mSelectedDevice;   // FSOutputDeviceUUID; null = default
        std::string               mSelectedDeviceName;
        bool                      mSelectedDeviceOpen = false;   // the context is on the chosen device (not a fallback)
        F64                       mNextListCheck = 0.0;
        static void ALC_APIENTRY onDeviceEvent(ALCenum eventType, ALCenum deviceType, ALCdevice* device,
                                               ALCsizei length, const ALCchar* message, void* userParam) ALC_API_NOEXCEPT17;
        LPALCREOPENDEVICESOFT     mReopenDeviceSOFT = nullptr;
        LPALCEVENTCONTROLSOFT     mEventControlSOFT = nullptr;
        LPALCEVENTCALLBACKSOFT    mEventCallbackSOFT = nullptr;
        LPALCEVENTISSUPPORTEDSOFT mEventIsSupportedSOFT = nullptr;
        bool                      mHasDisconnectExt = false;
        bool                      mEventsArmed = false;
        std::atomic<bool>         mDeviceChanged{false};
        F64                       mNextDeviceCheck = 0.0;
        F64                       mReopenNotBefore = 0.0;
        S32                       mReopenFailures = 0;
        S32                       mDisconnectedReads = 0;   // consecutive ALC_CONNECTED == 0 polls
        bool                      mPollGaveUp = false;      // a poll reopen did not restore ALC_CONNECTED
        bool                      mWatchEnabled = true;     // the WolfViewerAudioDeviceWatch setting
        S32                       mReopens = 0;             // reopens this session, event or poll
        // THE SAFETY VALVE. w30 shipped with the poll alone and a Windows 10 machine whose backend
        // read "disconnected" permanently died after ~10 minutes: ~300 WASAPI reopens at one every
        // 2 s, each creating a COM client, a mixer thread and handles. No driver behaviour may turn
        // this watch into a loop again: at most REOPEN_MAX reopens a session, never two within
        // REOPEN_GAP_SECS, and past the cap the watch switches itself off and says so.
        static constexpr S32 REOPEN_MAX = 20;
        static constexpr F64 REOPEN_GAP_SECS = 5.0;
        F64                       mNextPollReopen = 0.0;    // the poll may reopen once a minute at most

        typedef F32 WIND_SAMPLE_T;
        LLWindGen<WIND_SAMPLE_T> *mWindGen;
        F32 *mWindBuf;
        U32 mWindBufFreq;
        U32 mWindBufSamples;
        U32 mWindBufBytes;
        ALuint mWindSource;
        int mNumEmptyWindALBuffers;

        static const int MAX_NUM_WIND_BUFFERS = 80;
        static const float WIND_BUFFER_SIZE_SEC; // 1/20th sec
};

class LLAudioChannelOpenAL : public LLAudioChannel
{
    public:
        LLAudioChannelOpenAL();
        virtual ~LLAudioChannelOpenAL();
    protected:
        /*virtual*/ void play();
        /*virtual*/ void playSynced(LLAudioChannel *channelp);
        /*virtual*/ void cleanup();
        /*virtual*/ bool isPlaying();

        /*virtual*/ bool updateBuffer();
        /*virtual*/ void update3DPosition();
        /*virtual*/ void updateLoop();

        ALuint mALSource;
            ALint mLastSamplePos;
};

class LLAudioBufferOpenAL : public LLAudioBuffer{
    public:
        LLAudioBufferOpenAL();
        virtual ~LLAudioBufferOpenAL();

        bool loadWAV(const std::string& filename);
        U32 getLength();

        friend class LLAudioChannelOpenAL;
    protected:
        void cleanup();
        ALuint getBuffer() {return mALBuffer;}

        ALuint mALBuffer;
};

#endif
