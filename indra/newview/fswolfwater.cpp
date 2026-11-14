/**
 * @file fswolfwater.cpp
 * @brief Real water on any prim whose description contains "wolfwater".
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer
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
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "fswolfwater.h"

#include <algorithm>
#include <cctype>
#include <set>

#include "llagent.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvowater.h"
#include "pipeline.h"
#include "wolfobjectprops.h"

const std::string FSWolfWater::KEYWORD("wolfwater");

namespace
{
    // How often the object list is swept for wolfwater prims and for descriptions worth
    // asking about.
    // [FAR DRAW PERF 2026-11-14] Now the interval between asking for a shared object pass (wolfobjectsweep.h): a sweep is
    // spread over that pass's frames, so a surface appears, is re-fitted or goes up to one pass later than with the old
    // one-frame sweep, and the sweeps come every max(1.5 s, one pass). That is within what this feature already lives with:
    // the description it acts on arrives through WolfObjectProps, which drains every 1.5 s and re-asks a matched prim only
    // every REFRESH_SECS (120 s, wolfobjectprops.h), so a keyword change already took seconds to minutes to show.
    const F64 SWEEP_INTERVAL_SECS = 1.5;
    // Surfaces allowed at once. Past this a region is not a build, it is a griefing.
    const size_t MAX_SURFACES = 32;

    // A prim's scale has to move by more than this before its surface is re-fitted, so a
    // terse update that re-sends an unchanged transform does not rebuild geometry.
    const F32 REFIT_EPSILON_M = 0.01f;

    // How often the sweep reports what it is seeing. This exists because the first version
    // of this class logged only when it CREATED a surface, so a session that produced no
    // water produced no evidence either, and there was no way to tell "never asked" from
    // "asked and the description did not match" without guessing.
    const F64 STATS_INTERVAL_SECS = 10.0;

    // How far above the prim's top face the water sits. The prim's top face is opaque and
    // writes depth; a water plane at exactly the same depth z-fights with it and flickers.
    // Small enough that nobody can see the gap, large enough to clear depth precision at
    // the distances water is looked at.
    const F32 SURFACE_LIFT_M = 0.01f;

    // Vertical thickness given to the water object purely so it has a bounding volume.
    //
    // LLVOWater::updateSpatialExtents builds the drawable's extents from position +/-
    // scale/2, and region water carries scale.z == 0 — a box of zero height. That is
    // harmless for a plane 256m across, but LLWaterPartition leaves OCCLUSION CULLING
    // ENABLED (only LLVoidWaterPartition turns it off, llvowater.cpp:432), and a
    // zero-height box sitting one centimetre above the opaque top face of the very prim it
    // belongs to is an excellent candidate to be occluded by that face and never drawn.
    //
    // updateGeometry places vertices at position.z - scale.z/2, so the position is raised
    // by half of this to keep the surface exactly on the prim's top face. This is the same
    // trick LLWorld::updateWaterObjects uses for hole water, which carries scale.z 512 and
    // a position 256m above the water it draws.
    const F32 SURFACE_BBOX_THICKNESS_M = 0.2f;
}

namespace
{
    /**
     * Where the water plane belongs for a given prim: the centre of its top face, carried
     * up the prim's OWN up vector so a tilted prim's water sits on its tilted top rather
     * than floating above its centre, plus the depth-clearing lift.
     */
    LLVector3 surfacePositionFor(const LLViewerObject* objectp)
    {
        const LLVector3 up_local(0.f, 0.f,
                                 objectp->getScale().mV[VZ] * 0.5f + SURFACE_LIFT_M
                                 + SURFACE_BBOX_THICKNESS_M * 0.5f);
        return objectp->getPositionAgent() + (up_local * objectp->getRotation());
    }

    /** The water object's own scale: the prim's footprint, with a thin bounding volume. */
    LLVector3 surfaceScaleFor(const LLViewerObject* objectp)
    {
        return LLVector3(objectp->getScale().mV[VX],
                         objectp->getScale().mV[VY],
                         SURFACE_BBOX_THICKNESS_M);
    }
}

FSWolfWater::FSWolfWater()
{
}

FSWolfWater::~FSWolfWater()
{
    // Surfaces are LLPointer<LLVOWater> into the object list, which outlives this
    // singleton at shutdown; dropping the references is all that is wanted here.
    mSurfaces.clear();
    if (WolfObjectSweep::instanceExists())
    {
        WolfObjectSweep::instance().cancel(this);   // [FAR DRAW PERF 2026-11-14/]
    }
}

// [SWIM BUBBLES 2026-11-02]
void FSWolfWater::appendPlanes(std::vector<LLVOWater*>& out) const
{
    for (const auto& kv : mSurfaces)
    {
        LLVOWater* waterp = kv.second.mWater.get();
        if (waterp && !waterp->isDead()) out.push_back(waterp);
    }
}

// static
// Takes the store's already lower-cased description (WolfObjectProps::Props::mDescriptionLower),
// so the per-prim-per-sweep copy and transform this used to do are gone.
// static
bool FSWolfWater::matches(const std::string& description_lower)
{
    if (description_lower.empty())
    {
        return false;
    }
    return description_lower.find(KEYWORD) != std::string::npos;
}

void FSWolfWater::reset()
{
    for (auto& entry : mSurfaces)
    {
        if (entry.second.mWater.notNull() && !entry.second.mWater->isDead())
        {
            gObjectList.killObject(entry.second.mWater);
        }
    }
    mSurfaces.clear();
    mNextSweep = 0.0;
    // [FAR DRAW PERF 2026-11-14] a pass half way through belongs to what was just dropped: the next one starts afresh
    WolfObjectSweep::instance().cancel(this);
    // NOT mRegionHandle: idle() sets that immediately after calling this, and clearing it
    // here would make an explicit reset() look like a region change on the next tick.
}

void FSWolfWater::idle()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "WolfViewerWolfWaterPrims", true);
    if (!enabled)
    {
        if (!mSurfaces.empty())
        {
            reset();
        }
        WolfObjectSweep::instance().cancel(this);   // [FAR DRAW PERF 2026-11-14/] no sweep while switched off
        return;
    }

    LLViewerRegion* agent_region = gAgent.getRegion();
    if (!agent_region)
    {
        WolfObjectSweep::instance().cancel(this);   // [FAR DRAW PERF 2026-11-14/] no sweep without a region, as before
        return;
    }

    if (agent_region->getHandle() != mRegionHandle)
    {
        reset();
        mRegionHandle = agent_region->getHandle();
    }

    const F64 now = LLFrameTimer::getElapsedSeconds();
    // [FAR DRAW PERF 2026-11-14] the next sweep is asked for once the interval has run AND the last one has finished (a pass
    // takes several frames now; see SWEEP_INTERVAL_SECS)
    if (now < mNextSweep || WolfObjectSweep::instance().busy(this))
    {
        return;
    }
    mNextSweep = now + SWEEP_INTERVAL_SECS;

    WolfObjectSweep::instance().request(this);
}

// [FAR DRAW PERF 2026-11-14] The old sweep(), spread over the shared object pass: its locals are mPass*, its loop body is
// visit(), its tail is endPass(). The filters the shared pass already applies (dead, not LL_PCODE_VOLUME - which is what
// kept this class's own water planes out -, avatars, attachments, HUD attachments) are exactly the ones this loop began with.
void FSWolfWater::beginPass()
{
    mPassScanned = mPassInRange = mPassKnown = mPassMatched = 0;
    // Which prims still want water this pass. Anything holding a surface that is not in
    // here has been derezzed, has left the draw distance, or has had the keyword taken out
    // of its description.
    mPassWanted.clear();
}

void FSWolfWater::beginSlice()
{
    // The camera and the draw distance of THIS frame: object positions are read in this frame's agent space, so the two
    // always agree (a region crossing during a pass moves both).
    mSliceCamera = LLViewerCamera::getInstance()->getOrigin();
    // Only ask about, and only fit water to, what is inside the draw distance — beyond it
    // there is nothing to see and the request budget would be spent on it for nothing.
    static LLCachedControl<F32> draw_distance(gSavedSettings, "RenderFarClip", 128.f);
    mSliceDrawDistance = (F32)draw_distance;
    mSliceRangeSq = mSliceDrawDistance * mSliceDrawDistance;
    // [FAR DRAW PERF 2026-11-14/] LLSingleton::instance() takes a lock: looked up once a frame, not once an object
    mSliceProps = &WolfObjectProps::instance();
}

void FSWolfWater::visit(LLViewerObject* objectp)
{
    WolfObjectProps& props = *mSliceProps;
    ++mPassScanned;

    const LLVector3 delta = objectp->getPositionAgent() - mSliceCamera;
    const F32 dist_sq = delta.lengthSquared();
    if (dist_sq > mSliceRangeSq)
    {
        return;
    }
    // <WolfViewer 2026-10-02> ...and big enough to be seen as water from here: within
    // NEAR_M, plus PER_METRE_M for every metre of the prim's longest side (about 1.8
    // degrees of view, ~35 px of a 1080p screen at 60 degrees FOV). A 10 m pool counts to
    // 384 m, a 100 m lake to 3,264 m. Every prim in draw distance used to be asked about:
    // 26,722 at Wolf Territories Home, all furniture too small to show water from afar.
    const LLVector3 scale = objectp->getScale();
    const F32 reach = NEAR_M + PER_METRE_M * llmax(scale.mV[VX], llmax(scale.mV[VY], scale.mV[VZ]));
    if (dist_sq > reach * reach)
    {
        return;
    }
    ++mPassInRange;

    const LLUUID& id = objectp->getID();
    // Declare interest unconditionally. want() decides for itself whether this prim is
    // already answered, in flight, or given up on, and only queues the rest; a prim that
    // IS water is kept fresh, so a script that clears the keyword turns it back off.
    // Source: wolfwater.js sweep — harvester.want(obj) for every prim in range.
    const WolfObjectProps::Props* before = props.get(id);
    props.want(objectp, before && matches(before->mDescriptionLower));

    const WolfObjectProps::Props* known = props.get(id);
    if (known)
    {
        ++mPassKnown;
    }

    if (known && matches(known->mDescriptionLower))
    {
        ++mPassMatched;
        mPassWanted.insert(id);
        ensureSurface(objectp);
    }
}

void FSWolfWater::endPass()
{
    const F64 now = LLFrameTimer::getElapsedSeconds();
    for (auto it = mSurfaces.begin(); it != mSurfaces.end(); )
    {
        if (mPassWanted.find(it->first) == mPassWanted.end())
        {
            const LLUUID dying = it->first;
            ++it;
            destroySurface(dying);
        }
        else
        {
            ++it;
        }
    }

    // One line every STATS_INTERVAL_SECS saying exactly where the pipeline has got to, so
    // "no water appeared" can be told apart from "the description was never fetched" and
    // from "it was fetched and does not contain the keyword" without rebuilding anything.
    if (now >= mNextStatsLog)
    {
        mNextStatsLog = now + STATS_INTERVAL_SECS;
        LL_INFOS("WolfWater") << "sweep: " << mPassScanned << " prims, " << mPassInRange
                              << " within " << mSliceDrawDistance << "m draw distance and big enough to see, "
                              << mPassKnown << " descriptions known, "
                              << mPassMatched << " matching \"" << KEYWORD << "\", "
                              << mSurfaces.size() << " water surface(s)" << LL_ENDL;
    }
}

void FSWolfWater::ensureSurface(LLViewerObject* objectp)
{
    const LLUUID& id = objectp->getID();
    const LLVector3 prim_scale = objectp->getScale();
    const LLVector3 prim_pos   = objectp->getPositionAgent();

    auto it = mSurfaces.find(id);
    if (it != mSurfaces.end())
    {
        Surface& live = it->second;
        if (live.mWater.isNull() || live.mWater->isDead())
        {
            destroySurface(id);
        }
        else
        {
            // Re-fit only on a real move or resize. A prim that is merely re-sending its
            // transform must not throw away and rebuild a 16k-vertex lattice every sweep.
            const bool moved   = (live.mPrimPosition - prim_pos).lengthSquared()
                                 > REFIT_EPSILON_M * REFIT_EPSILON_M;
            const bool resized = (live.mPrimScale - prim_scale).lengthSquared()
                                 > REFIT_EPSILON_M * REFIT_EPSILON_M;
            if (!moved && !resized)
            {
                return;
            }

            // The surface is already there; move it rather than rebuilding the object.
            // Only a resize needs new geometry, and setScale marks the drawable rebuilt.
            LLVOWater* waterp = live.mWater;
            waterp->setRotation(objectp->getRotation());
            waterp->setPositionAgent(surfacePositionFor(objectp));
            waterp->setScale(surfaceScaleFor(objectp));
            waterp->setBoundedWaterDepth(llmax(objectp->getScale().mV[VZ], 0.05f));
            if (waterp->mDrawable.notNull())
            {
                // The lattice is built from getPositionAgent()/getScale()/getRotation() in
                // LLVOWater::updateGeometry, so a moved or resized surface needs the
                // geometry rebuilt, not just the transform poked. updateActive is what
                // LLWorld::updateWaterObjects does after it repositions a water plane —
                // LLVOWater reports isActive() false, so without it the object list is not
                // told the plane has anything to re-evaluate.
                gPipeline.markRebuild(waterp->mDrawable, LLDrawable::REBUILD_ALL);
            }
            gObjectList.updateActive(waterp);
            live.mPrimScale    = prim_scale;
            live.mPrimPosition = prim_pos;
            return;
        }
    }

    if (mSurfaces.size() >= MAX_SURFACES)
    {
        return;
    }

    LLViewerRegion* regionp = objectp->getRegion();
    if (!regionp)
    {
        return;
    }

    // A plain LLVOWater, which is what makes this feature cheap: it goes straight into
    // LLDrawPoolWater and inherits the swell, the EEP water colour and fog, the normal
    // maps, the fresnel, the reflection probes and the screen-space distortion. A pool
    // costs one more water plane, not a new render path.
    LLVOWater* waterp = (LLVOWater*)gObjectList.createObjectViewer(LLViewerObject::LL_VO_WATER, regionp);
    if (!waterp)
    {
        return;
    }

    // Zero Z scale puts the surface exactly at the object's own position, which is the
    // same convention region water uses (LLVOWater's constructor sets scale z to 0, and
    // updateGeometry offsets vertices by -scale.z/2). surfacePositionFor puts that
    // position on the prim's top face.
    // TRANSFORM FIRST, THEN createObject. This order is not incidental — it is the order
    // LLWorld::updateWaterObjects uses for its hole-water planes, and for a reason:
    // gPipeline.createObject builds the drawable and computes its spatial extents from
    // whatever scale and position the object holds at that moment. LLVOWater's constructor
    // sets the scale to the whole REGION's width, so creating the drawable before fitting
    // the object to the prim builds a region-sized plane at the region's default position
    // and only then moves the object under it.
    waterp->setRotation(objectp->getRotation());
    waterp->setPositionAgent(surfacePositionFor(objectp));
    waterp->setScale(surfaceScaleFor(objectp));
    // The prim's own thickness IS the depth of water it represents — the same rule
    // WolfStorm uses (its Water.js fixedDepth uniform). Without this the shader cannot tell
    // a pool from the region ocean and treats it as both colourless and permanently
    // shoaling. Floored so a paper-thin marker prim still reads as water.
    waterp->setBoundedWaterDepth(llmax(objectp->getScale().mV[VZ], 0.05f));
    gPipeline.createObject(waterp);
    gObjectList.updateActive(waterp);

    Surface surface;
    surface.mWater        = waterp;
    surface.mPrimScale    = prim_scale;
    surface.mPrimPosition = prim_pos;
    mSurfaces[id] = surface;

    LL_INFOS("WolfWater") << "water surface on prim " << id
                          << " (" << prim_scale.mV[VX] << "m x " << prim_scale.mV[VY] << "m)"
                          << LL_ENDL;
}

void FSWolfWater::destroySurface(const LLUUID& object_id)
{
    auto it = mSurfaces.find(object_id);
    if (it == mSurfaces.end())
    {
        return;
    }
    if (it->second.mWater.notNull() && !it->second.mWater->isDead())
    {
        gObjectList.killObject(it->second.mWater);
    }
    mSurfaces.erase(it);
    // The keyword was taken out, the prim left the draw distance or was derezzed. Logged so
    // water going away is as observable as water arriving (ensureSurface logs the create).
    LL_INFOS("WolfWater") << "water surface removed from prim " << object_id << LL_ENDL;
}
