/**
 * @file wolfAuroraF.glsl
 * @brief WolfViewer: the northern (and southern) lights, a physically based emission volume.
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

// [WOLF AURORA 2026-10-12] Paul: "can you make the northern lights look more like RL" - "100% no
// shortcuts no fakes". The design, every source and every number: region-terrain-work/wolf-aurora/
// DESIGN.md (firestorm repo); the constant tables come out of make_tables.py next to it.
// WolfStorm js/rendering/wolf_aurora.js carries this same shader - keep the two in step.
//
// What is drawn: light EMITTED by the upper atmosphere, integrated along each view ray through the
// emitting shell 80-400 km up (Lawlor & Genetti 2011, "Interactive Volume Rendering Aurora on the
// GPU", J. WSCG 19: the aurora factors into a 2D precipitation footprint times a height-dependent
// deposition function, and is purely emissive). Per sample:
//   volume emission (kR per km) = local energy flux (erg cm-2 s-1) x the group's vertical profile
//   (kR per km per erg cm-2 s-1, wolf_aurora_vert: Fang et al. 2008 ionisation for a Maxwellian of
//   characteristic energy E0 in the U.S. Standard Atmosphere 1976, the forbidden lines quenched,
//   column yields of Barthelemy et al. 2025 table 2).
// Four emission groups: G O I 557.7 nm, R O I 630.0/636.4 nm, B N2+ 1N 427.8 + 391.4 nm, P the
// visible N2 1PG bands. The column in kR along the ray is converted to luminance with the CIE 1931
// colour-matching functions (1 R = 10^6/(4 pi) photons s-1 cm-2 sr-1, Hunten et al. 1956).
// The footprint: the auroral band along the magnetic meridian (wolf_aurora_band: NOAA SWPC OVATION
// at the region's real place, or the owner's amount) carrying three discrete arcs (wolf_aurora_arc_*)
// and a diffuse part. Structure is evaluated at the FIELD-LINE FOOTPOINT, so curtains, folds and
// rays lean along the geomagnetic field (dipole dip, tan I = 2 tan(magnetic latitude), Laundal &
// Richmond 2017 eq. 22) and converge on the magnetic zenith overhead: the corona is perspective.
// Thin sheets are integrated analytically across each ray segment, widened by the footprint of a
// table texel; the folds are filtered to the segment's length (an octave fades out once a step is
// longer than its wavelength: I. Quilez, "filtering procedurals",
// https://iquilezles.org/articles/filteringrm/ and /articles/morenoise/ - frequency clamping), the
// step altitudes are jittered per texel and per pass (interleaved gradient noise, Jimenez 2014,
// "Next Generation Post Processing in Call of Duty: Advanced Warfare", SIGGRAPH Advances) and the
// passes are averaged in the table (an exponential moving average by blending, as volumetric clouds
// accumulate a jittered march: Schneider 2015, Hillaire 2016 section 5.5.3), so no step pattern is
// stamped into the curtains.
//
// The march is too dear to run per screen pixel (~13 ns a ray on a Radeon 890M at 24 steps, DESIGN.md),
// so it fills a direction table, as Hillaire 2020's sky-view LUT does for the sky, a strip of its rows
// a frame (WOLF_AURORA_LUT_PASS, WolfAurora::frameUpdate), and the sky shaders read the table
// (wolfAuroraRadiance / wolfAuroraDisplay below). The table is the upper hemisphere's stereographic
// projection, p = dir.xy / (1 + dir.z): no trigonometry to read it, no seam, and twice the texels
// per degree at the horizon (where distant curtains are thinnest) as at the zenith.

uniform float wolf_aurora;          // 0..1: night (sun -6 .. -12 deg, civil to nautical twilight; round 13) x the fog (WolfAurora); 0 = none
uniform float wolf_aurora_time;     // s, UTC seconds of the day: every viewer sees the same curtains
uniform int   wolf_aurora_color;    // 0 green (natural) 1 red 2 purple 3 blue 4 pink 5 multi 6 real (natural)
uniform vec4  wolf_aurora_eye;      // x adapting luminance cd/m2, y key (EEP display), z camera km, w ray steps
uniform vec4  wolf_aurora_arc_w;    // xyz: each arc's flux x latitude width, erg cm-2 s-1 km; w: 1 = as the eye sees it, 0 = as a photograph

#define WA_LUT_SCALE 1.0e4           // the table holds cd/m2 x this (fp16: 1e-5 cd/m2 stays a normal number)
const vec3 WA_LUMA = vec3(0.2126, 0.7152, 0.0722);   // IEC 61966-2-1 Y row

// the table's mapping: the stereographic projection of the upper hemisphere, uv = 0.5 + 0.5 p,
// p = dir.xy / (1 + dir.z) (the horizon is the unit circle); its inverse
vec3 waLutDir(vec2 uv)
{
    vec2 p = uv * 2.0 - 1.0;
    float r2 = dot(p, p);
    return vec3(2.0 * p, 1.0 - r2) / (1.0 + r2);
}

#ifdef WOLF_AURORA_LUT_PASS

out vec4 frag_color;

uniform float wolf_aurora_band[24]; // energy flux, erg cm-2 s-1, at footpoint distances first + i x step (poleward +)
uniform vec4  wolf_aurora_geo;      // x first sample km, y spacing km, z cos, w sin of the magnetic-north bearing
uniform vec4  wolf_aurora_morph;    // x activity 0..1, y oval curvature 1/km, z cot(dip), w 2 / the table's size (a texel's angle at the horizon, rad)
uniform vec4  wolf_aurora_arc_pos;  // xyz: the three arcs' footpoint distances km; w: diffuse fraction
uniform vec4  wolf_aurora_vert[64]; // (G, R, B, P) kR per km per erg cm-2 s-1 at h_i = 80 + 320 (i/63)^2 km
uniform vec3  wolf_aurora_main;     // the owner's colour for the main (green line) band, linear, Y = 1; 0 = natural
uniform vec4  wolf_aurora_lut;      // x, y: the band's footpoint extent km; z: the pass number (the jitter); w: the table's size

#define WA_R      6371.0             // km, mean Earth radius (as skyF.glsl's sea)
#define WA_H0     80.0               // km, the shell's bottom (Pfaff 2012 section 5.1: 80-250 km)
#define WA_HSPAN  320.0              // km, to 400 km (UAF GI "Rare Red Aurora": all-red aurora 300-500 km)
#define WA_HREF   110.0              // km, the altitude the footprint is defined at
// sheet widths (Gaussian sigma, km): arcs' typical width 18 +- 9 km (Knudsen et al. 2001, in
// Partamies et al. 2010 section 1) -> envelope FWHM 18 km; their fine structure 0.5-1.5 km thick
// (Partamies et al. 2010 section 4) -> filament FWHM 1 km; the red line lives 110 s (Barthelemy et
// al. 2025 table 1) while an arc drifts ~100 m/s (Williams et al. 1998 table 2) -> 11 km of smear.
#define WA_SIG_ENV 7.644
#define WA_SIG_FIL 0.4247
#define WA_SIG_SMEAR 11.0
// the share of an arc's light in its fine sheet (the rest in the 18 km envelope): the design's
// (DESIGN.md), so that arcs read as the thin draped sheets of the photographs
#define WA_W_FIL 0.75

// linear sRGB (IEC 61966-2-1) luminance in cd/m2 of 1 kR of each group, and its scotopic luminance
// (CIE 1931 2 deg CMFs, CIE scotopic V', 683 / 1700 lm/W): make_tables.py
const vec3  WA_RGB_G = vec3(5.13413e-05, 2.58338e-04, -3.24898e-05);
const vec3  WA_RGB_R = vec3(2.72957e-04, -2.09955e-05, -2.90101e-06);
const vec3  WA_RGB_B = vec3(5.21214e-05, -4.76286e-05, 3.53276e-04);
const vec3  WA_RGB_P = vec3(2.52825e-05, 1.41068e-06, -6.76254e-07);
const float WA_Y_G = 1.93324e-04;
const vec4  WA_SCOT = vec4(1.74084e-04, 1.28107e-06, 1.14271e-04, 1.68167e-06);

// "Hash without Sine" (Dave Hoskins, MIT licence, https://www.shadertoy.com/view/4djSRW): exact on
// every GPU, unlike fract(sin()).
float waHash(float p) { p = fract(p * 0.1031); p *= p + 33.33; p *= p + p; return fract(p); }
float waHash2(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}
float waNoise(float x)
{
    float i = floor(x), f = x - i;
    return mix(waHash(i), waHash(i + 1.0), f * f * (3.0 - 2.0 * f));
}

// the standard normal CDF, logistic approximation (Bowling et al. 2009, J. Industrial Engineering and
// Management 2(1):114-127: 1 / (1 + exp(-1.702 x)), max error 0.0095)
float waPhi(float x) { return 1.0 / (1.0 + exp(-1.702 * x)); }

// integral over a segment of length ds of a unit-area Gaussian (sigma) of d, d linear from da to db
float waSheet(float da, float db, float sigma, float ds)
{
    float dd = db - da;
    if (abs(dd) < 1e-3 * sigma)
    {
        float m = 0.5 * (da + db) / sigma;
        return ds * exp(-0.5 * m * m) / (sigma * 2.5066283);
    }
    return ds * (waPhi(db / sigma) - waPhi(da / sigma)) / dd;
}

vec4 waVert(float h)
{
    float x = sqrt(clamp((h - WA_H0) / WA_HSPAN, 0.0, 1.0)) * 63.0;
    int i = int(min(floor(x), 62.0));
    return mix(wolf_aurora_vert[i], wolf_aurora_vert[i + 1], x - float(i));
}

float waBand(float v)
{
    float x = (v - wolf_aurora_geo.x) / wolf_aurora_geo.y;
    if (x <= 0.0 || x >= 23.0) return 0.0;
    int i = int(floor(x));
    return mix(wolf_aurora_band[i], wolf_aurora_band[i + 1], x - float(i));
}

// An arc's footprint displacement (km, poleward +) at magnetic-east distance u: meanders ~150 km long
// (mesoscale curls ~100 km, Zhang et al. 2024 GRL; spirals 20-1300 km, Keiling et al. 2009) and
// folds ~10 km (Kataoka et al. 2021 section 2.2), both travelling along the arc at 0.5-3 km/s
// (westward travelling surge 2.2 km/s average, Craven et al. 1989; spiral hooks up to 3 km/s).
// Amplitudes grow with activity: smooth quiet arcs, folded and fragmented active ones (Lawlor &
// Genetti 2011 section 1, after Akasofu 1964). The amplitudes are the design's (DESIGN.md).
// fu: the length of the ray step along the arc (km), the filter width.
float waFold(float u, float k, float act, float t, float fu)
{
    float m = waNoise(u / 150.0 + k * 13.17 - t * mix(0.2, 2.0, act) / 150.0) * 2.0 - 1.0;
    float f = waNoise(u / 10.0 + k * 7.73 + t * mix(0.5, 3.0, act) / 10.0) * 2.0 - 1.0;
    return m * mix(3.0, 40.0, act) * exp(-fu * fu / (150.0 * 150.0)) + f * mix(0.3, 4.0, act) * exp(-fu * fu / 100.0);
}

// Rays: field-aligned striations of km scale (curls ~1 km, Trondsen & Cogger 1998; vortices seen
// side-on are the rays, Kataoka et al. 2021 section 2.2), averaged away once a texel covers them.
float waRays(float u, float k, float act, float t, float footprint)
{
    // each octave fades out as a texel's footprint reaches its scale (2 km, 0.7 km)
    float f2 = footprint * footprint;
    float n = (waNoise(u / 2.0 + k * 3.31 - t * mix(0.5, 3.0, act) / 2.0) * 2.0 - 1.0) * 0.6 * exp(-f2 / 4.0)
            + (waNoise(u / 0.7 + k * 5.71 + t * 1.3) * 2.0 - 1.0) * 0.4 * exp(-f2 / 0.49);
    return 1.0 + mix(0.15, 0.85, act) * n;
}

// value noise in 2D (smooth, so patches have no cell edges)
float waNoise2(vec2 x)
{
    vec2 i = floor(x), f = x - i;
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(waHash2(i), waHash2(i + vec2(1.0, 0.0)), f.x),
               mix(waHash2(i + vec2(0.0, 1.0)), waHash2(i + vec2(1.0, 1.0)), f.x), f.y);
}

// Pulsating aurora in the diffuse part: patches (15-25 km, "one to hundreds of kilometres", Grono &
// Donovan 2020 section 1) brightening and fading with periods 5-40 s, mean 10 s (Tagirov et al. 1999
// section 1; one period, 10 s, with a phase that changes smoothly from patch to patch), drifting
// ~250 m/s (Humberset et al. 2018). Mean 1, so the band's energy flux is kept.
float waPulse(float u, float v, float act, float t)
{
    vec2 p = vec2(u - 0.25 * t, v);
    float spot = smoothstep(0.45, 0.7, waNoise2(p / 40.0));
    float phase = 3.0 * waNoise2(p / 120.0 + vec2(17.3, 5.1));
    float on = smoothstep(-0.4, 0.4, sin(6.2831853 * (t / 10.0 + phase)));   // averages 0.5 over a period
    return 1.0 + smoothstep(0.2, 0.5, act) * 1.2 * spot * (on - 0.5);
}

vec3 waPalette(int i)
{
    // the owner's 'multi' colours (the old WolfStorm wsAuroraPalette, linearised, Y = 1)
    if (i == 0) return vec3(4.33443, 0.09527, 0.14349);   // red
    if (i == 1) return vec3(2.06969, 0.23987, 5.37991);   // purple
    if (i == 2) return vec3(0.13604, 0.85309, 4.99924);   // blue
    if (i == 3) return vec3(3.41973, 0.25046, 1.29969);   // pink
    return vec3(0.02690, 1.37181, 0.18227);               // green
}

// Out of the sRGB gamut (a monochromatic line is): keep the luminance and the hue, give up chroma
// (B. Ottosson 2021, "sRGB gamut clipping", https://bottosson.github.io/posts/gamutclipping/: keep
// lightness constant, compress chroma toward the grey axis).
vec3 waGamut(vec3 c)
{
    float y = dot(c, WA_LUMA);
    float m = min(c.r, min(c.g, c.b));
    if (m < 0.0 && y > 0.0) c = vec3(y) + (c - vec3(y)) * (y / (y - m));
    return max(c, vec3(0.0));
}

// one arc's light over one ray segment
void waArc(inout vec4 e, inout vec3 rgbMain, float da, float db, float w, float k, float ha, float hb,
           float ua, float ub, float ds, float foot, float act, float t, bool fantasy)
{
    float sr = sqrt(WA_SIG_ENV * WA_SIG_ENV + WA_SIG_SMEAR * WA_SIG_SMEAR + foot * foot);
    if (w <= 0.0 || (min(abs(da), abs(db)) > 4.0 * sr && da * db > 0.0)) return;
    float sf = sqrt(WA_SIG_FIL * WA_SIG_FIL + foot * foot);
    float sv = sqrt(WA_SIG_ENV * WA_SIG_ENV + foot * foot);
    float tc = clamp(da / (da - db + 1e-9), 0.0, 1.0);    // where the ray crosses the sheet
    float ux = mix(ua, ub, tc);
    vec4 vv = waVert(mix(ha, hb, tc));
    float sheet = ((1.0 - WA_W_FIL) * waSheet(da, db, sv, ds) + WA_W_FIL * waSheet(da, db, sf, ds)) * waRays(ux, k, act, t, foot);
    vec4 c = w * vv * vec4(sheet, waSheet(da, db, sr, ds), sheet, sheet);
    if (fantasy)
    {
        vec3 mc = wolf_aurora_main;
        if (wolf_aurora_color == 5)
        {
            // a hue per stretch of curtain, blended across the joins
            float hsel = waNoise(ux / 400.0 + k * 1.7 + t * 0.0005) * 4.0;
            int hi = int(floor(hsel));
            mc = mix(waPalette(hi), waPalette(min(hi + 1, 4)), smoothstep(0.25, 0.75, hsel - float(hi)));
        }
        rgbMain += mc * (c.x * WA_Y_G);
        c.x = 0.0;
    }
    e += c;
}

/**
 * The aurora's light along the view ray, emitted in the upper atmosphere and NOT yet dimmed by the air
 * below it: linear sRGB, luminance in cd/m2 (not x wolf_aurora). scotopic = its scotopic luminance.
 * dir: unit view direction, (east, north, up). Above the horizon only.
 */
vec3 waMarch(vec3 dir, out float scotopic)
{
    scotopic = 0.0;
    if (dir.z <= 0.0) return vec3(0.0);
    float hc = wolf_aurora_eye.z;
    float rc = WA_R + hc;
    float se = dir.z, ce = sqrt(max(1.0 - se * se, 0.0));
    vec2 hd = ce > 1e-5 ? dir.xy / ce : vec2(0.0, 1.0);
    float hu = hd.x * wolf_aurora_geo.z - hd.y * wolf_aurora_geo.w;   // toward magnetic east
    float hv = hd.x * wolf_aurora_geo.w + hd.y * wolf_aurora_geo.z;   // toward the magnetic pole
    float act = wolf_aurora_morph.x, kap = wolf_aurora_morph.y, cotI = wolf_aurora_morph.z;
    float t = wolf_aurora_time;
    float b = rc * se;

    // the whole ray misses the band (and anything an arc can fold to): nothing to march
    float s0 = -b + sqrt(max(b * b + (WA_R + WA_H0) * (WA_R + WA_H0) - rc * rc, 0.0));
    float s1 = -b + sqrt(max(b * b + (WA_R + WA_H0 + WA_HSPAN) * (WA_R + WA_H0 + WA_HSPAN) - rc * rc, 0.0));
    float d0 = WA_R * atan(s0 * ce, rc + s0 * se), d1 = WA_R * atan(s1 * ce, rc + s1 * se);
    float va = d0 * hv + (WA_H0 - WA_HREF) * cotI, vb = d1 * hv + (WA_H0 + WA_HSPAN - WA_HREF) * cotI;
    float bend = 0.5 * kap * (d1 * hu) * (d1 * hu);
    if (max(va, vb) < wolf_aurora_lut.x - 120.0 || min(va, vb) - bend > wolf_aurora_lut.y + 120.0) return vec3(0.0);

    float diffuse = wolf_aurora_arc_pos.w;
    bool fantasy = dot(wolf_aurora_main, wolf_aurora_main) > 0.0;
    vec4 e = vec4(0.0);           // kR per group (natural)
    vec3 rgbMain = vec3(0.0);     // the owner's colour, cd/m2 (fantasy)
    float da0 = 0.0, da1 = 0.0, da2 = 0.0;
    float sa = 0.0, ha = 0.0, ua = 0.0, vprev = 0.0;
    int n = int(wolf_aurora_eye.w);
    // the step grid's offset for this texel and pass: interleaved gradient noise (Jimenez 2014), its
    // animated form (the pass number x 5.588238 added to the pixel)
    vec2 ig = gl_FragCoord.xy + 5.588238 * wolf_aurora_lut.z;
    float jit = fract(52.9829189 * fract(dot(ig, vec2(0.06711056, 0.00583715))));
    for (int j = 0; j <= 65; ++j)
    {
        if (j > n + 1) break;
        float x = j == 0 ? 0.0 : (j > n ? 1.0 : (float(j) - 1.0 + jit) / float(n));
        float h = WA_H0 + WA_HSPAN * x * x;                      // steps on the profile's own warp
        float rh = WA_R + h;
        float s = -b + sqrt(max(b * b + rh * rh - rc * rc, 0.0)); // km along the ray to altitude h
        float D = WA_R * atan(s * ce, rc + s * se);              // km along the ground
        float u = D * hu, v = D * hv;
        // the oval is a small circle round the pole: a magnetic latitude line bends poleward away
        // from the magnetic-east geodesic by u^2 cot(colatitude) / 2R (geodesic curvature)
        float v0 = v - 0.5 * kap * u * u + (h - WA_HREF) * cotI;
        float fu = abs(u - ua);
        float db0 = v0 - wolf_aurora_arc_pos.x - waFold(u, 0.0, act, t, fu);
        float db1 = v0 - wolf_aurora_arc_pos.y - waFold(u, 1.0, act, t, fu);
        float db2 = v0 - wolf_aurora_arc_pos.z - waFold(u, 2.0, act, t, fu);
        if (j > 0)
        {
            float ds = s - sa;
            float foot = wolf_aurora_morph.w * (1.0 + se) * 0.5 * (sa + s);  // km a table texel covers there (stereographic: 2/size x (1 + z) rad)
            // the diffuse part (no fine structure), pulsating; the red line does not pulse (110 s)
            float vm = 0.5 * (vprev + v0);
            float fl = diffuse * waBand(vm) * ds;
            if (fl > 0.0)
            {
                float p = waPulse(0.5 * (ua + u), vm, act, t);
                e += fl * waVert(0.5 * (ha + h)) * vec4(p, 1.0, p, p);
            }
            waArc(e, rgbMain, da0, db0, wolf_aurora_arc_w.x, 0.0, ha, h, ua, u, ds, foot, act, t, fantasy);
            waArc(e, rgbMain, da1, db1, wolf_aurora_arc_w.y, 1.0, ha, h, ua, u, ds, foot, act, t, fantasy);
            waArc(e, rgbMain, da2, db2, wolf_aurora_arc_w.z, 2.0, ha, h, ua, u, ds, foot, act, t, fantasy);
        }
        da0 = db0; da1 = db1; da2 = db2;
        sa = s; ha = h; ua = u; vprev = v0;
    }
    if (fantasy)
    {
        // the diffuse part's main band in the owner's colour too
        rgbMain += wolf_aurora_main * (e.x * WA_Y_G);
        e.x = 0.0;
    }
    // the owner's colours have no spectrum: the eye's rods are given them as white light of their
    // luminance (scotopic = photopic x S/P of D65), so they fade to grey as the natural ones do
    scotopic = dot(e, WA_SCOT) + dot(rgbMain, WA_LUMA) * 2.4645;
    return waGamut(rgbMain + WA_RGB_G * e.x + WA_RGB_R * e.y + WA_RGB_B * e.z + WA_RGB_P * e.w);
}

void main()
{
    // WolfAurora::frameUpdate draws a strip of the table's rows a frame (its viewport), the whole
    // table when it is new
    vec2 uv = gl_FragCoord.xy / wolf_aurora_lut.w;
    if (dot(uv * 2.0 - 1.0, uv * 2.0 - 1.0) > 1.0 + 4.0 / wolf_aurora_lut.w) { frag_color = vec4(0.0); return; }   // below the horizon
    vec3 dir = waLutDir(uv);
    float scot;
    vec3 rad = waMarch(dir, scot);
    frag_color = vec4(rad, scot) * WA_LUT_SCALE;
}

#else   // the sky shaders: read the table

uniform sampler2D wolfAuroraLUT;    // the march's table (cd/m2 x WA_LUT_SCALE; a = scotopic), WolfAurora::bindUniforms

// the stereographic table, bilinear (WolfAurora binds it filtered and clamped)
vec4 waLut(vec3 dir)
{
    vec2 uv = dir.xy / (1.0 + max(dir.z, 0.0)) * 0.5 + 0.5;
    return max(texture(wolfAuroraLUT, uv), vec4(0.0)) * (1.0 / WA_LUT_SCALE);
}

/**
 * The PHYSICAL entry point (the native Wolf sky, wolfEnvSkyF.glsl): the aurora's emitted luminance
 * toward dir, linear sRGB in cd/m2, before the air below it dims it (multiply by the atmosphere's
 * transmittance to space and the frame's pre-exposure). dir: unit, (east, north, up).
 */
vec3 wolfAuroraRadiance(vec3 dir, out float scotopic)
{
    scotopic = 0.0;
    if (wolf_aurora <= 0.0 || dir.z <= 0.0) return vec3(0.0);
    vec4 l = waLut(dir) * wolf_aurora;
    scotopic = l.a;
    return l.rgb;
}

vec3 wolfAuroraRadiance(vec3 dir)
{
    float s;
    return wolfAuroraRadiance(dir, s);
}

/**
 * The EEP sky's aurora (skyF.glsl): display-referred 0..1, for a sky that has no physical exposure.
 * The light is dimmed by the air below it (Krisciunas & Schaefer 1991 PASP 103:1033 eq. 3 airmass,
 * "appropriate for the night glow", k = 0.172 mag per airmass, their Mauna Kea V value), then shown
 * as an eye adapted to tonight's sky sees it: photographic tone reproduction Yr = key Y / Yadapt,
 * L = Yr / (1 + Yr) (Reinhard et al. 2002, Krawczyk et al. 2005 eq. 4.1-4.2), the key from the
 * adapting luminance (Krawczyk eq. 4.11; both from WolfAurora), and - for the natural colours - rod
 * vision: sigma = 0.04 / (0.04 + Y) of the light seen as a blue-shifted grey (Krawczyk eq. 4.7, 4.15,
 * Hunt 1995). The grey's brightness is the light's SCOTOPIC luminance on the photopic scale of white
 * light (/ S/P of D65, 2.4645, make_tables.py): rods barely see the 630 nm red, as the eye does not.
 * It can never exceed 1: it never blows out.
 */
vec3 wolfAuroraDisplay(vec3 dir)
{
    float scot;
    vec3 c = wolfAuroraRadiance(dir, scot);
    if (scot <= 0.0 && dot(c, c) <= 0.0) return vec3(0.0);
    float X = inversesqrt(max(1.0 - 0.96 * (1.0 - dir.z * dir.z), 1e-4));
    float tr = pow(10.0, -0.4 * 0.172 * X);
    c *= tr;
    scot *= tr;
    float y = dot(c, WA_LUMA);
    float ya = max(wolf_aurora_eye.x, 1e-6), key = wolf_aurora_eye.y;
    float yr = key * y / ya;
    float ld = yr / (1.0 + yr);                     // the displayed luminance, < 1
    vec3 col = y > 0.0 ? c * (ld / y) : vec3(0.0);
    if (wolf_aurora_arc_w.w > 0.5)
    {
        float sig = 0.04 / (0.04 + y);
        float sr = key * (scot / 2.4645) / ya;
        col = col * (1.0 - sig) + vec3(1.05, 0.97, 1.27) * (sr / (1.0 + sr)) * sig;
    }
    // A saturated colour at high luminance has a channel over 1: rather than clip it to a flat hue,
    // keep its luminance and give up chroma toward the grey of that luminance (Ottosson 2021, as
    // waGamut) - bright displays whiten, as an over-exposed photograph's highlights do.
    float yd = dot(col, WA_LUMA);
    float mx = max(col.r, max(col.g, col.b));
    if (mx > 1.0 && yd < 1.0) col = vec3(yd) + (col - vec3(yd)) * ((1.0 - yd) / (mx - yd));
    return clamp(col, vec3(0.0), vec3(1.0));
}

#endif
