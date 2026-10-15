/**
 * @file wolfaurora.cpp
 * @brief WolfViewer: the northern lights as the real thing - the CPU half of wolfAuroraF.glsl.
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

// [WOLF AURORA 2026-10-12] Source: WolfStorm js/rendering/wolf_aurora.js state() and its helpers - the
// same computation, constant for constant; every number's source is cited there and in
// region-terrain-work/wolf-aurora/DESIGN.md (firestorm repo), and the tables come out of make_tables.py.

#include "llviewerprecompiledheaders.h"

#include "wolfaurora.h"

#include <algorithm>
#include <cmath>

#include "llagent.h"
#include "llagentcamera.h"
#include "lldate.h"
#include "llenvironment.h"
#include "llframetimer.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "llstartup.h"
#include "pipeline.h"
#include "wolfweather.h"
#include "wolfenvironmentrender.h"
#include "wolfweatherprofile.h"

extern bool gCubeSnapshot;

namespace
{
    // U.S. Standard Atmosphere 1976 (NASA-TM-X-74335) at the profile nodes h_i = 80 + 320 (i/63)^2 km:
    // km, T K, rho g/cm^3, n_tot, n_N2, n_O2, n_O cm^-3 (make_tables.py, ussa1976 0.3.4).
    // Source: wolf_aurora.js ATM.
    constexpr S32 NODES = 64;
    const F64 ATM[NODES][7] = {
        { 80.0000, 198.64, 1.84579e-08, 3.83769e+14, 2.99662e+14, 8.03903e+13, 0.00000e+00 },
        { 80.0806, 198.48, 1.82243e-08, 3.78912e+14, 2.95869e+14, 7.93729e+13, 0.00000e+00 },
        { 80.3225, 198.01, 1.75400e-08, 3.64685e+14, 2.84760e+14, 7.63927e+13, 0.00000e+00 },
        { 80.7256, 197.22, 1.64529e-08, 3.42083e+14, 2.67112e+14, 7.16582e+13, 0.00000e+00 },
        { 81.2900, 196.12, 1.50370e-08, 3.12644e+14, 2.44125e+14, 6.54914e+13, 0.00000e+00 },
        { 82.0156, 194.71, 1.33845e-08, 2.78285e+14, 2.17296e+14, 5.82940e+13, 0.00000e+00 },
        { 82.9025, 192.98, 1.15963e-08, 2.41106e+14, 1.88265e+14, 5.05059e+13, 0.00000e+00 },
        { 83.9506, 190.94, 9.77267e-09, 2.03189e+14, 1.58658e+14, 4.25633e+13, 0.00000e+00 },
        { 85.1600, 188.58, 8.00407e-09, 1.66418e+14, 1.29945e+14, 3.48605e+13, 0.00000e+00 },
        { 86.5306, 186.87, 6.33105e-09, 1.31699e+14, 1.02799e+14, 2.75702e+13, 1.00569e+11 },
        { 88.0625, 186.87, 4.82127e-09, 1.00332e+14, 7.82771e+13, 2.09655e+13, 1.55585e+11 },
        { 89.7556, 186.87, 3.56814e-09, 7.43152e+13, 5.79278e+13, 1.54629e+13, 2.36870e+11 },
        { 91.6100, 186.90, 2.56553e-09, 5.35178e+13, 4.16569e+13, 1.10354e+13, 3.35402e+11 },
        { 93.6256, 187.53, 1.78708e-09, 3.73777e+13, 2.90396e+13, 7.57937e+12, 4.22442e+11 },
        { 95.8025, 189.11, 1.20509e-09, 2.53038e+13, 1.96231e+13, 4.99010e+12, 4.69356e+11 },
        { 98.1406, 191.93, 7.87407e-10, 1.66254e+13, 1.28707e+13, 3.13996e+12, 4.75595e+11 },
        { 100.6400, 196.38, 5.01081e-10, 1.06617e+13, 8.23709e+12, 1.88897e+12, 4.52182e+11 },
        { 103.3006, 203.11, 3.13654e-10, 6.74012e+12, 5.19723e+12, 1.09177e+12, 4.03278e+11 },
        { 106.1224, 213.43, 1.91798e-10, 4.17181e+12, 3.20430e+12, 6.02866e+11, 3.38824e+11 },
        { 109.1056, 231.19, 1.14388e-10, 2.52336e+12, 1.92295e+12, 3.18895e+11, 2.68491e+11 },
        { 112.2499, 267.00, 6.59396e-11, 1.47672e+12, 1.11066e+12, 1.62154e+11, 1.97750e+11 },
        { 115.5556, 306.67, 3.98487e-11, 9.05354e+11, 6.69210e+11, 8.75497e+10, 1.45527e+11 },
        { 119.0224, 348.27, 2.52101e-11, 5.80323e+11, 4.20387e+11, 5.05447e+10, 1.07748e+11 },
        { 122.6505, 391.02, 1.65960e-11, 3.86709e+11, 2.73937e+11, 3.09375e+10, 8.08966e+10 },
        { 126.4399, 432.73, 1.13337e-11, 2.67209e+11, 1.84757e+11, 1.98792e+10, 6.20058e+10 },
        { 130.3905, 473.13, 7.97099e-12, 1.90136e+11, 1.28105e+11, 1.32268e+10, 4.84459e+10 },
        { 134.5024, 512.08, 5.74177e-12, 1.38589e+11, 9.08373e+10, 9.02936e+09, 3.84866e+10 },
        { 138.7755, 549.46, 4.21900e-12, 1.03066e+11, 6.56075e+10, 6.28897e+09, 3.10090e+10 },
        { 143.2099, 585.18, 3.15245e-12, 7.79610e+10, 4.81130e+10, 4.45263e+09, 2.52831e+10 },
        { 147.8055, 619.17, 2.38939e-12, 5.98346e+10, 3.57347e+10, 3.19562e+09, 2.08228e+10 },
        { 152.5624, 651.38, 1.83368e-12, 4.65086e+10, 2.68287e+10, 2.31997e+09, 1.72986e+10 },
        { 157.4805, 681.78, 1.42221e-12, 3.65449e+10, 2.03218e+10, 1.70017e+09, 1.44756e+10 },
        { 162.5598, 710.35, 1.11300e-12, 2.89815e+10, 1.55031e+10, 1.25536e+09, 1.21854e+10 },
        { 167.8005, 737.10, 8.78230e-13, 2.31791e+10, 1.19021e+10, 9.33102e+08, 1.03131e+10 },
        { 173.2023, 762.05, 6.98040e-13, 1.86776e+10, 9.18564e+09, 6.97364e+08, 8.76874e+09 },
        { 178.7654, 785.24, 5.58258e-13, 1.51465e+10, 7.11783e+09, 5.23355e+08, 7.48307e+09 },
        { 184.4898, 806.71, 4.49027e-13, 1.23554e+10, 5.53465e+09, 3.94150e+08, 6.40704e+09 },
        { 190.3754, 826.51, 3.62977e-13, 1.01305e+10, 4.31491e+09, 2.97617e+08, 5.50042e+09 },
        { 196.4223, 844.72, 2.94717e-13, 8.34374e+09, 3.37053e+09, 2.25151e+08, 4.73212e+09 },
        { 202.6304, 861.39, 2.40260e-13, 6.90031e+09, 2.63655e+09, 1.70546e+08, 4.07855e+09 },
        { 208.9997, 876.61, 1.96563e-13, 5.72696e+09, 2.06414e+09, 1.29272e+08, 3.51991e+09 },
        { 215.5304, 890.46, 1.61334e-13, 4.76835e+09, 1.61669e+09, 9.80090e+07, 3.04088e+09 },
        { 222.2222, 903.01, 1.32794e-13, 3.98113e+09, 1.26602e+09, 7.42741e+07, 2.62878e+09 },
        { 229.0753, 914.36, 1.09594e-13, 3.33228e+09, 9.91104e+08, 5.62553e+07, 2.27351e+09 },
        { 236.0897, 924.58, 9.06501e-14, 2.79495e+09, 7.75142e+08, 4.25525e+07, 1.96639e+09 },
        { 243.2653, 933.75, 7.51411e-14, 2.34873e+09, 6.05605e+08, 3.21436e+07, 1.70062e+09 },
        { 250.6022, 941.96, 6.24011e-14, 1.97688e+09, 4.72465e+08, 2.42369e+07, 1.47027e+09 },
        { 258.1003, 949.29, 5.19050e-14, 1.66607e+09, 3.67950e+08, 1.82358e+07, 1.27040e+09 },
        { 265.7596, 955.80, 4.32412e-14, 1.40580e+09, 2.86034e+08, 1.36905e+07, 1.09698e+09 },
        { 273.5802, 961.58, 3.60652e-14, 1.18712e+09, 2.21835e+08, 1.02494e+07, 9.46306e+08 },
        { 281.5621, 966.69, 3.01177e-14, 1.00329e+09, 1.71677e+08, 7.65371e+06, 8.15576e+08 },
        { 289.7052, 971.19, 2.51707e-14, 8.48245e+08, 1.32492e+08, 5.69678e+06, 7.02000e+08 },
        { 298.0096, 975.14, 2.10556e-14, 7.17501e+08, 1.01999e+08, 4.22791e+06, 6.03532e+08 },
        { 306.4752, 978.61, 1.76223e-14, 6.06960e+08, 7.82886e+07, 3.12680e+06, 5.18105e+08 },
        { 315.1020, 981.64, 1.47566e-14, 5.13502e+08, 5.99179e+07, 2.30476e+06, 4.44129e+08 },
        { 323.8901, 984.28, 1.23613e-14, 4.34413e+08, 4.57205e+07, 1.69292e+06, 3.80129e+08 },
        { 332.8395, 986.57, 1.03554e-14, 3.67388e+08, 3.47699e+07, 1.23867e+06, 3.24779e+08 },
        { 341.9501, 988.55, 8.67803e-15, 3.10703e+08, 2.63699e+07, 9.03445e+05, 2.77089e+08 },
        { 351.2220, 990.27, 7.26857e-15, 2.62555e+08, 1.99191e+07, 6.55895e+05, 2.35892e+08 },
        { 360.6551, 991.74, 6.08855e-15, 2.21830e+08, 1.50029e+07, 4.74593e+05, 2.00508e+08 },
        { 370.2494, 993.01, 5.09834e-15, 1.87325e+08, 1.12613e+07, 3.42049e+05, 1.70112e+08 },
        { 380.0050, 994.10, 4.26666e-15, 1.58078e+08, 8.42146e+06, 2.45471e+05, 1.44031e+08 },
        { 389.9219, 995.03, 3.56969e-15, 1.33358e+08, 6.27861e+06, 1.75547e+05, 1.21744e+08 },
        { 400.0000, 995.83, 2.98429e-15, 1.12426e+08, 4.66352e+06, 1.25002e+05, 1.02692e+08 }
    };

    // Source: wolf_aurora.js FANG_P - Fang et al. 2008, JGR 113 A09311, table 1
    const F64 FANG_P[8][4] = {
        { 3.49979e-1, -6.18200e-2, -4.08124e-2, 1.65414e-2 },
        { 5.85425e-1, -5.00793e-2, 5.69309e-2, -4.02491e-3 },
        { 1.69692e-1, -2.58981e-2, 1.96822e-2, 1.20505e-3 },
        { -1.22271e-1, -1.15532e-2, 5.37951e-6, 1.20189e-3 },
        { 1.57018, 2.87896e-1, -4.14857e-1, 5.18158e-2 },
        { 8.83195e-1, 4.31402e-2, -8.33599e-2, 1.02515e-2 },
        { 1.90953, -4.74704e-2, -1.80200e-1, 2.46652e-2 },
        { -1.29566, -2.10952e-1, 2.73106e-1, -2.92752e-2 },
    };
    constexpr F64 KB = 1.380649e-16;      // erg/K
    constexpr F64 RE_G = 6356.766e5;      // cm, USSA 1976 gravity radius
    constexpr F64 G0 = 980.665;           // cm/s^2

    // Source: wolf_aurora.js YIELD - Barthelemy et al. 2025 table 2, kR per erg cm^-2 s^-1 at 0.1, 1, 10 keV;
    // groups G 557.7, R 630-636-639, B 427.8, P N2 1PG
    const F64 YIELD[4][3] = {
        { 0.650, 0.325, 0.445 },
        { 1.720, 0.525, 0.082 },
        { 0.033, 0.072, 0.025 },
        { 0.580, 0.303, 0.296 },
    };
    constexpr F64 E0_QUIET = 1.5476;      // Source: wolf_aurora.js E0_QUIET (Whiter et al. 2023 green peak)
    constexpr F64 E0_TYPEB = 9.0102;      // Source: wolf_aurora.js E0_TYPEB (type-B lower border, 85 km)
    // [WolfViewer 2026-10-15] WolfForceAurora with no live data: the owner's slider at half way (10^1.5 = 32 kR at the brightest arc)
    constexpr F64 FORCE_AMOUNT = 0.5;
    // [w66 CI 2026-10-14] M_PI is not defined by MSVC's <cmath> unless _USE_MATH_DEFINES is set first (windows64 failed
    // with C2065); pi to double precision, the value glibc's math.h gives M_PI.
    constexpr F64 WA_PI = 3.14159265358979323846;
    constexpr F64 DEG = WA_PI / 180.0;
    constexpr F64 R_E = 6371.0;
    constexpr F64 SIG_ENV = 7.644, SIG_FIL = 0.4247, W_FIL = 0.75;   // wolfAuroraF.glsl WA_SIG_ENV / WA_SIG_FIL / WA_W_FIL
    constexpr F64 DIFFUSE = 0.15;
    constexpr F64 MANUAL_MLAT = 67.49;
    constexpr F64 Y_G = 1.93324e-4;       // wolfAuroraF.glsl WA_Y_G

    // Source: wolf_aurora.js MAIN - the owner's colours, linearised, Y = 1 (index = colour mode)
    const F32 MAIN[7][3] = {
        { 0.f, 0.f, 0.f },
        { 4.33443f, 0.09527f, 0.14349f },
        { 2.06969f, 0.23987f, 5.37991f },
        { 0.13604f, 0.85309f, 4.99924f },
        { 3.41973f, 0.25046f, 1.29969f },
        { 0.02690f, 1.37181f, 0.18227f },
        { 0.f, 0.f, 0.f },
    };

    // Source: wolf_aurora.js QUALITY - the table's size (square, the stereographic hemisphere), the
    // march's steps and the frames a pass over the table takes, per graphics level (RenderQualityPerformance 0..6)
    const S32 QUALITY[7][3] = { { 384, 14, 4 }, { 448, 16, 6 }, { 512, 16, 8 }, { 576, 18, 8 }, { 640, 18, 12 }, { 768, 20, 16 }, { 896, 22, 20 } };
    constexpr F32 ACCUMULATE = 0.5f;      // Source: wolf_aurora.js ACCUMULATE - the moving average's weight for a new march

    F64 clampd(F64 x, F64 a, F64 b) { return x < a ? a : (x > b ? b : x); }
    F64 nodeAlt(S32 i) { const F64 x = (F64)i / (NODES - 1); return 80.0 + 320.0 * x * x; }

    // Source: wolf_aurora.js yieldAt
    F64 yieldAt(const F64* list, F64 e)
    {
        const F64 l = log10(clampd(e, 0.1, 10.0));
        const S32 i = l < 0.0 ? 0 : 1;
        const F64 f = l < 0.0 ? l + 1.0 : l;
        return exp(log(list[i]) + (log(list[i + 1]) - log(list[i])) * f);
    }

    // Source: wolf_aurora.js vertTable
    void vertTable(F64 E0, F32* out)
    {
        const F64 L = log(E0);
        F64 C[8];
        for (S32 k = 0; k < 8; ++k) C[k] = exp(FANG_P[k][0] + FANG_P[k][1] * L + FANG_P[k][2] * L * L + FANG_P[k][3] * L * L * L);
        F64 shape[4][NODES];
        for (S32 i = 0; i < NODES; ++i)
        {
            const F64 z = ATM[i][0], T = ATM[i][1], rho = ATM[i][2], ntot = ATM[i][3], nN2 = ATM[i][4], nO2 = ATM[i][5], nO = ATM[i][6];
            const F64 m = rho / ntot;
            const F64 g = G0 * pow(RE_G / (RE_G + z * 1e5), 2.0);
            const F64 H = KB * T / (m * g);
            const F64 y = (1.0 / E0) * pow(rho * H / 4e-6, 0.606);
            const F64 f = C[0] * pow(y, C[1]) * exp(-C[2] * pow(y, C[3])) + C[4] * pow(y, C[5]) * exp(-C[6] * pow(y, C[7]));
            const F64 q = f / H;
            const F64 q1s = 1.26 / (1.26 + 4.9e-12 * exp(-885.0 / T) * nO2);
            const F64 a1d = 1.0 / 110.0;
            const F64 q1d = a1d / (a1d + 1.99e-11 * exp(145.0 / T) * nN2 + 3.39e-11 * exp(63.0 / T) * nO2);
            shape[0][i] = q * q1s;
            shape[1][i] = q * (nO / ntot) * q1d;
            shape[2][i] = q;
            shape[3][i] = q;
        }
        for (S32 c = 0; c < 4; ++c)
        {
            F64 area = 0.0;
            for (S32 i = 1; i < NODES; ++i) area += 0.5 * (shape[c][i] + shape[c][i - 1]) * (nodeAlt(i) - nodeAlt(i - 1));
            const F64 yk = yieldAt(YIELD[c], 2.0 * E0);   // the Maxwellian's mean energy
            for (S32 i = 0; i < NODES; ++i) out[i * 4 + c] = area > 0.0 ? (F32)(shape[c][i] / area * yk) : 0.f;
        }
    }

    struct Arcs { F64 pos[3] = {}; F64 w[3] = {}; F64 total = 0.0; };

    // Source: wolf_aurora.js bandAt
    F64 bandAt(const F64* band, F64 first, F64 step, F64 v)
    {
        const F64 x = (v - first) / step;
        if (x <= 0.0 || x >= 23.0) return 0.0;
        const S32 i = (S32)floor(x);
        return band[i] + (band[i + 1] - band[i]) * (x - i);
    }

    // Source: wolf_aurora.js placeArcs
    Arcs placeArcs(const F64* band, F64 first, F64 step, F64 act, F64 t)
    {
        F64 cum[24];
        cum[0] = 0.0;
        for (S32 i = 1; i < 24; ++i) cum[i] = cum[i - 1] + 0.5 * (band[i] + band[i - 1]) * step;
        Arcs a;
        a.total = cum[23];
        if (!(a.total > 0.0)) { a.total = 0.0; return a; }
        for (S32 k = 0; k < 3; ++k)
        {
            const F64 target = a.total * (2 * k + 1) / 6.0;
            S32 i = 1;
            while (i < 23 && cum[i] < target) ++i;
            const F64 f = (target - cum[i - 1]) / std::max(cum[i] - cum[i - 1], 1e-12);
            a.pos[k] = first + (i - 1 + clampd(f, 0.0, 1.0)) * step;
            a.w[k] = (1.0 - DIFFUSE) * a.total / 3.0;
        }
        const F64 spread = std::max(10.0, (a.pos[2] - a.pos[0]) / 2.0);
        const F64 A = std::min(20.0, 0.25 * spread);
        const F64 v = 0.1 + 0.5 * act;
        const F64 period = 2.0 * WA_PI * A / v;
        for (S32 k = 0; k < 3; ++k) a.pos[k] += A * sin(2.0 * WA_PI * t / (period * (1.0 + 0.37 * k)) + 2.1 * k);
        return a;
    }

    // Source: wolf_aurora.js brightestKR
    F64 brightestKR(const F64* band, F64 first, F64 step, const Arcs& a, F64 E0)
    {
        const F64 nFil = W_FIL / (SIG_FIL * sqrt(2.0 * WA_PI)), nEnv = (1.0 - W_FIL) / (SIG_ENV * sqrt(2.0 * WA_PI));
        F64 best = 0.0;
        for (S32 k = 0; k < 3; ++k) best = std::max(best, DIFFUSE * bandAt(band, first, step, a.pos[k]) + a.w[k] * (nFil + nEnv));
        return best * yieldAt(YIELD[0], 2.0 * E0);
    }

    // Source: wolf_aurora.js apparentKR
    F64 apparentKR(const Arcs& a, const F32* vert)
    {
        F64 peak = 0.0;
        for (S32 i = 0; i < NODES; ++i) peak = std::max(peak, (F64)vert[i * 4]);
        return std::max(a.w[0], std::max(a.w[1], a.w[2])) * peak;
    }

    // Source: wolf_aurora.js energyFor
    F64 energyFor(F64 kR)
    {
        const F64 c = clampd(log10(std::max(kR, 1e-6)) - 2.0, 0.0, 1.0);
        return exp(log(E0_QUIET) + (log(E0_TYPEB) - log(E0_QUIET)) * c);
    }

    // Source: wolf_aurora.js adaptation. sun, moon: unit (east, north, up)
    void adaptation(const LLVector3& sun, const LLVector3& moon, bool have_moon, F64& Y, F64& key)
    {
        const F64 night = 108000.0 * pow(10.0, -0.4 * 22.0);
        const F64 zeta = 90.0 - asin(clampd(sun.mV[VZ], -1.0, 1.0)) / DEG;
        const F64 x = std::min(zeta, 108.3) - 95.0;
        const F64 b = x < 0.0 ? 11.84 + 1.518 * x : 11.84 + 1.518 * x - 0.057 * x * x;
        const F64 tw = 108000.0 * pow(10.0, -0.4 * b);
        F64 moon_l = 0.0;
        if (have_moon && moon.mV[VZ] > 0.f)
        {
            const F64 k = 0.172;
            const F64 elong = acos(clampd(sun * moon, -1.0, 1.0)) / DEG;
            const F64 alpha = 180.0 - elong;
            const F64 m = -12.73 + 0.026 * fabs(alpha) + 4e-9 * pow(alpha, 4.0);
            const F64 istar = pow(10.0, -0.4 * (m + 16.57));
            const F64 zm = acos(clampd(moon.mV[VZ], -1.0, 1.0));
            const F64 rho = zm / DEG;
            const F64 f = pow(10.0, 5.36) * (1.06 + cos(zm) * cos(zm)) + pow(10.0, 6.15 - rho / 40.0);
            const F64 xm = 1.0 / sqrt(1.0 - 0.96 * sin(zm) * sin(zm));
            const F64 bnl = f * istar * pow(10.0, -0.4 * k * xm) * (1.0 - pow(10.0, -0.4 * k));
            moon_l = bnl * 3.1831e-6;
        }
        Y = std::max(night, tw) + moon_l;
        key = 1.03 - 2.0 / (2.0 + log10(Y + 1.0));
    }

    struct State
    {
        U32 frame = 0xffffffff;
        F32 gate = 0.f, time = 0.f;
        S32 color = 0;
        F32 band[24] = {};
        F32 geo[4] = {}, morph[4] = {}, arcPos[4] = {}, arcW[4] = {}, eye[4] = {}, lut[4] = {}, main[3] = {};
        F32 vert[NODES * 4] = {};
        F64 vertE0 = -1.0;
        S32 width = 0, parts = 4;
        bool tableFresh = false;   // the table has been started (cleared)
        U32 part = 0;
        U32 sweep = 0;             // whole passes over the table since it was started
    };
    State sState;
    LLRenderTarget sLut;

    S32 colorMode(const std::string& id)
    {
        static const char* IDS[7] = { "green", "red", "purple", "blue", "pink", "multi", "real" };
        for (S32 i = 0; i < 7; ++i) if (id == IDS[i]) return i;
        return 0;
    }

    void ensureVert(F64 E0)
    {
        if (sState.vertE0 > 0.0 && fabs(log(E0 / sState.vertE0)) <= 0.01) return;
        vertTable(E0, sState.vert);
        sState.vertE0 = E0;
    }

    // Source: wolf_aurora.js state()
    void computeState()
    {
        State& s = sState;
        s.gate = 0.f;
        if (!WolfWeather::instanceExists()) return;
        const WolfWeatherProfile& p = WolfWeather::instance().activeProfile();
        LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
        if (!sky) return;
        F64 amount = clampd(p.mAurora, 0, 100) / 100.0;
        // [WolfViewer 2026-10-15] Paul: "a setting wolfForceAurora ... force the aurora whatever the day time or weather" -
        // a test switch (the w66 Windows aurora crash hunt): night sun, clear air, and an aurora even with no live data
        const bool force = WolfAurora::forced();
        const S32 mode = colorMode(p.mAuroraColor);
        const F64 t = fmod(LLDate::now().secondsSinceEpoch(), 86400.0);
        const WolfWeatherProfile::AuroraLive& live_in = p.mAuroraLive;
        // forced, live data that is all zeros (no oval over this place tonight) counts as none
        bool live_flux = false;
        for (S32 i = 0; i < 24; ++i) live_flux = live_flux || live_in.mFlux[i] > 0.f;
        const bool live = mode == 6 && live_in.mValid && (!force || live_flux);
        if (force && !live) amount = std::max(amount, FORCE_AMOUNT);   // the region's live data when it has some

        LLVector3 sun = sky->getSunDirection(); sun.normalize();
        LLVector3 moon = sky->getMoonDirection(); moon.normalize();
        // [WolfViewer round 13] under the native sky its own sun and moon (a document's bodies can differ from the EEP sky's):
        // the aurora was gated by the EEP sun while the native sky showed the document's day (Paul 2026-10-13: "the
        // northern lights are showing all day")
        if (WolfEnvironmentRender::skyActive())
        {
            LLVector3 ns, nm;
            if (WolfEnvironmentRender::bodyDirections(ns, nm)) { sun = ns; sun.normalize(); moon = nm; moon.normalize(); }
        }
        if (force) sun = LLVector3(0.f, 0.f, -1.f);   // [2026-10-15] WolfForceAurora: the sun straight down (night gate, night eye)
        const F64 elev = asin(clampd(sun.mV[VZ], -1.0, 1.0)) / DEG;
        // [WolfViewer round 13] in from civil twilight's end to nautical twilight's end (the sun 6 to 12 deg under, AMS Glossary
        // "civil twilight" / "nautical twilight"): until the sky is dark the aurora is drawn on top of it at its display level
        // (the stock dome: wolfAuroraDisplay is display-referred) and read as daylight decoration. Was -1 .. -4 deg.
        const F64 sn = clampd((-6.0 - elev) / 6.0, 0.0, 1.0);
        const F64 night = sn * sn * (3.0 - 2.0 * sn);
        const F64 clear_air = force ? 1.0 : 1.0 - clampd(p.mFog, 0, 100) / 100.0;   // [2026-10-15] WolfForceAurora: no fog

        F64 band[24];
        F64 first, step, act, bearing = 0.0, mlat = MANUAL_MLAT, E0, kR;
        Arcs arcs;
        if (live)
        {
            const F64 hide = 1.0 - clampd(live_in.mHide, 0.0, 100.0) / 100.0;
            for (S32 i = 0; i < 24; ++i) band[i] = std::max(0.0, (F64)live_in.mFlux[i]) * hide;
            first = live_in.mStartKm; step = live_in.mStepKm;
            bearing = live_in.mBearing;
            mlat = clampd(fabs(live_in.mMLat), 1.0, 89.0);
            arcs = placeArcs(band, first, step, 0.0, t);
            kR = brightestKR(band, first, step, arcs, E0_QUIET);
            act = live_in.mKp >= 0.f ? clampd(live_in.mKp / 9.0, 0.0, 1.0) : clampd(log10(std::max(kR, 1.0)) / 3.0, 0.0, 1.0);
            arcs = placeArcs(band, first, step, act, t);
            kR = brightestKR(band, first, step, arcs, E0_QUIET);
            E0 = energyFor(kR);
            ensureVert(E0);
        }
        else
        {
            act = amount;
            const F64 c = 220.0 - 160.0 * amount, w = 90.0 + 160.0 * amount;
            first = c - 2.5 * w; step = 5.0 * w / 23.0;
            for (S32 i = 0; i < 24; ++i) { const F64 v = first + i * step; band[i] = exp(-((v - c) / w) * ((v - c) / w)); }
            arcs = placeArcs(band, first, step, act, t);
            kR = pow(10.0, 3.0 * amount);
            E0 = energyFor(kR);
            ensureVert(E0);
            const F64 unit = apparentKR(arcs, s.vert);
            const F64 F = unit > 0.0 ? kR / unit : 0.0;
            for (S32 i = 0; i < 24; ++i) band[i] *= F;
            for (S32 k = 0; k < 3; ++k) arcs.w[k] *= F;
        }
        const bool on = live ? arcs.total > 0.0 : amount > 0.0;
        S32 lo = 0, hi = -1;
        for (S32 i = 0; i < 24; ++i) if (band[i] > 0.0) { if (hi < lo) lo = i; hi = i; }
        const F64 sup_min = hi >= lo ? first + (lo - 1) * step : 0.0, sup_max = hi >= lo ? first + (hi + 1) * step : -1.0;
        static LLCachedControl<U32> level(gSavedSettings, "RenderQualityPerformance", 1);
        const S32* q = QUALITY[llmin((U32)level, 6u)];
        const F64 tanM = tan(mlat * DEG);
        F64 Y, key;
        adaptation(sun, moon, true, Y, key);
        // Source: wolf_aurora.js state() eye - one setting for every colour (Paul 2026-10-12: the eye by
        // default, with the option; Preferences / the Weather floater "Northern lights")
        static LLCachedControl<bool> photographic(gSavedSettings, "WolfAuroraPhotographic", false);
        const bool eye = !photographic;
        if (!eye)
        {
            const F64 yAur = apparentKR(arcs, s.vert) * Y_G;
            Y = sqrt(Y * std::max(Y, yAur));
            key = 0.18;
        }

        s.gate = on ? (F32)(night * clear_air * clear_air) : 0.f;
        s.time = (F32)t;
        s.color = mode;
        for (S32 i = 0; i < 24; ++i) s.band[i] = (F32)band[i];
        s.geo[0] = (F32)first; s.geo[1] = (F32)step; s.geo[2] = (F32)cos(bearing * DEG); s.geo[3] = (F32)sin(bearing * DEG);
        s.morph[0] = (F32)act; s.morph[1] = (F32)(tanM / R_E); s.morph[2] = (F32)(1.0 / (2.0 * tanM)); s.morph[3] = (F32)(2.0 / q[0]);
        for (S32 k = 0; k < 3; ++k) { s.arcPos[k] = (F32)arcs.pos[k]; s.arcW[k] = (F32)arcs.w[k]; }
        s.arcPos[3] = (F32)DIFFUSE; s.arcW[3] = eye ? 1.f : 0.f;
        s.eye[0] = (F32)Y; s.eye[1] = (F32)key;
        s.eye[2] = (F32)(gAgentCamera.getCameraPositionGlobal().mdV[VZ] / 1000.0);
        s.eye[3] = (F32)q[1];
        s.lut[0] = (F32)sup_min; s.lut[1] = (F32)sup_max; s.lut[2] = (F32)(s.frame % 4096); s.lut[3] = (F32)q[0];
        for (S32 c = 0; c < 3; ++c) s.main[c] = MAIN[mode][c];
        s.width = q[0];
        s.parts = q[2];
    }

    void setPassUniforms(LLGLSLShader& sh)
    {
        static LLStaticHashedString s_gate("wolf_aurora"), s_time("wolf_aurora_time"), s_color("wolf_aurora_color");
        static LLStaticHashedString s_band("wolf_aurora_band"), s_geo("wolf_aurora_geo"), s_morph("wolf_aurora_morph");
        static LLStaticHashedString s_pos("wolf_aurora_arc_pos"), s_w("wolf_aurora_arc_w"), s_eye("wolf_aurora_eye");
        static LLStaticHashedString s_vert("wolf_aurora_vert"), s_main("wolf_aurora_main"), s_lut("wolf_aurora_lut");
        const State& s = sState;
        sh.uniform1f(s_gate, s.gate);
        sh.uniform1f(s_time, s.time);
        sh.uniform1i(s_color, s.color);
        sh.uniform1fv(s_band, 24, s.band);
        sh.uniform4fv(s_geo, 1, s.geo);
        sh.uniform4fv(s_morph, 1, s.morph);
        sh.uniform4fv(s_pos, 1, s.arcPos);
        sh.uniform4fv(s_w, 1, s.arcW);
        sh.uniform4fv(s_eye, 1, s.eye);
        sh.uniform4fv(s_vert, NODES, s.vert);
        sh.uniform3fv(s_main, 1, s.main);
        sh.uniform4fv(s_lut, 1, s.lut);
    }
}

// static
void WolfAurora::frameUpdate()
{
    if (gCubeSnapshot) return;
    const U32 frame = LLFrameTimer::getFrameCount();
    if (frame == sState.frame) return;
    sState.frame = frame;
    computeState();
    if (sState.gate <= 0.f || !gWolfAuroraLUTProgram.isComplete())
    {
        sState.tableFresh = false;   // when it comes back, the table is cleared and refilled
        return;
    }
    const U32 w = (U32)sState.width, h = w;   // square: the stereographic hemisphere
    if (!sLut.isComplete() || sLut.getWidth() != w || sLut.getHeight() != h)
    {
        sLut.release();
        if (!sLut.allocate(w, h, GL_RGBA16F, false))
        {
            LL_WARNS("WolfAurora") << "the aurora's " << w << " x " << h << " table could not be allocated" << LL_ENDL;
            sState.gate = 0.f;
            return;
        }
        sState.tableFresh = false;
    }
    LLGLDepthTest depth(GL_FALSE, GL_FALSE);
    LLGLDisable blend(GL_BLEND);
    LLGLDisable cull(GL_CULL_FACE);
    LLGLDisable scissor(GL_SCISSOR_TEST);
    sLut.bindTarget();
    if (!sState.tableFresh)
    {
        // a new table, or the aurora back after a gap: start from nothing rather than march the whole
        // table in one frame (~10 ms at the top levels on an integrated GPU); it fills in `parts` frames
        GLfloat prev[4];
        glGetFloatv(GL_COLOR_CLEAR_VALUE, prev);
        glClearColor(0.f, 0.f, 0.f, 0.f);
        sLut.clear();
        glClearColor(prev[0], prev[1], prev[2], prev[3]);
        sState.part = 0;
        sState.sweep = 0;
    }
    // one strip of rows this frame (a whole pass over the table costs `parts` times this)
    const U32 parts = (U32)llmax(sState.parts, 1);
    const U32 part = sState.part % parts;
    const bool accumulate = sState.sweep > 0;   // the first sweep writes the table, the later ones blend
    if (++sState.part % parts == 0) ++sState.sweep;
    const U32 y0 = part * h / parts, y1 = (part + 1) * h / parts;
    glViewport(0, (GLint)y0, (GLsizei)w, (GLsizei)(y1 - y0));
    // After the first sweep, blend the jittered march into what is there: new x ACCUMULATE + old x
    // (1 - ACCUMULATE) (wolfAuroraF.glsl: a moving average, as clouds accumulate theirs). The constant
    // blend factor is not in LLRender's set, so the GL state is set and put back exactly as found,
    // keeping LLRender's cache true.
    GLint src_rgb = GL_ONE, dst_rgb = GL_ZERO, src_a = GL_ONE, dst_a = GL_ZERO;
    GLfloat blend_col[4] = { 0.f, 0.f, 0.f, 0.f };
    GLboolean was_blend = GL_FALSE;
    if (accumulate)
    {
        was_blend = glIsEnabled(GL_BLEND);
        glGetIntegerv(GL_BLEND_SRC_RGB, &src_rgb); glGetIntegerv(GL_BLEND_DST_RGB, &dst_rgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &src_a); glGetIntegerv(GL_BLEND_DST_ALPHA, &dst_a);
        glGetFloatv(GL_BLEND_COLOR, blend_col);
        glEnable(GL_BLEND);
        glBlendColor(0.f, 0.f, 0.f, ACCUMULATE);
        glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA);
    }
    gWolfAuroraLUTProgram.bind();
    setPassUniforms(gWolfAuroraLUTProgram);
    gPipeline.mScreenTriangleVB->setBuffer();
    gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
    gWolfAuroraLUTProgram.unbind();
    if (accumulate)
    {
        glBlendFuncSeparate(src_rgb, dst_rgb, src_a, dst_a);
        glBlendColor(blend_col[0], blend_col[1], blend_col[2], blend_col[3]);
        if (!was_blend) glDisable(GL_BLEND);
    }
    sLut.flush();
    sState.tableFresh = true;
}

// static
F32 WolfAurora::gate()
{
    return gCubeSnapshot ? 0.f : sState.gate;
}

// static
void WolfAurora::bindUniforms(LLGLSLShader* shader)
{
    if (!shader) return;
    static LLStaticHashedString s_gate("wolf_aurora"), s_w("wolf_aurora_arc_w"), s_eye("wolf_aurora_eye");
    const bool on = gate() > 0.f && sLut.isComplete() && sState.tableFresh;
    shader->uniform1f(s_gate, on ? sState.gate : 0.f);
    shader->uniform4fv(s_w, 1, sState.arcW);
    shader->uniform4fv(s_eye, 1, sState.eye);
    if (on) shader->bindTexture(LLShaderMgr::WOLF_AURORA_LUT, &sLut);
}

// static
bool WolfAurora::forced()
{
    static LLCachedControl<bool> force(gSavedSettings, "WolfForceAurora", false);
    return force;
}

// static
void WolfAurora::idle()
{
    // Paul 2026-10-15: "if force aurora is on force the viewer to night so we can see it". The same calls as the World >
    // Environment menu (llviewermenu.cpp:12358-12374, "midnight" and "region"), made once each time the switch changes
    // (and once after login when it was left on), never every frame - so the person can still pick another sky meanwhile.
    static bool s_applied = false;
    if (LLStartUp::getStartupState() < STATE_STARTED || !gAgent.getRegion()) return;
    const bool force = forced();
    if (force == s_applied) return;
    s_applied = force;
    if (force)
    {
        LLEnvironment::instance().setManualEnvironment(LLEnvironment::ENV_LOCAL, LLEnvironment::KNOWN_SKY_MIDNIGHT);
        LLEnvironment::instance().setSelectedEnvironment(LLEnvironment::ENV_LOCAL);
        LL_INFOS("WolfAurora") << "WolfForceAurora on: this viewer's sky set to Midnight" << LL_ENDL;
    }
    else
    {
        gPipeline.mReflectionMapManager.reset();   // as the menu's "region" (shared environment)
        LLEnvironment::instance().clearEnvironment(LLEnvironment::ENV_LOCAL);
        LLEnvironment::instance().setSelectedEnvironment(LLEnvironment::ENV_LOCAL, LLEnvironment::TRANSITION_INSTANT);
        LL_INFOS("WolfAurora") << "WolfForceAurora off: back to the shared sky" << LL_ENDL;
    }
}

// static
void WolfAurora::releaseGL()
{
    sLut.release();
    sState.tableFresh = false;
}
