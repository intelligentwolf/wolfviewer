/**
 * @file wolfdjcapture_win.cpp
 * @brief WolfViewer Wolf DJ capture on Windows: WASAPI. Microphone = the default capture
 *        endpoint; "Everything you hear" = loopback of the default render endpoint; one program
 *        = process loopback (Windows 10 build 20348 or later).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "wolfdjcapture.h"
#include "wolfdjaudio.h"

#include "llwin32headers.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <audioclientactivationparams.h>
#include <mmreg.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <algorithm>
#include <atomic>
#include <thread>

// [WOLF DJ 2026-10-04] Process loopback - Source: Microsoft Windows-classic-samples
// ApplicationLoopback (LoopbackCapture.cpp/.h): AUDIOCLIENT_ACTIVATION_PARAMS with
// AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK + PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE
// in a VT_BLOB PROPVARIANT, ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
// __uuidof(IAudioClient)), a RuntimeClass<ClassicCom, FtmBase, IActivateAudioInterfaceCompletionHandler>
// completion handler, our own capture format (the process loopback client has no mix format),
// Initialize(SHARED, LOOPBACK | EVENTCALLBACK | AUTOCONVERTPCM), SetEventHandle, GetService
// (IAudioCaptureClient). AUDIOCLIENT_ACTIVATION_TYPE "Minimum supported client: Windows 10
// Build 20348" (learn.microsoft.com, audioclientactivationparams.h).

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr DWORD MIN_PROCESS_LOOPBACK_BUILD = 20348;
    constexpr REFERENCE_TIME BUFFER_HNS = 2000000;     // 200 ms

    DWORD windows_build()
    {
        typedef LONG(WINAPI* RtlGetVersionFn)(PRTL_OSVERSIONINFOW);
        static DWORD build = 0;
        if (build == 0)
        {
            if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"))
            {
                if (RtlGetVersionFn fn = (RtlGetVersionFn)GetProcAddress(ntdll, "RtlGetVersion"))
                {
                    RTL_OSVERSIONINFOW vi = {};
                    vi.dwOSVersionInfoSize = sizeof(vi);
                    if (fn(&vi) == 0) build = vi.dwBuildNumber;
                }
            }
        }
        return build;
    }

    std::string process_name(DWORD pid)
    {
        std::string name = "Program " + std::to_string(pid);
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) return name;
        wchar_t path[MAX_PATH];
        DWORD len = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, path, &len))
        {
            std::wstring p(path, len);
            const size_t slash = p.find_last_of(L"\\/");
            if (slash != std::wstring::npos) p = p.substr(slash + 1);
            const size_t dot = p.find_last_of(L'.');
            if (dot != std::wstring::npos) p = p.substr(0, dot);
            name = ll_convert_wide_to_string(p);
        }
        CloseHandle(h);
        return name;
    }

    struct ComScope
    {
        HRESULT mHr;
        ComScope() : mHr(CoInitializeEx(NULL, COINIT_MULTITHREADED)) {}
        ~ComScope() { if (SUCCEEDED(mHr)) CoUninitialize(); }
    };

    class ActivateHandler : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                                                 Microsoft::WRL::FtmBase, IActivateAudioInterfaceCompletionHandler>
    {
    public:
        ActivateHandler() { mDone = CreateEventW(NULL, TRUE, FALSE, NULL); }
        ~ActivateHandler() { if (mDone) CloseHandle(mDone); }

        STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation* op) override
        {
            HRESULT hr_activate = E_FAIL;
            ComPtr<IUnknown> unk;
            HRESULT hr = op->GetActivateResult(&hr_activate, &unk);
            if (SUCCEEDED(hr)) hr = hr_activate;
            if (SUCCEEDED(hr)) hr = unk.As(&mClient);
            mResult = hr;
            SetEvent(mDone);
            return S_OK;
        }

        HANDLE mDone = NULL;
        HRESULT mResult = E_PENDING;
        ComPtr<IAudioClient> mClient;
    };

    class WasapiStream : public WolfDJCaptureStream
    {
    public:
        enum EMode { MIC, EVERYTHING, PROCESS };

        WasapiStream(WolfDJChannel* ch, EMode mode, DWORD pid) : mChannel(ch), mMode(mode), mPid(pid) {}

        ~WasapiStream() override
        {
            mStop = true;
            if (mThread.joinable()) mThread.join();
            mChannel->clear();
        }

        bool ok() const override { return !mFailed; }
        std::string error() const override { return mFailed ? std::string("The sound source stopped.") : std::string(); }

        // Starts the capture thread and waits until it is capturing (or has failed).
        bool start(std::string& err)
        {
            HANDLE ready = CreateEventW(NULL, TRUE, FALSE, NULL);
            mThread = std::thread([this, ready]() { run(ready); });
            WaitForSingleObject(ready, 5000);
            CloseHandle(ready);
            if (!mStarted)
            {
                err = mStartError.empty() ? std::string("Could not start capturing.") : mStartError;
                mStop = true;
                if (mThread.joinable()) mThread.join();
                return false;
            }
            return true;
        }

    private:
        bool openClient(ComPtr<IAudioClient>& client, WAVEFORMATEX*& fmt, WAVEFORMATEX& own_fmt, HANDLE event)
        {
            if (mMode == PROCESS)
            {
                AUDIOCLIENT_ACTIVATION_PARAMS params = {};
                params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
                params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
                params.ProcessLoopbackParams.TargetProcessId = mPid;
                PROPVARIANT pv = {};
                pv.vt = VT_BLOB;
                pv.blob.cbSize = sizeof(params);
                pv.blob.pBlobData = (BYTE*)&params;
                ComPtr<ActivateHandler> handler = Microsoft::WRL::Make<ActivateHandler>();
                ComPtr<IActivateAudioInterfaceAsyncOperation> op;
                HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv, handler.Get(), &op);
                if (FAILED(hr) || WaitForSingleObject(handler->mDone, 5000) != WAIT_OBJECT_0 || FAILED(handler->mResult) || !handler->mClient)
                {
                    mStartError = "Windows would not let the viewer capture that program.";
                    return false;
                }
                client = handler->mClient;
                // 16-bit PCM stereo 48 kHz, converted by Windows (AUTOCONVERTPCM), as the sample does.
                own_fmt = {};
                own_fmt.wFormatTag = WAVE_FORMAT_PCM;
                own_fmt.nChannels = 2;
                own_fmt.nSamplesPerSec = WolfDJ::RATE;
                own_fmt.wBitsPerSample = 16;
                own_fmt.nBlockAlign = own_fmt.nChannels * own_fmt.wBitsPerSample / 8;
                own_fmt.nAvgBytesPerSec = own_fmt.nSamplesPerSec * own_fmt.nBlockAlign;
                fmt = &own_fmt;
                hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                                        BUFFER_HNS, 0, fmt, NULL);
                if (FAILED(hr) || FAILED(client->SetEventHandle(event)))
                {
                    mStartError = "Could not start capturing that program.";
                    return false;
                }
                return true;
            }

            ComPtr<IMMDeviceEnumerator> en;
            ComPtr<IMMDevice> dev;
            if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, IID_PPV_ARGS(&en))) ||
                FAILED(en->GetDefaultAudioEndpoint(mMode == MIC ? eCapture : eRender, eConsole, &dev)) ||
                FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)client.GetAddressOf())))
            {
                mStartError = mMode == MIC ? "No microphone found." : "No sound output found.";
                return false;
            }
            if (FAILED(client->GetMixFormat(&fmt)) || !fmt)
            {
                mStartError = "Could not read the sound device's format.";
                return false;
            }
            mMixFormat = fmt;   // CoTaskMemFree later
            // Polled every 10 ms (no event handle): loopback capture of a render endpoint and
            // ordinary capture both work this way on every Windows 10/11.
            const DWORD flags = mMode == EVERYTHING ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
            if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, BUFFER_HNS, 0, fmt, NULL)))
            {
                mStartError = "Could not start the sound device.";
                return false;
            }
            return true;
        }

        void run(HANDLE ready)
        {
            ComScope com;
            HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
            ComPtr<IAudioClient> client;
            ComPtr<IAudioCaptureClient> cap;
            WAVEFORMATEX own_fmt;
            WAVEFORMATEX* fmt = nullptr;
            if (!openClient(client, fmt, own_fmt, event) ||
                FAILED(client->GetService(IID_PPV_ARGS(&cap))) || FAILED(client->Start()))
            {
                if (mStartError.empty()) mStartError = "Could not start capturing.";
                SetEvent(ready);
                if (mMixFormat) CoTaskMemFree(mMixFormat);
                CloseHandle(event);
                return;
            }

            // Sample layout: float32, or integer PCM 16/32-bit.
            bool is_float = fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
            if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= 22)
            {
                const WAVEFORMATEXTENSIBLE* ext = (const WAVEFORMATEXTENSIBLE*)fmt;
                is_float = ext->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT;   // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
            }
            const unsigned channels = fmt->nChannels;
            const unsigned rate = fmt->nSamplesPerSec;
            const unsigned bits = fmt->wBitsPerSample;
            std::vector<float> conv;

            mStarted = true;
            SetEvent(ready);
            while (!mStop)
            {
                if (mMode == PROCESS) WaitForSingleObject(event, 100);
                else Sleep(10);
                UINT32 packet = 0;
                HRESULT hr = cap->GetNextPacketSize(&packet);
                while (SUCCEEDED(hr) && packet > 0)
                {
                    BYTE* data = nullptr;
                    UINT32 frames = 0;
                    DWORD flags = 0;
                    hr = cap->GetBuffer(&data, &frames, &flags, NULL, NULL);
                    if (FAILED(hr)) break;
                    conv.assign((size_t)frames * channels, 0.f);
                    if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data)
                    {
                        for (size_t i = 0; i < conv.size(); ++i)
                        {
                            if (is_float && bits == 32) conv[i] = ((const float*)data)[i];
                            else if (bits == 16) conv[i] = ((const int16_t*)data)[i] / 32768.f;
                            else if (bits == 32) conv[i] = ((const int32_t*)data)[i] / 2147483648.f;
                        }
                    }
                    mChannel->pushFloat(conv.data(), frames, channels, rate);
                    cap->ReleaseBuffer(frames);
                    hr = cap->GetNextPacketSize(&packet);
                }
                if (FAILED(hr))
                {
                    mFailed = true;     // e.g. AUDCLNT_E_DEVICE_INVALIDATED: unplugged, program gone
                    break;
                }
            }
            client->Stop();
            if (mMixFormat) CoTaskMemFree(mMixFormat);
            CloseHandle(event);
        }

        WolfDJChannel* mChannel;
        EMode mMode;
        DWORD mPid;
        std::thread mThread;
        std::atomic<bool> mStop{ false };
        std::atomic<bool> mFailed{ false };
        std::atomic<bool> mStarted{ false };
        std::string mStartError;
        WAVEFORMATEX* mMixFormat = nullptr;
    };
}

bool WolfDJCapture::listApps(std::vector<WolfDJApp>& apps, std::string& why)
{
    apps.clear();
    apps.push_back({ WOLFDJ_EVERYTHING_ID, "Everything you hear" });
    if (windows_build() < MIN_PROCESS_LOOPBACK_BUILD)
    {
        why = "Picking one program needs Windows 11 (Windows 10 build 20348 or later). \"Everything you hear\" works here.";
        return true;
    }
    ComScope com;
    ComPtr<IMMDeviceEnumerator> en;
    ComPtr<IMMDevice> dev;
    ComPtr<IAudioSessionManager2> mgr;
    ComPtr<IAudioSessionEnumerator> sessions;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, IID_PPV_ARGS(&en))) ||
        FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) ||
        FAILED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, NULL, (void**)mgr.GetAddressOf())) ||
        FAILED(mgr->GetSessionEnumerator(&sessions)))
    {
        why = "Could not list the programs playing sound.";
        return true;
    }
    int count = 0;
    sessions->GetCount(&count);
    const DWORD self = GetCurrentProcessId();
    std::vector<DWORD> seen;
    for (int i = 0; i < count; ++i)
    {
        ComPtr<IAudioSessionControl> ctl;
        ComPtr<IAudioSessionControl2> ctl2;
        if (FAILED(sessions->GetSession(i, &ctl)) || FAILED(ctl.As(&ctl2))) continue;
        if (ctl2->IsSystemSoundsSession() == S_OK) continue;
        AudioSessionState state = AudioSessionStateExpired;
        ctl2->GetState(&state);
        if (state == AudioSessionStateExpired) continue;
        DWORD pid = 0;
        if (FAILED(ctl2->GetProcessId(&pid)) || pid == 0 || pid == self) continue;
        if (std::find(seen.begin(), seen.end(), pid) != seen.end()) continue;
        seen.push_back(pid);
        apps.push_back({ "pid:" + std::to_string(pid), process_name(pid) });
    }
    return true;
}

std::unique_ptr<WolfDJCaptureStream> WolfDJCapture::openMic(WolfDJChannel* ch, std::string& err)
{
    auto s = std::make_unique<WasapiStream>(ch, WasapiStream::MIC, 0);
    if (!s->start(err)) return nullptr;
    return s;
}

std::unique_ptr<WolfDJCaptureStream> WolfDJCapture::openApp(const std::string& id, WolfDJChannel* ch, std::string& err)
{
    if (id == WOLFDJ_EVERYTHING_ID)
    {
        auto s = std::make_unique<WasapiStream>(ch, WasapiStream::EVERYTHING, 0);
        if (!s->start(err)) return nullptr;
        return s;
    }
    unsigned long pid = 0;
    if (sscanf(id.c_str(), "pid:%lu", &pid) != 1 || pid == 0)
    {
        err = "Unknown program.";
        return nullptr;
    }
    if (windows_build() < MIN_PROCESS_LOOPBACK_BUILD)
    {
        err = "Picking one program needs Windows 11 (Windows 10 build 20348 or later).";
        return nullptr;
    }
    auto s = std::make_unique<WasapiStream>(ch, WasapiStream::PROCESS, (DWORD)pid);
    if (!s->start(err)) return nullptr;
    return s;
}
