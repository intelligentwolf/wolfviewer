/**
 * @file wolffarground.h
 * @brief WolfViewer: regions that are not connected yet stand in as their world-map tiles.
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

#ifndef WOLF_FAR_GROUND_H
#define WOLF_FAR_GROUND_H

#include <vector>

#include "llpointer.h"
#include "llsingleton.h"
#include "v3dmath.h"

class LLVertexBuffer;
class LLViewerFetchedTexture;
class LLViewerTexture;

// Paul, 2026-10-07, flying the Concorde: "i cant see anythign below me excpt white and the odd
// square", then "i do like the idea of temporarily using maptiles". A viewer is only connected to
// the regions within the server's region view distance, and draws nothing of the rest. Every 256 m
// cell within reach of the camera that the world map knows a region on, but that no connected region
// covers, is drawn as that region's map tile, flat at the water level - a top-down picture of the
// land and what is built on it, which is what it looks like from the air. When the region connects,
// its cells drop out and the real land takes over. The sea is drawn under the tiles as everywhere else
// (only their land shows: the water in a tile is cut away), and each tile lies a little above the sea,
// more the further it is, so the two never fight for the same depth.
// <WolfViewer 2026-10-08> Paul, standing at Wolf Territories Home: "why is there a big grey hole in the
// water" - the sea used to be left out under tiles, and where a tile's water was cut away the paler
// sky-drawn horizon sea showed through.
//
// Drawn in the deferred geometry pass with the alpha-cut diffuse program (LLDrawPoolGrass's), so the
// tiles are lit, fogged and darkened at night like any other surface, and their water - cut away by
// WolfMapGlobe::keyedTile - shows the sea beneath.
// Only while flying (WolfAltitudeSky::agentFlying) at least 100 m up. Wolf Territories only;
// WolfViewerFarGroundMapTiles turns it off.
class WolfFarGround : public LLSingleton<WolfFarGround>
{
    LLSINGLETON(WolfFarGround);
    ~WolfFarGround();

public:
    /** Every frame from LLAppViewer::idle(): what to draw, at most once a second, and the sea re-cut round it. */
    void idle();

    /** From LLPipeline::renderGeomDeferred, for the main camera only. */
    void renderDeferred();

    /** A region connected or went: look again on the next frame. */
    void invalidate() { mNextRefresh = 0.0; }

private:
    struct Tile
    {
        LLPointer<LLViewerFetchedTexture> mTexture;
        // The tile with its water see-through (WolfMapGlobe::keyedTile): drawn with an alpha cut, so
        // the sea behind shows where the map shows water. Null until made.
        LLPointer<LLViewerTexture> mDraw;
        U32 mGridX = 0, mGridY = 0;          // the tile's south-west region, grid units
        std::vector<U64> mCells;              // its cells to draw (region handles, metres)
        U32 mStart = 0, mCount = 0;           // index range in mVB
    };
    void refresh();
    void rebuildBuffer();
    void clear();

    std::vector<Tile> mTiles;
    LLPointer<LLVertexBuffer> mVB;
    U32 mVerts = 0;
    F64 mNextRefresh = 0.0;
    LLVector3d mBuiltOrigin;                  // the agent region's origin the buffer was built in
    F32 mBuiltWaterZ = 0.f;
    S32 mLevel = 1;
};

#endif // WOLF_FAR_GROUND_H
