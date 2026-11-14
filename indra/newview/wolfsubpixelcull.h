/**
 * @file wolfsubpixelcull.h
 * @brief WolfViewer: static prims and mesh whose whole octree node is smaller than one pixel on screen are not drawn.
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

#ifndef WOLF_SUBPIXEL_CULL_H
#define WOLF_SUBPIXEL_CULL_H

#include "stdtypes.h"

class LLCamera;
class LLSpatialGroup;

/**
 * [W107 FARPERF 2026-11-14] Paul: "skip drawing objects whose projected size is below one pixel ... a fixed physical threshold,
 * NEVER adaptive to frame rate". At Paddington Station with a 4096 m draw distance about 98,000 prims are loaded; the far ones
 * are each a few cm to a few m across, far below a pixel at 2-4 km, but every one is still culled, sorted and drawn.
 *
 * The test is on whole octree nodes of the static volume partition (LLViewerRegion::PARTITION_VOLUME: prims and mesh that are
 * not linked to anything moving), from LLOctreeCull::processGroup (llspatialpartition.cpp): a node whose own objects
 * (mObjectBounds) are wholly smaller than MAX_PIXELS across is not drawn, unless any face of them shines on its own
 * (fullbright, glow, PBR emissive, legacy emissive mask): a lamp or a lit window far away is a bright point that still lights
 * its pixel, so it is always drawn as stock. Subtrees are still walked (a child might hold such a face).
 * The size is an UPPER bound of the projected diameter, so nothing larger than MAX_PIXELS anywhere on the screen is dropped:
 *   diameter_px <= k x r / (d - r) x (1 + tan(half diagonal fov)^2)
 * with r the bounding box's half-diagonal, d - r the distance to the sphere's nearest point, k = view height px / tan(half
 * vertical fov) (the centre's pixels per radian, x2 for diameter / 2 for the half-height: llviewercamera.cpp setPerspective
 * mPixelMeterRatio), and 1 + tan^2 = 1 / cos^2 of the screen corner's angle off the view axis, the most a small object's
 * projection is stretched off-centre. The camera's CURRENT fov is used, so zooming in brings back what it magnifies.
 *
 * Not applied (stock behaviour): avatars, attachments, anything moving (bridge partitions), terrain, water, trees, grass,
 * particles, HUDs; shadow maps, reflection probes, mirrors, water reflections, snapshots, VR eyes, and while the Build
 * floater is open; and whenever WolfViewerSubPixelCull is off. A prim that is a light still lights its surroundings: lights are
 * found from LLPipeline::mLights (calcNearbyLights), not from this cull. A node is never culled while the camera is inside or near it (d <= r).
 *
 * What it can change: an unlit prim under one pixel across can still happen to cover one pixel's sample point, so a single
 * dark pixel here and there in the far distance can differ (it shimmers on and off as the camera moves anyway).
 */
class WolfSubPixelCull
{
public:
    /** The fixed threshold: a node is skipped only when its projected diameter is below this many pixels. */
    static constexpr F32 MAX_PIXELS = 1.f;

    /** From LLOctreeCull::processGroup, with the camera the cull was made for: true = do not draw this node's objects. */
    static bool skipObjects(LLSpatialGroup* group, const LLCamera& camera);
};

#endif // WOLF_SUBPIXEL_CULL_H
