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

#include "llviewerlayer.h"
#include "llerror.h"
#include "llmath.h"

LLViewerLayer::LLViewerLayer(const S32 width, const F32 scale)
{
    mWidth = width;
    mScale = scale;
    mScaleInv = 1.f/scale;

    // <FS:Wolf> Reserve the layer, do not COMMIT it. Same reasoning as LLSurface::create.
    //
    // This is 4 bytes per square metre of region: 256 KB for a standard region, but 2.6 GB for
    // a 25,600 m varregion — and the loop below wrote every one of those bytes at region
    // construction, before a single terrain packet had arrived. That is resident memory for
    // ground the user may never go near, and on a machine with 30 GB it was enough on its own
    // to push the viewer into swap, where it froze for seconds at a time and took minutes to
    // shut down.
    //
    // calloc returns the same already-zero mmap pages without walking them, so the cost becomes
    // proportional to the terrain actually generated. generateHeights() writes only the texels
    // of the patch it is given, so pages are faulted in as ground is composited, and the value
    // read back from an untouched page is 0.f exactly as the loop used to leave it.
    const size_t count = (size_t)width * (size_t)width;
    mDatap = (F32*)calloc(count, sizeof(F32));
    if (!mDatap)
    {
        LL_ERRS() << "Could not reserve a " << width << " x " << width
                  << " viewer layer (" << ((count * sizeof(F32)) >> 20) << " MB)" << LL_ENDL;
    }
    // </FS:Wolf>
}

LLViewerLayer::~LLViewerLayer()
{
    free(mDatap);   // <FS:Wolf/> calloc'd above, so free() — not delete[].
    mDatap = NULL;
}

F32 LLViewerLayer::getValue(const S32 x, const S32 y) const
{
//  llassert(x >= 0);
//  llassert(x < mWidth);
//  llassert(y >= 0);
//  llassert(y < mWidth);

    return *(mDatap + x + y*mWidth);
}

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
    S32 row1 = y1 * mWidth;
    S32 row2 = y2 * mWidth;

    // Access in squential order in memory, and don't use immediately.
    F32 row1_left  = mDatap[ row1 + x1 ];
    F32 row1_right = mDatap[ row1 + x2 ];
    F32 row2_left  = mDatap[ row2 + x1 ];
    F32 row2_right = mDatap[ row2 + x2 ];

    F32 row1_interp = row1_left - x_frac * (row1_left - row1_right);
    F32 row2_interp = row2_left - x_frac * (row2_left - row2_right);

    return row1_interp - y_frac * (row1_interp - row2_interp);
}
