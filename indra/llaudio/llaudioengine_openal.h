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
        virtual void idle();   // "virtual", not "override": clang -Winconsistent-missing-override (-Werror on macOS) wants the whole class one way

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
