/**
 * @file audioengine_openal.cpp
 * @brief implementation of audio engine using OpenAL
 * support as a OpenAL 3D implementation
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

#include "linden_common.h"
#include "lldir.h"
#include "llframetimer.h"   // <WolfViewer 2026-09-13> device watch clock

#include "llaudioengine_openal.h"
#include "lllistener_openal.h"


const float LLAudioEngine_OpenAL::WIND_BUFFER_SIZE_SEC = 0.05f;

LLAudioEngine_OpenAL::LLAudioEngine_OpenAL()
    :
    mWindGen(NULL),
    mWindBuf(NULL),
    mWindBufFreq(0),
    mWindBufSamples(0),
    mWindBufBytes(0),
    mWindSource(AL_NONE),
    mNumEmptyWindALBuffers(MAX_NUM_WIND_BUFFERS)
{
}

// virtual
LLAudioEngine_OpenAL::~LLAudioEngine_OpenAL()
{
}

// virtual
bool LLAudioEngine_OpenAL::init(void* userdata, const std::string &app_title)
{
    mWindGen = NULL;
    LLAudioEngine::init(userdata, app_title);

    if(!alutInit(NULL, NULL))
    {
        LL_WARNS() << "LLAudioEngine_OpenAL::init() ALUT initialization failed: " << alutGetErrorString (alutGetError ()) << LL_ENDL;
        return false;
    }

    LL_INFOS() << "LLAudioEngine_OpenAL::init() OpenAL successfully initialized" << LL_ENDL;

    LL_INFOS() << "OpenAL version: "
        << ll_safe_string(alGetString(AL_VERSION)) << LL_ENDL;
    LL_INFOS() << "OpenAL vendor: "
        << ll_safe_string(alGetString(AL_VENDOR)) << LL_ENDL;
    LL_INFOS() << "OpenAL renderer: "
        << ll_safe_string(alGetString(AL_RENDERER)) << LL_ENDL;

    ALint major = alutGetMajorVersion ();
    ALint minor = alutGetMinorVersion ();
    LL_INFOS() << "ALUT version: " << major << "." << minor << LL_ENDL;

    ALCdevice *device = alcGetContextsDevice(alcGetCurrentContext());

    alcGetIntegerv(device, ALC_MAJOR_VERSION, 1, &major);
    alcGetIntegerv(device, ALC_MINOR_VERSION, 1, &minor);
    LL_INFOS() << "ALC version: " << major << "." << minor << LL_ENDL;

    LL_INFOS() << "ALC default device: "
        << ll_safe_string(alcGetString(device,
                           ALC_DEFAULT_DEVICE_SPECIFIER))
        << LL_ENDL;

    initDeviceWatch(device);   // <WolfViewer 2026-09-13>

    return true;
}

// <WolfViewer 2026-09-13> ─── output-device hot-plug ─────────────────────────────────────────
// See the note on these members in llaudioengine_openal.h.

void LLAudioEngine_OpenAL::initDeviceWatch(ALCdevice* device)
{
    if (!device)
    {
        return;
    }
    // Source: alc.h:242 alcIsExtensionPresent, alc.h:247 alcGetProcAddress. The names are the
    // ones the shipped library advertises (strings libopenal.so.1.24.2: ALC_SOFT_reopen_device,
    // ALC_SOFT_system_events, ALC_EXT_disconnect).
    if (alcIsExtensionPresent(device, "ALC_SOFT_reopen_device"))
    {
        mReopenDeviceSOFT = (LPALCREOPENDEVICESOFT)alcGetProcAddress(device, "alcReopenDeviceSOFT");
    }
    mHasDisconnectExt = alcIsExtensionPresent(device, "ALC_EXT_disconnect") == ALC_TRUE;
    if (alcIsExtensionPresent(device, "ALC_SOFT_system_events"))
    {
        mEventControlSOFT     = (LPALCEVENTCONTROLSOFT)alcGetProcAddress(device, "alcEventControlSOFT");
        mEventCallbackSOFT    = (LPALCEVENTCALLBACKSOFT)alcGetProcAddress(device, "alcEventCallbackSOFT");
        mEventIsSupportedSOFT = (LPALCEVENTISSUPPORTEDSOFT)alcGetProcAddress(device, "alcEventIsSupportedSOFT");
    }
    LL_INFOS() << "OpenAL device watch: reopen=" << (mReopenDeviceSOFT ? "yes" : "no")
               << " events=" << (mEventControlSOFT && mEventCallbackSOFT ? "yes" : "no")
               << " disconnect=" << (mHasDisconnectExt ? "yes" : "no") << LL_ENDL;
    if (!mReopenDeviceSOFT)
    {
        // Without the reopen entry point a lost device can only be logged about. Watch off.
        return;
    }
    if (mEventControlSOFT && mEventCallbackSOFT)
    {
        // Source: alext.h:727 the default-device event; alext.h:739-740 the two calls.
        // ONLY the default-device change is asked for. This engine always opens the DEFAULT
        // device (alutInit(NULL, NULL)), and both directions of a hot-plug move the default —
        // headset out: the system falls back to the speakers; headset in: it comes back — so
        // that one event is the whole story. DEVICE_ADDED / DEVICE_REMOVED would also fire for
        // devices that are not ours and re-open the output for nothing, a click for no reason.
        // alcEventIsSupportedSOFT (alext.h:738) says whether this backend can raise it; if it
        // cannot, the ALC_CONNECTED poll in watchDevice() is what remains.
        std::vector<ALCenum> events;
        const ALCenum wanted[] = { ALC_EVENT_TYPE_DEFAULT_DEVICE_CHANGED_SOFT };
        for (ALCenum ev : wanted)
        {
            if (!mEventIsSupportedSOFT
                || mEventIsSupportedSOFT(ev, ALC_PLAYBACK_DEVICE_SOFT) == ALC_EVENT_SUPPORTED_SOFT)
            {
                events.push_back(ev);
            }
        }
        if (!events.empty())
        {
            mEventCallbackSOFT(&LLAudioEngine_OpenAL::onDeviceEvent, this);
            if (mEventControlSOFT((ALCsizei)events.size(), events.data(), ALC_TRUE) == ALC_TRUE)
            {
                mEventsArmed = true;
            }
            else
            {
                mEventCallbackSOFT(nullptr, nullptr);
            }
        }
        LL_INFOS() << "OpenAL device watch: " << events.size() << " event type(s) "
                   << (mEventsArmed ? "armed" : "refused") << LL_ENDL;
    }
    mNextDeviceCheck = LLFrameTimer::getElapsedSeconds() + 2.0;
}

void LLAudioEngine_OpenAL::shutdownDeviceWatch()
{
    if (mEventsArmed && mEventControlSOFT && mEventCallbackSOFT)
    {
        const ALCenum all[] = { ALC_EVENT_TYPE_DEFAULT_DEVICE_CHANGED_SOFT };
        mEventControlSOFT(1, all, ALC_FALSE);
        mEventCallbackSOFT(nullptr, nullptr);
    }
    mEventsArmed = false;
    mReopenDeviceSOFT = nullptr;
    mEventControlSOFT = nullptr;
    mEventCallbackSOFT = nullptr;
    mEventIsSupportedSOFT = nullptr;
}

// static — runs on OpenAL's thread (alext.h:732 ALCEVENTPROCTYPESOFT). No engine calls here.
void ALC_APIENTRY LLAudioEngine_OpenAL::onDeviceEvent(ALCenum eventType, ALCenum deviceType, ALCdevice* /*device*/,
                                                       ALCsizei /*length*/, const ALCchar* /*message*/, void* userParam) ALC_API_NOEXCEPT17
{
    if (deviceType != ALC_PLAYBACK_DEVICE_SOFT)
    {
        return;
    }
    if (eventType == ALC_EVENT_TYPE_DEFAULT_DEVICE_CHANGED_SOFT)
    {
        LLAudioEngine_OpenAL* self = static_cast<LLAudioEngine_OpenAL*>(userParam);
        if (self)
        {
            self->mDeviceChanged.store(true);
        }
    }
}

void LLAudioEngine_OpenAL::setDeviceWatchEnabled(bool enabled)
{
    if (mWatchEnabled != enabled)
    {
        LL_INFOS() << "OpenAL device watch " << (enabled ? "enabled" : "disabled") << " by preference" << LL_ENDL;
    }
    mWatchEnabled = enabled;
    if (!enabled)
    {
        mDeviceChanged.store(false);
        mDisconnectedReads = 0;
    }
}

void LLAudioEngine_OpenAL::watchDevice()
{
    if (!mReopenDeviceSOFT || !mWatchEnabled)
    {
        return;
    }
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (mDeviceChanged.exchange(false))
    {
        // The default device changed. Re-opening on the default covers both directions:
        // headset out (follow the system to the speakers) and headset back in (follow it
        // back). A burst of events collapses into one reopen.
        if (now >= mReopenNotBefore)
        {
            reopenDevice("device event");
        }
        else
        {
            mDeviceChanged.store(true);   // keep it pending until the back-off expires
        }
    }
    if (mHasDisconnectExt && now >= mNextDeviceCheck)
    {
        mNextDeviceCheck = now + 2.0;
        ALCdevice* device = alcGetContextsDevice(alcGetCurrentContext());
        if (device)
        {
            // Source: alext.h:160 ALC_CONNECTED — 0 once the device has gone away.
            ALCint connected = 1;
            alcGetIntegerv(device, ALC_CONNECTED, 1, &connected);
            // [2026-09-13] TWO consecutive "not connected" reads, and never more than one
            // poll-driven reopen a minute. A backend that mis-reports the flag (reported the
            // day w30 shipped: a Windows 10 user dropped from a busy region within minutes, w29
            // fine) must not turn this into a reopen every two seconds — each WASAPI reopen can
            // stall the main thread, and enough stalls in a row cost the circuit. The event
            // path above is unaffected: a real default-device change still reopens at once.
            if (connected)
            {
                mDisconnectedReads = 0;
                mPollGaveUp = false;
            }
            else
            {
                ++mDisconnectedReads;
            }
            // And if a poll-driven reopen did NOT bring ALC_CONNECTED back, the flag is not
            // telling the truth for this backend: stop reopening on its say-so altogether
            // (until it reads connected again). The event path is still live.
            if (mNextPollReopen > 0.0 && !connected && mDisconnectedReads == 1 && !mPollGaveUp && now < mNextPollReopen)
            {
                mPollGaveUp = true;
                LL_WARNS() << "OpenAL: the device still reads disconnected after a reopen — ignoring ALC_CONNECTED from now on" << LL_ENDL;
            }
            if (mDisconnectedReads >= 2 && !mPollGaveUp && now >= mReopenNotBefore && now >= mNextPollReopen)
            {
                mNextPollReopen = now + 60.0;
                mDisconnectedReads = 0;
                reopenDevice("device disconnected");
            }
        }
    }
}

void LLAudioEngine_OpenAL::reopenDevice(const char* why)
{
    ALCdevice* device = alcGetContextsDevice(alcGetCurrentContext());
    if (!device || !mReopenDeviceSOFT)
    {
        return;
    }
    // Source: alext.h:573 LPALCREOPENDEVICESOFT(device, deviceName, attribs): NULL name = the
    // default device, NULL attribs = keep the context's current attributes.
    const ALCboolean ok = mReopenDeviceSOFT(device, nullptr, nullptr);
    if (ok == ALC_TRUE)
    {
        mReopenFailures = 0;
        mReopenNotBefore = LLFrameTimer::getElapsedSeconds() + 1.0;
        LL_INFOS() << "OpenAL: re-opened the default output device (" << why << "): "
                   << ll_safe_string(alcGetString(device, ALC_ALL_DEVICES_SPECIFIER)) << LL_ENDL;
    }
    else
    {
        // The default device may itself be mid-change (nothing to open yet). Back off and let
        // the next event or poll try again; never spin on a failing open.
        ++mReopenFailures;
        mReopenNotBefore = LLFrameTimer::getElapsedSeconds() + llmin(30.0, 2.0 * mReopenFailures);
        const ALCenum err = alcGetError(device);
        LL_WARNS() << "OpenAL: could not re-open the output device (" << why << "), ALC error 0x"
                   << std::hex << err << std::dec << LL_ENDL;
    }
}

// virtual
void LLAudioEngine_OpenAL::idle()
{
    LLAudioEngine::idle();
    watchDevice();
}
// </WolfViewer 2026-09-13>

// virtual
std::string LLAudioEngine_OpenAL::getDriverName(bool verbose)
{
    ALCdevice *device = alcGetContextsDevice(alcGetCurrentContext());
    std::ostringstream version;

    version <<
        "OpenAL";

    if (verbose)
    {
        version <<
            ", version " <<
            ll_safe_string(alGetString(AL_VERSION)) <<
            " / " <<
            ll_safe_string(alGetString(AL_VENDOR)) <<
            " / " <<
            ll_safe_string(alGetString(AL_RENDERER));

        if (device)
            version <<
                ": " <<
                ll_safe_string(alcGetString(device,
                    ALC_DEFAULT_DEVICE_SPECIFIER));
    }

    return version.str();
}

// virtual
void LLAudioEngine_OpenAL::allocateListener()
{
    mListenerp = (LLListener *) new LLListener_OpenAL();
    if(!mListenerp)
    {
        LL_WARNS() << "LLAudioEngine_OpenAL::allocateListener() Listener creation failed" << LL_ENDL;
    }
}

// virtual
void LLAudioEngine_OpenAL::shutdown()
{
    LL_INFOS() << "About to LLAudioEngine::shutdown()" << LL_ENDL;
    shutdownDeviceWatch();   // <WolfViewer 2026-09-13> no callback may fire into a dead engine
    LLAudioEngine::shutdown();

    // If a subsequent error occurs while there is still an error recorded
    // internally, the second error will simply be ignored.
    // Clear previous error to make sure we will captuare a valid failure reason
    ALenum error = alutGetError();
    if (error != ALUT_ERROR_NO_ERROR)
    {
        LL_WARNS() << "Uncleared error state prior to shutdown: "
            << alutGetErrorString(error) << LL_ENDL;
    }

    LL_INFOS() << "About to alutExit()" << LL_ENDL;
    if(!alutExit())
    {
        LL_WARNS() << "LLAudioEngine_OpenAL::shutdown() ALUT shutdown failed: " << alutGetErrorString (alutGetError ()) << LL_ENDL;
    }

    LL_INFOS() << "LLAudioEngine_OpenAL::shutdown() OpenAL successfully shut down" << LL_ENDL;

    delete mListenerp;
    mListenerp = NULL;
}

LLAudioBuffer *LLAudioEngine_OpenAL::createBuffer()
{
    return new LLAudioBufferOpenAL();
}

LLAudioChannel *LLAudioEngine_OpenAL::createChannel()
{
    return new LLAudioChannelOpenAL();
}

void LLAudioEngine_OpenAL::setInternalGain(F32 gain)
{
    //LL_INFOS() << "LLAudioEngine_OpenAL::setInternalGain() Gain: " << gain << LL_ENDL;
    alListenerf(AL_GAIN, gain);
}

LLAudioChannelOpenAL::LLAudioChannelOpenAL()
    :
    mALSource(AL_NONE),
    mLastSamplePos(0)
{
    alGenSources(1, &mALSource);
}

LLAudioChannelOpenAL::~LLAudioChannelOpenAL()
{
    cleanup();
    alDeleteSources(1, &mALSource);
}

void LLAudioChannelOpenAL::cleanup()
{
    alSourceStop(mALSource);
    alSourcei(mALSource, AL_BUFFER, AL_NONE);

    mCurrentBufferp = NULL;
}

void LLAudioChannelOpenAL::play()
{
    if (mALSource == AL_NONE)
    {
        LL_WARNS() << "Playing without a mALSource, aborting" << LL_ENDL;
        return;
    }

    if(!isPlaying())
    {
        alSourcePlay(mALSource);
        getSource()->setPlayedOnce(true);
    }
}

void LLAudioChannelOpenAL::playSynced(LLAudioChannel *channelp)
{
    if (channelp)
    {
        LLAudioChannelOpenAL *masterchannelp =
            (LLAudioChannelOpenAL*)channelp;
        if (mALSource != AL_NONE &&
            masterchannelp->mALSource != AL_NONE)
        {
            // we have channels allocated to master and slave
            ALfloat master_offset;
            alGetSourcef(masterchannelp->mALSource, AL_SEC_OFFSET,
                     &master_offset);

            LL_INFOS() << "Syncing with master at " << master_offset
                << "sec" << LL_ENDL;
            // *TODO: detect when this fails, maybe use AL_SAMPLE_
            alSourcef(mALSource, AL_SEC_OFFSET, master_offset);
        }
    }
    play();
}

bool LLAudioChannelOpenAL::isPlaying()
{
    if (mALSource != AL_NONE)
    {
        ALint state;
        alGetSourcei(mALSource, AL_SOURCE_STATE, &state);
        if(state == AL_PLAYING)
        {
            return true;
        }
    }

    return false;
}

bool LLAudioChannelOpenAL::updateBuffer()
{
    if (!mCurrentSourcep)
    {
        // This channel isn't associated with any source, nothing
        // to be updated
        return false;
    }

    if (LLAudioChannel::updateBuffer())
    {
        // Base class update returned true, which means that we need to actually
        // set up the source for a different buffer.
        LLAudioBufferOpenAL *bufferp = (LLAudioBufferOpenAL *)mCurrentSourcep->getCurrentBuffer();
        ALuint buffer = bufferp->getBuffer();
        alSourcei(mALSource, AL_BUFFER, buffer);
        mLastSamplePos = 0;
    }

    if (mCurrentSourcep)
    {
        alSourcef(mALSource, AL_GAIN,
              mCurrentSourcep->getGain() * getSecondaryGain());
        alSourcei(mALSource, AL_LOOPING,
              mCurrentSourcep->isLoop() ? AL_TRUE : AL_FALSE);
        alSourcef(mALSource, AL_ROLLOFF_FACTOR,
              gAudiop->mListenerp->getRolloffFactor());
    }

    return true;
}


void LLAudioChannelOpenAL::updateLoop()
{
    if (mALSource == AL_NONE)
    {
        return;
    }

    // Hack:  We keep track of whether we looped or not by seeing when the
    // sample position looks like it's going backwards.  Not reliable; may
    // yield false negatives.
    //
    ALint cur_pos;
    alGetSourcei(mALSource, AL_SAMPLE_OFFSET, &cur_pos);
    if (cur_pos < mLastSamplePos)
    {
        mLoopedThisFrame = true;
    }
    mLastSamplePos = cur_pos;
}


void LLAudioChannelOpenAL::update3DPosition()
{
    if(!mCurrentSourcep)
    {
        return;
    }
    if (mCurrentSourcep->isForcedPriority())
    {
        alSource3f(mALSource, AL_POSITION, 0.0, 0.0, 0.0);
        alSource3f(mALSource, AL_VELOCITY, 0.0, 0.0, 0.0);
        alSourcei (mALSource, AL_SOURCE_RELATIVE, AL_TRUE);
    } else {
        LLVector3 float_pos;
        float_pos.setVec(mCurrentSourcep->getPositionGlobal());
        alSourcefv(mALSource, AL_POSITION, float_pos.mV);
        alSourcefv(mALSource, AL_VELOCITY, mCurrentSourcep->getVelocity().mV);
        alSourcei (mALSource, AL_SOURCE_RELATIVE, AL_FALSE);
    }

    alSourcef(mALSource, AL_GAIN, mCurrentSourcep->getGain() * getSecondaryGain());
}

LLAudioBufferOpenAL::LLAudioBufferOpenAL()
{
    mALBuffer = AL_NONE;
}

LLAudioBufferOpenAL::~LLAudioBufferOpenAL()
{
    cleanup();
}

void LLAudioBufferOpenAL::cleanup()
{
    if(mALBuffer != AL_NONE)
    {
        alGetError(); // <ND/>
        alDeleteBuffers(1, &mALBuffer);

        // <FS:ND> Print warning on possible leak.
        ALenum error = alGetError();
        if( AL_NO_ERROR != error )
            LL_WARNS() << "openal error: " << error << " possible memory leak hit" << LL_ENDL;
        // </FS:ND>

        mALBuffer = AL_NONE;
    }
}

bool LLAudioBufferOpenAL::loadWAV(const std::string& filename)
{
    cleanup();
    mALBuffer = alutCreateBufferFromFile(filename.c_str());
    if(mALBuffer == AL_NONE)
    {
        ALenum error = alutGetError();
        if (gDirUtilp->fileExists(filename))
        {
            LL_WARNS() <<
                "LLAudioBufferOpenAL::loadWAV() Error loading "
                << filename
                << " " << alutGetErrorString(error) << LL_ENDL;
        }
        else
        {
            // It's common for the file to not actually exist.
            LL_DEBUGS() <<
                "LLAudioBufferOpenAL::loadWAV() Error loading "
                 << filename
                 << " " << alutGetErrorString(error) << LL_ENDL;
        }
        return false;
    }

    return true;
}

U32 LLAudioBufferOpenAL::getLength()
{
    if(mALBuffer == AL_NONE)
    {
        return 0;
    }
    ALint length;
    alGetBufferi(mALBuffer, AL_SIZE, &length);
    return length / 2; // convert size in bytes to size in (16-bit) samples
}

// ------------

bool LLAudioEngine_OpenAL::initWind()
{
    ALenum error;
    LL_INFOS() << "LLAudioEngine_OpenAL::initWind() start" << LL_ENDL;

    mNumEmptyWindALBuffers = MAX_NUM_WIND_BUFFERS;

    alGetError(); /* clear error */

    alGenSources(1,&mWindSource);

    if((error=alGetError()) != AL_NO_ERROR)
    {
        LL_WARNS() << "LLAudioEngine_OpenAL::initWind() Error creating wind sources: "<<error<<LL_ENDL;
    }

    mWindGen = new LLWindGen<WIND_SAMPLE_T>;

    mWindBufFreq = mWindGen->getInputSamplingRate();
    mWindBufSamples = llceil(mWindBufFreq * WIND_BUFFER_SIZE_SEC);
    mWindBufBytes = mWindBufSamples * 2 /*stereo*/ * sizeof(WIND_SAMPLE_T);

    mWindBuf = new WIND_SAMPLE_T [mWindBufSamples * 2 /*stereo*/];

    if(mWindBuf==NULL)
    {
        LL_ERRS() << "LLAudioEngine_OpenAL::initWind() Error creating wind memory buffer" << LL_ENDL;
        return false;
    }

    LL_INFOS() << "LLAudioEngine_OpenAL::initWind() done" << LL_ENDL;

    return true;
}

void LLAudioEngine_OpenAL::cleanupWind()
{
    LL_INFOS() << "LLAudioEngine_OpenAL::cleanupWind()" << LL_ENDL;

    if (mWindSource != AL_NONE)
    {
        // detach and delete all outstanding buffers on the wind source
        alSourceStop(mWindSource);
        ALint processed;
        alGetSourcei(mWindSource, AL_BUFFERS_PROCESSED, &processed);
        while (processed--)
        {
            ALuint buffer = AL_NONE;
            alSourceUnqueueBuffers(mWindSource, 1, &buffer);
            alDeleteBuffers(1, &buffer);
        }

        // delete the wind source itself
        alDeleteSources(1, &mWindSource);

        mWindSource = AL_NONE;
    }

    delete[] mWindBuf;
    mWindBuf = NULL;

    delete mWindGen;
    mWindGen = NULL;
}

void LLAudioEngine_OpenAL::updateWind(LLVector3 wind_vec, F32 camera_altitude)
{
    LLVector3 wind_pos;
    F64 pitch;
    F64 center_freq;
    ALenum error;

    if (!mEnableWind)
        return;

    if(!mWindBuf)
        return;

    if (mWindUpdateTimer.checkExpirationAndReset(LL_WIND_UPDATE_INTERVAL))
    {

        // wind comes in as Linden coordinate (+X = forward, +Y = left, +Z = up)
        // need to convert this to the conventional orientation DS3D and OpenAL use
        // where +X = right, +Y = up, +Z = backwards

        wind_vec.setVec(-wind_vec.mV[1], wind_vec.mV[2], -wind_vec.mV[0]);

        pitch = 1.0 + mapWindVecToPitch(wind_vec);
        center_freq = 80.0 * pow(pitch,2.5*(mapWindVecToGain(wind_vec)+1.0));

        mWindGen->mTargetFreq = (F32)center_freq;
        mWindGen->mTargetGain = (F32)mapWindVecToGain(wind_vec) * mMaxWindGain;
        mWindGen->mTargetPanGainR = (F32)mapWindVecToPan(wind_vec);

        alSourcei(mWindSource, AL_LOOPING, AL_FALSE);
        alSource3f(mWindSource, AL_POSITION, 0.0, 0.0, 0.0);
        alSource3f(mWindSource, AL_VELOCITY, 0.0, 0.0, 0.0);
        alSourcef(mWindSource, AL_ROLLOFF_FACTOR, 0.0);
        alSourcei(mWindSource, AL_SOURCE_RELATIVE, AL_TRUE);
    }

    // ok lets make a wind buffer now

    ALint processed, queued, unprocessed;
    alGetSourcei(mWindSource, AL_BUFFERS_PROCESSED, &processed);
    alGetSourcei(mWindSource, AL_BUFFERS_QUEUED, &queued);
    unprocessed = queued - processed;

    // ensure that there are always at least 3x as many filled buffers
    // queued as we managed to empty since last time.
    mNumEmptyWindALBuffers = llmin(mNumEmptyWindALBuffers + processed * 3 - unprocessed, MAX_NUM_WIND_BUFFERS-unprocessed);
    mNumEmptyWindALBuffers = llmax(mNumEmptyWindALBuffers, 0);

    //LL_INFOS() << "mNumEmptyWindALBuffers: " << mNumEmptyWindALBuffers    <<" (" << unprocessed << ":" << processed << ")" << LL_ENDL;

    while(processed--) // unqueue old buffers
    {
        ALuint buffer;
        ALenum error;
        alGetError(); /* clear error */
        alSourceUnqueueBuffers(mWindSource, 1, &buffer);
        error = alGetError();
        if(error != AL_NO_ERROR)
        {
            LL_WARNS() << "LLAudioEngine_OpenAL::updateWind() error swapping (unqueuing) buffers" << LL_ENDL;
        }
        else
        {
            alDeleteBuffers(1, &buffer);
        }
    }

    unprocessed += mNumEmptyWindALBuffers;
    while (mNumEmptyWindALBuffers > 0) // fill+queue new buffers
    {
        ALuint buffer;
        alGetError(); /* clear error */
        alGenBuffers(1,&buffer);
        if((error=alGetError()) != AL_NO_ERROR)
        {
            LL_WARNS() << "LLAudioEngine_OpenAL::updateWind() Error creating wind buffer: " << error << LL_ENDL;
            break;
        }

        alBufferData(buffer,
                 AL_FORMAT_STEREO_FLOAT32,
                 mWindGen->windGenerate(mWindBuf,
                            mWindBufSamples),
                 mWindBufBytes,
                 mWindBufFreq);
        error = alGetError();
        if(error != AL_NO_ERROR)
        {
            LL_WARNS() << "LLAudioEngine_OpenAL::updateWind() error swapping (bufferdata) buffers" << LL_ENDL;
        }

        alSourceQueueBuffers(mWindSource, 1, &buffer);
        error = alGetError();
        if(error != AL_NO_ERROR)
        {
            LL_WARNS() << "LLAudioEngine_OpenAL::updateWind() error swapping (queuing) buffers" << LL_ENDL;
        }

        --mNumEmptyWindALBuffers;
    }

    ALint playing;
    alGetSourcei(mWindSource, AL_SOURCE_STATE, &playing);
    if(playing != AL_PLAYING)
    {
        alSourcePlay(mWindSource);

        LL_DEBUGS() << "Wind had stopped - probably ran out of buffers - restarting: " << (unprocessed+mNumEmptyWindALBuffers) << " now queued." << LL_ENDL;
    }
}

