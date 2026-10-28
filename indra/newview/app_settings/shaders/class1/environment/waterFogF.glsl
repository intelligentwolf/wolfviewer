/**
 * @file class1\environment\waterFogF.glsl
 *
 * $LicenseInfo:firstyear=2007&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2007, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */



uniform vec4 waterPlane;
uniform vec4 waterFogColor;
uniform float wolf_phys;
uniform vec3  wolf_light_h;
uniform float waterFogDensity;
uniform float waterFogKS;

vec3 srgb_to_linear(vec3 col);
vec3 linear_to_srgb(vec3 col);

vec3 atmosFragLighting(vec3 light, vec3 additive, vec3 atten);

// <WolfViewer 2026-10-28> [WOLF SEA TUNNEL] Ticket 319 (Malachi Coltrane): "An underwater tunnel device that clears the
// water murkiness ... for vehicle and train traffic". AIR-FILLED VOLUMES (wolfdryvolumes.cpp): a prim with "wolfdry" in
// its description, and the Tunnel tool's walkable boxes, hold air, not water. The water fog below is the integral over
// the line of sight's path through WATER (the "thickness" l), so the stretch of that path inside these volumes is taken
// out of it: from inside a sea tunnel its own inside is clear and the sea beyond the glass is fogged by the water out
// there only; from the sea a tunnel's inside is fogged by the water in front of it only.
// wolfDry[i*4 + 0..2]: the rows of the map from eye space to the volume's unit space (x, y, z in -1..1 at its faces);
// wolfDry[i*4 + 3].x: 1 = cylinder round x/y along z, 2 = box, 3 = sphere, 4 = a boat's hull (below). wolfDryCount = 0: none, and no cost but this test.
uniform int wolfDryCount;
uniform vec4 wolfDry[32];   // WolfDryVolumes::MAX_SHADER_VOLUMES (8) x 4

// <WolfViewer 2026-10-28> [WOLF DRY HULLS] Paul: "cant the viewer just detect that its a boat like we do for waves and
// not draw the water?" Kind 4 = a BOAT'S HULL (wolfdryvolumes.cpp measureHull): the boats WolfBoatRock finds are measured
// from their own visible geometry, so the inside of any hull is air with no tubes to build. Unit space: x along the
// hull (-1 stern end .. 1 bow end), y across (1 = the widest half-beam), z keel (-1) to 3 m above the waterline (1).
// The hull's half-width at (x, z) is a table: WOLF_HULL_STATIONS stations along x at WOLF_HULL_LEVELS heights
// (waterline + 1 m, waterline, and a third and two thirds of the way down to the keel), linear between them, vertical
// above the top level and closing to nothing at the keel. Fitted so it never reaches past the hull's outer skin at a
// level. wolfDry[i*4 + 3].y: which hull (0 .. WolfDryVolumes::MAX_SHADER_HULLS - 1).
// wolfHull[h*25 + 0..23]: the 4 x 24 half-widths (level-major, station-minor, 4 to a vec4); wolfHull[h*25 + 24]: the
// four levels' unit z, top first.
#define WOLF_HULL_STATIONS 24
#define WOLF_HULL_LEVELS 4
#define WOLF_HULL_VEC4 25
uniform vec4 wolfHull[100];   // WolfDryVolumes::MAX_SHADER_HULLS (4) x WOLF_HULL_VEC4

float wolfHullValue(int base, int n)
{
    vec4 q = wolfHull[base + n / 4];
    int c = n - (n / 4) * 4;
    return c == 0 ? q.x : (c == 1 ? q.y : (c == 2 ? q.z : q.w));
}

// The hull's half-width (unit y) at unit (x, z); -1 outside its length or height.
float wolfHullHalf(int h, float x, float z)
{
    if (abs(x) > 1.0 || abs(z) > 1.0)
    {
        return -1.0;
    }
    int base = h * WOLF_HULL_VEC4;
    vec4 lv = wolfHull[base + WOLF_HULL_VEC4 - 1];
    float s = (x + 1.0) * 0.5 * float(WOLF_HULL_STATIONS - 1);
    int i0 = int(min(floor(s), float(WOLF_HULL_STATIONS - 2)));
    float f = s - float(i0);
    if (z >= lv.x)
    {
        return mix(wolfHullValue(base, i0), wolfHullValue(base, i0 + 1), f);
    }
    // between two levels: the one above (k) and the one below (k + 1); below the last one, down to nothing at the keel
    int k = z >= lv.y ? 0 : (z >= lv.z ? 1 : (z >= lv.w ? 2 : 3));
    float za = k == 0 ? lv.x : (k == 1 ? lv.y : (k == 2 ? lv.z : lv.w));
    float zb = k == 0 ? lv.y : (k == 1 ? lv.z : (k == 2 ? lv.w : -1.0));
    int na = k * WOLF_HULL_STATIONS + i0;
    float ha = mix(wolfHullValue(base, na), wolfHullValue(base, na + 1), f);
    float hb = 0.0;
    if (k < 3)
    {
        int nb = na + WOLF_HULL_STATIONS;
        hb = mix(wolfHullValue(base, nb), wolfHullValue(base, nb + 1), f);
    }
    return mix(hb, ha, clamp((z - zb) / max(za - zb, 1e-6), 0.0, 1.0));
}

bool wolfInHull(int h, vec3 u)
{
    return abs(u.y) <= wolfHullHalf(h, u.x, u.z);
}
// </WolfViewer>

// The range t of a line o + s d (unit space) cut to where |o + s d| <= 1 on one axis; t.y <= t.x: none.
vec2 wolfDrySlab(float o, float d, vec2 t)
{
    if (abs(d) < 1e-6)
    {
        return abs(o) <= 1.0 ? t : vec2(1.0, 0.0);
    }
    float ta = (-1.0 - o) / d;
    float tb = (1.0 - o) / d;
    return vec2(max(t.x, min(ta, tb)), min(t.y, max(ta, tb)));
}

// The same, cut to where length(o + s d) <= 1 (a unit circle when o.z = d.z = 0, else a unit sphere).
vec2 wolfDryRound(vec3 o, vec3 d, vec2 t)
{
    float A = dot(d, d);
    float B = dot(o, d);
    float C = dot(o, o) - 1.0;
    if (A < 1e-10)
    {
        return C <= 0.0 ? t : vec2(1.0, 0.0);
    }
    float disc = B * B - A * C;
    if (disc < 0.0)
    {
        return vec2(1.0, 0.0);
    }
    float s = sqrt(disc);
    return vec2(max(t.x, (-B - s) / A), min(t.y, (-B + s) / A));
}

// The part of the segment a + s (b - a), s in 0..1 (eye space), inside volume i; t.y <= t.x: none.
vec2 wolfDryClip(int i, vec3 a, vec3 b)
{
    vec4 r0 = wolfDry[i * 4];
    vec4 r1 = wolfDry[i * 4 + 1];
    vec4 r2 = wolfDry[i * 4 + 2];
    float kind = wolfDry[i * 4 + 3].x;
    vec3 ab = b - a;
    vec3 o = vec3(dot(r0.xyz, a) + r0.w, dot(r1.xyz, a) + r1.w, dot(r2.xyz, a) + r2.w);
    vec3 d = vec3(dot(r0.xyz, ab), dot(r1.xyz, ab), dot(r2.xyz, ab));
    vec2 t = vec2(0.0, 1.0);
    if (kind > 3.5)
    {
        // [WOLF DRY HULLS] a hull: its unit box here, the part of that inside the hull in wolfDryLength
        t = wolfDrySlab(o.z, d.z, t);
        t = wolfDrySlab(o.x, d.x, t);
        return wolfDrySlab(o.y, d.y, t);
    }
    if (kind > 2.5)
    {
        return wolfDryRound(o, d, t);
    }
    t = wolfDrySlab(o.z, d.z, t);
    if (kind > 1.5)
    {
        t = wolfDrySlab(o.x, d.x, t);
        return wolfDrySlab(o.y, d.y, t);
    }
    return wolfDryRound(vec3(o.xy, 0.0), vec3(d.xy, 0.0), t);
}

// Metres of the segment a -> b (eye space) inside air-filled volumes (pieces that overlap at a join count the overlap
// twice; capped at the whole segment).
float wolfDryLength(vec3 a, vec3 b)
{
    float inside = 0.0;
    for (int i = 0; i < wolfDryCount; ++i)
    {
        vec2 t = wolfDryClip(i, a, b);
        if (t.y <= t.x)
        {
            continue;
        }
        if (wolfDry[i * 4 + 3].x > 3.5)
        {
            // [WOLF DRY HULLS] the share of the stretch through the hull's box that is inside the hull, from 8 points
            // along it (a camera in the hold sees all of them inside; one in the sea looking at the hull none)
            int h = int(wolfDry[i * 4 + 3].y + 0.5);
            vec4 r0 = wolfDry[i * 4];
            vec4 r1 = wolfDry[i * 4 + 1];
            vec4 r2 = wolfDry[i * 4 + 2];
            float hits = 0.0;
            for (int k = 0; k < 8; ++k)
            {
                vec3 p = mix(a, b, mix(t.x, t.y, (float(k) + 0.5) / 8.0));
                vec3 u = vec3(dot(r0.xyz, p) + r0.w, dot(r1.xyz, p) + r1.w, dot(r2.xyz, p) + r2.w);
                hits += wolfInHull(h, u) ? 1.0 : 0.0;
            }
            inside += (t.y - t.x) * hits / 8.0;
            continue;
        }
        inside += t.y - t.x;
    }
    return min(inside, 1.0) * length(b - a);
}

// Is this eye-space point inside an air-filled volume? (waterF.glsl / underWaterF.glsl: no water surface there.)
bool wolfInDry(vec3 p)
{
    for (int i = 0; i < wolfDryCount; ++i)
    {
        vec4 r0 = wolfDry[i * 4];
        vec4 r1 = wolfDry[i * 4 + 1];
        vec4 r2 = wolfDry[i * 4 + 2];
        float kind = wolfDry[i * 4 + 3].x;
        vec3 u = vec3(dot(r0.xyz, p) + r0.w, dot(r1.xyz, p) + r1.w, dot(r2.xyz, p) + r2.w);
        bool inside = kind > 3.5 ? wolfInHull(int(wolfDry[i * 4 + 3].y + 0.5), u)   // [WOLF DRY HULLS/]
                    : kind > 2.5 ? dot(u, u) <= 1.0
                    : (abs(u.z) <= 1.0 && (kind > 1.5 ? (abs(u.x) <= 1.0 && abs(u.y) <= 1.0) : dot(u.xy, u.xy) <= 1.0));
        if (inside)
        {
            return true;
        }
    }
    return false;
}
// </WolfViewer>

// get a water fog color that will apply the appropriate haze to a color given
// a blend function of (ONE, SOURCE_ALPHA)
vec4 getWaterFogViewNoClip(vec3 pos)
{
    vec3 view = normalize(pos);
    //normalize view vector
    float es = -(dot(view, waterPlane.xyz));

    //find intersection point with water plane and eye vector

    //get eye depth
    float e0 = max(-waterPlane.w, 0.0);

    vec3 int_v = waterPlane.w > 0.0 ? view * waterPlane.w/es : vec3(0.0, 0.0, 0.0);

    //get object depth
    float depth = length(pos - int_v);

    // <WolfViewer 2026-10-28> [WOLF SEA TUNNEL] no water along the stretch inside air-filled volumes (wolfDryLength)
    if (wolfDryCount > 0)
    {
        depth -= wolfDryLength(int_v, pos);
        if (depth <= 0.0)
        {
            return vec4(0.0, 0.0, 0.0, 1.0);   // all air: no fog at all (the 0.1 m floor below would leave a tint)
        }
    }
    // </WolfViewer>

    //get "thickness" of water
    float l = max(depth, 0.1);

    float kd = waterFogDensity;
    float ks = waterFogKS;
    vec4 kc = waterFogColor;

    float F = 0.98;

    float t1 = -kd * pow(F, ks * e0);
    float t2 = kd + ks * es;
    float t3 = pow(F, t2*l) - 1.0;

    float L = pow(min(t1/t2*t3, 1.0), 1.0/1.7);

    float D = pow(0.98, l*kd);

    // [WOLF ENV 2026-10-12 physical] the fog colour is the water body's colour: under the physical sky it is
    // lit by the light on the water (wolf_light_h, the horizontal illuminance / pi, pre-exposed); unlit it
    // was a constant the exposure did not scale (grey water, and a term the metering read back)
    vec3 body = srgb_to_linear(kc.rgb) * (wolf_phys > 0.5 ? wolf_light_h : vec3(1.0));
    return vec4(body*L, D);
}

vec4 getWaterFogView(vec3 pos)
{
    if (dot(pos, waterPlane.xyz) + waterPlane.w > 0.0)
    {
        return vec4(0,0,0,1);
    }

    return getWaterFogViewNoClip(pos);
}

vec4 applyWaterFogView(vec3 pos, vec4 color)
{
    vec4 fogged = getWaterFogView(pos);

    color.rgb = color.rgb * fogged.a + fogged.rgb;

    return color;
}

vec4 applyWaterFogViewLinearNoClip(vec3 pos, vec4 color)
{
    vec4 fogged = getWaterFogViewNoClip(pos);
    color.rgb *= fogged.a;
    color.rgb += fogged.rgb;
    return color;
}

vec4 applyWaterFogViewLinear(vec3 pos, vec4 color)
{
    if (dot(pos, waterPlane.xyz) + waterPlane.w > 0.0)
    {
        return color;
    }

    return applyWaterFogViewLinearNoClip(pos, color);
}

// for post deferred shaders, apply sky and water fog in a way that is consistent with
// the deferred rendering haze post effects
vec4 applySkyAndWaterFog(vec3 pos, vec3 additive, vec3 atten, vec4 color)
{
    bool eye_above_water = dot(vec3(0), waterPlane.xyz) + waterPlane.w > 0.0;
    bool obj_above_water = dot(pos.xyz, waterPlane.xyz) + waterPlane.w > 0.0;

    if (eye_above_water)
    {
        if (!obj_above_water)
        {
            color.rgb = applyWaterFogViewLinearNoClip(pos, color).rgb;
        }
        else
        {
            color.rgb = atmosFragLighting(color.rgb, additive, atten);
        }
    }
    else
    {
        if (obj_above_water)
        {
            color.rgb = atmosFragLighting(color.rgb, additive, atten);
        }
        else
        {
            color.rgb = applyWaterFogViewLinearNoClip(pos, color).rgb;
        }
    }

    return color;
}
