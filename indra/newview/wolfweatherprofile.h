/**
 * @file wolfweatherprofile.h
 * @brief WolfViewer: one weather setting, whole — the C++ side of the weather profile.
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

#ifndef WOLF_WEATHER_PROFILE_H
#define WOLF_WEATHER_PROFILE_H

#include <string>
#include <vector>

#include "v4color.h"
#include "llsd.h"

/**
 * The eleven numbers About Land > Weather sets, the grid stores (grid.weather) and the particle
 * systems and the weather sound read.
 *
 * Source: wolfstorm/js/world/weather_profile.js — the SAME field names, ranges and defaults.
 * The two viewers talk to one API and must not disagree about what "density 130" means, so this
 * is a deliberate mirror rather than a parallel invention. Change one, change the other.
 *
 * EVERY FIELD IS AN INTEGER. Percents, except mMoveSpeed which is hundredths (60 = 0.60 m/s of
 * lateral drift). Floats crossing C++, JSON, PHP and MySQL invite four different roundings.
 */
struct WolfWeatherProfile
{
    enum Kind { CLEAR = 0, RAIN = 1, SNOW = 2 };

    Kind        mKind        = CLEAR;
    /// 1..4 — the same digit a script writes as "wolfrain3" in a prim description.
    S32         mLevel       = 2;
    bool        mEnabled     = false;

    S32         mBrightness  = 75;      ///< 0..200 %, scales the precipitation's lit colour
    LLColor4    mTint        = LLColor4(0.667f, 0.800f, 1.f, 1.f);   ///< #aaccff
    S32         mTintAmount  = 100;     ///< 0..100 %, how far to the tint from the kind's neutral
    S32         mMoveSpeed   = 60;      ///< 0..300 hundredths of a m/s of lateral drift
    S32         mDensity     = 100;     ///< 10..300 % of the level's particle count
    S32         mVelocity    = 100;     ///< 10..300 % of the level's fall speed
    S32         mSize        = 100;     ///< 10..400 % of the particle's own width

    bool        mSound       = true;
    S32         mVolume      = 60;      ///< 0..100 %
    std::string mSoundPreset = "thunder";   ///< the ambience bed  (see WolfWeatherSound)
    std::string mSoundLayer  = "soft";      ///< the near layer

    // ── ranges, shared by the clamp and the XUI sliders ─────────────────────────────────────
    static constexpr S32 BRIGHTNESS_MIN = 0,  BRIGHTNESS_MAX = 200;
    static constexpr S32 TINT_MIN       = 0,  TINT_MAX       = 100;
    static constexpr S32 MOVE_MIN       = 0,  MOVE_MAX       = 300;
    static constexpr S32 DENSITY_MIN    = 10, DENSITY_MAX    = 300;
    static constexpr S32 VELOCITY_MIN   = 10, VELOCITY_MAX   = 300;
    // NOT SIZE_MIN / SIZE_MAX: <stdint.h> defines SIZE_MAX, so those names expand to a
    // numeric constant here and the declaration does not compile.
    static constexpr S32 PSIZE_MIN      = 10, PSIZE_MAX      = 400;
    static constexpr S32 VOLUME_MIN     = 0,  VOLUME_MAX     = 100;

    /** Force every field into range. Called after anything that came from outside this process. */
    void clampAll();

    /** Read a payload from php/weather.php (or a preset notecard). Missing fields keep the
        DEFAULTS, never zero — a profile written before a field existed must not arrive with
        brightness 0 and a black sky. */
    void fromLLSD(const LLSD& sd);
    /** The POST body / notecard form: exactly the JSON keys weather_profile.js uses. */
    LLSD toLLSD() const;

    /**
     * The colour the particles are actually drawn in: the kind's neutral colour moved
     * mTintAmount percent of the way to mTint, then scaled by mBrightness. The one place that
     * arithmetic lives, so the tab's swatch and the falling rain cannot drift apart.
     * Source: weather_profile.js drawColor().
     */
    LLColor4 drawColor() const;

    /** The neutral colour of a kind, before the tint moves it. rain #aaccff, snow #ffffff. */
    static LLColor4 neutralColor(Kind k);

    static const char* kindName(Kind k);
    static Kind        kindFromName(const std::string& s);
    /** "Heavy Rain" / "Heavy Snow" etc. — the ladder's labels, in the language of the kind. */
    static std::string levelLabel(Kind k, S32 level);
    /** The density hint under each ladder band: "Low density" ... "Maximum density". */
    static std::string levelDensity(S32 level);

    static std::string colorToHex(const LLColor4& c);
    static LLColor4    colorFromHex(const std::string& hex, const LLColor4& dflt);

    /** True when two profiles are the same in every field — what "unsaved" means in the panel. */
    bool operator==(const WolfWeatherProfile& o) const;
    bool operator!=(const WolfWeatherProfile& o) const { return !(*this == o); }

    /**
     * Read a prim description's weather keyword into this profile. [WEATHER 2026-09-12]
     *
     * The form a script writes with wolfSetParcelWeather, and the form a builder can type by
     * hand into a prim's description:
     *
     *   wolfrain3 bright=60 col=#8fb4e6 move=180 dens=170 amb=thunder near=heavy vol=85
     *   wolfsnow                       (no digit = level 2, moderate)
     *   wolfclear                      (no weather HERE, even when the region is raining)
     *
     * Unknown tokens are ignored rather than refusing the whole description: a description is
     * also a place people write ordinary words, and "wolfrain — my storm build" must still rain.
     * Out-of-range values are clamped for the same reason; a description is not an API call.
     *
     * @param lower the description, ALREADY lower-cased (WolfObjectProps::mDescriptionLower)
     * @return false when the description carries no weather keyword at all
     */
    bool fromDescription(const std::string& lower);

    /** A ready-made complete profile for the Apply Preset control. */
    struct Preset { const char* mId; const char* mLabel; };
    static const std::vector<Preset>& presets();
    /** The named preset, applied over `base`. Unknown id leaves `base` untouched. */
    static WolfWeatherProfile applyPreset(const WolfWeatherProfile& base, const std::string& id);

    /** The two Sound Presets dropdowns. id / label, in the order they are shown. */
    static const std::vector<Preset>& soundPresets();
    static const std::vector<Preset>& soundLayers();
};

#endif // WOLF_WEATHER_PROFILE_H
