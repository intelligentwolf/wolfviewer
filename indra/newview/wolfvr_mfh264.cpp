/**
 * @file wolfvr_mfh264.cpp
 * @brief [W111 VIDEO 2026-11-24] Windows' own H.264 encoders (Media Foundation transforms) as a wolfvr::Encoder:
 *        the GPU's hardware encoder MFT (asynchronous) or Microsoft's software H.264 encoder MFT. Windows only.
 *        See wolfvr_osh264.h.
 *
 * Microsoft documentation followed (every call below names its page):
 *   [H264]  https://learn.microsoft.com/en-us/windows/win32/medfound/h-264-video-encoder
 *   [ASYNC] https://learn.microsoft.com/en-us/windows/win32/medfound/asynchronous-mfts
 *   [ENUM]  https://learn.microsoft.com/en-us/windows/win32/api/mfapi/nf-mfapi-mftenumex
 *   [START] https://learn.microsoft.com/en-us/windows/win32/api/mfapi/nf-mfapi-mfstartup
 *   [COM]   https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-coinitializeex
 *   [PIN]   https://learn.microsoft.com/en-us/windows/win32/api/mftransform/nf-mftransform-imftransform-processinput
 *   [POUT]  https://learn.microsoft.com/en-us/windows/win32/api/mftransform/nf-mftransform-imftransform-processoutput
 *   [EVT]   https://learn.microsoft.com/en-us/windows/win32/api/mfobjects/nf-mfobjects-imfmediaeventgenerator-getevent
 *   [CHG]   https://learn.microsoft.com/en-us/windows/win32/medfound/handling-stream-changes
 *   [CAPI]  https://learn.microsoft.com/en-us/windows/win32/api/strmif/nf-strmif-icodecapi-setvalue
 *   [KEY]   https://learn.microsoft.com/en-us/windows/win32/medfound/codecapi-avencvideoforcekeyframe
 *   [LOWL]  https://learn.microsoft.com/en-us/windows/win32/medfound/codecapi-avlowlatencymode
 *   [EXT]   https://learn.microsoft.com/en-us/windows/win32/medfound/extended-color-information
 * FFmpeg's libavcodec/mfenc.c (LGPL) drives the same MFTs the same way and was read as a cross-check.
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

#include "wolfvr_osh264.h"
#include "wolfvr_yuv.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mftransform.h>
#include <codecapi.h>
#include <strmif.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace wolfvr
{
namespace
{
    std::string hrText(const char* what, HRESULT hr)
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s failed (0x%08lX)", what, (unsigned long)hr);
        return buf;
    }

    std::string wideToUtf8(const WCHAR* w)
    {
        if (!w) return std::string();
        const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        if (n <= 1) return std::string();
        std::string s((size_t)n - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
        return s;
    }

    // [CAPI] ICodecAPI::SetValue takes a VARIANT; VT_UI4 for the numeric properties, VT_BOOL for the boolean ones
    // (each property page names its type). Filled by hand: no oleaut32 needed for VariantInit.
    HRESULT setCodecUI4(ICodecAPI* api, const GUID& prop, ULONG value)
    {
        VARIANT v;
        std::memset(&v, 0, sizeof v);
        v.vt = VT_UI4;
        v.ulVal = value;
        return api->SetValue(&prop, &v);
    }
    HRESULT setCodecBool(ICodecAPI* api, const GUID& prop, bool value)
    {
        VARIANT v;
        std::memset(&v, 0, sizeof v);
        v.vt = VT_BOOL;
        v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
        return api->SetValue(&prop, &v);
    }

    // The colours the picture is converted with (wolfvr_yuv.h: BT.709, limited range) as Media Foundation names
    // them [EXT]: MF_MT_VIDEO_PRIMARIES, MF_MT_TRANSFER_FUNCTION, MF_MT_YUV_MATRIX, MF_MT_VIDEO_NOMINAL_RANGE.
    void setColours(IMFMediaType* t)
    {
        t->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
        t->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
        t->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        t->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    }

    bool startsWithStartCode(const std::vector<uint8_t>& b)
    {
        return (b.size() >= 4 && b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 1) ||
               (b.size() >= 3 && b[0] == 0 && b[1] == 0 && b[2] == 1);
    }

    bool hasNalType(const std::vector<uint8_t>& b, int type)
    {
        for (size_t i = 0; i + 3 < b.size(); ++i)
        {
            if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1 && (b[i + 3] & 0x1F) == type) return true;
        }
        return false;
    }

    class MfH264Encoder : public Encoder
    {
    public:
        ~MfH264Encoder() override
        {
            if (mShutdown) mShutdown->Shutdown();   // [ASYNC] "Shutting Down the MFT": asynchronous MFTs must be shut down
            mEvents.Reset();
            mShutdown.Reset();
            mCodec.Reset();
            mMft.Reset();
            if (mMfStarted) MFShutdown();           // [START] one MFShutdown per successful MFStartup
            if (mComStarted) CoUninitialize();      // [COM] one CoUninitialize per successful CoInitializeEx
        }

        bool open(uint32_t w, uint32_t h, uint32_t fps, uint32_t kbps, bool hardware, std::string& error)
        {
            mW = w; mH = h; mFps = std::max<uint32_t>(1, fps); mHardware = hardware;
            const char* kind = hardware ? "Media Foundation H.264 (hardware)" : "Media Foundation H.264 (software)";
            mName = kind;
            if (w < 16 || h < 16 || (w & 1) || (h & 1))
            {
                error = std::string(kind) + ": the picture size must be even and at least 16x16";
                return false;
            }
            // [COM] MFTs are COM objects; the multithreaded apartment, as FFmpeg's mfenc does (the hardware MFT's
            // events come from its own threads). S_FALSE = already initialised on this thread (still counted).
            HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (FAILED(hr))
            {
                error = std::string(kind) + ": " + hrText("CoInitializeEx", hr) +
                        (hr == RPC_E_CHANGED_MODE ? " (this thread is a single-threaded apartment)" : "");
                return false;
            }
            mComStarted = true;
            // [START] MFStartup(MF_VERSION, MFSTARTUP_LITE): no sockets needed for transforms. Fails on Windows N
            // without the Media Feature Pack, then the caller falls back to OpenH264.
            hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
            if (FAILED(hr)) { error = std::string(kind) + ": " + hrText("MFStartup", hr); return false; }
            mMfStarted = true;

            // [ENUM] MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, NV12 in, H.264 out). MFT_ENUM_FLAG_HARDWARE lists
            // hardware MFTs (always asynchronous [ASYNC]); MFT_ENUM_FLAG_SYNCMFT the synchronous software ones;
            // MFT_ENUM_FLAG_SORTANDFILTER orders them by merit. The array and every IMFActivate are the caller's
            // to release (CoTaskMemFree).
            MFT_REGISTER_TYPE_INFO in_info = {MFMediaType_Video, MFVideoFormat_NV12};
            MFT_REGISTER_TYPE_INFO out_info = {MFMediaType_Video, MFVideoFormat_H264};
            const UINT32 flags = MFT_ENUM_FLAG_SORTANDFILTER | (hardware ? MFT_ENUM_FLAG_HARDWARE : MFT_ENUM_FLAG_SYNCMFT);
            IMFActivate** acts = nullptr;
            UINT32 count = 0;
            hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, &in_info, &out_info, &acts, &count);
            if (FAILED(hr)) { error = std::string(kind) + ": " + hrText("MFTEnumEx", hr); return false; }
            std::string why;
            for (UINT32 i = 0; i < count && !mMft; ++i)
            {
                // IMFActivate::ActivateObject creates the MFT
                // (https://learn.microsoft.com/en-us/windows/win32/api/mfobjects/nf-mfobjects-imfactivate-activateobject)
                ComPtr<IMFTransform> mft;
                hr = acts[i]->ActivateObject(IID_PPV_ARGS(&mft));
                if (FAILED(hr)) { why += hrText("ActivateObject", hr) + " "; continue; }
                // MFT_FRIENDLY_NAME_Attribute on the activation object: the encoder's name, for the record
                WCHAR* fname = nullptr;
                UINT32 flen = 0;
                if (SUCCEEDED(acts[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &fname, &flen)) && fname)
                {
                    mName = std::string(kind) + " - " + wideToUtf8(fname);
                    CoTaskMemFree(fname);
                }
                std::string e;
                if (configure(mft.Get(), kbps, e))
                {
                    mMft = mft;
                }
                else
                {
                    why += e + " ";
                    ComPtr<IMFShutdown> sd;
                    if (SUCCEEDED(mft.As(&sd))) sd->Shutdown();
                    mEvents.Reset();
                    mShutdown.Reset();
                    mCodec.Reset();
                }
            }
            for (UINT32 i = 0; i < count; ++i) acts[i]->Release();
            CoTaskMemFree(acts);
            if (!mMft)
            {
                error = std::string(kind) + ": " + (count == 0 ? std::string("none on this computer") : why);
                return false;
            }
            mFrameBytes = (DWORD)((size_t)w * h * 3 / 2);   // NV12: a full-size Y plane + a half-size CbCr plane
            return true;
        }

        bool encode(const uint8_t* rgba, EncodedFrame& out, std::string& error) override
        {
            // input sample: one contiguous NV12 picture (Y rows then interleaved CbCr rows, stride = width)
            ComPtr<IMFMediaBuffer> buf;
            const DWORD size = mFrameBytes;
            // MFCreateMemoryBuffer https://learn.microsoft.com/en-us/windows/win32/api/mfapi/nf-mfapi-mfcreatememorybuffer
            HRESULT hr = MFCreateMemoryBuffer(size, &buf);
            if (FAILED(hr)) { error = mName + ": " + hrText("MFCreateMemoryBuffer", hr); return false; }
            BYTE* p = nullptr;
            // IMFMediaBuffer::Lock / Unlock / SetCurrentLength
            // https://learn.microsoft.com/en-us/windows/win32/api/mfobjects/nf-mfobjects-imfmediabuffer-lock
            hr = buf->Lock(&p, nullptr, nullptr);
            if (FAILED(hr) || !p) { error = mName + ": " + hrText("IMFMediaBuffer::Lock", hr); return false; }
            const uint32_t threads = std::clamp(std::thread::hardware_concurrency(), 1u, 4u);
            rgbaToYuv420(rgba, mW, mH, p, mW, p + (size_t)mW * mH, nullptr, mW, true, threads);
            buf->Unlock();
            hr = buf->SetCurrentLength(size);
            if (FAILED(hr)) { error = mName + ": " + hrText("SetCurrentLength", hr); return false; }
            ComPtr<IMFSample> sample;
            // MFCreateSample https://learn.microsoft.com/en-us/windows/win32/api/mfapi/nf-mfapi-mfcreatesample
            hr = MFCreateSample(&sample);
            if (SUCCEEDED(hr)) hr = sample->AddBuffer(buf.Get());
            // times in 100-ns units (IMFSample::SetSampleTime / SetSampleDuration); a steady clock at the rate the
            // encoder was opened with, as its rate control expects (the recorder keeps the real times itself)
            const LONGLONG t = (LONGLONG)(mFrame * 10000000ULL / mFps);
            const LONGLONG t1 = (LONGLONG)((mFrame + 1) * 10000000ULL / mFps);
            if (SUCCEEDED(hr)) hr = sample->SetSampleTime(t);
            if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(t1 - t);
            if (FAILED(hr)) { error = mName + ": " + hrText("MFCreateSample", hr); return false; }
            ++mFrame;

            const DWORD timeout_ms = mFirst ? 3000 : 1000;   // the first picture includes the encoder's start-up
            mFirst = false;
            if (mEvents)
            {
                // [ASYNC] "For each METransformNeedInput event, the client calls ProcessInput"
                if (!waitFor(true, timeout_ms, error)) return false;
                --mNeedInput;
            }
            hr = mMft->ProcessInput(0, sample.Get(), 0);   // [PIN]
            if (FAILED(hr)) { error = mName + ": " + hrText("ProcessInput", hr); return false; }

            // one access unit out for this picture
            for (;;)
            {
                if (mEvents && !waitFor(false, timeout_ms, error)) return false;
                bool again = false;
                if (!pullOutput(out, again, error)) return false;
                if (again) continue;   // a stream change was handled: the output comes with the next event
                return true;
            }
        }

        bool requestKeyFrame() override
        {
            // [KEY] CODECAPI_AVEncVideoForceKeyFrame (VT_UI4): "the next frame is encoded as a key frame"
            return mCodec && SUCCEEDED(setCodecUI4(mCodec.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1));
        }

        bool setBitrate(uint32_t kbps) override
        {
            // [H264] CODECAPI_AVEncCommonMeanBitRate: "In Windows 8, you can set the average bit rate during encoding"
            return mCodec && kbps &&
                   SUCCEEDED(setCodecUI4(mCodec.Get(), CODECAPI_AVEncCommonMeanBitRate, kbps * 1000));
        }

        std::string name() const override { return mName; }
        std::string codec() const override { return "h264"; }

    private:
        bool configure(IMFTransform* mft, uint32_t kbps, std::string& error)
        {
            HRESULT hr = S_OK;
            if (mHardware)
            {
                // [ASYNC] "Unlocking Asynchronous MFTs": MF_TRANSFORM_ASYNC must be TRUE, then set
                // MF_TRANSFORM_ASYNC_UNLOCK; events come through IMFMediaEventGenerator; IMFShutdown is mandatory.
                ComPtr<IMFAttributes> attrs;
                hr = mft->GetAttributes(&attrs);
                UINT32 async = 0;
                if (FAILED(hr) || !attrs || FAILED(attrs->GetUINT32(MF_TRANSFORM_ASYNC, &async)) || !async)
                {
                    error = "the hardware encoder is not asynchronous";
                    return false;
                }
                hr = attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
                if (FAILED(hr)) { error = hrText("MF_TRANSFORM_ASYNC_UNLOCK", hr); return false; }
                hr = mft->QueryInterface(IID_PPV_ARGS(&mEvents));
                if (FAILED(hr)) { error = hrText("IMFMediaEventGenerator", hr); return false; }
                hr = mft->QueryInterface(IID_PPV_ARGS(&mShutdown));
                if (FAILED(hr)) { error = hrText("IMFShutdown", hr); return false; }
            }
            // [H264] the encoder exposes ICodecAPI; the B-frame count "must be set before SetOutputType".
            // Failures of the optional properties are not fatal (a certified hardware encoder need not support
            // CODECAPI_AVEncMPVDefaultBPictureCount or CODECAPI_AVLowLatencyMode [H264] "Certified Hardware
            // Encoder"); the probe in openEncoder (one picture in, an SPS out) decides.
            if (SUCCEEDED(mft->QueryInterface(IID_PPV_ARGS(&mCodec))))
            {
                // [LOWL] low-latency mode: one picture in, one out (slice threading, no frame queue)
                setCodecBool(mCodec.Get(), CODECAPI_AVLowLatencyMode, true);
                // [H264] CODECAPI_AVEncMPVDefaultBPictureCount 0 = "Do not use B frames"
                const bool no_b = SUCCEEDED(setCodecUI4(mCodec.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0));
                mProfile = no_b ? eAVEncH264VProfile_Main : eAVEncH264VProfile_Base;   // Baseline has no B-frames
                // [H264] CBR (Windows 8+) at the mean bit rate; GOP 2 s, as the other encoders here
                setCodecUI4(mCodec.Get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
                setCodecUI4(mCodec.Get(), CODECAPI_AVEncCommonMeanBitRate, kbps * 1000);
                setCodecUI4(mCodec.Get(), CODECAPI_AVEncMPVGOPSize, mFps * 2);
            }
            else
            {
                mProfile = eAVEncH264VProfile_Base;
            }

            // [H264] "The output type must be set before the input type": MF_MT_MAJOR_TYPE Video, MF_MT_SUBTYPE
            // H264, MF_MT_AVG_BITRATE (> 0), MF_MT_FRAME_RATE, MF_MT_FRAME_SIZE, MF_MT_INTERLACE_MODE,
            // MF_MT_MPEG2_PROFILE; MF_MT_PIXEL_ASPECT_RATIO optional (1:1); the level left to the encoder.
            ComPtr<IMFMediaType> ot;
            hr = MFCreateMediaType(&ot);
            if (FAILED(hr)) { error = hrText("MFCreateMediaType", hr); return false; }
            ot->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            ot->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
            ot->SetUINT32(MF_MT_AVG_BITRATE, std::max<uint32_t>(1, kbps) * 1000);
            // MFSetAttributeRatio / MFSetAttributeSize (mfapi.h inline helpers):
            // https://learn.microsoft.com/en-us/windows/win32/api/mfapi/nf-mfapi-mfsetattributesize
            MFSetAttributeRatio(ot.Get(), MF_MT_FRAME_RATE, mFps, 1);
            MFSetAttributeSize(ot.Get(), MF_MT_FRAME_SIZE, mW, mH);
            ot->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            ot->SetUINT32(MF_MT_MPEG2_PROFILE, (UINT32)mProfile);
            MFSetAttributeRatio(ot.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            setColours(ot.Get());
            hr = mft->SetOutputType(0, ot.Get(), 0);
            if (FAILED(hr) && mProfile != eAVEncH264VProfile_Base)
            {
                mProfile = eAVEncH264VProfile_Base;
                ot->SetUINT32(MF_MT_MPEG2_PROFILE, (UINT32)mProfile);
                hr = mft->SetOutputType(0, ot.Get(), 0);
            }
            if (FAILED(hr)) { error = hrText("SetOutputType (H.264)", hr); return false; }
            readSequenceHeader(mft);

            // Input: the encoder's own NV12 type when it lists one (GetInputAvailableType), else a new one
            ComPtr<IMFMediaType> it;
            for (DWORD i = 0;; ++i)
            {
                ComPtr<IMFMediaType> cand;
                if (FAILED(mft->GetInputAvailableType(0, i, &cand))) break;
                GUID sub = GUID_NULL;
                if (SUCCEEDED(cand->GetGUID(MF_MT_SUBTYPE, &sub)) && sub == MFVideoFormat_NV12) { it = cand; break; }
            }
            if (!it)
            {
                hr = MFCreateMediaType(&it);
                if (FAILED(hr)) { error = hrText("MFCreateMediaType", hr); return false; }
                it->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                it->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
            }
            MFSetAttributeRatio(it.Get(), MF_MT_FRAME_RATE, mFps, 1);
            MFSetAttributeSize(it.Get(), MF_MT_FRAME_SIZE, mW, mH);
            it->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            MFSetAttributeRatio(it.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            it->SetUINT32(MF_MT_DEFAULT_STRIDE, mW);   // positive stride = top row first
            setColours(it.Get());
            hr = mft->SetInputType(0, it.Get(), 0);
            if (FAILED(hr)) { error = hrText("SetInputType (NV12)", hr); return false; }

            // IMFTransform::GetOutputStreamInfo: whether the MFT allocates the output samples, and the size if not
            // https://learn.microsoft.com/en-us/windows/win32/api/mftransform/nf-mftransform-imftransform-getoutputstreaminfo
            hr = mft->GetOutputStreamInfo(0, &mOutInfo);
            if (FAILED(hr)) { error = hrText("GetOutputStreamInfo", hr); return false; }

            // MFT_MESSAGE_NOTIFY_BEGIN_STREAMING then MFT_MESSAGE_NOTIFY_START_OF_STREAM ([ASYNC] "When streaming
            // starts, the client sends the MFT_MESSAGE_NOTIFY_START_OF_STREAM message"; an asynchronous MFT sends no
            // METransformNeedInput before it)
            // https://learn.microsoft.com/en-us/windows/win32/api/mftransform/nf-mftransform-imftransform-processmessage
            hr = mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
            if (SUCCEEDED(hr)) hr = mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
            if (FAILED(hr)) { error = hrText("ProcessMessage(START_OF_STREAM)", hr); return false; }
            return true;
        }

        void readSequenceHeader(IMFTransform* mft)
        {
            // [H264] "After the output type is set, the video encoder updates the type by adding the
            // MF_MT_MPEG_SEQUENCE_HEADER attribute" (SPS/PPS, start-code delimited)
            mSeqHeader.clear();
            ComPtr<IMFMediaType> cur;
            if (FAILED(mft->GetOutputCurrentType(0, &cur))) return;
            UINT32 n = 0;
            if (FAILED(cur->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &n)) || n == 0) return;
            mSeqHeader.resize(n);
            if (FAILED(cur->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, mSeqHeader.data(), n, nullptr))) mSeqHeader.clear();
        }

        // [ASYNC] waits for METransformNeedInput (need_input) or METransformHaveOutput, counting the other kind.
        // [EVT] GetEvent with MF_EVENT_FLAG_NO_WAIT returns MF_E_NO_EVENTS_AVAILABLE when the queue is empty: polled
        // every millisecond so a stalled driver cannot hang the recorder's thread.
        bool waitFor(bool need_input, DWORD timeout_ms, std::string& error)
        {
            const ULONGLONG deadline = GetTickCount64() + timeout_ms;
            for (;;)
            {
                if (need_input ? mNeedInput > 0 : mHaveOutput > 0) return true;
                ComPtr<IMFMediaEvent> ev;
                const HRESULT hr = mEvents->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev);
                if (hr == MF_E_NO_EVENTS_AVAILABLE)
                {
                    if (GetTickCount64() >= deadline)
                    {
                        error = mName + (need_input ? ": the encoder asked for no picture" : ": no output") +
                                " within " + std::to_string(timeout_ms) + " ms";
                        return false;
                    }
                    Sleep(1);
                    continue;
                }
                if (FAILED(hr)) { error = mName + ": " + hrText("GetEvent", hr); return false; }
                MediaEventType type = MEUnknown;
                ev->GetType(&type);   // IMFMediaEvent::GetType
                HRESULT status = S_OK;
                ev->GetStatus(&status);
                if (FAILED(status)) { error = mName + ": " + hrText("encoder event", status); return false; }
                if (type == METransformNeedInput) ++mNeedInput;
                else if (type == METransformHaveOutput) ++mHaveOutput;
                else if (type == MEError) { error = mName + ": the encoder reported an error"; return false; }
            }
        }

        bool pullOutput(EncodedFrame& out, bool& again, std::string& error)
        {
            again = false;
            MFT_OUTPUT_DATA_BUFFER db;
            std::memset(&db, 0, sizeof db);
            db.dwStreamID = 0;
            ComPtr<IMFSample> mine;
            const bool provides = (mOutInfo.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                                       MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
            if (!provides)
            {
                // [POUT] when the MFT does not provide samples the caller allocates one of at least cbSize bytes
                ComPtr<IMFMediaBuffer> ob;
                const DWORD cb = std::max<DWORD>(mOutInfo.cbSize, (DWORD)(mW * mH * 3 / 2));
                HRESULT hr = MFCreateMemoryBuffer(cb, &ob);
                if (SUCCEEDED(hr)) hr = MFCreateSample(&mine);
                if (SUCCEEDED(hr)) hr = mine->AddBuffer(ob.Get());
                if (FAILED(hr)) { error = mName + ": " + hrText("output sample", hr); return false; }
                db.pSample = mine.Get();
            }
            DWORD status = 0;
            const HRESULT hr = mMft->ProcessOutput(0, 1, &db, &status);   // [POUT]
            if (db.pEvents) db.pEvents->Release();   // [POUT] "the caller must release the collection"
            ComPtr<IMFSample> got;
            if (db.pSample)
            {
                if (provides) got.Attach(db.pSample);   // the MFT's sample: our reference to release
                else got = mine;
            }
            if (mEvents && mHaveOutput > 0) --mHaveOutput;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE)
            {
                // [CHG] the encoder wants a new output type: take its first available one, read the new sequence
                // header, and get the picture with the next output
                ComPtr<IMFMediaType> t;
                HRESULT h2 = mMft->GetOutputAvailableType(0, 0, &t);
                if (SUCCEEDED(h2)) h2 = mMft->SetOutputType(0, t.Get(), 0);
                if (FAILED(h2)) { error = mName + ": " + hrText("output type change", h2); return false; }
                readSequenceHeader(mMft.Get());
                mMft->GetOutputStreamInfo(0, &mOutInfo);
                again = true;
                return true;
            }
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
            {
                // a synchronous encoder that holds pictures back: not one in, one out
                error = mName + ": the encoder held the picture back (needs more input)";
                return false;
            }
            if (FAILED(hr) || !got) { error = mName + ": " + hrText("ProcessOutput", hr); return false; }

            // IMFSample::ConvertToContiguousBuffer + Lock: the access unit's bytes
            ComPtr<IMFMediaBuffer> cbuf;
            HRESULT h3 = got->ConvertToContiguousBuffer(&cbuf);
            BYTE* p = nullptr;
            DWORD len = 0;
            if (SUCCEEDED(h3)) h3 = cbuf->Lock(&p, nullptr, &len);
            if (FAILED(h3) || !p) { error = mName + ": " + hrText("output buffer", h3); return false; }
            out.data.assign(p, p + len);
            cbuf->Unlock();
            // A reordering encoder marks decode times that differ from presentation times
            // (MFSampleExtension_DecodeTimestamp, [H264] "In Windows 8, the encoder sets ... on the output samples").
            UINT64 dts = 0;
            LONGLONG pts = 0;
            if (SUCCEEDED(got->GetUINT64(MFSampleExtension_DecodeTimestamp, &dts)) &&
                SUCCEEDED(got->GetSampleTime(&pts)) && (LONGLONG)dts != pts)
            {
                error = mName + ": the encoder reorders pictures (B-frames)";
                return false;
            }
            if (!startsWithStartCode(out.data))
            {
                error = mName + ": the output is not an H.264 byte stream (Annex B)";
                return false;
            }
            out.key = avcHasIdr(out.data);
            if (out.key && !hasNalType(out.data, 7))
            {
                // the parameter sets live only in the output type: put them in front of the IDR (start codes
                // included), so every key frame can start a stream as with the other encoders
                if (mSeqHeader.empty() || !startsWithStartCode(mSeqHeader))
                {
                    error = mName + ": an IDR came without SPS/PPS and the encoder gave no sequence header";
                    return false;
                }
                out.data.insert(out.data.begin(), mSeqHeader.begin(), mSeqHeader.end());
            }
            return true;
        }

        ComPtr<IMFTransform> mMft;
        ComPtr<IMFMediaEventGenerator> mEvents;   // hardware (asynchronous) MFTs only
        ComPtr<IMFShutdown> mShutdown;
        ComPtr<ICodecAPI> mCodec;
        MFT_OUTPUT_STREAM_INFO mOutInfo = {};
        std::vector<uint8_t> mSeqHeader;
        DWORD mFrameBytes = 0;
        std::string mName;
        uint32_t mW = 0, mH = 0, mFps = 30;
        eAVEncH264VProfile mProfile = eAVEncH264VProfile_Base;
        uint64_t mFrame = 0;
        int mNeedInput = 0, mHaveOutput = 0;
        bool mHardware = false, mFirst = true, mComStarted = false, mMfStarted = false;
    };
}

std::unique_ptr<Encoder> openMediaFoundationH264(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate_kbps,
                                                 bool hardware, std::string& error)
{
    auto enc = std::make_unique<MfH264Encoder>();
    if (!enc->open(width, height, fps, bitrate_kbps, hardware, error)) return nullptr;
    return enc;
}
}
