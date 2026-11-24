/**
 * @file wolfrec_audioenc_mf.cpp
 * @brief [W111 VIDEO 2026-11-24] The recorder's AAC-LC encoder on Windows: Microsoft's Media Foundation AAC encoder
 *        MFT. Windows only. See wolfrec_audioenc.h.
 *
 * Microsoft documentation followed:
 *   [AACE]  https://learn.microsoft.com/en-us/windows/win32/medfound/aac-encoder
 *   [AACD]  https://learn.microsoft.com/en-us/windows/win32/medfound/aac-decoder
 *   [ENUM]  https://learn.microsoft.com/en-us/windows/win32/api/mfapi/nf-mfapi-mftenumex
 *   [START] https://learn.microsoft.com/en-us/windows/win32/api/mfapi/nf-mfapi-mfstartup
 *   [COM]   https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-coinitializeex
 *   [PIN]   https://learn.microsoft.com/en-us/windows/win32/api/mftransform/nf-mftransform-imftransform-processinput
 *   [POUT]  https://learn.microsoft.com/en-us/windows/win32/api/mftransform/nf-mftransform-imftransform-processoutput
 *   [MSG]   https://learn.microsoft.com/en-us/windows/win32/api/mftransform/nf-mftransform-imftransform-processmessage
 *   [INFO]  https://learn.microsoft.com/en-us/windows/win32/api/mftransform/nf-mftransform-imftransform-getoutputstreaminfo
 *   [HEAAC] https://learn.microsoft.com/en-us/windows/win32/api/mmreg/ns-mmreg-heaacwaveinfo
 *
 * Encoder delay: Microsoft documents none for its AAC encoder, and reports of it disagree (one says it primes 0
 * samples but loses its first ~700, another that it trims its own priming). So it is MEASURED when the encoder opens:
 * a second encoder with the same settings codes a click after the same lead-in the recording gets, Microsoft's AAC
 * decoder decodes it (raw AAC: exactly 1024 samples per packet, which is checked), and the click's place in the
 * decoded sound gives the samples to trim (the MP4 edit list's media_time). A recording also starts with a lead-in of
 * silence (kLeadIn) so a start that the encoder would lose is silence, and the trim is never negative.
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

#include "wolfrec_audioenc.h"

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
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace wolfrec
{
namespace
{
    constexpr uint32_t kAacFrame = 1024;    // [AACE] "Each output sample contains one compressed AAC frame
                                            // corresponding to 1024 PCM samples"
    constexpr uint32_t kLeadIn = 2048;      // silence in front of every recording (see the file comment)
    constexpr uint32_t kHeaacTail = 12;     // [HEAAC]/[AACE] MF_MT_USER_DATA = the HEAACWAVEINFO members after wfx
                                            // (wPayloadType, wAudioProfileLevelIndication, wStructType, wReserved1:
                                            // 4 x WORD, dwReserved2: DWORD = 12 bytes), then AudioSpecificConfig()

    std::string hrText(const char* what, HRESULT hr)
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s failed (0x%08lX)", what, (unsigned long)hr);
        return buf;
    }

    // One synchronous audio MFT (the AAC encoder, or the AAC decoder used to measure the encoder's delay).
    class SyncTransform
    {
    public:
        ComPtr<IMFTransform> mft;
        MFT_OUTPUT_STREAM_INFO info = {};

        // [ENUM] the first synchronous MFT in `category` from in_sub to out_sub that ActivateObject creates
        bool create(const GUID& category, const GUID& in_sub, const GUID& out_sub, std::string& error)
        {
            MFT_REGISTER_TYPE_INFO in_info = {MFMediaType_Audio, in_sub};
            MFT_REGISTER_TYPE_INFO out_info = {MFMediaType_Audio, out_sub};
            IMFActivate** acts = nullptr;
            UINT32 count = 0;
            HRESULT hr = MFTEnumEx(category, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, &in_info, &out_info,
                                   &acts, &count);
            if (FAILED(hr)) { error = hrText("MFTEnumEx", hr); return false; }
            for (UINT32 i = 0; i < count && !mft; ++i)
            {
                ComPtr<IMFTransform> t;
                if (SUCCEEDED(acts[i]->ActivateObject(IID_PPV_ARGS(&t)))) mft = t;
            }
            for (UINT32 i = 0; i < count; ++i) acts[i]->Release();
            CoTaskMemFree(acts);
            if (!mft) { error = count ? "the transform could not be created" : "none on this computer"; return false; }
            return true;
        }

        bool start(std::string& error)
        {
            HRESULT hr = mft->GetOutputStreamInfo(0, &info);   // [INFO]
            if (SUCCEEDED(hr)) hr = mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);   // [MSG]
            if (SUCCEEDED(hr)) hr = mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
            if (FAILED(hr)) { error = hrText("starting the transform", hr); return false; }
            return true;
        }

        // [PIN] one input sample with its time and duration ([AACE] "every input sample must have a valid time and
        // duration"; a zero duration crashes ProcessOutput, so it never is zero here)
        bool input(const void* data, DWORD bytes, LONGLONG time, LONGLONG duration, std::string& error)
        {
            ComPtr<IMFMediaBuffer> buf;
            ComPtr<IMFSample> s;
            HRESULT hr = MFCreateMemoryBuffer(bytes, &buf);
            BYTE* p = nullptr;
            if (SUCCEEDED(hr)) hr = buf->Lock(&p, nullptr, nullptr);
            if (SUCCEEDED(hr))
            {
                std::memcpy(p, data, bytes);
                buf->Unlock();
                hr = buf->SetCurrentLength(bytes);
            }
            if (SUCCEEDED(hr)) hr = MFCreateSample(&s);
            if (SUCCEEDED(hr)) hr = s->AddBuffer(buf.Get());
            if (SUCCEEDED(hr)) hr = s->SetSampleTime(time);
            if (SUCCEEDED(hr)) hr = s->SetSampleDuration(std::max<LONGLONG>(1, duration));
            if (SUCCEEDED(hr)) hr = mft->ProcessInput(0, s.Get(), 0);
            if (FAILED(hr)) { error = hrText("ProcessInput", hr); return false; }
            return true;
        }

        // [POUT] every output sample the transform has now (until MF_E_TRANSFORM_NEED_MORE_INPUT)
        template <typename Fn>
        bool drainOutput(Fn fn, std::string& error)
        {
            for (;;)
            {
                MFT_OUTPUT_DATA_BUFFER db;
                std::memset(&db, 0, sizeof db);
                ComPtr<IMFSample> mine;
                const bool provides = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                                       MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
                if (!provides)
                {
                    ComPtr<IMFMediaBuffer> ob;
                    HRESULT h = MFCreateMemoryBuffer(std::max<DWORD>(info.cbSize, 65536), &ob);
                    if (SUCCEEDED(h)) h = MFCreateSample(&mine);
                    if (SUCCEEDED(h)) h = mine->AddBuffer(ob.Get());
                    if (FAILED(h)) { error = hrText("output sample", h); return false; }
                    db.pSample = mine.Get();
                }
                DWORD status = 0;
                const HRESULT hr = mft->ProcessOutput(0, 1, &db, &status);
                if (db.pEvents) db.pEvents->Release();
                ComPtr<IMFSample> got;
                if (db.pSample) { if (provides) got.Attach(db.pSample); else got = mine; }
                if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
                if (FAILED(hr) || !got) { error = hrText("ProcessOutput", hr); return false; }
                ComPtr<IMFMediaBuffer> cb;
                HRESULT h = got->ConvertToContiguousBuffer(&cb);
                BYTE* p = nullptr;
                DWORD len = 0;
                if (SUCCEEDED(h)) h = cb->Lock(&p, nullptr, &len);
                if (FAILED(h) || !p) { error = hrText("output buffer", h); return false; }
                fn(p, len);
                cb->Unlock();
            }
        }
    };

    HRESULT pcmType(IMFMediaType** out, const GUID& sub, uint32_t bits, uint32_t rate, uint32_t ch)
    {
        // [AACE] input: MFAudioFormat_PCM, 16 bits, 44100/48000, 1/2/6 channels; block alignment and bytes/s as WAVE
        // [AACD] output: MFAudioFormat_Float or MFAudioFormat_PCM
        ComPtr<IMFMediaType> t;
        HRESULT hr = MFCreateMediaType(&t);
        if (FAILED(hr)) return hr;
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        t->SetGUID(MF_MT_SUBTYPE, sub);
        t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, bits);
        t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
        t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, ch);
        t->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, ch * bits / 8);
        t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * ch * bits / 8);
        *out = t.Detach();
        return S_OK;
    }

    // [AACE] MF_MT_AUDIO_AVG_BYTES_PER_SECOND: 12000, 16000, 20000 or 24000 (96..192 kbit/s) for mono and stereo
    uint32_t aacBytesPerSecond(uint32_t bitrate_bps)
    {
        const uint32_t allowed[] = {12000, 16000, 20000, 24000};
        uint32_t best = allowed[0];
        for (uint32_t a : allowed)
        {
            if (a * 8 <= std::max<uint32_t>(bitrate_bps, 96000)) best = a;
        }
        return best;
    }

    // Creates and configures one AAC encoder MFT; `user_data` gets its MF_MT_USER_DATA.
    bool makeEncoder(SyncTransform& t, uint32_t rate, uint32_t ch, uint32_t bytes_per_second,
                     std::vector<uint8_t>& user_data, std::string& error)
    {
        if (!t.create(MFT_CATEGORY_AUDIO_ENCODER, MFAudioFormat_PCM, MFAudioFormat_AAC, error)) return false;
        // [AACE] "You can set the types in either order"
        ComPtr<IMFMediaType> in;
        HRESULT hr = pcmType(&in, MFAudioFormat_PCM, 16, rate, ch);
        if (SUCCEEDED(hr)) hr = t.mft->SetInputType(0, in.Get(), 0);
        if (FAILED(hr)) { error = hrText("SetInputType (PCM)", hr); return false; }
        // [AACE] output: MFAudioFormat_AAC, 16 bits, the input's rate and channels, the bit rate in bytes/s,
        // MF_MT_AAC_PAYLOAD_TYPE 0 (raw_data_block only), MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION 0x29 (AAC L2)
        ComPtr<IMFMediaType> out;
        hr = MFCreateMediaType(&out);
        if (FAILED(hr)) { error = hrText("MFCreateMediaType", hr); return false; }
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
        out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, ch);
        out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bytes_per_second);
        out->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
        out->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
        hr = t.mft->SetOutputType(0, out.Get(), 0);
        if (FAILED(hr)) { error = hrText("SetOutputType (AAC)", hr); return false; }
        // [AACE] "After the output type is set, the AAC encoder updates the type by adding the MF_MT_USER_DATA"
        ComPtr<IMFMediaType> cur;
        UINT32 n = 0;
        hr = t.mft->GetOutputCurrentType(0, &cur);
        if (SUCCEEDED(hr)) hr = cur->GetBlobSize(MF_MT_USER_DATA, &n);
        if (FAILED(hr) || n <= kHeaacTail) { error = hrText("MF_MT_USER_DATA", FAILED(hr) ? hr : E_FAIL); return false; }
        user_data.resize(n);
        hr = cur->GetBlob(MF_MT_USER_DATA, user_data.data(), n, nullptr);
        if (FAILED(hr)) { error = hrText("MF_MT_USER_DATA", hr); return false; }
        return t.start(error);
    }

    void toInt16(const float* in, size_t n, std::vector<int16_t>& out)
    {
        out.resize(n);
        for (size_t i = 0; i < n; ++i)
        {
            const float v = std::clamp(in[i], -1.f, 1.f);
            out[i] = (int16_t)std::lrint(v * 32767.f);
        }
    }

    class MfAacEncoder : public AudioEncoder
    {
    public:
        ~MfAacEncoder() override
        {
            mEnc.mft.Reset();
            if (mMfStarted) MFShutdown();
            if (mComStarted) CoUninitialize();
        }

        bool open(uint32_t rate, uint32_t ch, uint32_t bitrate, std::string& error)
        {
            if ((rate != 44100 && rate != 48000) || (ch != 1 && ch != 2))
            {
                error = "Media Foundation AAC: needs 44100/48000 Hz mono or stereo";   // [AACE] input types
                return false;
            }
            mRate = rate; mCh = ch;
            HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // [COM]
            if (FAILED(hr)) { error = "Media Foundation AAC: " + hrText("CoInitializeEx", hr); return false; }
            mComStarted = true;
            hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);   // [START]
            if (FAILED(hr)) { error = "Media Foundation AAC: " + hrText("MFStartup", hr); return false; }
            mMfStarted = true;
            const uint32_t bps = aacBytesPerSecond(bitrate);
            mBitrate = bps * 8;
            std::vector<uint8_t> user;
            std::string e;
            if (!makeEncoder(mEnc, rate, ch, bps, user, e)) { error = "Media Foundation AAC: " + e; return false; }
            mAsc.assign(user.begin() + kHeaacTail, user.end());
            if (mAsc.empty()) { error = "Media Foundation AAC: no AudioSpecificConfig"; return false; }
            if (!measureDelay(bps, user, e)) { error = "Media Foundation AAC: delay measurement: " + e; return false; }
            // the lead-in of silence in front of the recording (counted in the measured priming)
            const std::vector<float> zeros((size_t)kLeadIn * ch, 0.f);
            std::vector<AudioPacket> early;
            if (!feed(zeros.data(), kLeadIn, early, e)) { error = "Media Foundation AAC: " + e; return false; }
            mPending = std::move(early);
            return true;
        }

        bool encode(const float* in, uint32_t frames, std::vector<AudioPacket>& out, std::string& error) override
        {
            if (mFlushed) { error = "Media Foundation AAC: encode after flush"; return false; }
            for (auto& p : mPending) out.push_back(std::move(p));
            mPending.clear();
            mInput += frames;
            std::string e;
            if (!feed(in, frames, out, e)) { error = "Media Foundation AAC: " + e; return false; }
            return true;
        }

        bool flush(std::vector<AudioPacket>& out, std::string& error) override
        {
            if (mFlushed) return true;
            mFlushed = true;
            for (auto& p : mPending) out.push_back(std::move(p));
            mPending.clear();
            std::string e;
            // silence so the last input samples (delayed by the priming) are fully coded, then drain
            const uint32_t pad = mPriming + 2 * kAacFrame;
            const std::vector<float> zeros((size_t)pad * mCh, 0.f);
            if (!feed(zeros.data(), pad, out, e)) { error = "Media Foundation AAC: " + e; return false; }
            HRESULT hr = mEnc.mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);   // [MSG]
            if (FAILED(hr)) { error = "Media Foundation AAC: " + hrText("drain", hr); return false; }
            if (!collect(out, e)) { error = "Media Foundation AAC: " + e; return false; }
            return true;
        }

        std::string codec() const override { return "aac"; }
        std::string name() const override { return "Media Foundation AAC"; }
        std::vector<uint8_t> decoderConfig() const override { return mAsc; }
        uint32_t primingFrames() const override { return mPriming; }
        uint64_t inputFrames() const override { return mInput; }
        uint32_t sampleRate() const override { return mRate; }
        uint32_t channels() const override { return mCh; }
        uint32_t bitrate() const override { return mBitrate; }

    private:
        bool feed(const float* in, uint32_t frames, std::vector<AudioPacket>& out, std::string& error)
        {
            if (!frames) return true;
            toInt16(in, (size_t)frames * mCh, mPcm);
            const LONGLONG t = (LONGLONG)(mFed * 10000000ULL / mRate);
            const LONGLONG t1 = (LONGLONG)((mFed + frames) * 10000000ULL / mRate);
            if (!mEnc.input(mPcm.data(), (DWORD)(mPcm.size() * sizeof(int16_t)), t, t1 - t, error)) return false;
            mFed += frames;
            return collect(out, error);
        }

        bool collect(std::vector<AudioPacket>& out, std::string& error)
        {
            return mEnc.drainOutput([&](const BYTE* p, DWORD len) {
                AudioPacket pk;
                pk.data.assign(p, p + len);
                pk.frames = kAacFrame;
                out.push_back(std::move(pk));
            }, error);
        }

        // Codes [kLeadIn zeros][kClickAt zeros][click][zeros] with a fresh encoder of the same settings, decodes it
        // with Microsoft's AAC decoder, and finds the click: priming = decoded position - kClickAt.
        bool measureDelay(uint32_t bps, const std::vector<uint8_t>& user, std::string& error)
        {
            constexpr uint32_t kClickAt = 8192, kClickLen = 32, kTotal = 8192 * 3;
            SyncTransform enc;
            std::vector<uint8_t> user2;
            if (!makeEncoder(enc, mRate, mCh, bps, user2, error)) return false;
            std::vector<int16_t> pcm((size_t)(kLeadIn + kTotal) * mCh, 0);
            for (uint32_t i = 0; i < kClickLen; ++i)
            {
                for (uint32_t c = 0; c < mCh; ++c) pcm[(size_t)(kLeadIn + kClickAt + i) * mCh + c] = 26000;
            }
            std::vector<std::vector<uint8_t>> packets;
            auto keep = [&](const BYTE* p, DWORD len) { packets.emplace_back(p, p + len); };
            const uint32_t frames = kLeadIn + kTotal;
            if (!enc.input(pcm.data(), (DWORD)(pcm.size() * sizeof(int16_t)), 0,
                           (LONGLONG)((uint64_t)frames * 10000000ULL / mRate), error) ||
                !enc.drainOutput(keep, error))
            {
                return false;
            }
            HRESULT hr = enc.mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
            if (FAILED(hr)) { error = hrText("drain", hr); return false; }
            if (!enc.drainOutput(keep, error)) return false;
            if (packets.empty()) { error = "the encoder gave no packets"; return false; }

            // [AACD] input: MFAudioFormat_AAC, MF_MT_AAC_PAYLOAD_TYPE 0 (raw: "each input sample must contain exactly
            // one full AAC compressed frame"), rate and channels, MF_MT_USER_DATA = HEAACWAVEINFO tail +
            // AudioSpecificConfig (the encoder's own); output MFAudioFormat_Float, 32 bits
            SyncTransform dec;
            if (!dec.create(MFT_CATEGORY_AUDIO_DECODER, MFAudioFormat_AAC, MFAudioFormat_Float, error)) return false;
            ComPtr<IMFMediaType> in;
            hr = MFCreateMediaType(&in);
            if (SUCCEEDED(hr))
            {
                in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
                in->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
                in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);   // [AACD] "Desired bit depth of the decoded PCM"
                in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, mRate);
                in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, mCh);
                hr = in->SetBlob(MF_MT_USER_DATA, user.data(), (UINT32)user.size());
            }
            if (SUCCEEDED(hr)) hr = dec.mft->SetInputType(0, in.Get(), 0);
            if (FAILED(hr)) { error = hrText("decoder SetInputType", hr); return false; }
            ComPtr<IMFMediaType> out;
            hr = pcmType(&out, MFAudioFormat_Float, 32, mRate, mCh);
            if (SUCCEEDED(hr)) hr = dec.mft->SetOutputType(0, out.Get(), 0);
            if (FAILED(hr)) { error = hrText("decoder SetOutputType", hr); return false; }
            if (!dec.start(error)) return false;
            std::vector<float> decoded;
            auto gather = [&](const BYTE* p, DWORD len) {
                const size_t n = len / sizeof(float);
                const size_t at = decoded.size();
                decoded.resize(at + n);
                std::memcpy(decoded.data() + at, p, n * sizeof(float));
            };
            const LONGLONG dur = (LONGLONG)(kAacFrame * 10000000ULL / mRate);
            for (size_t i = 0; i < packets.size(); ++i)
            {
                if (!dec.input(packets[i].data(), (DWORD)packets[i].size(), (LONGLONG)i * dur, dur, error) ||
                    !dec.drainOutput(gather, error))
                {
                    return false;
                }
            }
            hr = dec.mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
            if (FAILED(hr) || !dec.drainOutput(gather, error)) { if (error.empty()) error = hrText("decoder drain", hr); return false; }
            // a plain decode gives 1024 samples per packet from the first packet on; anything else means the decoder
            // trimmed or delayed on its own, and the measurement would not match other players
            if (decoded.size() != packets.size() * kAacFrame * mCh)
            {
                error = "the decoder gave " + std::to_string(decoded.size() / mCh) + " samples for " +
                        std::to_string(packets.size()) + " packets";
                return false;
            }
            size_t onset = decoded.size();
            for (size_t i = 0; i < decoded.size(); ++i)
            {
                if (std::fabs(decoded[i]) > 0.4f) { onset = i / mCh; break; }   // the click is 0.79 full scale
            }
            if (onset == decoded.size() || onset < kClickAt || onset - kClickAt > 3u * kAacFrame + kLeadIn)
            {
                error = "the click was not found where it could be";
                return false;
            }
            mPriming = (uint32_t)(onset - kClickAt);   // lead-in + the encoder's own delay (or minus what it lost)
            return true;
        }

        SyncTransform mEnc;
        std::vector<uint8_t> mAsc;
        std::vector<int16_t> mPcm;
        std::vector<AudioPacket> mPending;   // packets the lead-in made (handed out with the first encode)
        uint32_t mRate = 48000, mCh = 2, mBitrate = 0, mPriming = 0;
        uint64_t mInput = 0, mFed = 0;
        bool mFlushed = false, mComStarted = false, mMfStarted = false;
    };
}

std::unique_ptr<AudioEncoder> openMediaFoundationAac(uint32_t sample_rate, uint32_t channels, uint32_t bitrate_bps,
                                                     std::string& error)
{
    auto enc = std::make_unique<MfAacEncoder>();
    if (!enc->open(sample_rate, channels, bitrate_bps, error)) return nullptr;
    return enc;
}
}
