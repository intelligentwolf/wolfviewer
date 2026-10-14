/**
 * @file wolfEnvZoneF.glsl
 * @brief WolfViewer: the light inside a Wolf Environment zone (a cave, an interior), multiplied into the frame.
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

// [WOLF ENV 2026-10-12] Moved unchanged out of wolfEnvCloudsF.glsl pass 3 when that file was replaced by the
// translated three-geospatial renderer (wolftk/): the lit frame x this colour (blend DST_COLOR x ZERO,
// WolfEnvironmentRender::zonePass).
out vec4 frag_color;
uniform vec3 uLightMul;
void main()
{
    frag_color = vec4(clamp(uLightMul, vec3(0.0), vec3(4.0)), 1.0);
}
