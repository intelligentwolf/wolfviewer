/**
 * @file wolfairlead.h
 * @brief WolfViewer: Wolf Air - the gun's lead point for the selected target, with the wolf-air plugin's own ballistics.
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

#ifndef WOLF_AIR_LEAD_H
#define WOLF_AIR_LEAD_H

#include <cmath>

// Header only and free of the viewer's types, so the solver is tested on its own (the test compiles this file alone).
//
// The round, exactly as the plugin traces it: region-terrain-work/aircraft-dogfight-2026-11-01/c-wolf-air/src/Gunnery.cs
// Trace(): it leaves at the gun's muzzle velocity along the aim PLUS the firing aircraft's own velocity (AirEngine.cs Fire:
// "d * g.Type.MuzzleMs + vel"), and each step acc = g - v (k |v|), q = p + v dt + acc dt^2 / 2, v += acc dt, with
//   g = 9.80665 m/s2                      AirRules.cs G
//   dt = 0.02 s                           AirRules.cs STEP_S
//   at most 2.0 s of flight               AirRules.cs MAX_FLIGHT_S
//   spent after `reach` metres            AirRules.cs REACH_M (sent as rch=)
// k (drag, 1/m) and the muzzle velocity come in the contacts look (mv=, dk=: the gun this person fires).
namespace WolfAirLead
{
    struct V { double x = 0, y = 0, z = 0; };
    inline V add(V a, V b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
    inline V sub(V a, V b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
    inline V mul(V a, double k) { return { a.x * k, a.y * k, a.z * k }; }
    inline double len(V a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
    inline V unit(V a) { const double l = len(a); return l > 1e-9 ? mul(a, 1.0 / l) : V{ 1, 0, 0 }; }

    const double G = 9.80665, STEP_S = 0.02, MAX_FLIGHT_S = 2.0;
    const int ITERATIONS = 6;

    // Where a round fired along `aim` (unit) is after `t` seconds, relative to the muzzle; false once it is spent.
    inline bool roundAt(V aim, V own_vel, double mv, double k, double reach, double t, V& out)
    {
        V p, v = add(mul(aim, mv), own_vel);
        double travelled = 0, s = 0;
        while (s + STEP_S <= t + 1e-9)
        {
            const double sp = len(v);
            const V acc = sub(V{ 0, 0, -G }, mul(v, k * sp));
            const V q = add(add(p, mul(v, STEP_S)), mul(acc, 0.5 * STEP_S * STEP_S));
            travelled += len(sub(q, p));
            p = q;
            v = add(v, mul(acc, STEP_S));
            s += STEP_S;
            if (travelled >= reach) return false;
        }
        // the part step to t (same formula, a shorter dt)
        const double dt = t - s;
        if (dt > 0)
        {
            const V acc = sub(V{ 0, 0, -G }, mul(v, k * len(v)));
            p = add(add(p, mul(v, dt)), mul(acc, 0.5 * dt * dt));
        }
        out = p;
        return true;
    }

    // The time the round fired along `aim` takes to reach `dist` metres from the muzzle (straight line), or -1.
    inline double timeTo(V aim, V own_vel, double mv, double k, double reach, double dist)
    {
        V p, v = add(mul(aim, mv), own_vel);
        double travelled = 0;
        for (double s = 0; s < MAX_FLIGHT_S - 1e-9; s += STEP_S)
        {
            const double sp = len(v);
            const V acc = sub(V{ 0, 0, -G }, mul(v, k * sp));
            const V q = add(add(p, mul(v, STEP_S)), mul(acc, 0.5 * STEP_S * STEP_S));
            const double a = len(p), b = len(q);
            if (b >= dist)
            {
                const double f = b > a ? (dist - a) / (b - a) : 0.0;
                return s + STEP_S * f;
            }
            travelled += len(sub(q, p));
            if (travelled >= reach) return -1;
            p = q;
            v = add(v, mul(acc, STEP_S));
        }
        return -1;
    }

    /**
     * The aim (unit, the direction the guns must point) whose round meets a target at `rel` (from the muzzle, metres) moving
     * at `tgt_vel`, fired from an aircraft moving at `own_vel`; `tof` its time of flight. False when the round is spent first
     * (beyond the reach or 2 s) or the figures are not usable.
     */
    inline bool solve(V rel, V tgt_vel, V own_vel, double mv, double k, double reach, V& aim, double& tof)
    {
        if (!(mv > 0) || !(reach > 0) || !(k >= 0) || len(rel) < 1.0 || len(rel) > reach) return false;
        aim = unit(rel);
        tof = -1;
        for (int i = 0; i < ITERATIONS; ++i)
        {
            // where the target will be when the round has come as far as it
            double t = timeTo(aim, own_vel, mv, k, reach, len(rel));
            if (t < 0) return false;
            for (int j = 0; j < 3; ++j)
            {
                const double t2 = timeTo(aim, own_vel, mv, k, reach, len(add(rel, mul(tgt_vel, t))));
                if (t2 < 0) return false;
                t = t2;
            }
            const V want = add(rel, mul(tgt_vel, t));
            V got;
            if (!roundAt(aim, own_vel, mv, k, reach, t, got)) return false;
            // turn the aim by the miss, seen from the muzzle
            aim = unit(add(aim, mul(sub(want, got), 1.0 / std::fmax(1.0, len(want)))));
            tof = t;
        }
        return tof > 0;
    }
}

#endif // WOLF_AIR_LEAD_H
