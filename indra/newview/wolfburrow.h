/**
 * @file wolfburrow.h
 * @brief WolfViewer: "the burrow" - the painted look of the Tunnel tool, the Cave Builder and the Build window's Wolf strip.
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

#ifndef WOLF_BURROW_H
#define WOLF_BURROW_H

#include <functional>
#include <string>
#include <vector>

#include "llview.h"
#include "v4color.h"

class LLButton;
class LLFloater;

// [WOLF BURROW 2026-10-23] Paul: "upgrade the whole tunnel floater please i LOVE the mole in fact you know we could make the
// build floaters all look way nicer ... the plain ones are boring". The look of the dig plate (wolfdigprogress.cpp) for whole
// windows: warm earth paper, a painted header with a mole in its burrow, every group of controls on its own dark plate under an
// amber heading, and painted button faces with a clear weight (primary amber, secondary stone, danger with a red rule).
// Painted in code as the uploaders are (wolffloatermeshupload.cpp WolfMeshUploadSheet, llfloaterwolfavatars.cpp
// WolfAvatarsBackdrop, the wolfpaint.h primitives): the sheet is a view BEHIND every control, the controls stay the XUI widgets
// they were (names, keyboard, focus, tooltips, callbacks), their buttons draw no skin image (image_color 0 alpha in the XML) so
// the sheet's faces show, and every text colour is fixed (the text sits on the fixed paint, not the skin's background). Every
// text pair's contrast: region-terrain-work/cave-lights/check/burrow_contrast.py (all at least 4.5:1).
namespace WolfBurrow
{
    // the palette (burrow_contrast.py has every text pair)
    extern const LLColor4 CREAM, MUTED, DIS, AMBER, AMBER_DEEP, INK, ERR_INK;
    extern const LLColor4 SHEET_TOP, SHEET_BOTTOM, PLATE_TOP, PLATE_BOTTOM, EDGE;

    enum EWeight { WEIGHT_PRIMARY, WEIGHT_SECONDARY, WEIGHT_DANGER };

    /** The soil cut-away with the mole digging (the dig plate's scene). `t` the animation time (0 = still), `lit` a flame on its
     *  head. */
    void drawMoleScene(const LLRect& pit, F32 t, bool lit);
    /** The whole window's paper below its title bar. */
    void drawSheet(const LLRect& client);
    /** The header band: the mole in its burrow on the left, the title in the display face and a line under it. */
    void drawHeader(const LLRect& band, const std::string& title, const std::string& line, F32 t);
    /** A group's plate with its heading (amber, spaced) along the top; an empty heading draws the plate only. */
    void drawSection(const LLRect& plate, const std::string& heading);
    /** The face under a button (in `owner`'s coordinates): its weight, hover, pressed, a toggle's "on", disabled. */
    void drawButtonFace(LLButton* b, const LLView* owner, EWeight weight);
    /** The plate round some controls, in `owner`'s coordinates: their visible rectangles together, grown by `pad` and with
     *  `heading_room` above for the heading. Empty when none is visible. */
    LLRect around(const std::vector<LLView*>& views, const LLView* owner, S32 pad, S32 heading_room);

    // [WOLF BURROW 2026-10-23] The Build floater's look in the SKIN's own named colours (colors.xml WolfBuild*, every skin and
    // theme worked out and contrast-checked by region-terrain-work/cave-lights/check/build_colors.py), so its skin-coloured text
    // keeps its contrast in light and dark skins alike.
    /** A group's plate: WolfBuildPlate with a WolfBuildPlateEdge rule. */
    void drawBuildPlate(const LLRect& plate);
    /** The header band behind the tool row: WolfBuildHeaderTop to WolfBuildHeaderBottom, a WolfBuildAccent line along its foot. */
    void drawBuildHeader(const LLRect& band);

    /** The view behind a window's controls that runs `painter` (floater coordinates). Not mouse-opaque, so the window still
     *  drags by its paper. Not registered as a widget (made in code), so it adds no widget Params to the factory. */
    class Sheet : public LLView
    {
    public:
        /** Behind every control of `owner`. The window must then send its own drag handle behind the sheet
         *  (sendChildToBack(getDragHandle()), protected in LLFloater), as the uploaders do. */
        static Sheet* add(LLFloater* owner, const std::function<void()>& painter);
        void draw() override;
    private:
        struct Params : public LLInitParam::Block<Params, LLView::Params> { Params() {} };
        Sheet(const Params& p) : LLView(p) {}
        std::function<void()> mPainter;
    };
}

#endif // WOLF_BURROW_H
