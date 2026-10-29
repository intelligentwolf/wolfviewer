/**
 * @file wolfhurricane.cpp
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

#include "llviewerprecompiledheaders.h"

#include "wolfhurricane.h"

#include "llsd.h"

#include <algorithm>
#include <cmath>

namespace
{
    // Source: README "Sources and scaling" Categories (NHC https://www.nhc.noaa.gov/aboutsshws.php): vmax = the middle of the
    // band in kt (5: the band's lower bound), 1 kt = 1852 / 3600 m/s -> 37.55, 45.79, 53.50, 64.05, 70.48 m/s. The same
    // table as php/hurricane_lib.php HU_VMAX_KT / HU_KT.
    const F64 VMAX_KT[5] = { 73.0, 89.0, 104.0, 124.5, 137.0 };
    const F64 KT = 1852.0 / 3600.0;
    // Source: README "The storm document" table: the ranges every reader clamps to
    const F64 FORM_MIN = 120.0, FORM_MAX = 3600.0;
    const F64 MATURE_MIN = 60.0, MATURE_MAX = 14400.0;
    const F64 DECAY_MIN = 120.0, DECAY_MAX = 1800.0;
    const F64 SPEED_MAX = 30.0;
    const F64 RADIUS_MIN = 1000.0, RADIUS_MAX = 60000.0;

    bool readNumber(const LLSD& sd, const char* key, F64& out)
    {
        if (!sd.has(key)) return false;
        const LLSD& v = sd[key];
        if (!v.isReal() && !v.isInteger()) return false;
        const F64 x = v.asReal();
        if (!std::isfinite(x)) return false;
        out = x;
        return true;
    }

    bool isLowerHex16(const std::string& s)
    {
        if (s.size() != 16) return false;
        for (char ch : s)
        {
            if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
        }
        return true;
    }

    F64 clampd(F64 x, F64 lo, F64 hi) { return std::min(std::max(x, lo), hi); }
}

// static  Source: README "Time": s(x) = smoothstep, c = clamp(x, 0, 1); c c (3 - 2 c)
F64 WolfHurricane::smooth(F64 x)
{
    const F64 c = clampd(x, 0.0, 1.0);
    return c * c * (3.0 - 2.0 * c);
}

// static  Source: README "Time": S(x) = integral of s from 0 to x (x in 0..1) = x^3 - x^4 / 2
F64 WolfHurricane::smoothIntegral(F64 x)
{
    const F64 c = clampd(x, 0.0, 1.0);
    return c * c * c - 0.5 * c * c * c * c;
}

// static  Source: README "The storm document" (the keys, types and ranges); php/hurricane_lib.php hu_spec_valid is the writer's
// check (id 16 lowercase hex, hemisphere exactly 1 or -1, rain = absent or true). Here every number is clamped to its range
// rather than rejected (the brief: "read it tolerantly"); a missing or non-numeric field or a bad id is no storm.
bool WolfHurricane::fromLLSD(const LLSD& sd, WolfHurricane& out)
{
    if (!sd.isMap()) return false;
    WolfHurricane h;
    if (!sd.has("id") || !sd["id"].isString()) return false;
    h.mId = sd["id"].asString();
    if (!isLowerHex16(h.mId)) return false;

    F64 cat = 0.0, start = 0.0, form = 0.0, mature = 0.0, decay = 0.0, pass = 0.0;
    F64 x0 = 0.0, y0 = 0.0, heading = 0.0, speed = 0.0, radius = 0.0, hemi = 0.0;
    if (!readNumber(sd, "category", cat) || !readNumber(sd, "start", start) || !readNumber(sd, "form", form)
        || !readNumber(sd, "mature", mature) || !readNumber(sd, "decay", decay) || !readNumber(sd, "pass", pass)
        || !readNumber(sd, "x0", x0) || !readNumber(sd, "y0", y0) || !readNumber(sd, "heading", heading)
        || !readNumber(sd, "speed", speed) || !readNumber(sd, "radius", radius) || !readNumber(sd, "hemisphere", hemi))
    {
        return false;
    }
    if (start < 0.0) return false;
    // hemisphere is a sign, 1 or -1 (README table); anything else is not a storm document
    if (hemi != 1.0 && hemi != -1.0) return false;

    h.mCategory = (S32)clampd(std::floor(cat + 0.5), 1.0, 5.0);
    h.mStart = start;
    h.mForm = clampd(form, FORM_MIN, FORM_MAX);
    h.mMature = clampd(mature, MATURE_MIN, MATURE_MAX);
    h.mDecay = clampd(decay, DECAY_MIN, DECAY_MAX);
    h.mPass = clampd(pass, 0.0, h.mForm + h.mMature + h.mDecay);   // hu_spec_valid: 0 <= pass <= form + mature + decay
    h.mX0 = x0;
    h.mY0 = y0;
    h.mHeading = clampd(heading, 0.0, 360.0);
    h.mSpeed = clampd(speed, 0.0, SPEED_MAX);
    h.mRadius = clampd(radius, RADIUS_MIN, RADIUS_MAX);
    // vmax: the exact table value from the category (README Categories; the payload's `vmax` is rounded to 2 decimals for
    // show and is not used - the contract clarification of 2026-10-29, as the server and the plugin compute it)
    h.mVmax = VMAX_KT[h.mCategory - 1] * KT;
    // eye: Re = 0.1 R (README table); hu_spec_build writes round(R x 0.1, 1), so the payload's value is held to that rounding
    F64 eye = EYE_PER_R * h.mRadius;
    if (readNumber(sd, "eye", eye)) eye = clampd(eye, EYE_PER_R * h.mRadius - 0.05, EYE_PER_R * h.mRadius + 0.05);
    h.mEye = eye;
    h.mHemisphere = hemi < 0.0 ? -1 : 1;
    // rain: hu_spec_valid 'rain' => !isset($h['rain']) || $h['rain'] === true
    h.mRain = !sd.has("rain") || (sd["rain"].isBoolean() && sd["rain"].asBoolean());
    out = h;
    return true;
}

// Source: README "Time" (t = now - start, T = form + mature + decay; f = t / form, d = (t - form - mature) / decay)
WolfHurricane::Time WolfHurricane::timeAt(F64 now_unix) const
{
    Time T;
    const F64 t = now_unix - mStart;
    const F64 total = mForm + mMature + mDecay;
    if (t < 0.0 || t >= total || mForm <= 0.0 || mDecay <= 0.0) return T;   // "none": everything 0
    const F64 f = t / mForm;
    const F64 d = (t - mForm - mMature) / mDecay;
    if (t < mForm)
    {
        T.stage = FORMING;
        T.O = smooth(f / 0.7);
        T.E = smooth((f - 0.6) / 0.4);
        T.W = smooth(f);
        T.C = smooth(f / 0.4);
    }
    else if (t < mForm + mMature)
    {
        T.stage = MATURE;
        T.O = T.E = T.W = T.C = 1.0;
    }
    else
    {
        T.stage = DECAYING;
        T.O = 1.0 - smooth(d);
        T.E = 1.0 - smooth(d / 0.6);
        T.W = 1.0 - smooth(d);
        T.C = 1.0 - smooth(d);
    }
    // A = form S(clamp(f, 0, 1)) + clamp(t - form, 0, mature) + decay (dd - S(dd)), dd = clamp(d, 0, 1)
    const F64 dd = clampd(d, 0.0, 1.0);
    T.A = mForm * smoothIntegral(f) + clampd(t - mForm, 0.0, mMature) + mDecay * (dd - smoothIntegral(dd));
    return T;
}

// Source: README "The storm document": c(t) = (x0, y0) + (t - start - pass) speed (sin H, cos H), H = heading in radians
void WolfHurricane::centre(F64 now_unix, F64& cx, F64& cy) const
{
    const F64 H = mHeading * (3.14159265358979323846 / 180.0);
    const F64 dt = now_unix - mStart - mPass;
    cx = mX0 + dt * mSpeed * std::sin(H);
    cy = mY0 + dt * mSpeed * std::cos(H);
}

// Source: README "Clouds": D = 0.622 vmax sin(10 deg) A
F64 WolfHurricane::streamed(const Time& T) const
{
    return CLOUD_SPEED_SHARE * mVmax * SIN_ALPHA * T.A;
}

// Source: README "Geometry at a point p" (the same terms as php/hurricane_lib.php hu_geom)
WolfHurricane::Point WolfHurricane::geometry(F64 px, F64 py, F64 cx, F64 cy, const Time& T) const
{
    Point p;
    const F64 R = mRadius, Re = mEye, Rm = RM_PER_RE * Re, Rcdo = RCDO_PER_R * R;
    const F64 dx = px - cx, dy = py - cy;
    p.r = std::sqrt(dx * dx + dy * dy);
    p.phi = std::atan2(dy, dx);   // atan2(0, 0) = 0 in C++ and PHP alike
    p.env = 1.0 - smooth((p.r / R - 1.0) / 0.3);
    p.psi = (F64)mHemisphere * T.O * COT_ALPHA * std::log(R / std::max(p.r, 0.5 * Rm));
    p.B2 = 0.5 + 0.5 * std::cos(2.0 * (p.phi - p.psi));
    p.inner = 1.0 - smooth((p.r - Rcdo) / (0.5 * Rcdo));
    const F64 e = (p.r - 1.25 * Re) / (0.3 * Re);
    p.ew = T.E * std::exp(-e * e);
    p.Meye = 1.0 - T.E * (1.0 - smooth((p.r - 0.8 * Re) / (0.4 * Re)));
    return p;
}

// Source: README "Rain at a point": q = 0 when rain is off, else
//   O Meye max( exp(-((r - 1.25 Re) / (0.4 Re))^2), 0.7 inner, (0.25 + 0.5 B2) env(r) )
F64 WolfHurricane::rainQ(const Point& p, const Time& T) const
{
    if (!mRain || !T.active()) return 0.0;
    const F64 e = (p.r - 1.25 * mEye) / (0.4 * mEye);
    return T.O * p.Meye * std::max(std::max(std::exp(-e * e), 0.7 * p.inner), (0.25 + 0.5 * p.B2) * p.env);
}

// static  Source: README "Rain at a point": q >= 0.75: 4 (torrential), >= 0.5: 3, >= 0.25: 2, >= 0.08: 1, else none
S32 WolfHurricane::rainLevel(F64 q)
{
    if (q >= 0.75) return 4;
    if (q >= 0.5) return 3;
    if (q >= 0.25) return 2;
    if (q >= 0.08) return 1;
    return 0;
}
