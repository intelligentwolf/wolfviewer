/**
 * @file wolfvrpipe_video.h
 * @brief WolfViewer VR pipe, stage 1: the moving test pattern and the video encoders (GStreamer H.264 loaded at run
 *        time - NVENC, VA-API, x264, OpenH264 - and libjpeg as the always-there fallback). Internal to wolfvrpipe.cpp.
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

#ifndef WOLF_VRPIPE_VIDEO_H
#define WOLF_VRPIPE_VIDEO_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace wolfvr
{
    // Head orientation the picture is drawn for, WebXR convention: right-handed, +Y up, -Z forward.
    struct Quat { float x = 0, y = 0, z = 0, w = 1; };

    /** Draws the stage-1 test pattern into rgba (width*height*4): the inside of a cube "room" seen from orientation
     *  q with a square field of view fov_deg - a different colour per wall, a 4x4 grid on each wall and a cross at
     *  each wall's centre - so turning the head shows the viewer really rendering for the pose, and the in-page
     *  reprojection keeps the walls still. Overlaid in screen space: frame number, viewer clock, yaw/pitch, and a
     *  bar that moves one step per frame (a skipped or repeated frame shows as a jump or a stall). */
    void drawTestPattern(std::vector<uint8_t>& rgba, uint32_t width, uint32_t height, float fov_deg, const Quat& q,
                         uint64_t frame, double viewer_ms);

    struct EncodedFrame
    {
        std::vector<uint8_t> data;   // one H.264 access unit (Annex B) or one JPEG
        bool key = false;            // H.264: holds an IDR slice; JPEG: always
    };

    class Encoder
    {
    public:
        virtual ~Encoder() = default;
        /** Encodes one RGBA picture. Encoders here are configured for one-picture-in, one-access-unit-out (no
         *  B-frames, no look-ahead); false with error set if the picture could not be encoded. */
        virtual bool encode(const uint8_t* rgba, EncodedFrame& out, std::string& error) = 0;
        virtual std::string name() const = 0;      // "nvh264enc", "jpeg (libjpeg)", ...
        virtual std::string codec() const = 0;     // "h264" | "jpeg"
    };

    /** Opens the first encoder that works, in the order NVENC, VA-API, x264, OpenH264, JPEG - or only `which`
     *  (nvenc | va | x264 | openh264 | jpeg) when it is not "auto". GStreamer is loaded with dlopen on Linux only, so
     *  a machine without it (and Windows/macOS) gets JPEG. `tried` lists every attempt and why it failed. */
    std::unique_ptr<Encoder> openEncoder(const std::string& which, uint32_t width, uint32_t height, uint32_t fps,
                                         uint32_t bitrate_kbps, std::string& tried);

    /** "avc1.PPCCLL" from the first SPS in an Annex B access unit (ITU-T H.264 7.3.2.1.1: profile_idc,
     *  constraint flags, level_idc are the three bytes after the NAL header). Empty when there is no SPS. */
    std::string avcCodecString(const std::vector<uint8_t>& au);
    /** True when the access unit holds an IDR slice (nal_unit_type 5). */
    bool avcHasIdr(const std::vector<uint8_t>& au);
}

#endif // WOLF_VRPIPE_VIDEO_H
