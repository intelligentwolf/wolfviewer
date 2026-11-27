/**
 * @file wolfvkstate.h
 * @brief Lightweight tracked fixed-function state for Native Vulkan in-world passes.
 *
 * [NATIVE VULKAN 2026-11-26]
 * Kept in its own header (outside llgl.h) so draw pools and pipeline.cpp can record
 * cull-face, wireframe, polygon-offset, point-size, and viewport changes without
 * triggering a full viewer rebuild.
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

#ifndef WOLF_VK_STATE_H
#define WOLF_VK_STATE_H

#include "llgl.h"

namespace WolfVkState
{
    struct State
    {
        bool mCullFront = false;
        bool mWireframe = false;
        float mPolyOffsetFactor = 0.f;
        float mPolyOffsetUnits = 0.f;
        float mPointSize = 1.f;
        bool mViewportOverride = false;
        GLint mViewport[4] = { 0, 0, 0, 0 };
    };

    inline State& current()
    {
        static State s;
        return s;
    }

    inline void cullFace(GLenum mode)
    {
        current().mCullFront = (mode == GL_FRONT);
        if (!gGLManager.mIsNativeVulkan)
        {
            glCullFace(mode);
        }
    }

    inline void polygonMode(GLenum face, GLenum mode)
    {
        current().mWireframe = (mode == GL_LINE);
        if (!gGLManager.mIsNativeVulkan)
        {
            glPolygonMode(face, mode);
        }
    }

    inline void polygonOffset(GLfloat factor, GLfloat units)
    {
        current().mPolyOffsetFactor = factor;
        current().mPolyOffsetUnits = units;
        if (!gGLManager.mIsNativeVulkan)
        {
            glPolygonOffset(factor, units);
        }
    }

    inline void pointSize(GLfloat size)
    {
        current().mPointSize = size;
        if (!gGLManager.mIsNativeVulkan)
        {
            glPointSize(size);
        }
    }

    inline void viewport(GLint x, GLint y, GLsizei w, GLsizei h)
    {
        current().mViewportOverride = true;
        current().mViewport[0] = x;
        current().mViewport[1] = y;
        current().mViewport[2] = w;
        current().mViewport[3] = h;
        if (!gGLManager.mIsNativeVulkan)
        {
            glViewport(x, y, w, h);
        }
    }

    inline void clearViewportOverride()
    {
        current().mViewportOverride = false;
    }
}

#endif // WOLF_VK_STATE_H
