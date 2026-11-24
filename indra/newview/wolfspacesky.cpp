/**
 * @file wolfspacesky.cpp
 * @brief WolfViewer: the Wolf sky. See wolfspacesky.h.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Copyright (C) 2026, IntelligentWolf Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */


#include "llviewerprecompiledheaders.h"

#include "wolfspacesky.h"

#include "llimage.h"
#include <cmath>

namespace
{
    // Source: wolfmapglobe.cpp SPACE (0.010, 0.015, 0.040) - the colour of empty space.
    const F32 SKY_SPACE_RGB[3] = { 0.010f, 0.015f, 0.040f };
    // Points at least this big are clouds (the Milky Way's glow 40+ px, the nebulae 22+ px); smaller ones are stars.
    // Source: wolf_space_sky.js CLOUD_PX.
    const F32 CLOUD_PX = 18.f;

    std::vector<WolfSpaceSky::Point> sAdd, sOver;
    bool sMade = false;

    // Source: wolfmapglobe.cpp WolfMapGlobe::makeSky (2026-10-03), the generator moved here unchanged
    // (only its two lists are now these); itself a port of map_globe.js MapGlobe._makeSky.
    void make(std::vector<WolfSpaceSky::Point>& add, std::vector<WolfSpaceSky::Point>& over)
    {
        using Point = WolfSpaceSky::Point;
        // mulberry32, as JavaScript computes it with Math.imul and >>> (32-bit wrap-around).
        U32 seed = 0x5751F00Du;
        auto rnd = [&seed]() -> F64
        {
            seed += 0x6D2B79F5u;
            U32 t = seed;
            t = (t ^ (t >> 15)) * (t | 1u);
            t ^= t + (t ^ (t >> 7)) * (t | 61u);
            return (F64)(t ^ (t >> 14)) / 4294967296.0;
        };
        auto gauss = [&rnd]() -> F64
        {
            F64 u = 0.0;
            while (u == 0.0)
            {
                u = rnd();
            }
            const F64 r = sqrt(-2.0 * log(u));
            return r * cos(2.0 * F_PI * rnd());
        };
        auto onSphere = [&rnd](F64 out[3])
        {
            const F64 z = rnd() * 2.0 - 1.0;
            const F64 a = rnd() * 2.0 * F_PI;
            const F64 r = sqrt(1.0 - z * z);
            out[0] = r * cos(a);
            out[1] = r * sin(a);
            out[2] = z;
        };
        // Star colours by spectral class (approximate blackbody tints) and how common each is among
        // stars bright enough to see.
        struct StarClass { F64 weight; F32 c[3]; };
        static const StarClass CLASSES[] = {
            { 0.10, { 0.66f, 0.76f, 1.00f } },   // O/B blue-white
            { 0.22, { 0.86f, 0.90f, 1.00f } },   // A white
            { 0.33, { 1.00f, 0.97f, 0.90f } },   // F/G yellow-white
            { 0.23, { 1.00f, 0.84f, 0.64f } },   // K orange
            { 0.12, { 1.00f, 0.70f, 0.52f } },   // M orange-red
        };
        auto colour = [&rnd]() -> const F32*
        {
            F64 r = rnd();
            for (const StarClass& sc : CLASSES)
            {
                if ((r -= sc.weight) <= 0.0)
                {
                    return sc.c;
                }
            }
            return CLASSES[2].c;
        };
        auto push = [](std::vector<Point>& list, const F64 d[3], F64 size, const F32 c[3], F64 a)
        {
            Point p;
            for (int i = 0; i < 3; ++i)
            {
                p.d[i] = (F32)d[i];
                p.c[i] = c[i];
            }
            p.size = (F32)size;
            p.a = (F32)a;
            list.push_back(p);
        };

        // Field stars: many faint, few bright (brightness ~ u^3).
        for (int i = 0; i < 2400; ++i)
        {
            const F64 b = pow(rnd(), 3.2);
            F64 d[3];
            onSphere(d);
            const F32* c = colour();
            push(add, d, 1.2 + 2.6 * b, c, 0.25 + 0.75 * llmin(1.0, b * 1.6));
        }
        // A handful of bright ones with a soft halo.
        for (int i = 0; i < 14; ++i)
        {
            F64 d[3];
            onSphere(d);
            const F32* c = colour();
            const F64 core_size = 3.6 + 1.6 * rnd();
            push(add, d, core_size, c, 1.0);
            const F64 halo_size = 10.0 + 6.0 * rnd();
            push(add, d, halo_size, c, 0.16);
        }

        // The Milky Way: a great circle tilted across the sky, densest and brightest towards one side
        // (the galactic centre). Aimed so the band crosses the view at the map's middle (the view
        // looks along +z; band(-pi/2, 0) is nearly +z for this tilt), the core just off to one side.
        F64 n[3] = { 0.6, -0.78, 0.15 };
        {
            const F64 l = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            for (F64& x : n) x /= l;
        }
        F64 e1[3] = { n[1], -n[0], 0.0 };
        {
            const F64 l = sqrt(e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2]);
            for (F64& x : e1) x /= l;
        }
        const F64 e2[3] = { n[1] * e1[2] - n[2] * e1[1], n[2] * e1[0] - n[0] * e1[2], n[0] * e1[1] - n[1] * e1[0] };
        auto band = [&](F64 lon, F64 lat, F64 out[3])
        {
            const F64 cl = cos(lat);
            for (int i = 0; i < 3; ++i)
            {
                out[i] = cl * (cos(lon) * e1[i] + sin(lon) * e2[i]) + sin(lat) * n[i];
            }
        };
        const F64 core_lon = -1.1;
        auto near_core = [core_lon](F64 lon)
        {
            return fabs(fmod(fmod(lon - core_lon, 2.0 * F_PI) + 3.0 * F_PI, 2.0 * F_PI) - F_PI);
        };
        static const F32 BAND_COOL[3] = { 0.95f, 0.93f, 1.0f };
        static const F32 BAND_WARM[3] = { 1.0f, 0.92f, 0.80f };
        for (int i = 0; i < 9000; ++i)
        {
            const F64 lon = rnd() * 2.0 * F_PI;
            const F64 core = exp(-pow(near_core(lon) / 0.9, 2.0));
            if (rnd() > 0.35 + 0.65 * core)
            {
                continue;
            }
            const F64 lat = gauss() * (0.07 + 0.08 * core);
            F64 d[3];
            band(lon, lat, d);
            const F64 size = 1.0 + 0.9 * rnd();
            const F32* c = rnd() < 0.5 ? BAND_COOL : BAND_WARM;
            const F64 a = 0.18 + 0.30 * rnd() * (0.5 + core);
            push(add, d, size, c, a);
        }
        // Its glow: soft, very faint, large.
        for (int i = 0; i < 1100; ++i)
        {
            const F64 lon = rnd() * 2.0 * F_PI;
            const F64 core = exp(-pow(near_core(lon) / 0.9, 2.0));
            const F64 lat = gauss() * (0.05 + 0.06 * core);
            const F64 warm = core * 0.5 + rnd() * 0.3;
            F64 d[3];
            band(lon, lat, d);
            const F64 size = 40.0 + 70.0 * rnd() + 60.0 * core;
            const F32 c[3] = { (F32)(0.78 + 0.22 * warm), (F32)(0.80 + 0.10 * warm), (F32)(1.0 - 0.18 * warm) };
            push(add, d, size, c, 0.022 + 0.04 * core);
        }
        // The dark dust lane down the bright middle: drawn over the glow in the space colour.
        for (int i = 0; i < 160; ++i)
        {
            const F64 lon = core_lon + gauss() * 0.9;
            const F64 lat = gauss() * 0.018 + 0.01 * sin(lon * 3.0);
            F64 d[3];
            band(lon, lat, d);
            const F64 size = 18.0 + 26.0 * rnd();
            push(over, d, size, SKY_SPACE_RGB, 0.12);
        }
        // Nebulae: three faint coloured clouds away from the band.
        static const F32 NEB[3][3] = { { 1.0f, 0.45f, 0.62f }, { 0.42f, 0.80f, 0.85f }, { 0.70f, 0.50f, 1.0f } };
        for (const auto& c : NEB)
        {
            F64 centre[3];
            onSphere(centre);
            for (int i = 0; i < 26; ++i)
            {
                F64 d[3];
                for (int k = 0; k < 3; ++k)
                {
                    d[k] = centre[k] + gauss() * 0.035;
                }
                const F64 l = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                for (F64& x : d) x /= l;
                const F64 size = 22.0 + 40.0 * rnd();
                push(add, d, size, c, 0.022 + 0.02 * rnd());
            }
        }

    }

    void ensure()
    {
        if (sMade) return;
        sMade = true;
        make(sAdd, sOver);
    }
}

const std::vector<WolfSpaceSky::Point>& WolfSpaceSky::starlight()
{
    ensure();
    return sAdd;
}

const std::vector<WolfSpaceSky::Point>& WolfSpaceSky::dust()
{
    ensure();
    return sOver;
}

// Source: wolf_space_sky.js EXPERIENCES_VIEW (2026-11-21): aimed at azimuth -75 deg, elevation +8 deg, lens 0.34,
// stars x2.2, clouds x2.6, exposure 1 - the Milky Way and its dust lane across the lower half, the cyan nebula
// (the generator's second cloud, about (-0.69, 0.64, -0.33)) up to the left.
WolfSpaceSky::View WolfSpaceSky::experiencesView()
{
    View v;
    v.yaw = -75.0 * F_PI / 180.0;
    v.pitch = 8.0 * F_PI / 180.0;
    v.lens = 0.34;
    v.starGain = 2.2f;
    v.cloudGain = 2.6f;
    v.exposure = 1.f;
    return v;
}

// Source: wolf_space_sky.js render() (the same steps), and wolfmapglobe.cpp WolfMapGlobe::drawSpace for the turn
// and projection (screen y grows up here).
LLPointer<LLImageRaw> WolfSpaceSky::paint(S32 width, S32 height, F32 scale, const View& view)
{
    ensure();
    const S32 W = llmax(1, ll_round((F32)width * scale)), H = llmax(1, ll_round((F32)height * scale));
    std::vector<F32> buf((size_t)W * H * 3);
    for (size_t i = 0; i < (size_t)W * H; ++i)
    {
        buf[i * 3] = SKY_SPACE_RGB[0];
        buf[i * 3 + 1] = SKY_SPACE_RGB[1];
        buf[i * 3 + 2] = SKY_SPACE_RGB[2];
    }
    const F32 f = (F32)view.lens * (F32)llmax(width, height);
    const F32 cyw = (F32)cos(view.yaw), syw = (F32)sin(view.yaw), cp = (F32)cos(view.pitch), sp = (F32)sin(view.pitch);

    auto splat = [&](const std::vector<Point>& list, bool over)
    {
        for (const Point& s : list)
        {
            const F32 x = s.d[0], y = s.d[1], z = s.d[2];
            const F32 x1 = x * cyw - z * syw, z1 = x * syw + z * cyw;
            const F32 y2 = y * cp - z1 * sp, z2 = y * sp + z1 * cp;
            if (z2 < 0.05f) continue;
            const F32 cx = ((F32)width * 0.5f + f * x1 / z2) * scale, cy = ((F32)height * 0.5f + f * y2 / z2) * scale;
            const F32 h = s.size * 0.5f * scale;
            if (cx < -h || cy < -h || cx > (F32)W + h || cy > (F32)H + h) continue;
            const F32 gain = over ? 1.f : (s.size >= CLOUD_PX ? view.cloudGain : view.starGain);
            const F32 alpha = llmin(1.f, s.a * gain);
            const S32 bx0 = llmax(0, (S32)floorf(cx - h)), bx1 = llmin(W - 1, (S32)ceilf(cx + h));
            const S32 by0 = llmax(0, (S32)floorf(cy - h)), by1 = llmin(H - 1, (S32)ceilf(cy + h));
            const F32 inv = 1.f / (h * h);
            for (S32 py = by0; py <= by1; ++py)
            {
                const F32 dy = (F32)py + 0.5f - cy;
                for (S32 px = bx0; px <= bx1; ++px)
                {
                    const F32 dx = (F32)px + 0.5f - cx;
                    const F32 r2 = (dx * dx + dy * dy) * inv;
                    if (r2 > 1.f) continue;
                    // wolfmapglobe.cpp makeSky's dot: alpha exp(-4 r^2)
                    const F32 a = alpha * expf(-4.f * r2);
                    F32* o = &buf[((size_t)py * W + px) * 3];
                    if (over)
                    {
                        for (int k = 0; k < 3; ++k) o[k] = s.c[k] * a + o[k] * (1.f - a);
                    }
                    else
                    {
                        for (int k = 0; k < 3; ++k) o[k] += s.c[k] * a;
                    }
                }
            }
        }
    };
    splat(starlight(), false);
    splat(dust(), true);

    LLPointer<LLImageRaw> raw = new LLImageRaw((U16)W, (U16)H, 3);
    U8* out = raw->getData();
    if (!out) return nullptr;
    const F32 ex = view.exposure;
    for (size_t i = 0; i < (size_t)W * H * 3; ++i)
    {
        const F32 c = ex > 0.f ? 1.f - expf(-ex * buf[i]) : buf[i];
        out[i] = (U8)llclamp(ll_round(c * 255.f), 0, 255);
    }
    return raw;
}

// [STARFOLD 2026-11-24] Source: paint() above (the dot, the add-then-lay-over order, the alpha held to 1), on an
// equirectangular grid instead of a perspective view. The angular distance of a texel's direction t from a point's
// direction d comes from the chord: |t - d|^2 = 2 - 2 t.d (the angle squared to within 0.1 % below 0.1 rad); a dot of
// angular radius rho reaches rho / cos(latitude) in longitude on a row (the meridians close up towards the poles).
LLPointer<LLImageRaw> WolfSpaceSky::paintSphere(S32 width)
{
    ensure();
    const S32 W = llmax(64, width), H = W / 2;
    const F32 texel = (F32)(2.0 * F_PI) / (F32)W;
    std::vector<F32> buf((size_t)W * H * 3, 0.f);
    // each texel's direction, as wolftk/skyF.glsl wolfStarfoldSpace reads the image back
    std::vector<F32> rowZ((size_t)H), rowR((size_t)H), colX((size_t)W), colY((size_t)W);
    for (S32 j = 0; j < H; ++j)
    {
        const F32 lat = ((F32)j + 0.5f) / (F32)H * F_PI - F_PI * 0.5f;
        rowZ[j] = sinf(lat);
        rowR[j] = cosf(lat);
    }
    for (S32 i = 0; i < W; ++i)
    {
        const F32 lon = ((F32)i + 0.5f) / (F32)W * 2.f * F_PI - F_PI;
        colX[i] = cosf(lon);
        colY[i] = sinf(lon);
    }
    auto splat = [&](const std::vector<Point>& list, bool over)
    {
        for (const Point& s : list)
        {
            // the map's pixels as texels; a dot never under 1.5 texels across, its light kept (alpha x its area's share)
            F32 rho = s.size * 0.5f * texel;
            F32 alpha = llmin(1.f, s.a);
            const F32 least = texel * 0.75f;
            if (rho < least)
            {
                alpha *= (rho * rho) / (least * least);
                rho = least;
            }
            const F32 lat0 = asinf(llclamp(s.d[2], -1.f, 1.f));
            const F32 lon0 = atan2f(s.d[1], s.d[0]);
            const S32 j0 = llmax(0, (S32)floorf((lat0 - rho + F_PI * 0.5f) / F_PI * (F32)H));
            const S32 j1 = llmin(H - 1, (S32)ceilf((lat0 + rho + F_PI * 0.5f) / F_PI * (F32)H));
            const F32 inv = 1.f / (rho * rho);
            for (S32 j = j0; j <= j1; ++j)
            {
                // the longitudes this row can reach: every one near a pole
                S32 i0 = 0, i1 = W - 1;
                const F32 reach = rowR[j] > 1e-4f ? rho / rowR[j] : 10.f;
                if (reach < F_PI)
                {
                    i0 = (S32)floorf((lon0 - reach + F_PI) / (2.f * F_PI) * (F32)W);
                    i1 = (S32)ceilf((lon0 + reach + F_PI) / (2.f * F_PI) * (F32)W);
                }
                for (S32 ii = i0; ii <= i1; ++ii)
                {
                    const S32 i = ((ii % W) + W) % W;   // across the seam at -pi / pi
                    const F32 dot = rowR[j] * (colX[i] * s.d[0] + colY[i] * s.d[1]) + rowZ[j] * s.d[2];
                    const F32 r2 = llmax(0.f, 2.f - 2.f * dot) * inv;
                    if (r2 > 1.f) continue;
                    const F32 a = alpha * expf(-4.f * r2);
                    F32* o = &buf[((size_t)j * W + i) * 3];
                    if (over)
                    {
                        for (int k = 0; k < 3; ++k) o[k] = s.c[k] * a + o[k] * (1.f - a);
                    }
                    else
                    {
                        for (int k = 0; k < 3; ++k) o[k] += s.c[k] * a;
                    }
                }
            }
        }
    };
    splat(starlight(), false);
    splat(dust(), true);

    LLPointer<LLImageRaw> raw = new LLImageRaw((U16)W, (U16)H, 3);
    U8* out = raw->getData();
    if (!out) return nullptr;
    for (size_t i = 0; i < (size_t)W * H * 3; ++i)
    {
        out[i] = (U8)llclamp(ll_round(sqrtf(llclamp(buf[i], 0.f, 1.f)) * 255.f), 0, 255);
    }
    return raw;
}
