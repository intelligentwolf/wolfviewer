/**
 * @file wolftooladventureplace.h
 * @brief WolfViewer: the Adventure Studio's in-world place tool (STUDIO_AND_CONSOLE_V1.md §2.4).
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

#ifndef WOLF_TOOL_ADVENTURE_PLACE_H
#define WOLF_TOOL_ADVENTURE_PLACE_H

#include <string>
#include <vector>

#include "llhandle.h"
#include "llsingleton.h"
#include "lltool.h"
#include "lluuid.h"
#include "v3dmath.h"
#include "v3math.h"

class WolfAdventureStudioView;

/**
 * [WOLF ADVENTURE P3 2026-10-16] STUDIO_AND_CONSOLE_V1.md §2.4 "Places: putting it in the world", the
 * same click-and-drag pattern as the Cave Builder's pen (WolfToolCave, wolfcave.h:253):
 *   Spot     click the ground, a floor or an object; drag the ring to turn the facing; [ / ] turn it
 *            15 degrees, the arrow keys nudge it 0.5 m (Shift 0.1 m) (§6 2.1.1);
 *   Circle   click the centre then the edge, or drag from the centre outward;
 *   Polygon  click each corner; Enter or a click on the first point closes it; Backspace removes the last;
 *   Path     click each point (z is the surface clicked); Enter finishes.
 * Escape cancels.
 *
 * The tool is the TRANSIENT tool (LLToolMgr::setTransientTool) only while a shape is being placed: the
 * base tool stays LLToolPie, so llviewerwindow.cpp updateLayout keeps the Build floater shut (its
 * "suppress_toolbox" rule for an override over the pie tool), and every other click in the world is the
 * pie's as normal. The overlay (§2.4 "Overlay": spots as pins with labels, areas as outlines, paths as
 * dotted lines with arrows) is drawn while the Studio's Places page shows whether or not the tool is
 * current, through renderIdle / drawIdle (llviewerwindow.cpp, beside the current tool's render and draw).
 *
 * A place is named by its list and id (places/spots|areas|paths, id), never by a JSON pointer, so a list
 * reordered or edited while the tool is out still writes to the right place; a place that is gone ends
 * the tool with a message.
 */
class WolfToolAdventurePlace : public LLTool, public LLSingleton<WolfToolAdventurePlace>
{
    LLSINGLETON(WolfToolAdventurePlace);
public:
    enum EMode { M_NONE = -1, M_SPOT = 0, M_CIRCLE, M_POLYGON, M_PATH };

    /** Draw the open draft's places (and take clicks for a shape when one is begun). */
    void attach(LLHandle<WolfAdventureStudioView> view);
    void detach();
    bool attached() const { return !mView.isDead(); }
    /** Start a shape for the place `id` of places/<kind> on the region the agent stands on. */
    void begin(const std::string& kind, const std::string& id, EMode mode, const std::string& what);
    /** Stop placing (a nudge waiting to be written is written first) and hand the pointer back. */
    void cancel();
    EMode mode() const { return mMode; }
    std::string hint() const;
    /** Once a frame from the Studio: writes a nudge 250 ms after the last key; stops when the agent leaves the region. */
    void tick();

    /** The overlay while the Studio's Places page shows and the tool is not the current tool. */
    static void renderIdle();
    static void drawIdle();

    bool handleAnyMouseClick(S32 x, S32 y, MASK mask, EMouseClickType clicktype, bool down) override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleScrollWheel(S32 x, S32 y, S32 clicks) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;
    bool handleKey(KEY key, MASK mask) override;
    void handleDeselect() override;
    void onMouseCaptureLost() override;
    bool isAlwaysRendered() override { return true; }
    void render() override;
    void draw() override;

private:
    /** The surface under the pointer, region metres; `precise` picks objects too (clicks only - a pick
     *  renders the scene, so hovering uses the land alone). False off this region. */
    bool surface(S32 x, S32 y, LLVector3& out, bool precise) const;
    /** The pointer on the level plane at height z, region metres. */
    bool plane(S32 x, S32 y, F32 z, LLVector3& out) const;
    void finish();
    /** Nudge or turn the spot the tool is on (keys; §6 2.1.1), written after a pause (flushNudge). */
    void nudgeSpot(F32 dx, F32 dy, F32 dturn);
    void flushNudge();
    void say(const std::string& what) const;
    /** The overlay's segments and labels, made again when the draft or the region changes. */
    void rebuild();
    bool shown() const;

    LLHandle<WolfAdventureStudioView> mView;
    EMode mMode = M_NONE;
    std::string mKind, mId, mWhat;
    std::string mRegionAtBegin;     // the region the shape is being placed on
    std::vector<LLVector3> mPts;    // the shape being drawn, region metres
    bool mDragging = false;
    bool mHaveCursor = false;
    LLVector3 mCursor;
    F32 mFacing = 0.f;
    // a nudge not yet written (§6 2.1.1 keys, coalesced: one write ~250 ms after the last key)
    bool mPending = false;
    LLVector3 mPendPos;
    F32 mPendFacing = 0.f;
    F64 mPendAt = 0.0;
    bool mPendTurned = false;
    bool mTouched = false;          // the shape wrote something (a spot ended with Escape says "Done.", not "Cancelled")
    // the overlay, cached per draft generation and region
    struct Group { LLVector3 mCentre; std::vector<LLVector3> mSegs; S32 mColour = 0; };
    struct Label { LLVector3 mPos; std::string mText; };
    std::vector<Group> mGroups;
    std::vector<Label> mLabels;
    U32 mCacheGen = 0;
    bool mCacheValid = false;
    LLUUID mCacheRegion;
    LLVector3d mCacheOrigin;   // agent origin the cached agent-space outlines were built for
};

#endif // WOLF_TOOL_ADVENTURE_PLACE_H
