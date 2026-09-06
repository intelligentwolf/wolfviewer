/**
 * @file wolfwakeDecayF.glsl
 * @brief WolfViewer: boat wake field, decay + diffuse pass (wolfwakefield.cpp).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 * $/LicenseInfo$
 */

// Source: wolfstorm/js/world/wake_field.js WakeField._decayMat fragment — the same maths.
// out = max(prev * decay - floor, 0), with a 4-tap box spread so the trail softens and
// widens as it ages. R = foam, G = surface lift, B = surface dip (two positive channels
// differenced at read time, because a fragment output is clamped to [0,1] before
// blending on a fixed-point target). The subtractive floor is what guarantees an
// 8-bit field returns to exactly zero instead of sticking at a faint ghost.

out vec4 frag_color;

uniform sampler2D uPrev;
uniform float uFoamDecay;
uniform float uCrestDecay;
uniform float uTexel;
uniform float uSpread;

void main()
{
    vec2 uv = gl_FragCoord.xy * uTexel;
    vec4 c = texture(uPrev, uv);
    vec4 n = texture(uPrev, uv + vec2(uTexel, 0.0))
           + texture(uPrev, uv - vec2(uTexel, 0.0))
           + texture(uPrev, uv + vec2(0.0, uTexel))
           + texture(uPrev, uv - vec2(0.0, uTexel));
    vec4 blurred = mix(c, n * 0.25, uSpread);
    vec3 decayed = blurred.rgb * vec3(uFoamDecay, uCrestDecay, uCrestDecay);
    decayed = max(decayed - 0.0025, 0.0);
    frag_color = vec4(decayed, 1.0);
}
