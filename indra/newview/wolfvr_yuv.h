/**
 * @file wolfvr_yuv.h
 * @brief [W111 VIDEO 2026-11-24] RGBA -> 4:2:0 YCbCr (ITU-R BT.709, limited range) for the H.264 encoders that take
 *        YUV: OpenH264 (I420), Media Foundation and VideoToolbox (NV12). Moved out of wolfvr_openh264.cpp unchanged
 *        so all of them convert the same way and signal the same colours (VUI and the MP4 'colr' box: BT.709).
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

#ifndef WOLF_VR_YUV_H
#define WOLF_VR_YUV_H

#include <cstddef>
#include <cstdint>

namespace wolfvr
{
    /** Converts a w x h RGBA picture (top row first, 4 bytes a pixel, rows packed) into 4:2:0 BT.709 limited range.
     *  Luma into `y` (y_stride bytes a row). Chroma ((w+1)/2 x (h+1)/2): with nv12 false, Cb into `u` and Cr into
     *  `v` (planar, I420); with nv12 true, Cb/Cr interleaved into `u` (NV12; `v` unused). c_stride: bytes a chroma
     *  row. Odd sizes repeat the last column/row. `threads` bands (1 = this thread only). */
    void rgbaToYuv420(const uint8_t* rgba, uint32_t w, uint32_t h, uint8_t* y, size_t y_stride, uint8_t* u, uint8_t* v,
                      size_t c_stride, bool nv12, uint32_t threads);
}

#endif // WOLF_VR_YUV_H
