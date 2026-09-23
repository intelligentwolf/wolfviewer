/**
 * @file llviewerlayer.h
 * @brief LLViewerLayer class header file
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
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

#ifndef LL_LLVIEWERLAYER_H
#define LL_LLVIEWERLAYER_H

// Viewer-side representation of a layer...

#include <memory>
#include <vector>

class LLViewerLayer
{
public:
    LLViewerLayer(const S32 width, const F32 scale = 1.f);
    virtual ~LLViewerLayer();

    F32 getValueScaled(const F32 x, const F32 y) const;
protected:
    F32 getValue(const S32 x, const S32 y) const;
    // <WolfViewer 2026-09-23/> the only way to write a texel now (see mPages)
    void setValue(const S32 x, const S32 y, const F32 value);
protected:
    S32 mWidth;
    F32 mScale;
    F32 mScaleInv;
private:
    // <WolfViewer 2026-09-23> Sparse storage. This was one calloc'd width x width float array:
    // 4 bytes per square metre, reserved not committed (the 2026-09-09 change), which is 4.4 TB
    // of address space at 1,048,576 m and refused. Texels live in TILE_EDGE^2 tiles, grouped in
    // pages of PAGE_TILES^2 tile pointers; a tile is made when a texel in it is first written
    // (generateHeights, per patch), and a texel never written reads 0, as the zero pages did.
    static constexpr S32 TILE_EDGE = 64;
    static constexpr S32 PAGE_TILES = 64;
    S32 mPagesPerEdge;
    std::vector<std::unique_ptr<std::unique_ptr<F32[]>[]>> mPages;
    // </WolfViewer>
};

#endif // LL_LLVIEWERLAYER_H
