/**
 * @file wolfrec_mp4.h
 * @brief [W111 VIDEO 2026-11-24] The video recorder's MP4 writer: one H.264 track ('avc1', ISO/IEC 14496-15) and an
 *        optional AAC ('mp4a' + 'esds') or Opus ('Opus' + 'dOps') track in an ISO/IEC 14496-12 file. No viewer
 *        headers: the standalone test compiles this file.
 *
 * Layout: ftyp, a 'free' box kept for the final movie box (moov), then [W112 RECORDER CRASH-SAFE 2026-11-25] a
 * fragmented movie (ISO/IEC 14496-12 8.8): a moov with no samples and an 'mvex', then a movie fragment (moof + mdat)
 * every 1-4 s, each starting at a video key frame when one comes in time. A recording cut off at any point (the viewer
 * crashes or is killed) plays up to its last whole fragment. finish() writes the full moov into the kept space and
 * turns everything after it into one mdat (the moov with no samples and the moofs become bytes inside it), so a saved
 * recording is an ordinary MP4 with its index at the front, as in W111.
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
     *  makes a playable file from the fragments written whole before the failure. */
    class Mp4Writer
    {
    public:
        Mp4Writer() = default;
        ~Mp4Writer();   // closes the file without finishing it: it plays up to its last whole fragment
        Mp4Writer(const Mp4Writer&) = delete;
        Mp4Writer& operator=(const Mp4Writer&) = delete;

        /** Space kept after ftyp for the final movie box (default 4 MB, about an hour at 60 fps). Before open(); at
         *  least 24 bytes. A recording whose movie box does not fit gets it after the last fragment instead. */
        void setMoovReserve(uint64_t bytes) { mReserveSize = bytes < 24 ? 24 : bytes; }
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
         *  later one starts the pictures that much later, with an empty edit). May write a fragment to the file. */
        bool writeVideo(const std::vector<uint8_t>& annexb_au, int64_t pts_us, bool key, std::string& error);
        /** One audio packet, in order; the sound's first sample (after priming) plays at video pts 0. */
        bool writeAudio(const AudioPacket& pkt, std::string& error);
        /** Writes the last fragment and the full movie box, and closes the file. last_video_duration_us: how long
         *  the last picture stays on screen (<= 0: as long as the one before it). After an earlier write error it
         *  still makes a playable file of the fragments that were written whole; when the disk is full it drops
         *  fragments from the end to make room for the movie box. False with `error` when no playable file could be
         *  made. */
        bool finish(int64_t last_video_duration_us, std::string& error);
        /** Bytes of samples taken so far, written or waiting for their fragment (the file is this plus about 4 MB
         *  of headers). */
        uint64_t bytesWritten() const { return mSampleBytes; }

    private:
        struct Sample
        {
            uint64_t offset = 0;     // in the file; for a sample waiting for its fragment: in Track::pending_bytes
            uint32_t size = 0;
            uint32_t duration = 0;   // in the track's timescale; video: filled when the next picture arrives
            bool sync = false;
        };
        struct Track
        {
            std::vector<Sample> samples;          // written whole, in fragments
            uint32_t timescale = 0;
            uint64_t decode_time = 0;             // sum of the written samples' durations (the next tfdt)
            std::vector<Sample> pending;          // taken, waiting for the next fragment
            std::vector<uint8_t> pending_bytes;
        };
        /** Where each fragment starts, to drop whole fragments from the end when the disk is full at finish(). */
        struct Fragment
        {
            uint64_t start = 0;   // file offset of its first byte (for the first fragment: the moov with no samples)
            size_t video_samples = 0, audio_samples = 0;   // samples written before it
        };

        bool writeAt(uint64_t pos, const void* data, size_t len, std::string& error);
        bool syncFile(std::string& error);
        void addPending(Track& t, const std::vector<uint8_t>& data, uint32_t duration, bool sync);
        bool flushFragment(std::string& error);
        std::vector<uint8_t> buildMoov(uint32_t movie_timescale, bool initial, size_t* entry_type_at,
                                       size_t* mehd_at) const;
        uint64_t fragmentedDuration() const;
        uint32_t movieTimescale(bool initial) const;
        void closeFile();
        static std::string ioError(int err);

        std::FILE* mFile = nullptr;
        std::string mPath;
        bool mFailed = false;         // a write failed: later samples are refused
        std::string mFailure;
        uint64_t mReserveStart = 0, mReserveSize = 4ull << 20;   // the 'free' box kept for moov (see wolfrec_mp4.cpp)
        uint64_t mEnd = 0;            // end of the last fragment written whole
        uint64_t mSampleBytes = 0;
        bool mInitWritten = false;    // the moov with no samples is in the file (with the first fragment)
        uint64_t mInitEntryType = 0;  // file offset of its video sample entry's type ('avc1' / 'avc3')
        bool mInitAvc3 = false;       // that type says 'avc3'
        uint64_t mMehdAt = 0;         // file offset of its mehd fragment_duration (64 bits)
        uint32_t mSequence = 0;       // mfhd sequence_number of the last fragment written
        std::vector<Fragment> mFragments;

        // video
        uint32_t mWidth = 0, mHeight = 0;
        Track mVideo;
        std::vector<uint8_t> mSps, mPps;   // from the first key frame (NAL units without start codes)
        AvcSpsInfo mSpsInfo;
        bool mInBandParams = false;   // a later, different SPS/PPS kept in the samples: the entry becomes 'avc3'
        int64_t mFirstPts = 0, mLastPts = 0;
        uint64_t mLastTick = 0;
        bool mHaveVideo = false;      // the first picture has been taken

        // audio
        const AudioEncoder* mAudioEnc = nullptr;
        bool mHasAudio = false, mAudioOpus = false;
        uint32_t mAudioRate = 0, mAudioChannels = 0, mAudioBitrate = 0, mAudioPriming = 0;
        std::vector<uint8_t> mAudioConfig;
        Track mAudio;
        uint32_t mFirstAudioFrames = 0;   // the first packet's length (Opus 'roll' distance in packets)
        bool mInitRoll = false;       // the moov with no samples holds the Opus 'roll' group (fragments refer to it)
    };
}

#endif // WOLFREC_MP4_H
