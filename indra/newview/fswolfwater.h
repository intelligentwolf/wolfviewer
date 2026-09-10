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
 * A prim's description is NOT in ObjectUpdate, and the only two messages that carry it
 * take one ObjectID per request — see wolfobjectprops.h for the whole story. This class
 * declares interest in every volume prim inside the draw distance through
 * WolfObjectProps::want() (nearest first, roots before linkset children, throttled,
 * refreshed only with idle budget) and reads the answer back with WolfObjectProps::get().
 * It sends nothing itself. Source: wolfstorm/js/world/wolfwater.js, which was rerouted
 * through the shared harvester (object_props_harvester.js) on 2026-09-03 for the same
 * reason WolfViewer is: the boat rocker wants the same strings for the same prims, and two
 * private "already asked" maps meant the same prim was asked twice.
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

    /** Drop every surface. Called on a region change. */
    void reset();

    /** Does this description ask for water? */
    static bool matches(const std::string& description_lower);

private:
    void sweep();
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
    F64 mNextSweep = 0.0;
    /** Diagnostics: when the next sweep summary is due. */
    F64 mNextStatsLog = 0.0;

    /**
     * The region handle the surface set belongs to. This class invalidates ITSELF on a
     * region change rather than waiting to be told about a teleport: localIds and the
     * object list are both rebuilt across the hop, so a surface parented to an object from
     * the old region is a dangling plane over the new one. Owning the invalidation here
     * means there is no teleport path that can forget to call it. (The descriptions
     * themselves live in WolfObjectProps, which invalidates itself the same way.)
     */
    U64 mRegionHandle = 0;
};

#endif // FS_WOLFWATER_H
