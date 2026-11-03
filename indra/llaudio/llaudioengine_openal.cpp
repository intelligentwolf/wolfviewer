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
#include <cstring>          // <WolfViewer 2026-09-18> strlen over the NUL-separated device list
#include <cstdlib>          // <WolfViewer 2026-10-30> getenv/setenv (ticket 379)

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

#if LL_LINUX
    // <WolfViewer 2026-10-30> [ticket 379] On a PipeWire desktop, play through OpenAL Soft's PulseAudio backend
    // (pipewire-pulse) rather than its native PipeWire one, so the viewer's sound effects and UI sounds go where the
    // system sends every other program's sound.
    // Source: openal-soft 1.24.2 alc/backends/pipewire.cpp:1526-1561 PipeWirePlayback::open - the default device is
    //   opened by pinning the stream to the current default sink's serial, and :1687-1688 always sets
    //   PW_KEY_TARGET_OBJECT on the stream.
    // Source: WirePlumber 0.5 src/scripts/node/state-stream.lua:92-117 - a stream that names its own target.object
    //   does NOT get the output the user last moved this program to (pavucontrol / GNOME "Volume Levels").
    // Ticket 379 (Fedora 44): his log shows WolfViewer's OpenAL output on the default sink, the monitor's HDMI
    //   output, while he heard the music stream (GStreamer, no pinned target) - sounds and music on different outputs.
    // Source: openal-soft 1.24.2 alc/backends/pulseaudio.cpp:844-906 PulsePlayback::open - an empty name connects
    //   with no sink, so the sound server picks it (the remembered output for this program, else the default) and
    //   the stream follows later default changes; a device chosen in Preferences is still opened by name.
    // Source: openal-soft 1.24.2 alc/alc.cpp:594-648 - ALSOFT_DRIVERS is read before the config file's "drivers";
    //   a list without a trailing comma is exactly the backends named. The list below is alc.cpp:219-269's own
    //   order with pulse first and the null/wave writers left out, as the default list leaves them out
    //   (alc.cpp:650-663); unknown or not-built names are skipped (alc.cpp:636-639). Pure PulseAudio systems are
    //   unchanged (the PipeWire backend never initialised there); without a pulse server, pipewire is next.
    // A value the user set themselves is kept.
    if (!getenv("ALSOFT_DRIVERS"))
    {
        setenv("ALSOFT_DRIVERS", "pulse,pipewire,alsa,solaris,sndio,oss,port,sdl3,sdl2,jack,otherio", 0);
    }
#endif

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
    // <WolfViewer 2026-10-30/> [ticket 379] the output actually opened (the line above names the library, not the
    // device): alcGetString(device, ALC_ALL_DEVICES_SPECIFIER) on an open device is that device's name (alc.h).
    const char* drivers = getenv("ALSOFT_DRIVERS");
    LL_INFOS() << "OpenAL output device: " << ll_safe_string(alcGetString(device, ALC_ALL_DEVICES_SPECIFIER))
               << " (ALSOFT_DRIVERS=" << (drivers ? drivers : "unset") << ")" << LL_ENDL;

    initDeviceWatch(device);   // <WolfViewer 2026-09-13>
    enumerateDevices(false);   // <WolfViewer 2026-09-18> the Preferences picker's list; setDevice() follows from llstartup

    // [VR AUDIO 2026-11-04] see setMirrorMix in the header; on any failure the engine plays as it always has
    if (sMirrorMixWanted && !initMirrorMix())
    {
        LL_WARNS() << "OpenAL: the VR headset sound mirror could not start; sound plays on this PC only" << LL_ENDL;
    }

    return true;
}

// [VR AUDIO 2026-11-04] ─── the final mix for the VR headset ─────────────────────────────────────────
bool LLAudioEngine_OpenAL::sMirrorMixWanted = false;

ALCdevice* LLAudioEngine_OpenAL::outputDevice() const
{
    return mOutDevice ? mOutDevice : alcGetContextsDevice(alcGetCurrentContext());
}

void LLAudioEngine_OpenAL::setMixTap(MixTapFn fn, void* user)
{
    // user first: the mixer thread reads fn, then user (fn is only ever set with the one user it was given)
    if (fn) mMixTapUser.store(user);
    mMixTap.store(fn);
}

bool LLAudioEngine_OpenAL::initMirrorMix()
{
    ALCcontext* out_ctx = alcGetCurrentContext();   // alutInit's context on the speakers
    ALCdevice* out_dev = alcGetContextsDevice(out_ctx);
    if (!out_ctx || !out_dev) return false;
    // Source: alext.h:286-301 ALC_SOFT_loopback (device-independent: queried on NULL), alext.h:163-167
    // ALC_EXT_thread_local_context, alext.h:581-588 AL_SOFT_callback_buffer (an AL extension: needs a context).
    if (alcIsExtensionPresent(nullptr, "ALC_SOFT_loopback") != ALC_TRUE ||
        alcIsExtensionPresent(nullptr, "ALC_EXT_thread_local_context") != ALC_TRUE ||
        alIsExtensionPresent("AL_SOFT_callback_buffer") != AL_TRUE)
    {
        LL_WARNS() << "OpenAL: no loopback / thread-context / callback-buffer extension" << LL_ENDL;
        return false;
    }
    auto loopOpen = (LPALCLOOPBACKOPENDEVICESOFT)alcGetProcAddress(nullptr, "alcLoopbackOpenDeviceSOFT");
    auto formatOk = (LPALCISRENDERFORMATSUPPORTEDSOFT)alcGetProcAddress(nullptr, "alcIsRenderFormatSupportedSOFT");
    mRenderSamplesSOFT = (LPALCRENDERSAMPLESSOFT)alcGetProcAddress(nullptr, "alcRenderSamplesSOFT");
    mSetThreadContext = (PFNALCSETTHREADCONTEXTPROC)alcGetProcAddress(nullptr, "alcSetThreadContext");
    auto bufferCallback = (LPALBUFFERCALLBACKSOFT)alGetProcAddress("alBufferCallbackSOFT");
    if (!loopOpen || !formatOk || !mRenderSamplesSOFT || !mSetThreadContext || !bufferCallback) return false;

    ALCdevice* loop = loopOpen(nullptr);
    if (!loop) return false;
    // Source: alext.h:292-307 - 32-bit float stereo; alc.h:119 ALC_FREQUENCY
    if (formatOk(loop, (ALCsizei)MIRROR_RATE, ALC_STEREO_SOFT, ALC_FLOAT_SOFT) != ALC_TRUE)
    {
        alcCloseDevice(loop);
        return false;
    }
    // Keep the speakers' HRTF choice (OpenAL Soft switches HRTF on by itself for headphones; a loopback device
    // cannot know what it will play on). Source: alext.h:440-447 ALC_HRTF_SOFT on a device, ALC_TRUE/ALC_FALSE.
    ALCint hrtf = ALC_FALSE;
    if (alcIsExtensionPresent(out_dev, "ALC_SOFT_HRTF") == ALC_TRUE)
    {
        alcGetIntegerv(out_dev, ALC_HRTF_SOFT, 1, &hrtf);
    }
    const ALCint attrs[] = { ALC_FORMAT_CHANNELS_SOFT, ALC_STEREO_SOFT, ALC_FORMAT_TYPE_SOFT, ALC_FLOAT_SOFT,
                             ALC_FREQUENCY, (ALCint)MIRROR_RATE, ALC_HRTF_SOFT, hrtf ? ALC_TRUE : ALC_FALSE, 0 };
    ALCcontext* loop_ctx = alcCreateContext(loop, attrs);
    if (!loop_ctx)
    {
        alcCloseDevice(loop);
        return false;
    }

    // The speakers' one source, made on their context through this thread's own context so the process-wide
    // current context is never the speakers' one. Source: alext.h:165 alcSetThreadContext; alext.h:53-57
    // AL_FORMAT_STEREO_FLOAT32; alext.h:283 AL_DIRECT_CHANNELS_SOFT (stereo straight to the front pair, no
    // panning or HRTF applied twice).
    mSetThreadContext(out_ctx);
    alGetError();
    alGenBuffers(1, &mOutBuffer);
    alGenSources(1, &mOutSource);
    bufferCallback(mOutBuffer, AL_FORMAT_STEREO_FLOAT32, (ALsizei)MIRROR_RATE, &LLAudioEngine_OpenAL::onOutputPull, this);
    alSourcei(mOutSource, AL_BUFFER, (ALint)mOutBuffer);
    alSourcei(mOutSource, AL_SOURCE_RELATIVE, AL_TRUE);
    alSourcei(mOutSource, AL_DIRECT_CHANNELS_SOFT, AL_TRUE);
    const ALenum err = alGetError();
    mSetThreadContext(nullptr);
    if (err != AL_NO_ERROR)
    {
        mSetThreadContext(out_ctx);
        alDeleteSources(1, &mOutSource);
        alDeleteBuffers(1, &mOutBuffer);
        mSetThreadContext(nullptr);
        mOutSource = mOutBuffer = 0;
        alcDestroyContext(loop_ctx);
        alcCloseDevice(loop);
        LL_WARNS() << "OpenAL: the speakers' mirror source failed, AL error 0x" << std::hex << err << std::dec << LL_ENDL;
        return false;
    }
    mOutDevice = out_dev;
    mOutContext = out_ctx;
    mLoopDevice = loop;
    mLoopContext = loop_ctx;
    // From here every engine call (sources, listener, wind, gain) is on the loopback context.
    alcMakeContextCurrent(loop_ctx);
    mSetThreadContext(out_ctx);
    alSourcePlay(mOutSource);
    mSetThreadContext(nullptr);
    LL_INFOS() << "OpenAL: VR headset sound mirror on (loopback " << MIRROR_RATE << " Hz stereo, HRTF "
               << (hrtf ? "on" : "off") << ") playing on '" << ll_safe_string(alcGetString(out_dev, ALC_ALL_DEVICES_SPECIFIER))
               << "'" << LL_ENDL;
    return true;
}

void LLAudioEngine_OpenAL::shutdownMirrorMix()
{
    if (!mLoopDevice) return;
    mMixTap.store(nullptr);
    // Back to alutInit's own context, so alutExit closes what alutInit opened, exactly as without the mirror.
    alcMakeContextCurrent(mOutContext);
    alSourceStop(mOutSource);
    alDeleteSources(1, &mOutSource);
    alDeleteBuffers(1, &mOutBuffer);
    mOutSource = mOutBuffer = 0;
    alcDestroyContext(mLoopContext);
    alcCloseDevice(mLoopDevice);
    mLoopContext = nullptr;
    mLoopDevice = nullptr;
    mOutContext = nullptr;
    mOutDevice = nullptr;
}

// static - the speakers' mixer thread (alext.h:583 ALBUFFERCALLBACKTYPESOFT: return the bytes written; fewer than
// asked ends the stream, so always all of them). Renders the engine's mix for exactly this many frames.
ALsizei AL_APIENTRY LLAudioEngine_OpenAL::onOutputPull(ALvoid* user, ALvoid* data, ALsizei bytes) AL_API_NOEXCEPT17
{
    LLAudioEngine_OpenAL* self = static_cast<LLAudioEngine_OpenAL*>(user);
    const ALsizei frames = bytes / (ALsizei)(2 * sizeof(F32));
    if (!self || !self->mLoopDevice || frames <= 0)
    {
        memset(data, 0, (size_t)bytes);
        return bytes;
    }
    self->mRenderSamplesSOFT(self->mLoopDevice, data, frames);
    if (MixTapFn tap = self->mMixTap.load())
    {
        tap(static_cast<const F32*>(data), (U32)frames, MIRROR_RATE, self->mMixTapUser.load());
    }
    if (self->mSpeakersMuted.load())
    {
        memset(data, 0, (size_t)bytes);
    }
    return bytes;
}
// [/VR AUDIO]

// <WolfViewer 2026-09-18> ─── output-device selection ─────────────────────────────────────────
// See the note on getDevices/setDevice in llaudioengine_openal.h.

bool LLAudioEngine_OpenAL::enumerateDevices(bool announce)
{
    // Source: alc.h ALC_ENUMERATE_ALL_EXT - alcGetString(NULL, ALC_ALL_DEVICES_SPECIFIER) is a
    // list of NUL-terminated names ended by an empty name (double NUL). ALC_DEVICE_SPECIFIER is
    // the older, shorter list for a library without the extension.
    const bool all = alcIsExtensionPresent(nullptr, "ALC_ENUMERATE_ALL_EXT") == ALC_TRUE;
    const ALCchar* list = alcGetString(nullptr, all ? ALC_ALL_DEVICES_SPECIFIER : ALC_DEVICE_SPECIFIER);
    std::string sig;
    std::map<LLUUID, std::string> names;
    for (const ALCchar* p = list; p && *p; p += strlen(p) + 1)
    {
        const std::string name(p);
        LLUUID id;
        id.generate("wolfviewer-openal-output:" + name);
        names[id] = name;
        sig += name;
        sig += '\n';
    }
    const bool changed = sig != mDeviceListSig;
    mDeviceListSig = sig;
    mDeviceNames.swap(names);
    if (changed)
    {
        LL_INFOS() << "OpenAL: " << mDeviceNames.size() << " playback device(s)" << (all ? "" : " (ALC_ENUMERATE_ALL_EXT absent)") << LL_ENDL;
        for (const auto& kv : mDeviceNames) LL_INFOS() << "OpenAL:   '" << kv.second << "' " << kv.first << LL_ENDL;
        if (announce) OnOutputDeviceListChanged(mDeviceNames);
    }
    return changed;
}

const char* LLAudioEngine_OpenAL::selectedDeviceName() const
{
    if (mSelectedDevice.isNull() || mSelectedDeviceName.empty()) return nullptr;
    // A chosen device that is not plugged in right now: open the default instead, and come
    // back to it when the list shows it again (watchDevice).
    for (const auto& kv : mDeviceNames) if (kv.second == mSelectedDeviceName) return mSelectedDeviceName.c_str();
    return nullptr;
}

// virtual
LLAudioEngine::output_device_map_t LLAudioEngine_OpenAL::getDevices()
{
    enumerateDevices(false);
    return mDeviceNames;
}

// virtual
void LLAudioEngine_OpenAL::setDevice(const LLUUID& device_uuid)
{
    mSelectedDevice = device_uuid;
    enumerateDevices(false);
    auto it = mDeviceNames.find(device_uuid);
    mSelectedDeviceName = it != mDeviceNames.end() ? it->second : std::string();
    if (!device_uuid.isNull() && mSelectedDeviceName.empty())
    {
        LL_WARNS() << "OpenAL: the chosen output device " << device_uuid << " is not present; using the default until it is" << LL_ENDL;
    }
    ALCdevice* device = outputDevice()  /* [VR AUDIO 2026-11-04] the speakers */;
    if (!device || !mReopenDeviceSOFT)
    {
        // Before init, or no reopen extension: nothing to switch. (alutInit opened the default.)
        return;
    }
    const char* want = selectedDeviceName();
    // Already on it? alcGetString(device, ALC_ALL_DEVICES_SPECIFIER) names the OPEN device.
    const std::string open = ll_safe_string(alcGetString(device, ALC_ALL_DEVICES_SPECIFIER));
    if (want ? open == want : mSelectedDeviceName.empty() && !mSelectedDeviceOpen)
    {
        mSelectedDeviceOpen = want != nullptr;
        return;
    }
    // Source: alext.h:573 LPALCREOPENDEVICESOFT(device, deviceName, attribs) - a name opens THAT
    // device on the live context, keeping sources and buffers; NULL is the default.
    if (mReopenDeviceSOFT(device, want, nullptr) == ALC_TRUE)
    {
        mSelectedDeviceOpen = want != nullptr;
        LL_INFOS() << "OpenAL: output device is now '" << ll_safe_string(alcGetString(device, ALC_ALL_DEVICES_SPECIFIER))
                   << "' (" << (want ? "chosen" : "default") << ")" << LL_ENDL;
    }
    else
    {
        const ALCenum err = alcGetError(device);
        LL_WARNS() << "OpenAL: could not open '" << (want ? want : "(default)") << "', ALC error 0x" << std::hex << err << std::dec
                   << (want ? " - staying on the current device" : "") << LL_ENDL;
        mSelectedDeviceOpen = false;
    }
}
// </WolfViewer>

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
    // <WolfViewer 2026-09-18> The device LIST, for the Preferences picker: re-read every few
    // seconds (one alcGetString; no device is opened) and announced when it changes. And if the
    // chosen device has just come back while we are on the default, go back to it - the
    // default-device event below does not fire for a device that was never the default.
    if (now >= mNextListCheck)
    {
        mNextListCheck = now + 3.0;
        if (enumerateDevices(true) && !mSelectedDeviceOpen && selectedDeviceName() && now >= mReopenNotBefore)
        {
            reopenDevice("chosen device is back");
        }
    }
    // </WolfViewer>
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
        ALCdevice* device = outputDevice()  /* [VR AUDIO 2026-11-04] the speakers */;
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
    ALCdevice* device = outputDevice()  /* [VR AUDIO 2026-11-04] the speakers */;
    if (!device || !mReopenDeviceSOFT)
    {
        return;
    }
    if (++mReopens > REOPEN_MAX)
    {
        LL_WARNS() << "OpenAL: " << REOPEN_MAX << " output-device reopens this session (" << why
                   << ") — switching the device watch off; restart the viewer to follow the device again" << LL_ENDL;
        setDeviceWatchEnabled(false);
        return;
    }
    // Source: alext.h:573 LPALCREOPENDEVICESOFT(device, deviceName, attribs): NULL name = the
    // default device, NULL attribs = keep the context's current attributes.
    // <WolfViewer 2026-09-18/> a CHOSEN device (setDevice) is reopened by name; the default only
    // when none is chosen or the chosen one is not plugged in.
    const char* want = selectedDeviceName();
    const ALCboolean ok = mReopenDeviceSOFT(device, want, nullptr);
    if (ok == ALC_TRUE)
    {
        mReopenFailures = 0;
        mSelectedDeviceOpen = want != nullptr;
        mReopenNotBefore = LLFrameTimer::getElapsedSeconds() + REOPEN_GAP_SECS;
        LL_INFOS() << "OpenAL: re-opened the " << (want ? "chosen" : "default") << " output device (" << why << "): "
                   << ll_safe_string(alcGetString(device, ALC_ALL_DEVICES_SPECIFIER)) << LL_ENDL;
    }
    else
    {
        // The default device may itself be mid-change (nothing to open yet). Back off and let
        // the next event or poll try again; never spin on a failing open.
        ++mReopenFailures;
        mReopenNotBefore = LLFrameTimer::getElapsedSeconds() + llmax(REOPEN_GAP_SECS, llmin(30.0, 2.0 * mReopenFailures));
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
    // [VR AUDIO 2026-11-04] OpenAL stops every source on a device that goes away (alext.h:158 ALC_EXT_disconnect);
    // the engine's own sounds restart themselves, the speakers' mirror source is restarted here, after the
    // device watch has reopened the device.
    static F64 next_mirror_check = 0.0;   // once a second at most: a device that stays gone must not spin
    if (mOutSource && mSetThreadContext && LLFrameTimer::getElapsedSeconds() >= next_mirror_check)
    {
        next_mirror_check = LLFrameTimer::getElapsedSeconds() + 1.0;
        mSetThreadContext(mOutContext);
        ALint state = AL_PLAYING;
        alGetSourcei(mOutSource, AL_SOURCE_STATE, &state);
        if (state != AL_PLAYING)
        {
            alSourcePlay(mOutSource);
            LL_INFOS() << "OpenAL: restarted the VR mirror's speaker source (state 0x" << std::hex << state << std::dec << ")" << LL_ENDL;
        }
        mSetThreadContext(nullptr);
    }
}
// </WolfViewer 2026-09-13>

// virtual
std::string LLAudioEngine_OpenAL::getDriverName(bool verbose)
{
    ALCdevice *device = outputDevice()  /* [VR AUDIO 2026-11-04] the speakers */;
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
    shutdownMirrorMix();     // [VR AUDIO 2026-11-04] after the engine's sources are gone, before alutExit

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

