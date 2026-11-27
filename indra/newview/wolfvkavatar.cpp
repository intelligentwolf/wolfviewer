/**
 * @file wolfvkavatar.cpp
 * @brief Stage S4 of the native Vulkan plan: the avatar draws of the deferred G-buffer phase (wolfvkavatar.h).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer Source Code
 * Copyright (C) 2026, IntelligentWolf Ltd
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "wolfvkavatar.h"

#include "lldrawpool.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "vk/wolfvkshader.h"
#include "vk/wolfvkspirv.h"

#include <string>
#include <unordered_map>

namespace
{
    // The answer of paletteMirrored per SPIR-V program (WolfVkShaderMirror::key(): the recipe key names the modules,
    // so a relinked program with the same recipe has the same reflection). Null = the palette is as the viewer uploads it.
    std::unordered_map<std::string, const char*> sPaletteAnswer;

    const char* checkPalette(const LLGLSLShader* shader, const WolfVkShaderMirror* mirror)
    {
        const WolfVkSpirv::BlockMember* palette = mirror->vertexMember("matrixPalette");   // llshadermgr.cpp mReservedUniforms "matrixPalette" (AVATAR_MATRIX)
        if (!palette) return "a skinned program whose SPIR-V vertex block has no matrixPalette";
        if (shader->mFeatures.hasObjectSkinning)
        {
            // Source: class1/avatar/objectSkinV.glsl:27 `uniform mat3x4 matrixPalette[MAX_JOINTS_PER_MESH_OBJECT];`
            // (a mat3x4 has 3 columns of 4 rows, GLSL 4.60 4.1.6); lldrawpool.cpp:713 uniformMatrix3x4fv, 12 floats a joint
            if (palette->mColumns != 3 || palette->mRows != 4) return "a rigged program whose matrixPalette is not a mat3x4 array";
        }
        else
        {
            // Source: class1/avatar/avatarSkinV.glsl:27 `uniform vec4 matrixPalette[45];`; llviewerjointmesh.cpp:185
            // uniform4fv(AVATAR_MATRIX, 45, ...): the whole array is sent at once
            if (palette->mColumns != 1 || palette->mRows != 4 || palette->mArraySize != 45) return "an avatar program whose matrixPalette is not vec4[45]";
        }
        return nullptr;
    }
}

bool WolfVkAvatar::poolInPhase(U32 pool_type)
{
    // Source: lldrawpool.h pool enum. POOL_ALPHA_MASK: LLDrawPoolAlphaMask::renderDeferred (lldrawpoolsimple.cpp) pushes
    // PASS_ALPHA_MASK and PASS_ALPHA_MASK_RIGGED; POOL_GLTF_PBR_ALPHA_MASK: LLDrawPoolGLTFPBR::renderDeferred
    // (lldrawpoolpbropaque.cpp) pushes the PBR alpha-mask batches and their rigged ones. POOL_FULLBRIGHT_ALPHA_MASK has
    // no deferred pass (lldrawpoolsimple.h: post-deferred only), so it never draws in this phase.
    return pool_type == LLDrawPool::POOL_AVATAR || pool_type == LLDrawPool::POOL_CONTROL_AV
        || pool_type == LLDrawPool::POOL_ALPHA_MASK || pool_type == LLDrawPool::POOL_GLTF_PBR_ALPHA_MASK;
}

bool WolfVkAvatar::paletteMirrored(const LLGLSLShader* shader, const WolfVkShaderMirror* mirror, const char*& why)
{
    if (!shader || !mirror) { why = "no program or no mirror"; return false; }
    if (!shader->mFeatures.hasObjectSkinning && !shader->mFeatures.hasSkinning) return true;
    auto it = sPaletteAnswer.find(mirror->key());
    if (it == sPaletteAnswer.end()) it = sPaletteAnswer.emplace(mirror->key(), checkPalette(shader, mirror)).first;
    if (it->second) { why = it->second; return false; }
    return true;
}

bool WolfVkAvatar::cullsFrontFaces()
{
    GLint mode = GL_BACK;   // the initial value (OpenGL 4.6 table 23.10) and LLRender::init's (llrender.cpp:895)
    glGetIntegerv(GL_CULL_FACE_MODE, &mode);
    return mode == GL_FRONT;
}

bool WolfVkAvatar::leftToGL(const LLGLSLShader* shader)
{
    static LLCachedControl<bool> gl_rigged(gSavedSettings, "WolfVulkanDebugGLRigged", false);
    static LLCachedControl<bool> gl_avatar(gSavedSettings, "WolfVulkanDebugGLAvatar", false);
    if (!shader) return false;
    if (gl_rigged && shader->mFeatures.hasObjectSkinning) return true;
    // Source: lldrawpoolavatar.cpp:560 beginDeferredImpostor, :585 beginDeferredRigid, :650 beginDeferredSkinned
    if (gl_avatar && (shader == &gDeferredAvatarProgram || shader == &gDeferredNonIndexedDiffuseAlphaMaskNoColorProgram || shader == &gDeferredImpostorProgram)) return true;
    return false;
}
