/**
 * @file wolfhudscale.h
 * @brief WolfViewer: the vehicle dashboard and the flight deck drawn smaller, sized by dragging their top edge.
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

#ifndef WOLF_HUDSCALE_H
#define WOLF_HUDSCALE_H

#include <string>

#include "llcontrol.h"
#include "llfontgl.h"
#include "llrect.h"

class LLColor4;

// [HUD SIZE 2026-10-12] Paul: "the huds are all much too big ... the user should be able to drag the top of
// the huds (apart from gaming) down to reduce its size on the screen".
//
// The dashboard (wolfdashboard.cpp) and the flight deck (wolfflightdeck.cpp) lay themselves out exactly as
// they always have, in their own units, and a Scope draws that whole picture at a scale about the view's
// bottom-left corner: a point (x, y) of the picture lands at (x * scale, y * scale) of the view, so a click
// at (x, y) on the view is (x / scale, y / scale) in the picture. The picture is laid out W / scale wide,
// so drawn it still spans the window, its bottom stays on the toolbar and its middle stays in the middle.
// The Wolf Roleplay HUD (wolfgamehud.cpp) is not sized this way (Paul: "apart from gaming").
namespace WolfHudScale
{
    // Paul: "much too big". The full size is today's (wolfdashboard.cpp draw(): a 304-unit panel 30% of the
    // window, 170..380 px, and the strip of buttons above it; wolfflightdeck.cpp layoutAndDraw(): 34%, 200..430
    // px). By default both are drawn at that full size (Paul 10-12: "start them off big at the size they were")
    // and the driver can take them down to 0.4 (the smallest whose figures stay readable) and back to 1.0.
    const F32 MIN_SCALE = 0.4f;
    const F32 MAX_SCALE = 1.0f;
    const F32 DEFAULT_SCALE = 1.0f;  // settings.xml WolfDashboardScale / WolfFlightDeckScale

    /** Everything drawn while one is alive comes out at `scale` of its size (see above). */
    class Scope
    {
    public:
        explicit Scope(F32 scale);
        ~Scope();
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    private:
        F32 mPrev;
        bool mPushed;
    };

    /** The scale things are being drawn at now: 1 outside a Scope. */
    F32 current();

    /** Text at (x, y) of the picture. Inside a Scope the B612 lettering is drawn from the size of the face
     *  nearest to how big it really comes out, so it stays sharp; it takes exactly the room the face `f`
     *  takes in the picture, so it never spills out of a box laid out for `f`. */
    void text(const LLFontGL* f, const std::string& s, F32 x, F32 y, const LLColor4& c,
              LLFontGL::HAlign h, LLFontGL::VAlign v);

    /** A clip rectangle in the picture's coordinates, as the view's own (LLLocalClipRect wants the view's;
     *  the scissor test is in window pixels and does not see the Scope). */
    LLRect clip(const LLRect& r);
}

// [HUD INVENTORY 2026-10-12] Paul: "add an inventory button to every hud - only because i noticed that i often
// open the main toolbar to get a plane etc". Every HUD's INVENTORY button does what the toolbar's Inventory button
// does (commands.xml "inventory": Floater.Toggle "inventory", llui.cpp LLFloaterReg::toggleInstance), and like that
// button it is refused while RLV forbids the inventory (commands.xml is_enabled RLV.EnableIfNot "showinv").
namespace WolfHud
{
    void toggleInventory();
    bool inventoryOpen();
    const std::string& inventoryTip();
}

/**
 * The top edge of a HUD as its size handle: hover it for the resize cursor and a grip, drag it down to make
 * the HUD smaller and up to make it bigger, double-click it for the default. The size is a saved setting
 * (WolfDashboardScale, WolfFlightDeckScale in settings.xml). The HUD owns the mouse capture; this keeps
 * the arithmetic and draws the grip. Coordinates are the view's own (on screen, not the scaled picture).
 */
class WolfHudResize
{
public:
    explicit WolfHudResize(const char* setting);

    /** The saved size, MIN_SCALE..MAX_SCALE. */
    F32 scale() const;

    /** This frame the HUD is drawn from `bottom` to `top`, `width` wide. */
    void place(F32 bottom, F32 top, F32 width);
    /** This frame the HUD is not drawn at its full size (closed, hidden, or the mini bar): no handle. */
    void hide();

    /** (x, y) is on the handle: a band along the top edge, the whole width, and the tab in its middle. */
    bool onGrip(S32 x, S32 y) const;
    void hovered();                  // the mouse is over the handle this frame: show the grip
    void begin(S32 y);               // pressed on the handle
    void drag(S32 y);                // moved while pressed: the top edge follows the mouse
    void end();                      // let go, or the capture was lost
    bool dragging() const { return mDragging; }
    void reset();                    // double-click: the default size

    /** The handle: a tab always on the middle of the top edge (Paul 10-12: "make the drag bar obvious"),
     *  lit with a line along the whole edge while the mouse is on it or dragging it. Drawn outside the Scope. */
    void draw() const;

    static const std::string& tip();

private:
    const char* mSetting;
    LLCachedControl<F32> mScale;     // the saved size, read every frame (declared in settings.xml)
    bool mShown = false;
    F32 mBottom = 0.f, mTop = 0.f, mWidth = 0.f;
    F32 mPlacedScale = 1.f;          // the size the HUD was drawn at, from mBottom to mTop
    bool mDragging = false;
    F32 mGrabOffset = 0.f;           // the top edge's height above the mouse when the drag began
    U32 mHoverFrame = 0;
    bool mHoverSeen = false;
};

#endif // WOLF_HUDSCALE_H
