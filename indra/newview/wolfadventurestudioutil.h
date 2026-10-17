/**
 * @file wolfadventurestudioutil.h
 * @brief WolfViewer: the Adventure Studio's shared helpers (made in wolffloateradventurestudioedit.cpp).
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

#ifndef WOLF_ADVENTURE_STUDIO_UTIL_H
#define WOLF_ADVENTURE_STUDIO_UTIL_H

#include <string>
#include <boost/json.hpp>

#include "llsd.h"
#include "v4color.h"

/**
 * [WOLF ADVENTURE AUDIT 2026-10-17] The editors' own helpers, for the "Start here" wizard
 * (wolffloateradventurestudiowizard.cpp) and the Guide, so they are written once (in
 * wolffloateradventurestudioedit.cpp) and not copied.
 */
namespace WolfAdvStudio
{
    /** The width a drawn button needs for its label (WolfGameDraw::button's face and padding), at least `min_w`. */
    F32 btnW(const std::string& label, F32 min_w = 0.f);
    /** The value at a JSON pointer (RFC 6901); with `create`, missing object members along the way are made. */
    boost::json::value* jAt(boost::json::value& root, const std::string& ptr, bool create);
    /** The LLSD value at a JSON pointer, or undefined. */
    const LLSD& sdAt(const LLSD& root, const std::string& ptr);
    /** An id from `base` that no "id" in the pack's JSON text uses yet. */
    std::string uniqueId(const std::string& base, const std::string& pack_text);
    /** "#rrggbb" as a colour; false when it is not one (adventure_pack_v1.schema.json $defs/colour). */
    bool parseColour(const std::string& s, LLColor4& out);
    /** The WCAG 2.2 contrast ratio of two colours, 1 to 21 (relative luminance, WCAG 2.2 "relative luminance" and
     *  "contrast ratio" definitions). */
    F32 contrast(const LLColor4& a, const LLColor4& b);
    /** Black or white, whichever reads better on `bg` (always at least 4.5:1, the lowest best case being about 4.58). */
    LLColor4 inkOn(const LLColor4& bg);
    /** [M4 2026-10-17] The pack schema this viewer carries has a top-level list `key` (enemies, battles arrived with M4). */
    bool packHas(const std::string& key);
    /** [FRONT DOOR 2026-10-17] the helper's first-run line (the coordinator's words; WolfStorm says the same). */
    inline const char* greeting() { return "Tell me your story or an idea, and we'll build it together. You can walk your land with me to set each scene."; }
}

#endif // WOLF_ADVENTURE_STUDIO_UTIL_H
