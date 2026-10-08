/**
 * @file wolffarterrain.h
 * @brief WolfViewer: the grid's ground beyond the terrain the regions have sent, from coarse elevation tiles.
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

#ifndef WOLF_FAR_TERRAIN_H
#define WOLF_FAR_TERRAIN_H

#include <map>
#include <unordered_set>
#include <tuple>
#include <vector>

#include "llpointer.h"
#include "lluuid.h"
#include "llsingleton.h"
#include "v3dmath.h"

class LLImageRaw;
class LLVertexBuffer;
class LLViewerTexture;
class LLViewerFetchedTexture;

// <WolfViewer 2026-10-08> FAR TERRAIN. Paul, flying over Ireland: "its all water ... think about how we can get that
// terrain showing", then "do the later thing now please and the same for all other regions". A region sends its
// terrain only round the avatar (4 km at most, nothing above 1,500 m: TerrainModule.cs:1679), so from the air a
// big region - Ireland is 460.8 km - was its water plane and nothing else.
//
// Every region bakes its own ground as coarse ELEVATION TILES (WolfSim WolfElevationModule, lazily, in the
// background) and wolf-grid.com/elev.php hands them out: z 4..8, named and aligned like the map tiles (a zoom z tile
// covers 2^(z-1) 256 m cells; 33 x 33 samples, 64 m apart at z4 up to 1024 m at z8), each sample the LOWEST ground
// near it. This draws them: a quadtree from z8 down to z4 picks finer tiles nearer the camera, and every region on
// the world map that overlaps a tile gives its own piece of it (a tile is clipped to its region). A connected region's
// piece is dressed in that region's own four ground textures, layered by height as LLVLComposition::generateHeights
// layers the near ground (baked on the CPU, a couple of pieces a frame); any other piece, and one waiting for its bake,
// is draped with the same map tile WolfFarGround uses (water cut away). Being the lower envelope, and sunk a little more with distance,
// it lies UNDER the real terrain wherever that has arrived, so it is drawn for connected regions as well: it shows
// only where the real ground is missing. Below the water level the water covers it.
//
// Only while flying at least 100 m up (as WolfFarGround), Wolf Territories only; WolfViewerFarTerrain turns it off.
// A tile a region has not baked yet (404) is asked again 10 minutes later; until then WolfFarGround's flat map
// tile stands in, as before.
class WolfFarTerrain : public LLSingleton<WolfFarTerrain>
{
    LLSINGLETON(WolfFarTerrain);
    ~WolfFarTerrain();

public:
    /** Every frame from LLAppViewer::idle(): what to draw (at most once a second), fetches, meshes. */
    void idle();

    /** From LLPipeline::renderGeomDeferred, for the main camera only. */
    void renderDeferred();

    /** Is the 256 m cell whose south-west corner is at global (x, y) metres drawn here now? (WolfFarGround then
     *  draws no flat map tile for it.) */
    bool coversCell(U32 x, U32 y) const;

    static bool enabled();

private:
    // A region's piece of a tile: (region grid x, region grid y, z, tile grid x, tile grid y)
    typedef std::tuple<U32, U32, S32, U32, U32> PieceKey;

    struct Piece
    {
        enum State { NEW, FETCHING, READY, MISSING };
        State mState = NEW;
        F64 mRetryAt = 0.0;
        F64 mLastWanted = 0.0;
        std::vector<S16> mHeights;           // 33 x 33, quarter metres, S16_MIN = none
        LLPointer<LLVertexBuffer> mVB;
        U32 mIndices = 0;
        LLVector3d mBuiltOrigin;             // the agent frame origin the vertices are relative to
        U32 mRegionX = 0, mRegionY = 0;      // the region, grid cells
        U32 mRegionW = 0, mRegionH = 0;      // its size, cells
        LLPointer<LLViewerTexture> mGround;  // the region's own ground textures, baked for this piece (connected regions)
        bool mBuildFailed = false;           // no mesh from these heights (too few samples): not tried again until new ones
        F64 mBakeRetryAt = 0.0;              // a bake that could not run (textures not loaded, PBR estate) waits until then
    };

    struct TileTex
    {
        LLPointer<LLViewerFetchedTexture> mTexture;
        LLPointer<LLViewerTexture> mDraw;    // water cut away (WolfMapGlobe::keyedTile); null until made
        F64 mLastWanted = 0.0;
    };

    void refresh();
    void fetch(const PieceKey& key);
    void buildMesh(const PieceKey& key, Piece& p);
    bool bakeGround(const PieceKey& key, Piece& p);
    LLImageRaw* detailSample(LLViewerFetchedTexture* tex);
    void clearAll();

    static void fetchCoro(PieceKey key, std::string url);

    std::map<PieceKey, Piece> mPieces;
    std::map<std::tuple<S32, U32, U32>, TileTex> mTextures;   // (z, tile grid x, tile grid y)
    std::map<LLUUID, LLPointer<LLImageRaw>> mDetail;          // detail textures, small copies for baking ground
    std::map<LLUUID, F64> mDetailRetryAt;                     // a read back that gave nothing is not tried again before
    std::vector<PieceKey> mDraw;                              // pieces to draw this frame (READY, wanted)
    std::unordered_set<U64> mCovered;                         // cells (handles, metres) of regions NOT connected drawn now
    F64 mNextRefresh = 0.0;
    S32 mFetching = 0;
};

#endif // WOLF_FAR_TERRAIN_H
