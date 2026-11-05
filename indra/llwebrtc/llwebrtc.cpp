/**
 * @file llwebrtc.cpp
 * @brief WebRTC interface implementation
 *
 * $LicenseInfo:firstyear=2023&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2023, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only
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

#include "llwebrtc_impl.h"
#include <algorithm>
#include <string.h>
// <FS:minerjr> [FIRE-36022] - Removing my USB headset crashes entire viewer
// Needed for accessing the inline timed mutex for accessing audio hardware.
#include <mutex>
#include <chrono>
// </FS:minerjr> [FIRE-36022]

#include "api/audio_codecs/audio_decoder_factory.h"
#include "api/audio_codecs/audio_encoder_factory.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/audio/builtin_audio_processing_builder.h"
#include "api/media_stream_interface.h"
#include "api/media_stream_track.h"
#include "modules/audio_processing/audio_buffer.h"
#include "modules/audio_mixer/audio_mixer_impl.h"
#include "api/environment/environment_factory.h"
#include "api/audio/create_audio_device_module.h"   // <WolfViewer 2026-09-26/> m144 CreateAudioDeviceModule

// <FS:minerjr> [FIRE-36022] - Removing my USB headset crashes entire viewer
// Audio device mutex to be shared between audio engine and Voice systems to 
// syncronize on when audio hardware accessed for disconnected/connecting hardware
// Uses Timed Mutex so as to not lockup the threads forever.
inline std::timed_mutex gAudioDeviceMutex;
// Need to use to access the 3 second timeout for the lock.
using namespace std::chrono_literals;
// </FS:minerjr> [FIRE-36022]
#if WEBRTC_WIN
#include <mmdeviceapi.h>   // <WolfViewer 2026-11-03/> ticket 502: IMMNotificationClient (LLWebRTCDefaultDeviceWatcher)
#endif

namespace llwebrtc
{
#if WEBRTC_WIN
// <WolfViewer 2026-11-03> ticket 502 (voice on the TV, not the headset). Windows keeps two "default" devices:
// eConsole (Sound settings' "Default Device") and eCommunications ("Default Communications Device", the role
// Microsoft's ERole documentation gives to voice communications). "Default" used kDefaultDevice, which the ADM
// maps to eConsole (Source: webrtc-sdk m144_release modules/audio_device/win/audio_device_core_win.cc:1452
// SetPlayoutDevice(WindowsDeviceType), :675 InitSpeaker, :1718 SetRecordingDevice, :747 InitMicrophone), so with
// a TV on HDMI as the console default and a headset as the communications default, voice went to the TV.
// Voice now uses the communications default, as secondlife/viewer develop does since 71d2d15245 ("Use
// communications devices/scheme"). Values: api/audio/audio_device.h:55 (kDefaultCommunicationDevice = -1,
// kDefaultDevice = -2). Stock Firestorm still uses kDefaultDevice here.
static int16_t PLAYOUT_DEVICE_DEFAULT = webrtc::AudioDeviceModule::kDefaultCommunicationDevice;
static int16_t RECORD_DEVICE_DEFAULT  = webrtc::AudioDeviceModule::kDefaultCommunicationDevice;
// </WolfViewer>
#else
static int16_t PLAYOUT_DEVICE_DEFAULT = 0;
static int16_t RECORD_DEVICE_DEFAULT  = 0;
#endif

// <WolfViewer 2026-11-03/> ticket 502: the saved value meaning "the system default" (llpanelvoicedevicesettings.cpp
// DEFAULT_DEVICE); an empty value (nothing saved yet) also ends up on the default in workerDeployDevices().
static bool isDefaultDeviceSelection(const std::string& id)
{
    return id.empty() || id == "Default";
}

// <WolfViewer 2026-11-03/> ticket 502: whether the selected device would be found by workerDeployDevices() in
// this list. "Default" is always available.
static bool isDeviceSelectionPresent(const LLWebRTCVoiceDeviceList& list, const std::string& id)
{
    if (isDefaultDeviceSelection(id))
    {
        return true;
    }
    return std::any_of(list.begin(), list.end(), [&id](const LLWebRTCVoiceDevice& device) { return device.mID == id; });
}

#if WEBRTC_WIN
// <WolfViewer 2026-11-03> ticket 502: follow Windows' default communications device while the viewer runs.
// The ADM registers its own IMMNotificationClient, but its OnDefaultDeviceChanged only logs, and only
// OnDeviceStateChanged reaches AudioDeviceObserver::OnDevicesUpdated (Source: webrtc-sdk m144_release
// modules/audio_device/win/audio_device_core_win.cc:3901-3920), so a change of default in Windows was never seen.
// The callbacks come on an MMDevAPI thread and must not block, so this only posts work to the worker thread.
class LLWebRTCDefaultDeviceWatcher final : public IMMNotificationClient
{
public:
    // Worker thread (COM there is MTA: the ADM made on that thread holds ScopedCOMInitializer::kMTA,
    // audio_device_core_win.cc:339; CoInitializeEx below is balanced in stop()). Returns nullptr on failure.
    static LLWebRTCDefaultDeviceWatcher* start(LLWebRTCImpl* owner)
    {
        HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IMMDeviceEnumerator* enumerator = nullptr;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
        if (FAILED(hr) || !enumerator)
        {
            if (SUCCEEDED(com))
            {
                CoUninitialize();
            }
            return nullptr;
        }
        LLWebRTCDefaultDeviceWatcher* watcher = new LLWebRTCDefaultDeviceWatcher(owner, enumerator, SUCCEEDED(com));
        if (FAILED(enumerator->RegisterEndpointNotificationCallback(watcher)))
        {
            watcher->stop();
            return nullptr;
        }
        watcher->mRegistered = true;
        return watcher;
    }

    // Worker thread. After this no callback reaches the owner; releases this object.
    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(mOwnerMutex);
            mOwner = nullptr;
        }
        if (mRegistered)
        {
            mEnumerator->UnregisterEndpointNotificationCallback(this);
            mRegistered = false;
        }
        mEnumerator->Release();
        mEnumerator = nullptr;
        if (mComInitialised)
        {
            CoUninitialize();
        }
        Release();
    }

    // IMMNotificationClient
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override
    {
        // Only the role "Default" uses (PLAYOUT_DEVICE_DEFAULT / RECORD_DEVICE_DEFAULT above).
        if (role == eCommunications && (flow == eRender || flow == eCapture))
        {
            std::lock_guard<std::mutex> lock(mOwnerMutex);
            if (mOwner)
            {
                mOwner->onSystemDefaultDeviceChanged(flow == eRender);
            }
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

    // IUnknown
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return static_cast<ULONG>(InterlockedIncrement(&mRefCount));
    }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const LONG count = InterlockedDecrement(&mRefCount);
        if (count == 0)
        {
            delete this;
        }
        return static_cast<ULONG>(count);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
    {
        if (!object)
        {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient))
        {
            *object = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

private:
    LLWebRTCDefaultDeviceWatcher(LLWebRTCImpl* owner, IMMDeviceEnumerator* enumerator, bool com_initialised) :
        mOwner(owner),
        mEnumerator(enumerator),
        mComInitialised(com_initialised)
    {
    }
    ~LLWebRTCDefaultDeviceWatcher() = default;

    volatile LONG        mRefCount = 1;
    std::mutex           mOwnerMutex;
    LLWebRTCImpl*        mOwner;
    IMMDeviceEnumerator* mEnumerator;
    bool                 mComInitialised;
    bool                 mRegistered = false;
};
// </WolfViewer>
#endif


//
// LLWebRTCAudioTransport implementation
//

LLWebRTCAudioTransport::LLWebRTCAudioTransport() : mMicrophoneEnergy(0.0)
{
    memset(mSumVector, 0, sizeof(mSumVector));
}

void LLWebRTCAudioTransport::SetEngineTransport(webrtc::AudioTransport* t)
{
    engine_.store(t, std::memory_order_release);
}

int32_t LLWebRTCAudioTransport::RecordedDataIsAvailable(const void* audio_data,
                                                        size_t      number_of_frames,
                                                        size_t      bytes_per_frame,
                                                        size_t      number_of_channels,
                                                        uint32_t    samples_per_sec,
                                                        uint32_t    total_delay_ms,
                                                        int32_t     clock_drift,
                                                        uint32_t    current_mic_level,
                                                        bool        key_pressed,
                                                        uint32_t&   new_mic_level)
{
    // [VR AUDIO 2026-11-04] a headset microphone is in use: the PC microphone's buffers are not sent (the ADM's
    // mic clock delivers the headset's through DeliverCapture instead)
    if (mExternalMicActive.load(std::memory_order_acquire))
    {
        new_mic_level = current_mic_level;
        return 0;
    }
    return DeliverCapture(audio_data, number_of_frames, bytes_per_frame, number_of_channels, samples_per_sec,
                          total_delay_ms, clock_drift, current_mic_level, key_pressed, new_mic_level);
}

int32_t LLWebRTCAudioTransport::DeliverCapture(const void* audio_data,
                                               size_t      number_of_frames,
                                               size_t      bytes_per_frame,
                                               size_t      number_of_channels,
                                               uint32_t    samples_per_sec,
                                               uint32_t    total_delay_ms,
                                               int32_t     clock_drift,
                                               uint32_t    current_mic_level,
                                               bool        key_pressed,
                                               uint32_t&   new_mic_level)
{
    auto* engine = engine_.load(std::memory_order_acquire);

    // [AI MODE 2026-11-05] the resident's own sound to the AI's ears first, then (while the AI talks) the AI's voice in place
    // of it - see LLWebRTCSpeechSource (llwebrtc.h). 16-bit interleaved: the only layout the ADM's AudioDeviceBuffer and the
    // mic clock deliver (bytes_per_frame is 2 x channels, as the playout taps check below).
    if (number_of_channels > 0 && bytes_per_frame == 2 * number_of_channels)
    {
        LLWebRTCSpeechSource* speech = mSpeechSource.load(std::memory_order_acquire);
        const size_t n = number_of_frames * number_of_channels;
        if (speech)
        {
            if (mSpeechBuf.size() < n) mSpeechBuf.resize(n);
            memset(mSpeechBuf.data(), 0, n * sizeof(int16_t));
            if (speech->speak(mSpeechBuf.data(), number_of_frames, number_of_channels, samples_per_sec))
            {
                audio_data = mSpeechBuf.data();   // the microphone is held: only the AI's voice goes on
            }
            else if (LLWebRTCPlayoutTap* tap = mCaptureTap.load(std::memory_order_acquire))
            {
                tap->onPlayoutAudio(static_cast<const int16_t*>(audio_data), number_of_frames, number_of_channels, samples_per_sec);
            }
        }
        else if (LLWebRTCPlayoutTap* tap = mCaptureTap.load(std::memory_order_acquire))
        {
            tap->onPlayoutAudio(static_cast<const int16_t*>(audio_data), number_of_frames, number_of_channels, samples_per_sec);
        }
    }

    // 1) Deliver to engine (authoritative).
    int32_t ret = 0;
    if (engine)
    {
        ret = engine->RecordedDataIsAvailable(audio_data,
                                              number_of_frames,
                                              bytes_per_frame,
                                              number_of_channels,
                                              samples_per_sec,
                                              total_delay_ms,
                                              clock_drift,
                                              current_mic_level,
                                              key_pressed,
                                              new_mic_level);
    }

    // 2) Calculate energy for microphone level monitoring
    // calculate the energy
    float        energy  = 0;
    const short *samples = (const short *) audio_data;

    for (size_t index = 0; index < number_of_frames * number_of_channels; index++)
    {
        float sample = (static_cast<float>(samples[index]) / (float) 32767);
        energy += sample * sample;
    }
    float gain = mGain.load(std::memory_order_relaxed);
    energy     = energy * gain * gain;
    // smooth it.
    size_t buffer_size = sizeof(mSumVector) / sizeof(mSumVector[0]);
    float  totalSum    = 0;
    int    i;
    for (i = 0; i < (buffer_size - 1); i++)
    {
        mSumVector[i] = mSumVector[i + 1];
        totalSum += mSumVector[i];
    }
    mSumVector[i] = energy;
    totalSum += energy;
    mMicrophoneEnergy = std::sqrt(totalSum / (number_of_frames * number_of_channels * buffer_size));

    return ret;
}

int32_t LLWebRTCAudioTransport::NeedMorePlayData(size_t   number_of_frames,
                                                 size_t   bytes_per_frame,
                                                 size_t   number_of_channels,
                                                 uint32_t samples_per_sec,
                                                 void*    audio_data,
                                                 size_t&  number_of_samples_out,
                                                 int64_t* elapsed_time_ms,
                                                 int64_t* ntp_time_ms)
{
    auto* engine = engine_.load(std::memory_order_acquire);
    if (!engine)
    {
        // No engine sink; output silence to be safe.
        const size_t bytes = number_of_frames * bytes_per_frame * number_of_channels;
        memset(audio_data, 0, bytes);
        number_of_samples_out = bytes_per_frame;
        return 0;
    }

    // Only the engine should fill the buffer.
    int32_t ret = engine->NeedMorePlayData(number_of_frames,
                                           bytes_per_frame,
                                           number_of_channels,
                                           samples_per_sec,
                                           audio_data,
                                           number_of_samples_out,
                                           elapsed_time_ms,
                                           ntp_time_ms);

    // <WolfViewer 2026-10-04> WOLF DJ talk-show mode: hand a copy of what will be played (the
    // mixed far-end voices) to the tap. 16-bit PCM: bytes_per_frame is 2 x channels here, the
    // only layout the ADM's AudioDeviceBuffer uses (kBytesPerSample checks below are defensive).
    if (LLWebRTCPlayoutTap* tap = mPlayoutTap.load(std::memory_order_acquire))
    {
        if (ret == 0 && number_of_channels > 0 && bytes_per_frame == 2 * number_of_channels)
        {
            tap->onPlayoutAudio(static_cast<const int16_t*>(audio_data), number_of_frames, number_of_channels, samples_per_sec);
        }
    }
    // </WolfViewer>
    // [VR AUDIO 2026-11-04] the VR headset's copy of the voices, the same check as the DJ tap above; then, while
    // the headset plays the sound, silence for the PC speakers. 16-bit samples, so the buffer is frames x channels
    // x 2 bytes whichever way bytes_per_frame is counted (audio_device_defines.h:70 calls it nBytesPerSample).
    if (ret == 0 && number_of_channels > 0 && bytes_per_frame == 2 * number_of_channels)
    {
        if (LLWebRTCPlayoutTap* tap = mVRTap.load(std::memory_order_acquire))
        {
            tap->onPlayoutAudio(static_cast<const int16_t*>(audio_data), number_of_frames, number_of_channels, samples_per_sec);
        }
        // [AI MODE 2026-11-05] the voices heard, for the AI to listen to (before any silencing for the headset)
        if (LLWebRTCPlayoutTap* tap = mListenTap.load(std::memory_order_acquire))
        {
            tap->onPlayoutAudio(static_cast<const int16_t*>(audio_data), number_of_frames, number_of_channels, samples_per_sec);
        }
        if (mSilenced.load(std::memory_order_relaxed))
        {
            memset(audio_data, 0, number_of_frames * number_of_channels * sizeof(int16_t));
        }
    }
    return ret;
}

void LLWebRTCAudioTransport::PullRenderData(int      bits_per_sample,
                                            int      sample_rate,
                                            size_t   number_of_channels,
                                            size_t   number_of_frames,
                                            void*    audio_data,
                                            int64_t* elapsed_time_ms,
                                            int64_t* ntp_time_ms)
{
    auto* engine = engine_.load(std::memory_order_acquire);

    if (engine)
    {
        engine
            ->PullRenderData(bits_per_sample, sample_rate, number_of_channels, number_of_frames, audio_data, elapsed_time_ms, ntp_time_ms);
    }
}

LLCustomProcessor::LLCustomProcessor(LLCustomProcessorStatePtr state) : mSampleRateHz(0), mNumChannels(0), mState(state)
{
    memset(mSumVector, 0, sizeof(mSumVector));
}

void LLCustomProcessor::Initialize(int sample_rate_hz, int num_channels)
{
    mSampleRateHz = sample_rate_hz;
    mNumChannels  = num_channels;
    memset(mSumVector, 0, sizeof(mSumVector));
}

void LLCustomProcessor::Process(webrtc::AudioBuffer *audio)
{
    if (audio->num_channels() < 1 || audio->num_frames() < 480)
    {
        return;
    }

    // calculate the energy

    float desired_gain = mState->getEffectiveGain();   // [AI MODE 2026-11-05] unity while the AI talks
    if (mState->getDirty())
    {
        // We'll delay ramping by 30ms in order to clear out buffers that may
        // have had content before muting.  And for the last 20ms, we'll ramp
        // down or up smoothly.
        mRampFrames = 5;

        // we've changed our desired gain, so set the incremental
        // gain change so that we smoothly step over 20ms
        mGainStep = (desired_gain - mCurrentGain) / (mSampleRateHz / 50);
    }

    if (mRampFrames)
    {
        if (mRampFrames-- > 2)
        {
            // don't change the gain if we're still in the 'don't move' phase
            mGainStep = 0.0f;
        }
    }
    else
    {
        // We've ramped all the way down, so don't step the gain any more and
        // just maintaint he current gain.
        mGainStep = 0.0f;
        mCurrentGain = desired_gain;
    }

    float energy       = 0;

    auto chans = audio->channels();
    for (size_t ch = 0; ch < audio->num_channels(); ch++)
    {
        float* frame_samples = chans[ch];
        float  gain          = mCurrentGain;
        for (size_t index = 0; index < audio->num_frames(); index++)
        {
            float sample         = frame_samples[index];
            sample               = sample * gain;    // apply gain
            frame_samples[index] = sample;        // write processed sample back to buffer.
            energy += sample * sample;
            gain += mGainStep;
        }
    }
    mCurrentGain += audio->num_frames() * mGainStep;

    // smooth it.
    size_t buffer_size = sizeof(mSumVector) / sizeof(mSumVector[0]);
    float  totalSum    = 0;
    int    i;
    for (i = 0; i < (buffer_size - 1); i++)
    {
        mSumVector[i] = mSumVector[i + 1];
        totalSum += mSumVector[i];
    }
    mSumVector[i] = energy;
    totalSum += energy;
    mState->setMicrophoneEnergy(std::sqrt(totalSum / (audio->num_channels() * audio->num_frames() * buffer_size)));
}

//
// LLWebRTCImpl implementation
//

LLWebRTCImpl::LLWebRTCImpl(LLWebRTCLogCallback* logCallback) :
    mEnv(webrtc::CreateEnvironment(webrtc::CreateDefaultTaskQueueFactory())),   // <WolfViewer 2026-09-26/> m144, as secondlife/viewer develop
    mLogSink(new LLWebRTCLogSink(logCallback)),
    mPeerCustomProcessor(nullptr),
    mMute(true),
    mTuningMode(false),
    mDevicesDeploying(0),
    mGain(0.0f)
{
}

void LLWebRTCImpl::init()
{
    webrtc::InitializeSSL();

    // Normal logging is rather spammy, so turn it off.
    // <FS:minerjr> [FIRE-36022] - Removing my USB headset crashes entire viewer
    // Turn on more verbose logging as we are looking for crashes.
    webrtc::LogMessage::LogToDebug(webrtc::LS_NONE);
    //webrtc::LogMessage::LogToDebug(webrtc::LS_VERBOSE);
    // </FS:minerjr> [FIRE-36022]
    webrtc::LogMessage::SetLogToStderr(true);
    webrtc::LogMessage::AddLogToStream(mLogSink, webrtc::LS_VERBOSE);

    // Create the native threads.
    mNetworkThread = webrtc::Thread::CreateWithSocketServer();
    mNetworkThread->SetName("WebRTCNetworkThread", nullptr);
    mNetworkThread->Start();
    mWorkerThread = webrtc::Thread::Create();
    mWorkerThread->SetName("WebRTCWorkerThread", nullptr);
    mWorkerThread->Start();
    mSignalingThread = webrtc::Thread::Create();
    mSignalingThread->SetName("WebRTCSignalingThread", nullptr);
    mSignalingThread->Start();

    mWorkerThread->BlockingCall(
        [this]()
        {
            // <WolfViewer 2026-09-26> m144 API. Source: secondlife/viewer develop llwebrtc.cpp:326.
            webrtc::scoped_refptr<webrtc::AudioDeviceModule> realADM =
                webrtc::CreateAudioDeviceModule(mEnv, webrtc::AudioDeviceModule::AudioLayer::kPlatformDefaultAudio);
            mDeviceModule = webrtc::make_ref_counted<LLWebRTCAudioDeviceModule>(realADM);
            mDeviceModule->SetObserver(this);
#if WEBRTC_WIN
            mDefaultDeviceWatcher = LLWebRTCDefaultDeviceWatcher::start(this);   // <WolfViewer 2026-11-03/> ticket 502
#endif
        });

    // The custom processor allows us to retrieve audio data (and levels)
    // from after other audio processing such as AEC, AGC, etc.
    mPeerCustomProcessor = std::make_shared<LLCustomProcessorState>();
    webrtc::BuiltinAudioProcessingBuilder apb;
    apb.SetCapturePostProcessing(std::make_unique<LLCustomProcessor>(mPeerCustomProcessor));
    mAudioProcessingModule = apb.Build(webrtc::CreateEnvironment());

    webrtc::AudioProcessing::Config apm_config;
    apm_config.echo_canceller.enabled         = false;
    apm_config.echo_canceller.mobile_mode     = false;
    apm_config.gain_controller1.enabled       = false;
    apm_config.gain_controller2.enabled       = true;
    apm_config.high_pass_filter.enabled       = true;
    apm_config.noise_suppression.enabled      = true;
    apm_config.noise_suppression.level        = webrtc::AudioProcessing::Config::NoiseSuppression::kVeryHigh;
    apm_config.transient_suppression.enabled  = true;
    apm_config.pipeline.multi_channel_render  = true;
    apm_config.pipeline.multi_channel_capture = false;

    mAudioProcessingModule->ApplyConfig(apm_config);

    webrtc::ProcessingConfig processing_config;

    processing_config.input_stream().set_num_channels(2);
    processing_config.input_stream().set_sample_rate_hz(48000);
    processing_config.output_stream().set_num_channels(2);
    processing_config.output_stream().set_sample_rate_hz(48000);
    processing_config.reverse_input_stream().set_num_channels(2);
    processing_config.reverse_input_stream().set_sample_rate_hz(48000);
    processing_config.reverse_output_stream().set_num_channels(2);
    processing_config.reverse_output_stream().set_sample_rate_hz(48000);

    mAudioProcessingModule->Initialize(processing_config);

    mPeerConnectionFactory = webrtc::CreatePeerConnectionFactory(mNetworkThread.get(),
                                                                 mWorkerThread.get(),
                                                                 mSignalingThread.get(),
                                                                 mDeviceModule,
                                                                 webrtc::CreateBuiltinAudioEncoderFactory(),
                                                                 webrtc::CreateBuiltinAudioDecoderFactory(),
                                                                 nullptr /* video_encoder_factory */,
                                                                 nullptr /* video_decoder_factory */,
                                                                 nullptr /* audio_mixer */,
                                                                 mAudioProcessingModule);
    mWorkerThread->PostTask(
        [this]()
        {
            if (mDeviceModule)
            {
                mDeviceModule->EnableBuiltInAEC(false);
                // <FS:minerjr> [FIRE-36022] - Removing my USB headset crashes entire viewer
                // It seems that this code is also called by one of the other callback functions
                // and ends up creating a 2nd observer,which when there was a hardware change,
                // would cause multiple messages to be dumped to the queue at the same time and
                // cause a possible race condition/spin lock issue.
                //updateDevices();
            }
        });

}

void LLWebRTCImpl::terminate()
{
    mWorkerThread->BlockingCall(
        [this]()
        {
            if (mDeviceModule)
            {
                mDeviceModule->ForceStopRecording();
                mDeviceModule->StopPlayout();
            }
        });

    for (auto &connection : mPeerConnections)
    {
        connection->terminate();
    }

    // connection->terminate() above spawns a number of Signaling thread calls to
    // shut down the connection.  The following Blocking Call will wait
    // until they're done before it's executed, allowing time to clean up.

    mSignalingThread->BlockingCall([this]() { mPeerConnectionFactory = nullptr; });

    mWorkerThread->BlockingCall(
        [this]()
        {
#if WEBRTC_WIN
            // <WolfViewer 2026-11-03/> ticket 502: no default-device callbacks once the threads are going.
            if (mDefaultDeviceWatcher)
            {
                mDefaultDeviceWatcher->stop();
                mDefaultDeviceWatcher = nullptr;
            }
#endif
            if (mDeviceModule)
            {
                // <WolfViewer 2026-11-03/> ticket 502: OnDevicesUpdated() posts to mWorkerThread, which goes below.
                mDeviceModule->SetObserver(nullptr);
                mDeviceModule->Terminate();
            }
            mDeviceModule     = nullptr;
        });

    // In case peer connections still somehow have jobs in workers,
    // only clear connections up after clearing workers.
    mNetworkThread = nullptr;
    mWorkerThread = nullptr;
    mSignalingThread = nullptr;

    mPeerConnections.clear();
    webrtc::LogMessage::RemoveLogToStream(mLogSink);
}

void LLWebRTCImpl::setAudioConfig(LLWebRTCDeviceInterface::AudioConfig config)
{
    webrtc::AudioProcessing::Config apm_config;
    apm_config.echo_canceller.enabled         = config.mEchoCancellation;
    apm_config.echo_canceller.mobile_mode     = false;
    apm_config.gain_controller1.enabled       = false;
    apm_config.gain_controller2.enabled       = config.mAGC;
    apm_config.gain_controller2.adaptive_digital.enabled = true; // auto-level speech
    apm_config.high_pass_filter.enabled       = true;
    apm_config.transient_suppression.enabled  = true;
    apm_config.pipeline.multi_channel_render  = true;
    apm_config.pipeline.multi_channel_capture = true;
    apm_config.pipeline.multi_channel_capture = true;

    switch (config.mNoiseSuppressionLevel)
    {
        case LLWebRTCDeviceInterface::AudioConfig::NOISE_SUPPRESSION_LEVEL_NONE:
            apm_config.noise_suppression.enabled = false;
            apm_config.noise_suppression.level = webrtc::AudioProcessing::Config::NoiseSuppression::kLow;
            break;
        case LLWebRTCDeviceInterface::AudioConfig::NOISE_SUPPRESSION_LEVEL_LOW:
            apm_config.noise_suppression.enabled = true;
            apm_config.noise_suppression.level = webrtc::AudioProcessing::Config::NoiseSuppression::kLow;
            break;
        case LLWebRTCDeviceInterface::AudioConfig::NOISE_SUPPRESSION_LEVEL_MODERATE:
            apm_config.noise_suppression.enabled = true;
            apm_config.noise_suppression.level = webrtc::AudioProcessing::Config::NoiseSuppression::kModerate;
            break;
        case LLWebRTCDeviceInterface::AudioConfig::NOISE_SUPPRESSION_LEVEL_HIGH:
            apm_config.noise_suppression.enabled = true;
            apm_config.noise_suppression.level = webrtc::AudioProcessing::Config::NoiseSuppression::kHigh;
            break;
        case LLWebRTCDeviceInterface::AudioConfig::NOISE_SUPPRESSION_LEVEL_VERY_HIGH:
            apm_config.noise_suppression.enabled = true;
            apm_config.noise_suppression.level = webrtc::AudioProcessing::Config::NoiseSuppression::kVeryHigh;
            break;
        default:
            apm_config.noise_suppression.enabled = false;
            apm_config.noise_suppression.level = webrtc::AudioProcessing::Config::NoiseSuppression::kLow;
    }
    mAudioProcessingModule->ApplyConfig(apm_config);
}

void LLWebRTCImpl::refreshDevices()
{
    mWorkerThread->PostTask([this]() { updateDevices(); });
}

void LLWebRTCImpl::setDevicesObserver(LLWebRTCDevicesObserver *observer) { mVoiceDevicesObserverList.emplace_back(observer); }

void LLWebRTCImpl::unsetDevicesObserver(LLWebRTCDevicesObserver *observer)
{
    std::vector<LLWebRTCDevicesObserver *>::iterator it =
        std::find(mVoiceDevicesObserverList.begin(), mVoiceDevicesObserverList.end(), observer);
    if (it != mVoiceDevicesObserverList.end())
    {
        mVoiceDevicesObserverList.erase(it);
    }
}

// <WolfViewer 2026-11-03> ticket 502: ends a workerDeployDevices() turn that did not reach the signalling-thread
// task at its end (which does the same fetch_sub), so a failed pass no longer blocks every later deployDevices().
// Worker thread.
void LLWebRTCImpl::workerFinishDeployTurn()
{
    if (1 < mDevicesDeploying.fetch_sub(1, std::memory_order_relaxed))
    {
        mWorkerThread->PostTask([this] { workerDeployDevices(); });
    }
}
// </WolfViewer>

// must be run in the worker thread.
void LLWebRTCImpl::workerDeployDevices()
{
    bool turnHandedOn = false;   // <WolfViewer 2026-11-03/> ticket 502: see workerFinishDeployTurn()
    // <FS:minerjr> [FIRE-36022] - Removing my USB headset crashes entire viewer
    try // Try catch needed for uniquie lock as will throw an exception if a second lock is attempted or the mutex is invalid
    {
    // Attempt to lock the access to the audio device, wait up to 1 second for other threads to unlock.
    std::unique_lock lock(gAudioDeviceMutex, 1s);
    // If the lock could not be accessed, return as we don't have hardware access and will need to try again another pass.
    // Prevents threads from interacting with the hardware at the same time as other audio/voice threads.
    if (!lock.owns_lock())
    {
        // <WolfViewer 2026-11-03> ticket 502: "try again another pass" never happened - this turn kept
        // mDevicesDeploying above zero, so deployDevices() dropped every later device choice. Retry, keeping the turn.
        mWorkerThread->PostDelayedTask([this] { workerDeployDevices(); }, webrtc::TimeDelta::Millis(250));
        // </WolfViewer>
        return;
    }
    // </FS:minerjr> [FIRE-36022]
    if (!mDeviceModule)
    {
        // <FS:minerjr> [FIRE-36022]
        // If the device is not avaiable, then make sure the flag for the WebRTC updated devices flag is turned off for the co-routine
        gWebRTCUpdateDevices = false;
        // </FS:minerjr> [FIRE-36022] - Removing my USB headset crashes entire viewer
        workerFinishDeployTurn();   // <WolfViewer 2026-11-03/> ticket 502: give the turn back
        return;
    }

    int16_t recordingDevice = RECORD_DEVICE_DEFAULT;
    int16_t recording_device_start = 0;

    if (mRecordingDevice != "Default")
    {
        for (int16_t i = recording_device_start; i < mRecordingDeviceList.size(); i++)
        {
            if (mRecordingDeviceList[i].mID == mRecordingDevice)
            {
                recordingDevice = i;
#if !WEBRTC_WIN
                // linux and mac devices range from 1 to the end of the list, with the index 0 being the
                // 'default' device.  Windows has a special 'default' device and other devices are indexed
                // from 0
                recordingDevice++;
#endif
                break;
            }
        }
    }

    // <FS:minerjr> [FIRE-36022] - Removing my USB headset crashes entire viewer
    // Flag the device is being interacted with for the Co-routine in case something goes wrong.
    gWebRTCUpdateDevices = true;
    // </FS:minerjr> [FIRE-36022]
    mDeviceModule->StopPlayout();
    mDeviceModule->ForceStopRecording();
#if WEBRTC_WIN
    if (recordingDevice < 0)
    {
        mDeviceModule->SetRecordingDevice((webrtc::AudioDeviceModule::WindowsDeviceType)recordingDevice);
    }
    else
    {
        mDeviceModule->SetRecordingDevice(recordingDevice);
    }
#else
    mDeviceModule->SetRecordingDevice(recordingDevice);
#endif
    mDeviceModule->InitMicrophone();
    mDeviceModule->SetStereoRecording(false);
    mDeviceModule->InitRecording();

    int16_t playoutDevice = PLAYOUT_DEVICE_DEFAULT;
    int16_t playout_device_start = 0;
    if (mPlayoutDevice != "Default")
    {
        for (int16_t i = playout_device_start; i < mPlayoutDeviceList.size(); i++)
        {
            if (mPlayoutDeviceList[i].mID == mPlayoutDevice)
            {
                playoutDevice = i;
#if !WEBRTC_WIN
                // linux and mac devices range from 1 to the end of the list, with the index 0 being the
                // 'default' device.  Windows has a special 'default' device and other devices are indexed
                // from 0
                playoutDevice++;
#endif
                break;
            }
        }
    }

#if WEBRTC_WIN
    if (playoutDevice < 0)
    {
        mDeviceModule->SetPlayoutDevice((webrtc::AudioDeviceModule::WindowsDeviceType)playoutDevice);
    }
    else
    {
        mDeviceModule->SetPlayoutDevice(playoutDevice);
    }
#else
    mDeviceModule->SetPlayoutDevice(playoutDevice);
#endif
    mDeviceModule->InitSpeaker();
    mDeviceModule->SetStereoPlayout(true);
    mDeviceModule->InitPlayout();

    if ((!mMute && mPeerConnections.size()) || mTuningMode)
    {
        mDeviceModule->ForceStartRecording();
    }

    if (!mTuningMode)
    {
        mDeviceModule->StartPlayout();
    }
    // <FS:minerjr> [FIRE-36022] - Removing my USB headset crashes entire viewer
    // Finally signal to the co-routine everyting is OK.
    gWebRTCUpdateDevices = false;
    // </FS:minerjr> [FIRE-36022]
    turnHandedOn = true;   // <WolfViewer 2026-11-03/> ticket 502: the task below ends this turn
    mSignalingThread->PostTask(
        [this]
        {
            for (auto& connection : mPeerConnections)
            {
                if (mTuningMode)
                {
                    connection->enableSenderTracks(false);
                }
                else
                {
                    connection->resetMute();
                }
                connection->enableReceiverTracks(!mTuningMode);
            }
            if (1 < mDevicesDeploying.fetch_sub(1, std::memory_order_relaxed))
            {
                mWorkerThread->PostTask([this] { workerDeployDevices(); });
            }
        });
    // <FS:minerjr> [FIRE-36022] - Removing my USB headset crashes entire viewer
    }
    // There are two exceptions that unique_lock can trigger, operation_not_permitted or resource_deadlock_would_occur
    catch (const std::system_error& e)
    {
        if (e.code() == std::errc::resource_deadlock_would_occur)
        {
            // Another thead may have alreayd called this method
            mLogSink->OnLogMessage(std::string("Excepton: WebRTC: ") + e.what());
        }
        else if (e.code() == std::errc::operation_not_permitted)
        {
            // This should not be reached
            mLogSink->OnLogMessage(std::string("Excepton: WebRTC: ") + e.what());
        }
        else
        {
            // Log any other message
            mLogSink->OnLogMessage(std::string("Excepton: WebRTC: ") + e.what());
        }
        // Device no longer being interacted with
        gWebRTCUpdateDevices = false;
        if (!turnHandedOn) workerFinishDeployTurn();   // <WolfViewer 2026-11-03/> ticket 502
        return;
    }
    catch (const std::exception& e)
    {
        mLogSink->OnLogMessage(std::string("Excepton: WebRTC: ") + e.what());
        // Device no longer being interacted with
        gWebRTCUpdateDevices = false;
        if (!turnHandedOn) workerFinishDeployTurn();   // <WolfViewer 2026-11-03/> ticket 502
        return;
    }
    // </FS:minerjr> [FIRE-36022]
}

void LLWebRTCImpl::setCaptureDevice(const std::string &id)
{

    if (mRecordingDevice != id)
    {
        mRecordingDevice = id;
        deployDevices();
    }
}

void LLWebRTCImpl::setRenderDevice(const std::string &id)
{
    if (mPlayoutDevice != id)
    {
        mPlayoutDevice = id;
        deployDevices();
    }
}

// updateDevices needs to happen on the worker thread.
void LLWebRTCImpl::updateDevices()
{
    // <FS:minerjr> [FIRE-36022] - Removing my USB headset crashes entire viewer
    try // Try catch needed for uniquie lock as will throw an exception if a second lock is attempted or the mutex is invalid
    {
    // Attempt to lock the access to the audio device, wait up to 1 second for other threads to unlock.
    std::unique_lock lock(gAudioDeviceMutex, 1s);
    // If the lock could not be accessed, return as we don't have hardware access and will need to try again another pass.
    // Prevents threads from interacting with the hardware at the same time as other audio/voice threads.
    if (!lock.owns_lock())
    {
        // <WolfViewer 2026-11-03/> ticket 502: a device change must not be lost - enumerate again shortly.
        mWorkerThread->PostDelayedTask([this] { updateDevices(); }, webrtc::TimeDelta::Millis(250));
        return;
    }
    // </FS:minerjr> [FIRE-36022]
    if (!mDeviceModule)
    {
        return;
    }

    // <WolfViewer 2026-11-03> ticket 502: whether the chosen devices were in the old lists, so a chosen device that
    // was missing when it was applied (headset switched on after log-in, or applied before the first enumeration)
    // is applied once it appears, and one that goes away falls back to the default. Before this the device was
    // never re-applied: setRenderDevice()/setCaptureDevice() skip an unchanged id, and nothing else redeployed.
    const bool playoutWasPresent   = isDeviceSelectionPresent(mPlayoutDeviceList, mPlayoutDevice);
    const bool recordingWasPresent = isDeviceSelectionPresent(mRecordingDeviceList, mRecordingDevice);
    // </WolfViewer>

    // <FS:minerjr> [FIRE-36022] - Removing my USB headset crashes entire viewer
    // Flag the device is being interacted with for the Co-routine in case something goes wrong.
    gWebRTCUpdateDevices = true;
    // </FS:minerjr> [FIRE-36022]
    int16_t renderDeviceCount  = mDeviceModule->PlayoutDevices();

    mPlayoutDeviceList.clear();
#if WEBRTC_WIN
    int16_t index = 0;
#else
    // index zero is always "Default" for darwin/linux,
    // which is a special case, so skip it.
    int16_t index = 1;
#endif
    for (; index < renderDeviceCount; index++)
    {
        char name[webrtc::kAdmMaxDeviceNameSize];
        char guid[webrtc::kAdmMaxGuidSize];
        mDeviceModule->PlayoutDeviceName(index, name, guid);
        mPlayoutDeviceList.emplace_back(name, guid);
    }

    int16_t captureDeviceCount        = mDeviceModule->RecordingDevices();

    mRecordingDeviceList.clear();
#if WEBRTC_WIN
    index = 0;
#else
    // index zero is always "Default" for darwin/linux,
    // which is a special case, so skip it.
    index = 1;
#endif
    for (; index < captureDeviceCount; index++)
    {
        char name[webrtc::kAdmMaxDeviceNameSize];
        char guid[webrtc::kAdmMaxGuidSize];
        mDeviceModule->RecordingDeviceName(index, name, guid);
        mRecordingDeviceList.emplace_back(name, guid);
    }

    // <WolfViewer 2026-11-03> ticket 502: on Linux "Default" opens PulseAudio's default at InitPlayout /
    // InitRecording time (a null device name: audio_device_pulse_linux.cc:2105-2147), and the ADM reports no device
    // changes, so a new server default is picked up here, when the list is refreshed. macOS needs nothing: its ADM
    // restarts a running index-0 stream itself (audio_device_mac.cc HandleDefaultOutputDeviceChange /
    // HandleDefaultInputDeviceChange); Windows: LLWebRTCDefaultDeviceWatcher.
    bool defaultChanged = false;
#if WEBRTC_LINUX
    {
        char name[webrtc::kAdmMaxDeviceNameSize];
        char guid[webrtc::kAdmMaxGuidSize];
        std::string playoutDefault;
        std::string recordingDefault;
        if (renderDeviceCount > 0 && mDeviceModule->PlayoutDeviceName(0, name, guid) == 0)
        {
            playoutDefault = name;
        }
        if (captureDeviceCount > 0 && mDeviceModule->RecordingDeviceName(0, name, guid) == 0)
        {
            recordingDefault = name;
        }
        defaultChanged = (isDefaultDeviceSelection(mPlayoutDevice) && !mDefaultPlayoutName.empty()
                          && !playoutDefault.empty() && playoutDefault != mDefaultPlayoutName)
                      || (isDefaultDeviceSelection(mRecordingDevice) && !mDefaultRecordingName.empty()
                          && !recordingDefault.empty() && recordingDefault != mDefaultRecordingName);
        if (!playoutDefault.empty())
        {
            mDefaultPlayoutName = playoutDefault;
        }
        if (!recordingDefault.empty())
        {
            mDefaultRecordingName = recordingDefault;
        }
    }
#endif
    const bool redeploy = defaultChanged
                       || playoutWasPresent != isDeviceSelectionPresent(mPlayoutDeviceList, mPlayoutDevice)
                       || recordingWasPresent != isDeviceSelectionPresent(mRecordingDeviceList, mRecordingDevice);
    // </WolfViewer>

    // <FS:minerjr> [FIRE-36022] - Removing my USB headset crashes entire viewer
    // Flag the device is no longer being interacted with for the Co-routine in case something goes wrong.
    gWebRTCUpdateDevices = false;
    // </FS:minerjr> [FIRE-36022]
    for (auto &observer : mVoiceDevicesObserverList)
    {
        observer->OnDevicesChanged(mPlayoutDeviceList, mRecordingDeviceList);
    }
    if (redeploy)
    {
        deployDevices();   // <WolfViewer 2026-11-03/> ticket 502 (posts workerDeployDevices, which takes the lock itself)
    }
    // <FS:minerjr> [FIRE-36022] - Removing my USB headset crashes entire viewer
    }
    // There are two exceptions that unique_lock can trigger, operation_not_permitted or resource_deadlock_would_occur
    catch (const std::system_error& e)
    {
        if (e.code() == std::errc::resource_deadlock_would_occur)
        {
            // Another thead may have alreayd called this method
            mLogSink->OnLogMessage(std::string("Excepton: WebRTC: ") + e.what());
        }
        else if (e.code() == std::errc::operation_not_permitted)
        {
            // This should not be reached
            mLogSink->OnLogMessage(std::string("Excepton: WebRTC: ") + e.what());
        }
        else
        {
            // Log any other message
            mLogSink->OnLogMessage(std::string("Excepton: WebRTC: ") + e.what());
        }
        // Device no longer being interacted with
        gWebRTCUpdateDevices = false;
        return;
    }
    catch (const std::exception& e)
    {
        mLogSink->OnLogMessage(std::string("Excepton: WebRTC: ") + e.what());
        // Device no longer being interacted with
        gWebRTCUpdateDevices = false;
        return;
    }
    // </FS:minerjr> [FIRE-36022]
}

void LLWebRTCImpl::OnDevicesUpdated()
{
    // <WolfViewer 2026-11-03> ticket 502: the ADM calls this on its own notification thread (Windows MMDevAPI:
    // audio_device_core_win.cc:3903; macOS CoreAudio listener: audio_device_mac.cc:1945), where updateDevices()
    // raced the worker thread over mPlayoutDeviceList / mRecordingDeviceList. Run it on the worker thread, as
    // secondlife/viewer develop llwebrtc.cpp OnDevicesUpdated() does.
    mWorkerThread->PostTask([this] { updateDevices(); });
    // </WolfViewer>
}

#if WEBRTC_WIN
// <WolfViewer 2026-11-03> ticket 502: Windows' default communications device changed (LLWebRTCDefaultDeviceWatcher,
// MMDevAPI thread). Only a "Default" selection follows it; a chosen device stays where it is.
void LLWebRTCImpl::onSystemDefaultDeviceChanged(bool render)
{
    mWorkerThread->PostTask(
        [this, render]
        {
            if (isDefaultDeviceSelection(render ? mPlayoutDevice : mRecordingDevice))
            {
                deployDevices();
            }
        });
}
// </WolfViewer>
#endif


void LLWebRTCImpl::setTuningMode(bool enable)
{
    mTuningMode = enable;
    if (!mTuningMode
        && !mMute
        && mPeerCustomProcessor
        && mPeerCustomProcessor->getGain() != mGain)
    {
        mPeerCustomProcessor->setGain(mGain);
    }
    mWorkerThread->PostTask(
        [this]
        {
            mDeviceModule->SetTuning(mTuningMode, mMute);
            mSignalingThread->PostTask(
                [this]
                {
                    for (auto& connection : mPeerConnections)
                    {
                        if (mTuningMode)
                        {
                            connection->enableSenderTracks(false);
                        }
                        else
                        {
                            connection->resetMute();
                        }
                        connection->enableReceiverTracks(!mTuningMode);
                    }
                });
        });
}

void LLWebRTCImpl::deployDevices()
{
    if (0 < mDevicesDeploying.fetch_add(1, std::memory_order_relaxed))
    {
        return;
    }
    mWorkerThread->PostTask(
        [this] {
            workerDeployDevices();
        });
}

float LLWebRTCImpl::getTuningAudioLevel()
{
    return mDeviceModule ? -20 * log10f(mDeviceModule->GetMicrophoneEnergy()) : std::numeric_limits<float>::infinity();
}

void LLWebRTCImpl::setTuningMicGain(float gain)
{
    if (mTuningMode && mDeviceModule)
    {
        mDeviceModule->SetTuningMicGain(gain);
    }
}

float LLWebRTCImpl::getPeerConnectionAudioLevel()
{
    return mTuningMode ? std::numeric_limits<float>::infinity()
                       : (mPeerCustomProcessor ? -20 * log10f(mPeerCustomProcessor->getMicrophoneEnergy())
                                               : std::numeric_limits<float>::infinity());
}

// <WolfViewer 2026-10-04> WOLF DJ: see LLWebRTCPlayoutTap.
void LLWebRTCImpl::setPlayoutTap(LLWebRTCPlayoutTap* tap)
{
    if (mDeviceModule)
    {
        mDeviceModule->SetPlayoutTap(tap);
    }
}
// </WolfViewer>

// [VR AUDIO 2026-11-04] see LLWebRTCExternalMic (llwebrtc.h)
void LLWebRTCImpl::setVRPlayoutTap(LLWebRTCPlayoutTap* tap)
{
    if (mDeviceModule)
    {
        mDeviceModule->SetVRPlayoutTap(tap);
    }
}

void LLWebRTCImpl::setPlayoutSilenced(bool silenced)
{
    if (mDeviceModule)
    {
        mDeviceModule->SetPlayoutSilenced(silenced);
    }
}

void LLWebRTCImpl::setExternalMic(LLWebRTCExternalMic* mic)
{
    if (mDeviceModule)
    {
        mDeviceModule->SetExternalMic(mic);
    }
}

// [AI MODE 2026-11-05] see LLWebRTCSpeechSource (llwebrtc.h)
void LLWebRTCImpl::setSpeechSource(LLWebRTCSpeechSource* source)
{
    if (mDeviceModule)
    {
        mDeviceModule->SetSpeechSource(source);
    }
}

void LLWebRTCImpl::setCaptureTap(LLWebRTCPlayoutTap* tap)
{
    if (mDeviceModule)
    {
        mDeviceModule->SetCaptureTap(tap);
    }
}

void LLWebRTCImpl::setListenTap(LLWebRTCPlayoutTap* tap)
{
    if (mDeviceModule)
    {
        mDeviceModule->SetListenTap(tap);
    }
}

void LLWebRTCImpl::setSpeechGain(bool open)
{
    if (mPeerCustomProcessor)
    {
        mPeerCustomProcessor->setSpeechGain(open);
    }
}

void LLWebRTCAudioDeviceModule::SetExternalMic(LLWebRTCExternalMic* mic)
{
    if (mic == external_mic_.load())
    {
        return;
    }
    // stop the clock (if any) before changing the source, then start it again for the new one
    mic_clock_stop_ = true;
    if (mic_clock_.joinable())
    {
        mic_clock_.join();
    }
    external_mic_.store(mic);
    audio_transport_.SetExternalMicActive(mic != nullptr);
    if (mic)
    {
        mic_clock_stop_ = false;
        mic_clock_ = std::thread([this] { micClock(); });
    }
}

// The headset microphone's clock: every 10 ms, 480 mono samples at 48 kHz to the voice engine, as a recording
// device would deliver them (pc/test/fake_audio_capture_module.h does the same: 10 ms frames from its own thread,
// 16-bit mono). Only while the app wants recording (ForceStartRecording; mute stops it as it stops a real mic).
void LLWebRTCAudioDeviceModule::micClock()
{
    constexpr size_t   kFrames = 480;
    constexpr uint32_t kRate   = 48000;
    int16_t            buf[kFrames];
    auto               next = std::chrono::steady_clock::now();
    uint32_t           mic_level = 0;
    while (!mic_clock_stop_.load())
    {
        next += std::chrono::milliseconds(10);
        std::this_thread::sleep_until(next);
        const auto now = std::chrono::steady_clock::now();
        if (now - next > std::chrono::milliseconds(100))
        {
            next = now;   // the thread was held up (a stall, a suspend): start the clock again, no burst
        }
        LLWebRTCExternalMic* mic = external_mic_.load();
        if (!mic || !recording_wanted_.load())
        {
            continue;
        }
        size_t got = mic->pullMic(buf, kFrames, kRate);
        if (got > kFrames) got = kFrames;
        if (got < kFrames)
        {
            memset(buf + got, 0, (kFrames - got) * sizeof(int16_t));
        }
        uint32_t new_level = mic_level;
        audio_transport_.DeliverCapture(buf, kFrames, sizeof(int16_t), 1, kRate, 0, 0, mic_level, false, new_level);
        mic_level = new_level;
    }
}

void LLWebRTCImpl::setMicGain(float gain)
{
    mGain = gain;
    if (!mTuningMode && mPeerCustomProcessor)
    {
        mPeerCustomProcessor->setGain(gain);
    }
}

void LLWebRTCImpl::setMute(bool mute, int delay_ms)
{
    if (mMute != mute)
    {
        mMute = mute;
        intSetMute(mute, delay_ms);
    }
}

void LLWebRTCImpl::intSetMute(bool mute, int delay_ms)
{
    if (mPeerCustomProcessor)
    {
        mPeerCustomProcessor->setGain(mMute ? 0.0f : mGain);
    }

    // Sequence counter to prevent race conditions from rapid requests to mute/unmute
    static std::atomic<uint32_t> mute_sequence(0);
    uint32_t current_sequence = ++mute_sequence;

    if (mMute)
    {
        mWorkerThread->PostDelayedTask(
            [this, current_sequence]
            {
                if (mDeviceModule && (current_sequence == mute_sequence.load()))
                {
                    // <FS:minerjr> [FIRE-36022] - Removing my USB headset crashes entire viewer
                    try // Try catch needed for uniquie lock as will throw an exception if a second lock is attempted or the mutex is
                        // invalid
                    {
                    // Attempt to lock the access to the audio device, wait up to 1 second for other threads to unlock.
                    std::unique_lock lock(gAudioDeviceMutex, 1s);
                    // If the lock could not be accessed, return as we don't have hardware access and will need to try again another pass.
                    // Prevents threads from interacting with the hardware at the same time as other audio/voice threads.
                    if (!lock.owns_lock())
                    {
                        return;
                    }
                    // Flag the device is being interacted with for the Co-routine in case something goes wrong.
                    gWebRTCUpdateDevices = true;
                    // </FS:minerjr> [FIRE-36022]
                    mDeviceModule->ForceStopRecording();
                    // <FS:minerjr> [FIRE-36022] - Removing my USB headset crashes entire viewer
                    // Finally signal to the co-routine everyting is OK.
                    gWebRTCUpdateDevices = false;
                    }
                    // There are two exceptions that unique_lock can trigger, operation_not_permitted or resource_deadlock_would_occur
                    catch (const std::system_error& e)
                    {
                        if (e.code() == std::errc::resource_deadlock_would_occur)
                        {
                            // Another thead may have alreayd called this method
                            mLogSink->OnLogMessage(std::string("Excepton: WebRTC: ") + e.what());
                        }
                        else if (e.code() == std::errc::operation_not_permitted)
                        {
                            // This should not be reached
                            mLogSink->OnLogMessage(std::string("Excepton: WebRTC: ") + e.what());
                        }
                        else
                        {
                            // Log any other message
                            mLogSink->OnLogMessage(std::string("Excepton: WebRTC: ") + e.what());
                        }
                        // Device no longer being interacted with
                        gWebRTCUpdateDevices = false;
                        return;
                    }
                    catch (const std::exception& e)
                    {
                        mLogSink->OnLogMessage(std::string("Excepton: WebRTC: ") + e.what());
                        // Device no longer being interacted with
                        gWebRTCUpdateDevices = false;
                        return;
                    }
                    // </FS:minerjr> [FIRE-36022]
                }
            },
            webrtc::TimeDelta::Millis(delay_ms));
    }
    else
    {
        mWorkerThread->PostTask(
            [this, current_sequence]
            {
                if (mDeviceModule && (current_sequence == mute_sequence.load()))
                {
                    // <FS:minerjr> [FIRE-36022] - Removing my USB headset crashes entire viewer
                    try // Try catch needed for uniquie lock as will throw an exception if a second lock is attempted or the mutex is
                        // invalid
                    {
                    // Attempt to lock the access to the audio device, wait up to 1 second for other threads to unlock.
                    std::unique_lock lock(gAudioDeviceMutex, 1s);
                    // If the lock could not be accessed, return as we don't have hardware access and will need to try again another pass.
                    // Prevents threads from interacting with the hardware at the same time as other audio/voice threads.
                    if (!lock.owns_lock())
                    {
                        return;
                    }
                    // Flag the device is being interacted with for the Co-routine in case something goes wrong.
                    gWebRTCUpdateDevices = true;
                    // </FS:minerjr> [FIRE-36022]
                    mDeviceModule->InitRecording();
                    mDeviceModule->ForceStartRecording();
                    // <FS:minerjr> [FIRE-36022] - Removing my USB headset crashes entire viewer
                    // Finally signal to the co-routine everyting is OK.
                    gWebRTCUpdateDevices = false;
                    }
                    // There are two exceptions that unique_lock can trigger, operation_not_permitted or resource_deadlock_would_occur
                    catch (const std::system_error& e)
                    {
                        if (e.code() == std::errc::resource_deadlock_would_occur)
                        {
                            // Another thead may have alreayd called this method
                            mLogSink->OnLogMessage(std::string("Excepton: WebRTC: ") + e.what());
                        }
                        else if (e.code() == std::errc::operation_not_permitted)
                        {
                            // This should not be reached
                            mLogSink->OnLogMessage(std::string("Excepton: WebRTC: ") + e.what());
                        }
                        else
                        {
                            // Log any other message
                            mLogSink->OnLogMessage(std::string("Excepton: WebRTC: ") + e.what());
                        }
                        // Device no longer being interacted with
                        gWebRTCUpdateDevices = false;
                        return;
                    }
                    catch (const std::exception& e)
                    {
                        mLogSink->OnLogMessage(std::string("Excepton: WebRTC: ") + e.what());
                        // Device no longer being interacted with
                        gWebRTCUpdateDevices = false;
                        return;
                    }
                    // </FS:minerjr> [FIRE-36022]
                }
            });
    }
}

//
// Peer Connection Helpers
//

LLWebRTCPeerConnectionInterface *LLWebRTCImpl::newPeerConnection()
{
    bool empty = mPeerConnections.empty();
    webrtc::scoped_refptr<LLWebRTCPeerConnectionImpl> peerConnection = webrtc::scoped_refptr<LLWebRTCPeerConnectionImpl>(new webrtc::RefCountedObject<LLWebRTCPeerConnectionImpl>());
    peerConnection->init(this);
    if (mPeerConnections.empty())
    {
        intSetMute(mMute);
    }
    mPeerConnections.emplace_back(peerConnection);

    peerConnection->enableSenderTracks(false);
    peerConnection->resetMute();
    return peerConnection.get();
}

void LLWebRTCImpl::freePeerConnection(LLWebRTCPeerConnectionInterface* peer_connection)
{
    std::vector<webrtc::scoped_refptr<LLWebRTCPeerConnectionImpl>>::iterator it =
    std::find(mPeerConnections.begin(), mPeerConnections.end(), peer_connection);
    if (it != mPeerConnections.end())
    {
        // Todo: make sure conection had no jobs in workers
        mPeerConnections.erase(it);
        if (mPeerConnections.empty())
        {
            intSetMute(true);
        }
    }
}


//
// LLWebRTCPeerConnectionImpl implementation.
//
// Most peer connection (signaling) happens on
// the signaling thread.

LLWebRTCPeerConnectionImpl::LLWebRTCPeerConnectionImpl() :
    mWebRTCImpl(nullptr),
    mPeerConnection(nullptr),
    mMute(MUTE_INITIAL),
    mAnswerReceived(false),
    mPeerConnectionState(webrtc::PeerConnectionInterface::PeerConnectionState::kNew),
    mDisconnectCount(0),
    mPendingJobs(0)
{
}

LLWebRTCPeerConnectionImpl::~LLWebRTCPeerConnectionImpl()
{
    mSignalingObserverList.clear();
    mDataObserverList.clear();
    mPeerConnectionFactory.release();
    if (mPendingJobs > 0)
    {
        RTC_LOG(LS_ERROR) << __FUNCTION__ << "Destroying a connection that has " << std::to_string(mPendingJobs) << " unfinished jobs that might cause workers to crash";
    }
}

//
// LLWebRTCPeerConnection interface
//

void LLWebRTCPeerConnectionImpl::init(LLWebRTCImpl * webrtc_impl)
{
    mWebRTCImpl = webrtc_impl;
    mPeerConnectionFactory = mWebRTCImpl->getPeerConnectionFactory();
}

void LLWebRTCPeerConnectionImpl::terminate()
{
    mPendingJobs++;
    mWebRTCImpl->PostSignalingTask(
        [this]()
        {
            if (mPeerConnection)
            {
                if (mDataChannel)
                {
                    {
                        mDataChannel->Close();
                        mDataChannel = nullptr;
                    }
                }

                // to remove 'Secondlife is recording' icon from taskbar
                // if user was speaking
                auto senders = mPeerConnection->GetSenders();
                for (auto& sender : senders)
                {
                    auto track = sender->track();
                    if (track)
                    {
                        track->set_enabled(false);
                    }
                }

                mPeerConnection->Close();
                if (mLocalStream)
                {
                    auto tracks = mLocalStream->GetAudioTracks();
                    for (auto& track : tracks)
                    {
                        mLocalStream->RemoveTrack(track);
                    }
                    mLocalStream = nullptr;
                }
                mPeerConnection = nullptr;

                for (auto &observer : mSignalingObserverList)
                {
                    observer->OnPeerConnectionClosed();
                }
            }
            mPendingJobs--;
        });
}

void LLWebRTCPeerConnectionImpl::setSignalingObserver(LLWebRTCSignalingObserver *observer) { mSignalingObserverList.emplace_back(observer); }

void LLWebRTCPeerConnectionImpl::unsetSignalingObserver(LLWebRTCSignalingObserver *observer)
{
    std::vector<LLWebRTCSignalingObserver *>::iterator it =
    std::find(mSignalingObserverList.begin(), mSignalingObserverList.end(), observer);
    if (it != mSignalingObserverList.end())
    {
        mSignalingObserverList.erase(it);
    }
}


bool LLWebRTCPeerConnectionImpl::initializeConnection(const LLWebRTCPeerConnectionInterface::InitOptions& options)
{
    RTC_DCHECK(!mPeerConnection);
    mAnswerReceived = false;

    mPendingJobs++;
    mWebRTCImpl->PostSignalingTask(
        [this,options]()
        {
            webrtc::PeerConnectionInterface::RTCConfiguration config;
            for (auto server : options.mServers)
            {
                webrtc::PeerConnectionInterface::IceServer ice_server;
                for (auto url : server.mUrls)
                {
                    ice_server.urls.push_back(url);
                }
                ice_server.username = server.mUserName;
                ice_server.password = server.mPassword;
                config.servers.push_back(ice_server);
            }
            config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;

            config.set_min_port(60000);
            config.set_max_port(60100);

            webrtc::PeerConnectionDependencies pc_dependencies(this);
            // Other thread manages mPeerConnectionFactory's lifetime and it can be reset
            // at any momment, create own scoped_refptr (atomic).
            webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> peer_connection_factory = mPeerConnectionFactory;
            if (peer_connection_factory == nullptr)
            {
                RTC_LOG(LS_ERROR) << __FUNCTION__ << "Error creating peer connection, factory doesn't exist";
                // Too early?
                mPendingJobs--;
                return;
            }
            auto error_or_peer_connection = peer_connection_factory->CreatePeerConnectionOrError(config, std::move(pc_dependencies));
            if (error_or_peer_connection.ok())
            {
                mPeerConnection = std::move(error_or_peer_connection.value());
            }
            else
            {
                RTC_LOG(LS_ERROR) << __FUNCTION__ << "Error creating peer connection: " << error_or_peer_connection.error().message();
                for (auto &observer : mSignalingObserverList)
                {
                    observer->OnRenegotiationNeeded();
                }
                mPendingJobs--;
                return;
            }

            webrtc::DataChannelInit init;
            init.ordered = true;

            auto data_channel_or_error = mPeerConnection->CreateDataChannelOrError("SLData", &init);
            if (data_channel_or_error.ok())
            {
                mDataChannel = std::move(data_channel_or_error.value());

                mDataChannel->RegisterObserver(this);
            }

            webrtc::AudioOptions audioOptions;
            audioOptions.auto_gain_control = true;
            audioOptions.echo_cancellation = true;
            audioOptions.noise_suppression = true;
            audioOptions.init_recording_on_send = false;

            mLocalStream = peer_connection_factory->CreateLocalMediaStream("SLStream");

            webrtc::scoped_refptr<webrtc::AudioTrackInterface> audio_track(
                peer_connection_factory->CreateAudioTrack("SLAudio", peer_connection_factory->CreateAudioSource(audioOptions).get()));
            audio_track->set_enabled(false);
            mLocalStream->AddTrack(audio_track);

            mPeerConnection->AddTrack(audio_track, {"SLStream"});

            auto senders = mPeerConnection->GetSenders();

            for (auto &sender : senders)
            {
                webrtc::RtpParameters      params;
                webrtc::RtpCodecParameters codecparam;
                codecparam.name                       = "opus";
                codecparam.kind                       = webrtc::MediaType::AUDIO;
                codecparam.clock_rate                 = 48000;
                codecparam.num_channels               = 2;
                codecparam.parameters["stereo"]       = "1";
                codecparam.parameters["sprop-stereo"] = "1";
                params.codecs.push_back(codecparam);
                sender->SetParameters(params);
            }

            auto receivers = mPeerConnection->GetReceivers();
            for (auto &receiver : receivers)
            {
                webrtc::RtpParameters      params;
                webrtc::RtpCodecParameters codecparam;
                codecparam.name                       = "opus";
                codecparam.kind                       = webrtc::MediaType::AUDIO;
                codecparam.clock_rate                 = 48000;
                codecparam.num_channels               = 2;
                codecparam.parameters["stereo"]       = "1";
                codecparam.parameters["sprop-stereo"] = "1";
                params.codecs.push_back(codecparam);
                receiver->SetParameters(params);
            }

            webrtc::PeerConnectionInterface::RTCOfferAnswerOptions offerOptions;
            this->AddRef(); // CreateOffer will deref this when it's done.  Without this, the callbacks never get called.
            mPeerConnection->CreateOffer(this, offerOptions);
            mPendingJobs--;
        });

    return true;
}

bool LLWebRTCPeerConnectionImpl::shutdownConnection()
{
    terminate();
    return true;
}

void LLWebRTCPeerConnectionImpl::enableSenderTracks(bool enable)
{
    // set_enabled shouldn't be done on the worker thread.
    if (mPeerConnection)
    {
        auto senders = mPeerConnection->GetSenders();
        for (auto &sender : senders)
        {
            sender->track()->set_enabled(enable);
        }
    }
}

void LLWebRTCPeerConnectionImpl::enableReceiverTracks(bool enable)
{
    // set_enabled shouldn't be done on the worker thread
    if (mPeerConnection)
    {
        auto receivers = mPeerConnection->GetReceivers();
        for (auto &receiver : receivers)
        {
            receiver->track()->set_enabled(enable);
        }
    }
}

// Tell the peer connection that we've received a SDP answer from the sim.
void LLWebRTCPeerConnectionImpl::AnswerAvailable(const std::string &sdp)
{
    RTC_LOG(LS_INFO) << __FUNCTION__ << " Remote SDP: " << sdp;

    mPendingJobs++;
    mWebRTCImpl->PostSignalingTask(
                               [this, sdp]()
                               {
                                   if (mPeerConnection)
                                   {
                                       RTC_LOG(LS_INFO) << __FUNCTION__ << " " << mPeerConnection->peer_connection_state();
                                       mPeerConnection->SetRemoteDescription(webrtc::CreateSessionDescription(webrtc::SdpType::kAnswer, sdp),
                                                                             webrtc::scoped_refptr<webrtc::SetRemoteDescriptionObserverInterface>(this));
                                   }
                                   mPendingJobs--;
                               });
}


//
// LLWebRTCAudioInterface implementation
//

void LLWebRTCPeerConnectionImpl::setMute(bool mute)
{
    EMicMuteState new_state = mute ? MUTE_MUTED : MUTE_UNMUTED;

    // even if mute hasn't changed, we still need to update the mute
    // state on the connections to handle cases where the 'Default' device
    // has changed in the OS (unplugged headset, etc.) which messes
    // with the mute state.

    bool force_reset = mMute == MUTE_INITIAL && mute;
    bool enable = !mute;
    mMute = new_state;


    mPendingJobs++;
    mWebRTCImpl->PostSignalingTask(
        [this, force_reset, enable]()
        {
        if (mPeerConnection)
        {
            auto senders = mPeerConnection->GetSenders();

            RTC_LOG(LS_INFO) << __FUNCTION__ << (mMute ? "disabling" : "enabling") << " streams count " << senders.size();
            for (auto &sender : senders)
            {
                auto track = sender->track();
                if (track)
                {
                    if (force_reset)
                    {
                        // Force notify observers
                        // Was it disabled too early?
                        // Without this microphone icon in Win's taskbar will stay
                        track->set_enabled(true);
                    }
                    track->set_enabled(enable);
                }
            }
            mPendingJobs--;
        }
    });
}

void LLWebRTCPeerConnectionImpl::resetMute()
{
    switch(mMute)
    {
    case MUTE_MUTED:
         setMute(true);
         break;
    case MUTE_UNMUTED:
         setMute(false);
         break;
    default:
        break;
    }
}

void LLWebRTCPeerConnectionImpl::setReceiveVolume(float volume)
{
    mPendingJobs++;
    mWebRTCImpl->PostSignalingTask(
        [this, volume]()
        {
            if (mPeerConnection)
            {
                auto receivers = mPeerConnection->GetReceivers();

                for (auto &receiver : receivers)
                {
                    for (auto &stream : receiver->streams())
                    {
                        for (auto &track : stream->GetAudioTracks())
                        {
                            track->GetSource()->SetVolume(volume);
                        }
                    }
                }
            }
            mPendingJobs--;
        });
}

void LLWebRTCPeerConnectionImpl::setSendVolume(float volume)
{
    mPendingJobs++;
    mWebRTCImpl->PostSignalingTask(
        [this, volume]()
        {
            if (mLocalStream)
            {
                for (auto &track : mLocalStream->GetAudioTracks())
                {
                    track->GetSource()->SetVolume(volume*5.0);
                }
            }
            mPendingJobs--;
        });
}

//
// PeerConnectionObserver implementation.
//

void LLWebRTCPeerConnectionImpl::OnAddTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface>                     receiver,
                                            const std::vector<webrtc::scoped_refptr<webrtc::MediaStreamInterface>> &streams)
{
    RTC_LOG(LS_INFO) << __FUNCTION__ << " " << receiver->id();
    webrtc::RtpParameters      params;
    webrtc::RtpCodecParameters codecparam;
    codecparam.name                       = "opus";
    codecparam.kind                       = webrtc::MediaType::AUDIO;
    codecparam.clock_rate                 = 48000;
    codecparam.num_channels               = 2;
    codecparam.parameters["stereo"]       = "1";
    codecparam.parameters["sprop-stereo"] = "1";
    params.codecs.push_back(codecparam);
    receiver->SetParameters(params);
}

void LLWebRTCPeerConnectionImpl::OnRemoveTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver)
{
    RTC_LOG(LS_INFO) << __FUNCTION__ << " " << receiver->id();
}

void LLWebRTCPeerConnectionImpl::OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel)
{
    if (mDataChannel)
    {
        mDataChannel->UnregisterObserver();
    }
    mDataChannel = channel;
    channel->RegisterObserver(this);
}

void LLWebRTCPeerConnectionImpl::OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState new_state)
{
    LLWebRTCSignalingObserver::EIceGatheringState webrtc_new_state = LLWebRTCSignalingObserver::EIceGatheringState::ICE_GATHERING_NEW;
    switch (new_state)
    {
        case webrtc::PeerConnectionInterface::IceGatheringState::kIceGatheringNew:
            webrtc_new_state = LLWebRTCSignalingObserver::EIceGatheringState::ICE_GATHERING_NEW;
            break;
        case webrtc::PeerConnectionInterface::IceGatheringState::kIceGatheringGathering:
            webrtc_new_state = LLWebRTCSignalingObserver::EIceGatheringState::ICE_GATHERING_GATHERING;
            break;
        case webrtc::PeerConnectionInterface::IceGatheringState::kIceGatheringComplete:
            webrtc_new_state = LLWebRTCSignalingObserver::EIceGatheringState::ICE_GATHERING_COMPLETE;
            break;
        default:
            RTC_LOG(LS_ERROR) << __FUNCTION__ << " Bad Ice Gathering State" << new_state;
            webrtc_new_state = LLWebRTCSignalingObserver::EIceGatheringState::ICE_GATHERING_NEW;
            return;
    }

    if (mAnswerReceived)
    {
        for (auto &observer : mSignalingObserverList)
        {
            observer->OnIceGatheringState(webrtc_new_state);
        }
    }
}

static const webrtc::TimeDelta DISCONNECT_RENEGOTIATE_DELAY = webrtc::TimeDelta::Millis(10000);

// Called any time the PeerConnectionState changes.
void LLWebRTCPeerConnectionImpl::OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState new_state)
{
    RTC_LOG(LS_ERROR) << __FUNCTION__ << " Peer Connection State Change " << new_state;

    mPeerConnectionState = new_state;

    switch (new_state)
    {
        case webrtc::PeerConnectionInterface::PeerConnectionState::kConnected:
        {
            mPendingJobs++;
            mWebRTCImpl->PostWorkerTask([this]() {
                for (auto &observer : mSignalingObserverList)
                {
                    observer->OnAudioEstablished(this);
                }
                mPendingJobs--;
            });
            break;
        }
        case webrtc::PeerConnectionInterface::PeerConnectionState::kFailed:
        {
            for (auto &observer : mSignalingObserverList)
            {
                observer->OnRenegotiationNeeded();
            }
            break;
        }
        case webrtc::PeerConnectionInterface::PeerConnectionState::kDisconnected:
        {
            // Wait 10 seconds before renegotiating in case the connection recovers on its own.
            // Use a sequence count so that only the most recent disconnect transition can trigger
            // a renegotiation, avoiding stale delayed tasks from earlier disconnect/reconnect cycles.
            uint32_t disconnect_count = ++mDisconnectCount;
            mWebRTCImpl->PostDelayedSignalingTask(
                [this, disconnect_count]()
                {
                    if (disconnect_count == mDisconnectCount
                        && mPeerConnectionState == webrtc::PeerConnectionInterface::PeerConnectionState::kDisconnected)
                    {
                        for (auto &observer : mSignalingObserverList)
                        {
                            observer->OnRenegotiationNeeded();
                        }
                    }
                },
                DISCONNECT_RENEGOTIATE_DELAY);
            break;
        }
        default:
        {
            break;
        }
    }
}

// Convert an ICE candidate into a string appropriate for trickling
// to the Secondlife WebRTC server via the sim.
static std::string iceCandidateToTrickleString(const webrtc::IceCandidateInterface *candidate)
{
    std::ostringstream candidate_stream;

    candidate_stream <<
    candidate->candidate().foundation() << " " <<
    std::to_string(candidate->candidate().component()) << " " <<
    candidate->candidate().protocol() << " " <<
    std::to_string(candidate->candidate().priority()) << " " <<
    candidate->candidate().address().ipaddr().ToString() << " " <<
    candidate->candidate().address().PortAsString() << " typ ";

    if (candidate->candidate().type() == webrtc::IceCandidateType::kHost)
    {
        candidate_stream << "host";
    }
    else if (candidate->candidate().type() == webrtc::IceCandidateType::kSrflx)
    {
        candidate_stream << "srflx " <<
        "raddr " << candidate->candidate().related_address().ipaddr().ToString() << " " <<
        "rport " << candidate->candidate().related_address().PortAsString();
    }
    else if (candidate->candidate().type() == webrtc::IceCandidateType::kRelay)
    {
        candidate_stream << "relay " <<
        "raddr " << candidate->candidate().related_address().ipaddr().ToString() << " " <<
        "rport " << candidate->candidate().related_address().PortAsString();
    }
    else if (candidate->candidate().type() == webrtc::IceCandidateType::kPrflx)
    {
        candidate_stream << "prflx " <<
        "raddr " << candidate->candidate().related_address().ipaddr().ToString() << " " <<
        "rport " << candidate->candidate().related_address().PortAsString();
    }
    else {
        RTC_LOG(LS_ERROR) << __FUNCTION__ << " Unknown candidate type " << candidate->candidate().type();
    }
    if (candidate->candidate().protocol() == "tcp")
    {
        candidate_stream << " tcptype " << candidate->candidate().tcptype();
    }

    return candidate_stream.str();
}

// The webrtc library has a new ice candidate.
void LLWebRTCPeerConnectionImpl::OnIceCandidate(const webrtc::IceCandidateInterface *candidate)
{
    RTC_LOG(LS_INFO) << __FUNCTION__ << " " << candidate->sdp_mline_index();

    if (!candidate)
    {
        RTC_LOG(LS_ERROR) << __FUNCTION__ << " No Ice Candidate Given";
        return;
    }
    if (mAnswerReceived)
    {
        // We've already received an answer SDP from the Secondlife WebRTC server
        // so simply tell observers about our new ice candidate.
        for (auto &observer : mSignalingObserverList)
        {
            LLWebRTCIceCandidate ice_candidate;
            ice_candidate.mCandidate  = iceCandidateToTrickleString(candidate);
            ice_candidate.mMLineIndex = candidate->sdp_mline_index();
            ice_candidate.mSdpMid     = candidate->sdp_mid();
            observer->OnIceCandidate(ice_candidate);
        }
    }
    else
    {
        // As we've not yet received our answer, cache the candidate.
        mCachedIceCandidates.push_back(
            webrtc::CreateIceCandidate(candidate->sdp_mid(),
                                       candidate->sdp_mline_index(),
                                       candidate->candidate()));
    }
}

//
// CreateSessionDescriptionObserver implementation.
//
void LLWebRTCPeerConnectionImpl::OnSuccess(webrtc::SessionDescriptionInterface *desc)
{
    std::string sdp;
    desc->ToString(&sdp);
    RTC_LOG(LS_INFO) << sdp;
    ;
    // mangle the sdp as this is the only way currently to bump up
    // the send audio rate to 48k
    std::istringstream sdp_stream(sdp);
    std::ostringstream sdp_mangled_stream;
    std::string        sdp_line;
    std::string opus_payload;
    while (std::getline(sdp_stream, sdp_line))
    {
        int bandwidth  = 0;
        int payload_id = 0;
        // force mono down, stereo up
        if (std::sscanf(sdp_line.c_str(), "a=rtpmap:%i opus/%i/2", &payload_id, &bandwidth) == 2)
        {
            opus_payload = std::to_string(payload_id);
            sdp_mangled_stream << "a=rtpmap:" << opus_payload << " opus/48000/2" << "\n";
        }
        else if (sdp_line.find("a=fmtp:" + opus_payload) == 0)
        {
            sdp_mangled_stream << sdp_line << "a=fmtp:" << opus_payload
            << " minptime=10;useinbandfec=1;stereo=1;sprop-stereo=1;maxplaybackrate=48000;sprop-maxplaybackrate=48000;sprop-maxcapturerate=48000\n";
        }
        else
        {
            sdp_mangled_stream << sdp_line << "\n";
        }
    }

    RTC_LOG(LS_INFO) << __FUNCTION__ << " Local SDP: " << sdp_mangled_stream.str();
    std::string mangled_sdp = sdp_mangled_stream.str();
    for (auto &observer : mSignalingObserverList)
    {
        observer->OnOfferAvailable(mangled_sdp);
    }

   mPeerConnection->SetLocalDescription(std::unique_ptr<webrtc::SessionDescriptionInterface>(
                                                     webrtc::CreateSessionDescription(webrtc::SdpType::kOffer, mangled_sdp)),
                                                 webrtc::scoped_refptr<webrtc::SetLocalDescriptionObserverInterface>(this));

}

void LLWebRTCPeerConnectionImpl::OnFailure(webrtc::RTCError error)
{
    RTC_LOG(LS_ERROR) << ToString(error.type()) << ": " << error.message();
    for (auto &observer : mSignalingObserverList)
    {
        observer->OnRenegotiationNeeded();
    }
}

//
// SetRemoteDescriptionObserverInterface implementation.
//
void LLWebRTCPeerConnectionImpl::OnSetRemoteDescriptionComplete(webrtc::RTCError error)
{
    // we've received an answer SDP from the sim.

    RTC_LOG(LS_INFO) << __FUNCTION__ << " " << mPeerConnection->signaling_state();
    if (!error.ok())
    {
        RTC_LOG(LS_ERROR) << ToString(error.type()) << ": " << error.message();
        for (auto &observer : mSignalingObserverList)
        {
            observer->OnRenegotiationNeeded();
        }
        return;
    }
    mAnswerReceived = true;

    // tell the observers about any cached ICE candidates.
    for (auto &observer : mSignalingObserverList)
    {
        for (auto &candidate : mCachedIceCandidates)
        {
            LLWebRTCIceCandidate ice_candidate;
            ice_candidate.mCandidate  = iceCandidateToTrickleString(candidate.get());
            ice_candidate.mMLineIndex = candidate->sdp_mline_index();
            ice_candidate.mSdpMid     = candidate->sdp_mid();
            observer->OnIceCandidate(ice_candidate);
        }
    }
    mCachedIceCandidates.clear();
    if (mPeerConnection)
    {
        OnIceGatheringChange(mPeerConnection->ice_gathering_state());
    }

}

//
// SetLocalDescriptionObserverInterface implementation.
//
void LLWebRTCPeerConnectionImpl::OnSetLocalDescriptionComplete(webrtc::RTCError error)
{
}

//
// DataChannelObserver implementation
//

void LLWebRTCPeerConnectionImpl::OnStateChange()
{
    if (!mDataChannel)
    {
        return;
    }
    RTC_LOG(LS_INFO) << __FUNCTION__ << " Data Channel State: " << webrtc::DataChannelInterface::DataStateString(mDataChannel->state());
    switch (mDataChannel->state())
    {
        case webrtc::DataChannelInterface::kOpen:
            RTC_LOG(LS_INFO) << __FUNCTION__ << " Data Channel State Open";
            for (auto &observer : mSignalingObserverList)
            {
                observer->OnDataChannelReady(this);
            }
            break;
        case webrtc::DataChannelInterface::kConnecting:
            RTC_LOG(LS_INFO) << __FUNCTION__ << " Data Channel State Connecting";
            break;
        case webrtc::DataChannelInterface::kClosing:
            RTC_LOG(LS_INFO) << __FUNCTION__ << " Data Channel State closing";
            break;
        case webrtc::DataChannelInterface::kClosed:
            RTC_LOG(LS_INFO) << __FUNCTION__ << " Data Channel State closed";
            break;
        default:
            break;
    }
}

void LLWebRTCPeerConnectionImpl::OnMessage(const webrtc::DataBuffer& buffer)
{
    std::string data((const char*)buffer.data.cdata(), buffer.size());
    for (auto &observer : mDataObserverList)
    {
        observer->OnDataReceived(data, buffer.binary);
    }
}

//
// LLWebRTCDataInterface
//

void LLWebRTCPeerConnectionImpl::sendData(const std::string& data, bool binary)
{
    if (mDataChannel)
    {
        webrtc::CopyOnWriteBuffer cowBuffer(data.data(), data.length());
        webrtc::DataBuffer     buffer(cowBuffer, binary);
        mPendingJobs++;
        mWebRTCImpl->PostNetworkTask([this, buffer]() {
                if (mDataChannel)
                {
                    mDataChannel->Send(buffer);
                }
                mPendingJobs--;
            });
    }
}

void LLWebRTCPeerConnectionImpl::setDataObserver(LLWebRTCDataObserver* observer)
{
    mDataObserverList.emplace_back(observer);
}

void LLWebRTCPeerConnectionImpl::unsetDataObserver(LLWebRTCDataObserver* observer)
{
    std::vector<LLWebRTCDataObserver *>::iterator it =
    std::find(mDataObserverList.begin(), mDataObserverList.end(), observer);
    if (it != mDataObserverList.end())
    {
        mDataObserverList.erase(it);
    }
}

LLWebRTCImpl * gWebRTCImpl = nullptr;
LLWebRTCDeviceInterface * getDeviceInterface()
{
    return gWebRTCImpl;
}

LLWebRTCPeerConnectionInterface* newPeerConnection()
{
    return gWebRTCImpl->newPeerConnection();
}

void freePeerConnection(LLWebRTCPeerConnectionInterface* peer_connection)
{
    gWebRTCImpl->freePeerConnection(peer_connection);
}


void init(LLWebRTCLogCallback* logCallback)
{
    if (gWebRTCImpl)
    {
        return;
    }
    gWebRTCImpl = new LLWebRTCImpl(logCallback);
    gWebRTCImpl->init();
}

void terminate()
{
    if (gWebRTCImpl)
    {
        gWebRTCImpl->terminate();
        delete gWebRTCImpl;
        gWebRTCImpl = nullptr;
    }
}

}  // namespace llwebrtc
