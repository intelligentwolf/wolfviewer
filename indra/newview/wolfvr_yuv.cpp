/**
 * @file wolfvr_yuv.cpp
 * @brief [W111 VIDEO 2026-11-24] RGBA -> 4:2:0 BT.709 limited range. See wolfvr_yuv.h.
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

#include "wolfvr_yuv.h"

#include <algorithm>
#include <thread>
#include <vector>

namespace wolfvr
{
namespace
{
    // [VR OPENH264 2026-11-03, moved here W111] ITU-R BT.709-6 item 3.2/3.3: Kr = 0.2126, Kb = 0.0722
    // (Kg = 1 - Kr - Kb); E'Cb = (E'B - E'Y) / 1.8556, E'Cr = (E'R - E'Y) / 1.5748. Item 4.6 (8-bit):
    // Y = 219 E'Y + 16, Cb/Cr = 224 E'C + 128. The 8-bit RGB input is E' * 255. Coefficients in 16.16 fixed point.
    constexpr double kKr = 0.2126, kKb = 0.0722, kKg = 1.0 - kKr - kKb;
    constexpr int fix16(double v) { return (int)(v >= 0 ? v * 65536.0 + 0.5 : v * 65536.0 - 0.5); }
    constexpr double kYs = 219.0 / 255.0, kCs = 224.0 / 255.0;
    constexpr int kYR = fix16(kYs * kKr), kYG = fix16(kYs * kKg), kYB = fix16(kYs * kKb);
    constexpr int kUR = fix16(-kCs * kKr / 1.8556), kUG = fix16(-kCs * kKg / 1.8556), kUB = fix16(kCs * (1 - kKb) / 1.8556);
    constexpr int kVR = fix16(kCs * (1 - kKr) / 1.5748), kVG = fix16(-kCs * kKg / 1.5748), kVB = fix16(-kCs * kKb / 1.5748);

    inline uint8_t lumaOf(const uint8_t* p)
    {
        return (uint8_t)((kYR * p[0] + kYG * p[1] + kYB * p[2] + (16 << 16) + (1 << 15)) >> 16);
    }

    // Chroma rows [cy0, cy1) (= luma rows 2*cy0 .. 2*cy1-1).
    void convertBand(const uint8_t* rgba, uint32_t w, uint32_t h, uint8_t* Y, size_t ys, uint8_t* U, uint8_t* V,
                     size_t cs, bool nv12, uint32_t cw, uint32_t cy0, uint32_t cy1)
    {
        const size_t stride = (size_t)w * 4;
        const size_t step = nv12 ? 2 : 1;
        for (uint32_t cy = cy0; cy < cy1; ++cy)
        {
            const uint32_t y0 = cy * 2, y1 = std::min(y0 + 1, h - 1);
            const uint8_t* r0 = rgba + y0 * stride;
            const uint8_t* r1 = rgba + y1 * stride;
            uint8_t* Y0 = Y + (size_t)y0 * ys;
            uint8_t* Y1 = Y + (size_t)y1 * ys;   // == Y0 for the last row of an odd height: same values written
            uint8_t* Ur = U + (size_t)cy * cs;
            uint8_t* Vr = nv12 ? Ur + 1 : V + (size_t)cy * cs;
            for (uint32_t cx = 0; cx < cw; ++cx)
            {
                const uint32_t x0 = cx * 2, x1 = std::min(x0 + 1, w - 1);
                const uint8_t* a = r0 + (size_t)x0 * 4;
                const uint8_t* b = r0 + (size_t)x1 * 4;
                const uint8_t* c = r1 + (size_t)x0 * 4;
                const uint8_t* d = r1 + (size_t)x1 * 4;
                Y0[x0] = lumaOf(a);
                Y0[x1] = lumaOf(b);
                Y1[x0] = lumaOf(c);
                Y1[x1] = lumaOf(d);
                // chroma of the 2x2 average: sums are 0..1020, so the shift is 16 + 2
                const int sr = a[0] + b[0] + c[0] + d[0];
                const int sg = a[1] + b[1] + c[1] + d[1];
                const int sb = a[2] + b[2] + c[2] + d[2];
                Ur[cx * step] = (uint8_t)std::clamp((kUR * sr + kUG * sg + kUB * sb + (128 << 18) + (1 << 17)) >> 18, 0, 255);
                Vr[cx * step] = (uint8_t)std::clamp((kVR * sr + kVG * sg + kVB * sb + (128 << 18) + (1 << 17)) >> 18, 0, 255);
            }
        }
    }
}

void rgbaToYuv420(const uint8_t* rgba, uint32_t w, uint32_t h, uint8_t* y, size_t y_stride, uint8_t* u, uint8_t* v,
                  size_t c_stride, bool nv12, uint32_t threads)
{
    if (!w || !h) return;
    const uint32_t cw = (w + 1) / 2, ch = (h + 1) / 2;
    const uint32_t bands = std::max(1u, std::min(threads, ch));
    if (bands <= 1)
    {
        convertBand(rgba, w, h, y, y_stride, u, v, c_stride, nv12, cw, 0, ch);
        return;
    }
    std::vector<std::thread> workers;
    workers.reserve(bands - 1);
    const uint32_t per = (ch + bands - 1) / bands;
    for (uint32_t i = 1; i < bands; ++i)
    {
        const uint32_t a = std::min(ch, i * per), b = std::min(ch, (i + 1) * per);
        workers.emplace_back(convertBand, rgba, w, h, y, y_stride, u, v, c_stride, nv12, cw, a, b);
    }
    convertBand(rgba, w, h, y, y_stride, u, v, c_stride, nv12, cw, 0, std::min(ch, per));
    for (std::thread& t : workers) t.join();
}
}
