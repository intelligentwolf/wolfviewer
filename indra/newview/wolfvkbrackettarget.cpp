/**
 * @file wolfvkbrackettarget.cpp
 * @brief The sampled images that travel with a bracket shared with OpenGL (wolfvkbrackettarget.h).
 *
 * [W114-VULKAN S5 2026-11-26]
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
#include "wolfvkbrackettarget.h"
#include "wolfvkshare.h"
#include <algorithm>

namespace
{
    // what Vulkan may do with a sampled image of the bracket: read it in a shader, nothing else
    VkImageLayout readLayout(const WolfVkImage& image)
    {
        // Source: vulkan_core.h VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL / VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        // EXT_external_objects table 4.4 names both (GL_LAYOUT_DEPTH_STENCIL_READ_ONLY_EXT, GL_LAYOUT_SHADER_READ_ONLY_EXT)
        return image.format().mDepth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
}

void WolfVkBracketExtras::want(uint32_t gl_texture)
{
    if (gl_texture == 0) return;
    if (std::find(mWanted.begin(), mWanted.end(), gl_texture) == mWanted.end()) mWanted.push_back(gl_texture);
}

void WolfVkBracketExtras::appendForSignal(std::vector<uint32_t>& textures, std::vector<uint32_t>& layouts)
{
    mOpen.clear();
    for (auto it = mWanted.begin(); it != mWanted.end();)
    {
        WolfVkSharedImage* shared = WolfVkSharedImage::find(*it);
        if (!shared || !shared->isValid())
        {
            it = mWanted.erase(it);   // the texture has gone; a new one of the same name is wanted again when a draw asks
            continue;
        }
        const VkImageLayout layout = readLayout(*shared->image());
        const uint32_t token = WolfVkSharedImage::glLayout(layout);
        if (token != 0)
        {
            mOpen.push_back({ *it, layout });
            textures.push_back(*it);
            layouts.push_back(token);
        }
        ++it;
    }
}

void WolfVkBracketExtras::appendForWait(std::vector<uint32_t>& textures, std::vector<uint32_t>& layouts)
{
    for (const Entry& e : mOpen)
    {
        textures.push_back(e.mName);
        layouts.push_back(WolfVkSharedImage::glLayout(e.mLayout));
    }
    mOpen.clear();
}

void WolfVkBracketExtras::acquire(VkCommandBuffer cmd)
{
    for (const Entry& e : mOpen)
    {
        WolfVkSharedImage* shared = WolfVkSharedImage::find(e.mName);
        if (!shared) continue;
        // vulkan_core.h VK_PIPELINE_STAGE_VERTEX_SHADER_BIT / FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT: sampled by either stage
        shared->acquireFromGL(cmd, e.mLayout, e.mLayout, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
}

void WolfVkBracketExtras::release(VkCommandBuffer cmd)
{
    for (const Entry& e : mOpen)
    {
        WolfVkSharedImage* shared = WolfVkSharedImage::find(e.mName);
        if (shared) shared->releaseToGL(cmd, e.mLayout);
    }
}

WolfVkImage* WolfVkBracketExtras::inBracket(uint32_t gl_texture)
{
    for (const Entry& e : mOpen)
    {
        if (e.mName != gl_texture) continue;
        WolfVkSharedImage* shared = WolfVkSharedImage::find(gl_texture);
        return shared ? shared->image() : nullptr;
    }
    want(gl_texture);
    return nullptr;
}
