/**
 * @file fswolfwater.h
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

#ifndef FS_WOLFWATER_H
#define FS_WOLFWATER_H

#include <map>
#include <string>

#include "llsingleton.h"
#include "lluuid.h"
#include "llpointer.h"
#include "llframetimer.h"
#include "v3math.h"

class LLViewerObject;
class LLVOWater;

/**
 * A builder types "wolfwater" into a prim's description and the viewer draws a real water
 * surface across the top of it — the same Gerstner swell, EEP colour, fog and reflection
 * the region ocean uses, bounded to the prim. Ponds, pools, fountains, canal locks.
 *
 * This is the WolfViewer counterpart of WolfStorm's js/world/wolfwater.js, and the two
 * agree on what the keyword means and on what gets drawn, so a build looks the same in
 * both viewers.
 *
 * ── FINDING THE PRIMS ────────────────────────────────────────────────────────────────
 * A prim's description is NOT in ObjectUpdate. The message template is explicit
 * (message_template.msg ObjectUpdate): the ObjectData block carries Text, NameValue,
 * MediaURL, ExtraParams and TextureEntry, and neither Name nor Description. The only two
 * messages that carry it are ObjectProperties (Medium 9, sent on select — and selecting
 * freezes the object's physics sim-side, so it cannot be used as a bulk query) and
 * ObjectPropertiesFamily (Medium 10), whose request takes ONE ObjectID per packet.
 *
 * So descriptions have to be asked for, one prim at a time. Stock Firestorm only ever
 * asks about the object under the cursor or in a selection (LLSelectMgr::
 * requestObjectPropertiesFamily), and keeps the answer in a select node rather than on the
 * object — which is why this class carries its own throttled sweep and its own description
 * store rather than reading one off LLViewerObject.
 *
 * The sweep is nearest-first and capped per tick, so the budget goes to what the user is
 * actually looking at, and root prims are drained before linkset children (most prims in a
 * region are children; a water surface almost never is). Once a description is known it is
 * only re-asked after REFRESH_SECS, and only with budget that no first-time request wanted
 * — so a description edited by someone else still propagates, at no extra packet cost.
 *
 * ── WHAT GETS DRAWN ──────────────────────────────────────────────────────────────────
 * An LLVOWater, sized to the prim's X/Y footprint and placed at the top of its bounding
 * box. Reusing the water object rather than inventing a surface means the pool goes
 * through LLDrawPoolWater with everything already attached to it: the swell, the EEP water
 * colour, the normal maps, the fresnel, the reflection probes and the screen-space
 * distortion. A pool costs one more water plane, not a new render path.
 *
 * The prim itself keeps rendering. Water is transparent, so the prim's own top face reads
 * as the pool floor and its sides as the rim; a builder who wants open water sets the prim
 * transparent, which is the normal in-world workflow.
 */
class FSWolfWater : public LLSingleton<FSWolfWater>
{
    LLSINGLETON(FSWolfWater);
    ~FSWolfWater();

public:
    /** The keyword, matched case-insensitively anywhere in a description. */
    static const std::string KEYWORD;

    /** Called every frame from LLAppViewer::idle(). Rate-limits itself. */
    void idle();

    /**
     * Record a description the sim just sent. Called from LLSelectMgr's ObjectProperties
     * and ObjectPropertiesFamily handlers, which are the only two places one arrives.
     * An EMPTY description is recorded as emphatically as a non-empty one — that is how
     * water gets switched back off when a builder clears the keyword.
     */
    void noteDescription(const LLUUID& object_id, const std::string& description);

    /** Drop every surface and every pending request. Called on teleport. */
    void reset();

    /** Does this description ask for water? */
    static bool matches(const std::string& description);

private:
    void sweep();
    void requestPending();
    /** Create or re-fit the surface for one prim. */
    void ensureSurface(LLViewerObject* objectp);
    void destroySurface(const LLUUID& object_id);

    struct Surface
    {
        LLPointer<LLVOWater>    mWater;
        LLVector3               mPrimScale;     // what the surface was fitted to
        LLVector3               mPrimPosition;
    };

    /** object id -> its live water plane. */
    std::map<LLUUID, Surface> mSurfaces;
    /** object id -> the description the sim last gave us. */
    std::map<LLUUID, std::string> mDescriptions;

    struct Ask
    {
        F64 mSentAt = 0.0;
        S32 mTries  = 0;
    };
    /** object id -> when we last asked, and how many times running. */
    std::map<LLUUID, Ask> mAsked;

    /** Candidates gathered by this tick's sweep, sorted before they are sent. */
    struct Candidate
    {
        LLUUID  mId;
        F32     mDistSq;
        S32     mPriority;      // 0 root, 1 child, 2 refresh
    };
    std::vector<Candidate> mCandidates;

    F64 mNextSweep = 0.0;
    /** Diagnostics: when the next sweep summary is due, and what the last tick sent. */
    F64 mNextStatsLog = 0.0;
    S32 mLastRequestCount = 0;

    /**
     * The region handle the surface set belongs to. This class invalidates ITSELF on a
     * region change rather than waiting to be told about a teleport: localIds and the
     * object list are both rebuilt across the hop, so every description held from the old
     * region is a claim about a prim that can no longer be seen, and a surface parented to
     * a dead object is a dangling plane over the new region. Owning the invalidation here
     * means there is no teleport path that can forget to call it.
     */
    U64 mRegionHandle = 0;
};

#endif // FS_WOLFWATER_H
