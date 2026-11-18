/**
 * @file wolfmeteors.cpp
 * @brief [METEOR SHOWERS 2026-11-18] Ticket 895: the meteor showers' calendar, schedule and geometry.
 *
 * Copyright (C) 2026 Wolf Territories Grid / IntelligentWolf Ltd. All rights reserved.
 *
 * Source: WolfStorm js/world/wolf_meteors.js, line for line (the reference; every figure's source is cited there and in
 * region-terrain-work/meteor-showers-2026-11-18/README.md). Doubles throughout, as JavaScript's numbers, so the two viewers
 * draw the same numbers; the hash is the same unsigned 32-bit arithmetic.
 */
#include "llviewerprecompiledheaders.h"

#include "wolfmeteors.h"

#include <algorithm>
#include <cmath>

namespace
{
    constexpr double PI = 3.14159265358979323846;
    constexpr double D = PI / 180.0;

    /** Source: wolf_meteors.js SHOWERS (IMO 2026 Meteor Shower Calendar Tables 5 and 6; the profiles' sources there). */
    struct Comp { double f, bp, bm; };
    struct Rad { double l, ra, dec; };
    struct Shower
    {
        const char* id;
        double lmax, zhr, v, r, win0, win1;
        int nComps;
        Comp comps[2];
        int nRad;
        Rad rad[9];
        double green;
    };
    const Shower SHOWERS[WolfMeteors::SHOWER_COUNT] = {
        { "QUA", 283.15, 80, 41, 2.1, 276.13, 292.44, 1, { { 1, 2.5, 2.5 }, { 0, 0, 0 } },
          3, { { 279.19, 228, 50 }, { 284.29, 231, 49 }, { 289.38, 234, 48 } }, 0 },
        { "LYR", 32.32, 18, 49, 2.1, 23.71, 40.29, 1, { { 1, 0.457, 0.457 }, { 0, 0, 0 } },
          4, { { 24.69, 263, 34 }, { 29.58, 269, 34 }, { 34.45, 274, 34 }, { 39.32, 279, 34 } }, 0 },
        { "ETA", 45.5, 50, 66, 2.4, 28.60, 67.30, 1, { { 1, 0.080, 0.080 }, { 0, 0, 0 } },
          8, { { 29.58, 323, -7 }, { 34.45, 328, -5 }, { 39.32, 332, -3 }, { 44.17, 337, -1 }, { 49.01, 341, 1 },
               { 53.84, 345, 3 }, { 58.65, 349, 5 }, { 63.46, 353, 7 } }, 0 },
        { "PER", 140.0, 100, 59, 2.2, 114.10, 151.46, 2, { { 70.0 / 93.0, 0.35, 0.35 }, { 23.0 / 93.0, 0.050, 0.092 } },
          9, { { 112.19, 6, 50 }, { 116.96, 11, 52 }, { 121.73, 22, 53 }, { 126.51, 29, 54 }, { 132.25, 37, 56 }, { 137.04, 45, 57 },
               { 141.84, 51, 58 }, { 146.65, 57, 58 }, { 151.46, 63, 58 } }, 0 },
        { "ORI", 208.0, 20, 66, 2.5, 188.45, 225.22, 1, { { 1, 0.12, 0.12 }, { 0, 0, 0 } },
          7, { { 191.40, 85, 14 }, { 196.34, 88, 15 }, { 201.28, 91, 15 }, { 206.24, 94, 16 }, { 211.22, 98, 16 },
               { 216.21, 101, 16 }, { 222.21, 105, 17 } }, 0 },
        { "LEO", 235.27, 15, 71, 2.5, 223.21, 248.42, 1, { { 1, 0.25, 0.25 }, { 0, 0, 0 } },
          5, { { 227.23, 147, 24 }, { 232.26, 150, 23 }, { 237.30, 153, 21 }, { 242.35, 156, 20 }, { 247.41, 159, 19 } }, 0 },
        { "GEM", 262.2, 150, 35, 2.6, 251.46, 268.74, 2, { { 74.0 / 92.0, 0.59, 0.81 }, { 18.0 / 92.0, 0.09, 0.31 } },
          4, { { 252.48, 103, 33 }, { 257.55, 108, 33 }, { 262.64, 113, 33 }, { 267.72, 118, 32 } }, 0.35 },
    };
    const int GEM_INDEX = 6;

    // Source: wolf_meteors.js "the schedule" and "the geometry" constants (GAME TUNING marked there)
    constexpr uint32_t SALT = 0x4d455452u;   // 'METR'
    constexpr double BOOST = 2.0, BRIGHTEN = 1.5, M_FAINT = 6.5, M_BRIGHT = -1.0, FIREBALL_P = 0.02;
    constexpr double FB_BRIGHT = -8.0, FB_FAINT = -4.0, FB_R = 3.0;
    constexpr int MAX_PER_SLOT = 24;
    constexpr double SPORADIC_PER_H = 1.0 / 3.5, SP_BRIGHT = -1.0, SP_FAINT = 2.0, SP_FIREBALL_P = 0.125;
    constexpr double FORCE_X = 10.0;
    constexpr int32_t NO_PLACE = 1000000;
    constexpr double R_EARTH = 6371.0;
    constexpr double MIN_ELEV_DEG = 10.0;
    const double H_PER[3] = { 113.9, 104.4, 96.0 }, H_LEO[3] = { 120.0, 106.9, 96.5 };
    constexpr double V_PER = 59.0, V_LEO = 71.0, MAX_PATH_KM = 400.0;
    constexpr double TAIL = 0.35, TAIL_FB = 0.5, WIDTH_FB = 2.0;
    constexpr double TRAIN_V = 55.0, TRAIN_MAG = 1.0, TRAIN_K = 0.03, TRAIN_TAU = 1.0, TRAIN_K_FB = 0.08, TRAIN_TAU_FB = 2.5;

    double clampd(double v, double lo, double hi) { return std::max(lo, std::min(hi, v)); }
    double dot3(const double* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    void norm3(double* a)
    {
        double l = std::sqrt(dot3(a, a));
        if (!(l > 0.0)) l = 1.0;   // wolf_meteors.js norm: len || 1
        a[0] /= l; a[1] /= l; a[2] /= l;
    }

    /** Source: wolf_meteors.js mix32 (the "lowbias32" finaliser; Math.imul >>> 0 = U32 wrap-around). */
    uint32_t mix32(uint32_t h)
    {
        h ^= h >> 16; h *= 0x7feb352du;
        h ^= h >> 15; h *= 0x846ca68bu;
        h ^= h >> 16;
        return h;
    }

    /** Source: wolf_meteors.js dLam: lambda - lambda0 in (-180, 180]. */
    double dLam(double l, double l0)
    {
        double d = std::fmod(std::fmod(l - l0, 360.0) + 360.0, 360.0);
        if (d > 180.0) d -= 360.0;
        return d;
    }

    /** Source: wolf_meteors.js radiantAt. */
    void radiantAt(const Shower& s, double l, double& ra, double& dec)
    {
        int i = 0;
        while (i < s.nRad - 2 && dLam(l, s.rad[i + 1].l) > 0.0) i++;
        const Rad& a = s.rad[i];
        const Rad& b = s.rad[i + 1];
        const double f = dLam(l, a.l) / dLam(b.l, a.l);
        ra = a.ra + (b.ra - a.ra) * f;
        dec = a.dec + (b.dec - a.dec) * f;
    }

    /** Source: wolf_meteors.js lstDeg (Meeus eq. 12.4; llvowlsky.cpp updateRealStars the same). */
    double lstDeg(double lonDeg, double t)
    {
        const double jd = t / 86400.0 + 2440587.5, T = (jd - 2451545.0) / 36525.0;
        const double gmst = 280.46061837 + 360.98564736629 * (jd - 2451545.0) + 0.000387933 * T * T - T * T * T / 38710000.0;
        return std::fmod(std::fmod(gmst + lonDeg, 360.0) + 360.0, 360.0);
    }

    /** Source: wolf_meteors.js radiantEnu (Meeus eq. 13.5/13.6, llvowlsky.cpp wolf_eq_to_enu). */
    void radiantEnu(const Shower& s, double l, bool hasPlace, double lat, double lon, double t, double* out)
    {
        double ra, dec;
        radiantAt(s, l, ra, dec);
        if (hasPlace)
        {
            const double h = (lstDeg(lon, t) - ra) * D, dc = dec * D, la = lat * D, cd = std::cos(dc), sd = std::sin(dc);
            out[0] = -cd * std::sin(h);
            out[1] = sd * std::cos(la) - cd * std::cos(h) * std::sin(la);
            out[2] = sd * std::sin(la) + cd * std::cos(h) * std::cos(la);
            return;
        }
        const double az = (ra - lstDeg(0.0, t)) * D, c = std::cos(45.0 * D);
        out[0] = c * std::sin(az);
        out[1] = c * std::cos(az);
        out[2] = std::sin(45.0 * D);
    }

    /** Source: wolf_meteors.js heights (Koten et al. 2004). */
    void heights(double v, double* h)
    {
        const double f = clampd((v - V_PER) / (V_LEO - V_PER), 0.0, 1.0);
        for (int k = 0; k < 3; ++k) h[k] = H_PER[k] + (H_LEO[k] - H_PER[k]) * f;
    }

    /** Source: wolf_meteors.js tintFor. */
    void tintFor(double v, double green, bool fireball, double* c)
    {
        const double f = clampd((v - 25.0) / (71.0 - 25.0), 0.0, 1.0);
        c[0] = 1.0 + (0.80 - 1.0) * f;
        c[1] = 0.80 + (0.90 - 0.80) * f;
        c[2] = 0.55 + (1.0 - 0.55) * f;
        if (green > 0.0)
        {
            c[0] = c[0] + (0.85 - c[0]) * green;
            c[1] = c[1] + (1.0 - c[1]) * green;
            c[2] = c[2] + (0.80 - c[2]) * green;
        }
        if (fireball) for (int k = 0; k < 3; ++k) c[k] = 1.0 + (c[k] - 1.0) * 2.0;
        const double y = 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
        for (int k = 0; k < 3; ++k) c[k] /= y;
    }

    /** Source: wolf_meteors.js magDraw. */
    double magDraw(double u, double bright, double faint, double r)
    {
        const double a = std::pow(r, bright), b = std::pow(r, faint);
        return std::log(a + u * (b - a)) / std::log(r);
    }

    /** Source: wolf_meteors.js poisson. */
    int poisson(double u, double mean, int cap)
    {
        if (!(mean > 0.0)) return 0;
        int k = 0;
        double p = std::exp(-mean), F = p;
        while (u > F && k < cap) { k++; p *= mean / k; F += p; }
        return k;
    }

    /** Source: wolf_meteors.js meteor(). u(q) = hashU(cx, cy, slot, k0 + q). */
    bool meteor(double t0, const double* rad, double v, double mag, bool fireball, const double* tint,
                const WolfMeteors::Context& ctx, uint32_t slot, uint32_t k0, WolfMeteors::Meteor& out)
    {
        auto u = [&](uint32_t q) { return WolfMeteors::hashU(ctx.cx, ctx.cy, slot, k0 + q); };
        double h[3];
        heights(v, h);
        const double hb = h[0], hp = h[1], he = h[2];
        const double rp = R_EARTH + hp;
        const double minElev = MIN_ELEV_DEG * D;
        const double phiMax = std::acos(R_EARTH * std::cos(minElev) / rp) - minElev;
        const double cphi = std::cos(phiMax) + u(1) * (1.0 - std::cos(phiMax)), sphi = std::sqrt(std::max(0.0, 1.0 - cphi * cphi));
        const double az = u(2) * 2.0 * PI;
        const double P[3] = { rp * sphi * std::sin(az), rp * sphi * std::cos(az), rp * cphi };
        const double d[3] = { -rad[0], -rad[1], -rad[2] };
        double up[3] = { P[0], P[1], P[2] };
        norm3(up);
        if (-dot3(d, up) < 0.05) return false;
        const double pd = dot3(P, d), pp = dot3(P, P);
        const double sb = -pd - std::sqrt(pd * pd - pp + (R_EARTH + hb) * (R_EARTH + hb));
        const double disc = pd * pd - pp + (R_EARTH + he) * (R_EARTH + he);
        if (disc < 0.0) return false;
        const double se = -pd - std::sqrt(disc);
        if (!(se > 0.0) || !(sb < 0.0) || se - sb > MAX_PATH_KM) return false;
        const double O[3] = { 0.0, 0.0, R_EARTH };
        double Bp[3], Ep[3];
        for (int k = 0; k < 3; ++k) { Bp[k] = P[k] + d[k] * sb; Ep[k] = P[k] + d[k] * se; }
        const double toP[3] = { P[0] - O[0], P[1] - O[1], P[2] - O[2] };
        const double m = mag + 5.0 * std::log10(std::sqrt(dot3(toP, toP)) / 100.0);
        if (m > 7.0) return false;
        double start[3] = { Bp[0] - O[0], Bp[1] - O[1], Bp[2] - O[2] }, end[3] = { Ep[0] - O[0], Ep[1] - O[1], Ep[2] - O[2] };
        norm3(start);
        norm3(end);
        const double arc = std::acos(clampd(dot3(start, end), -1.0, 1.0));
        if (!(arc > 1e-5)) return false;
        double axis[3] = { start[1] * end[2] - start[2] * end[1], start[2] * end[0] - start[0] * end[2], start[0] * end[1] - start[1] * end[0] };
        norm3(axis);
        out.t0 = t0;
        out.dur = (se - sb) / v;
        out.qp = -sb / (se - sb);
        out.pathLen = se - sb;
        for (int k = 0; k < 3; ++k)
        {
            out.B[k] = Bp[k]; out.d[k] = d[k]; out.start[k] = start[k]; out.axis[k] = axis[k]; out.tint[k] = tint[k];
        }
        out.arc = arc;
        out.mag = m;
        out.fireball = fireball;
        const bool train = (v >= TRAIN_V && m <= TRAIN_MAG) || fireball;
        out.trainK = train ? (fireball ? TRAIN_K_FB : TRAIN_K) : 0.0;
        out.tau = fireball ? TRAIN_TAU_FB : TRAIN_TAU;
        return true;
    }
}

double WolfMeteors::hashU(int32_t cx, int32_t cy, uint32_t slot, uint32_t k)
{
    // Source: wolf_meteors.js hashU
    uint32_t h = mix32(slot ^ SALT);
    h = mix32(h ^ (uint32_t)cx);
    h = mix32(h ^ (uint32_t)cy);
    h = mix32(h ^ k);
    return (double)h / 4294967296.0;
}

double WolfMeteors::solarLongitude(double t)
{
    // Source: wolf_meteors.js solarLongitude (Meeus ch. 25 low accuracy; IAU 2006 general precession 5028.796195"/cy)
    const double jd = t / 86400.0 + 2440587.5, T = (jd - 2451545.0) / 36525.0;
    const double l0 = 280.46646 + T * (36000.76983 + T * 0.0003032);
    const double m = (357.52911 + T * (35999.05029 - T * 0.0001537)) * D;
    const double c = (1.914602 - T * (0.004817 + T * 0.000014)) * std::sin(m) + (0.019993 - T * 0.000101) * std::sin(2.0 * m)
                   + 0.000289 * std::sin(3.0 * m);
    const double l = l0 + c - 1.396971 * T;
    return std::fmod(std::fmod(l, 360.0) + 360.0, 360.0);
}

double WolfMeteors::zhrAt(int i, double l)
{
    // Source: wolf_meteors.js zhrAt (Jenniskens 1994: ZHR = ZHRmax 10^(-B |lambda - lambda_max|))
    const Shower& s = SHOWERS[i];
    if (dLam(l, s.win0) < 0.0 || dLam(l, s.win1) > 0.0) return 0.0;
    const double x = dLam(l, s.lmax);
    double z = 0.0;
    for (int c = 0; c < s.nComps; ++c) z += s.zhr * s.comps[c].f * std::pow(10.0, -(x < 0.0 ? s.comps[c].bp : s.comps[c].bm) * std::fabs(x));
    return z;
}

void WolfMeteors::cellOf(bool hasPlace, double lat, double lon, int32_t& cx, int32_t& cy)
{
    // Source: wolf_meteors.js cellOf: Math.round(x * 100) | 0 (Math.round: floor(x + 0.5))
    if (!hasPlace) { cx = cy = NO_PLACE; return; }
    cx = (int32_t)std::floor(lat * 100.0 + 0.5);
    cy = (int32_t)std::floor(lon * 100.0 + 0.5);
}

std::vector<WolfMeteors::Meteor> WolfMeteors::plan(uint32_t slot, const Context& ctx)
{
    // Source: wolf_meteors.js plan
    std::vector<Meteor> out;
    const double tMid = (double)slot * SLOT_S + SLOT_S / 2.0, tSky = tMid + ctx.skyShift;
    const double l = solarLongitude(tSky);
    for (int i = 0; i < SHOWER_COUNT; ++i)
    {
        const Shower& s = SHOWERS[i];
        const bool forced = ctx.force == i + 1;
        if (!ctx.showersOn && !forced) continue;
        const double z = forced ? s.zhr * FORCE_X : zhrAt(i, l);
        if (!(z > 0.0)) continue;
        double rad[3];
        radiantEnu(s, forced ? s.lmax : l, ctx.hasPlace, ctx.lat, ctx.lon, tSky, rad);
        if (forced && rad[2] < std::sin(20.0 * D)) radiantEnu(s, s.lmax, false, 0.0, 0.0, tSky, rad);
        if (rad[2] <= 0.0) continue;
        const uint32_t base = 1000u * (uint32_t)(i + 1);
        const int n = poisson(hashU(ctx.cx, ctx.cy, slot, base), BOOST * z * rad[2] * SLOT_S / 3600.0, MAX_PER_SLOT);
        const double fbp = FIREBALL_P * (i == GEM_INDEX ? 2.0 : 1.0);
        for (int j = 0; j < n; ++j)
        {
            const uint32_t k0 = base + 1u + 16u * (uint32_t)j;
            auto u = [&](uint32_t q) { return hashU(ctx.cx, ctx.cy, slot, k0 + q); };
            const bool fireball = u(4) < fbp;
            const double mag = fireball ? magDraw(u(5), FB_BRIGHT, FB_FAINT, FB_R) : magDraw(u(3), M_BRIGHT, M_FAINT, s.r) - BRIGHTEN;
            double tint[3];
            tintFor(s.v, s.green, fireball, tint);
            Meteor mt;
            if (meteor((double)slot * SLOT_S + u(0) * SLOT_S, rad, s.v, mag, fireball, tint, ctx, slot, k0, mt))
            {
                mt.shower = i + 1;
                out.push_back(mt);
            }
        }
    }
    // the sporadics (Paul 2026-10-14)
    const int ns = poisson(hashU(ctx.cx, ctx.cy, slot, 9000u), SPORADIC_PER_H * SLOT_S / 3600.0, 4);
    for (int j = 0; j < ns; ++j)
    {
        const uint32_t k0 = 9001u + 16u * (uint32_t)j;
        auto u = [&](uint32_t q) { return hashU(ctx.cx, ctx.cy, slot, k0 + q); };
        const double raz = u(6) * 2.0 * PI, rz = std::max(u(7), 1e-3), rh = std::sqrt(1.0 - rz * rz);
        const double rad[3] = { rh * std::sin(raz), rh * std::cos(raz), rz };
        const double v = 20.0 + 50.0 * u(8);
        const bool fireball = u(4) < SP_FIREBALL_P;
        const double mag = SP_BRIGHT + (SP_FAINT - SP_BRIGHT) * u(3) - (fireball ? 3.0 : 0.0);
        double tint[3];
        tintFor(v, 0.0, fireball, tint);
        Meteor mt;
        if (meteor((double)slot * SLOT_S + u(0) * SLOT_S, rad, v, mag, fireball, tint, ctx, slot, k0, mt))
        {
            mt.shower = 0;
            out.push_back(mt);
        }
    }
    return out;
}

double WolfMeteors::luxOf(double m)
{
    // Source: wolf_meteors.js luxOf; Wikipedia "Apparent magnitude" (wolfenvironmentrender.cpp realPlanets the same)
    return std::pow(10.0, -(m + 14.18) / 2.5);
}

double WolfMeteors::nightSunSin()
{
    // Source: wolf_meteors.js NIGHT_SUN_SIN (Paul 2026-10-14, the sun 10 degrees or more down)
    return std::sin(-10.0 * D);
}

int WolfMeteors::uniforms(const std::vector<Meteor>& list, double t, double scale, Drawn out[MAX_DRAWN])
{
    // Source: wolf_meteors.js state() and uniforms()
    struct Live { double head, train, headAngle, tail, width; const Meteor* m; };
    std::vector<Live> live;
    for (const Meteor& mt : list)
    {
        const double age = t - mt.t0;
        const double trainLife = mt.trainK > 0.0 ? 4.0 * mt.tau : 0.0;
        if (age < 0.0 || age >= mt.dur + trainLife) continue;
        const double q = std::min(age / mt.dur, 1.0);
        double hd[3] = { mt.B[0] + mt.d[0] * q * mt.pathLen, mt.B[1] + mt.d[1] * q * mt.pathLen, mt.B[2] + mt.d[2] * q * mt.pathLen - R_EARTH };
        norm3(hd);
        const double headAngle = std::acos(clampd(dot3(mt.start, hd), -1.0, 1.0));
        double lc = 0.0;
        if (age < mt.dur)
        {
            lc = q < mt.qp ? std::pow(std::sin(PI / 2.0 * q / mt.qp), 2.0) : std::pow(std::cos(PI / 2.0 * (q - mt.qp) / (1.0 - mt.qp)), 2.0);
        }
        const double peak = luxOf(mt.mag) * scale;
        const double train = mt.trainK > 0.0 ? mt.trainK * peak * (age < mt.dur ? 1.0 : std::exp(-(age - mt.dur) / mt.tau)) : 0.0;
        const double head = peak * lc;
        if (!(head > 0.0 || train > 0.0)) continue;
        live.push_back({ head, train, headAngle, (mt.fireball ? TAIL_FB : TAIL) * mt.arc, mt.fireball ? WIDTH_FB : 1.0, &mt });
    }
    std::sort(live.begin(), live.end(), [](const Live& a, const Live& b) { return (b.head + b.train) < (a.head + a.train); });
    const int n = std::min((int)live.size(), MAX_DRAWN);
    for (int i = 0; i < n; ++i)
    {
        const Live& s = live[i];
        const Meteor& m = *s.m;
        Drawn& o = out[i];
        o.A[0] = (float)m.start[0]; o.A[1] = (float)m.start[1]; o.A[2] = (float)m.start[2]; o.A[3] = (float)m.arc;
        o.B[0] = (float)m.axis[0];  o.B[1] = (float)m.axis[1];  o.B[2] = (float)m.axis[2];  o.B[3] = (float)s.headAngle;
        o.C[0] = (float)s.head;     o.C[1] = (float)s.tail;     o.C[2] = (float)s.train;    o.C[3] = (float)s.width;
        o.D[0] = (float)m.tint[0];  o.D[1] = (float)m.tint[1];  o.D[2] = (float)m.tint[2];  o.D[3] = 0.f;
    }
    return n;
}
