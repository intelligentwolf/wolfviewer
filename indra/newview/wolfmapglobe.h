/**
 * @file wolfmapglobe.h
 * @brief World map on Wolf Territories: animated water under the map tiles, and a globe you
 *        spin and zoom into that flattens into the ordinary map (WolfViewer 2026-10-02).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Copyright (C) 2026, IntelligentWolf Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef WOLF_MAPGLOBE_H
#define WOLF_MAPGLOBE_H

#include "llsingleton.h"
#include "llpointer.h"
#include "v3dmath.h"
#include <map>
#include <vector>

class LLViewerTexture;
class LLViewerFetchedTexture;
class LLUIImage;

// Paul 2026-10-02: "now we colour empty regions the same as normal water as there is no empty
// regions when on wolf territories grid", "could we use an animated water gif instead and just
// tile it?", "start off like a globe and the user can spin and then zoom in like google earth".
//
// WATER. The region tiles paint water in the region's MapColorWater (#112D54 on this grid) and the
// map service fills empty space in its zoomed-out tiles with (29,72,96) (MapImageService.cs:64).
// Both are made see-through as each tile loads, and a looping water animation (skins textures
// wolfmap/water_NN.png) is drawn underneath, so all the sea moves as one.
//
// GLOBE. The flat map wrapped onto a sphere around the point at the centre of the view: a point
// d metres from it sits |d|/R radians round the sphere, in the same direction (azimuthal
// equidistant). At the centre the globe's scale is the flat map's in every direction, so zooming
// in bends the globe into the flat map with nothing jumping; zooming out does the reverse.
// Spinning is panning: dragging moves the centre point across the grid.
//
// SPACE (2026-10-03, Paul: "add stars and stuff to the back ground of it please so that it looks
// more realistic", "when you rotate the globe it should move the stars etc behind it"). The same
// sky as WolfStorm's map (js/world/map_globe.js _makeSky / _drawSpace, same seed, same stars),
// turning as the globe is spun.
class WolfMapGlobe : public LLSingleton<WolfMapGlobe>
{
    LLSINGLETON_EMPTY_CTOR(WolfMapGlobe);
public:
    // On Wolf Territories with the setting WolfMapGlobe on. Everything here is a no-op otherwise.
    bool active() const;

    // The whole map's box, read from wolfstorm.app/php/map_extent.php (non-cancelled regions in
    // grid.regions). The globe is sized from it: until it has arrived there is no globe, only the
    // flat map (with its moving water). extentGeneration() changes each time a new box arrives.
    bool hasExtent() const { return mHaveExtent; }
    U32 extentGeneration() const { return mExtentGen; }
    // The globe's radius in metres: the map's longer side over pi, so a map centred on the globe
    // reaches its rim and covers its face (Paul 10-02: "calculate the actual width and height of
    // the whole map then paint it on so the whole map is covered").
    F64 radius() const { return mRadius; }

    // The map view's size, each frame (the globe's thresholds depend on how big it is on screen).
    // Also asks for the map's box when it is missing or ten minutes old.
    void setViewSize(S32 width, S32 height);

    // 1 = the full globe, 0 = the flat map, in between = bending, for a map scale in pixels per
    // region. Always 0 when !active() or before the map's box has arrived.
    F32 amount(F32 pixels_per_region) const;

    // Zoom (LLWorldMapView::zoomFromScale units) at which the whole globe fits a view this size.
    F32 globeZoom(S32 width, S32 height) const;
    // Zoom a double-click on the globe flies in to (the flat map).
    F32 flyInZoom() const;
    // Zoom that shows a region of this size (metres) across ~60% of the view; never further out
    // than the flat map.
    F32 regionZoom(F32 size_x_m, F32 size_y_m, S32 width, S32 height) const;

    // The view centre kept on the grid, so the grid never spins round to the antipode.
    LLVector3d clampCentre(const LLVector3d& centre) const;

    // Called by LLWorldMipmap::loadObjectsTile for every new tile: keep its decoded pixels so a
    // see-through copy can be made.
    void prepareTile(LLViewerFetchedTexture* tile) const;
    // The see-through copy of a loaded tile, made on first use (a few per frame); NULL until then.
    LLViewerTexture* keyedTile(LLViewerFetchedTexture* tile, S32 level, U32 grid_x, U32 grid_y);

    // One frame's view of the map. centre: global point at the middle of the view; ppr: pixels per
    // region; t: amount().
    struct View
    {
        LLVector3d centre;
        F32 ppr = 1.f;
        F32 t = 0.f;
        F32 width = 0.f;
        F32 height = 0.f;
        F64 radius = 1.0;   // radius()
    };

    // Screen position of a global point. Returns false if it is round the back of the globe.
    // shade (optional) is the globe's lighting there, 1 on the flat map.
    static bool project(const View& v, const LLVector3d& global, F32& x, F32& y, F32* shade = nullptr);
    // The global point under a screen position. Returns false off the edge of the globe.
    static bool unproject(const View& v, F32 x, F32 y, LLVector3d& global);

    // The flat map's water: the whole view, moving water. Drawn where the plain background was.
    void drawFlatWater(const View& v);
    // The globe: space, atmosphere, water, the tiles, home and you. Drawn instead of the flat map
    // while amount() > 0.
    void drawGlobe(const View& v, LLUIImage* home_image, LLUIImage* you_image);

private:
    void loadWater();
    void fetchExtentCoro();
    // Pixels per region at which the globe's radius on screen is this many times half the view's
    // shorter side.
    F32 pprForScreenRadius(F32 half_views) const;
    void drawWaterMesh(const View& v);
    void drawTiles(const View& v);
    // <WolfViewer 2026-10-03> Space behind the globe: stars, the Milky Way, nebulae.
    void makeSky();
    void drawSpace(const View& v, F32 alpha);
    void drawMarker(const View& v, const LLVector3d& global, LLUIImage* image);

    struct Keyed
    {
        LLPointer<LLViewerFetchedTexture> source;
        LLPointer<LLViewerTexture> texture;
        S32 raw_level = -1;      // discard level the copy was made from
        F64 first_seen = 0.0;
        F64 last_used = 0.0;
    };
    std::map<U64, Keyed> mKeyed;
    S32 mKeyedThisFrame = 0;
    U32 mKeyedFrame = 0;

    bool mHaveExtent = false;
    U32 mExtentGen = 0;
    F64 mX0 = 0.0, mY0 = 0.0, mX1 = 0.0, mY1 = 0.0;   // global metres
    F64 mRadius = 1.0;
    bool mFetching = false;
    F64 mFetchedAt = -1.0e9;   // last answer (or failure), LLTimer seconds
    S32 mViewWidth = 800, mViewHeight = 600;

    static constexpr S32 WATER_FRAMES = 16;
    LLPointer<LLViewerFetchedTexture> mWater[WATER_FRAMES];
    bool mWaterLoaded = false;

    // <WolfViewer 2026-10-03> The sky (makeSky): a direction on the unit sphere, a size in
    // pixels, a colour and an alpha per point; drawn as soft round dots (mStarDot).
    struct SkyPoint
    {
        F32 d[3];
        F32 size;
        F32 c[3];
        F32 a;
    };
    std::vector<SkyPoint> mSkyAdd;    // starlight: added
    std::vector<SkyPoint> mSkyOver;   // the dust lane: drawn over
    bool mSkyMade = false;
    LLPointer<LLViewerTexture> mStarDot;
};

#endif // WOLF_MAPGLOBE_H
