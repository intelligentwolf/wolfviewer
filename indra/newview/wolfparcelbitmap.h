/**
 * @file wolfparcelbitmap.h
 * @brief WolfViewer: parcel bitmaps from ParcelProperties, and parcel edge segments, for any size of region.
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

#ifndef WOLF_PARCELBITMAP_H
#define WOLF_PARCELBITMAP_H

#include <vector>

#include "stdtypes.h"
#include "wolfcellquadtree.h"

// A parcel bitmap: 1 where the parcel is, per 4 m cell.
typedef WolfCellQuadTree<U8> WolfParcelCells;

// [2026-09-23] Decode the ParcelProperties "Bitmap" field into cells_x x cells_y cells.
//
// Two encodings arrive, both written by OpenSimWolf's OpenSim.Framework.LandBitmap:
//  - LEGACY: one bit per cell, X fastest, LSB first, cells_x wide rows - what every viewer has
//    always read. Used for every region up to 102,400 m.
//  - COMPACT, for larger regions, where the legacy form is gigabytes per parcel: "OSLB",
//    version 1, int32 width, int32 height (cells, little-endian), then a gzip stream of the
//    parcel's quadtree in preorder: 0 = uniform empty, 1 = uniform set, 2 = split (SW, SE, NW,
//    NE follow), 3 = a 64 x 64 leaf of bits (X fastest, LSB first).
// Bytes that start with the compact magic but do not decode as a compact bitmap are read as
// legacy (a legacy bitmap can begin "OSLB"). Returns false (out empty) only for no data.
bool wolfDecodeParcelBitmap(const U8* data, S32 size, S32 cells_x, S32 cells_y, WolfParcelCells& out);

// Parcel edge segments: which cell edges to draw (LLViewerParcelMgr's SOUTH_MASK / WEST_MASK).
//
// The parcel manager kept these as a (cells + 1)^2 byte grid - 655 MB at 102,400 m, wrapped to a
// few hundred KB and overrun at 1,048,576 m - and the renderers scanned every byte of it every
// frame. Every way it is written, though, draws the BOUNDARY of a set of cells: a selection
// rectangle, or a parcel bitmap (writeSegmentsFromBitmap XORed each set cell's four sides, so
// sides shared by two set cells cancelled). So the edges are kept as runs along grid lines.
class WolfParcelSegments
{
public:
    struct Run
    {
        S32 mX, mY;     // first cell edge: a SOUTH run lies along y = mY, a WEST run along x = mX
        S32 mLength;    // cells
        U8  mMask;      // the segment mask each cell of the run carried (SOUTH_MASK or WEST_MASK)
    };

    void clear() { mRuns.clear(); }
    bool empty() const { return mRuns.empty(); }

    // LLViewerParcelMgr::writeHighlightSegments: the outline of cells [min_x, max_x) x
    // [min_y, max_y). As with the XOR grid, a zero-width side and the side opposite cancel.
    void addRectangle(S32 min_x, S32 min_y, S32 max_x, S32 max_y, U8 south_mask, U8 west_mask);

    // LLViewerParcelMgr::writeSegmentsFromBitmap: every edge between a set cell and an unset (or
    // off-grid) one.
    void addBoundary(const WolfParcelCells& cells, U8 south_mask, U8 west_mask);

    const std::vector<Run>& runs() const { return mRuns; }

private:
    std::vector<Run> mRuns;
};

#endif // WOLF_PARCELBITMAP_H
