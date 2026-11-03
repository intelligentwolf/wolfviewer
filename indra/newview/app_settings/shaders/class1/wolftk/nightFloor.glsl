/**
 * @file wolftk/nightFloor.glsl
 * @brief WolfViewer: the night sky's floor - the sky, the clouds and the haze over the sky never darker on screen than the stock
 *        "Midnight" sky residents know from Second Life / Firestorm.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer - Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 * $/LicenseInfo$
 *
 * [WolfViewer 2026-11-02 W92] Jimmy Olsen (TROMSO, real-place sky, sun -27 deg, overcast 0.6, rain): the town lit by its own
 * lights and the sky above it pure black (3,3,5 on screen). Paul (2026-11-02): the sky as seen on screen never below the stock
 * night sky residents know; the ground and the exposure keep their real values.
 *
 * THE SKY'S FLOOR is the stock day cycle's midnight: Firestorm's default day cycle (app_settings/windlight/days/Default.xml:3-6,
 * "A-12AM" at 0) uses skies/A%2D12AM.xml, the values of skies/Midnight.xml:3-139 (ambient 0.204 0.242 0.330, blue_density 0.45,
 * blue_horizon 0.24, haze_density 4, haze_horizon 0 - the legacy array's element 0, [S] llsettingssky.cpp:982-985 -,
 * density_multiplier 0.0003, max_y 906.2, cloud_shadow 0.27, cloud_color 0.226, sunlight_color 0.349 0.356 0.660, gamma 1, the
 * moon at the zenith: lightnorm 0 1 0). Through the stock shaders as WolfEepReference transcribes them (wolfeepreference.cpp
 * skyHaze = [S] class1/deferred/skyV.glsl:73-166 with the moon's light x 0.7 and no haze glow - sun_moon_glow_factor 0.5 x 0.25
 * < 1, [S] llsettingssky.cpp:1318-1322; skyLinear = [S] skyF.glsl:114-115 x 2, then [S] softenLightF.glsl srgb_to_linear x
 * sky_hdr_scale 1 x 1.1 classic) it is, in the stock renderer's linear G-buffer units, at the elevations 0, 8, 30 and 90 deg:
 *   0.001786 0.001934 0.002682 | 0.001774 0.001926 0.002728 | 0.001630 0.001783 0.002664 | 0.001361 0.001491 0.002272
 * - sRGB 6,6,9 at the horizon to 4,5,7 at the zenith on the stock display (no post, gamma 1: WolfEepReference::display).
 * (Computed by this change's midnight.py, a line-for-line port of those functions; the same session's screen showed the stock
 * Midnight sky, the native sky off, at 5..9, 6..10, 8..14 between its stars and clouds.) Stock Midnight's sky is that dark: what
 * makes a Second Life night readable is its clouds and stars over it.
 *
 * THE CLOUDS' FLOOR (wolfNightFloorCloudNodes, the same four elevations) comes from the C++ (wolfenvironmentrender.cpp
 * updateNightCloudFloor): per channel the larger of the stock Midnight clouds (sRGB 7,7,10 .. 24,26,33) and the stock renderer's
 * own clouds for the EEP in force, as the night light shows them, x nightFactor (0 by day and through nautical twilight).
 *
 * The floors are in the units the frame is in while an EEP drives the native sky (wolfenvironmentrender.cpp updateLight: the
 * exposure puts the EEP's surfaces at the stock values), so they are on only then (wolfNightFloorOn; setNightFloorUniforms), and
 * never in a reflection-probe face (that sky lights the scene's ambient and the water's mirror).
 *
 * How it is laid on: the colour's luminance is raised to the floor's and its hue moved toward the floor's by the same share,
 *   c' = c + F max(0, 1 - Y(c) / Y(F))
 * so a colour at or above the floor (every day and dusk sky) is not touched, black becomes the floor, and between the two the
 * result is continuous in c: no step at any sun angle. The sky dome takes it before LL's stars are added over it (skyF.glsl;
 * lldrawpoolwlsky.cpp renderStarsDeferred: BT_ADD_WITH_ALPHA), so every star stays as bright above the floor as it was above
 * black. The clouds take theirs per unit of their own opacity (premultiplied: compositeF.glsl), so a thin cloud lifts little and a
 * gap shows the sky and the stars behind it. Moonlit clouds, the moon's aureole and the moon itself are physical and keep their
 * own light where it is above the floor. The light meter (logLumF.glsl) takes the clouds' lift back out.
 */

// 1 in the screen's own passes while an EEP drives the native sky, 0 otherwise (setNightFloorUniforms; an unset uniform reads 0)
uniform float wolfNightFloorOn;
uniform vec3 wolfNightFloorCloudNodes[4];   // the clouds' floor at 0, 8, 30, 90 deg (updateNightCloudFloor)

const vec3 WOLF_NIGHT_FLOOR_SKY_0  = vec3(0.001786, 0.001934, 0.002682);
const vec3 WOLF_NIGHT_FLOOR_SKY_8  = vec3(0.001774, 0.001926, 0.002728);
const vec3 WOLF_NIGHT_FLOOR_SKY_30 = vec3(0.001630, 0.001783, 0.002664);
const vec3 WOLF_NIGHT_FLOOR_SKY_90 = vec3(0.001361, 0.001491, 0.002272);

// Rec.709 luma, as the frame's meter (logLumF.glsl) and LL's luminanceF.glsl take it
float wolfNightFloorY(vec3 c)
{
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

// the floor at a direction whose sine of elevation is sinElev (below the horizon: the horizon's)
vec3 wolfNightFloorAt(float sinElev, vec3 f0, vec3 f8, vec3 f30, vec3 f90)
{
    float e = degrees(asin(clamp(sinElev, 0.0, 1.0)));
    if (e < 8.0) return mix(f0, f8, e / 8.0);
    if (e < 30.0) return mix(f8, f30, (e - 8.0) / 22.0);
    return mix(f30, f90, (e - 30.0) / 60.0);
}
vec3 wolfNightFloorSky(float sinElev)
{
    return wolfNightFloorAt(sinElev, WOLF_NIGHT_FLOOR_SKY_0, WOLF_NIGHT_FLOOR_SKY_8, WOLF_NIGHT_FLOOR_SKY_30, WOLF_NIGHT_FLOOR_SKY_90);
}
vec3 wolfNightFloorCloud(float sinElev)
{
    return wolfNightFloorAt(sinElev, wolfNightFloorCloudNodes[0], wolfNightFloorCloudNodes[1], wolfNightFloorCloudNodes[2], wolfNightFloorCloudNodes[3]);
}

// c' = c + F max(0, 1 - Y(c) / Y(F)) (above)
vec3 wolfNightFloorLift(vec3 c, vec3 floorColour)
{
    float yf = wolfNightFloorY(floorColour);
    if (!(yf > 0.0)) return c;
    float y = wolfNightFloorY(max(c, vec3(0.0)));
    return c + floorColour * max(0.0, 1.0 - y / yf);
}

// the same per unit of opacity, for a premultiplied colour (rgb already x alpha): the floor x alpha is the floor's light a layer
// of that opacity gives.
// [WolfViewer 2026-11-03 W93] KEEPING THE DECK'S OWN LIGHT. The sky's rule above sets every luminance under the floor to the floor
// itself: Y(c') = Y(c) + Y(F) (1 - Y(c) / Y(F)) = Y(F), whatever Y(c) was. On a sky that is right (a smooth gradient), but on the
// clouds it erased the deck's own moonlit / skylit shading wherever it was under the floor, leaving Y(F) x alpha - the deck's
// opacity alone, whose march steps near the horizon (cloudsF.glsl marchClouds: "Step longer in empty space ... produces banding
// artifacts") then read as horizontal bands (Paul, Lampert Highlands, W92: "this banding is terrible"). The clouds' lift now
// tapers out at twice the floor instead:
//   Y(c') = Y(c) + Y(F) a max(0, 1 - Y(c) / (2 Y(F) a))
// so black still becomes the floor (Y(F) a: the W92 night deck is never darker), a colour at or above 2 Y(F) a is not touched,
// and between the two half of the deck's own contrast stays (dY'/dY = 1/2, against 0 before); continuous, no step.
vec3 wolfNightFloorLiftPremultiplied(vec4 c, vec3 floorColour)
{
    float a = clamp(c.a, 0.0, 1.0);
    if (!(a > 0.0)) return c.rgb;
    float yf = wolfNightFloorY(floorColour) * a;
    if (!(yf > 0.0)) return c.rgb;
    float y = wolfNightFloorY(max(c.rgb, vec3(0.0)));
    return c.rgb + floorColour * a * max(0.0, 1.0 - y / (2.0 * yf));
}
