/**
 * @file wolfparcelbitmap.cpp
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

#include "llviewerprecompiledheaders.h"

#include "wolfparcelbitmap.h"

#include <cstring>

#include "llerror.h"
#ifdef LL_USESYSTEMLIBS
#include <zlib.h>
#else
#include "zlib-ng/zlib.h"
#endif

namespace
{
    // Source: OpenSimWolf OpenSim/Framework/LandBitmap.cs CompactMagic / CompactVersion /
    // CompactHeaderLength (4 + 1 + 4 + 4).
    const U8 COMPACT_MAGIC[4] = { 'O', 'S', 'L', 'B' };
    const U8 COMPACT_VERSION = 1;
    const S32 COMPACT_HEADER = 13;
    const S32 LEAF_EDGE = WolfParcelCells::LEAF_EDGE;
    const S32 LEAF_BYTES = LEAF_EDGE * LEAF_EDGE / 8;
    // The largest region OpenSimWolf allows (Constants.MaximumRegionSize) in 4 m cells.
    const S32 MAX_CELLS = 1048576 / 4;

    S32 readS32LE(const U8* p)
    {
        return (S32)((U32)p[0] | ((U32)p[1] << 8) | ((U32)p[2] << 16) | ((U32)p[3] << 24));
    }

    // gunzip into out; zlib checks the gzip trailer's CRC32 and length at Z_STREAM_END. Stops
    // (false) past max_out bytes, so a small claimed size cannot carry a decompression bomb.
    bool gunzip(const U8* data, size_t size, std::vector<U8>& out, size_t max_out)
    {
        z_stream strm;
        memset(&strm, 0, sizeof(strm));
        if (inflateInit2(&strm, 16 + MAX_WBITS) != Z_OK)
        {
            return false;
        }
        strm.next_in = const_cast<U8*>(data);
        strm.avail_in = (uInt)size;
        U8 chunk[16384];
        int ret = Z_OK;
        while (ret != Z_STREAM_END)
        {
            strm.next_out = chunk;
            strm.avail_out = sizeof(chunk);
            ret = inflate(&strm, Z_NO_FLUSH);
            if (ret != Z_OK && ret != Z_STREAM_END)
            {
                inflateEnd(&strm);
                return false;
            }
            const size_t produced = sizeof(chunk) - strm.avail_out;
            if (out.size() + produced > max_out)
            {
                inflateEnd(&strm);
                return false;
            }
            out.insert(out.end(), chunk, chunk + produced);
            if (ret == Z_OK && strm.avail_in == 0 && strm.avail_out != 0)
            {
                inflateEnd(&strm);
                return false;   // ran out of input before the end of the stream
            }
        }
        inflateEnd(&strm);
        return strm.avail_in == 0;   // nothing may follow the gzip member
    }

    struct CompactReader
    {
        const std::vector<U8>& mBody;
        size_t mPos = 0;
        WolfParcelCells& mOut;

        // Source: CellQuadTree.BuildNode - a preorder of the whole padded tree.
        bool node(S32 nx, S32 ny, S32 size)
        {
            if (mPos >= mBody.size())
            {
                return false;
            }
            const U8 tag = mBody[mPos++];
            switch (tag)
            {
                case 0:
                    return true;
                case 1:
                    mOut.fillRect(nx, ny, nx + size, ny + size, 1);   // clipped to the grid
                    return true;
                case 2:
                {
                    if (size <= LEAF_EDGE)
                    {
                        return false;
                    }
                    const S32 half = size >> 1;
                    for (S32 k = 0; k < 4; ++k)
                    {
                        if (!node(nx + ((k & 1) ? half : 0), ny + ((k & 2) ? half : 0), half))
                        {
                            return false;
                        }
                    }
                    return true;
                }
                case 3:
                {
                    if (size != LEAF_EDGE || mPos + LEAF_BYTES > mBody.size())
                    {
                        return false;
                    }
                    const U8* bits = &mBody[mPos];
                    mPos += LEAF_BYTES;
                    for (S32 y = 0; y < LEAF_EDGE; ++y)
                    {
                        S32 x = 0;
                        while (x < LEAF_EDGE)
                        {
                            const S32 i = y * LEAF_EDGE + x;
                            if (!(bits[i >> 3] & (1 << (i & 7))))
                            {
                                ++x;
                                continue;
                            }
                            S32 end = x + 1;
                            while (end < LEAF_EDGE)
                            {
                                const S32 j = y * LEAF_EDGE + end;
                                if (!(bits[j >> 3] & (1 << (j & 7))))
                                {
                                    break;
                                }
                                ++end;
                            }
                            mOut.fillRect(nx + x, ny + y, nx + end, ny + y + 1, 1);
                            x = end;
                        }
                    }
                    return true;
                }
                default:
                    return false;
            }
        }
    };
}

namespace
{
    // The compact form into out (already sized to the region's cells). False, with out left to
    // the caller to reset, when data is not a well-formed compact bitmap.
    bool decodeCompact(const U8* data, S32 size, WolfParcelCells& out)
    {
        if (data[4] != COMPACT_VERSION)
        {
            return false;
        }
        const S32 width = readS32LE(data + 5), height = readS32LE(data + 9);
        if (width <= 0 || height <= 0 || width > MAX_CELLS || height > MAX_CELLS)
        {
            return false;
        }
        // Source: OpenSimWolf LandBitmap.FromCompactBytes maxBody - a tag byte for every node of
        // the padded tree, plus a leaf's bits for every leaf holding in-bounds cells.
        S64 side = LEAF_EDGE;
        while (side < width || side < height)
        {
            side *= 2;
        }
        const S64 leaves_per_side = side / LEAF_EDGE;
        const S64 tree_nodes = (4 * leaves_per_side * leaves_per_side - 1) / 3;
        const S64 max_body = tree_nodes
            + ((width + LEAF_EDGE - 1) / LEAF_EDGE) * (S64)((height + LEAF_EDGE - 1) / LEAF_EDGE) * LEAF_BYTES;
        std::vector<U8> body;
        if (!gunzip(data + COMPACT_HEADER, (size_t)(size - COMPACT_HEADER), body, (size_t)max_body))
        {
            return false;
        }
        // Source: CellQuadTree constructor - root side is the power of two >= both, at least a
        // leaf. The tree is in the stored bitmap's own cells; anything beyond this region's grid
        // is clipped by fillRect, as LandObject.ConvertBytesToLandBitmap keeps what fits.
        CompactReader reader{ body, 0, out };
        return reader.node(0, 0, (S32)side) && reader.mPos == body.size();
    }
}

bool wolfDecodeParcelBitmap(const U8* data, S32 size, S32 cells_x, S32 cells_y, WolfParcelCells& out)
{
    out.reset(cells_x, cells_y, 0);
    if (!data || size <= 0 || cells_x <= 0 || cells_y <= 0)
    {
        return false;
    }

    // Source: OpenSimWolf LandBitmap.TryFromCompactBytes - the magic alone does not make a bitmap
    // compact: a legacy bitmap whose first 32 cells read "OSLB" starts the same way, and every
    // existing parcel is legacy. Only one that fully decodes (version, gzip CRC32 and length, a
    // tree ending exactly at the end of the stream) is taken as compact; anything else is read
    // as legacy below, as every viewer always has.
    if (size >= COMPACT_HEADER && memcmp(data, COMPACT_MAGIC, 4) == 0 && decodeCompact(data, size, out))
    {
        return true;
    }
    out.reset(cells_x, cells_y, 0);

    // Legacy: bit (x + y * cells_x), LSB first. Set bits along a row become one rectangle.
    const S64 total_bits = (S64)size * 8;
    for (S64 bit = 0; bit < total_bits;)
    {
        if ((bit & 7) == 0 && data[bit >> 3] == 0)
        {
            bit += 8;
            continue;
        }
        if (!(data[bit >> 3] & (1 << (bit & 7))))
        {
            ++bit;
            continue;
        }
        const S64 y = bit / cells_x;
        const S64 row_end = std::min((y + 1) * cells_x, total_bits);
        const S64 run_start = bit;
        while (bit < row_end && (data[bit >> 3] & (1 << (bit & 7))))
        {
            ++bit;
        }
        if (y < cells_y)
        {
            out.fillRect((S32)(run_start - y * cells_x), (S32)y, (S32)(bit - y * cells_x), (S32)y + 1, 1);
        }
    }
    return true;
}

void WolfParcelSegments::addRectangle(S32 min_x, S32 min_y, S32 max_x, S32 max_y, U8 south_mask, U8 west_mask)
{
    // Source: LLViewerParcelMgr::writeHighlightSegments (upstream). Its four loops XOR the south
    // row, west column, north row (at max_y) and east column (at max_x); with a zero width or
    // height the opposite sides land on the same cells and cancel, leaving nothing.
    if (max_x <= min_x || max_y <= min_y)
    {
        return;
    }
    mRuns.push_back({ min_x, min_y, max_x - min_x, south_mask });   // south edge
    mRuns.push_back({ min_x, min_y, max_y - min_y, west_mask });    // west edge
    mRuns.push_back({ min_x, max_y, max_x - min_x, south_mask });   // north edge
    mRuns.push_back({ max_x, min_y, max_y - min_y, west_mask });    // east edge
}

void WolfParcelSegments::addBoundary(const WolfParcelCells& cells, U8 south_mask, U8 west_mask)
{
    // Source: LLViewerParcelMgr::writeSegmentsFromBitmap (upstream): each set cell XORs the
    // south mask into its own cell and the one above, and the west mask into its own and the one
    // to the right. The result is set exactly where a set cell meets an unset or off-grid one,
    // which is what is collected here, side by side of each uniform set rectangle.
    const S32 w = cells.width(), h = cells.height();
    cells.forEachBlock([&](S32 bx, S32 by, S32 bw, S32 bh, U8 value)
    {
        if (!value)
        {
            return;
        }
        // South side: the row below, where it is unset.
        if (by == 0)
        {
            mRuns.push_back({ bx, by, bw, south_mask });
        }
        else
        {
            cells.forEachBlock(bx, by - 1, bx + bw, by, [&](S32 sx, S32, S32 sw, S32, U8 below)
            {
                if (!below) mRuns.push_back({ sx, by, sw, south_mask });
            });
        }
        // North side: the row above.
        if (by + bh >= h)
        {
            mRuns.push_back({ bx, by + bh, bw, south_mask });
        }
        else
        {
            cells.forEachBlock(bx, by + bh, bx + bw, by + bh + 1, [&](S32 sx, S32, S32 sw, S32, U8 above)
            {
                if (!above) mRuns.push_back({ sx, by + bh, sw, south_mask });
            });
        }
        // West side: the column to the left.
        if (bx == 0)
        {
            mRuns.push_back({ bx, by, bh, west_mask });
        }
        else
        {
            cells.forEachBlock(bx - 1, by, bx, by + bh, [&](S32, S32 sy, S32, S32 sh, U8 left)
            {
                if (!left) mRuns.push_back({ bx, sy, sh, west_mask });
            });
        }
        // East side: the column to the right.
        if (bx + bw >= w)
        {
            mRuns.push_back({ bx + bw, by, bh, west_mask });
        }
        else
        {
            cells.forEachBlock(bx + bw, by, bx + bw + 1, by + bh, [&](S32, S32 sy, S32, S32 sh, U8 right)
            {
                if (!right) mRuns.push_back({ bx + bw, sy, sh, west_mask });
            });
        }
    });
}
