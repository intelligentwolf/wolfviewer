/**
 * @file wolfterrainlevels.h
 * @brief WolfViewer: sky levels - extra native terrain heightfields of a region, high above its ground (ticket 541).
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
#ifndef WOLF_TERRAINLEVELS_H
#define WOLF_TERRAINLEVELS_H

// [WOLF SKY LEVELS 2026-11-05] Ticket 541 (Jimmy Olsen): ground high above a region that people walk on.
// Design and server: firestorm/region-terrain-work/stacked-terrain-2026-11-05/README.md (WolfSim WolfSkyLevelsModule.cs,
// SharedBase WolfSkyLevels.cs).
//
// A sky level is a second LLSurface of the same region (setWolfSkyLevel: no water of its own, banded by its own height above
// its base), fed by the SAME LayerData bit stream the ground gets (LLSurface::decompressDCTPatch), but from the region's
// "WolfSkyLevels" capability instead of LayerData packets. Stock viewers never see the stream: a LayerData packet with a layer
// type they do not know is LL_ERRS in LLVLManager::addLayerData (llvlmanager.cpp:91-94), so levels never travel as LayerData.
// Only the patches a level has are sent, and the surface makes a patch only when its first data arrives (LLSurface::getPatch),
// so a level costs nothing where it has no ground. The top is drawn, culled and shadowed as the ground's own
// (LLVOSurfacePatch / LLDrawPoolTerrain); the rock underneath and the walls round the edge (WolfSkyLevel.TryUnderside /
// BuildBlockMesh, which the region's physics also uses) are drawn here (renderDeferred).
//
// Capability (WolfSkyLevelsModule.cs OnRequest):
//   GET            -> LLSD {success, rev, max_levels, quota, used, max_height, max_side, may_manage, levels:[{id,name,base,top,patches,rev}]}
//   GET ?level=id  -> the level's LayerData stream (application/octet-stream)
//   POST LLSD {op: create|delete|set|apply|remove|undo, ...} -> {success, message?, level, changed, refused, no_room, rev}
// GenericMessage "WolfSkyLevels" (invoice = the region's id) [rev, ids] goes to every viewer that read the list when anything
// changes (WolfSkyLevelsModule.cs Tell).

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "stdtypes.h"
#include "llpointer.h"
#include "llsd.h"
#include "v3dmath.h"
#include "v3math.h"

class LLAgent;
class LLSurface;
class LLVertexBuffer;
class LLViewerRegion;

class WolfTerrainLevels
{
public:
    static constexpr const char* CAP = "WolfSkyLevels";       // WolfSkyLevelsModule.cs CAP
    static constexpr const char* MESSAGE = "WolfSkyLevels";   // WolfSkyLevelsModule.cs MESSAGE_OUT
    static constexpr S32 PATCH = 16;                          // WolfSkyLevels.cs PatchSize
    static constexpr S32 UNDER = 4;                           // WolfSkyLevels.cs UndersideStep

    struct Piece
    {
        LLPointer<LLVertexBuffer> mVB;
        U32 mIndices = 0;
        LLVector3 mMin, mMax;           // agent frame, for the frustum
    };

    struct Level
    {
        S32 mId = 0;
        std::string mName;
        F32 mBase = 0.f;                    // the level's height (its rock hangs below it), metres
        F32 mTop = 0.f;                     // its highest sample
        S64 mRev = -1;                      // the level's revision when its stream was read
        std::unique_ptr<LLSurface> mSurface;
        std::vector<std::pair<S32, S32>> mPatches;   // the patches the stream held (patch x, y)
        // the underside and walls: one buffer per 128 m block (8 x 8 patches, so U16 indices always do)
        std::map<std::pair<S32, S32>, Piece> mPieces;
        LLVector3d mBuiltOrigin;            // the agent origin the pieces were built in
        bool mPiecesDirty = true;
        bool hasPatch(S32 px, S32 py) const;
    };

    explicit WolfTerrainLevels(LLViewerRegion* regionp);
    ~WolfTerrainLevels();

    // The capability arrived (LLViewerRegion::setCapabilitiesReceived): read the list and every level.
    void onCapabilitiesReceived();
    // Read the list again (and the levels whose revision changed); a refresh while one runs runs once more after it.
    void refresh();
    bool hasCapability() const;

    // Feeds one level's LayerData-format stream (STRIDE u16, patch size u8, layer type u8 'L' or 'M', patches,
    // END_OF_PATCHES) into a new surface for that level, replacing the one it had. False + why on a bad stream.
    bool applyLevelStream(S32 id, const std::string& name, F32 base, F32 top, S64 rev, const U8* data, U32 size, std::string& why);
    void removeLevel(S32 id);
    void clear();

    // LLViewerRegion::idleUpdate / forceUpdate and LLWorld::updateVisibilities, as for the region's ground.
    void idleUpdate(bool pbr, F32 max_update_time);
    void updatePatchVisibilities(LLAgent& agent);

    // LLViewerRegion::setOriginGlobal, as for the ground (LLSurface::setOriginGlobal); the undersides are rebuilt.
    void setOriginGlobal(const LLVector3d& origin_global);

    // LLWorld regionLandInFrustum: the height band the region's land takes, sky land included.
    bool hasLevels() const { return !mLevels.empty(); }
    F32 minZ() const;
    F32 maxZ() const;

    // The height of the ground or level something at region (x, y, z) stands on: the highest surface at (x, y) at or below
    // z + tolerance. The ground counts everywhere; a level only where it has ground. above_ground (optional): true when a level
    // won over the ground.
    F32 resolveSupportHeight(F32 x, F32 y, F32 z, F32 tolerance, bool* above_ground = nullptr) const;

    // One level's top at a whole metre (x, y) of the region, as the stream gave it; false where it has no ground there.
    bool sample(const Level& level, S32 x, S32 y, F32& out) const;
    // WolfSkyLevel.TrySurface: the top at region (x, y) by the region's own triangles; false where it has no ground.
    bool surface(const Level& level, F32 x, F32 y, F32& out) const;

    const std::vector<Level>& levels() const { return mLevels; }
    const Level* findLevel(S32 id) const;
    const LLSD& info() const { return mInfo; }          // the last list answer (undefined until one arrived)
    bool mayManage() const { return mInfo.isMap() && mInfo["may_manage"].asBoolean(); }
    LLViewerRegion* getRegion() const { return mRegionp; }

    // Ticks whenever a list or a level changes anywhere, so a window can redraw (WolfFloaterSkyLevels::draw).
    static U32 changeCount() { return sChangeCount; }

    // The floater's brushes - the same rules as WolfStorm's wolf_sky_levels.js brush(): the w x h rectangle of whole metres
    // round (cx, cy), cells NaN where nothing is to change (WolfSkyLevelsModule.Apply leaves NaN alone), each between the
    // level's base and max_height. tool: make (every cell `height`), raise / lower (by `amount` at the centre, a cosine to the
    // edge of the circle), flatten (towards the centre's height), smooth (towards the 5 x 5 mean). False outside the region.
    bool brush(const Level& level, const std::string& tool, F32 cx, F32 cy, S32 size, F32 amount, F32 height,
               S32& x0, S32& y0, S32& w, S32& h, std::vector<F32>& heights) const;

    // POST one change to this region; `done` gets the region's answer, or {success:false, message} when it did not answer.
    // The list is read again after every answer. The region takes changes only from an avatar standing in it.
    void post(const LLSD& body, std::function<void(const LLSD&)> done);
    static LLSD applyBody(S32 level, S32 x, S32 y, S32 w, S32 h, const std::vector<F32>& heights, const std::string& label);

    // Every region's undersides and walls, from LLPipeline::renderGeomDeferred beside the far terrain.
    static void renderDeferred();

    // [SKY LAND TOOLS 2026-11-10] Paul: "fix the tools first, then announce" - the land brush (lltoolbrush.cpp) and the Terraform
    // window (wolfterraform.cpp) shape a sky level too (README.md 3.5 Editing). What they share:
    // The highest ground a level may have: the region's max_height (WolfSkyLevelsModule.cs OnRequest = Constants.MaxTerrainHeightmap,
    // Constants.cs:166 4000 m); the region refuses a whole "apply" with any height above it or below the level's base
    // (WolfSkyLevelsModule.Edit.cs Apply).
    F32 maxHeight() const;
    // Where the ray o + t * d (region metres, d a unit vector) first meets `level`'s top from above, 0 <= t <= max_t. A ray that
    // reaches the level from below (its rock) never hits the top.
    bool rayTop(const Level& level, const LLVector3& o, const LLVector3& d, F32 max_t, F32& t) const;
    // The land under the pointer at window (mx, my): the nearest sky level top on the ray from the camera (any region's), else the
    // ground (LLViewerWindow::mousePointOnLandGlobal, which knows only the ground). level = 0 for the ground.
    static bool pickLand(S32 mx, S32 my, LLVector3d& hit_global, LLViewerRegion*& regionp, S32& level);
    // Only `level` of `regionp`: its top under the pointer, else where the ray crosses the flat plane at plane_z (region metres),
    // so a stroke or a box can go on past the level's edge.
    static bool pickOnLevel(S32 mx, S32 my, LLViewerRegion* regionp, S32 level, F32 plane_z, LLVector3d& hit_global);
    // post() for a coroutine: the region's answer (or {success:false, message}); the list is read again after it.
    static LLSD postAndWait(LLViewerRegion* regionp, const LLSD& body);

    // [W104 FEATFIX review 2026-11-11] Two kinds of undo touch sky land: the Sky Land window's "Undo my last change" sends the
    // region's "undo" (WolfSkyLevelsModule.Edit.cs Undo: the caller's LAST change in this region, whatever made it), while the
    // land brush and the Terraform window keep their own histories and undo by sending the old heights back ("apply", itself a
    // change the region remembers). So the window's Undo is offered only for the window's own changes: those made since this
    // viewer's last brush stroke / brush Undo / Terraform job here (or, when nothing else changed sky land here this session, as
    // before). Otherwise it is refused with where to undo instead (stroke, brush Undo, window Undo would re-apply the stroke).
    enum EEditor { EDITOR_WINDOW, EDITOR_BRUSH, EDITOR_TERRAFORM };
    /** A change the region kept (it remembers it as one undo step). */
    void noteChange(EEditor by);
    /** May the Sky Land window's Undo go now? If not, `why` says where to undo instead. */
    bool windowMayUndo(std::string& why) const;
    /** The window's Undo went through (the region took back one of the window's steps). */
    void noteWindowUndo();

private:
    Level* findLevelMutable(S32 id);
    bool vertexTop(const Level& level, S32 X, S32 Y, S32 px, S32 py, F32& out) const;
    F32 undersideLattice(const Level& level, S32 X, S32 Y, S32 px, S32 py) const;
    void buildPieces(Level& level);
    void renderLevels();
    static void fetchCoro(U64 handle, U32 serial, std::string url);
    static void postCoro(U64 handle, U32 serial, std::string url, LLSD body, std::function<void(const LLSD&)> done);
    static WolfTerrainLevels* lookup(U64 handle, U32 serial);
    static void registerDispatcher();

    LLViewerRegion* mRegionp;
    std::vector<Level> mLevels;
    LLSD mInfo;
    U32 mSerial;                // tells a coroutine's region from a later region with the same handle
    bool mFetching = false;
    bool mAgain = false;
    S32 mWindowSteps = 0;               // [review] the window's changes on top of the region's history (this viewer's)
    bool mOtherChanged = false;         // [review] the brush or the Terraform window changed sky land here this session
    EEditor mLastOther = EDITOR_BRUSH;

    static U32 sChangeCount;
    static U32 sNextSerial;
};

#endif // WOLF_TERRAINLEVELS_H
