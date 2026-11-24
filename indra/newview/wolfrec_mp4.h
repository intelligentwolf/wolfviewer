/**
 * @file wolfrec_mp4.h
 * @brief [W111 VIDEO 2026-11-24] The video recorder's MP4 writer: one H.264 track ('avc1', ISO/IEC 14496-15) and an
 *        optional AAC ('mp4a' + 'esds') or Opus ('Opus' + 'dOps') track in an ISO/IEC 14496-12 file. No viewer
 *        headers: the standalone test compiles this file.
 *
 * Layout: ftyp, a 'free' box kept for the movie box (moov), then mdat. finish() writes moov into that space when it
 * fits (the file can then start playing before it has all arrived), otherwise after mdat.
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

#ifndef WOLFREC_MP4_H
#define WOLFREC_MP4_H

#include "wolfrec_audioenc.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace wolfrec
{
    /** What an H.264 sequence parameter set says (ITU-T H.264 7.3.2.1.1 and E.1.1). */
    struct AvcSpsInfo
    {
        uint8_t profile_idc = 0, constraint_flags = 0, level_idc = 0;
        uint32_t chroma_format_idc = 1, bit_depth_luma_minus8 = 0, bit_depth_chroma_minus8 = 0;
        uint32_t width = 0, height = 0;   // after frame cropping
        bool has_colour = false;          // VUI colour_description_present_flag
        uint8_t colour_primaries = 2, transfer_characteristics = 2, matrix_coefficients = 2;   // 2 = unspecified
        bool full_range = false;          // VUI video_full_range_flag (0 when there is no video_signal_type)
    };
    /** Parses one SPS NAL unit (starting with the NAL header byte, emulation prevention bytes still in). */
    bool parseAvcSps(const uint8_t* nal, size_t len, AvcSpsInfo& out);

    /** One writer per recording. Every call on the recorder's encoder thread. Every write is checked: a failure
     *  returns false with a plain sentence ("The disk is full." ...), later samples are refused, and finish() still
     *  makes a playable file from what was written before the failure. */
    class Mp4Writer
    {
    public:
        Mp4Writer() = default;
        ~Mp4Writer();   // closes the file without finishing it (an unfinished file does not play)
        Mp4Writer(const Mp4Writer&) = delete;
        Mp4Writer& operator=(const Mp4Writer&) = delete;

        /** Space kept after ftyp for the movie box (default 4 MB, about an hour at 60 fps). Before open(); at
         *  least 8 bytes. A recording whose movie box does not fit gets it after mdat instead. */
        void setMoovReserve(uint64_t bytes) { mReserveSize = bytes < 8 ? 8 : bytes; }
        /** Creates (or empties) the file. UTF-8 path (wide on Windows). */
        bool open(const std::string& path_utf8, std::string& error);
        /** The picture size, before the first video sample (the SPS's own size is used when it can be read). */
        void setVideo(uint32_t width, uint32_t height);
        /** Adds the sound track, before the first audio sample: codec, decoder config, priming, rate, channels,
         *  bit rate. `enc` must stay alive until finish() returns: finish() asks it for inputFrames() (the valid
         *  samples, where the edit list ends the track). */
        void setAudio(const AudioEncoder& enc);
        /** One H.264 access unit (Annex B, as wolfvr::Encoder makes it). pts in microseconds from the start of the
         *  recording (time 0 = the sound's first sample), strictly rising (dropped frames are gaps: each picture
         *  lasts until the next). The first one must be a key frame holding SPS and PPS; its pts is normally 0 (a
         *  later one starts the pictures that much later, with an empty edit). */
        bool writeVideo(const std::vector<uint8_t>& annexb_au, int64_t pts_us, bool key, std::string& error);
        /** One audio packet, in order; the sound's first sample (after priming) plays at video pts 0. */
        bool writeAudio(const AudioPacket& pkt, std::string& error);
        /** Writes the movie box and closes the file. last_video_duration_us: how long the last picture stays on
         *  screen (<= 0: as long as the one before it). After an earlier write error it still writes a playable file
         *  of the samples that were written whole; when the disk is full it drops samples from the end to make room
         *  for the movie box. False with `error` when no playable file could be made. */
        bool finish(int64_t last_video_duration_us, std::string& error);
        /** Bytes of samples written so far (the file is this plus about 4 MB of headers). */
        uint64_t bytesWritten() const { return mSampleBytes; }

    private:
        struct Sample
        {
            uint64_t offset = 0;
            uint32_t size = 0;
            uint32_t duration = 0;   // in the track's timescale; video: filled when the next picture arrives
            bool sync = false;
        };
        struct Track
        {
            std::vector<Sample> samples;
            uint32_t timescale = 0;
        };

        bool writeAt(uint64_t pos, const void* data, size_t len, std::string& error);
        bool appendSample(Track& t, const std::vector<uint8_t>& data, uint32_t duration, bool sync,
                          std::string& error);
        std::vector<uint8_t> buildMoov(uint32_t movie_timescale) const;
        void closeFile();
        static std::string ioError(int err);

        std::FILE* mFile = nullptr;
        std::string mPath;
        bool mFailed = false;         // a sample write failed: later samples are refused
        std::string mFailure;
        uint64_t mReserveStart = 0, mReserveSize = 4ull << 20;   // the 'free' box kept for moov (see wolfrec_mp4.cpp)
        uint64_t mMdatStart = 0;      // offset of the mdat box header (16 bytes: size 1, 'mdat', largesize)
        uint64_t mEnd = 0;            // end of the last sample written whole
        uint64_t mSampleBytes = 0;

        // video
        uint32_t mWidth = 0, mHeight = 0;
        Track mVideo;
        std::vector<uint8_t> mSps, mPps;   // from the first key frame (NAL units without start codes)
        AvcSpsInfo mSpsInfo;
        bool mInBandParams = false;   // a later, different SPS/PPS kept in the samples: the entry becomes 'avc3'
        int64_t mFirstPts = 0, mLastPts = 0;
        uint64_t mLastTick = 0;

        // audio
        const AudioEncoder* mAudioEnc = nullptr;
        bool mHasAudio = false, mAudioOpus = false;
        uint32_t mAudioRate = 0, mAudioChannels = 0, mAudioBitrate = 0, mAudioPriming = 0;
        std::vector<uint8_t> mAudioConfig;
        Track mAudio;
    };
}

#endif // WOLFREC_MP4_H
