/**
 * @file wolfhudkit.h
 * @brief WolfViewer: the Wolf HUDs' shared parts kit - Paul's "Gunmetal Console" look drawn from one picture sheet.
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

#ifndef WOLF_HUDKIT_H
#define WOLF_HUDKIT_H

#include "v4color.h"   // LLColor4 (as wolfhudscale.cpp includes it)
#include "wolfhudkitmap.h"

// [HUD KIT 2026-11-25] Paul chose look B, "Gunmetal Console", for every Wolf HUD (review page 25 Nov): machined anodised
// gunmetal, carbon, backlit keys in wells, chrome and brass bezels, instruments behind glass. Every part that does not move
// (plates, key caps in their normal, lit and pressed states, bezels, gauge faces, glass glare, glow) was rendered once in
// Blender (hud-kit/parts.py: Eevee, orthographic, transparent film) and packed into one sheet,
// skins/default/textures/wolfhud/hudkit_gunmetal.png ("WolfHudKit_Gunmetal" in textures.xml), mapped by wolfhudkitmap.h
// (generated with the sheet; WolfStorm reads the same map as JSON). The HUD draws the parts as textured quads, the
// stretchable ones 9-sliced, under its own live needles, words and lamps.
//
// Coordinates are the drawing view's own, y up, as the HUDs' other primitives (gGL.vertex2f), so a WolfHudScale::Scope
// scales them with everything else.
namespace WolfHudKit
{
    using WolfHudKitMap::EPart;

    /** The sheet is loaded and drawable. Until it is (the first frames), a HUD draws its old flat look. */
    bool ready();

    /** One part across (l, b) - (r, t). A part with 9-slice borders keeps its corners square and stretches its edges and middle;
     *  its borders are drawn `k` screen units per sheet pixel (k <= 0: in proportion to the part, min of width and height
     *  ratios), never more than half the rectangle. A part without borders is stretched to fill. `tint` multiplies it
     *  (a glow sprite is white: tint it). Returns false (draws nothing) while the sheet is not ready. */
    bool part(EPart p, F32 l, F32 b, F32 r, F32 t, const LLColor4& tint = LLColor4::white, F32 k = -1.f);

    /** A physical key in its well: normal, lit (backlit, with the lamp) or held down. */
    enum ECap { CAP_UP, CAP_LIT, CAP_DOWN };
    bool key(F32 l, F32 b, F32 r, F32 t, ECap state, F32 k = -1.f);

    /** A soft glow under something lit, centred at (cx, cy), `rad` from its middle to where it fades out. */
    bool glow(F32 cx, F32 cy, F32 rad, const LLColor4& c);

    /** A round instrument: its face (black or enamel) inside radius r, then - drawn after the needle - its bezel (chrome or
     *  brass) and the glass's glare. */
    bool gaugeFace(F32 cx, F32 cy, F32 r, bool enamel);
    bool gaugeBezel(F32 cx, F32 cy, F32 r, bool brass);
}

#endif // WOLF_HUDKIT_H
