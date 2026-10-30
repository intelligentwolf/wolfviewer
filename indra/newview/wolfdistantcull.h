/**
 * @file wolfdistantcull.h
 * @brief WolfViewer: far-away prims and mesh too small to see are not drawn, more so while the frame is slow.
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

#ifndef WOLF_DISTANT_CULL_H
#define WOLF_DISTANT_CULL_H

#include "stdtypes.h"

class LLSpatialGroup;

/**
 * [bigregion-perf 2026-10-30] Paul at The Caribbean (one 10,240 m region, ~49,000 prims, port towns): 12-16 fps at
 * a 4,096 m draw distance on a 5090, better only when the draw distance is lowered, "but normal users won't do that".
 *
 * The stock octree cull (llspatialpartition.cpp LLOctreeCull) draws every octree group in the frustum and inside the
 * draw distance, whatever its size on screen: a 1 m crate 3 km away costs the same cull, sort, occlusion query and
 * draw calls (main pass, four sun-shadow cascades) as one at your feet. On a standard 256 m region that set is
 * small; on a 10 km region seen 4 km deep it is most of the region.
 *
 * Rule (the viewer side of WolfSim's far-object rule, LLClientView.WolfObjectInView, live since 2026-10-08 with
 * FullDetailDistance 1024 and FarObjectMinRatio 0.006 - grid.opensimini 1129/1132):
 *   - only the region's static prims and mesh (LLViewerRegion::PARTITION_VOLUME). Never avatars, animesh, attachments,
 *     moving linksets (bridges), terrain, water, trees, grass, particles, clouds or HUDs;
 *   - nothing nearer than FULL_DETAIL_M (1,024 m, the server's FullDetailDistance) is ever touched, so the near scene
 *     is drawn exactly as before;
 *   - beyond it an octree group is skipped when its bounds would cover fewer than threshold() pixels across on screen
 *     (2 r / d, scaled by the camera's own vertical field of view and the window height, so zooming in brings far
 *     things back exactly as it makes them bigger);
 *   - threshold() is MIN_PIXELS (1 px: smaller than a pixel) while frames are fast and rises towards MAX_SCREEN_FRACTION
 *     of the view height (the server's 0.006 radius per metre at the default 60 degree view: 2 x 0.006 / (2 tan 30)
 *     = 1.04 % of the height, 11 px of 1080) as WolfFrameGovernor's budget falls - the measured frame time, never a
 *     guess at what the GPU can do. It falls from 1 to 0.25 in about 4 s of frames slower than 25 fps and climbs
 *     back over about 17 s of frames faster than 30 fps, holding in between (wolfframegovernor.h);
 *   - a group drawn last frame stays until it is below 3/4 of the threshold (the server's own x0.75 for an object
 *     already shown), so a group on the edge does not flicker as the camera moves.
 * Lights are not affected: deferred lights come from LLPipeline::mNearbyLights (all light prims, calcNearbyLights),
 * not from the visible groups. Reflection-probe and mirror renders (gCubeSnapshot), the reflection pass and object
 * selection are left exactly as stock, and while the Build floater is open nothing is culled.
 *
 * WolfViewerDistantCull (debug setting, default on) turns it off for comparison; resident settings are not touched.
 */
class WolfDistantCull
{
public:
    /** LLOctreeCull::earlyFail: the whole subtree under this group is too small to draw (it is not traversed). */
    static bool skipSubtree(const LLSpatialGroup* group);
    /** LLOctreeCull::processGroup: the objects held in this group are too small to draw (not marked visible). */
    static bool skipObjects(const LLSpatialGroup* group);

    /** The threshold in force this frame, in pixels across, and the governor's load behind it (0 = none, 1 = full). */
    static F32 thresholdPixels();
    static F32 load();
    /** Groups / subtrees skipped for the world camera since the last call (the frame profile's log line). */
    static void takeCounts(U32& groups, U32& subtrees);

    static constexpr F32 FULL_DETAIL_M = 1024.f;
    static constexpr F32 MIN_PIXELS = 1.f;
    static constexpr F32 MAX_SCREEN_FRACTION = 0.0104f;
    static constexpr F32 SHOWN_KEEP = 0.75f;
};

#endif // WOLF_DISTANT_CULL_H
