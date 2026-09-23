/**
 * @file wolfcellquadtree.h
 * @brief WolfViewer: a width x height grid of cells held as a region quadtree.
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

#ifndef WOLF_CELLQUADTREE_H
#define WOLF_CELLQUADTREE_H

#include <algorithm>
#include <memory>

#include "stdtypes.h"

// [2026-09-23] Parcel data is one cell per 4 m (PARCEL_GRID_STEP_METERS). A byte per cell is
// (width / 4)^2 bytes - 655 MB at 102,400 m and 6.9e10 at 1,048,576 m, where the S32 products
// that sized those arrays wrapped to 0 and the writers ran off the end. Parcels are a handful of
// rectangles, so the grid is a tree of square nodes: a node whose cells all hold one value is
// one node however big (a parcel covering the whole region is ONE node), and only mixed nodes
// split, down to LEAF_EDGE-square leaves that keep their cells in an array.
//
// This is the viewer's copy of OpenSimWolf's OpenSim.Framework.CellQuadTree (the server's land
// grid, tested against dense arrays there): the same leaf size, quadrant order (SW, SE, NW, NE)
// and "cells outside width x height are don't-care" rule, so a node counts as uniform when its
// in-bounds cells are and the padding up to a power of two never forces a split.
template <typename T>
class WolfCellQuadTree
{
public:
    static constexpr S32 LEAF_EDGE = 64;

    WolfCellQuadTree(S32 width = 0, S32 height = 0, T fill = T())
    {
        reset(width, height, fill);
    }

    void reset(S32 width, S32 height, T fill)
    {
        mWidth = std::max(0, width);
        mHeight = std::max(0, height);
        mSize = LEAF_EDGE;
        while (mSize < mWidth || mSize < mHeight)
        {
            mSize *= 2;
        }
        mRoot.reset(new Node());
        mRoot->mValue = fill;
    }

    S32 width() const  { return mWidth; }
    S32 height() const { return mHeight; }

    // The value at (x, y); outside the grid, the value of the tree's root default is not
    // meaningful, so callers check bounds as they did with the arrays.
    T get(S32 x, S32 y) const
    {
        const Node* node = mRoot.get();
        S32 nx = 0, ny = 0, size = mSize;
        while (true)
        {
            if (node->mCells)
            {
                return node->mCells[(y - ny) * LEAF_EDGE + (x - nx)];
            }
            if (!node->mKids)
            {
                return node->mValue;
            }
            size >>= 1;
            S32 k = 0;
            if (x >= nx + size) { k |= 1; nx += size; }
            if (y >= ny + size) { k |= 2; ny += size; }
            node = &node->mKids[k];
        }
    }

    void set(S32 x, S32 y, T value) { fillRect(x, y, x + 1, y + 1, value); }

    // Every cell in [x0, x1) x [y0, y1), clipped to the grid.
    void fillRect(S32 x0, S32 y0, S32 x1, S32 y1, T value)
    {
        x0 = std::max(0, x0); y0 = std::max(0, y0);
        x1 = std::min(mWidth, x1); y1 = std::min(mHeight, y1);
        if (x0 >= x1 || y0 >= y1)
        {
            return;
        }
        fill(*mRoot, 0, 0, mSize, x0, y0, x1, y1, value);
    }

    // f(x, y, w, h, value) for uniform rectangles covering every cell of [x0,x1) x [y0,y1) once:
    // whole nodes where the tree is uniform, row runs inside a detailed leaf.
    template <typename F>
    void forEachBlock(S32 x0, S32 y0, S32 x1, S32 y1, F f) const
    {
        x0 = std::max(0, x0); y0 = std::max(0, y0);
        x1 = std::min(mWidth, x1); y1 = std::min(mHeight, y1);
        if (x0 >= x1 || y0 >= y1)
        {
            return;
        }
        walk(*mRoot, 0, 0, mSize, x0, y0, x1, y1, f);
    }

    template <typename F>
    void forEachBlock(F f) const { forEachBlock(0, 0, mWidth, mHeight, f); }

private:
    struct Node
    {
        T mValue = T();
        std::unique_ptr<Node[]> mKids;   // SW, SE, NW, NE
        std::unique_ptr<T[]> mCells;     // leaf only, LEAF_EDGE^2, index y * LEAF_EDGE + x
        bool uniform() const { return !mKids && !mCells; }
    };

    bool outOfBounds(S32 nx, S32 ny) const { return nx >= mWidth || ny >= mHeight; }

    static void split(Node& node, S32 size)
    {
        if (size == LEAF_EDGE)
        {
            node.mCells.reset(new T[LEAF_EDGE * LEAF_EDGE]);
            std::fill(node.mCells.get(), node.mCells.get() + LEAF_EDGE * LEAF_EDGE, node.mValue);
        }
        else
        {
            node.mKids.reset(new Node[4]);
            for (S32 i = 0; i < 4; ++i)
            {
                node.mKids[i].mValue = node.mValue;
            }
        }
    }

    // Back to uniform when every in-bounds cell under the node holds one value.
    void collapse(Node& node, S32 nx, S32 ny, S32 size)
    {
        if (node.mCells)
        {
            const S32 w = std::min(LEAF_EDGE, mWidth - nx), h = std::min(LEAF_EDGE, mHeight - ny);
            const T first = node.mCells[0];
            for (S32 y = 0; y < h; ++y)
            {
                for (S32 x = 0; x < w; ++x)
                {
                    if (!(node.mCells[y * LEAF_EDGE + x] == first))
                    {
                        return;
                    }
                }
            }
            node.mValue = first;
            node.mCells.reset();
            return;
        }
        if (!node.mKids)
        {
            return;
        }
        const S32 half = size >> 1;
        bool any = false;
        T value = T();
        for (S32 k = 0; k < 4; ++k)
        {
            const S32 kx = nx + ((k & 1) ? half : 0), ky = ny + ((k & 2) ? half : 0);
            if (outOfBounds(kx, ky))
            {
                continue;
            }
            const Node& kid = node.mKids[k];
            if (!kid.uniform())
            {
                return;
            }
            if (!any) { value = kid.mValue; any = true; }
            else if (!(kid.mValue == value)) { return; }
        }
        node.mValue = any ? value : node.mKids[0].mValue;
        node.mKids.reset();
    }

    void fill(Node& node, S32 nx, S32 ny, S32 size, S32 x0, S32 y0, S32 x1, S32 y1, T value)
    {
        if (outOfBounds(nx, ny))
        {
            return;
        }
        const S32 ix1 = std::min(nx + size, mWidth), iy1 = std::min(ny + size, mHeight);
        const S32 cx0 = std::max(nx, x0), cy0 = std::max(ny, y0);
        const S32 cx1 = std::min(ix1, x1), cy1 = std::min(iy1, y1);
        if (cx0 >= cx1 || cy0 >= cy1)
        {
            return;
        }
        if (cx0 == nx && cy0 == ny && cx1 == ix1 && cy1 == iy1)
        {
            node.mKids.reset();
            node.mCells.reset();
            node.mValue = value;
            return;
        }
        if (node.uniform())
        {
            if (node.mValue == value)
            {
                return;
            }
            split(node, size);
        }
        if (node.mCells)
        {
            for (S32 y = cy0; y < cy1; ++y)
            {
                T* row = node.mCells.get() + (y - ny) * LEAF_EDGE;
                std::fill(row + (cx0 - nx), row + (cx1 - nx), value);
            }
        }
        else
        {
            const S32 half = size >> 1;
            for (S32 k = 0; k < 4; ++k)
            {
                fill(node.mKids[k], nx + ((k & 1) ? half : 0), ny + ((k & 2) ? half : 0), half, x0, y0, x1, y1, value);
            }
        }
        collapse(node, nx, ny, size);
    }

    template <typename F>
    void walk(const Node& node, S32 nx, S32 ny, S32 size, S32 x0, S32 y0, S32 x1, S32 y1, F& f) const
    {
        if (outOfBounds(nx, ny))
        {
            return;
        }
        const S32 cx0 = std::max(nx, x0), cy0 = std::max(ny, y0);
        const S32 cx1 = std::min(std::min(nx + size, mWidth), x1), cy1 = std::min(std::min(ny + size, mHeight), y1);
        if (cx0 >= cx1 || cy0 >= cy1)
        {
            return;
        }
        if (node.uniform())
        {
            f(cx0, cy0, cx1 - cx0, cy1 - cy0, node.mValue);
            return;
        }
        if (node.mCells)
        {
            for (S32 y = cy0; y < cy1; ++y)
            {
                const T* row = node.mCells.get() + (y - ny) * LEAF_EDGE - nx;
                S32 start = cx0;
                T run = row[start];
                for (S32 x = cx0 + 1; x <= cx1; ++x)
                {
                    if (x < cx1 && row[x] == run)
                    {
                        continue;
                    }
                    f(start, y, x - start, 1, run);
                    if (x < cx1) { start = x; run = row[x]; }
                }
            }
            return;
        }
        const S32 half = size >> 1;
        for (S32 k = 0; k < 4; ++k)
        {
            walk(node.mKids[k], nx + ((k & 1) ? half : 0), ny + ((k & 2) ? half : 0), half, x0, y0, x1, y1, f);
        }
    }

    S32 mWidth = 0;
    S32 mHeight = 0;
    S32 mSize = LEAF_EDGE;
    std::unique_ptr<Node> mRoot;
};

#endif // WOLF_CELLQUADTREE_H
