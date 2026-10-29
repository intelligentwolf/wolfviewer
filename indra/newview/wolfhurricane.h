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
    F64  mEye        = 0.0;     ///< Re, m (= 0.1 R)
    S32  mHemisphere = 1;       ///< 1 north (anticlockwise), -1 south (clockwise)
    bool mRain       = true;

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
    };
    Point geometry(F64 px, F64 py, F64 cx, F64 cy, const Time& T) const;

    /** README "Rain at a point": q (0..1), 0 when rain is off. */
    F64 rainQ(const Point& p, const Time& T) const;
    /** README "Rain at a point": the level from q - 4 torrential, 3, 2, 1, 0 none. */
    static S32 rainLevel(F64 q);
    /** w = C env(r): the storm's share at the point (README "Clouds"; "Rain at a point" replaces the region's at w >= 0.5). */
    static F64 share(const Point& p, const Time& T) { return T.C * p.env; }

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
    static constexpr F64 EYE_PER_R = 0.1;               ///< Re = 0.1 R
};

#endif // WOLF_HURRICANE_H
