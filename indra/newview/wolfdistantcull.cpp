/**
 * @file wolfdistantcull.cpp
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

#include "llviewerprecompiledheaders.h"

#include "wolfdistantcull.h"

#include "llfloatertools.h"
#include "llframetimer.h"
#include "llspatialpartition.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "pipeline.h"
#include "wolfframegovernor.h"

#include <cmath>

extern bool gCubeSnapshot;   // pipeline.cpp: a reflection probe or mirror is being rendered

namespace
{
    // Worked out once a frame, on the first group asked about (see frame()).
    struct Frame
    {
        U32        frame = U32_MAX;
        bool       on = false;
        LLVector4a eye;
        F32        k = 0.f;        // pixels across = k x radius / distance  (k = view height px / tan(vertical fov / 2))
        F32        hidePx = 0.f;   // a group not drawn last frame is skipped below this many pixels across
        F32        keepPx = 0.f;   // one drawn last frame only below this (SHOWN_KEEP x hidePx)
        F32        load = 0.f;
    };
    Frame sFrame;
    U32 sSkippedGroups = 0, sSkippedSubtrees = 0;

    const Frame& frame()
    {
        // Source: llframetimer.h:71 getFrameCount - goes up once per frame (LLFrameTimer::updateFrameTime).
        const U32 now = LLFrameTimer::getFrameCount();
        if (sFrame.frame == now)
        {
            return sFrame;
        }
        sFrame.frame = now;

        static LLCachedControl<bool> enabled(gSavedSettings, "WolfViewerDistantCull", true);
        LLViewerCamera* camera = LLViewerCamera::instanceExists() ? LLViewerCamera::getInstance() : nullptr;
        sFrame.on = enabled && camera && !(gFloaterTools && gFloaterTools->getVisible());
        if (!sFrame.on)
        {
            return sFrame;
        }

        // Source: llcamera.h:161 getView - vertical field of view, radians; :162 getViewHeightInPixels. The same
        // projection as LLViewerCamera::setPerspective's mPixelMeterRatio (llviewercamera.cpp:161), but with the
        // camera's CURRENT field of view rather than the default one, so a zoomed-in view keeps what it magnifies.
        const F32 half_tan = tanf(0.5f * llclamp(camera->getView(), 0.01f, 3.0f));
        const F32 height = (F32)llmax(camera->getViewHeightInPixels(), 1);
        sFrame.k = height / half_tan;
        sFrame.eye.load3(camera->getOrigin().mV);

        // Source: wolfframegovernor.h - budget 1 (frames fast) .. MIN_BUDGET (slow); load 0 .. 1 between them.
        const F32 g = WolfFrameGovernor::budget();
        sFrame.load = llclamp((1.f - g) / (1.f - WolfFrameGovernor::MIN_BUDGET), 0.f, 1.f);
        const F32 max_px = llmax(WolfDistantCull::MIN_PIXELS, WolfDistantCull::MAX_SCREEN_FRACTION * height);
        sFrame.hidePx = WolfDistantCull::MIN_PIXELS + (max_px - WolfDistantCull::MIN_PIXELS) * sFrame.load;
        sFrame.keepPx = WolfDistantCull::SHOWN_KEEP * sFrame.hidePx;
        return sFrame;
    }

    /** Is a box (centre, half size) smaller than `px` pixels across, and wholly beyond FULL_DETAIL_M? */
    bool tooSmall(const Frame& f, const LLVector4a* bounds, F32 px)
    {
        const F32 radius = bounds[1].getLength3().getF32();
        LLVector4a to;
        to.setSub(bounds[0], f.eye);
        const F32 nearest = to.getLength3().getF32() - radius;   // nearest point of the bounding sphere
        if (!(nearest > WolfDistantCull::FULL_DETAIL_M))
        {
            return false;   // anything reaching inside FULL_DETAIL_M (or NaN bounds) is drawn exactly as stock
        }
        return f.k * radius < px * nearest;   // k r / d < px, without the division
    }

    /** The groups this applies to at all (see wolfdistantcull.h), and only while nothing special is being rendered. */
    const Frame* applies(LLSpatialGroup* group)
    {
        if (!group || gCubeSnapshot || LLPipeline::sReflectionRender)
        {
            return nullptr;
        }
        LLSpatialPartition* part = group->getSpatialPartition();
        if (!part || part->mPartitionType != LLViewerRegion::PARTITION_VOLUME)
        {
            return nullptr;
        }
        const Frame& f = frame();
        return f.on ? &f : nullptr;
    }

    bool worldCamera()
    {
        return LLViewerCamera::sCurCameraID == LLViewerCamera::CAMERA_WORLD && !LLPipeline::sShadowRender;
    }
}

// static
bool WolfDistantCull::skipSubtree(const LLSpatialGroup* cgroup)
{
    LLSpatialGroup* group = const_cast<LLSpatialGroup*>(cgroup);
    const Frame* f = applies(group);
    if (!f || !group->getOctreeNode() || !group->getOctreeNode()->getParent())
    {
        return false;   // never the root: it holds the whole region
    }
    // The whole subtree (mBounds: this node and all its children). Always the lenient keep threshold, so a subtree is
    // never dropped while something in it could still be over the line for a group drawn last frame (a child's box
    // lies inside its parent's, so it is no bigger and no nearer).
    if (!tooSmall(*f, group->getBounds(), f->keepPx))
    {
        return false;
    }
    if (worldCamera())
    {
        ++sSkippedSubtrees;
    }
    return true;
}

// static
bool WolfDistantCull::skipObjects(const LLSpatialGroup* cgroup)
{
    LLSpatialGroup* group = const_cast<LLSpatialGroup*>(cgroup);
    const Frame* f = applies(group);
    if (!f)
    {
        return false;
    }
    // Source: llvieweroctree.h:221 getVisible(camera) - the frame this group was last marked visible for this camera
    // (LLViewerOctreeGroup::setVisible, from LLPipeline::markNotCulled); drawn last frame -> the keep threshold.
    const bool shown = group->getVisible(LLViewerCamera::sCurCameraID) >= LLViewerOctreeEntryData::getCurrentFrame() - 1;
    if (!tooSmall(*f, group->getObjectBounds(), shown ? f->keepPx : f->hidePx))
    {
        return false;
    }
    if (worldCamera())
    {
        ++sSkippedGroups;
    }
    return true;
}

// static
F32 WolfDistantCull::thresholdPixels()
{
    return frame().on ? sFrame.hidePx : 0.f;
}

// static
F32 WolfDistantCull::load()
{
    return frame().on ? sFrame.load : 0.f;
}

// static
void WolfDistantCull::takeCounts(U32& groups, U32& subtrees)
{
    groups = sSkippedGroups;
    subtrees = sSkippedSubtrees;
    sSkippedGroups = sSkippedSubtrees = 0;
}
