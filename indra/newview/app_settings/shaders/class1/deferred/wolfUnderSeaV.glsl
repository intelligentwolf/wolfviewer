/**
 * @file wolfUnderSeaV.glsl
 * @brief [UNDERSEA 2026-11-09] WolfViewer ticket 652: the sea plants (wolfundersea.cpp), swaying in the water.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer - Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

// Source: deferred/diffuseV.glsl (the non-skinned path, unchanged) with one addition: the sway. Paired with the stock
// deferred/diffuseAlphaMaskF.glsl, so the plants go into the G-buffer exactly as gDeferredNonIndexedDiffuseAlphaMaskProgram
// writes the birds and the dolphins (lit, shadowed, hazed and water-fogged by the deferred pipeline).
//
// texcoord0 is the plant's place in the undersea atlas (our own procedural textures, wolfundersea.cpp texel()).
// texcoord1 carries the sway: x = how far this vertex may move, metres (0 at the root), y = n + phase, where n (whole part)
// is the frequency in 1/60 Hz and the fraction the phase in turns. wolf_sway_time runs 0..60 s and wraps: every frequency
// is a whole number of cycles in 60 s, so the wrap is seamless.

uniform mat3 normal_matrix;
uniform mat4 modelview_projection_matrix;
uniform mat4 modelview_matrix;

uniform float wolf_sway_time;
uniform vec2 wolf_sway_dir;

in vec3 position;
in vec4 diffuse_color;
in vec3 normal;
in vec2 texcoord0;
in vec2 texcoord1;

out vec3 vary_normal;
out vec4 vertex_color;
out vec2 vary_texcoord0;
out vec3 vary_position;

void passTextureIndex();

void main()
{
    const float TWO_PI = 6.2831853;
    float freq = floor(texcoord1.y) / 60.0;
    float phase = fract(texcoord1.y) * TWO_PI;
    float w = TWO_PI * freq * wolf_sway_time;
    // the surge to and fro along the sway direction, and a smaller twice-as-fast wobble across it
    float s = sin(w + phase);
    float c = sin(2.0 * w + phase * 1.7);
    vec2 across = vec2(-wolf_sway_dir.y, wolf_sway_dir.x);
    vec3 pos = position.xyz;
    pos.xy += texcoord1.x * (wolf_sway_dir * s + across * (0.3 * c));
    // a bent stem is shorter: the tip dips a little as it leans
    pos.z -= texcoord1.x * 0.15 * abs(s);

    vary_position = (modelview_matrix * vec4(pos, 1.0)).xyz;
    gl_Position = modelview_projection_matrix * vec4(pos, 1.0);
    vary_normal = normalize(normal_matrix * normal);
    vary_texcoord0 = texcoord0;

    passTextureIndex();

    vertex_color = diffuse_color;
}
