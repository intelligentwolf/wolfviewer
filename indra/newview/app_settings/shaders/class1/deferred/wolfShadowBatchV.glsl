/**
 * @file wolfShadowBatchV.glsl
 * @brief [W107 STATIC BATCH 2026-11-14] WolfViewer: the shadow map vertex shader for static casters drawn from the shared
 * buffers of wolfstaticshadowbatch.cpp (many spatial groups in one multi-draw call).
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

// Source: deferred/shadowV.glsl - the same expression, gl_Position = modelview_projection_matrix * vec4(position.xyz, 1.0).
// The one difference is where the matrix comes from: shadowV.glsl reads a uniform that LLRender::syncMatrices loads once
// per draw call; here each spatial group's matrix (the same product, worked out the same way on the CPU:
// wolfstaticshadowbatch.cpp groupMatrix()) is a per-instance attribute, picked by the draw command's base instance, so
// groups with different model matrices can share one glMultiDrawElementsIndirect call.
// Paired with the stock deferred/shadowF.glsl.

in vec3 position;
in mat4 wolf_mvp;

void main()
{
    //transform vertex
    gl_Position = wolf_mvp*vec4(position.xyz, 1.0);
}
