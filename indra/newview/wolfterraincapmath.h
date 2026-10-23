/**
 * @file wolfterraincapmath.h
 * @brief WolfViewer: how the terrain writer's points become WolfTerraform capability requests (pure, tested off-line).
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

#ifndef WOLF_TERRAIN_CAP_MATH_H
#define WOLF_TERRAIN_CAP_MATH_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <utility>
#include <vector>

#include "wolfterraformmath.h"

/**
 * [WOLF TERRAFORM 2026-10-23] DESIGN.md 3.4 / 3.7 Phase 3. The region's WolfTerraform capability (WolfSim
 * OptionalModules/World/WolfTerraform/WolfTerraformModule.cs) takes a rectangle of heights per request:
 *   {op:"apply", x, y, w, h, heights: binary float32 little-endian, w*h row by row from y (NaN = leave alone), group, label}
 * and answers `cells`, one byte per cell (0 set, 1 left alone, 2 refused: no right to terraform there, 3 set but held at the
 * estate's limit). The region refuses a rectangle over 256 x 256 m or one touching more than 4 terrain tiles
 * (TerrainModule.WolfTerraform.cs WolfMaxApplySide, WolfMaxTilesPerRequest; tiles are 256 m, TerrainHeightmap.TileEdge).
 * Batches here are cut on that 256 m grid, so each request is one tile and never more.
 */
namespace WolfTerrainCapMath
{
    // Source: TerrainModule.WolfTerraform.cs WolfMaxApplySide = 256; TerrainHeightmap.cs TileEdge = Constants.RegionSize = 256.
    constexpr int BLOCK = 256;
    // Source: TerrainModule.WolfTerraform.cs WolfCellWritten / WolfCellLeft / WolfCellRights / WolfCellLimit.
    constexpr uint8_t CELL_SET = 0, CELL_LEFT = 1, CELL_RIGHTS = 2, CELL_LIMIT = 3;

    /** One request: the tight rectangle round the points of one 256 m block. */
    struct Batch
    {
        int x0 = 0, y0 = 0, w = 0, h = 0;
        std::vector<float> heights;               // w * h, row by row from y0; NaN where no point
        std::vector<WolfTerraformMath::Point> pts; // the points in it, in the order given
        float& at(int x, int y) { return heights[(size_t)(y - y0) * (size_t)w + (size_t)(x - x0)]; }
    };

    /** Points to batches, one per 256 m block the points touch, row of blocks by row (a later point for the same cell wins). */
    inline std::vector<Batch> makeBatches(const std::vector<WolfTerraformMath::Point>& pts)
    {
        std::map<std::pair<int, int>, std::vector<WolfTerraformMath::Point>> blocks;   // key (by, bx): south to north
        for (const WolfTerraformMath::Point& p : pts)
        {
            if (p.x < 0 || p.y < 0 || !std::isfinite(p.z)) continue;   // never in a region; the region would refuse it
            blocks[{ p.y / BLOCK, p.x / BLOCK }].push_back(p);
        }
        std::vector<Batch> out;
        for (auto& kv : blocks)
        {
            Batch b;
            int x1 = std::numeric_limits<int>::min(), y1 = x1;
            b.x0 = std::numeric_limits<int>::max(); b.y0 = b.x0;
            for (const WolfTerraformMath::Point& p : kv.second)
            {
                b.x0 = std::min(b.x0, p.x); b.y0 = std::min(b.y0, p.y);
                x1 = std::max(x1, p.x); y1 = std::max(y1, p.y);
            }
            b.w = x1 - b.x0 + 1;
            b.h = y1 - b.y0 + 1;
            b.heights.assign((size_t)b.w * (size_t)b.h, std::numeric_limits<float>::quiet_NaN());
            for (const WolfTerraformMath::Point& p : kv.second) b.at(p.x, p.y) = p.z;
            b.pts = std::move(kv.second);
            out.push_back(std::move(b));
        }
        return out;
    }

    /** The heights as the region reads them: float32 little-endian (WolfTerraformModule.Apply, BinaryPrimitives.ReadSingleLittleEndian). */
    inline std::vector<uint8_t> encode(const std::vector<float>& heights)
    {
        std::vector<uint8_t> out(heights.size() * 4);
        for (size_t i = 0; i < heights.size(); ++i)
        {
            uint32_t u;
            std::memcpy(&u, &heights[i], 4);
            out[i * 4 + 0] = (uint8_t)(u & 0xff);
            out[i * 4 + 1] = (uint8_t)((u >> 8) & 0xff);
            out[i * 4 + 2] = (uint8_t)((u >> 16) & 0xff);
            out[i * 4 + 3] = (uint8_t)((u >> 24) & 0xff);
        }
        return out;
    }

    /** The batch's points the region did not set to their height (refused, or held at a limit), from its `cells` answer, with
     *  each one's cell code in `codes`. No `cells` means the region set them all (it sends them only when something was
     *  refused or held); an answer of another size counts every point as refused. */
    inline std::vector<WolfTerraformMath::Point> notSet(const Batch& b, const std::vector<uint8_t>& cells, std::vector<uint8_t>& codes)
    {
        std::vector<WolfTerraformMath::Point> out;
        codes.clear();
        if (cells.empty()) return out;
        const bool whole = cells.size() == (size_t)b.w * (size_t)b.h;
        for (const WolfTerraformMath::Point& p : b.pts)
        {
            const uint8_t c = whole ? cells[(size_t)(p.y - b.y0) * (size_t)b.w + (size_t)(p.x - b.x0)] : CELL_RIGHTS;
            if (c == CELL_RIGHTS || c == CELL_LIMIT) { out.push_back(p); codes.push_back(c); }
        }
        return out;
    }
}

#endif // WOLF_TERRAIN_CAP_MATH_H
