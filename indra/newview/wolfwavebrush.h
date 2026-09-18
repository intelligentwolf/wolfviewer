/** Circular wave brush over the service's region-local cell grid. */
#ifndef WOLF_WAVE_BRUSH_H
#define WOLF_WAVE_BRUSH_H

#include <algorithm>
#include <cmath>
#include <vector>

namespace WolfWaveBrush
{
// UI bounds for Flying Dragon's brush diameter, measured in stored wave cells.
constexpr int MIN_DIAMETER = 1;
constexpr int MAX_DIAMETER = 8;

// Source: WolfWaveZones::Region w()/h()/mCell and php/waves.php waves_cell().
// Visit each touched cell once per segment. Half-cell sampling keeps fast drags continuous;
// clipping is at the region boundary, never into a neighbour's independently saved layout.
template <typename Visitor>
void visit(int w, int h, int cell, int diameter, double ax, double ay,
           double bx, double by, Visitor visitor)
{
    if (w < 1 || h < 1 || w > 1024 || h > 1024 || cell < 1 ||   // 1024: WolfWaveZones::MAX_CELLS_EDGE
        diameter < MIN_DIAMETER || diameter > MAX_DIAMETER ||
        !std::isfinite(ax) || !std::isfinite(ay) || !std::isfinite(bx) || !std::isfinite(by) ||
        ax < 0 || ay < 0 || bx < 0 || by < 0 ||
        ax >= double(w) * cell || bx >= double(w) * cell ||
        ay >= double(h) * cell || by >= double(h) * cell) return;
    const double radius = double(diameter) * cell * 0.5;
    const int steps = std::max(1, int(std::ceil(std::hypot(bx - ax, by - ay) / (cell * 0.5))));
    std::vector<bool> seen(size_t(w) * h, false);
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
                const size_t index = size_t(cy) * w + cx;
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
