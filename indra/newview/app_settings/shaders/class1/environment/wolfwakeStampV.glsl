/**
 * @file wolfwakeStampV.glsl
 * @brief WolfViewer: boat wake field, one oriented stamp quad (wolfwakefield.cpp).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 * $/LicenseInfo$
 */

// Source: wolfstorm/js/world/wake_field.js WakeField._stampMat vertex + _stampBoat: the quad
// is a unit square (-0.5..0.5) positioned, rotated and scaled on the CPU; here that
// transform arrives as a centre and two axes in FIELD space (0..1 over the region), so
// the shader stays a single reusable unit quad. Local x = across track, y = along track
// (+0.5 ahead = the hull).

in vec3 position;
out vec2 vLocal;

uniform vec2 uCenter;
uniform vec2 uAxisX;
uniform vec2 uAxisY;

void main()
{
    vLocal = position.xy;
    vec2 f = uCenter + position.x * uAxisX + position.y * uAxisY;
    gl_Position = vec4(f * 2.0 - 1.0, 0.0, 1.0);
}
