/**
 * @file wolfterraformmath.h
 * @brief WolfViewer: the pure maths of the terrain writer and the Terraform tools (no viewer types, so
 *        tests/wolfterraformmath_test.cpp checks it off-line).
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

#ifndef WOLF_TERRAFORM_MATH_H
#define WOLF_TERRAFORM_MATH_H

// [WOLF TERRAFORM 2026-10-23] Paul: "where is the extra terraforming tools you know we were going to enhance it based on other
// terraforming applications". Design: firestorm/region-terrain-work/terraforming-tools/DESIGN.md (sections 3.1-3.2).
//
// Every tool here works out TARGET heights from the ground the viewer already has; nothing here talks to a region. The target
// is then written by WolfTerrainWriter as ModifyLand flatten floods: a flood with Seconds >= 25 SETS every metre of its
// rectangle to the one height it carries (Source: OpenSim FloodBrushes/FlattenArea.cs:47-58 FloodEffect: strength = seconds *
// 0.04, >= 0.999 sets map[x, y] = height, the height clamped to Constants.MinTerrainHeightmap..MaxTerrainHeightmap, -100..4000,
// Framework/Constants.cs:165-166), over (int)west..(int)east x (int)south..(int)north inclusive (Source: TerrainModule.cs:
// 1855-1880 client_OnModifyTerrain, the flood branch).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace WolfTerraformMath
{
    inline float nan() { return std::numeric_limits<float>::quiet_NaN(); }
    inline bool has(float v) { return !std::isnan(v); }

    // Source: llcommon/lldefs.h:180-184 llmax / llmin (d0 > d1 ? d0 : d1, d0 < d1 ? d0 : d1): the cave writer's merge used them,
    // so the moved code keeps the same expressions.
    inline float maxLL(float a, float b) { return a > b ? a : b; }
    inline float minLL(float a, float b) { return a < b ? a : b; }

    // Source: OpenSim Framework/Constants.cs:165-166 MinTerrainHeightmap = -100, MaxTerrainHeightmap = 4000 (FlattenArea clamps
    // the flood's height to these).
    constexpr float MIN_TERRAIN_M = -100.f;
    constexpr float MAX_TERRAIN_M = 4000.f;

    /** A rectangle of whole-metre ground cells (region metres), one value per cell, row by row; NaN = no value / leave alone. */
    struct Grid
    {
        int x0 = 0, y0 = 0, w = 0, h = 0;
        std::vector<float> v;

        Grid() = default;
        Grid(int ax0, int ay0, int aw, int ah, float fill = nan())
            : x0(ax0), y0(ay0), w(std::max(0, aw)), h(std::max(0, ah)), v((size_t)std::max(0, aw) * (size_t)std::max(0, ah), fill) {}
        bool inside(int x, int y) const { return x >= x0 && y >= y0 && x < x0 + w && y < y0 + h; }
        float at(int x, int y) const { return inside(x, y) ? v[(size_t)(y - y0) * (size_t)w + (size_t)(x - x0)] : nan(); }
        float& ref(int x, int y) { return v[(size_t)(y - y0) * (size_t)w + (size_t)(x - x0)]; }
        size_t cells() const { return v.size(); }
        size_t defined() const { size_t n = 0; for (float f : v) n += has(f) ? 1 : 0; return n; }
    };

    /** A point of the cave writer's list: a cell and its wanted height. */
    struct Point { int x, y; float z; };
    /** One flatten flood: cells x0..x1, y0..y1 (inclusive) all set to z. */
    struct Run { int x0, x1, y0, y1; float z; };

    /** The ModifyLand ParcelData rectangle and Height of one flood. Source: wolfcave.cpp (before 2026-10-23) WolfCaves::shape:
     *  West = x, South = y0, East = x + 0.5, North = y1 + 0.5 (the region floods (int)west..(int)east inclusive, so +0.5 keeps it
     *  on the last cell and makes the rectangle more than a point: TerrainModule.cs:1803 a point is a brush dab). */
    struct Flood { float west, south, east, north, height; };
    inline Flood floodOf(const Run& r)
    {
        return Flood{ (float)r.x0, (float)r.y0, (float)r.x1 + 0.5f, (float)r.y1 + 0.5f, r.z };
    }

    /** What the region does with one flood (FlattenArea with strength >= 0.999), for the tests: every cell of the run that is
     *  inside the map is set to the run's height, clamped as the region clamps it. */
    inline void applyFlood(Grid& map, const Run& r)
    {
        const float z = std::min(std::max(r.z, MIN_TERRAIN_M), MAX_TERRAIN_M);
        for (int x = r.x0; x <= r.x1; ++x)
            for (int y = r.y0; y <= r.y1; ++y)
                if (map.inside(x, y)) map.ref(x, y) = z;
    }

    // Source: wolfcave.cpp (before 2026-10-23) WolfCaves::shape, the `send` lambda's merge, moved here unchanged so the Cave
    // Builder sends exactly the floods it sent before (tests/wolfterraformmath_test.cpp compares the two on random caves):
    // runs along y per column; digging (`exact`) keeps a run while the height stays within 0.005 m of the run's first point and
    // floods it at that height; putting back keeps it while the run's heights stay within `band` and floods it at the band's
    // middle. One flood per run, x1 = x0.
    inline std::vector<Run> caveRuns(const std::vector<Point>& list, bool exact, float band)
    {
        std::map<int, std::vector<Point>> cols;
        for (const Point& p : list) cols[p.x].push_back(p);
        std::vector<Run> runs;
        for (auto& kv : cols)
        {
            std::vector<Point>& col = kv.second;
            std::sort(col.begin(), col.end(), [](const Point& a, const Point& b) { return a.y < b.y; });
            size_t s = 0;
            float lo = col.empty() ? 0.f : col[0].z, hi = lo;
            for (size_t i = 1; i <= col.size(); ++i)
            {
                const bool same = i < col.size() && (exact ? std::fabs(col[i].z - col[s].z) < 0.005f
                                                           : maxLL(hi, col[i].z) - minLL(lo, col[i].z) <= band);
                if (i < col.size() && col[i].y == col[i - 1].y + 1 && same)
                {
                    lo = minLL(lo, col[i].z);
                    hi = maxLL(hi, col[i].z);
                    continue;
                }
                runs.push_back(Run{ kv.first, kv.first, col[s].y, col[i - 1].y, exact ? col[s].z : (lo + hi) * 0.5f });
                s = i;
                if (i < col.size()) lo = hi = col[i].z;
            }
        }
        return runs;
    }

    /**
     * The fewest floods for a target grid (the Terraform tools). Each line of cells (a column, or a row: whichever gives fewer
     * floods) is cut into runs whose targets stay within `band` (flooded at the band's middle, so no cell is more than band / 2
     * off: the cave put-back's rule); a run whose cells all already stand within `skip` of its height in `current` is left out;
     * then neighbouring lines' identical runs are joined into one rectangle. Cells with no target (NaN) are never touched.
     */
    inline std::vector<Run> planRuns(const Grid& target, const Grid& current, float band, float skip)
    {
        auto plan = [&](bool by_column) -> std::vector<Run>
        {
            const int lines = by_column ? target.w : target.h, len = by_column ? target.h : target.w;
            std::vector<Run> out;
            // the runs of the line before, by (start, end, height), to join with
            std::map<std::pair<std::pair<int, int>, uint32_t>, size_t> open, next;
            auto key = [](int a, int b, float z)
            {
                uint32_t bits;
                std::memcpy(&bits, &z, sizeof bits);
                return std::make_pair(std::make_pair(a, b), bits);
            };
            for (int li = 0; li < lines; ++li)
            {
                next.clear();
                auto cell = [&](int k, int& x, int& y) { x = target.x0 + (by_column ? li : k); y = target.y0 + (by_column ? k : li); };
                int k = 0;
                while (k < len)
                {
                    int x, y;
                    cell(k, x, y);
                    float t = target.at(x, y);
                    if (!has(t)) { ++k; continue; }
                    const int s = k;
                    float lo = t, hi = t;
                    while (k + 1 < len)
                    {
                        int nx, ny;
                        cell(k + 1, nx, ny);
                        const float u = target.at(nx, ny);
                        if (!has(u) || std::max(hi, u) - std::min(lo, u) > band) break;
                        lo = std::min(lo, u);
                        hi = std::max(hi, u);
                        ++k;
                    }
                    const float z = (lo + hi) * 0.5f;
                    bool needed = false;
                    for (int j = s; j <= k && !needed; ++j)
                    {
                        int cx, cy;
                        cell(j, cx, cy);
                        const float g = current.at(cx, cy);
                        needed = !has(g) || std::fabs(g - z) > skip;
                    }
                    if (needed)
                    {
                        const auto kk = key(s, k, z);
                        auto it = open.find(kk);
                        if (it != open.end())
                        {
                            Run& r = out[it->second];
                            if (by_column) r.x1 = target.x0 + li; else r.y1 = target.y0 + li;
                            next[kk] = it->second;
                        }
                        else
                        {
                            Run r;
                            if (by_column) r = Run{ target.x0 + li, target.x0 + li, target.y0 + s, target.y0 + k, z };
                            else r = Run{ target.x0 + s, target.x0 + k, target.y0 + li, target.y0 + li, z };
                            out.push_back(r);
                            next[kk] = out.size() - 1;
                        }
                    }
                    ++k;
                }
                open.swap(next);
            }
            return out;
        };
        std::vector<Run> cols = plan(true), rows = plan(false);
        return rows.size() < cols.size() ? rows : cols;
    }

    /** A cosine fall from 1 (t <= 0) to 0 (t >= 1): the soft edges and shoulders (Unreal's "Smooth" falloff, DESIGN.md 2.1). */
    inline float fall(float t)
    {
        if (t <= 0.f) return 1.f;
        if (t >= 1.f) return 0.f;
        return 0.5f * (1.f + std::cos(3.14159265358979f * t));
    }

    /** The ground between cells (bilinear), NaN when a corner is not known. */
    inline float sample(const Grid& g, float fx, float fy)
    {
        const int x = (int)std::floor(fx), y = (int)std::floor(fy);
        const float tx = fx - (float)x, ty = fy - (float)y;
        const float a = g.at(x, y), b = g.at(x + 1, y), c = g.at(x, y + 1), d = g.at(x + 1, y + 1);
        if (tx == 0.f && ty == 0.f) return a;
        if (!has(a) || !has(b) || !has(c) || !has(d))
        {
            // at the grid's last column / row the far corners are outside: take the nearest known cell
            const float n = g.at((int)std::lround(fx), (int)std::lround(fy));
            return n;
        }
        return (a * (1.f - tx) + b * tx) * (1.f - ty) + (c * (1.f - tx) + d * tx) * ty;
    }

    /** A box of whole cells between two clicked corners (inclusive). */
    struct Box { int x0, y0, x1, y1; };
    inline Box boxOf(float ax, float ay, float bx, float by)
    {
        Box b;
        b.x0 = (int)std::lround(std::min(ax, bx));
        b.x1 = (int)std::lround(std::max(ax, bx));
        b.y0 = (int)std::lround(std::min(ay, by));
        b.y1 = (int)std::lround(std::max(ay, by));
        return b;
    }
    /** How far cell (x, y) is outside the box, metres (0 inside). */
    inline float outside(const Box& b, int x, int y)
    {
        const float dx = (float)std::max(std::max(b.x0 - x, 0), x - b.x1), dy = (float)std::max(std::max(b.y0 - y, 0), y - b.y1);
        return std::sqrt(dx * dx + dy * dy);
    }
    /** How far cell (x, y) is inside the box from its nearest edge, cells (0 on the edge row). */
    inline int inward(const Box& b, int x, int y)
    {
        return std::min(std::min(x - b.x0, b.x1 - x), std::min(y - b.y0, b.y1 - y));
    }

    /** [2026-10-23 Paul: "we have an 1800x1800 region ... If you try and write the whole thing it will crash"] No grid here is
     *  ever sized from the region: every one is a job's rectangle, and a rectangle wider than this is refused (an empty grid)
     *  - one job's 512 m (wolfterraform.h MAX_SIDE_M) plus the widest margin a tool adds round it (soft edges, shoulders,
     *  banks, a smooth's reach: 64 m). On Ireland (460,800 m) a whole-region grid would be 849 GB. */
    constexpr int MAX_GRID_SIDE = 512 + 2 * 64;

    /** The grid a tool writes: the box grown by `margin`, clipped to the region (0 .. region_w - 1); empty past MAX_GRID_SIDE. */
    inline Grid areaGrid(const Box& b, int margin, int region_w)
    {
        // in 64 bits: a box and its margin can lie anywhere on a 460,800 m region
        const long long x0 = std::max(0LL, (long long)b.x0 - margin), y0 = std::max(0LL, (long long)b.y0 - margin);
        const long long x1 = std::min((long long)region_w - 1, (long long)b.x1 + margin), y1 = std::min((long long)region_w - 1, (long long)b.y1 + margin);
        if (x1 < x0 || y1 < y0) return Grid();
        if (x1 - x0 + 1 > MAX_GRID_SIDE || y1 - y0 + 1 > MAX_GRID_SIDE) return Grid();
        return Grid((int)x0, (int)y0, (int)(x1 - x0 + 1), (int)(y1 - y0 + 1));
    }

    // ---- Level: the box at one height, an optional soft edge round it -------------------------------------------------------

    /** Every cell of the box at `height`; within `edge` metres outside it, eased from the ground to `height`. */
    inline Grid level(const Grid& ground, const Box& box, float height, float edge, int region_w)
    {
        Grid t = areaGrid(box, (int)std::ceil(std::max(0.f, edge)), region_w);
        for (int y = t.y0; y < t.y0 + t.h; ++y)
            for (int x = t.x0; x < t.x0 + t.w; ++x)
            {
                const float g = ground.at(x, y);
                if (!has(g)) continue;
                const float d = outside(box, x, y);
                if (d == 0.f) t.ref(x, y) = height;
                else if (edge > 0.f && d < edge) t.ref(x, y) = g + (height - g) * fall(d / edge);
            }
        return t;
    }

    // ---- Ramp: a straight slope from A to B, width w, soft shoulders s ----------------------------------------------------

    struct RampShape { float ax, ay, az, bx, by, bz, width, shoulder; };
    /** Rise over run between the two ends (0 when they are on one spot). */
    inline float grade(float ax, float ay, float az, float bx, float by, float bz)
    {
        const float run = std::sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
        return run > 1e-3f ? std::fabs(bz - az) / run : 0.f;
    }
    /** The ramp's height and how far the cell is from its core strip (0 on it); false when the cell is past the shoulders. */
    inline bool rampAt(const RampShape& r, float px, float py, float& z, float& d)
    {
        const float dx = r.bx - r.ax, dy = r.by - r.ay, l2 = dx * dx + dy * dy;
        if (l2 < 1e-6f) return false;
        const float len = std::sqrt(l2);
        const float u = ((px - r.ax) * dx + (py - r.ay) * dy) / l2;
        const float uc = std::min(std::max(u, 0.f), 1.f);
        z = r.az + (r.bz - r.az) * uc;
        const float side = std::fabs((px - r.ax) * dy - (py - r.ay) * dx) / len;   // distance across
        const float past = u < 0.f ? -u * len : u > 1.f ? (u - 1.f) * len : 0.f;   // distance beyond an end
        const float across = std::max(0.f, side - r.width * 0.5f);
        d = std::sqrt(across * across + past * past);
        return d <= r.shoulder && !(d > 0.f && r.shoulder <= 0.f);
    }
    inline Grid ramp(const Grid& ground, const RampShape& r, int region_w)
    {
        const float reach = r.width * 0.5f + std::max(0.f, r.shoulder);
        Box b{ (int)std::floor(std::min(r.ax, r.bx) - reach), (int)std::floor(std::min(r.ay, r.by) - reach),
               (int)std::ceil(std::max(r.ax, r.bx) + reach), (int)std::ceil(std::max(r.ay, r.by) + reach) };
        Grid t = areaGrid(b, 0, region_w);
        for (int y = t.y0; y < t.y0 + t.h; ++y)
            for (int x = t.x0; x < t.x0 + t.w; ++x)
            {
                const float g = ground.at(x, y);
                float z, d;
                if (!has(g) || !rampAt(r, (float)x, (float)y, z, d)) continue;
                t.ref(x, y) = d <= 0.f ? z : g + (z - g) * fall(d / r.shoulder);
            }
        return t;
    }

    // ---- Terrace: the slopes cut into level shelves ------------------------------------------------------------------------

    /** One height terraced: shelves `step` metres apart. `sharpness` 0 leaves the slope, 1 cuts crisp shelves; between, each
     *  step's slope is bent down towards its shelf (f^p, p = 1 / (1 - sharpness)). Never higher than the ground (it cuts). */
    inline float terraced(float g, float step, float sharpness)
    {
        if (step <= 0.f) return g;
        const float q = g / step, n = std::floor(q), f = q - n;
        float r;
        if (sharpness >= 1.f) r = 0.f;
        else if (sharpness <= 0.f) r = f;
        else r = std::pow(f, 1.f / (1.f - sharpness));
        return (n + r) * step;
    }
    inline Grid terrace(const Grid& ground, const Box& box, float step, float sharpness, float edge, int region_w)
    {
        Grid t = areaGrid(box, (int)std::ceil(std::max(0.f, edge)), region_w);
        for (int y = t.y0; y < t.y0 + t.h; ++y)
            for (int x = t.x0; x < t.x0 + t.w; ++x)
            {
                const float g = ground.at(x, y);
                if (!has(g)) continue;
                const float d = outside(box, x, y), z = terraced(g, step, sharpness);
                if (d == 0.f) t.ref(x, y) = z;
                else if (edge > 0.f && d < edge) t.ref(x, y) = g + (z - g) * fall(d / edge);
            }
        return t;
    }

    // ---- Smooth+: a Gaussian blur of the radius asked for (the region's own is 9 x 9 m at most and stops at 0 m:
    // SmoothSphere.cs:45, :98), eased in over the box's edge so no step is left there ------------------------------------------

    inline Grid smooth(const Grid& ground, const Box& box, int radius, float strength, int region_w)
    {
        radius = std::max(1, radius);
        Grid t = areaGrid(box, 0, region_w);
        if (!t.w || !t.h) return t;
        const float sigma = std::max(0.5f, (float)radius * 0.5f);
        std::vector<float> k((size_t)(2 * radius + 1));
        for (int i = -radius; i <= radius; ++i) k[(size_t)(i + radius)] = std::exp(-(float)(i * i) / (2.f * sigma * sigma));
        // pass 1: along x over the box's rows grown by the radius in y (the cells the pass along y needs)
        const int gy0 = t.y0 - radius, gh = t.h + 2 * radius;
        Grid across(t.x0, gy0, t.w, gh);
        for (int y = gy0; y < gy0 + gh; ++y)
            for (int x = t.x0; x < t.x0 + t.w; ++x)
            {
                float s = 0.f, ws = 0.f;
                for (int i = -radius; i <= radius; ++i)
                {
                    const float g = ground.at(x + i, y);
                    if (!has(g)) continue;
                    s += g * k[(size_t)(i + radius)];
                    ws += k[(size_t)(i + radius)];
                }
                if (ws > 0.f) across.ref(x, y) = s / ws;
            }
        const int taper = std::max(1, radius / 2);
        for (int y = t.y0; y < t.y0 + t.h; ++y)
            for (int x = t.x0; x < t.x0 + t.w; ++x)
            {
                const float g = ground.at(x, y);
                if (!has(g)) continue;
                float s = 0.f, ws = 0.f;
                for (int i = -radius; i <= radius; ++i)
                {
                    const float a = across.at(x, y + i);
                    if (!has(a)) continue;
                    s += a * k[(size_t)(i + radius)];
                    ws += k[(size_t)(i + radius)];
                }
                if (ws <= 0.f) continue;
                const float edge_w = 1.f - fall((float)(inward(box, x, y) + 1) / (float)taper);
                t.ref(x, y) = g + (s / ws - g) * std::min(std::max(strength, 0.f), 1.f) * edge_w;
            }
        return t;
    }

    // ---- routes (Road / River): a line of clicked points ------------------------------------------------------------------

    struct Polyline
    {
        std::vector<float> x, y, cum;   // the points and the length along the line to each
        float len = 0.f;
        explicit Polyline(const std::vector<float>& xy)
        {
            for (size_t i = 0; i + 1 < xy.size(); i += 2)
            {
                if (!x.empty())
                {
                    const float d = std::sqrt((xy[i] - x.back()) * (xy[i] - x.back()) + (xy[i + 1] - y.back()) * (xy[i + 1] - y.back()));
                    if (d < 1e-3f) continue;
                    len += d;
                }
                x.push_back(xy[i]);
                y.push_back(xy[i + 1]);
                cum.push_back(len);
            }
        }
        bool usable() const { return x.size() >= 2 && len > 0.f; }
        /** The nearest point of the line to (px, py): its distance, and the length along the line there. */
        void nearest(float px, float py, float& dist, float& along) const
        {
            dist = 1e30f;
            along = 0.f;
            for (size_t i = 1; i < x.size(); ++i)
            {
                const float dx = x[i] - x[i - 1], dy = y[i] - y[i - 1], l2 = dx * dx + dy * dy;
                const float u = std::min(std::max(((px - x[i - 1]) * dx + (py - y[i - 1]) * dy) / l2, 0.f), 1.f);
                const float ex = x[i - 1] + dx * u - px, ey = y[i - 1] + dy * u - py;
                const float d = std::sqrt(ex * ex + ey * ey);
                if (d < dist) { dist = d; along = cum[i - 1] + std::sqrt(l2) * u; }
            }
        }
        void at(float s, float& px, float& py) const
        {
            s = std::min(std::max(s, 0.f), len);
            size_t i = 1;
            while (i + 1 < x.size() && cum[i] < s) ++i;
            const float seg = cum[i] - cum[i - 1], u = seg > 0.f ? (s - cum[i - 1]) / seg : 0.f;
            px = x[i - 1] + (x[i] - x[i - 1]) * u;
            py = y[i - 1] + (y[i] - y[i - 1]) * u;
        }
        /** The bounding box grown by `reach`. */
        Box bounds(float reach) const
        {
            float lx = x[0], hx = x[0], ly = y[0], hy = y[0];
            for (size_t i = 1; i < x.size(); ++i) { lx = std::min(lx, x[i]); hx = std::max(hx, x[i]); ly = std::min(ly, y[i]); hy = std::max(hy, y[i]); }
            return Box{ (int)std::floor(lx - reach), (int)std::floor(ly - reach), (int)std::ceil(hx + reach), (int)std::ceil(hy + reach) };
        }
    };

    /** A height profile along a line: one value a metre (samples at i * len / n). */
    struct Profile
    {
        float len = 0.f;
        std::vector<float> z;
        float at(float s) const
        {
            if (z.empty()) return nan();
            if (z.size() == 1 || len <= 0.f) return z[0];
            const float f = std::min(std::max(s / len, 0.f), 1.f) * (float)(z.size() - 1);
            const size_t i = std::min((size_t)f, z.size() - 2);
            const float u = f - (float)i;
            return z[i] * (1.f - u) + z[i + 1] * u;
        }
        float step() const { return z.size() > 1 ? len / (float)(z.size() - 1) : 0.f; }
    };

    /** The ground along the line, a sample about every metre; false when some of it is not known. */
    inline bool groundProfile(const Grid& ground, const Polyline& line, Profile& out)
    {
        const int n = std::max(1, (int)std::ceil(line.len));
        out.len = line.len;
        out.z.assign((size_t)n + 1, 0.f);
        for (int i = 0; i <= n; ++i)
        {
            float px, py;
            line.at(line.len * (float)i / (float)n, px, py);
            const float g = sample(ground, px, py);
            if (!has(g)) return false;
            out.z[(size_t)i] = g;
        }
        return true;
    }

    /** A Gaussian smoothing of a profile (sigma in metres), the ends kept as they were. */
    inline Profile smoothProfile(const Profile& p, float sigma_m)
    {
        Profile o = p;
        const float ds = p.step();
        if (p.z.size() < 3 || ds <= 0.f || sigma_m <= 0.f) return o;
        const float sigma = sigma_m / ds;
        const int r = std::max(1, (int)std::ceil(2.5f * sigma));
        for (size_t i = 1; i + 1 < p.z.size(); ++i)
        {
            float s = 0.f, ws = 0.f;
            for (int k = -r; k <= r; ++k)
            {
                const long j = (long)i + k;
                if (j < 0 || j >= (long)p.z.size()) continue;
                const float w = std::exp(-(float)(k * k) / (2.f * sigma * sigma));
                s += p.z[(size_t)j] * w;
                ws += w;
            }
            o.z[i] = s / ws;
        }
        return o;
    }

    /** No piece of the profile steeper than `max_grade` (rise over run): a pass each way, so the steepest places are eased
     *  from both sides. */
    inline void limitGrade(Profile& p, float max_grade)
    {
        const float ds = p.step();
        if (p.z.size() < 2 || ds <= 0.f || max_grade <= 0.f) return;
        const float m = max_grade * ds;
        for (size_t i = 1; i < p.z.size(); ++i) p.z[i] = std::min(std::max(p.z[i], p.z[i - 1] - m), p.z[i - 1] + m);
        for (size_t i = p.z.size() - 1; i-- > 0;) p.z[i] = std::min(std::max(p.z[i], p.z[i + 1] - m), p.z[i + 1] + m);
    }
    /** The steepest piece of a profile (rise over run). */
    inline float steepest(const Profile& p)
    {
        const float ds = p.step();
        float g = 0.f;
        for (size_t i = 1; i < p.z.size() && ds > 0.f; ++i) g = std::max(g, std::fabs(p.z[i] - p.z[i - 1]) / ds);
        return g;
    }

    /** Road / path: `follow` the ground (smoothed over twice the width, then no steeper than `max_grade`) or one even grade
     *  from end to end. Flat across (no camber). */
    inline Profile roadProfile(const Profile& ground_line, float width, bool follow, float max_grade)
    {
        Profile p = ground_line;
        if (p.z.empty()) return p;
        if (follow)
        {
            // a road's grade changes over about two of its widths (the window's own choice of smoothing length)
            p = smoothProfile(p, 2.f * std::max(1.f, width));
            limitGrade(p, max_grade);
        }
        else
        {
            const float a = p.z.front(), b = p.z.back();
            for (size_t i = 0; i < p.z.size(); ++i) p.z[i] = a + (b - a) * (float)i / (float)std::max<size_t>(1, p.z.size() - 1);
        }
        return p;
    }

    /** River: the bed `depth` under the (smoothed) ground, and never rising downstream; `sea_bed` (not NaN) makes it run down
     *  to that height at its last point, evenly from its first. */
    inline Profile riverBed(const Profile& ground_line, float width, float depth, float sea_bed)
    {
        Profile p = smoothProfile(ground_line, std::max(1.f, width));
        if (p.z.empty()) return p;
        for (size_t i = 0; i < p.z.size(); ++i) p.z[i] -= depth;
        for (size_t i = 1; i < p.z.size(); ++i) p.z[i] = std::min(p.z[i], p.z[i - 1]);
        if (has(sea_bed))
        {
            const float a = p.z.front(), b = std::min(p.z.back(), sea_bed);
            for (size_t i = 0; i < p.z.size(); ++i)
                p.z[i] = std::min(p.z[i], a + (b - a) * (float)i / (float)std::max<size_t>(1, p.z.size() - 1));
        }
        return p;
    }

    /** The road across: flat on the strip, the banks eased from the road's height to the ground over `shoulder`. */
    inline Grid road(const Grid& ground, const Polyline& line, const Profile& prof, float width, float shoulder, int region_w)
    {
        const float half = width * 0.5f, reach = half + std::max(0.f, shoulder);
        Grid t = areaGrid(line.bounds(reach), 0, region_w);
        for (int y = t.y0; y < t.y0 + t.h; ++y)
            for (int x = t.x0; x < t.x0 + t.w; ++x)
            {
                const float g = ground.at(x, y);
                if (!has(g)) continue;
                float d, s;
                line.nearest((float)x, (float)y, d, s);
                if (d > reach) continue;
                const float z = prof.at(s);
                t.ref(x, y) = d <= half ? z : g + (z - g) * fall((d - half) / shoulder);
            }
        return t;
    }

    /** The river's channel across: a U from the bed up `depth` at its edges, soft banks out to `banks`; it only cuts. */
    inline float riverAcross(float bed, float depth, float half, float banks, float d, float g)
    {
        float z;
        if (d <= half) { const float u = half > 0.f ? d / half : 0.f; z = bed + depth * u * u; }
        else z = g + (bed + depth - g) * fall(banks > 0.f ? (d - half) / banks : 1.f);
        return std::min(z, g);
    }
    inline Grid river(const Grid& ground, const Polyline& line, const Profile& bed, float width, float depth, float banks, int region_w)
    {
        const float half = width * 0.5f, reach = half + std::max(0.f, banks);
        Grid t = areaGrid(line.bounds(reach), 0, region_w);
        for (int y = t.y0; y < t.y0 + t.h; ++y)
            for (int x = t.x0; x < t.x0 + t.w; ++x)
            {
                const float g = ground.at(x, y);
                if (!has(g)) continue;
                float d, s;
                line.nearest((float)x, (float)y, d, s);
                if (d > reach) continue;
                t.ref(x, y) = riverAcross(bed.at(s), depth, half, banks, d, g);
            }
        return t;
    }

    // ---- Beach / coastline: the shore made one even slope through the water line -------------------------------------------

    /** Each cell's distance, metres, to the nearest cell on the other side of the water line (land: ground >= water), signed:
     *  + on land, - in the water; a two-pass chamfer (steps 1 and sqrt 2) over the grid, measured to the line between the two
     *  cells (the half cell taken off). NaN where the ground is not known, everywhere when the grid has no water line. */
    inline Grid shoreDistance(const Grid& ground, float water)
    {
        Grid out(ground.x0, ground.y0, ground.w, ground.h);
        const float BIG = 1e9f, D1 = 1.f, D2 = 1.41421356f;
        std::vector<float> to_sea(out.cells(), BIG), to_land(out.cells(), BIG);
        bool any_land = false, any_sea = false;
        for (size_t i = 0; i < out.cells(); ++i)
        {
            if (!has(ground.v[i])) continue;
            if (ground.v[i] >= water) { any_land = true; to_land[i] = 0.f; } else { any_sea = true; to_sea[i] = 0.f; }
        }
        if (!any_land || !any_sea) return out;
        auto pass = [&](std::vector<float>& d)
        {
            const int w = out.w, h = out.h;
            auto at = [&](int x, int y) -> float& { return d[(size_t)y * (size_t)w + (size_t)x]; };
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                    float& c = at(x, y);
                    if (x > 0) c = std::min(c, at(x - 1, y) + D1);
                    if (y > 0) c = std::min(c, at(x, y - 1) + D1);
                    if (x > 0 && y > 0) c = std::min(c, at(x - 1, y - 1) + D2);
                    if (x + 1 < w && y > 0) c = std::min(c, at(x + 1, y - 1) + D2);
                }
            for (int y = h - 1; y >= 0; --y)
                for (int x = w - 1; x >= 0; --x)
                {
                    float& c = at(x, y);
                    if (x + 1 < w) c = std::min(c, at(x + 1, y) + D1);
                    if (y + 1 < h) c = std::min(c, at(x, y + 1) + D1);
                    if (x + 1 < w && y + 1 < h) c = std::min(c, at(x + 1, y + 1) + D2);
                    if (x > 0 && y + 1 < h) c = std::min(c, at(x - 1, y + 1) + D2);
                }
        };
        pass(to_sea);
        pass(to_land);
        for (size_t i = 0; i < out.cells(); ++i)
        {
            if (!has(ground.v[i])) continue;
            out.v[i] = ground.v[i] >= water ? to_sea[i] - 0.5f : -(to_land[i] - 0.5f);
        }
        return out;
    }

    /** The beach: inside the box, the ground within `above` of the water line on land and `below` it in the water lies on one
     *  slope (`slope` rise over run) through the water line; beyond that band it is eased back to the ground over `edge`, and
     *  [2026-10-23 Paul's Beach: "tall vertical walls ... where sand meets the higher ground"] it is eased in from the box's own
     *  sides over `edge` too, so the finished beach meets the ground outside the box without a step. */
    inline Grid beach(const Grid& ground, const Box& box, float water, float slope, float above, float below, float edge, int region_w)
    {
        Grid area = areaGrid(box, 0, region_w);
        Grid local(area.x0, area.y0, area.w, area.h);
        for (int y = area.y0; y < area.y0 + area.h; ++y)
            for (int x = area.x0; x < area.x0 + area.w; ++x) local.ref(x, y) = ground.at(x, y);
        const Grid sd = shoreDistance(local, water);
        Grid t = area;
        if (slope <= 0.f) return t;
        const float land_band = above / slope, sea_band = below / slope;
        for (int y = t.y0; y < t.y0 + t.h; ++y)
            for (int x = t.x0; x < t.x0 + t.w; ++x)
            {
                const float g = local.at(x, y), d = sd.at(x, y);
                if (!has(g) || !has(d)) continue;
                const float z = std::min(std::max(water + slope * d, water - below), water + above);
                const float past = d >= 0.f ? d - land_band : -d - sea_band;   // metres beyond the beach's band
                float w = past <= 0.f ? 1.f : (edge > 0.f && past < edge) ? fall(past / edge) : 0.f;
                // from the box's side inwards: the outermost cell keeps almost all of the ground, `edge` metres in none of it
                if (edge > 0.f) w *= 1.f - fall((float)(inward(box, x, y) + 1) / edge);
                if (w > 0.f) t.ref(x, y) = g + (z - g) * w;
            }
        return t;
    }
}

#endif // WOLF_TERRAFORM_MATH_H
