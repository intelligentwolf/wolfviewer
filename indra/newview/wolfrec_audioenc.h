/**
 * @file wolfrec_audioenc.h
 * @brief [W111 VIDEO 2026-11-24] The video recorder's sound encoders: AAC-LC from the operating system on Windows
 *        (Media Foundation AAC encoder) and macOS (AudioToolbox AudioConverter), Opus (libopus inside llwebrtc) on
 *        Linux and as the Windows/macOS fallback. No viewer headers: the standalone test compiles this file.
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

#ifndef WOLFREC_AUDIOENC_H
#define WOLFREC_AUDIOENC_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace wolfrec
{
    /** One compressed packet = one MP4 sample. `frames` is its duration in samples per channel at the encoder's
     *  sample rate (AAC: always 1024; Opus: the frame size, 960 = 20 ms here). */
    struct AudioPacket
    {
        std::vector<uint8_t> data;
        uint32_t frames = 0;
    };

    /** Everything runs on the caller's one thread (the recorder's encoder thread). */
    class AudioEncoder
    {
    public:
        virtual ~AudioEncoder() = default;
        /** Takes `frames` interleaved float samples per channel (-1..1; outside is clipped) and appends every packet
         *  that became complete to `out`. False with `error` set when the encoder failed (it is then unusable). */
        virtual bool encode(const float* interleaved, uint32_t frames, std::vector<AudioPacket>& out,
                            std::string& error) = 0;
        /** End of the sound: pads with silence so every input sample (and the codec's look-ahead) is in a packet,
         *  and appends the last packets. Call once; nothing may be encoded after it. */
        virtual bool flush(std::vector<AudioPacket>& out, std::string& error) = 0;
        /** "aac" (MPEG-4 AAC-LC) or "opus". */
        virtual std::string codec() const = 0;
        /** For the record: "Media Foundation AAC", "AudioToolbox AAC", "Opus (libopus 1.x)". */
        virtual std::string name() const = 0;
        /** AAC: the AudioSpecificConfig (ISO/IEC 14496-3 1.6.2.1) for the MP4 'esds' DecoderSpecificInfo.
         *  Opus: the 19-byte identification header OpusHead (RFC 7845 5.1, channel mapping family 0) - its fields are
         *  the ones the MP4 'dOps' box carries. */
        virtual std::vector<uint8_t> decoderConfig() const = 0;
        /** Samples per channel at the start of the decoded sound that are not the input (AAC priming / encoder delay,
         *  Opus pre-skip). The MP4 edit list skips them, so the first input sample plays at the track's start. */
        virtual uint32_t primingFrames() const = 0;
        /** Samples per channel handed to encode() so far (the valid samples: the MP4 edit list ends there). */
        virtual uint64_t inputFrames() const = 0;
        virtual uint32_t sampleRate() const = 0;
        virtual uint32_t channels() const = 0;
        /** Target bit rate in bits/s (for the 'esds' avgBitrate). */
        virtual uint32_t bitrate() const = 0;
    };

    /** Opens a sound encoder. `which`: "auto" (Windows/macOS: the system AAC encoder, then Opus; Linux: Opus),
     *  "aac" (the system AAC only) or "opus". sample_rate 48000 and channels 2 are what the recorder uses (both AAC
     *  encoders and Opus take them). `tried` lists every attempt that failed and why. Null when none worked.
     *  Note for the UI: an Opus track in MP4 does not play in QuickTime before macOS 14 (Sonoma). */
    std::unique_ptr<AudioEncoder> openAudioEncoder(const std::string& which, uint32_t sample_rate, uint32_t channels,
                                                   uint32_t bitrate_bps, std::string& tried);

    // Platform encoders (wolfrec_audioenc_mf.cpp on Windows, wolfrec_audioenc_at.mm on macOS); used by
    // openAudioEncoder. Null with `error` set when the system encoder is missing or refuses.
#if defined(_WIN32)
    std::unique_ptr<AudioEncoder> openMediaFoundationAac(uint32_t sample_rate, uint32_t channels, uint32_t bitrate_bps,
                                                         std::string& error);
#elif defined(__APPLE__)
    std::unique_ptr<AudioEncoder> openAudioToolboxAac(uint32_t sample_rate, uint32_t channels, uint32_t bitrate_bps,
                                                      std::string& error);
#endif
    std::unique_ptr<AudioEncoder> openOpus(uint32_t sample_rate, uint32_t channels, uint32_t bitrate_bps,
                                           std::string& error);

    /** AudioSpecificConfig for AAC-LC (ISO/IEC 14496-3 1.6.2.1, 1.6.3.3-1.6.3.5): 2 bytes for the nine standard
     *  sample rates. Empty for a rate without a sampling frequency index (an explicit 24-bit rate is not written). */
    std::vector<uint8_t> aacLcAudioSpecificConfig(uint32_t sample_rate, uint32_t channels);
}

#endif // WOLFREC_AUDIOENC_H
