/**
 * @file llviewerlayer.cpp
 * @brief LLViewerLayer class implementation
 *
 * $LicenseInfo:firstyear=2002&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"
#include "llterraingridoffset.h"

#include "llviewerlayer.h"
#include "llerror.h"
#include "llmath.h"

LLViewerLayer::LLViewerLayer(const S32 width, const F32 scale)
{
    mWidth = width;
    mScale = scale;
    mScaleInv = 1.f/scale;

    // <WolfViewer 2026-09-23> See the note on mPages in llviewerlayer.h: only the page
    // directory is made here. (It replaced the 2026-09-09 calloc of width x width floats, which
    // itself replaced an eager loop that wrote all of them.)
    const S32 page_texels = TILE_EDGE * PAGE_TILES;
    mPagesPerEdge = (width + page_texels - 1) / page_texels;
    mPages.resize((size_t)mPagesPerEdge * mPagesPerEdge);
    // </WolfViewer>
}

LLViewerLayer::~LLViewerLayer()
{
}

F32 LLViewerLayer::getValue(const S32 x, const S32 y) const
{
//  llassert(x >= 0);
//  llassert(x < mWidth);
//  llassert(y >= 0);
//  llassert(y < mWidth);

    // <WolfViewer 2026-09-23> page -> tile -> texel; anything not written yet is 0.
    if (x < 0 || y < 0 || x >= mWidth || y >= mWidth)
    {
        return 0.f;
    }
    const S32 tx = x / TILE_EDGE, ty = y / TILE_EDGE;
    const auto& page = mPages[(size_t)(ty / PAGE_TILES) * mPagesPerEdge + (tx / PAGE_TILES)];
    if (!page)
    {
        return 0.f;
    }
    const auto& tile = page[(ty % PAGE_TILES) * PAGE_TILES + (tx % PAGE_TILES)];
    if (!tile)
    {
        return 0.f;
    }
    return tile[(y % TILE_EDGE) * TILE_EDGE + (x % TILE_EDGE)];
    // </WolfViewer>
}

// <WolfViewer 2026-09-23> See llviewerlayer.h.
void LLViewerLayer::setValue(const S32 x, const S32 y, const F32 value)
{
    if (x < 0 || y < 0 || x >= mWidth || y >= mWidth)
    {
        return;
    }
    const S32 tx = x / TILE_EDGE, ty = y / TILE_EDGE;
    auto& page = mPages[(size_t)(ty / PAGE_TILES) * mPagesPerEdge + (tx / PAGE_TILES)];
    if (!page)
    {
        page.reset(new std::unique_ptr<F32[]>[PAGE_TILES * PAGE_TILES]);
    }
    auto& tile = page[(ty % PAGE_TILES) * PAGE_TILES + (tx % PAGE_TILES)];
    if (!tile)
    {
        tile.reset(new F32[TILE_EDGE * TILE_EDGE]());
    }
    tile[(y % TILE_EDGE) * TILE_EDGE + (x % TILE_EDGE)] = value;
}
// </WolfViewer>

F32 LLViewerLayer::getValueScaled(const F32 x, const F32 y) const
{
    S32 x1, x2, y1, y2;
    F32 x_frac, y_frac;

    x_frac = x*mScaleInv;
    x1 = llfloor(x_frac);
    x2 = x1 + 1;
    x_frac -= x1;

    y_frac = y*mScaleInv;
    y1 = llfloor(y_frac);
    y2 = y1 + 1;
    y_frac -= y1;

    x1 = llmin((S32)mWidth-1, x1);
    x1 = llmax(0, x1);
    x2 = llmin((S32)mWidth-1, x2);
    x2 = llmax(0, x2);
    y1 = llmin((S32)mWidth-1, y1);
    y1 = llmax(0, y1);
    y2 = llmin((S32)mWidth-1, y2);
    y2 = llmax(0, y2);

    // Take weighted average of all four points (bilinear interpolation)
    // <WolfViewer 2026-09-23/> through getValue: the texels are in sparse tiles now
    F32 row1_left  = getValue(x1, y1);
    F32 row1_right = getValue(x2, y1);
    F32 row2_left  = getValue(x1, y2);
    F32 row2_right = getValue(x2, y2);

    F32 row1_interp = row1_left - x_frac * (row1_left - row1_right);
    F32 row2_interp = row2_left - x_frac * (row2_left - row2_right);

    return row1_interp - y_frac * (row1_interp - row2_interp);
}
