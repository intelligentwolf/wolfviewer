/** Circular wave brush over the service's region-local cell grid. */
#ifndef WOLF_WAVE_BRUSH_H
#define WOLF_WAVE_BRUSH_H

#include <algorithm>
#include <cmath>
#include <vector>

namespace WolfWaveBrush
{
// UI bounds for Flying Dragon's brush diameter, measured in 16 m paint cells
// (WolfWaveZones::PAINT_CELL_M, the same on every region since layout v2, 2026-09-26).
constexpr int MIN_DIAMETER = 1;
constexpr int MAX_DIAMETER = 8;

// Source: WolfWaveZones::Region pw()/ph() and php/waves.php waves_paint_dims().
// Visit each touched cell once per segment. Half-cell sampling keeps fast drags continuous;
// clipping is at the region boundary, never into a neighbour's independently saved layout.
// <WolfViewer 2026-09-26> The paint grid is 28,800 cells an edge on Ireland, so the once-only
// set covers the segment's bounding box, not the region.
template <typename Visitor>
void visit(int w, int h, int cell, int diameter, double ax, double ay,
           double bx, double by, Visitor visitor)
{
    if (w < 1 || h < 1 || w > 65536 || h > 65536 || cell < 1 ||   // 65536 x 16 m: the largest region handle span
        diameter < MIN_DIAMETER || diameter > MAX_DIAMETER ||
        !std::isfinite(ax) || !std::isfinite(ay) || !std::isfinite(bx) || !std::isfinite(by) ||
        ax < 0 || ay < 0 || bx < 0 || by < 0 ||
        ax >= double(w) * cell || bx >= double(w) * cell ||
        ay >= double(h) * cell || by >= double(h) * cell) return;
    const double radius = double(diameter) * cell * 0.5;
    const int steps = std::max(1, int(std::ceil(std::hypot(bx - ax, by - ay) / (cell * 0.5))));
    const int bx0 = std::max(0, int(std::floor((std::min(ax, bx) - radius) / cell)));
    const int by0 = std::max(0, int(std::floor((std::min(ay, by) - radius) / cell)));
    const int bx1 = std::min(w - 1, int(std::floor((std::max(ax, bx) + radius) / cell)));
    const int by1 = std::min(h - 1, int(std::floor((std::max(ay, by) + radius) / cell)));
    const int bw = bx1 - bx0 + 1;
    std::vector<bool> seen(size_t(bw) * (by1 - by0 + 1), false);
    for (int step = 0; step <= steps; ++step)
    {
        const double t = double(step) / steps;
        const double x = ax + (bx - ax) * t, y = ay + (by - ay) * t;
        const int left = std::max(0, int(std::floor((x - radius) / cell)));
        const int right = std::min(w - 1, int(std::floor((x + radius) / cell)));
        const int bottom = std::max(0, int(std::floor((y - radius) / cell)));
        const int top = std::min(h - 1, int(std::floor((y + radius) / cell)));
        for (int cy = bottom; cy <= top; ++cy)
            for (int cx = left; cx <= right; ++cx)
            {
                const double dx = std::max({double(cx) * cell - x, 0.0, x - double(cx + 1) * cell});
                const double dy = std::max({double(cy) * cell - y, 0.0, y - double(cy + 1) * cell});
                const size_t index = size_t(cy - by0) * bw + (cx - bx0);
                if (!seen[index] && dx * dx + dy * dy < radius * radius)
                {
                    seen[index] = true;
                    visitor(cx, cy);
                }
            }
    }
}
}
#endif
