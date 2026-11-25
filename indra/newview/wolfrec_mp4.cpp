/**
 * @file wolfrec_mp4.cpp
 * @brief [W111 VIDEO 2026-11-24] The video recorder's MP4 writer. See wolfrec_mp4.h.
 *
 * Specifications cited below:
 *   ISO/IEC 14496-12:2015 (ISO base media file format)       "12 x.y"
 *   ISO/IEC 14496-14:2020 (MP4 file format: 'esds')           "14 x.y"
 *   ISO/IEC 14496-15:2014 (AVC file format: 'avcC')            "15 x.y"
 *   ISO/IEC 14496-1:2010  (MPEG-4 Systems: descriptors)        "1 x.y"
 *   ITU-T H.264 (08/2021)                                      "H.264 x.y"
 *   Encapsulation of Opus in ISOBMFF 0.6.8, opus-codec.org/docs/opus_in_isobmff.html   "Opus x.y"
 *
 * [W112 RECORDER CRASH-SAFE 2026-11-25] The fragmented form (12 8.8) is checked against the readers and writers that
 * make and play such files: FFmpeg n9.0.2 libavformat/mov.c (reader) and movenc.c (writer, hybrid_fragmented), and
 * OBS Studio's hybrid MP4 writer plugins/obs-outputs/mp4-mux.c.
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

#include "wolfrec_mp4.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>   // _chsize_s, _commit, _fileno
#else
#include <unistd.h>   // ftruncate, fsync
#include <sys/types.h>
#endif

namespace wolfrec
{
// ---------------------------------------------------------------------------------------------------------------
// H.264 bitstream helpers
// ---------------------------------------------------------------------------------------------------------------
namespace
{
    // H.264 7.3.1 / 7.4.1: emulation_prevention_three_byte (0x03 after two zero bytes) is not part of the RBSP.
    std::vector<uint8_t> nalToRbsp(const uint8_t* p, size_t n)
    {
        std::vector<uint8_t> out;
        out.reserve(n);
        int zeros = 0;
        for (size_t i = 0; i < n; ++i)
        {
            if (zeros >= 2 && p[i] == 0x03)
            {
                zeros = 0;
                continue;
            }
            out.push_back(p[i]);
            zeros = p[i] == 0 ? zeros + 1 : 0;
        }
        return out;
    }

    class BitReader
    {
    public:
        BitReader(const uint8_t* p, size_t n) : mP(p), mN(n) {}
        bool ok() const { return !mOver; }
        uint32_t u(int bits)
        {
            uint32_t v = 0;
            for (int i = 0; i < bits; ++i)
            {
                if (mPos >= mN * 8) { mOver = true; return 0; }
                v = (v << 1) | ((mP[mPos >> 3] >> (7 - (mPos & 7))) & 1);
                ++mPos;
            }
            return v;
        }
        // H.264 9.1 ue(v): leadingZeroBits zeros, a one, then leadingZeroBits bits
        uint32_t ue()
        {
            int zeros = 0;
            while (u(1) == 0)
            {
                if (mOver || ++zeros > 31) { mOver = true; return 0; }
            }
            if (zeros == 0) return 0;
            return (uint32_t)(((1ull << zeros) - 1) + u(zeros));
        }
        // H.264 9.1.1 se(v): k = ue; (-1)^(k+1) * Ceil(k / 2)
        int32_t se()
        {
            const uint32_t k = ue();
            return (k & 1) ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2);
        }
    private:
        const uint8_t* mP;
        size_t mN, mPos = 0;
        bool mOver = false;
    };

    // Calls fn(nal_type, nal_ptr (header byte first), nal_len) for each NAL unit of an Annex B byte stream
    // (H.264 B.1.1: start code prefix 0x000001, optionally preceded by zero_byte).
    template <typename Fn>
    void forEachNal(const std::vector<uint8_t>& b, Fn fn)
    {
        const size_t n = b.size();
        auto findStart = [&](size_t from, size_t& sc) -> size_t {
            for (size_t k = from; k + 3 <= n; ++k)
            {
                if (b[k] == 0 && b[k + 1] == 0 && b[k + 2] == 1) { sc = 3; return k; }
            }
            sc = 0;
            return n;
        };
        size_t sc = 0;
        size_t i = findStart(0, sc);
        while (i < n)
        {
            const size_t start = i + sc;
            size_t sc2 = 0;
            size_t next = findStart(start, sc2);
            // trailing_zero_8bits / the zero_byte of a 4-byte start code belong to neither NAL unit (B.1.1)
            size_t end = next;
            while (end > start && b[end - 1] == 0) --end;
            if (end > start) fn(b[start] & 0x1F, &b[start], end - start);
            i = next;
            sc = sc2;
        }
    }

    // H.264 7.3.2.1.1.1 scaling_list(): only skipped here
    void skipScalingList(BitReader& br, int size)
    {
        int last = 8, next = 8;
        for (int j = 0; j < size; ++j)
        {
            if (next != 0)
            {
                const int delta = br.se();
                next = (last + delta + 256) % 256;
            }
            last = next == 0 ? last : next;
        }
    }
}

bool parseAvcSps(const uint8_t* nal, size_t len, AvcSpsInfo& o)
{
    if (len < 4 || (nal[0] & 0x1F) != 7) return false;
    const std::vector<uint8_t> rbsp = nalToRbsp(nal + 1, len - 1);
    BitReader br(rbsp.data(), rbsp.size());
    // H.264 7.3.2.1.1 seq_parameter_set_data()
    o.profile_idc = (uint8_t)br.u(8);
    o.constraint_flags = (uint8_t)br.u(8);
    o.level_idc = (uint8_t)br.u(8);
    br.ue();   // seq_parameter_set_id
    uint32_t separate_colour_plane = 0;
    const uint8_t p = o.profile_idc;
    o.chroma_format_idc = 1;   // 7.4.2.1.1: inferred 1 when not present
    if (p == 100 || p == 110 || p == 122 || p == 244 || p == 44 || p == 83 || p == 86 || p == 118 || p == 128 ||
        p == 138 || p == 139 || p == 134 || p == 135)
    {
        o.chroma_format_idc = br.ue();
        if (o.chroma_format_idc == 3) separate_colour_plane = br.u(1);
        o.bit_depth_luma_minus8 = br.ue();
        o.bit_depth_chroma_minus8 = br.ue();
        br.u(1);   // qpprime_y_zero_transform_bypass_flag
        if (br.u(1))   // seq_scaling_matrix_present_flag
        {
            const int lists = o.chroma_format_idc != 3 ? 8 : 12;
            for (int i = 0; i < lists; ++i)
            {
                if (br.u(1)) skipScalingList(br, i < 6 ? 16 : 64);   // seq_scaling_list_present_flag[i]
            }
        }
    }
    br.ue();   // log2_max_frame_num_minus4
    const uint32_t poc_type = br.ue();
    if (poc_type == 0)
    {
        br.ue();   // log2_max_pic_order_cnt_lsb_minus4
    }
    else if (poc_type == 1)
    {
        br.u(1);   // delta_pic_order_always_zero_flag
        br.se();   // offset_for_non_ref_pic
        br.se();   // offset_for_top_to_bottom_field
        const uint32_t cycle = br.ue();   // num_ref_frames_in_pic_order_cnt_cycle (0..255, 7.4.2.1.1)
        if (cycle > 255) return false;
        for (uint32_t i = 0; i < cycle; ++i) br.se();
    }
    br.ue();   // max_num_ref_frames
    br.u(1);   // gaps_in_frame_num_value_allowed_flag
    const uint32_t w_mbs = br.ue() + 1;   // pic_width_in_mbs_minus1
    const uint32_t h_map = br.ue() + 1;   // pic_height_in_map_units_minus1
    const uint32_t frame_mbs_only = br.u(1);
    if (!frame_mbs_only) br.u(1);   // mb_adaptive_frame_field_flag
    br.u(1);   // direct_8x8_inference_flag
    uint32_t cl = 0, cr = 0, ct = 0, cb = 0;
    if (br.u(1))   // frame_cropping_flag
    {
        cl = br.ue(); cr = br.ue(); ct = br.ue(); cb = br.ue();
    }
    // 7.4.2.1.1 frame_crop_*: ChromaArrayType = 0 if separate_colour_plane_flag else chroma_format_idc;
    // CropUnitX = 1, CropUnitY = 2 - frame_mbs_only_flag when ChromaArrayType is 0, otherwise
    // CropUnitX = SubWidthC, CropUnitY = SubHeightC * (2 - frame_mbs_only_flag) (Table 6-1: 4:2:0 = 2/2,
    // 4:2:2 = 2/1, 4:4:4 = 1/1). Height in map units: FrameHeightInMbs = (2 - frame_mbs_only_flag) * map units.
    const uint32_t cat = separate_colour_plane ? 0 : o.chroma_format_idc;
    const uint32_t sub_w = (cat == 1 || cat == 2) ? 2 : 1;
    const uint32_t sub_h = cat == 1 ? 2 : 1;
    const uint32_t unit_x = cat == 0 ? 1 : sub_w;
    const uint32_t unit_y = (cat == 0 ? 1 : sub_h) * (2 - frame_mbs_only);
    const uint64_t full_w = (uint64_t)w_mbs * 16, full_h = (uint64_t)(2 - frame_mbs_only) * h_map * 16;
    const uint64_t crop_w = (uint64_t)unit_x * (cl + cr), crop_h = (uint64_t)unit_y * (ct + cb);
    if (crop_w >= full_w || crop_h >= full_h) return false;
    o.width = (uint32_t)(full_w - crop_w);
    o.height = (uint32_t)(full_h - crop_h);
    o.has_colour = false;
    o.full_range = false;
    if (br.u(1))   // vui_parameters_present_flag -> H.264 E.1.1 vui_parameters()
    {
        if (br.u(1))   // aspect_ratio_info_present_flag
        {
            if (br.u(8) == 255) { br.u(16); br.u(16); }   // aspect_ratio_idc Extended_SAR (Table E-1): sar_width/height
        }
        if (br.u(1)) br.u(1);   // overscan_info_present_flag -> overscan_appropriate_flag
        if (br.u(1))   // video_signal_type_present_flag
        {
            br.u(3);   // video_format
            o.full_range = br.u(1) != 0;   // video_full_range_flag
            if (br.u(1))   // colour_description_present_flag
            {
                o.colour_primaries = (uint8_t)br.u(8);
                o.transfer_characteristics = (uint8_t)br.u(8);
                o.matrix_coefficients = (uint8_t)br.u(8);
                o.has_colour = true;
            }
        }
    }
    return br.ok();
}

// ---------------------------------------------------------------------------------------------------------------
// Box building
// ---------------------------------------------------------------------------------------------------------------
namespace
{
    // Big-endian writer with nested boxes (12 4.2: size(32) type(32) [largesize(64)]; FullBox adds version(8)
    // flags(24)). Boxes here are never 4 GB, so the 32-bit size is patched in when a box ends.
    class Buf
    {
    public:
        std::vector<uint8_t> b;
        void u8(uint32_t v) { b.push_back((uint8_t)v); }
        void u16(uint32_t v) { u8(v >> 8); u8(v); }
        void u24(uint32_t v) { u8(v >> 16); u8(v >> 8); u8(v); }
        void u32(uint32_t v) { u8(v >> 24); u8(v >> 16); u8(v >> 8); u8(v); }
        void u64(uint64_t v) { u32((uint32_t)(v >> 32)); u32((uint32_t)v); }
        void fourcc(const char* c) { for (int i = 0; i < 4; ++i) u8((uint8_t)c[i]); }
        void bytes(const void* p, size_t n) { const uint8_t* s = (const uint8_t*)p; b.insert(b.end(), s, s + n); }
        void zeros(size_t n) { b.insert(b.end(), n, 0); }
        size_t begin(const char* type)
        {
            const size_t at = b.size();
            u32(0);
            fourcc(type);
            return at;
        }
        size_t beginFull(const char* type, uint32_t version, uint32_t flags)
        {
            const size_t at = begin(type);
            u8(version);
            u24(flags);
            return at;
        }
        void end(size_t at)
        {
            const uint32_t size = (uint32_t)(b.size() - at);
            b[at] = (uint8_t)(size >> 24); b[at + 1] = (uint8_t)(size >> 16);
            b[at + 2] = (uint8_t)(size >> 8); b[at + 3] = (uint8_t)size;
        }
        void append(const Buf& o) { b.insert(b.end(), o.b.begin(), o.b.end()); }
    };

    // 1 8.3.3 expandable size of a descriptor: 7 bits per byte, the top bit set on every byte but the last. Written
    // in 4 bytes (as many writers do: the length is then fixed whatever the payload).
    void descriptor(Buf& out, uint8_t tag, const Buf& body)
    {
        const uint32_t n = (uint32_t)body.b.size();
        out.u8(tag);
        out.u8(0x80 | ((n >> 21) & 0x7F));
        out.u8(0x80 | ((n >> 14) & 0x7F));
        out.u8(0x80 | ((n >> 7) & 0x7F));
        out.u8(n & 0x7F);
        out.append(body);
    }

    // 12 8.2.2.3 / 8.3.2.3: identity matrix { 0x00010000,0,0, 0,0x00010000,0, 0,0,0x40000000 }
    void unityMatrix(Buf& b)
    {
        const uint32_t m[9] = {0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000};
        for (uint32_t v : m) b.u32(v);
    }

    uint64_t rescale(uint64_t v, uint64_t from, uint64_t to)
    {
        // v * to / from, rounded, without overflowing for any realistic recording (v < 2^40, to < 2^24)
        return (v * to + from / 2) / from;
    }

    // 12 8.2.2.3 creation_time: seconds since midnight, Jan. 1, 1904, in UTC. 2082844800 s from 1904 to 1970.
    uint64_t mp4Now()
    {
        const std::time_t t = std::time(nullptr);
        return (t > 0 ? (uint64_t)t : 0) + 2082844800ull;
    }

    constexpr uint32_t kVideoTimescale = 90000;   // the usual MPEG clock: 1 µs is 0.09 ticks, so ticks are rounded
                                                  // from each picture's absolute time (no drift)

    // The sample tables of one track (12 8.5-8.7). Chunks: runs of samples that follow each other in the file.
    void sampleTables(Buf& stbl, const std::vector<std::pair<uint64_t, uint32_t>>& at,   // offset, size
                      const std::vector<uint32_t>& durations, const std::vector<uint32_t>* sync_numbers)
    {
        // stts 12 8.6.1.2: entry_count, { sample_count, sample_delta } runs
        {
            std::vector<std::pair<uint32_t, uint32_t>> runs;
            for (uint32_t d : durations)
            {
                if (!runs.empty() && runs.back().second == d) ++runs.back().first;
                else runs.emplace_back(1, d);
            }
            const size_t box = stbl.beginFull("stts", 0, 0);
            stbl.u32((uint32_t)runs.size());
            for (auto& r : runs) { stbl.u32(r.first); stbl.u32(r.second); }
            stbl.end(box);
        }
        // stss 12 8.6.2.2: entry_count, sample_number (1-based) - absent when every sample is a sync sample
        if (sync_numbers)
        {
            const size_t box = stbl.beginFull("stss", 0, 0);
            stbl.u32((uint32_t)sync_numbers->size());
            for (uint32_t n : *sync_numbers) stbl.u32(n);
            stbl.end(box);
        }
        // chunks
        std::vector<uint64_t> chunk_offsets;
        std::vector<uint32_t> chunk_counts;
        for (size_t i = 0; i < at.size(); ++i)
        {
            if (i > 0 && at[i].first == at[i - 1].first + at[i - 1].second) ++chunk_counts.back();
            else { chunk_offsets.push_back(at[i].first); chunk_counts.push_back(1); }
        }
        // stsc 12 8.7.4.2: entry_count, { first_chunk (1-based), samples_per_chunk, sample_description_index }
        {
            std::vector<std::pair<uint32_t, uint32_t>> runs;
            for (size_t c = 0; c < chunk_counts.size(); ++c)
            {
                if (runs.empty() || runs.back().second != chunk_counts[c]) runs.emplace_back((uint32_t)c + 1, chunk_counts[c]);
            }
            const size_t box = stbl.beginFull("stsc", 0, 0);
            stbl.u32((uint32_t)runs.size());
            for (auto& r : runs) { stbl.u32(r.first); stbl.u32(r.second); stbl.u32(1); }
            stbl.end(box);
        }
        // stsz 12 8.7.3.2: sample_size 0 (sizes differ), sample_count, entry_size per sample
        {
            const size_t box = stbl.beginFull("stsz", 0, 0);
            stbl.u32(0);
            stbl.u32((uint32_t)at.size());
            for (auto& s : at) stbl.u32(s.second);
            stbl.end(box);
        }
        // stco / co64 12 8.7.5.2: entry_count, chunk_offset (32 or 64 bits); co64 only when needed
        const bool big = !chunk_offsets.empty() && chunk_offsets.back() > 0xFFFFFFFFull;
        const size_t box = stbl.beginFull(big ? "co64" : "stco", 0, 0);
        stbl.u32((uint32_t)chunk_offsets.size());
        for (uint64_t o : chunk_offsets)
        {
            if (big) stbl.u64(o); else stbl.u32((uint32_t)o);
        }
        stbl.end(box);
    }

    void handler(Buf& b, const char* type, const char* name)
    {
        // hdlr 12 8.4.3.2: pre_defined(32) handler_type(32) reserved(32)[3] name (null-terminated UTF-8)
        const size_t box = b.beginFull("hdlr", 0, 0);
        b.u32(0);
        b.fourcc(type);
        b.zeros(12);
        b.bytes(name, std::strlen(name) + 1);
        b.end(box);
    }

    void dataInformation(Buf& b)
    {
        // dinf 12 8.7.1 / dref 12 8.7.2.2: entry_count 1, one 'url ' FullBox with flags 1 = media in this file
        const size_t dinf = b.begin("dinf");
        const size_t dref = b.beginFull("dref", 0, 0);
        b.u32(1);
        const size_t url = b.beginFull("url ", 0, 1);
        b.end(url);
        b.end(dref);
        b.end(dinf);
    }

    void mediaHeader(Buf& b, uint64_t now, uint32_t timescale, uint64_t duration)
    {
        // mdhd 12 8.4.2.2: version 1 when a field needs 64 bits; pad(1) + language (ISO-639-2/T, 3 x 5 bits,
        // each letter - 0x60): "und" = 0x55C4; pre_defined(16) 0
        const bool v1 = duration > 0xFFFFFFFFull || now > 0xFFFFFFFFull;
        const size_t box = b.beginFull("mdhd", v1 ? 1 : 0, 0);
        if (v1) { b.u64(now); b.u64(now); b.u32(timescale); b.u64(duration); }
        else { b.u32((uint32_t)now); b.u32((uint32_t)now); b.u32(timescale); b.u32((uint32_t)duration); }
        b.u16(0x55C4);
        b.u16(0);
        b.end(box);
    }

    void trackHeader(Buf& b, uint64_t now, uint32_t id, uint64_t duration, bool audio, uint32_t w, uint32_t h)
    {
        // tkhd 12 8.3.2.2: flags 3 = track_enabled | track_in_movie; layer 0, alternate_group 0, volume 0x0100 for
        // audio else 0; width/height 16.16 fixed point
        const bool v1 = duration > 0xFFFFFFFFull || now > 0xFFFFFFFFull;
        const size_t box = b.beginFull("tkhd", v1 ? 1 : 0, 3);
        if (v1) { b.u64(now); b.u64(now); b.u32(id); b.u32(0); b.u64(duration); }
        else { b.u32((uint32_t)now); b.u32((uint32_t)now); b.u32(id); b.u32(0); b.u32((uint32_t)duration); }
        b.zeros(8);
        b.u16(0);
        b.u16(0);
        b.u16(audio ? 0x0100 : 0);
        b.u16(0);
        unityMatrix(b);
        b.u32(w << 16);
        b.u32(h << 16);
        b.end(box);
    }

    void editList(Buf& b, uint64_t empty_duration, uint64_t segment_duration, int64_t media_time)
    {
        // edts 12 8.6.5 / elst 12 8.6.6.2: entry_count, then per entry segment_duration (movie timescale),
        // media_time (media timescale; -1 = an empty edit: nothing plays for segment_duration), media_rate_integer 1,
        // media_rate_fraction 0. An empty edit first when the track starts after the movie's time 0.
        const bool v1 = segment_duration > 0xFFFFFFFFull || empty_duration > 0xFFFFFFFFull || media_time > 0x7FFFFFFF;
        const size_t edts = b.begin("edts");
        const size_t elst = b.beginFull("elst", v1 ? 1 : 0, 0);
        b.u32(empty_duration ? 2 : 1);
        auto entry = [&](uint64_t dur, int64_t mt) {
            if (v1) { b.u64(dur); b.u64((uint64_t)mt); }
            else { b.u32((uint32_t)dur); b.u32((uint32_t)mt); }
            b.u16(1);
            b.u16(0);
        };
        if (empty_duration) entry(empty_duration, -1);
        entry(segment_duration, media_time);
        b.end(elst);
        b.end(edts);
    }
}


// ---------------------------------------------------------------------------------------------------------------
// [W112 RECORDER CRASH-SAFE 2026-11-25] Movie fragments
// ---------------------------------------------------------------------------------------------------------------
namespace
{
    // A fragment is closed at a video key frame once it holds 1 s of pictures (the encoders here make an IDR every
    // 2 s: wolfvr_openh264.cpp uiIntraPeriod = mFps * 2, wolfvrpipe_video.cpp gop-size, wolfvr_vth264.mm
    // MaxKeyFrameInterval = mFps * 2), and at any picture once it holds 4 s (dropped frames stretch a GOP counted in
    // pictures; an encoder that never sends a key frame still loses at most 4 s in a crash).
    constexpr uint64_t kFragmentMinTicks = kVideoTimescale * 1;
    constexpr uint64_t kFragmentMaxTicks = kVideoTimescale * 4;

    // 12 8.8.3.1 sample_flags: reserved(4) is_leading(2) sample_depends_on(2) sample_is_depended_on(2)
    // sample_has_redundancy(2) sample_padding_value(3) sample_is_non_sync_sample(1) sample_degradation_priority(16).
    // sample_depends_on 2 = does not depend on others (an I picture), 1 = depends on others. The same words as
    // FFmpeg's isom.h MOV_FRAG_SAMPLE_FLAG_DEPENDS_NO 0x02000000, DEPENDS_YES 0x01000000, IS_NON_SYNC 0x00010000.
    constexpr uint32_t kFlagsSync = 0x02000000u;
    constexpr uint32_t kFlagsNonSync = 0x01000000u | 0x00010000u;

    void put32(uint8_t* p, uint32_t v)
    {
        p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
    }
    void put64(uint8_t* p, uint64_t v)
    {
        put32(p, (uint32_t)(v >> 32));
        put32(p + 4, (uint32_t)v);
    }
    // a box header of 12 4.2 with a 64-bit size: size 1, type, largesize
    void largeHeader(uint8_t* p, const char* type, uint64_t size)
    {
        put32(p, 1);
        std::memcpy(p + 4, type, 4);
        put64(p + 8, size);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// File I/O
// ---------------------------------------------------------------------------------------------------------------
std::string Mp4Writer::ioError(int err)
{
    // The C runtime maps the system's errors to errno on every platform (on Windows ERROR_DISK_FULL / ERROR_HANDLE_DISK_FULL
    // become ENOSPC and ERROR_ACCESS_DENIED EACCES: Microsoft "errno, _doserrno, _sys_errlist, and _sys_nerr").
    switch (err)
    {
        case ENOSPC: return "The disk is full.";
#if defined(EDQUOT)
        case EDQUOT: return "The disk is full (your disk quota is used up).";
#endif
        case EACCES:
        case EPERM: return "WolfViewer is not allowed to write to that folder or file (permission denied).";
        case EROFS: return "That disk is read-only.";
        case ENOENT: return "That folder does not exist.";
        case EFBIG: return "The file is too big for that disk's file system.";
        default: break;
    }
    const char* s = err ? std::strerror(err) : nullptr;
    return std::string("The video file could not be written") + (s ? std::string(" (") + s + ")." : ".");
}

Mp4Writer::~Mp4Writer()
{
    closeFile();
}

void Mp4Writer::closeFile()
{
    if (mFile) std::fclose(mFile);
    mFile = nullptr;
}

bool Mp4Writer::writeAt(uint64_t pos, const void* data, size_t len, std::string& error)
{
    if (!mFile) { error = "The video file is not open."; return false; }
    errno = 0;
#if defined(_WIN32)
    const int seek = _fseeki64(mFile, (__int64)pos, SEEK_SET);
#else
    const int seek = fseeko(mFile, (off_t)pos, SEEK_SET);
#endif
    if (seek != 0) { error = ioError(errno); return false; }
    errno = 0;
    // The stream is unbuffered (open: setvbuf _IONBF), so a short count here is the system's own answer for these
    // bytes and errno says why; nothing is left in a buffer to be written later.
    if (len && std::fwrite(data, 1, len, mFile) != len)
    {
        error = ioError(errno ? errno : EIO);
        std::clearerr(mFile);
        return false;
    }
    return true;
}

bool Mp4Writer::syncFile(std::string& error)
{
    // Hands what the system holds for this file to the disk before finish() switches the file to its final form, so
    // a power cut cannot leave the switch on the disk without what it points at. Microsoft "_commit" (flushes a file
    // directly to disk; sets errno), POSIX fsync. The stream itself is unbuffered (open).
    errno = 0;
#if defined(_WIN32)
    const bool ok = _commit(_fileno(mFile)) == 0;
#else
    const bool ok = fsync(fileno(mFile)) == 0;
#endif
    if (!ok) error = ioError(errno ? errno : EIO);
    return ok;
}

bool Mp4Writer::open(const std::string& path_utf8, std::string& error)
{
    closeFile();
    mPath = path_utf8;
    errno = 0;
#if defined(_WIN32)
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path_utf8.data(), (int)path_utf8.size(), nullptr, 0);
    if (n <= 0) { error = "That file name is not valid."; return false; }
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path_utf8.data(), (int)path_utf8.size(), &w[0], n);
    mFile = _wfopen(w.c_str(), L"wb");   // _wfopen sets errno (Microsoft "fopen, _wfopen")
#else
    mFile = std::fopen(path_utf8.c_str(), "wb");
#endif
    if (!mFile) { error = ioError(errno); return false; }
    std::setvbuf(mFile, nullptr, _IONBF, 0);

    Buf head;
    // ftyp 12 4.3.2: major_brand 'isom', minor_version 0x200, compatible brands isom, iso6, iso2, avc1, mp41 - what
    // FFmpeg's mp4 muxer writes for H.264 when it fragments (byte for byte what ffmpeg -movflags
    // frag_keyframe+empty_moov makes; libavformat/movenc.c n9.0.2 mov_write_ftyp_tag: "We add tfdt atoms when
    // fragmenting, signal this with the iso6 compatible brand"), and what OBS's hybrid MP4 writes while it
    // fragments (plugins/obs-outputs/mp4-mux.c mp4_write_ftyp). 'iso2' also covers Opus's roll groups (Opus 4.1).
    // The same ftyp serves the finished, unfragmented file (FFmpeg's hybrid_fragmented keeps it too).
    // No 'iso5': the fragments give each track's data offset in full instead of counting from the moof
    // (movenc.c mov_write_ftyp_tag_internal: 'iso5' is "Required when using default-base-is-moof").
    // No 'dash': this is one file, not a DASH segment (FFmpeg writes 'dash' only for its DASH output with a
    // global sidx).
    const size_t ftyp = head.begin("ftyp");
    head.fourcc("isom");
    head.u32(0x200);
    head.fourcc("isom");
    head.fourcc("iso6");
    head.fourcc("iso2");
    head.fourcc("avc1");
    head.fourcc("mp41");
    head.end(ftyp);
    // free 12 8.1.2: the space finish() writes the full moov into. Zeros, so the disk blocks are taken now: a full
    // disk later cannot stop the movie box being written here (on a copy-on-write file system it can: finish()
    // then drops fragments from the end to free space).
    // mReserveSize (default 4 MB, wolfrec_mp4.h): moov costs about 20 bytes per sample (stsz 4, stts/stsc/stco
    // runs, stss), so 4 MB holds an hour at 60 fps with AAC; a longer recording gets moov after the fragments.
    if (mReserveSize > 0xFFFFFFFFull) mReserveSize = 0xFFFFFFFFull;
    mReserveStart = head.b.size();
    head.u32((uint32_t)mReserveSize);
    head.fourcc("free");
    head.zeros((size_t)mReserveSize - 8);
    if (!writeAt(0, head.b.data(), head.b.size(), error))
    {
        closeFile();
        return false;
    }
    mEnd = head.b.size();
    mFailed = false;
    mFailure.clear();
    mSampleBytes = 0;
    mInitWritten = false;
    mInitEntryType = 0;
    mInitAvc3 = false;
    mSequence = 0;
    mFragments.clear();
    mVideo = Track();
    mVideo.timescale = kVideoTimescale;
    mAudio = Track();
    if (mHasAudio) mAudio.timescale = mAudioOpus ? 48000 : mAudioRate;
    mSps.clear();
    mPps.clear();
    mInBandParams = false;
    mHaveVideo = false;
    mFirstPts = mLastPts = 0;
    mLastTick = 0;
    mFirstAudioFrames = 0;
    mInitRoll = false;
    return true;
}

void Mp4Writer::setVideo(uint32_t width, uint32_t height)
{
    mWidth = width;
    mHeight = height;
}

void Mp4Writer::setAudio(const AudioEncoder& enc)
{
    mAudioEnc = &enc;
    mHasAudio = true;
    mAudioOpus = enc.codec() == "opus";
    mAudioRate = enc.sampleRate();
    mAudioChannels = enc.channels();
    mAudioBitrate = enc.bitrate();
    mAudioPriming = enc.primingFrames();
    mAudioConfig = enc.decoderConfig();
    mAudio.timescale = mAudioOpus ? 48000 : mAudioRate;   // Opus 4.4: "typically set to 48000"; AAC: its rate
}

void Mp4Writer::addPending(Track& t, const std::vector<uint8_t>& data, uint32_t duration, bool sync)
{
    Sample s;
    s.offset = t.pending_bytes.size();
    s.size = (uint32_t)data.size();
    s.duration = duration;
    s.sync = sync;
    t.pending.push_back(s);
    t.pending_bytes.insert(t.pending_bytes.end(), data.begin(), data.end());
    mSampleBytes += data.size();
}

bool Mp4Writer::writeVideo(const std::vector<uint8_t>& au, int64_t pts_us, bool key, std::string& error)
{
    if (!mFile) { error = "The video file is not open."; return false; }
    if (mFailed) { error = mFailure; return false; }
    std::vector<uint8_t> sps, pps;
    std::vector<uint8_t> sample;
    sample.reserve(au.size() + 16);
    bool params_kept = false;
    // 15 5.3.3.1 (avc1): parameter sets go in the sample entry's avcC; samples are NAL units each preceded by its
    // length (lengthSizeMinusOne = 3: 4-byte big-endian lengths, 15 5.3.3.1.2). Every other NAL unit is kept.
    // A later SPS/PPS that differs from the stored one stays in the sample, and the entry becomes 'avc3' (15, 3rd
    // edition: the 'avc3' entry is laid out as 'avc1' but parameter sets may also be carried in the samples).
    // The recorder keeps one encoder setting for the whole recording, so this is the exception, not the rule.
    forEachNal(au, [&](int type, const uint8_t* p, size_t n) {
        if (type == 7 || type == 8)
        {
            std::vector<uint8_t>& dst = type == 7 ? sps : pps;
            if (dst.empty()) dst.assign(p, p + n);
            const std::vector<uint8_t>& stored = type == 7 ? mSps : mPps;
            if (stored.empty() || (stored.size() == n && std::memcmp(stored.data(), p, n) == 0)) return;
            params_kept = true;
        }
        const uint32_t len = (uint32_t)n;
        sample.push_back((uint8_t)(len >> 24)); sample.push_back((uint8_t)(len >> 16));
        sample.push_back((uint8_t)(len >> 8)); sample.push_back((uint8_t)len);
        sample.insert(sample.end(), p, p + n);
    });
    AvcSpsInfo first_info;
    if (!mHaveVideo)
    {
        if (pts_us < 0)
        {
            error = "A video picture's time was before the start of the recording.";
            return false;
        }
        if (!key || sps.empty() || pps.empty())
        {
            error = "The first video picture is not a key frame with its SPS and PPS.";
            return false;
        }
        if (!parseAvcSps(sps.data(), sps.size(), first_info))
        {
            error = "The video's sequence parameter set could not be read.";
            return false;
        }
    }
    else if (pts_us <= mLastPts)
    {
        error = "A video picture's time did not rise; it was not written.";
        return false;
    }
    if (sample.empty()) { error = "The video picture had no slices."; return false; }
    if (sample.size() > 0xFFFFFFFFull) { error = "An oversized video picture was refused."; return false; }
    // ticks from the absolute time since the first picture: durations never drift from the real clock
    const int64_t first_pts = mHaveVideo ? mFirstPts : pts_us;
    const uint64_t rel = (uint64_t)(pts_us - first_pts);
    const uint64_t tick = (rel * kVideoTimescale + 500000) / 1000000;
    if (mHaveVideo)
    {
        if (tick <= mLastTick)
        {
            error = "Two video pictures were closer than one 90 kHz tick; the later one was not written.";
            return false;
        }
        // The picture before this one now has its duration. It is always waiting for its fragment: a fragment is
        // only written just before a new picture is taken (below), or by finish().
        mVideo.pending.back().duration = (uint32_t)std::min<uint64_t>(tick - mLastTick, 0xFFFFFFFFull);
        // the waiting pictures start at the track's decode time (the sum of the written durations) and end here
        const uint64_t span = tick - mVideo.decode_time;
        if ((key && span >= kFragmentMinTicks) || span >= kFragmentMaxTicks)
        {
            if (!flushFragment(error)) return false;
        }
    }
    else
    {
        mSps = sps;
        mPps = pps;
        mSpsInfo = first_info;
        mFirstPts = pts_us;
        mHaveVideo = true;
    }
    addPending(mVideo, sample, 0, key);
    mLastTick = tick;
    mLastPts = pts_us;
    if (params_kept) mInBandParams = true;
    return true;
}

bool Mp4Writer::writeAudio(const AudioPacket& pkt, std::string& error)
{
    if (!mFile) { error = "The video file is not open."; return false; }
    if (!mHasAudio) { error = "No sound track was set up."; return false; }
    if (mFailed) { error = mFailure; return false; }
    if (pkt.frames == 0) { error = "An audio packet without duration was refused."; return false; }
    if (pkt.data.empty() || pkt.data.size() > 0xFFFFFFFFull)
    {
        error = "An empty or oversized sample was refused.";
        return false;
    }
    if (mFirstAudioFrames == 0) mFirstAudioFrames = pkt.frames;
    // every AAC and Opus packet can start decoding (Opus 4.3.6.1: every Opus sample is a sync sample; AAC frames
    // are independent apart from the overlap the priming / roll handles)
    addPending(mAudio, pkt.data, pkt.frames, true);
    // A file without pictures (setVideo never called) is cut into fragments by the sound alone, every second. With
    // pictures, writeVideo() cuts them (the sound waiting then goes into the same fragment).
    if (mWidth == 0 && !mHaveVideo)
    {
        uint64_t span = 0;
        for (const Sample& s : mAudio.pending) span += s.duration;
        if (span >= mAudio.timescale) return flushFragment(error);
    }
    return true;
}

uint32_t Mp4Writer::movieTimescale(bool initial) const
{
    // Opus 4.4: movie timescale = media timescale; the same for AAC (the edit list's units are then exact samples)
    const bool audio = initial ? mHasAudio : !mAudio.samples.empty();
    return audio ? mAudio.timescale : kVideoTimescale;
}

bool Mp4Writer::flushFragment(std::string& error)
{
    if (mFailed) { error = mFailure; return false; }
    if (mVideo.pending.empty() && mAudio.pending.empty()) return true;

    Buf out;
    size_t entry_type_at = 0, mehd_at = 0;
    if (!mInitWritten)
    {
        // the moov with no samples goes in front of the first fragment (12 8.8.1: a movie with fragments has an
        // mvex in its moov)
        const std::vector<uint8_t> init = buildMoov(movieTimescale(true), true, &entry_type_at, &mehd_at);
        out.bytes(init.data(), init.size());
    }
    const bool roll = mInitWritten ? mInitRoll : (mAudioOpus && mHasAudio && mFirstAudioFrames != 0);
    const uint64_t moof_at = mEnd + out.b.size();
    const uint64_t data_bytes = mVideo.pending_bytes.size() + mAudio.pending_bytes.size();
    // mdat 12 8.1.1 / 12 4.2: an 8-byte header, or 16 (size 1 + largesize) when the box would reach 4 GB
    const uint64_t mdat_header = data_bytes + 8 > 0xFFFFFFFFull ? 16 : 8;

    // moof 12 8.8.4: mfhd, then one traf per track with samples waiting (video first, then the sound - the order
    // of their data in the mdat)
    auto buildMoof = [&](uint64_t data_start) {
        Buf m;
        const size_t moof = m.begin("moof");
        // mfhd 12 8.8.5.2: sequence_number, 1 for the first fragment and rising by one with each
        const size_t mfhd = m.beginFull("mfhd", 0, 0);
        m.u32(mSequence + 1);
        m.end(mfhd);
        uint64_t base = data_start;
        for (int i = 0; i < 2; ++i)
        {
            const bool video = i == 0;
            const Track& t = video ? mVideo : mAudio;
            if (t.pending.empty()) continue;
            const uint32_t track_id = video ? 1u : (mHaveVideo ? 2u : 1u);   // as buildMoov numbers them
            const size_t traf = m.begin("traf");
            // tfhd 12 8.8.7.2: flags 0x000001 base-data-offset-present (base_data_offset = the file offset of the
            // track's first sample here, as FFmpeg's and OBS's fragmented MP4 write it), 0x000020
            // default-sample-flags-present (video: a picture that depends on others; sound: every packet a sync
            // sample)
            const size_t tfhd = m.beginFull("tfhd", 0, 0x000001 | 0x000020);
            m.u32(track_id);
            m.u64(base);
            m.u32(video ? kFlagsNonSync : kFlagsSync);
            m.end(tfhd);
            // tfdt 12 8.8.12.2, version 1: baseMediaDecodeTime (64 bits) = the sum of the durations of every
            // earlier sample of the track, so each fragment's time stands on its own
            const size_t tfdt = m.beginFull("tfdt", 1, 0);
            m.u64(t.decode_time);
            m.end(tfdt);
            // trun 12 8.8.8.2: flags 0x000001 data-offset-present, 0x000100 sample-duration-present, 0x000200
            // sample-size-present, and for video 0x000400 sample-flags-present (the pictures' own sync flags: a
            // fragment cut at 4 s can start between key frames, and an encoder may add an IDR anywhere);
            // sample_count, data_offset (signed 32 bits, from base_data_offset: 0), then per sample duration,
            // size [, flags]
            const uint32_t flags = 0x000001 | 0x000100 | 0x000200 | (video ? 0x000400u : 0u);
            const size_t trun = m.beginFull("trun", 0, flags);
            m.u32((uint32_t)t.pending.size());
            m.u32(0);
            for (const Sample& s : t.pending)
            {
                m.u32(s.duration);
                m.u32(s.size);
                if (video) m.u32(s.sync ? kFlagsSync : kFlagsNonSync);
            }
            m.end(trun);
            if (!video && roll)
            {
                // Opus 4.3.6.2 roll group (see buildMoov). sbgp 12 8.9.2.2 in a traf: group_description_index 1
                // is the first entry of the track's own sgpd in the moov (12 8.9.4: indexes from 0x10001 would
                // mean an sgpd in this fragment; Chromium media/formats/mp4/track_run_iterator.cc quotes it)
                const size_t sbgp = m.beginFull("sbgp", 0, 0);
                m.fourcc("roll");
                m.u32(1);
                m.u32((uint32_t)t.pending.size());
                m.u32(1);
                m.end(sbgp);
            }
            m.end(traf);
            base += t.pending_bytes.size();
        }
        m.end(moof);
        return m;
    };
    const uint64_t moof_size = buildMoof(0).b.size();   // the same size whatever the offsets
    const uint64_t data_start = moof_at + moof_size + mdat_header;
    out.append(buildMoof(data_start));
    if (mdat_header == 16)
    {
        uint8_t h[16];
        largeHeader(h, "mdat", data_bytes + 16);
        out.bytes(h, 16);
    }
    else
    {
        out.u32((uint32_t)(data_bytes + 8));
        out.fourcc("mdat");
    }
    out.bytes(mVideo.pending_bytes.data(), mVideo.pending_bytes.size());
    out.bytes(mAudio.pending_bytes.data(), mAudio.pending_bytes.size());

    // The moov with no samples was written as 'avc1' and a picture now carries a different SPS/PPS: its entry
    // becomes 'avc3' (see writeVideo) before that picture's fragment is written. 4 bytes in place.
    if (mInitWritten && mInBandParams && !mInitAvc3)
    {
        if (!writeAt(mInitEntryType, "avc3", 4, error))
        {
            mFailed = true;
            mFailure = error;
            return false;
        }
        mInitAvc3 = true;
    }

    // Crash safety: everything after the fragment's first 8 bytes is written first, then those 8 bytes (the size and
    // type of its first box). Until they are written, the file ends with a box of size 0 ("the last one in the
    // file, extends to the end of the file", 12 4.2) and type 0 - the 8 bytes at mEnd are still zeros, as the
    // write past the end of the file left them - which readers skip, so a recording killed during this write
    // plays up to the fragment before. (mEnd is the end of the file here: nothing is ever written after an
    // unfinished fragment.)
    if (!writeAt(mEnd + 8, out.b.data() + 8, out.b.size() - 8, error) || !writeAt(mEnd, out.b.data(), 8, error))
    {
        mFailed = true;
        mFailure = error;
        return false;
    }

    if (!mInitWritten)
    {
        mInitWritten = true;
        mInitEntryType = mEnd + entry_type_at;
        mMehdAt = mEnd + mehd_at;
        mInitAvc3 = mInBandParams;
        mInitRoll = roll;
    }
    Fragment f;
    f.start = mEnd;
    f.video_samples = mVideo.samples.size();
    f.audio_samples = mAudio.samples.size();
    mFragments.push_back(f);
    uint64_t base = data_start;
    for (Track* t : { &mVideo, &mAudio })
    {
        for (Sample s : t->pending)
        {
            s.offset += base;
            t->decode_time += s.duration;
            t->samples.push_back(s);
        }
        base += t->pending_bytes.size();
        t->pending.clear();
        t->pending_bytes.clear();
    }
    mEnd += out.b.size();
    ++mSequence;
    // The movie's length so far in the moov with no samples' mehd, after the fragment it counts is in the file (so
    // it never says more than is there). GStreamer's qtdemux takes a fragmented file's length from it
    // (qtdemux.c qtdemux_parse_mehd -> qtdemux->duration); without it, it shows the first fragment's.
    uint8_t dur[8];
    put64(dur, fragmentedDuration());
    if (!writeAt(mMehdAt, dur, 8, error))
    {
        mFailed = true;
        mFailure = error;
        return false;
    }
    return true;
}

uint64_t Mp4Writer::fragmentedDuration() const
{
    // as buildMoov's mvhd: the pictures from their start (empty edit) to the end of the last written one, the sound
    // without its priming (the edits), on the movie's clock of the moov with no samples
    const uint32_t ts = movieTimescale(true);
    const uint64_t v = mHaveVideo ? rescale((uint64_t)mFirstPts, 1000000, ts) +
                                        rescale(mVideo.decode_time, kVideoTimescale, ts)
                                  : 0;
    const uint64_t a_media = mAudio.decode_time > mAudioPriming ? mAudio.decode_time - mAudioPriming : 0;
    const uint64_t a = mHasAudio && mAudio.timescale ? rescale(a_media, mAudio.timescale, ts) : 0;
    return std::max(v, a);
}

// ---------------------------------------------------------------------------------------------------------------
// moov
// ---------------------------------------------------------------------------------------------------------------
std::vector<uint8_t> Mp4Writer::buildMoov(uint32_t movie_ts, bool initial, size_t* entry_type_at, size_t* mehd_at) const
{
    // initial: the moov with no samples that starts the fragmented file (12 8.8.1): every duration 0 (12 8.8.1:
    // the movie's samples are in the fragments; FFmpeg and OBS write 0 too), empty sample tables, an mvex.
    // Otherwise the full moov of the finished file, every sample in its tables.
    const uint64_t now = mp4Now();
    Buf m;
    const size_t moov = m.begin("moov");
    const bool has_video = initial ? mHaveVideo : !mVideo.samples.empty();
    const bool has_audio = initial ? mHasAudio : !mAudio.samples.empty();

    // durations
    uint64_t v_media = 0;
    if (!initial) for (const Sample& s : mVideo.samples) v_media += s.duration;
    uint64_t a_media = 0;
    if (!initial) for (const Sample& s : mAudio.samples) a_media += s.duration;
    const uint64_t v_movie = rescale(v_media, kVideoTimescale, movie_ts);
    // the first picture's time on the movie's clock (the sound starts at 0); normally 0
    const uint64_t v_start = has_video ? rescale((uint64_t)mFirstPts, 1000000, movie_ts) : 0;
    // Audio: the edit starts after the priming and lasts the valid samples (Opus 4.4: segment_duration = valid
    // samples, media_time = priming samples; the same edit trims AAC's encoder delay, 12 8.6.6.1)
    uint64_t a_valid = 0;
    if (!initial && has_audio)
    {
        const uint64_t coded = a_media > mAudioPriming ? a_media - mAudioPriming : 0;
        a_valid = mAudioEnc ? std::min<uint64_t>(mAudioEnc->inputFrames(), coded) : coded;
        // fragments dropped at the end (a full disk) shorten the edit to what is there
    }
    const uint64_t a_movie = has_audio ? rescale(a_valid, mAudio.timescale, movie_ts) : 0;
    // In the moov with no samples the edits only place the media: segment_duration 0. ISO/IEC 14496-12:2012
    // elst semantics, quoted in W3C bug 26066: "Particularly in an empty initial movie of a fragmented movie file
    // (when there are no media samples yet present), the segment_duration of this edit may be zero", the edit then
    // giving the offset from media composition time to movie presentation time. FFmpeg (movenc.c
    // mov_write_edts_tag: "For fragmented files ... Setting duration to 0 allows us to only specify the offset") and
    // OBS write the same. FFmpeg's reader applies such an edit to every fragment (mov.c mov_build_index ->
    // sc->time_offset, subtracted in mov_read_trun from each tfdt).

    // mvhd 12 8.2.2.2: rate 1.0, volume 1.0, reserved, matrix, pre_defined[6], next_track_ID
    {
        const uint64_t dur = initial ? 0 : std::max(v_start + v_movie, a_movie);
        const bool v1 = dur > 0xFFFFFFFFull || now > 0xFFFFFFFFull;
        const size_t box = m.beginFull("mvhd", v1 ? 1 : 0, 0);
        if (v1) { m.u64(now); m.u64(now); m.u32(movie_ts); m.u64(dur); }
        else { m.u32((uint32_t)now); m.u32((uint32_t)now); m.u32(movie_ts); m.u32((uint32_t)dur); }
        m.u32(0x00010000);
        m.u16(0x0100);
        m.zeros(2 + 8);
        unityMatrix(m);
        m.zeros(24);
        m.u32(3);
        m.end(box);
    }

    uint32_t track_id = 1;
    if (has_video)
    {
        const uint32_t w = mSpsInfo.width ? mSpsInfo.width : mWidth;
        const uint32_t h = mSpsInfo.height ? mSpsInfo.height : mHeight;
        const size_t trak = m.begin("trak");
        trackHeader(m, now, track_id++, initial ? 0 : v_start + v_movie, false, w, h);
        editList(m, v_start, initial ? 0 : v_movie, 0);
        const size_t mdia = m.begin("mdia");
        mediaHeader(m, now, kVideoTimescale, v_media);
        handler(m, "vide", "WolfViewer video");
        const size_t minf = m.begin("minf");
        {
            // vmhd 12 12.1.2.2: version 0, flags 1, graphicsmode 0 (copy), opcolor 0,0,0
            const size_t box = m.beginFull("vmhd", 0, 1);
            m.u16(0);
            m.zeros(6);
            m.end(box);
        }
        dataInformation(m);
        Buf stbl;
        const size_t stbl_box = stbl.begin("stbl");
        size_t entry = 0;
        {
            // stsd 12 8.5.2.2: entry_count 1
            const size_t stsd = stbl.beginFull("stsd", 0, 0);
            stbl.u32(1);
            // VisualSampleEntry 12 12.1.3.2: SampleEntry (reserved[6], data_reference_index 1), pre_defined(16),
            // reserved(16), pre_defined(32)[3], width(16), height(16), horizresolution 0x00480000 (72 dpi),
            // vertresolution 0x00480000, reserved(32), frame_count 1, compressorname[32] (Pascal string),
            // depth 0x0018, pre_defined(16) -1
            entry = stbl.begin(mInBandParams ? "avc3" : "avc1");
            stbl.zeros(6);
            stbl.u16(1);
            stbl.u16(0);
            stbl.u16(0);
            stbl.zeros(12);
            stbl.u16(w);
            stbl.u16(h);
            stbl.u32(0x00480000);
            stbl.u32(0x00480000);
            stbl.u32(0);
            stbl.u16(1);
            uint8_t compressor[32] = {0};
            const char* cname = "WolfViewer H.264";
            compressor[0] = (uint8_t)std::strlen(cname);
            std::memcpy(compressor + 1, cname, std::strlen(cname));
            stbl.bytes(compressor, 32);
            stbl.u16(0x0018);
            stbl.u16(0xFFFF);
            {
                // avcC 15 5.3.3.1.2 AVCDecoderConfigurationRecord: configurationVersion 1, AVCProfileIndication,
                // profile_compatibility, AVCLevelIndication (the SPS's three bytes after its header), 6 bits
                // reserved '1' + lengthSizeMinusOne (3), 3 bits reserved '1' + numOfSequenceParameterSets,
                // { 16-bit length, SPS }, numOfPictureParameterSets, { 16-bit length, PPS }, and for profile_idc
                // 100, 110, 122 or 144: 6 bits '1' + chroma_format, 5 bits '1' + bit_depth_luma_minus8,
                // 5 bits '1' + bit_depth_chroma_minus8, numOfSequenceParameterSetExt (0)
                const size_t avcc = stbl.begin("avcC");
                stbl.u8(1);
                stbl.u8(mSps[1]);
                stbl.u8(mSps[2]);
                stbl.u8(mSps[3]);
                stbl.u8(0xFC | 3);
                stbl.u8(0xE0 | 1);
                stbl.u16((uint32_t)mSps.size());
                stbl.bytes(mSps.data(), mSps.size());
                stbl.u8(1);
                stbl.u16((uint32_t)mPps.size());
                stbl.bytes(mPps.data(), mPps.size());
                const uint8_t p = mSpsInfo.profile_idc;
                if (p == 100 || p == 110 || p == 122 || p == 144)
                {
                    stbl.u8(0xFC | (mSpsInfo.chroma_format_idc & 3));
                    stbl.u8(0xF8 | (mSpsInfo.bit_depth_luma_minus8 & 7));
                    stbl.u8(0xF8 | (mSpsInfo.bit_depth_chroma_minus8 & 7));
                    stbl.u8(0);
                }
                stbl.end(avcc);
            }
            {
                // colr 12 12.1.5.2: colour_type 'nclx', colour_primaries(16), transfer_characteristics(16),
                // matrix_coefficients(16), full_range_flag(1) + reserved(7) - the code points of ITU-T H.273, the
                // same numbers as the H.264 VUI. From the SPS when it says them; otherwise what every encoder here
                // is set up for: BT.709 (1, 1, 1), limited range.
                const size_t colr = stbl.begin("colr");
                stbl.fourcc("nclx");
                stbl.u16(mSpsInfo.has_colour ? mSpsInfo.colour_primaries : 1);
                stbl.u16(mSpsInfo.has_colour ? mSpsInfo.transfer_characteristics : 1);
                stbl.u16(mSpsInfo.has_colour ? mSpsInfo.matrix_coefficients : 1);
                stbl.u8(mSpsInfo.has_colour && mSpsInfo.full_range ? 0x80 : 0);
                stbl.end(colr);
            }
            stbl.end(entry);
            stbl.end(stsd);
        }
        std::vector<std::pair<uint64_t, uint32_t>> at;
        std::vector<uint32_t> durations;
        std::vector<uint32_t> sync;
        if (!initial)
        {
            for (size_t i = 0; i < mVideo.samples.size(); ++i)
            {
                const Sample& s = mVideo.samples[i];
                at.emplace_back(s.offset, s.size);
                durations.push_back(s.duration);
                if (s.sync) sync.push_back((uint32_t)i + 1);
            }
        }
        // no stss in the moov with no samples: the fragments flag their own sync samples (trun sample flags)
        sampleTables(stbl, at, durations, initial || sync.size() == mVideo.samples.size() ? nullptr : &sync);
        stbl.end(stbl_box);
        // where the sample entry's type sits in the moov: the stbl's bytes start here (+4: after its size)
        if (entry_type_at) *entry_type_at = m.b.size() + entry + 4;
        m.append(stbl);
        m.end(minf);
        m.end(mdia);
        m.end(trak);
    }

    if (has_audio)
    {
        const size_t trak = m.begin("trak");
        trackHeader(m, now, track_id++, a_movie, true, 0, 0);
        editList(m, 0, a_movie, (int64_t)mAudioPriming);
        const size_t mdia = m.begin("mdia");
        mediaHeader(m, now, mAudio.timescale, a_media);
        handler(m, "soun", "WolfViewer sound");
        const size_t minf = m.begin("minf");
        {
            // smhd 12 12.2.2.2: balance 0, reserved
            const size_t box = m.beginFull("smhd", 0, 0);
            m.u16(0);
            m.u16(0);
            m.end(box);
        }
        dataInformation(m);
        Buf stbl;
        const size_t stbl_box = stbl.begin("stbl");
        {
            const size_t stsd = stbl.beginFull("stsd", 0, 0);
            stbl.u32(1);
            // AudioSampleEntry 12 12.2.3.2 (version 0): SampleEntry (reserved[6], data_reference_index 1),
            // reserved(32)[2], channelcount, samplesize 16, pre_defined(16), reserved(16), samplerate 16.16.
            // Opus 4.3.1: samplesize 16, samplerate 48000<<16.
            const size_t entry = stbl.begin(mAudioOpus ? "Opus" : "mp4a");
            stbl.zeros(6);
            stbl.u16(1);
            stbl.zeros(8);
            stbl.u16(mAudioChannels);
            stbl.u16(16);
            stbl.u16(0);
            stbl.u16(0);
            stbl.u32((mAudioOpus ? 48000u : mAudioRate) << 16);
            if (mAudioOpus)
            {
                // dOps Opus 4.3.2: Version 0, OutputChannelCount, PreSkip(16), InputSampleRate(32), OutputGain
                // (16, signed 8.8), ChannelMappingFamily (0: mono/stereo, no table). PreSkip = the encoder's
                // look-ahead, as in Opus 4.7's example (PreSkip 312 = elst media_time 312).
                const size_t dops = stbl.begin("dOps");
                stbl.u8(0);
                stbl.u8(mAudioChannels);
                stbl.u16(mAudioPriming);
                stbl.u32(mAudioRate);
                stbl.u16(0);
                stbl.u8(0);
                stbl.end(dops);
            }
            else
            {
                // bit rates for the DecoderConfigDescriptor: average over the track, peak over any 1 s. In the moov
                // with no samples neither is known: both the encoder's bit rate and bufferSizeDB 0, as FFmpeg
                // writes for a fragmented file (movenc.c calculate_mpeg4_bit_rates: "if the average bit rate cannot
                // be calculated at this point, such as in the case of fragmented MP4", the codec's bit rate;
                // max_bit_rate = FFMAX(bit_rate, avg); buffer_size 0 without CPB properties)
                uint64_t total = 0;
                uint32_t largest = 0;
                uint64_t peak = 0, window = 0, window_frames = 0;
                if (!initial)
                {
                    for (const Sample& s : mAudio.samples) { total += s.size; largest = std::max(largest, s.size); }
                    for (size_t i = 0, j = 0; i < mAudio.samples.size(); ++i)
                    {
                        window += mAudio.samples[i].size;
                        window_frames += mAudio.samples[i].duration;
                        while (window_frames > mAudio.timescale && j < i)
                        {
                            window -= mAudio.samples[j].size;
                            window_frames -= mAudio.samples[j].duration;
                            ++j;
                        }
                        peak = std::max(peak, window);
                    }
                }
                const uint64_t avg = a_media ? total * 8 * mAudio.timescale / a_media : mAudioBitrate;
                const uint64_t max_rate = initial ? mAudioBitrate : peak * 8;
                // esds 14 5.6.1: FullBox(version 0, flags 0) + ES_Descriptor
                const size_t esds = stbl.beginFull("esds", 0, 0);
                Buf dsi;   // 1 7.2.6.7 DecoderSpecificInfo (tag 0x05): the AudioSpecificConfig
                dsi.bytes(mAudioConfig.data(), mAudioConfig.size());
                Buf dcd;   // 1 7.2.6.6.1 DecoderConfigDescriptor (tag 0x04)
                dcd.u8(0x40);   // objectTypeIndication 0x40 = Audio ISO/IEC 14496-3 (1 Table 5)
                dcd.u8((0x05 << 2) | 1);   // streamType 0x05 AudioStream (1 Table 6), upStream 0, reserved 1
                dcd.u24(largest);   // bufferSizeDB
                dcd.u32((uint32_t)std::min<uint64_t>(max_rate, 0xFFFFFFFFull));   // maxBitrate
                dcd.u32((uint32_t)std::min<uint64_t>(avg, 0xFFFFFFFFull));        // avgBitrate
                descriptor(dcd, 0x05, dsi);
                Buf sl;   // 1 7.3.2.3.1 SLConfigDescriptor (tag 0x06): predefined 2 (14 3.1.2: "reserved for use
                sl.u8(2); //  in MP4 files")
                Buf es;   // 1 7.2.6.5.1 ES_Descriptor (tag 0x03): ES_ID(16) 0, streamDependenceFlag, URL_Flag,
                es.u16(0);//  OCRstreamFlag 0, streamPriority(5) 0; then DecoderConfigDescriptor, SLConfigDescriptor
                es.u8(0);
                descriptor(es, 0x04, dcd);
                descriptor(es, 0x06, sl);
                descriptor(stbl, 0x03, es);
                stbl.end(esds);
            }
            stbl.end(entry);
            stbl.end(stsd);
        }
        std::vector<std::pair<uint64_t, uint32_t>> at;
        std::vector<uint32_t> durations;
        if (!initial)
        {
            for (const Sample& s : mAudio.samples)
            {
                at.emplace_back(s.offset, s.size);
                durations.push_back(s.duration);
            }
        }
        sampleTables(stbl, at, durations, nullptr);   // every audio sample is a sync sample: no stss
        // the first packet's length: from the samples, or (moov with no samples) the first packet taken
        const uint32_t first_frames = !initial ? mAudio.samples.front().duration : mFirstAudioFrames;
        if (mAudioOpus && first_frames != 0)
        {
            // Opus 4.3.6.2: at least 80 ms of pre-roll after a random access, signalled by a 'roll' sample group
            // (AudioRollRecoveryEntry, 12 10.1.1.2: roll_distance(16, signed), in samples - here packets - and
            // negative). 80 ms = 3840 samples at 48 kHz; packets of `d` samples -> ceil(3840 / d).
            const uint32_t d = first_frames;
            const int16_t roll = (int16_t)-(int32_t)((3840 + d - 1) / d);
            // sgpd 12 8.9.3.2 version 1: grouping_type, default_length 2, entry_count 1, roll_distance
            const size_t sgpd = stbl.beginFull("sgpd", 1, 0);
            stbl.fourcc("roll");
            stbl.u32(2);
            stbl.u32(1);
            stbl.u16((uint16_t)roll);
            stbl.end(sgpd);
            if (!initial)
            {
                // sbgp 12 8.9.2.2 version 0: grouping_type, entry_count 1, { sample_count = all,
                // group_description_index 1 }. In the fragmented file each traf has its own (flushFragment).
                const size_t sbgp = stbl.beginFull("sbgp", 0, 0);
                stbl.fourcc("roll");
                stbl.u32(1);
                stbl.u32((uint32_t)mAudio.samples.size());
                stbl.u32(1);
                stbl.end(sbgp);
            }
        }
        stbl.end(stbl_box);
        m.append(stbl);
        m.end(minf);
        m.end(mdia);
        m.end(trak);
    }

    if (initial)
    {
        // mvex 12 8.8.1.2: one trex per track (12 8.8.3.2: track_ID, default_sample_description_index 1,
        // default_sample_duration, default_sample_size, default_sample_flags - all 0: every trun / tfhd here gives
        // its own). First the mehd (12 8.8.2.2, version 1: fragment_duration, 64 bits - the whole movie's length,
        // fragments included, on the movie's clock; qtdemux.c qtdemux_parse_mehd reads it so, into the duration it
        // keeps in mvhd units): 0 here, and flushFragment() writes the length so far in its place after each
        // fragment.
        const size_t mvex = m.begin("mvex");
        {
            const size_t mehd = m.beginFull("mehd", 1, 0);
            if (mehd_at) *mehd_at = m.b.size();
            m.u64(0);
            m.end(mehd);
        }
        for (uint32_t id = 1; id < track_id; ++id)
        {
            const size_t trex = m.beginFull("trex", 0, 0);
            m.u32(id);
            m.u32(1);
            m.u32(0);
            m.u32(0);
            m.u32(0);
            m.end(trex);
        }
        m.end(mvex);
    }
    m.end(moov);
    return m.b;
}

bool Mp4Writer::finish(int64_t last_video_duration_us, std::string& error)
{
    if (!mFile) { error = "The video file is not open."; return false; }
    std::string err;
    if (!mFailed)
    {
        // the last picture's duration
        if (!mVideo.pending.empty())
        {
            uint64_t d = 0;
            if (last_video_duration_us > 0)
            {
                const uint64_t rel_end = (uint64_t)(mLastPts - mFirstPts) + (uint64_t)last_video_duration_us;
                const uint64_t end_tick = (rel_end * kVideoTimescale + 500000) / 1000000;
                d = end_tick > mLastTick ? end_tick - mLastTick : 1;
            }
            else if (mVideo.pending.size() > 1)
            {
                d = mVideo.pending[mVideo.pending.size() - 2].duration;
            }
            else if (!mVideo.samples.empty())
            {
                d = mVideo.samples.back().duration;
            }
            else
            {
                d = kVideoTimescale / 30;   // a single picture and no duration given: one frame at 30 fps
            }
            mVideo.pending.back().duration = (uint32_t)std::min<uint64_t>(d, 0xFFFFFFFFull);
        }
        flushFragment(err);   // a failure leaves these samples out (mFailed): the file ends at the fragment before
    }
    // samples that never reached a fragment written whole are not in the file
    for (Track* t : { &mVideo, &mAudio })
    {
        t->pending.clear();
        t->pending_bytes.clear();
    }
    if (mVideo.samples.empty() && mAudio.samples.empty())
    {
        closeFile();
        error = mFailed ? mFailure : "Nothing was recorded.";
        return false;
    }

    // The switch to an ordinary MP4 (the idea of FFmpeg's hybrid_fragmented, movenc.c mov_write_trailer, and OBS's
    // hybrid MP4, mp4-mux.c mp4_mux_finalise: the fragmented form ends up inside one mdat, a full moov indexes the
    // samples where they already are). Here the full moov goes into the space kept at the front, and the switch is
    // ONE 8-byte write, so the file is a playable fragmented file before it and a playable ordinary one after it:
    //  1. the full moov (all but its first 8 bytes) and an mdat header covering everything from the end of that moov
    //     to the end of the file are written INSIDE the kept 'free' box, which still hides them;
    //  2. everything is handed to the disk (syncFile);
    //  3. the free box's 8-byte header is overwritten with the moov's size and type: the moov and the mdat appear,
    //     the moov with no samples and the fragments become bytes inside the mdat.
    // When the full moov does not fit in the kept space (more than about an hour), it goes after the last fragment
    // (until step 3 the file then holds a second moov after the fragments - FFmpeg skips a later moov, mov.c
    // mov_read_moov "found a moov box but already parsed one" - as FFmpeg's own hybrid_fragmented does), the mdat
    // header goes in the last 16 bytes of the kept space, and step 3 shortens the free box to end at it.
    const uint32_t movie_ts = movieTimescale(false);
    for (int round = 0; round < 64; ++round)
    {
        // cut anything after the last whole fragment (one half-written when the disk filled)
        errno = 0;
#if defined(_WIN32)
        const int cut_err = (int)_chsize_s(_fileno(mFile), (__int64)mEnd);   // 0, or the error as an errno value
#else
        const int cut_err = ftruncate(fileno(mFile), (off_t)mEnd) == 0 ? 0 : (errno ? errno : EIO);
#endif
        if (cut_err != 0) { err = ioError(cut_err); break; }
        const std::vector<uint8_t> moov = buildMoov(movie_ts, false, nullptr, nullptr);
        const bool front = moov.size() + 16 <= mReserveSize;
        const uint64_t mdat_at = front ? mReserveStart + moov.size() : mReserveStart + mReserveSize - 16;
        uint8_t mdat[16];
        largeHeader(mdat, "mdat", mEnd - mdat_at);   // 12 4.2: size 1, largesize 64 bits
        bool ok = writeAt(mdat_at, mdat, 16, err);
        if (ok && front) ok = writeAt(mReserveStart + 8, moov.data() + 8, moov.size() - 8, err);
        // after the fragments: header last, as for a fragment (a box of size 0 and type 0 until then)
        if (ok && !front)
        {
            ok = writeAt(mEnd + 8, moov.data() + 8, moov.size() - 8, err) && writeAt(mEnd, moov.data(), 8, err);
        }
        if (ok) ok = syncFile(err);
        if (ok)
        {
            uint8_t sw[8];
            if (front)
            {
                std::memcpy(sw, moov.data(), 8);   // the moov's own size and 'moov'
            }
            else
            {
                put32(sw, (uint32_t)(mReserveSize - 16));   // the free box now ends where the mdat starts
                std::memcpy(sw + 4, "free", 4);
            }
            if (writeAt(mReserveStart, sw, 8, err) && syncFile(err))
            {
                closeFile();
                return true;
            }
            break;   // the file stays a playable fragmented file; say why it was not finished
        }
        if (err.rfind("The disk is full", 0) != 0) break;
        // The disk is full (the moov after the fragments, or a copy-on-write file system needing new blocks even for
        // the kept space): drop whole fragments from the end until their bytes make room for the moov (+64 KB).
        // Nothing of step 3 has happened, so the file is still the fragmented one, now shorter.
        const uint64_t need = moov.size() + 65536;
        uint64_t freed = 0;
        while (freed < need && mFragments.size() > 1)
        {
            const Fragment f = mFragments.back();
            mFragments.pop_back();
            freed += mEnd - f.start;
            mEnd = f.start;
            mVideo.samples.resize(f.video_samples);
            mAudio.samples.resize(f.audio_samples);
        }
        // the first fragment holds the moov with no samples that the file needs while it is fragmented: it is never
        // dropped, and when it is all that is left there is nothing more to free
        if (freed == 0) break;
        // the new last picture keeps its own duration
    }
    closeFile();
    error = err.empty() ? "The video file could not be finished." : err;
    return false;
}
}
