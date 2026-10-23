/**
 * @file wolftunnelcamera.h
 * @brief WolfViewer: the pure maths of the camera inside a tunnel (no viewer types, so tests/wolftunnelcamera_test.cpp checks
 *        it off-line). WolfTunnels::keepCameraInside (wolffloatertunnels.cpp) is the only caller.
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

#ifndef WOLF_TUNNEL_CAMERA_H
#define WOLF_TUNNEL_CAMERA_H

// [WOLF TUNNELS 2026-10-23] Paul: "i've still got this thing when i walk through a tunnel where the camera flashes back" (10-22:
// "it kept flicking the camera"). The 10-21 keepCameraInside walked out from the head in 0.2 m steps and stopped at the first
// point that was in no box SHRUNK by the 0.25 m inset. The region's boxes along one straight run overlap by only 0.2 m
// (Source: wolfstorm/rust_proxy/src/wolftunnel.rs:1859-1861 volumes(): "if p > 0 { a -= 0.1; } if p + 1 < pieces { b += 0.1; }",
// pieces at most VOL_PIECE_M = 16 m, wolftunnel.rs:207), so shrunk by 0.25 m each they leave a 0.3 m gap at every join - wider
// than the 0.2 m step, so a step always landed in it. Every time the head passed a join the camera snapped to the head, then
// slid back out as the join fell behind: the "flash" every few metres. The 0.2 m steps also moved the camera in 0.2 m jumps,
// and at a mouth the ground test in the inset band flipped from step to step.
//
// Now: a point is allowed when a ball of the inset's radius round it stays in the union of the boxes (or the open air): it is
// in a box, and past each face of that box it is nearer than the inset to, the point the inset beyond it is in another box or in
// the air. The boxes' joins are inside the union, so they no longer stop the camera; only rock does. The edge found by the steps
// is refined by halving, so it moves smoothly as the avatar walks, and the distance the camera keeps is eased over time (quick
// in, slower out), so where the edge itself jumps (a bend's corner coming between the head and the camera, the mouth) the camera
// glides instead of snapping.

#include <algorithm>
#include <cmath>
#include <vector>

namespace WolfTunnelCamera
{
    struct Vec3
    {
        float x = 0.f, y = 0.f, z = 0.f;
        Vec3() = default;
        Vec3(float ax, float ay, float az) : x(ax), y(ay), z(az) {}
        Vec3 operator+(const Vec3& o) const { return Vec3(x + o.x, y + o.y, z + o.z); }
        Vec3 operator-(const Vec3& o) const { return Vec3(x - o.x, y - o.y, z - o.z); }
        Vec3 operator*(float k) const { return Vec3(x * k, y * k, z * k); }
        float length() const { return std::sqrt(x * x + y * y + z * z); }
    };

    /** A tunnel box as the region keeps it (wolffloatertunnels.h WolfTunnels::Box): centre and full sizes in region metres,
     *  sizes along its turn (length, width, height), turned about +Z by yaw degrees. */
    struct Box
    {
        Vec3 c;
        Vec3 s;
        float yaw_deg = 0.f;
    };

    // The ball's radius: the camera is kept this far inside the union of the boxes, metres. The boxes reach 0.3 m past the walls
    // and over the roof (Source: wolftunnel.rs:209-210 VOL_ABOVE_M = 0.3, VOL_SIDE_M = 0.3), and the lining's rock stands just
    // outside the air (wolftunnel.rs:185-188), so 0.4 m keeps the camera 0.1 m inside the air - the stock camera's own clearance
    // from what it collides with (Source: llagentcamera.cpp:80 CAMERA_COLLIDE_EPSILON = 0.1).
    constexpr float INSET_M = 0.4f;
    // Steps along the head-to-camera line when finding where it leaves the allowed space, metres (the 10-21 value).
    constexpr float STEP_M = 0.2f;
    // Halvings of the last step: 0.2 m / 2^7 = 1.6 mm.
    constexpr int REFINE = 7;
    // Easing of the distance kept, half-lives in seconds (the formula of LLSmoothInterpolation::calcInterpolant, llcommon/
    // llcriticaldamp.cpp:118: 1 - 2^(-dt / half_life)). In quickly, so the camera is in rock for a moment at most; out more slowly,
    // so it glides back (our own choices: in a little faster, out about twice as slow as the stock zoom, llagentcamera.cpp:72
    // CAMERA_ZOOM_HALF_LIFE = 0.07 s).
    constexpr float EASE_IN_S = 0.04f;
    constexpr float EASE_OUT_S = 0.15f;
    // A gap longer than this between two updates (the head left the boxes, the viewer stalled) starts the easing again.
    constexpr float EASE_RESTART_S = 0.5f;

    constexpr float DEG_TO_RAD_F = 0.017453292519943295f;

    /** p in local box coordinates (the inverse of the box's turn: local = R(-yaw) * (p - c)), the same formula as
     *  wolffloatertunnels.cpp inBox. */
    inline Vec3 toLocal(const Box& b, const Vec3& p)
    {
        const float a = b.yaw_deg * DEG_TO_RAD_F, c = std::cos(a), s = std::sin(a);
        const float dx = p.x - b.c.x, dy = p.y - b.c.y;
        return Vec3(c * dx + s * dy, -s * dx + c * dy, p.z - b.c.z);
    }

    /** Local axis k (0 x, 1 y, 2 z) of the box, in region metres. */
    inline Vec3 axis(const Box& b, int k)
    {
        if (k == 2) return Vec3(0.f, 0.f, 1.f);
        const float a = b.yaw_deg * DEG_TO_RAD_F, c = std::cos(a), s = std::sin(a);
        return k == 0 ? Vec3(c, s, 0.f) : Vec3(-s, c, 0.f);
    }

    inline bool inBox(const Box& b, const Vec3& p)
    {
        const Vec3 l = toLocal(b, p);
        return std::fabs(l.x) <= b.s.x * 0.5f && std::fabs(l.y) <= b.s.y * 0.5f && std::fabs(l.z) <= b.s.z * 0.5f;
    }

    /** In a box, or in the open air (above the land under it). GroundFn: float(float region_x, float region_y). */
    template <typename GroundFn>
    bool covered(const std::vector<const Box*>& boxes, const GroundFn& ground, const Vec3& q)
    {
        for (const Box* b : boxes) if (inBox(*b, q)) return true;
        return q.z > ground(q.x, q.y);
    }

    /** May the camera be at p: the ball of radius r round it is (as far as its six extreme points along the box's faces tell) in
     *  the union of the boxes, or p is in the open air at least r over the land. */
    template <typename GroundFn>
    bool allowed(const std::vector<const Box*>& boxes, const GroundFn& ground, const Vec3& p, float r)
    {
        if (p.z > ground(p.x, p.y) + r) return true;
        for (const Box* b : boxes)
        {
            const Vec3 l = toLocal(*b, p);
            const float half[3] = { b->s.x * 0.5f, b->s.y * 0.5f, b->s.z * 0.5f };
            const float lv[3] = { l.x, l.y, l.z };
            bool in = true;
            for (int k = 0; k < 3 && in; ++k) in = std::fabs(lv[k]) <= half[k];
            if (!in) continue;
            bool clear = true;
            for (int k = 0; k < 3 && clear; ++k)
            {
                // near the + face and / or the - face of this axis: the point r beyond it must be in another box or in the air
                if (half[k] - lv[k] < r) clear = covered(boxes, ground, p + axis(*b, k) * r);
                if (clear && half[k] + lv[k] < r) clear = covered(boxes, ground, p - axis(*b, k) * r);
            }
            if (clear) return true;
        }
        return false;
    }

    /**
     * How far from the head (metres, 0..len) along the unit direction dir the camera may go before it would leave the allowed
     * space: stepped out every STEP_M, the step where it leaves refined by halving. The head itself is taken as allowed (it is
     * in a box; the caller checks that).
     */
    template <typename GroundFn>
    float allowedDistance(const std::vector<const Box*>& boxes, const GroundFn& ground, const Vec3& head, const Vec3& dir,
                          float len, float r = INSET_M)
    {
        if (!(len > 0.f)) return 0.f;
        const int steps = std::max(1, (int)std::ceil(len / STEP_M));
        float keep = 0.f;
        for (int i = 1; i <= steps; ++i)
        {
            const float d = len * (float)i / (float)steps;
            if (!allowed(boxes, ground, head + dir * d, r))
            {
                float lo = keep, hi = d;
                for (int j = 0; j < REFINE; ++j)
                {
                    const float mid = 0.5f * (lo + hi);
                    if (allowed(boxes, ground, head + dir * mid, r)) lo = mid; else hi = mid;
                }
                return lo;
            }
            keep = d;
        }
        return len;
    }

    /** One easing step of the kept distance towards target over dt seconds (dt <= 0: unchanged). */
    inline float ease(float current, float target, float dt)
    {
        if (!(dt > 0.f)) return current;
        const float half_life = target < current ? EASE_IN_S : EASE_OUT_S;
        const float k = std::min(std::max(1.f - std::pow(2.f, -dt / half_life), 0.f), 1.f);
        return current + (target - current) * k;
    }
}

#endif // WOLF_TUNNEL_CAMERA_H
