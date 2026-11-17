/**
 * @file wolffarproxy.h
 * @brief WolfViewer: beyond a switch distance a region's static objects are drawn as one baked low-detail model
 * (a proxy) and the real objects standing there are not drawn at all.
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

#ifndef WOLF_FAR_PROXY_H
#define WOLF_FAR_PROXY_H

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "llpointer.h"
#include "lluuid.h"
#include "llsingleton.h"
#include "v3math.h"

class LLCamera;
class LLImageRaw;
class LLSpatialBridge;
class LLSpatialGroup;
class LLVector4a;
class LLVertexBuffer;
class LLViewerObject;
class LLViewerRegion;
class LLViewerTexture;

/**
 * [W107 FAR HLOD 2026-11-14] Paul: "what would REALLY increase the performance". At Paddington Station with a 4096 m
 * draw distance every far object still costs a cull test, a level-of-detail update, draw calls, occlusion queries and
 * shadow work. Design, file format, switch rule and measurements:
 * firestorm/region-terrain-work/far-hlod-2026-11-14/DESIGN.md.
 *
 * The grid bakes one model per region from the region's own database (static objects only, cut on a 64 m grid of
 * cells, one texture atlas) and wolf-grid.com/hlod.php hands it out by the region's grid origin, as elev.php hands
 * out the far terrain's tiles (wolffarterrain.h). Here, for every connected region that has a model:
 *
 *  - per 64 m cell, d = the main camera's distance to the cell's box, D = WolfFarProxyDistance: the cell goes over to
 *    its proxy when d reaches D + 32 m and comes back to its real objects when d falls to D (so a camera standing still
 *    changes nothing), and each change is a dissolve of 0.75 s on the clock, f running 0 -> 1 or back;
 *  - f = 0: nothing of the proxy is drawn, the real objects are, as stock;
 *  - 0 < f < 1: the real objects are drawn and the cell's proxy is drawn over them through a noise mask that keeps
 *    the share f of its pixels;
 *  - f = 1: the proxy is drawn whole and an octree node of the region's static volume partition whose extents lie only
 *    over such cells fails the cull, for the main view and the shadow passes alike (hidesSubtree / hidesObjects) -
 *    unless something scripted or moving stands under it (below).
 *
 * A model's triangles are cut on the cell lines, so a cell's proxy is exactly what stands on that cell: an object
 * reaching over a nearer cell stays real, and no low-detail triangle comes nearer than D.
 *
 * Moving objects are not in a model and must never be hidden. An object that has moved is active for good
 * (LLDrawable::updateMove -> makeActive, lldrawable.cpp:822; LLPipeline::updateMovedList only calls makeStatic for a
 * drawable that is not ACTIVE, pipeline.cpp:2258-2260, which makeStatic then ignores, lldrawable.cpp:574) and is drawn
 * through a spatial bridge. The bridge is made IN the region's static volume octree (LLSpatialBridge's constructor,
 * lldrawable.cpp:1329-1340 part->put(this)) and at its first LLSpatialBridge::updateMove goes over to the region's
 * bridge partition (lldrawable.cpp:1694-1707; LLVolumeBridge is PARTITION_BRIDGE, llvovolume.cpp:5436), which the cull
 * hooks never touch - found on the first round 2 sessions, 17 Nov, where no mover was ever counted in the volume
 * octree. So a mover is safe twice over: in the bridge partition nothing hides it, and while a bridge is still in
 * the volume octree every cell it stands on (8 m fine cells under its box and 16 m round it) keeps its real objects,
 * whatever its distance: no node holding the bridge, and no node above it, then passes the test. The same mask holds
 * the places the MODEL lists: the box of every linkset the baker left out because it can move or is scripted and to
 * be kept real (Paul, 15 Nov: "anything scripted or with a wolf-rail script stays a real object, even when it's
 * standing still" - a train at a platform that has not moved since the viewer connected is a static object here).
 * What is left out and what is kept real are so one list, made by the baker, never two guesses that must agree: the
 * model lists each such linkset twice, as the box where it stood at the bake and as its root prim's id, and the id
 * keeps it real wherever it stands now (refreshKeepLive) - a train that has since come to rest at another platform.
 * Avatars, attachments, trees,
 * grass, particles, terrain, water and sky are other partitions and are never touched.
 *
 * Wolf Territories regions only (WolfGrid::showsWolfRegion); the setting WolfFarProxy turns it off. With the setting off, on
 * another grid, while the build tools are open, in a reflection probe's pictures, or for a region with no model,
 * everything is stock.
 */
class WolfFarProxy : public LLSingleton<WolfFarProxy>
{
    LLSINGLETON(WolfFarProxy);
    ~WolfFarProxy();

public:
    /** Every frame from LLAppViewer::idle(), after LLPipeline::updateMove (the keep-real mask needs this frame's
     *  boxes): which regions have models, fetches, building, and each cell's f. */
    void idle();

    /** From LLPipeline::renderGeomDeferred, for the main camera only: the proxies of cells with f > 0. */
    void renderDeferred();

    /** From LLPipeline::renderShadow with the stock shadow program bound (static pass): the opaque triangles of the
     *  proxies of cells with f > 0 that are in the shadow camera's frustum. Leaves that program bound. */
    void renderShadowOpaque(LLCamera& shadow_cam);

    /** From LLPipeline::renderShadow with gDeferredTreeShadowProgram bound (alpha cut 0.5 set here): the alpha-cut
     *  triangles of the same proxies. Leaves that program bound. */
    void renderShadowAlphaCut(LLCamera& shadow_cam);

    /** From LLOctreeCull::earlyFail: true = neither this node nor anything under it is looked at. */
    static bool hidesSubtree(const LLSpatialGroup* group);

    /** From LLOctreeCull::processGroup: true = this node's own objects are not drawn. */
    static bool hidesObjects(const LLSpatialGroup* group);

    /** From LLSpatialGroup, where a spatial bridge (a moving object's own octree) is put on / taken off a node's
     *  bridge list, and from the bridge's destructor: the cells a bridge stands on keep their real objects. */
    static void bridgeIn(LLSpatialBridge* bridge, LLSpatialGroup* group);
    static void bridgeOut(LLSpatialBridge* bridge);

    /** From LLPipeline::cleanup: the vertex buffers and atlases go while the GL context is still there. */
    void releaseAll();

    static bool enabled();

    /** A model's bytes, checked and unpacked (on a worker thread). */
    struct Decoded;

private:
    struct Part
    {
        U16 mCellX = 0, mCellY = 0;
        LLPointer<LLVertexBuffer> mVB;
        U32 mVerts = 0;
        U32 mOpaque = 0;     // index counts: the opaque triangles first, then the alpha-cut ones
        U32 mCut = 0;
        F32 mZMin = 0.f, mZMax = 0.f;
    };

    struct Model
    {
        U32 mBuildTime = 0;
        U32 mRegionSize = 0;      // metres
        U32 mCellM = 0;           // metres
        U32 mSide = 0;            // cells per side
        std::vector<Part> mParts;                 // sorted by cell
        std::vector<U32> mCellFirst;              // mSide * mSide + 1: a cell's parts are [first[c], first[c + 1])
        std::vector<F32> mCellZMin, mCellZMax;    // of a cell's parts; min > max = the cell has none
        LLPointer<LLViewerTexture> mAtlas;
        U32 mKeepSide = 0;                        // fine cells per side of the keep-real mask
        std::vector<U8> mKeepBaked;               // the places the baker left a mover or kept-real scripted object out
        U32 mKeepBoxes = 0;                       // how many the file listed
        std::vector<LLUUID> mKeepIds;             // the root prims of those linksets: kept real wherever they stand now
    };

    struct Region
    {
        enum State { NEW, INFO, MODEL, DECODING, BUILDING, IDLE };
        State mState = NEW;
        U32 mGridX = 0, mGridY = 0;               // the region's origin, 256 m cells (hlod.php rx, ry)
        F64 mNextInfoAt = 0.0;
        U32 mSerial = 0;                          // of the request in flight: an answer for an older one is dropped
        U32 mFailedBuildTime = 0;                 // a build that would not decode or build: not fetched again
        std::shared_ptr<Decoded> mDecoded;        // being turned into vertex buffers, a few parts a frame
        std::unique_ptr<Model> mBuilding;
        size_t mBuiltParts = 0;
        std::unique_ptr<Model> mModel;            // drawn
        // per cell of mModel, this frame
        std::vector<F32> mFade;                   // f: 0 = real objects, 1 = proxy, between = dissolving
        std::vector<U8> mFar;                     // which side the cell is on (changes only outside the hysteresis ring)
        std::vector<U8> mHidden;                  // f == 1: real objects wholly on such cells are not drawn, unless ...
        // ... the keep-real mask: fine cells (8 m) the model lists (Model::mKeepBaked) or under a moving object
        // (added every frame); mKeepSum is mKeep's summed-area table for the cull hooks
        std::vector<U8> mKeep;
        std::vector<U32> mKeepSum;
        // the fine cells under the linksets any model lists by id, where they stand on this region now and are not
        // (yet) moving objects (refreshKeepLive, every frame)
        std::vector<U8> mKeepLive;
        bool mAnyFade = false;
        // the cull hooks compare this with the partition's region and never follow it
        const LLViewerRegion* mRegionp = nullptr;
        LLVector3 mOriginAgent;
    };

    void refreshRegions();
    void step(U64 handle, Region& r, F64 now);
    void fetch(U64 handle, Region& r, bool model);
    static void fetchCoro(U64 handle, U32 serial, bool model, std::string url);
    void gotInfo(Region& r, const std::vector<U8>& body, F64 now);
    void gotModel(U64 handle, Region& r, std::shared_ptr<std::vector<U8>> body);
    void decoded(U64 handle, U32 serial, std::shared_ptr<Decoded> d);
    bool buildSome(Region& r);
    void computeFades();
    void refreshKeepLive();
    void markKeepLive(const LLViewerObject* object);
    static void markKeep(const Model& m, const LLVector3& origin_agent, std::vector<U8>& grid, const LLVector4a* ext, F32 margin);
    void drawParts(LLCamera& camera, bool far_clip, bool opaque, bool cut, bool fade_uniform);
    void logStats(F64 now);
    void clear();

    static bool allHidden(const LLSpatialGroup* group, bool subtree);

    std::map<U64, Region> mRegions;       // by region handle
    // The regions whose model has a cell at f = 1 this frame, for the cull hooks: they run for every octree node of
    // every cull, and LLSingleton::instance() takes a lock each call (measured: 7% of the main thread). Entries point
    // into mRegions; set by computeFades, emptied before anything is erased from mRegions.
    static std::vector<const Region*> sHiding;
    U32 mKeepCells = 0;                   // fine cells kept real by scripted or moving objects, this frame
    U32 mKeepIdsFound = 0;                // the listed linksets this viewer has an object for, at the last refreshKeepLive
    U32 mFarMovers = 0, mFarMoversSeen = 0;   // moving objects standing on proxy cells this frame; those a cull let through lately
    // for the log line: the first few of those movers - which region, where on it, and whether a cull let it through
    struct FarMover
    {
        U32 mGridX = 0, mGridY = 0;
        LLVector3 mLocal;
        bool mSeen = false;
    };
    static constexpr U32 FAR_MOVER_SAMPLES = 6;
    FarMover mFarSample[FAR_MOVER_SAMPLES];
    F64 mLastFadeAt = 0.0;
    F64 mNextRefresh = 0.0;
    S32 mFetching = 0;
    U32 mNextSerial = 0;                  // one count for every region's requests, so a region that went and came back never matches an old answer
    bool mActive = false;                 // this frame: enabled, and the build tools are closed
    F64 mNextLog = 0.0;
    U32 mDrawnParts = 0;                  // main pass, since the last log line
    U32 mShadowParts = 0;                 // shadow passes, since the last log line
    U32 mLogFrames = 0;                   // frames since the last log line
};

#endif // WOLF_FAR_PROXY_H
