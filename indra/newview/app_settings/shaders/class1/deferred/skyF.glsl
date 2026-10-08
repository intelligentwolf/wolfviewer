/**
 * @file class1/deferred/skyF.glsl
 *
 * $LicenseInfo:firstyear=2005&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2005, Linden Research, Inc.
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

// Inputs
in vec3 vary_HazeColor;
in float vary_LightNormPosDot;
in vec3 vary_wolf_sky_dir;         // <WolfViewer 2026-09-18/> skyV.glsl
uniform float wolf_aurora;         // 0..1: the profile's aurora x night x what the fog lets through (WolfWeather::auroraAmount)
uniform float wolf_aurora_time;
uniform int   wolf_aurora_color;   // 0 green 1 red 2 purple 3 blue 4 pink 5 multi (WolfWeatherProfile::auroraColorMode)
// <WolfViewer 2026-10-07> the ocean to the true horizon (wolfHorizonSea below)
uniform float wolf_sea_height;     // camera metres above the sea; < 0 = off (WolfAltitudeSky::horizonSeaHeight)
uniform vec3  waterFogColorLinear; // the region's EEP water colour (LLSettingsVOWater, every shader group)
uniform vec3  blue_density;
uniform float haze_density;
uniform float density_multiplier;  // the sky group's: thinner with height (WolfAltitudeSky::airFraction)
uniform float distance_multiplier;
// the rest of skyV.glsl's inputs, for the sky the sea mirrors (wolfSkyColour)
uniform vec3  lightnorm;
uniform vec3  sunlight_color;
uniform vec3  moonlight_color;
uniform int   sun_up_factor;
uniform vec3  ambient_color;
uniform vec3  blue_horizon;
uniform float haze_horizon;
uniform float cloud_shadow;
uniform float max_y;
uniform vec3  glow;
uniform float sun_moon_glow_factor;

#ifdef HAS_HDRI
in vec4 vary_position;
in vec3 vary_rel_pos;
uniform float sky_hdr_scale;
uniform float hdri_split_screen;
uniform mat3 env_mat;
uniform sampler2D environmentMap;
#endif

uniform sampler2D rainbow_map;
uniform sampler2D halo_map;

uniform float moisture_level;
uniform float droplet_radius;
uniform float ice_level;

out vec4 frag_data[4];

vec3 srgb_to_linear(vec3 c);
vec3 linear_to_srgb(vec3 c);

#define PI 3.14159265

/////////////////////////////////////////////////////////////////////////
// The fragment shader for the sky
/////////////////////////////////////////////////////////////////////////


vec3 rainbow(float d)
{
    // 'Interesting' values of d are -0.75 .. -0.825, i.e. when view vec nearly opposite of sun vec
    // Rainbox tex is mapped with REPEAT, so -.75 as tex coord is same as 0.25.  -0.825 -> 0.175. etc.
    // SL-13629
    // Unfortunately the texture is inverted, so we need to invert the y coord, but keep the 'interesting'
    // part within the same 0.175..0.250 range, i.e. d = (1 - d) - 1.575
    d         = clamp(-0.575 - d, 0.0, 1.0);

    // With the colors in the lower 1/4 of the texture, inverting the coords leaves most of it inaccessible.
    // So, we can stretch the texcoord above the colors (ie > 0.25) to fill the entire remaining coordinate
    // space. This improves gradation, reduces banding within the rainbow interior. (1-0.25) / (0.425/0.25) = 4.2857
    float interior_coord = max(0.0, d - 0.25) * 4.2857;
    d = clamp(d, 0.0, 0.25) + interior_coord;

    float rad = (droplet_radius - 5.0f) / 1024.0f;
    return pow(texture(rainbow_map, vec2(rad+0.5, d)).rgb, vec3(1.8)) * moisture_level;
}

vec3 halo22(float d)
{
    d       = clamp(d, 0.1, 1.0);
    float v = sqrt(clamp(1 - (d * d), 0, 1));
    return texture(halo_map, vec2(0, v)).rgb * ice_level;
}

// <WolfViewer 2026-09-18> The northern lights (WolfWeatherProfile::mAurora).
// Source: wolfstorm/js/rendering/render_manager.js createSky() wsAurora / wsAuroraCurtain /
// wsAuroraNoise / wsAuroraHash - the same functions, constant for constant; keep them in step.
// OUR OWN implementation of the usual idea - a moving 2D curtain pattern extruded upward and
// summed along the view ray through a stack of altitude slabs - written from the physics, not
// from anyone's shader. Heights are in units of 100 km: a SHARP lower border near 100 km with a
// long tail above; colour by altitude (the purple N2+ fringe at 95-105 km, the 557.7 nm oxygen
// GREEN that dominates to ~180 km, the 630 nm oxygen RED above ~200 km, stronger in a storm);
// curtains run east-west with fine shimmering rays; the display is a BAND of latitude ~220 km to
// the north that widens and comes overhead as `amount` rises. `dir` is (east, north, up).
float wolfAuroraHash(vec2 p) { return fract(sin(dot(p, vec2(41.31, 289.17))) * 45751.37); }
float wolfAuroraNoise(vec2 p)
{
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(wolfAuroraHash(i), wolfAuroraHash(i + vec2(1.0, 0.0)), f.x),
               mix(wolfAuroraHash(i + vec2(0.0, 1.0)), wolfAuroraHash(i + vec2(1.0, 1.0)), f.x), f.y);
}
float wolfAuroraCurtain(vec2 p, float t)
{
    vec2 w = p + 1.7 * vec2(wolfAuroraNoise(p * 0.35 + vec2(0.0, t * 0.020)),
                            wolfAuroraNoise(p * 0.35 + vec2(7.3, -t * 0.017)));
    float n = wolfAuroraNoise(vec2(w.x * 0.6 + t * 0.03, w.y * 2.4));
    float ridge = pow(1.0 - abs(2.0 * n - 1.0), 7.0);
    float rays = 0.55 + 0.45 * wolfAuroraNoise(vec2(w.x * 9.0 - t * 0.35, w.y * 0.7));
    return ridge * rays;
}
// The chosen colour is the curtain's MAIN band (100-180 km); the purple lower fringe and the
// red high tops stay, as they do in every real display. 'multi' picks a hue per curtain fold.
vec3 wolfAuroraPalette(int mode)
{
    if (mode == 1) return vec3(1.00, 0.16, 0.20);   // red
    if (mode == 2) return vec3(0.62, 0.22, 0.95);   // purple
    if (mode == 3) return vec3(0.18, 0.45, 1.00);   // blue
    if (mode == 4) return vec3(1.00, 0.30, 0.65);   // pink
    return vec3(0.15, 1.00, 0.40);                   // green
}
vec3 wolfAurora(vec3 dir, float t, float amount, int colorMode)
{
    if (dir.z < 0.02) return vec3(0.0);
    vec3 sum = vec3(0.0);
    for (int i = 0; i < 16; i++)
    {
        float h = 0.95 + 2.05 * pow(float(i) / 15.0, 1.6);    // 95 .. 300 km
        vec2 p = dir.xy / dir.z * h;                           // the ray at that height
        float lat = (p.y - (2.2 - 1.6 * amount)) / (0.9 + 1.6 * amount);
        float emit = smoothstep(0.95, 1.05, h) * exp(-(h - 1.05) / 0.45) * exp(-lat * lat);
        vec3 main;
        if (colorMode == 5)
        {
            float hue = wolfAuroraNoise(vec2(p.x * 0.25 + t * 0.01, 3.7)) * 4.99;
            main = wolfAuroraPalette(int(hue));
        }
        else
        {
            main = wolfAuroraPalette(colorMode);
        }
        vec3 c = mix(vec3(0.55, 0.20, 0.75), main, smoothstep(0.95, 1.12, h));
        c = mix(c, vec3(0.90, 0.12, 0.18), smoothstep(1.7, 2.6, h) * (0.35 + 0.65 * amount));
        sum += c * wolfAuroraCurtain(p, t) * emit;
    }
    return sum * (6.6 / 16.0) * smoothstep(0.02, 0.22, dir.z) * amount;
}
// </WolfViewer>

// <WolfViewer 2026-10-07> THE OCEAN TO THE TRUE HORIZON. Paul, flying high over Wolf North: "its still
// like i'm flying over a square not the horizon blue downwards ... real horizon make it look perfect".
// The water planes stop at the far clip; the sea out to the real horizon is drawn here, by direction,
// as flight simulators draw what lies past their geometry. The earth is a sphere (R = 6371 km): from a
// height h the sea is in view down to the horizon dip, sqrt(2h/R) below level, ~2 degrees at 3.8 km,
// sqrt(2Rh + h^2) away, 220 km. Each direction below it meets the sea after t metres (ray / sphere);
// the sea there is its water colour, lit as the sky is, mirroring the sky by Schlick's Fresnel
// (2 % straight down, nearly all of it at grazing angles), seen through t metres of the same air the
// windlight atmosphere fogs everything with (exp(-(blue + haze density) x density x distance multiplier
// x t), atmosphericsFuncs.glsl), so it fades into the horizon haze exactly as the sky does. The water
// shader fades its planes into this toward the far clip, so there is no edge.
// The sky's own colour for a direction ABOVE the horizon, exactly as skyV.glsl works it out per
// vertex (same steps, same constants), times 2 as main() does. dir: dome frame (x north, y up, z east),
// normalised, dir.y > 0.
vec3 wolfSkyColour(vec3 dir)
{
    vec3 rel_pos = dir * (max_y / max(dir.y, 0.001));        // skyV: rel_pos *= max_y / rel_pos.y
    vec3 rel_pos_norm = dir;
    float rel_pos_len = length(rel_pos);
    float rel_pos_lightnorm_dot = dot(rel_pos_norm, lightnorm.xyz);
    vec3 sunlight = (sun_up_factor == 1) ? sunlight_color : moonlight_color * 0.7;
    vec3 light_atten = (blue_density + vec3(haze_density * 0.25)) * (density_multiplier * max_y);
    vec3 combined_haze = max(abs(blue_density) + vec3(abs(haze_density)), vec3(1e-6));
    vec3 blue_weight = blue_density / combined_haze;
    vec3 haze_weight = haze_density / combined_haze;
    float off_axis = 1.0 / max(1e-6, max(0., rel_pos_norm.y) + lightnorm.y);
    sunlight *= exp(-light_atten * off_axis);
    float density_dist = rel_pos_len * density_multiplier;
    combined_haze = exp(-combined_haze * density_dist);
    float haze_glow = 1.0 - rel_pos_lightnorm_dot;
    haze_glow = max(haze_glow, .001);
    haze_glow *= glow.x;
    haze_glow = pow(haze_glow, glow.z);
    haze_glow = (sun_moon_glow_factor < 1.0) ? 0.0 : (sun_moon_glow_factor * (haze_glow + 0.25));
    vec3 color = (blue_horizon * blue_weight * (sunlight + ambient_color)
               + (haze_horizon * haze_weight) * (sunlight * haze_glow + ambient_color));
    color *= (1. - combined_haze);
    vec3 ambient = ambient_color + max(vec3(0), (1. - ambient_color)) * cloud_shadow * 0.5;
    sunlight *= max(0.0, (1. - cloud_shadow));
    vec3 add_below_cloud = (blue_horizon * blue_weight * (sunlight + ambient)
                         + (haze_horizon * haze_weight) * (sunlight * haze_glow + ambient));
    combined_haze = sqrt(combined_haze);
    color += (add_below_cloud - color) * (1. - sqrt(combined_haze));
    return color * 2.0;
}

// dir: the view direction (dome frame, normalised); sky = the dome's own colour for it (below the
// horizon that is the windlight haze, which is what the distance fades the sea into).
// <WolfViewer 2026-10-07> Paul at midday: the first version mirrored that haze, so the whole sea came
// out white-grey. Water mirrors the sky at the MIRRORED angle - as far above the horizon as the ray
// is below it - which is bluer; the body is deep water, lit by the sun and sky.
vec3 wolfHorizonSea(vec3 dir, vec3 sky)
{
    const float R = 6371000.0;
    float dir_up = dir.y;
    float h = max(wolf_sea_height, 0.5);
    float rh = R + h;
    float b = rh * dir_up;                       // (camera from the earth's centre) . dir
    float disc = b * b - (2.0 * R * h + h * h);
    float dip_up = -sqrt(max(0.0, 1.0 - (R / rh) * (R / rh)));   // the horizon's up component
    // a soft edge a tenth of a degree wide across the horizon, so it does not crawl
    float sea_w = clamp((dip_up - dir_up) / 0.0015 + 0.5, 0.0, 1.0);
    if (sea_w <= 0.0)
    {
        return sky;
    }
    float t = (disc > 0.0) ? (-b - sqrt(disc)) : sqrt(2.0 * R * h + h * h);
    float cos_i = clamp(-(b + t) / R, 0.0, 1.0);            // -dir . the surface normal there
    float fresnel = 0.02 + 0.98 * pow(1.0 - cos_i, 5.0);
    // the sky at the mirrored angle (the surface tilts with the curve: cos_i above the horizon)
    vec3 mirror_dir = normalize(vec3(dir.x, max(cos_i, 0.002), dir.z));
    vec3 mirrored = wolfSkyColour(mirror_dir);
    vec3 sunlight = (sun_up_factor == 1) ? sunlight_color : moonlight_color * 0.7;
    vec3 body = waterFogColorLinear * (sunlight * max(lightnorm.y, 0.0) + ambient_color);
    vec3 sea = mix(body, mirrored, fresnel);
    float ext = ((blue_density.r + blue_density.g + blue_density.b) / 3.0 + haze_density) * density_multiplier * distance_multiplier;
    float haze = 1.0 - exp(-ext * t);
    return mix(sky, mix(sea, sky, haze), sea_w);
}
// </WolfViewer>

void main()
{
    vec3 color;
#ifdef HAS_HDRI
    vec3 frag_coord = vary_position.xyz/vary_position.w;
    if (-frag_coord.x > ((1.0-hdri_split_screen)*2.0-1.0))
    {
        vec3 pos = normalize(vary_rel_pos);
        pos = env_mat * pos;
        vec2 texCoord = vec2(atan(pos.z, pos.x) + PI, acos(pos.y)) / vec2(2.0 * PI, PI);
        color = textureLod(environmentMap, texCoord.xy, 0).rgb * sky_hdr_scale;
        color = min(color, vec3(8192*8192*16)); // stupidly large value arrived at by binary search -- avoids framebuffer corruption from some HDRIs

        frag_data[2] = vec4(0.0,0.0,0.0,GBUFFER_FLAG_HAS_HDRI);
    }
    else
#endif
    {
        // Potential Fill-rate optimization.  Add cloud calculation
        // back in and output alpha of 0 (so that alpha culling kills
        // the fragment) if the sky wouldn't show up because the clouds
        // are fully opaque.

        color = vary_HazeColor;

        float  rel_pos_lightnorm = vary_LightNormPosDot;
        float optic_d = rel_pos_lightnorm;
        vec3  halo_22 = halo22(optic_d);
        color.rgb += rainbow(optic_d);
        color.rgb += halo_22;
        color.rgb *= 2.;
        // <WolfViewer 2026-10-07> below the horizon: the sea (dome frame: x north, y up, z east).
        // skyV.glsl adds 50 m of lift to rel_pos for the windlight haze; the true view ray from the
        // camera is without it (renderDome: the dome is drawn at position - camPosLocal).
        if (wolf_sea_height >= 0.0)
        {
            color.rgb = wolfHorizonSea(normalize(vary_wolf_sky_dir - vec3(0.0, 50.0, 0.0)), color.rgb);
        }
        // <WolfViewer 2026-09-18> Light ADDED to the night sky. The dome frame is (north, up,
        // east); wolfAurora wants (east, north, up). This buffer is linear and the WolfStorm
        // numbers are display-referred, hence srgb_to_linear. Clouds are drawn after the dome,
        // so they cover it; the fog and the daylight are already in wolf_aurora.
        if (wolf_aurora > 0.0)
        {
            vec3 wd = normalize(vary_wolf_sky_dir);
            color.rgb += srgb_to_linear(clamp(wolfAurora(vec3(wd.z, wd.x, wd.y), wolf_aurora_time, wolf_aurora, wolf_aurora_color), vec3(0.0), vec3(1.0)));
        }
        // </WolfViewer>
        color.rgb = clamp(color.rgb, vec3(0), vec3(5));

        frag_data[2] = vec4(0.0,0.0,0.0,GBUFFER_FLAG_SKIP_ATMOS);
    }

    frag_data[1] = vec4(0);

#if defined(HAS_EMISSIVE)
    frag_data[0] = vec4(0);
    frag_data[3] = vec4(color.rgb, 1.0);
#else
    frag_data[0] = vec4(color.rgb, 1.0);
#endif
}

