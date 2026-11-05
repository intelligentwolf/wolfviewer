/**
 * @file llwebrtc.h
 * @brief WebRTC interface
 *
 * $LicenseInfo:firstyear=2023&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2023, Linden Research, Inc.
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
 * License along with this library; if not, write to the Free tSoftware
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

/*
 * llwebrtc wraps the native webrtc c++ library in a dynamic library with a simlified interface
 * so that the viewer can use it.  This is done because native webrtc has a different
 * overall threading model than the viewer.
 * The native webrtc library is also compiled with clang, and has memory management
 * functions that conflict namespace-wise with those in the viewer.
 *
 * Due to these differences, code from the viewer cannot be pulled in to this
 * dynamic library, so it remains very simple.
 */

#ifndef LLWEBRTC_H
#define LLWEBRTC_H

#include <cstdint>   // <WolfViewer 2026-10-04/> LLWebRTCPlayoutTap
#include <string>
#include <vector>
// <FS:minerjr> [FIRE-36022] - Removing my USB headset crashes entire viewer
// Needed for inline variable for the crash check
#include <atomic>
// </FS:minerjr> [FIRE-36022]

#ifdef LL_MAKEDLL
#ifdef WEBRTC_WIN
#define LLSYMEXPORT __declspec(dllexport)
#elif WEBRTC_LINUX
#define LLSYMEXPORT __attribute__((visibility("default")))
#else
#define LLSYMEXPORT /**/
#endif
#else
#define LLSYMEXPORT /**/
#endif // LL_MAKEDLL

// <FS:minerjr> [FIRE-36022] - Removing my USB headset crashes entire viewer
// Create an atomic inline flag that will be shared between the various WebRTC threads and co-routines
// to track of when the audio hardware is being talked to. The co-routine can use it to
// exit if it too many iterations with the hardware locked indicating that the worker thread died.
inline std::atomic<bool> gWebRTCUpdateDevices = false;
// </FS:minerjr> [FIRE-36022]
namespace llwebrtc
{

class LLWebRTCLogCallback
{
public:
    typedef enum {
        LOG_LEVEL_VERBOSE = 0,
        LOG_LEVEL_INFO,
        LOG_LEVEL_WARNING,
        LOG_LEVEL_ERROR
    } LogLevel;

    virtual void LogMessage(LogLevel level, const std::string& message) = 0;
};


// LLWebRTCVoiceDevice is a simple representation of the
// components of a device, used to communicate this
// information to the viewer.


// A note on threading.
// Native WebRTC has it's own threading model.  Some discussion
// can be found here (https://webrtc.github.io/webrtc-org/native-code/native-apis/)
//
// Note that all callbacks to observers will occurr on one of the WebRTC native threads
// (signaling, worker, etc.)  Care should be taken to assure there are not
// bad interactions with the viewer threads.

class LLWebRTCVoiceDevice
{
  public:
    std::string mDisplayName;  // friendly name for user interface purposes
    std::string mID;           // internal value for selection

    LLWebRTCVoiceDevice(const std::string &display_name, const std::string &id) :
        mDisplayName(display_name),
        mID(id)
    {
        if (mID.empty())
        {
            mID = display_name;
        }
    };
};

typedef std::vector<LLWebRTCVoiceDevice> LLWebRTCVoiceDeviceList;


// The LLWebRTCDeviceObserver should be implemented by the viewer
// webrtc module, which will receive notifications when devices
// change (are unplugged, etc.)
class LLWebRTCDevicesObserver
{
  public:
    virtual void OnDevicesChanged(const LLWebRTCVoiceDeviceList &render_devices,
                                  const LLWebRTCVoiceDeviceList &capture_devices) = 0;
};


// <WolfViewer 2026-10-04> WOLF DJ talk-show mode (newview wolfdjaudio.cpp): a copy of the mixed
// voice audio the viewer is about to play - everyone you hear in voice - so it can go out on the
// DJ's radio stream. Called on the audio device's playout thread for every buffer the device asks
// for, after the voice engine has filled it; the implementation must only copy the samples and
// return (no locks held long, no blocking).
class LLWebRTCPlayoutTap
{
  public:
    virtual void onPlayoutAudio(const int16_t* samples, size_t frames, size_t channels, uint32_t sample_rate) = 0;
};
// </WolfViewer>

// [VR AUDIO 2026-11-04] THE VR HEADSET MICROPHONE (newview wolfvraudio.cpp). While one is set, the PC
// microphone's buffers are not used: a 10 ms clock in the audio device module asks pullMic for 480 mono 16-bit
// samples at 48 kHz and hands them to the voice engine as if the device had recorded them (same path, so echo
// cancellation, gain and noise suppression still apply; mute still stops it). pullMic returns how many samples it
// wrote (the rest is silence). Called on that clock's own thread; it must not block.
class LLWebRTCExternalMic
{
  public:
    virtual size_t pullMic(int16_t* out, size_t frames, uint32_t sample_rate) = 0;
};

// [AI MODE 2026-11-05] AI MODE (newview wolfaimode.cpp): the avatar's own AI personality speaks over the resident's voice.
// While a speech source is set, every 10 ms capture buffer - the PC microphone's (RecordedDataIsAvailable) or the VR headset
// microphone's (the mic clock) - is offered to it first, on that capture thread, before the voice engine gets it. When
// speak() returns true the buffer the engine gets is what speak() wrote (the AI's voice; what it did not write is silence):
// the microphone is held while the AI talks, so the two never mix and the resident's speakers cannot echo him back out.
// False: the microphone's own sound goes on unchanged. It must not block (a ring read, no locks held long).
class LLWebRTCSpeechSource
{
  public:
    virtual bool speak(int16_t* out, size_t frames, size_t channels, uint32_t sample_rate) = 0;
};

// The LLWebRTCDeviceInterface provides a way for the viewer
// to enumerate, set, and get notifications of changes
// for both capture (microphone) and render (speaker)
// devices.

class LLWebRTCDeviceInterface
{
  public:
    struct AudioConfig {

        bool mAGC { true };

        bool mEchoCancellation { true };

        // TODO: The various levels of noise suppression are configured
        // on the APM which would require setting config on the APM.
        // We should pipe the various values through
        // later.
        typedef enum {
            NOISE_SUPPRESSION_LEVEL_NONE = 0,
            NOISE_SUPPRESSION_LEVEL_LOW,
            NOISE_SUPPRESSION_LEVEL_MODERATE,
            NOISE_SUPPRESSION_LEVEL_HIGH,
            NOISE_SUPPRESSION_LEVEL_VERY_HIGH
        } ENoiseSuppressionLevel;
        ENoiseSuppressionLevel mNoiseSuppressionLevel { NOISE_SUPPRESSION_LEVEL_VERY_HIGH };
    };

    virtual void setAudioConfig(AudioConfig config) = 0;

    // instructs webrtc to refresh the device list.
    virtual void refreshDevices() = 0;

    // set the capture and render devices using the unique identifier for the device
    virtual void setCaptureDevice(const std::string& id) = 0;
    virtual void setRenderDevice(const std::string& id) = 0;

    // Device observers for device change callbacks.
    virtual void setDevicesObserver(LLWebRTCDevicesObserver *observer) = 0;
    virtual void unsetDevicesObserver(LLWebRTCDevicesObserver *observer) = 0;

    // tuning and audio levels
    virtual void setTuningMode(bool enable) = 0;
    virtual float getTuningAudioLevel() = 0; // for use during tuning
    virtual float getPeerConnectionAudioLevel() = 0; // for use when not tuning
    virtual void setMicGain(float gain) = 0;
    virtual void setTuningMicGain(float gain)        = 0;

    virtual void setMute(bool mute, int delay_ms = 0) = 0;

    // <WolfViewer 2026-10-04/> WOLF DJ: nullptr removes it. The tap must outlive the voice engine
    // or be removed first.
    virtual void setPlayoutTap(LLWebRTCPlayoutTap* tap) = 0;

    // [VR AUDIO 2026-11-04] a second playout tap for the VR headset (the DJ tap above is separate); while
    // silenced, voice is still tapped but the PC speakers get silence; a headset microphone (nullptr = the PC's).
    virtual void setVRPlayoutTap(LLWebRTCPlayoutTap* tap) = 0;
    virtual void setPlayoutSilenced(bool silenced) = 0;
    virtual void setExternalMic(LLWebRTCExternalMic* mic) = 0;

    // [AI MODE 2026-11-05] see LLWebRTCSpeechSource (nullptr removes it); a tap of the microphone as captured (before the
    // speech source - the resident's own words, never the AI's), called on the capture thread like the playout taps; a
    // third playout tap (the voices heard, for the AI to listen to). setSpeechGain(true): while the AI talks the voice
    // engine's gain stage passes the buffer at unity whatever the mute state asks of it (the microphone is held by the
    // speech source then, so nothing of the resident's goes out).
    virtual void setSpeechSource(LLWebRTCSpeechSource* source) = 0;
    virtual void setCaptureTap(LLWebRTCPlayoutTap* tap) = 0;
    virtual void setListenTap(LLWebRTCPlayoutTap* tap) = 0;
    virtual void setSpeechGain(bool open) = 0;
};

// LLWebRTCAudioInterface provides the viewer with a way
// to set audio characteristics (mute, send and receive volume)
class LLWebRTCAudioInterface
{
  public:
    virtual void setMute(bool mute) = 0;
    virtual void setReceiveVolume(float volume) = 0;  // volume between 0.0 and 1.0
    virtual void setSendVolume(float volume) = 0;  // volume between 0.0 and 1.0
};

// LLWebRTCDataObserver allows the viewer voice module to be notified when
// data is received over the data channel.
class LLWebRTCDataObserver
{
public:
    virtual void OnDataReceived(const std::string& data, bool binary) = 0;
};

// LLWebRTCDataInterface allows the viewer to send data over the data channel.
class LLWebRTCDataInterface
{
public:

    virtual void sendData(const std::string& data, bool binary=false) = 0;

    virtual void setDataObserver(LLWebRTCDataObserver *observer) = 0;
    virtual void unsetDataObserver(LLWebRTCDataObserver *observer) = 0;
};

// LLWebRTCIceCandidate is a basic structure containing
// information needed for ICE trickling.
struct LLWebRTCIceCandidate
{
    std::string mCandidate;
    std::string mSdpMid;
    int         mMLineIndex;
};

// LLWebRTCSignalingObserver provides a way for the native
// webrtc library to notify the viewer voice module of
// various state changes.
class LLWebRTCSignalingObserver
{
  public:

    typedef enum e_ice_gathering_state {
        ICE_GATHERING_NEW,
        ICE_GATHERING_GATHERING,
        ICE_GATHERING_COMPLETE
    } EIceGatheringState;

    // Called when ICE gathering states have changed.
    // This may be called at any time, as ICE gathering
    // can be redone while a connection is up.
    virtual void OnIceGatheringState(EIceGatheringState state) = 0;

    // Called when a new ice candidate is available.
    virtual void OnIceCandidate(const LLWebRTCIceCandidate& candidate) = 0;

    // Called when an offer is available after a connection is requested.
    virtual void OnOfferAvailable(const std::string& sdp) = 0;

    // Called when a connection enters a failure state and renegotiation is needed.
    virtual void OnRenegotiationNeeded() = 0;

    // Called when a peer connection has shut down
    virtual void OnPeerConnectionClosed() = 0;

    // Called when the audio channel has been established and audio
    // can begin.
    virtual void OnAudioEstablished(LLWebRTCAudioInterface *audio_interface) = 0;

    // Called when the data channel has been established and data
    // transfer can begin.
    virtual void OnDataChannelReady(LLWebRTCDataInterface *data_interface) = 0;
};

// LLWebRTCPeerConnectionInterface representsd a connection to a peer,
// in most cases a Secondlife WebRTC server.  This interface
// allows for management of this peer connection.
class LLWebRTCPeerConnectionInterface
{
  public:

    struct InitOptions
    {
        // equivalent of PeerConnectionInterface::IceServer
        struct IceServers {

            // Valid formats are described in RFC7064 and RFC7065.
            // Urls should containe dns hostnames (not IP addresses)
            // as the TLS certificate policy is 'secure.'
            // and we do not currentply support TLS extensions.
            std::vector<std::string> mUrls;
            std::string mUserName;
            std::string mPassword;
        };

        std::vector<IceServers> mServers;
    };

    virtual bool initializeConnection(const InitOptions& options) = 0;
    virtual bool shutdownConnection() = 0;

    virtual void setSignalingObserver(LLWebRTCSignalingObserver* observer) = 0;
    virtual void unsetSignalingObserver(LLWebRTCSignalingObserver* observer) = 0;

    virtual void AnswerAvailable(const std::string &sdp) = 0;
};

// The following define the dynamic linked library
// exports.

// This library must be initialized before use.
LLSYMEXPORT void init(LLWebRTCLogCallback* logSink);

// And should be terminated as part of shutdown.
LLSYMEXPORT void terminate();

// Return an interface for device management.
LLSYMEXPORT LLWebRTCDeviceInterface* getDeviceInterface();

// Allocate and free peer connections.
LLSYMEXPORT LLWebRTCPeerConnectionInterface* newPeerConnection();
LLSYMEXPORT void freePeerConnection(LLWebRTCPeerConnectionInterface *connection);
}

#endif // LLWEBRTC_H
