/**
 * @file wolffftV.glsl
 * @brief WolfViewer: full-screen triangle for the spectral ocean passes (wolfoceanfft.cpp).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 * $/LicenseInfo$
 */

// Source: class1/interface/copyV.glsl — the same screen triangle LLPipeline::mScreenTriangleVB
// draws (pipeline.cpp: (-1,1) (-1,-3) (3,1)); the fragment stages address texels by
// gl_FragCoord, so no texcoord is needed.
in vec3 position;

void main()
{
    gl_Position = vec4(position, 1.0);
}
