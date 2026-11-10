/**
 * @file wolfundersea.h
 * @brief WolfViewer: sea plants, schools of fish and, in warm seas, the odd shark under the water of Wolf Territories
 *        regions (ticket 652, Jimmy Olsen's "Undersea with better scenery").
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

#ifndef WOLF_UNDER_SEA_H
#define WOLF_UNDER_SEA_H

#include "llpointer.h"
#include "llsingleton.h"
#include "lluuid.h"
#include "v3dmath.h"
#include "v3math.h"
#include "v4math.h"
#include "wolfdryvolumes.h"   // [T733 2026-11-10/] air-filled volumes: nothing grows or swims in them

#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

class LLImageRaw;
class LLVertexBuffer;
class LLViewerRegion;
class LLViewerTexture;

/**
 * Drawn by each viewer for itself, exactly as the birds and the dolphins are (wolfbirds.h, wolfsealife.h): no objects,
 * scripts or messages on the region. The grid's only part is the `undersea` block weather.php already sends each minute
 * (WolfRegionWeather::underSeaAllowed / underSeaWarm / underSeaCold / underSeaNear). The design, with every figure and
 * its source: docs/UNDERSEA_LIFE.md.
 *
 *  WHETHER  the owner's switch (on unless switched off), a Wolf region (visitors too), an Earth sky, the resident's own
 *           WolfViewerUnderSea; plants while the camera is under the water or within PLANT_ABOVE_M of it, fish while it is
 *           under or within FISH_ABOVE_M.
 *  WHERE    the sea only (terrain below the region's water height whose patch has arrived), never above the water, never
 *           where a prim reaches into the plant's height (an incremental scan of the objects near the camera).
 *  WHAT     plants placed by a hash of world cells (everyone sees the same garden): seagrass, kelp, branching and mound
 *           corals, sea fans, rocks, by depth and the sea's warmth; schools of fish that flock, keep off the seabed, the
 *           surface and builds, and scatter from swimmers; a rare shark in warm water that keeps away from everyone.
 *  COST     nothing while off or far above the water; plants: one static vertex buffer per 32 m chunk, built one a frame,
 *           swayed on the GPU (gWolfUnderSeaProgram); fish: one buffer written each frame, at most WolfViewerUnderSeaFishMax,
 *           lowered by the frame budget (WolfFrameGovernor).
 */
class WolfUnderSea : public LLSingleton<WolfUnderSea>
{
    LLSINGLETON(WolfUnderSea);
    ~WolfUnderSea();

public:
    /** Once a frame (llappviewer.cpp idle, after WolfSeaLife): decide, build the garden, swim. */
    void idle();
    /** The deferred geometry pass (pipeline.cpp, beside WolfSeaLife): the plants, then the fish. */
    void renderDeferred();
    S32 fishCount() const { return (S32)mFish.size(); }
    S32 chunkCount() const { return (S32)mChunks.size(); }

    // not COUNT / BOOL style names: platform headers #define many short words (wingdi.h, Xlib.h)
    enum PlantKind : U8 { P_SEAGRASS = 0, P_KELP = 1, P_BRANCH_CORAL = 2, P_MOUND_CORAL = 3, P_SEA_FAN = 4, P_ROCK = 5 };
    enum FishKind : U8 { F_SILVER = 0, F_REEF = 1, F_BIG = 2, F_SHARK = 3, F_COD = 4, FISH_KINDS = 5 };   // [COLD SEAS 2026-11-09/] F_COD

private:
    // ── the garden ──────────────────────────────────────────────────────────────────────────
    typedef std::pair<S64, S64> CellKey;
    struct CellKeyHash { size_t operator()(const CellKey& k) const; };

    struct Chunk
    {
        LLVector3d mOriginGlobal;      // the chunk's south-west corner, z = 0
        LLPointer<LLVertexBuffer> mVB; // chunk-local positions (x, y from the corner; z absolute)
        U32 mVerts = 0;
        F32 mZMin = 0.f, mZMax = 0.f;  // the plants' height range (for culling)
        U64 mBlockHash = 0;            // the objects' cells inside it when it was built
        U32 mClimate = 0;              // the climate and permission stamp it was built with (climateStamp)
        U64 mDryHash = 0;              // [T733/] the air-filled volumes over it when it was built (dryHash)
        F64 mLastWanted = 0.0;
        F64 mRetryAt = 0.0;            // > 0: some of its terrain had not come; build it again then
        bool mDirty = false;           // the objects round it changed: build it again
    };

    /** The height range of the prims reaching into a 2 m world cell (an obstacle for plants and fish). */
    struct Block { F32 mZMin = 1e9f, mZMax = -1e9f; };

    // ── the fish ────────────────────────────────────────────────────────────────────────────
    struct Fish
    {
        S32 mSchool = 0;
        LLVector3 mPos, mVel;     // agent metres, m/s
        F32 mLen = 0.2f;
        F32 mPhase = 0.f;
        F32 mPitch = 0.f, mHeading = 0.f;
        F32 mFlee = 0.f;          // seconds of flight left
        LLVector3 mFleeDir;
        // [LOOK 2026-11-09] each fish its own: a wandering offset from the school's goal (renewed every few seconds) and a
        // speed factor, so a school mills loosely instead of swimming in rigid rows
        LLVector3 mJitter;
        F64 mJitterUntil = 0.0;
        F32 mSpeedK = 1.f;
    };
    struct School
    {
        FishKind mKind = F_SILVER;
        U8 mColourway = 0;
        LLVector3 mGoal;          // agent metres
        F64 mNextGoal = 0.0;
        F32 mCruise = 0.5f;
        F32 mKeepUp = 0.f;        // metres above the seabed it prefers (big fish and sharks swim low)
        F32 mAlarm = 0.f;         // seconds the whole school stays alarmed (one of them saw a swimmer)
        LLVector3 mAlarmFrom;     // where the swimmer that alarmed it was (agent metres)
        F32 mCeiling = 0.f;       // this frame: the drawn water surface over its centre, less SURFACE_CLEAR_M
        bool mLeaving = false;
        bool mHasHome = false;    // reef and big fish: the reef patch they keep to
        LLVector4 mHome;          // its centre (agent x, y), the bed's height under it, its radius
    };

    void reset();
    void rebase(const LLVector3& delta);
    bool wanted(LLViewerRegion*& region) const;

    // the place
    F32 water() const;
    F32 depth(const LLVector3& agent) const;          // 0 on land, before the terrain has come, over a sea that may not hold them
    F32 seabed(const LLVector3& agent) const;         // terrain height at agent (x, y); -1e9 where depth() is 0
    LLViewerRegion* regionFor(const LLVector3& agent) const;   // the region whose sea holds them at agent, or nullptr
    U32 climateStamp() const;

    // the objects' cells
    void scanObjects(const LLVector3& cam);
    /** Whether a prim reaches into the box round world (global) metres gx, gy, `radius` out, between heights zlo..zhi, or
     *  that column reaches into an air-filled volume (dry; boats' hulls too unless `hulls` is false). */
    bool blocked(F64 gx, F64 gy, F32 radius, F32 zlo, F32 zhi, bool hulls = true) const;
    /** The plants' test: blocked() without the boats' hulls (they sail on: a garden built round one would keep the hole). */
    bool plantBlocked(F64 gx, F64 gy, F32 radius, F32 zlo, F32 zhi) const { return blocked(gx, gy, radius, zlo, zhi, false); }
    U64 blockHash(const LLVector3d& corner) const;

    // [T733 2026-11-10] the air-filled volumes (WolfDryVolumes: "wolfdry" prims, the Tunnel tool's boxes under the sea,
    // boats' hulls) - the same shapes the water fog and surface leave out; nothing grows or swims inside them
    struct DryVolume
    {
        WolfDryVolumes::Volume mVol;   // agent frame, this frame
        F32 mRadius = 0.f;             // its bounding sphere about mVol.mCentre
        LLVector3d mGlobal;            // mVol.mCentre in world (global) metres
        U64 mKey = 0;                  // its kind, place, turn and size, rounded (dryHash)
    };
    /** Once a frame: this frame's volumes into mDry, and mDrySig (the plants' volumes: all but hulls) for the garden. */
    void dryUpdate();
    /** Whether the vertical column at agent (x, y), `radius` out, zlo..zhi, reaches into one of mDry. */
    bool dry(const LLVector3& agent_xy, F32 radius, F32 zlo, F32 zhi, bool hulls) const;
    /** Whether an agent point is inside one of mDry. */
    bool inDry(const LLVector3& agent) const;
    /** The plants' volumes (not hulls) reaching over a chunk (change detection only). */
    U64 dryHash(const LLVector3d& corner) const;

    // the reef patches round a world point (agent x, y, the bed's height, the radius), for the fish to gather round
    void patchesNear(F64 gx, F64 gy, F32 reach, std::vector<LLVector4>& out) const;

    // the garden
    void garden(const LLVector3& cam, F64 now, bool plants_on);
    bool buildChunk(Chunk& c, S64 cx, S64 cy);

    // the fish
    void direct(const LLVector3& cam, F64 now, bool fish_on);
    bool pickSpot(FishKind kind, const LLVector3& cam, F32 near_m, F32 far_m, LLVector3& out, bool prefer_hidden) const;
    void spawn(FishKind kind, const LLVector3& cam, F64 now, S32 room, bool in_view);
    void threats(const LLVector3& cam);
    void swim(F32 dt, F64 now, const LLVector3& cam);
    void newGoal(School& s, S32 si, const LLVector3& cam, F64 now);

    // the look
    bool ensureFishBuffer();
    void buildFish(S32& verts);

    std::map<CellKey, Chunk> mChunks;
    std::unordered_map<CellKey, Block, CellKeyHash> mBlocks, mBlocksScan;
    S32 mScanIndex = 0;
    bool mScanDone = false;            // a whole pass of the object list has been made since the start / a teleport
    U64 mScanGeneration = 0;           // bumped each finished pass
    U64 mCheckedGeneration = 0;        // the pass the chunks were last checked against
    LLVector3d mScanCentre;            // the camera (global) when the pass began

    std::vector<DryVolume> mDry;       // [T733/] this frame's air-filled volumes
    U64 mDrySig = 0;                   // [T733/] the plants' ones, to rebuild the chunks they reach when they change
    U64 mCheckedDrySig = 0;

    std::vector<Fish> mFish;
    std::vector<School> mSchools;
    std::vector<LLVector3> mThreats;   // swimmers (avatars) and the camera under the water, agent metres
    std::vector<LLVector3d> mAvatarScratch;
    bool mSharkDecided = false;
    bool mSharkWanted = false;
    F64 mNextSharkRoll = 0.0;

    LLVector3d mLastOrigin;
    LLVector3 mLastCamera;
    bool mHaveLastCamera = false;
    F64 mLastIdle = 0.0, mNextDirect = 0.0, mLastLog = 0.0;
    F64 mCostIdleUs = 0.0, mCostBuildUs = 0.0, mCostFishMeshUs = 0.0;
    S32 mCostFrames = 0, mChunksBuilt = 0;
    bool mPlantsActive = false, mFishActive = false;

    LLPointer<LLVertexBuffer> mFishVB;
    // the atlas (our own procedural textures, wolfundersea.cpp make the rows): drawn a few rows a frame, then uploaded
    LLPointer<LLImageRaw> mAtlasRaw;
    S32 mAtlasRow = 0;
    LLPointer<LLViewerTexture> mAtlas;
    bool atlasReady();
};

#endif // WOLF_UNDER_SEA_H
