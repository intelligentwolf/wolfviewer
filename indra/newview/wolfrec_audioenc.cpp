/**
 * @file wolfrec_audioenc.cpp
 * @brief [W111 VIDEO 2026-11-24] The recorder's sound encoders: the choice (openAudioEncoder), the Opus encoder
 *        (libopus inside llwebrtc) and the AAC-LC AudioSpecificConfig. See wolfrec_audioenc.h.
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

#include "llwebrtc.h"

#include <algorithm>
#include <cstring>

namespace wolfrec
{
std::vector<uint8_t> aacLcAudioSpecificConfig(uint32_t sample_rate, uint32_t channels)
{
    // ISO/IEC 14496-3:2009 1.6.3.4 Table 1.18 samplingFrequencyIndex: 0x0 96000, 0x1 88200, 0x2 64000, 0x3 48000,
    // 0x4 44100, 0x5 32000, 0x6 24000, 0x7 22050, 0x8 16000, 0x9 12000, 0xa 11025, 0xb 8000, 0xc 7350.
    static const uint32_t kRates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025,
                                      8000, 7350};
    uint32_t index = 0xF;
    for (uint32_t i = 0; i < sizeof(kRates) / sizeof(kRates[0]); ++i)
    {
        if (kRates[i] == sample_rate) { index = i; break; }
    }
    // channelConfiguration 1..7 are the standard layouts (14496-3 1.6.3.5 Table 1.19); 1 = mono, 2 = stereo
    if (index == 0xF || channels < 1 || channels > 2) return {};
    // 14496-3 1.6.2.1 AudioSpecificConfig(): audioObjectType 5 bits (2 = AAC LC, Table 1.17),
    // samplingFrequencyIndex 4 bits, channelConfiguration 4 bits, then GASpecificConfig() (4.4.1 Table 4.1):
    // frameLengthFlag 1 bit (0 = 1024-sample frames), dependsOnCoreCoder 1 bit (0), extensionFlag 1 bit (0
    // for object type 2). 16 bits in all: 48 kHz stereo = 0x11 0x90 (Microsoft's 44.1 kHz example: 0x12 0x10).
    const uint32_t bits = (2u << 11) | (index << 7) | (channels << 3);
    return {(uint8_t)(bits >> 8), (uint8_t)(bits & 0xFF)};
}

// ---------------------------------------------------------------------------------------------------------------
// Opus (libopus inside llwebrtc: llwebrtc.h newOpusEncoder)
// ---------------------------------------------------------------------------------------------------------------
namespace
{
    class OpusAudioEncoder : public AudioEncoder
    {
    public:
        ~OpusAudioEncoder() override
        {
            if (mEnc) llwebrtc::freeOpusEncoder(mEnc);
        }

        bool open(uint32_t rate, uint32_t channels, uint32_t bitrate, std::string& error)
        {
            // Ogg Opus / Opus-in-ISOBMFF carry the stream as 48 kHz (RFC 7845 4; opus_in_isobmff 4.3.1 samplerate
            // 48000<<16), so the recorder's 48 kHz is the only rate taken here.
            if (rate != 48000 || channels < 1 || channels > 2)
            {
                error = "opus: needs 48000 Hz mono or stereo (got " + std::to_string(rate) + " Hz, " +
                        std::to_string(channels) + " channels)";
                return false;
            }
            int code = 0;
            mEnc = llwebrtc::newOpusEncoder((int)rate, (int)channels, (int)bitrate, &code);
            if (!mEnc)
            {
                const char* text = llwebrtc::opusErrorText(code);
                error = std::string("opus: the encoder could not be created (") + (text ? text : "?") + ")";
                return false;
            }
            mRate = rate;
            mChannels = channels;
            mBitrate = bitrate;
            mPreSkip = (uint32_t)mEnc->lookahead();
            mPending.reserve((size_t)kFrame * channels);
            return true;
        }

        bool encode(const float* in, uint32_t frames, std::vector<AudioPacket>& out, std::string& error) override
        {
            if (mFlushed) { error = "opus: encode after flush"; return false; }
            mInput += frames;
            return push(in, frames, out, error);
        }

        bool flush(std::vector<AudioPacket>& out, std::string& error) override
        {
            if (mFlushed) return true;
            mFlushed = true;
            // Everything the decoder needs to give back every input sample: input + pre-skip samples must be coded
            // (OPUS_GET_LOOKAHEAD: "the real data begins this many samples late"), rounded up to whole frames.
            const uint64_t needed = mInput + mPreSkip;
            const std::vector<float> zeros((size_t)kFrame * mChannels, 0.f);
            while (mCoded + mPending.size() / mChannels < needed)
            {
                const uint64_t missing = needed - mCoded - mPending.size() / mChannels;
                const uint32_t n = (uint32_t)std::min<uint64_t>(missing, kFrame);
                if (!push(zeros.data(), n, out, error)) return false;
            }
            if (!mPending.empty())
            {
                const uint32_t n = kFrame - (uint32_t)(mPending.size() / mChannels);
                if (!push(zeros.data(), n, out, error)) return false;
            }
            return true;
        }

        std::string codec() const override { return "opus"; }
        std::string name() const override { return "Opus (libopus in llwebrtc)"; }

        std::vector<uint8_t> decoderConfig() const override
        {
            // RFC 7845 5.1 identification header: "OpusHead", version 1, output channel count, pre-skip (16-bit LE),
            // input sample rate (32-bit LE), output gain (16-bit LE, 0), channel mapping family 0 (mono/stereo).
            std::vector<uint8_t> h = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, (uint8_t)mChannels};
            h.push_back((uint8_t)(mPreSkip & 0xFF));
            h.push_back((uint8_t)(mPreSkip >> 8));
            for (int i = 0; i < 4; ++i) h.push_back((uint8_t)(mRate >> (8 * i)));
            h.push_back(0);
            h.push_back(0);
            h.push_back(0);
            return h;
        }
        uint32_t primingFrames() const override { return mPreSkip; }
        uint64_t inputFrames() const override { return mInput; }
        uint32_t sampleRate() const override { return mRate; }
        uint32_t channels() const override { return mChannels; }
        uint32_t bitrate() const override { return mBitrate; }

    private:
        // 20 ms at 48 kHz, one of opus_encode_float's frame sizes (opus.h: 120, 240, 480, 960, 1920, 2880)
        static constexpr uint32_t kFrame = 960;
        // opus.h:139 "4000 bytes is recommended" for the output buffer
        static constexpr int kMaxPacket = 4000;

        bool push(const float* in, uint32_t frames, std::vector<AudioPacket>& out, std::string& error)
        {
            size_t i = 0;
            const size_t total = (size_t)frames * mChannels;
            while (i < total)
            {
                const size_t room = (size_t)kFrame * mChannels - mPending.size();
                const size_t take = std::min(room, total - i);
                for (size_t k = 0; k < take; ++k) mPending.push_back(std::clamp(in[i + k], -1.f, 1.f));
                i += take;
                if (mPending.size() == (size_t)kFrame * mChannels)
                {
                    AudioPacket p;
                    p.data.resize(kMaxPacket);
                    const int n = mEnc->encode(mPending.data(), (int)kFrame, p.data.data(), kMaxPacket);
                    if (n < 0)
                    {
                        const char* text = llwebrtc::opusErrorText(n);
                        error = std::string("opus: ") + (text ? text : "encode failed");
                        return false;
                    }
                    p.data.resize((size_t)n);
                    p.frames = kFrame;
                    out.push_back(std::move(p));
                    mPending.clear();
                    mCoded += kFrame;
                }
            }
            return true;
        }

        llwebrtc::LLWebRTCOpusEncoder* mEnc = nullptr;
        uint32_t mRate = 48000, mChannels = 2, mBitrate = 0, mPreSkip = 0;
        uint64_t mInput = 0, mCoded = 0;
        bool mFlushed = false;
        std::vector<float> mPending;
    };
}

std::unique_ptr<AudioEncoder> openOpus(uint32_t sample_rate, uint32_t channels, uint32_t bitrate_bps,
                                       std::string& error)
{
    auto enc = std::make_unique<OpusAudioEncoder>();
    if (!enc->open(sample_rate, channels, bitrate_bps, error)) return nullptr;
    return enc;
}

std::unique_ptr<AudioEncoder> openAudioEncoder(const std::string& which, uint32_t sample_rate, uint32_t channels,
                                               uint32_t bitrate_bps, std::string& tried)
{
    tried.clear();
    const bool any = which.empty() || which == "auto";
#if defined(_WIN32) || defined(__APPLE__)
    // Paul (W111): AAC from the operating system on Windows and macOS - QuickTime before macOS 14 cannot play Opus
    // in MP4. Opus is the fallback when the system encoder is missing or refuses (Windows N without the Media
    // Feature Pack, a failed calibration): the file still plays in VLC, browsers, Windows 10+ and macOS 14+.
    if (any || which == "aac")
    {
        std::string err;
#if defined(_WIN32)
        auto enc = openMediaFoundationAac(sample_rate, channels, bitrate_bps, err);
#else
        auto enc = openAudioToolboxAac(sample_rate, channels, bitrate_bps, err);
#endif
        if (enc) return enc;
        tried += err + "; ";
    }
#else
    if (which == "aac") tried += "aac: the system AAC encoder is used on Windows and macOS only; ";
#endif
    if (any || which == "opus")
    {
        std::string err;
        auto enc = openOpus(sample_rate, channels, bitrate_bps, err);
        if (enc) return enc;
        tried += err + "; ";
    }
    if (!any && which != "aac" && which != "opus") tried += "no sound encoder matched '" + which + "'";
    return nullptr;
}
}
