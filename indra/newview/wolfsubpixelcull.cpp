/**
 * @file wolfsubpixelcull.cpp
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

#include "llviewerprecompiledheaders.h"

#include "wolfsubpixelcull.h"

#include "llfloatertools.h"
#include "llframetimer.h"
#include "llspatialpartition.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "pipeline.h"
#include "lltextureentry.h"
#include "llvovolume.h"
#include "wolfvrscreen.h"
#include "wolfvrstream.h"

#include <cmath>

extern bool gCubeSnapshot;   // pipeline.cpp: a reflection probe or mirror is being rendered
extern bool gSnapshot;       // llviewerdisplay.cpp:206: a snapshot (its own size and zoom) is being rendered

namespace
{
    // Worked out once a frame, on the first node asked about (frame(camera)): the camera the cull was given.
    struct Frame
    {
        U32        frame = U32_MAX;
        bool       on = false;
        LLVector4a eye;
        F32        k = 0.f;   // upper bound of pixels across = k x radius / nearest distance (see wolfsubpixelcull.h)
    };
    Frame sFrame;
    U32 sSkippedGroups = 0;

    const Frame& frame(const LLCamera& camera)
    {
        // Source: llframetimer.h getFrameCount - goes up once a frame (LLFrameTimer::updateFrameTime).
        const U32 now = LLFrameTimer::getFrameCount();
        if (sFrame.frame == now)
        {
            return sFrame;
        }
        sFrame.frame = now;

        // the counts, in the log every 10 s while WolfLogFrameRate (the frame-rate test switch) is on
        static LLCachedControl<bool> log_rate(gSavedSettings, "WolfLogFrameRate", false);
        static F64 next_log = 0.0;
        if (log_rate && LLTimer::getTotalSeconds() >= next_log)
        {
            next_log = LLTimer::getTotalSeconds() + 10.0;
            LL_INFOS("WolfSubPixelCull") << "sub-pixel cull, last 10 s: " << sSkippedGroups << " octree nodes' objects not drawn (under " << WolfSubPixelCull::MAX_PIXELS
                                         << " px across), " << (sFrame.on ? "on" : "off") << LL_ENDL;
            sSkippedGroups = 0;
        }

        static LLCachedControl<bool> enabled(gSavedSettings, "WolfViewerSubPixelCull", true);
        // [VR] the eyes are drawn at the headset's own size and field of view, and an eye's crop (WolfVRScreen::eyeCrop,
        // applied by LLViewerCamera::setPerspective) magnifies beyond getView(): not covered, stock there
        float ax = 1.f, bx = 0.f, ay = 1.f, by = 0.f;
        sFrame.on = enabled && !(gFloaterTools && gFloaterTools->getVisible()) && !WolfVRStream::headsetInVR()
                    && !WolfVRScreen::eyeCrop(ax, bx, ay, by);
        if (!sFrame.on)
        {
            return sFrame;
        }
        // Source: llcamera.h:161 getView (vertical field of view, radians), :162 getViewHeightInPixels, :163 getAspect
        // (width / height). The centre's pixels across for a sphere of radius r at distance d are r x height / (d x
        // tan(view / 2)) - the projection of llviewercamera.cpp:164 mPixelMeterRatio, with the CURRENT view rather than the
        // default one. Off-centre a small object is stretched by up to 1 / cos^2 of its angle off the axis, the most at the
        // screen corner: 1 + tan^2(half diagonal) = 1 + tan^2(half vertical) x (1 + aspect^2).
        const F32 half_tan = tanf(0.5f * llclamp(camera.getView(), 0.01f, 3.0f));
        const F32 height = (F32)llmax(camera.getViewHeightInPixels(), 1);
        const F32 aspect = llmax(camera.getAspect(), 0.01f);
        const F32 corner = 1.f + half_tan * half_tan * (1.f + aspect * aspect);
        sFrame.k = height / half_tan * corner;
        sFrame.eye.load3(camera.getOrigin().mV);
        return sFrame;
    }

    /** Is a box (centre, half size) wholly below MAX_PIXELS across wherever it is on the screen? */
    bool tooSmall(const Frame& f, const LLVector4a* bounds)
    {
        const F32 radius = bounds[1].getLength3().getF32();   // the box's half-diagonal: its bounding sphere
        LLVector4a to;
        to.setSub(bounds[0], f.eye);
        const F32 nearest = to.getLength3().getF32() - radius;
        if (!(nearest > 0.f) || !(radius >= 0.f))
        {
            return false;   // camera inside or touching the sphere, or NaN bounds: stock
        }
        return f.k * radius < WolfSubPixelCull::MAX_PIXELS * nearest;   // k r / d < MAX_PIXELS, without the division
    }

    /** The main view's cull of the static volume partition only, and only while nothing special is being rendered. */
    const Frame* applies(LLSpatialGroup* group, const LLCamera& camera)
    {
        if (!group || gCubeSnapshot || gSnapshot || LLPipeline::sReflectionRender || LLPipeline::sShadowRender
            || LLViewerCamera::sCurCameraID != LLViewerCamera::CAMERA_WORLD)
        {
            return nullptr;
        }
        LLSpatialPartition* part = group->getSpatialPartition();
        if (!part || part->mPartitionType != LLViewerRegion::PARTITION_VOLUME)
        {
            return nullptr;
        }
        const Frame& f = frame(camera);
        return f.on ? &f : nullptr;
    }

    /** Can any face of this node's objects shine on its own: fullbright, glow, a PBR emissive colour or texture, or a
     *  legacy material whose alpha is an emissive mask? Such a prim is a bright point at night (a lamp, a lit window)
     *  that lights its pixel even when far smaller than it, so it is always drawn as stock. */
    bool selfLit(LLSpatialGroup* group)
    {
        for (LLSpatialGroup::element_iter i = group->getDataBegin(); i != group->getDataEnd(); ++i)
        {
            LLDrawable* drawablep = (LLDrawable*)(*i)->getDrawable();
            LLViewerObject* objectp = drawablep ? drawablep->getVObj().get() : nullptr;
            if (!objectp)
            {
                continue;
            }
            const U8 count = objectp->getNumTEs();
            for (U8 te = 0; te < count; ++te)
            {
                const LLTextureEntry* tep = objectp->getTE(te);
                if (!tep)
                {
                    continue;
                }
                // Source: lltextureentry.h:154 getFullbright, :162 getGlow
                if (tep->getFullbright() || tep->getGlow() > 0.f)
                {
                    return true;
                }
                // Source: llgltfmaterial.h:278 mEmissiveColor, :272 mTextureId[GLTF_TEXTURE_INFO_EMISSIVE]
                const LLGLTFMaterial* gltf = tep->getGLTFRenderMaterial();
                if (gltf && (gltf->mEmissiveColor != LLColor3::black
                             || gltf->mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_EMISSIVE].notNull()))
                {
                    return true;
                }
                // Source: llmaterial.h:45 DIFFUSE_ALPHA_MODE_EMISSIVE - the diffuse alpha is an emissive mask
                const LLMaterialPtr mat = tep->getMaterialParams();
                if (mat.notNull() && mat->getDiffuseAlphaMode() == LLMaterial::DIFFUSE_ALPHA_MODE_EMISSIVE)
                {
                    return true;
                }
            }
        }
        return false;
    }
}

// static
bool WolfSubPixelCull::skipObjects(LLSpatialGroup* group, const LLCamera& camera)
{
    const Frame* f = applies(group, camera);
    if (!f || !tooSmall(*f, group->getObjectBounds()) || selfLit(group))
    {
        return false;
    }
    ++sSkippedGroups;
    return true;
}
