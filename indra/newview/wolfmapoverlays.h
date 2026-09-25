/**
 * @file wolfmapoverlays.h
 * @brief WolfViewer: images region owners lay over the World Map (Wolf Territories only).
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

#ifndef WOLF_MAPOVERLAYS_H
#define WOLF_MAPOVERLAYS_H

#include <set>
#include <string>
#include <vector>

#include "llui.h"                 // EDragAndDropType, EAcceptance (llui.h:64, :94)
#include "llpointer.h"
#include "llsingleton.h"
#include "llrect.h"
#include "lluuid.h"
#include "v3dmath.h"

class LLViewerFetchedTexture;
class LLWorldMapView;

// [MAP OVERLAYS 2026-09-25] Source: wolfstorm/js/world/map_overlays.js — the same API, the same
// record and the same editing rules; keep them in step.
//
// Paul: "allow region owners to upload images and drop them on the map like an overlay — on
// our Ireland region I could make an image called 'Dublin' and put it over Dublin, just drag
// it and stretch from my inventory ... only for Wolf Territories grid and I can only do it on
// a region I'm the owner of."
//
// SERVICE: https://wolfstorm.app/php/map_overlays.php (table robust.map_overlays). Reads are a
// public GET by a global-metre box; writes are POSTs carrying X-Wolf-Agent / X-Wolf-Session,
// which the service checks against the grid's presence service and then allows only on a
// region the caller holds (owner, estate owner or manager, god). The image is named by an
// INVENTORY ITEM — the service reads the texture and the name out of the caller's own
// inventory and requires copy + transfer rights, because everyone on the grid sees it.
//
// UI: the images are ALWAYS drawn on the grid (Paul 09-25: "make sure everyone can always see
// map textures"). A region OWNER (the API's "owner" action) gets an "Edit my map images" button
// drawn on the map itself, top left (drawControls) — not in the floater's legend, where Paul
// could not find it. Drop a texture from inventory onto a region you own to add one; while
// editing (WolfMapEditImages) drag an image to move it, a corner to stretch it (Shift keeps its
// shape), Delete removes it.
// LLWorldMapView forwards drawing, the mouse, drag-and-drop and keys to this class.
class WolfMapOverlays : public LLSingleton<WolfMapOverlays>
{
    LLSINGLETON(WolfMapOverlays);
    ~WolfMapOverlays();

public:
    static const char* API_URL;

    struct Overlay
    {
        S32 mId = 0;
        LLUUID mRegion;
        LLUUID mTexture;
        std::string mName;
        std::string mRegionName;
        F64 mX = 0, mY = 0, mW = 0, mH = 0;             // global metres, SW corner + size
        F64 mRegionX = 0, mRegionY = 0, mRegionW = 256, mRegionH = 256;
        S32 mVersion = 0;
        LLPointer<LLViewerFetchedTexture> mImage;
    };

    /** On Wolf Territories (the images are always shown there). */
    bool active() const;

    // ── called by LLWorldMapView ──
    /** Draw the images (inside the map's tilt projection, after the tiles). */
    void draw(LLWorldMapView& view);
    /** Draw the owner's "Edit my map images" button and hint (flat, after the tilt is popped). */
    void drawControls(LLWorldMapView& view);
    /** A click on the edit button toggles editing, on "Delete image" asks to delete the selected
     *  image; true when (x, y) was on either. */
    bool clickControls(S32 x, S32 y);
    /** True when the press started a move / stretch (the view must not pan). */
    bool mouseDown(LLWorldMapView& view, S32 x, S32 y, MASK mask);
    bool isDragging() const { return mOp.mKind != Op::NONE; }
    void mouseMove(LLWorldMapView& view, S32 x, S32 y, MASK mask);
    void mouseUp();
    /** The cursor to show when hovering over (x, y) without a button down; false = not ours. */
    bool hoverCursor(LLWorldMapView& view, S32 x, S32 y);
    /** The button or an editable image lies under (x, y) — a double-click there is not a teleport. */
    bool hitEditable(LLWorldMapView& view, S32 x, S32 y);
    /** The name of the image under (x, y), for the map's tooltip; empty when none. */
    std::string toolTipAt(LLWorldMapView& view, S32 x, S32 y);
    bool dragAndDrop(LLWorldMapView& view, S32 x, S32 y, bool drop, EDragAndDropType cargo_type,
                     void* cargo_data, EAcceptance* accept, std::string& tooltip_msg);
    bool handleKey(KEY key, MASK mask);

private:
    struct Op
    {
        enum Kind { NONE, MOVE, RESIZE } mKind = NONE;
        S32 mId = 0;
        F64 mOrigX = 0, mOrigY = 0, mOrigW = 0, mOrigH = 0;
        LLVector3d mGrab;           // global point the press landed on
        F64 mAnchorX = 0, mAnchorY = 0;
        bool mMoved = false;
    };

    Overlay* find(S32 id);
    bool canEdit(const Overlay& o) const { return mEditable.count(o.mRegion) != 0; }
    bool editMode() const;
    /** Image rectangle corners in view pixels (before the tilt). */
    void viewRect(LLWorldMapView& view, const Overlay& o, F32& left, F32& bottom, F32& right, F32& top);
    /** Index of the topmost editable image containing the global point, or -1. */
    S32 hitOverlay(const LLVector3d& g) const;
    /** Which corner of the selected image the view point is on: 0 none, 1 SW, 2 SE, 3 NW, 4 NE. */
    S32 hitCorner(LLWorldMapView& view, S32 x, S32 y);
    void drawEditFrame(LLWorldMapView& view, const Overlay& o, F32 left, F32 bottom, F32 right, F32 top);

    void ensureFetched(LLWorldMapView& view);
    void fetchCoro(F64 x0, F64 y0, F64 x1, F64 y1);
    void askRights();
    void askOwner();
    void ownerCoro();
    void rightsCoro(std::vector<LLUUID> regions);
    void createCoro(LLUUID item, std::string item_name, F64 x, F64 y, F64 w, F64 h);
    void updateCoro(S32 id, S32 version, F64 x, F64 y, F64 w, F64 h, F64 ox, F64 oy, F64 ow, F64 oh);
    void deleteCoro(S32 id, std::string name);
    void confirmDelete();
    /** POST a JSON body; returns the parsed reply (or an empty map) and the HTTP status. */
    LLSD post(const LLSD& body, S32& http_status);
    static bool parseOverlay(const LLSD& in, Overlay& out);
    void storeOverlay(const Overlay& o);

    std::vector<Overlay> mOverlays;     // in id order (= draw order, newest on top)
    std::set<LLUUID> mEditable;
    std::set<LLUUID> mAskedRights;
    std::set<S32> mSaving;
    S32 mSelectedId = 0;
    Op mOp;

    bool mFetching = false;
    bool mHaveBox = false;
    F64 mBoxX0 = 0, mBoxY0 = 0, mBoxX1 = 0, mBoxY1 = 0;
    F64 mFetchedAt = -1.0e9;
    bool mLastEditMode = false;
    /** Bumped by every write, so a read that started before it is not applied over it. */
    U32 mWriteGen = 0;
    /** Does the agent own any region? -1 not known yet, 0 no, 1 yes. */
    S32 mOwns = -1;
    bool mAskingOwner = false;
    F64 mOwnerRetryAt = 0.0;
    LLRect mButtonRect;             // where drawControls put the button (view pixels); empty = none
    LLRect mDeleteRect;             // "Delete image" button, only while an editable image is selected
};

#endif // WOLF_MAPOVERLAYS_H
