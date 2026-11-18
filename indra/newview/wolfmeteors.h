/**
 * @file wolfmeteors.h
 * @brief [METEOR SHOWERS 2026-11-18] Ticket 895 (Luke Koda): the real annual meteor showers, and Paul's sporadic shooting
 *        stars, in the native Wolf night sky.
 *
 * Copyright (C) 2026 Wolf Territories Grid / IntelligentWolf Ltd. All rights reserved.
 *
 * A port of WolfStorm's js/world/wolf_meteors.js (the reference): the same calendar, hash, schedule and geometry, so both
 * viewers show the same meteor at the same moment. The spec with every figure and its source is the firestorm repository's
 * region-terrain-work/meteor-showers-2026-11-18/README.md.
 *
 * This file is plain C++ (no viewer types), so the schedule can be built and compared with the JavaScript on its own.
 * wolfenvironmentrender.cpp feeds it the frame (the place, the clocks, the switches) and hands what it returns to
 * skyF.glsl wolfMeteors.
 */
#ifndef WOLF_METEORS_H
#define WOLF_METEORS_H

#include <cstdint>
#include <vector>

namespace WolfMeteors
{
    /** The shaders' arrays (skyF.glsl wolfMeteors, wolf_meteors.js GLSL). */
    constexpr int MAX_DRAWN = 8;
    /** The showers in the table, numbered 1..SHOWER_COUNT for the debug switch (WolfViewerMeteorShowerForce). */
    constexpr int SHOWER_COUNT = 7;
    /** Seconds of UTC per slot (wolf_meteors.js SLOT_S). */
    constexpr int SLOT_S = 60;

    /** What a slot's schedule depends on (wolf_meteors.js plan's ctx). */
    struct Context
    {
        int32_t cx = 0, cy = 0;     // the sky cell (cellOf)
        bool hasPlace = false;      // the sky follows a real place
        double lat = 0.0, lon = 0.0;
        double skyShift = 0.0;      // whole seconds: the moment the sky shows - UTC
        bool showersOn = false;     // the resident's switch and the region's
        int force = 0;              // 0, or 1..SHOWER_COUNT: that shower at its peak, ten times over (debug)
    };

    /** One meteor of a slot (wolf_meteors.js meteor()). Positions in km, the Earth's centre at 0, the observer at (0, 0, R);
     *  directions (east, north, up). */
    struct Meteor
    {
        double t0 = 0.0, dur = 0.0, qp = 0.5, pathLen = 0.0;
        double B[3] = {}, d[3] = {}, start[3] = {}, axis[3] = {};
        double arc = 0.0, mag = 0.0, trainK = 0.0, tau = 1.0;
        double tint[3] = { 1.0, 1.0, 1.0 };
        bool fireball = false;
        int shower = 0;             // 1..SHOWER_COUNT, 0 sporadic
    };

    /** One drawn meteor as the shader takes it: A = start.xyz, arc; B = axis.xyz, head angle; C = head light, tail (rad),
     *  train light, width factor; D = tint rgb, 0. */
    struct Drawn
    {
        float A[4], B[4], C[4], D[4];
    };

    /** The schedule's hash (wolf_meteors.js hashU): 0 <= u < 1. */
    double hashU(int32_t cx, int32_t cy, uint32_t slot, uint32_t k);
    /** The sun's geometric longitude, J2000 equinox, degrees, at unix seconds t (wolf_meteors.js solarLongitude). */
    double solarLongitude(double t);
    /** Shower i's (0-based) ZHR at solar longitude l. */
    double zhrAt(int i, double l);
    /** The sky cell of a place (wolf_meteors.js cellOf). */
    void cellOf(bool hasPlace, double lat, double lon, int32_t& cx, int32_t& cy);
    /** Every meteor of slot `slot` (wolf_meteors.js plan). */
    std::vector<Meteor> plan(uint32_t slot, const Context& ctx);
    /** E = 10^(-(m + 14.18) / 2.5) lux (Wikipedia "Apparent magnitude"). */
    double luxOf(double m);
    /** The brightest MAX_DRAWN meteors of `list` alive at UTC t, lit by `scale` x luxOf(magnitude) (wolf_meteors.js uniforms
     *  with WolfViewer's unit); returns how many were written to out. */
    int uniforms(const std::vector<Meteor>& list, double t, double scale, Drawn out[MAX_DRAWN]);
    /** The sun 10 degrees or more below the horizon (Paul 2026-10-14): sin of that elevation (wolf_meteors.js NIGHT_SUN_SIN). */
    double nightSunSin();
}

#endif // WOLF_METEORS_H
