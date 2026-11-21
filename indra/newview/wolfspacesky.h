/**
 * @file wolfspacesky.h
 * @brief WolfViewer: the Wolf sky - the world map's space, shared (stars, Milky Way, nebulae).
 *        spin and zoom into that flattens into the ordinary map (WolfViewer 2026-10-02).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Copyright (C) 2026, IntelligentWolf Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef WOLF_SPACESKY_H
#define WOLF_SPACESKY_H

#include <vector>
#include "llpointer.h"

class LLImageRaw;

// [2026-11-21] The world map's sky, shared so other windows wear the SAME sky. Paul 2026-11-21 on Wolf
// Experiences: "a 3D scrolling carousel ... with a space background like the map".
//
// Source: wolfmapglobe.cpp WolfMapGlobe::makeSky (2026-10-03), moved here unchanged (a port, call for call, of
// WolfStorm's map_globe.js MapGlobe._makeSky, now js/world/wolf_space_sky.js): the same seed, the random numbers
// drawn in the same order, so the stars land in the same places on both viewers' maps and here.
namespace WolfSpaceSky
{
    // A direction on the unit sphere, a size in pixels, a colour and an alpha.
    struct Point
    {
        F32 d[3];
        F32 size;
        F32 c[3];
        F32 a;
    };

    // Made once. starlight() is added (the stars, the Milky Way and its glow, the nebulae); dust() is laid over
    // (the Milky Way's dark dust lane, in the space colour).
    const std::vector<Point>& starlight();
    const std::vector<Point>& dust();

    // A camera on the sky. The map's: yaw / pitch from the spin, lens 0.8 (f = lens * max(W, H)), no gain.
    // starGain multiplies the stars' alpha (points under CLOUD_PX), cloudGain the clouds' (the glow and the nebulae);
    // exposure 0 = the map's plain clamp, above 0 a soft roll-off 1 - exp(-exposure * c).
    struct View
    {
        F64 yaw = 0.0, pitch = 0.0, lens = 0.8;
        F32 starGain = 1.f, cloudGain = 1.f, exposure = 0.f;
    };
    // Wolf Experiences' camera (the same numbers as wolf_space_sky.js EXPERIENCES_VIEW).
    View experiencesView();

    // The sky painted once into an RGB image width x height (rows bottom-up, as GL draws them), a point of `size`
    // px drawn `scale` times bigger. Projected as WolfMapGlobe::drawSpace: z2 < 0.05 behind the eye,
    // x = W/2 + f x / z, y = H/2 + f y / z; each point the map's soft dot (alpha exp(-4 r^2) inside its circle),
    // starlight added and the dust lane laid over as the map blends them. A few tens of milliseconds: callers keep
    // the image (a window that shows it draws one textured quad a frame, nothing more).
    LLPointer<LLImageRaw> paint(S32 width, S32 height, F32 scale, const View& view);
}

#endif // WOLF_SPACESKY_H
