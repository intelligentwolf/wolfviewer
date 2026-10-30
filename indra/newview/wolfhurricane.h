/**
 * @file wolfhurricane.h
 * @brief WolfViewer: a scripted hurricane (weather.hurricane, ticket #367) - the storm document and the model.
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

#ifndef WOLF_HURRICANE_H
#define WOLF_HURRICANE_H

#include "stdtypes.h"

#include <string>

class LLSD;

/**
 * [HURRICANE 2026-10-29] Jimmy Olsen, ticket #367 ("Is it possible to hurricanes in W.E?"); Paul: "create a new hurricane lsl
 * command ... it will form properly from the clouds".
 *
 * THE MODEL is a contract shared by every reader - the web (php/hurricane_lib.php), the region plugin (wolf-hurricane),
 * WolfStorm and this viewer - so everyone on the region sees the same storm:
 * Source: region-terrain-work/hurricane-2026-10-29/README.md "THE MODEL". Every formula and constant below is that
 * section's; nothing here is tuned in the viewer. This is the ONE place in the viewer that holds them on the CPU (the cloud
 * shader's copy is cloudsCommon.glsl wolfStormApply, fed by wolfenvironmentrender.cpp cloudsFrame from timeAt / centre).
 *
 * The document arrives as `hurricane` in the php/weather.php region payload while a storm is running and is absent
 * otherwise (WolfRegionWeather::readHurricane). Times are unix seconds UTC, positions region metres of the region the
 * payload is for.
 */
class WolfHurricane
{
public:
    // ---- the storm document (README "The storm document"), every number clamped to the table's range on reading ----
    std::string mId;            ///< 16 lowercase hex
    S32  mCategory   = 1;       ///< Saffir-Simpson 1..5
    F64  mVmax       = 0.0;     ///< m/s, the category's maximum sustained wind
    F64  mStart      = 0.0;     ///< unix seconds UTC: formation begins
    F64  mForm       = 0.0;     ///< s, 120..3600
    F64  mMature     = 0.0;     ///< s, 60..14400
    F64  mDecay      = 0.0;     ///< s, 120..1800
    F64  mPass       = 0.0;     ///< s from mStart: the eye is at (mX0, mY0)
    F64  mX0 = 0.0, mY0 = 0.0;  ///< region metres
    F64  mHeading    = 0.0;     ///< degrees 0..360, compass, the direction moved TOWARDS (0 = north = +Y, 90 = east = +X)
    F64  mSpeed      = 0.0;     ///< m/s 0..30
    F64  mRadius     = 0.0;     ///< R, m 1000..60000
    F64  mEye        = 0.0;     ///< Re, m (the block's eye; 0.15 R for storms made from 2026-10-30)
    S32  mHemisphere = 1;       ///< 1 north (anticlockwise), -1 south (clockwise)
    bool mRain       = true;
    S32  mSx = 0, mSy = 0;      ///< the region's size, m (robust.regions sizeX / sizeY); 0 = not in the payload
    F64  mAmbientMs = 0.0;      ///< the region's own wind when the storm was made, m/s ..
    F64  mAmbientFrom = 0.0;    ///< .. and the direction it blows FROM, compass degrees

    /** Reads the `hurricane` object tolerantly: every number clamped to the README table's range; anything missing or
     *  malformed (not a map, a non-numeric field, a bad id) = no storm (returns false and leaves `out` alone). */
    static bool fromLLSD(const LLSD& sd, WolfHurricane& out);

    enum Stage { NONE = 0, FORMING, MATURE, DECAYING };

    /** README "Time": t = now - start; O organisation, E eye clear, W intensity, C gathering, A seconds of full-strength
     *  motion. Everything 0 when the stage is NONE (before start, or ended). */
    struct Time
    {
        Stage stage = NONE;
        F64 O = 0.0, E = 0.0, W = 0.0, C = 0.0, A = 0.0;
        bool active() const { return stage != NONE; }
    };
    Time timeAt(F64 now_unix) const;

    /** c(t): the eye's position (region metres) at unix time `now_unix`. */
    void centre(F64 now_unix, F64& cx, F64& cy) const;

    /** D = 0.622 vmax sin(10 deg) A: metres the cloud field has streamed inwards (README "Clouds"). */
    F64 streamed(const Time& T) const;

    /** README "Geometry at a point p": every term at the point (px, py) (region metres) given the eye (cx, cy). */
    struct Point
    {
        F64 r = 0.0, phi = 0.0;     ///< distance from the eye, angle anticlockwise from +X (east), radians
        F64 env = 0.0;              ///< 1 inside R, 0 beyond 1.3 R
        F64 psi = 0.0;              ///< the bands' wind-up
        F64 B2 = 0.0;               ///< the two principal bands
        F64 inner = 0.0;            ///< the dense overcast
        F64 ew = 0.0;               ///< the eyewall ring
        F64 Meye = 1.0;             ///< 0 in the eye, 1 beyond 1.2 Re
        F64 Mmoat = 1.0;            ///< [2026-10-30] README "Clouds": 0 in the moats between the bands outside the dense overcast
    };
    Point geometry(F64 px, F64 py, F64 cx, F64 cy, const Time& T) const;

    /** README "Rain at a point": q (0..1), 0 when rain is off. */
    F64 rainQ(const Point& p, const Time& T) const;
    /** README "Rain at a point": the level from q - 4 torrential, 3, 2, 1, 0 none. */
    static S32 rainLevel(F64 q);
    /** [HURRICANE 2026-10-30] README "Rain at a point" / php hurricane_lib.php:51 HU_RAIN_MMH: the rain rate (mm/h) at q = 1 for a
     *  category (Lonfat, Marks & Chen, AMS 2000 15A.1: > 15 mm/h near the centre for Cat 3+, ~3 mm/h for tropical storms) -
     *  1: 7, 2: 11, 3-5: 15. The rain curtains' extinction comes from it (cloudsCommon.glsl wolfStormApply). */
    static F64 rainRateMmh(S32 category) { return category >= 3 ? 15.0 : (category == 2 ? 11.0 : 7.0); }
    /** w = C env(r): the storm's share at the point (README "Clouds"; "Rain at a point" replaces the region's at w >= 0.5). */
    static F64 share(const Point& p, const Time& T) { return T.C * p.env; }

    /** README "Wind": the wind (m/s, +x east, +y north, the way the air moves) at the point (px, py) given the eye (cx, cy):
     *  the Holland profile with the inflow angle and the track term, mixed with the ambient wind by k = W env(r), |v| <= 75. */
    void windAt(F64 px, F64 py, F64 cx, F64 cy, const Time& T, F64& wx, F64& wy) const;
    /** README "The sea" S_storm(p): the region's wind -> sea rule on U_sea (the storm wind; inside Rm the eyewall wind at the
     *  same bearing; at r < 1 m the bearing heading + 90 h degrees). */
    F64 seaIndexAt(F64 px, F64 py, F64 cx, F64 cy, const Time& T) const;
    /** README "The sea" U_sea's wind vector (m/s, the way the air moves) at p: the wind at p' (p, or inside Rm the eyewall
     *  point at the same bearing; at r < 1 m the bearing heading + 90 h degrees). seaIndexAt is seaIndexOfWind(|this|). */
    void seaWind(F64 px, F64 py, F64 cx, F64 cy, const Time& T, F64& wx, F64& wy) const;
    /** [ROUND 10] README "The storm swell": the storm swell train's amplitude (m) at p, before the water's own exposure / edge /
     *  distance fades: A_MAX W g(r), g = (V_H(max(r, Rm)) / V_5)^2 inside R, (V_H(R) / V_5)^2 sqrt(R / r) beyond. 0 when no
     *  storm is drawn. It runs out from the eye in rings (direction (p - c) / r). */
    F64 stormSwellAmp(F64 px, F64 py, F64 cx, F64 cy, const Time& T) const;
    /** [ROUND 10] README "The sea" (viewers, per point): S_local = clamp(max(S_storm, S_eep), 0.15, 3); the drawn amplitude there
     *  is x (S_local / S_eep)^1.5 - this returns that factor (1 wherever the storm's sea is below the region's own). */
    static F64 seaAmplitudeFactor(F64 s_storm, F64 s_eep);
    /** WolfAutoEnvironmentModule.cs ApplyWind (README "The sea"): index = clamp((wind - 3) / 3, 0.15, 3). */
    static F64 seaIndexOfWind(F64 ms);

    // README "Time" s(x) and S(x) = its integral from 0 to x
    static F64 smooth(F64 x);
    static F64 smoothIntegral(F64 x);

    // ---- the model's constants (README "Sources and scaling" and "Geometry"); the shader's copies cite these ----
    /// cot 10 deg: the bands' log spiral (Dvorak's crossing angle). README: "cot alpha = 5.6713" (rounded); the full value
    /// as php/hurricane_lib.php HU_COT_ALPHA, so the web and the viewer wind the bands identically
    static constexpr F64 COT_ALPHA = 5.671281819617709;
    static constexpr F64 CLOUD_SPEED_SHARE = 0.622;     ///< cloud features move at 0.622 vmax
    static constexpr F64 SIN_ALPHA = 0.17364817766693;  ///< sin 10 deg (D = 0.622 vmax sin(10 deg) A)
    static constexpr F64 RM_PER_RE = 1.25;              ///< Rm = 1.25 Re
    static constexpr F64 RCDO_PER_R = 0.35;             ///< Rcdo = 0.35 R
    static constexpr F64 EYE_PER_R = 0.15;              ///< Re = 0.15 R when the block gives none ([2026-10-30], was 0.1)
    static constexpr F64 EYE_MIN_PER_R = 0.05, EYE_MAX_PER_R = 0.3;   ///< the range of the block's own eye (php HU_EYE_MIN / MAX_SHARE)
    static constexpr F64 HOLLAND_B = 1.31;              ///< README Wind: B (Willoughby & Rahn 2004)
    static constexpr F64 INFLOW_DEG = 22.6;             ///< README Wind: the surface inflow angle (Zhang & Uhlhorn 2012)
    static constexpr F64 WIND_CAP = 75.0;               ///< README Wind: |v| capped at 75 m/s
    static constexpr F64 SEA_MIN_INDEX = 0.15;          ///< wolfseastate.h MIN_INDEX
    static constexpr F64 SEA_MAX_INDEX = 3.0;           ///< wolfseastate.h MAX_INDEX
    static constexpr F64 STORM_SWELL_LENGTH = 300.0;    ///< README L_STORM, m (Wright et al. 2001, near Bonnie's eye)
    /// [ROUND 10] README "The storm swell": its amplitude at a category 5's eyewall, m - 2 a = 28 m crest to trough (Hurricane Ivan's
    /// largest measured wave 27.7 m, Wang et al. 2005, Science 309:896); k a = 0.29 at L 300 m, under the 0.3 steepness guard
    static constexpr F64 STORM_SWELL_A_MAX = 14.0;
};

#endif // WOLF_HURRICANE_H
