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
#include "llselectmgr.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvowater.h"
#include "pipeline.h"

const std::string FSWolfWater::KEYWORD("wolfwater");

namespace
{
    // How often the object list is swept for wolfwater prims and for descriptions worth
    // asking about.
    const F64 SWEEP_INTERVAL_SECS = 1.5;
    // Requests per sweep. 48 per 1.5s is about 32/s of a Medium-frequency packet — the same
    // order the hover tooltip already produces when a user sweeps the mouse across a
    // crowded scene, and it is bounded: an object is asked once and then not again for two
    // minutes, so this is a burst on arrival, not a sustained rate.
    //
    // 24 was too slow to be usable. Roots are drained before linkset children, but a busy
    // region still holds several hundred root prims, and at 16/s a wolfwater prim sitting
    // behind them was not asked about for the better part of a minute — long enough to look
    // like the feature simply did not work.
    const S32 REQUEST_BUDGET = 48;
    // How long to wait for a reply before the single retry.
    const F64 RETRY_SECS = 30.0;
    // Attempts before giving up on an object entirely.
    const S32 MAX_TRIES = 2;
    // How stale a known description must be before spare budget may re-ask.
    const F64 REFRESH_SECS = 120.0;
    // Surfaces allowed at once. Past this a region is not a build, it is a griefing.
    const size_t MAX_SURFACES = 32;

    // Priority classes for the request queue, best first.
    const S32 PRIORITY_ROOT    = 0;
    const S32 PRIORITY_CHILD   = 1;
    const S32 PRIORITY_REFRESH = 2;

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
}

// static
bool FSWolfWater::matches(const std::string& description)
{
    if (description.empty())
    {
        return false;
    }
    std::string lower(description);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return lower.find(KEYWORD) != std::string::npos;
}

void FSWolfWater::noteDescription(const LLUUID& object_id, const std::string& description)
{
    if (object_id.isNull())
    {
        return;
    }
    // Stored even when empty. An empty description is the answer to "does this prim still
    // say wolfwater", and storing it is what lets the next sweep take the water away.
    const bool was_match = matches(mDescriptions.count(object_id) ? mDescriptions[object_id]
                                                                 : std::string());
    mDescriptions[object_id] = description;
    const bool is_match = matches(description);
    if (is_match != was_match)
    {
        LL_INFOS("WolfWater") << "prim " << object_id << " description "
                              << (is_match ? "now matches" : "no longer matches")
                              << " \"" << KEYWORD << "\": \"" << description << "\""
                              << LL_ENDL;
    }
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
    // Descriptions and the ask log go too: the object list is rebuilt from scratch across a
    // teleport, so every pending request refers to something that no longer exists, and a
    // description held from the old region is a claim about a prim we can no longer see.
    mDescriptions.clear();
    mAsked.clear();
    mCandidates.clear();
    mNextSweep = 0.0;
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
        return;
    }

    LLViewerRegion* agent_region = gAgent.getRegion();
    if (!agent_region)
    {
        return;
    }

    if (agent_region->getHandle() != mRegionHandle)
    {
        reset();
        mRegionHandle = agent_region->getHandle();
    }

    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (now < mNextSweep)
    {
        return;
    }
    mNextSweep = now + SWEEP_INTERVAL_SECS;

    sweep();
    requestPending();
}

void FSWolfWater::sweep()
{
    const F64 now = LLFrameTimer::getElapsedSeconds();
    const LLVector3 camera_pos = LLViewerCamera::getInstance()->getOrigin();

    // Only ask about, and only fit water to, what is inside the draw distance — beyond it
    // there is nothing to see and the request budget would be spent on it for nothing.
    static LLCachedControl<F32> draw_distance(gSavedSettings, "RenderFarClip", 128.f);
    const F32 range_sq = (F32)draw_distance * (F32)draw_distance;

    mCandidates.clear();

    S32 n_scanned = 0, n_in_range = 0, n_known = 0, n_matched = 0;

    // Which prims still want water this tick. Anything holding a surface that is not in
    // here has been derezzed, has left the draw distance, or has had the keyword taken out
    // of its description.
    std::set<LLUUID> still_wanted;

    const S32 count = gObjectList.getNumObjects();
    for (S32 i = 0; i < count; ++i)
    {
        LLViewerObject* objectp = gObjectList.getObject(i);
        if (!objectp || objectp->isDead() || objectp->isAvatar())
        {
            continue;
        }
        if (objectp->isHUDAttachment() || objectp->isAttachment())
        {
            continue;
        }
        // A water plane this class made is itself an object in the list. Fitting water to
        // water would be an unbounded loop of surfaces; skip anything in the water pcode.
        if (objectp->getPCode() != LL_PCODE_VOLUME)
        {
            continue;
        }
        ++n_scanned;

        const LLVector3 delta = objectp->getPositionAgent() - camera_pos;
        const F32 dist_sq = delta.lengthSquared();
        if (dist_sq > range_sq)
        {
            continue;
        }
        ++n_in_range;

        const LLUUID& id = objectp->getID();
        auto desc_it = mDescriptions.find(id);
        const bool known = (desc_it != mDescriptions.end());
        if (known)
        {
            ++n_known;
        }

        // Queue a request. A known description is only re-asked once it is stale, and even
        // then it sorts behind everything that has never been asked at all, so a refresh
        // can only ever consume budget that nothing new wanted.
        auto ask_it = mAsked.find(id);
        if (known)
        {
            if (ask_it == mAsked.end() || (now - ask_it->second.mSentAt) >= REFRESH_SECS)
            {
                mCandidates.push_back({ id, dist_sq, PRIORITY_REFRESH });
            }
        }
        else if (ask_it == mAsked.end() || ask_it->second.mTries < MAX_TRIES)
        {
            if (ask_it == mAsked.end() || (now - ask_it->second.mSentAt) >= RETRY_SECS)
            {
                mCandidates.push_back({ id, dist_sq,
                                        objectp->isRoot() ? PRIORITY_ROOT : PRIORITY_CHILD });
            }
        }

        if (known && matches(desc_it->second))
        {
            ++n_matched;
            still_wanted.insert(id);
            ensureSurface(objectp);
        }
    }

    for (auto it = mSurfaces.begin(); it != mSurfaces.end(); )
    {
        if (still_wanted.find(it->first) == still_wanted.end())
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
        LL_INFOS("WolfWater") << "sweep: " << n_scanned << " prims, " << n_in_range
                              << " within " << (F32)draw_distance << "m draw distance, "
                              << n_known << " descriptions known ("
                              << mDescriptions.size() << " total), "
                              << n_matched << " matching \"" << KEYWORD << "\", "
                              << mSurfaces.size() << " water surface(s); "
                              << mLastRequestCount << " request(s) sent last tick, "
                              << mCandidates.size() << " queued" << LL_ENDL;
    }
}

void FSWolfWater::requestPending()
{
    mLastRequestCount = 0;
    if (mCandidates.empty())
    {
        return;
    }

    // Roots before children before refreshes, and nearest first inside each class, so the
    // budget goes to what the user is looking at.
    std::sort(mCandidates.begin(), mCandidates.end(),
              [](const Candidate& a, const Candidate& b)
              {
                  if (a.mPriority != b.mPriority)
                  {
                      return a.mPriority < b.mPriority;
                  }
                  return a.mDistSq < b.mDistSq;
              });

    const F64 now = LLFrameTimer::getElapsedSeconds();
    const S32 budget = llmin((S32)mCandidates.size(), REQUEST_BUDGET);
    for (S32 i = 0; i < budget; ++i)
    {
        const Candidate& c = mCandidates[i];
        LLViewerObject* objectp = gObjectList.findObject(c.mId);
        if (!objectp || objectp->isDead())
        {
            continue;
        }

        Ask& ask = mAsked[c.mId];
        ask.mSentAt = now;
        // A refresh restarts the attempt count: the object demonstrably exists and
        // demonstrably answers, so the give-up counter from its first fetch is spent
        // history rather than evidence.
        ask.mTries = (c.mPriority == PRIORITY_REFRESH) ? 1 : (ask.mTries + 1);

        LLSelectMgr::getInstance()->requestObjectPropertiesFamily(objectp);
        ++mLastRequestCount;
    }

    mCandidates.clear();
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
}
