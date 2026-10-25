/**
 * @file wolftk/nightLight.glsl
 * @brief WolfViewer: the moonlit night as the eye sees it, applied to the night's LIGHT (the sky, the clouds, the moon's light).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer - Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 * $/LicenseInfo$
 *
 * [WolfViewer 2026-10-25] Jimmy Olsen (ticket #226, Tromso): the moonlit night was amber. Moonlight is sunlight off the moon -
 * photometrically a little redder than daylight - and a moonlit night LOOKS blue-white because at moonlight levels the rods take
 * part (the Purkinje shift). Source: H. W. Jensen, S. Premoze, P. Shirley, W. B. Thompson, J. A. Ferwerda, M. M. Stark, "Night
 * Rendering", University of Utah UUCS-00-016 (2000), section 3.1 (the tone mapping of Jensen et al., "A Physically-Based Night
 * Sky Model", SIGGRAPH 2001):
 *   s = 0 below log10 Y = -2, 3 t^2 - 2 t^3 with t = (log10 Y + 2) / 2.6 up to log10 Y = 0.6, 1 above (Y in cd/m2)
 *   x = (1 - s) xb + s x,  y = (1 - s) yb + s y,  Y = 0.4468 (1 - s) V + s Y,  X = x Y / y,  Z = X / x - X - Y
 * with V from Larson, Rushmeier & Piatko, IEEE TVCG 3(4) 1997, eq. 13: V = Y [1.33 (1 + (Y + Z) / X) - 1.68].
 * (xb, yb) = (0.29, 0.28): the same report's MEASURED mean chromaticity of the dark areas of ten realist night paintings
 * ("approximately (x, y) = (0.29, 0.28)"); the authors moved their own to (0.25, 0.25) only "to allow for the uncertainties of
 * the printing process" - this is a screen.
 *
 * Applied here to the night's light, not to every pixel of the frame (W80's first version, Paul: "everywhere I go at night is
 * this" - the whole frame grey-blue, red and skin black): LL's emissive, fullbright and local lights are display-referred
 * (WolfEnvironmentRender::physicalLightColor), so a per-pixel rod model read windows and fires as dim and washed them out, and the
 * rods' V of a red albedo is 0. As the light's colour (the C++ does the same to the scene's moon and ambient light,
 * WolfEnvironmentRender night light), the surfaces keep their own colours under a blue-white moonlight and warm lights stay warm.
 * s is the frame's ONE adaptation level - the luminance of a white surface under the night's light (updateLight) - as the eye
 * adapts to the scene, not per pixel. wolfNight: 0 by day and in civil / nautical twilight (WolfEnvironmentRender::nightFactor).
 */
uniform float wolfNight;    // 0..1 (WolfTk::Atmosphere::setNightLight)
uniform float wolfNightS;   // the rods' saturation s at the frame's adaptation luminance (above)

vec3 wolfNightLight(vec3 rgb)
{
    if (!(wolfNight > 0.0)) return rgb;
    // Source: Filament ColorSpaceUtils.h:49-53 sRGB_to_XYZ / :43-47 XYZ_to_sRGB (column-major, as filamentAgx.glsl)
    const mat3 toXYZ = mat3(0.4124560, 0.2126730, 0.0193339, 0.3575760, 0.7151520, 0.1191920, 0.1804380, 0.0721750, 0.9503040);
    const mat3 toRGB = mat3(3.2404542, -0.9692660, 0.0556434, -1.5371385, 1.8760108, -0.2040259, -0.4985314, 0.0415560, 1.0572252);
    vec3 c = max(rgb, vec3(0.0));
    vec3 XYZ = toXYZ * c;
    float W = XYZ.x + XYZ.y + XYZ.z;
    if (!(W > 0.0) || !(XYZ.y > 0.0)) return rgb;
    float s = clamp(wolfNightS, 0.0, 1.0);
    float V = max(XYZ.y * (1.33 * (1.0 + (XYZ.y + XYZ.z) / max(XYZ.x, 1e-6 * W)) - 1.68), 0.0);
    float x = mix(0.29, XYZ.x / W, s);
    float y = mix(0.28, XYZ.y / W, s);
    float Y = mix(0.4468 * V, XYZ.y, s);
    vec3 night = max(toRGB * vec3(x * Y / y, Y, (1.0 - x - y) * Y / y), vec3(0.0));
    return mix(rgb, night, clamp(wolfNight, 0.0, 1.0));
}
