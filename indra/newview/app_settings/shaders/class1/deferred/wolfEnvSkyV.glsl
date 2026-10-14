/**
 * @file wolfEnvSkyV.glsl
 * @brief WolfViewer: the Wolf Environment's sky dome (wolfEnvSkyF.glsl), vertex stage.
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

// [WOLF ENV PHASE 2 2026-10-12] The same dome, drawn the same way, as the EEP sky (skyV.glsl,
// LLDrawPoolWLSky::renderDome): only the view direction is passed on. The dome frame is x north,
// y up, z east (renderDome's 120 degree turn about (1,1,1), LLEnvironment::toCFR).

in vec3 position;

// [WolfViewer 2026-10-13] probe mode (wolfenvironmentrender.cpp probeSky): the pipeline's screen triangle, its positions
// already in clip space, covers the small probe target; skyF.glsl takes each pixel's direction from uProbeDir
uniform float uProbe;

void main()
{
    // [WolfViewer 2026-10-14] every draw as the screen triangle the probes use, the main view's on the far plane - not LL's
    // dome mesh. Paul's log 04:44Z: the dome drew 6.3 million samples through every teleport, and 0 after each shader reload
    // (the WE button, and the reloads at login) - an occlusion query around the draw, with the program bound, uProbe 0, the
    // depth buffer 1.0, LEQUAL, every mask on and the dome inside the far plane; the screen triangle with the same program
    // kept drawing (the probes read right throughout). The sky's colour depends on the direction alone, and skyF.glsl takes
    // it from the camera (uScreenToWorldDir), so no mesh is needed. Depth just inside the far plane: never clipped, behind
    // everything the scene drew.
    gl_Position = vec4(position.xy, uProbe > 0.5 ? 0.0 : 0.99999, 1.0);
}
