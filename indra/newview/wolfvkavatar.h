/**
 * @file wolfvkavatar.h
 * @brief Stage S4 of the native Vulkan plan: the avatar draws of the deferred G-buffer phase, recorded by Vulkan.
 *
 * [W114-VULKAN S4 2026-11-26] Avatars are drawn by OpenGL's own code, unchanged (LLDrawPoolAvatar, LLVOAvatar,
 * LLViewerJointMesh, LLRenderPass::pushRiggedBatches: none of it is touched). The draw interceptor of stage S2
 * (wolfvkbridge.h setGBufferPhase, wolfvkscene.h recordGBufferDraw, docs/VULKAN_BACKEND.md 8.6) records each draw
 * call they make from OpenGL's state at that moment; this file holds what the interceptor has to know about avatars:
 *
 *   - which pools of LLPipeline::renderGeomDeferred are avatar pools the phase must cover (poolInPhase);
 *   - the check that lets a skinned program be recorded at all (paletteMirrored): the joint matrices reach the
 *     program's Vulkan mirror through the same LLGLSLShader setters OpenGL's values go through, so the mirror's
 *     vertex block must hold "matrixPalette" in exactly the shape the viewer uploads -
 *       rigged meshes   mat3x4 matrixPalette[MAX_JOINTS_PER_MESH_OBJECT]  (class1/avatar/objectSkinV.glsl:27),
 *                       LLRenderPass::uploadMatrixPalette -> uniformMatrix3x4fv(AVATAR_MATRIX) (lldrawpool.cpp:713)
 *       system body     vec4 matrixPalette[45]                            (class1/avatar/avatarSkinV.glsl:27),
 *                       LLViewerJointMesh::uploadJointMatrices -> uniform4fv(AVATAR_MATRIX, 45) (llviewerjointmesh.cpp:185)
 *   - the two debug switches that send a family back to OpenGL for A/B pictures (leftToGL).
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

#ifndef WOLF_VK_AVATAR_H
#define WOLF_VK_AVATAR_H

#include "stdtypes.h"

class LLGLSLShader;
class WolfVkShaderMirror;

namespace WolfVkAvatar
{
    // True for the pool types stage S4 opens the interceptor's phase for (the pools before terrain are S2's): the
    // avatar pools LLDrawPool::POOL_AVATAR and POOL_CONTROL_AV (both LLDrawPoolAvatar), and the two alpha-mask pools
    // with a deferred pass, POOL_ALPHA_MASK and POOL_GLTF_PBR_ALPHA_MASK, whose rigged batches are avatar attachments
    // (their static batches go through the same interceptor). Each draw call is recorded or refused one by one.
    bool poolInPhase(U32 pool_type);

    // A skinned program (LLShaderFeatures hasObjectSkinning or hasSkinning) may be recorded only when its Vulkan
    // mirror holds the joint palette in the shape the viewer uploads (see above). False with why (a static string)
    // when it does not: the draw is OpenGL's. True for a program that is not skinned.
    bool paletteMirrored(const LLGLSLShader* shader, const WolfVkShaderMirror* mirror, const char*& why);

    // True when OpenGL culls FRONT faces right now. Asked only for a system-avatar program (hasSkinning) with culling
    // on: LLViewerJoint::render sets glCullFace(GL_FRONT) by a raw call for the inside pass of small hair and skirts
    // (llviewerjoint.cpp:122, back to GL_BACK at :128), which no LLGLState record sees, so OpenGL itself is asked
    // (glGetIntegerv(GL_CULL_FACE_MODE), OpenGL 4.6 table 23.10). The other raw glCullFace(GL_FRONT) calls of the
    // viewer (llmaniptranslate.cpp:1747, llviewerdisplay.cpp:2051, llviewerwindow.cpp:5349) are not avatar draws and not
    // in any phase the interceptor covers. In the opaque deferred pass the hair path is not reached at all
    // (LLVOAvatar::renderSkinned calls renderTransparent only when !sSkipTransparent || sImpostorRender,
    // llvoavatar.cpp:6305); the answer matters for the passes that draw hair: post-deferred and impostor generation.
    bool cullsFrontFaces();

    // The debug switches (settings.xml, HideFromEditor, off = Vulkan records), read at every draw:
    //   WolfVulkanDebugGLRigged  OpenGL draws every rigged-mesh program (hasObjectSkinning: the skinned variants of
    //                            the diffuse, bump, material, PBR, alpha-mask ... programs)
    //   WolfVulkanDebugGLAvatar  OpenGL draws the avatar pool's own deferred programs: gDeferredAvatarProgram (system
    //                            body), gDeferredNonIndexedDiffuseAlphaMaskNoColorProgram (eyes), gDeferredImpostorProgram
    bool leftToGL(const LLGLSLShader* shader);
}

#endif // WOLF_VK_AVATAR_H
