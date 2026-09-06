/**
 * @file wolfwakeStampF.glsl
 * @brief WolfViewer: boat wake field, the three components of a wash stamp (wolfwakefield.cpp).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 * $/LicenseInfo$
 */

// Source: wolfstorm/js/world/wake_field.js WakeField._stampMat fragment — the same maths.
//  1. PROPELLER WASH — turbulent trail astern, expanding and dissipating.
//  2. KELVIN WAKE — the V at a HALF-ANGLE of asin(1/3) = 19.47 degrees, a constant
//     independent of speed and hull (Kelvin 1887); speed changes the height, never
//     the angle. In local units the arm sits at across = astern * tan * 2 * aspect.
//  3. BOW WAVE — bright churn at the leading edge, growing with speed.
// Output R = foam, G = lift (Kelvin arms), B = dip (the churn astern), additively blended.

out vec4 frag_color;

in vec2 vLocal;

uniform float uSpeed;      // 0..1 normalised speed
uniform float uFoam;       // per-stamp intensity (1 / segments this frame)
uniform float uAspect;     // quad length / width
uniform float uKelvinTan;  // tan(asin(1/3))

void main()
{
    float astern = clamp(0.5 - vLocal.y, 0.0, 1.0);
    float across = abs(vLocal.x) * 2.0;

    float washWidth = 0.18 + astern * 0.55;
    float wash = (1.0 - smoothstep(washWidth * 0.55, washWidth, across))
               * (1.0 - smoothstep(0.05, 1.0, astern));

    float armAt = astern * uKelvinTan * uAspect * 2.0;
    float armW = 0.06 + astern * 0.10;
    float arm = 1.0 - smoothstep(0.0, armW, abs(across - armAt));
    arm *= smoothstep(0.0, 0.12, astern) * (1.0 - smoothstep(0.55, 1.0, astern));

    float bow = (1.0 - smoothstep(0.0, 0.16, astern))
              * (1.0 - smoothstep(0.0, 0.7, across));

    float sp = uSpeed;
    float foam = wash * (0.35 + 0.65 * sp)
               + arm * 0.55 * (0.25 + 0.75 * sp)
               + bow * 0.9 * sp;
    foam *= uFoam;
    float lift = arm * 0.80 * sp * uFoam;
    float dip  = wash * 0.35 * sp * uFoam;

    frag_color = vec4(clamp(foam, 0.0, 1.0), clamp(lift, 0.0, 1.0), clamp(dip, 0.0, 1.0), 1.0);
}
