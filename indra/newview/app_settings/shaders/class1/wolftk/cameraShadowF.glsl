/**
 * @file wolftk/cameraShadowF.glsl
 * @brief WolfViewer: the clouds' transmittance to the sun at the camera, for the light meter (1 x 1 target).
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

// [WOLF ENV TAKRAM 2026-10-12] The clouds' light at the camera for the light meter and the scene's sky light
// (1 x 1 target, read back by wolfenvironmentrender.cpp postPassInner). Linked with atmosphereLibF.glsl.
vec3 wolfTkCloudSkyLight();   // (direct transmittance at the camera, mean diffuse T, mean diffused share of the beam)

out vec4 frag_color;

void main()
{
    // atmosphereLibF.glsl wolfTkCloudSkyLight: the direct term is wolfTkCloudShadow at a point twice the near
    // clip ahead of the camera (the camera itself lies before the first shadow cascade's interval)
    frag_color = vec4(wolfTkCloudSkyLight(), 1.0);
}
