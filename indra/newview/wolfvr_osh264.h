/**
 * @file wolfvr_osh264.h
 * @brief [W111 VIDEO 2026-11-24] The operating system's own H.264 encoders as wolfvr::Encoder: Media Foundation on
 *        Windows (wolfvr_mfh264.cpp) and VideoToolbox on macOS (wolfvr_vth264.mm). Used by wolfvr::openEncoder
 *        (wolfvrpipe_video.cpp) for the VR pipe and the video recorder.
 *
 * Both take RGBA (top row first, as every wolfvr::Encoder), convert it to NV12 BT.709 limited range (wolfvr_yuv.h),
 * tell the encoder those colours (so the SPS VUI says BT.709), make one Annex B access unit per picture with SPS and
 * PPS before every IDR, and use no B-frames. Width and height must be even.
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

#ifndef WOLF_VR_OSH264_H
#define WOLF_VR_OSH264_H

#include "wolfvrpipe_video.h"

#include <cstdint>
#include <memory>
#include <string>

namespace wolfvr
{
#if defined(_WIN32)
    /** hardware true: the GPU's H.264 encoder MFT (asynchronous); false: Microsoft's software H.264 encoder MFT.
     *  Null with `error` set when there is none or it refuses. Every call on the one thread that opened it. */
    std::unique_ptr<Encoder> openMediaFoundationH264(uint32_t width, uint32_t height, uint32_t fps,
                                                     uint32_t bitrate_kbps, bool hardware, std::string& error);
#elif defined(__APPLE__)
    /** hardware true: a VTCompressionSession that requires the hardware encoder; false: one that is not allowed to
     *  use it (Apple's software encoder). Null with `error` set when it cannot be made. */
    std::unique_ptr<Encoder> openVideoToolboxH264(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate_kbps,
                                                  bool hardware, std::string& error);
#endif
}

#endif // WOLF_VR_OSH264_H
