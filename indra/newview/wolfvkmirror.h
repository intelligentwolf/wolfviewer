/**
 * @file wolfvkmirror.h
 * @brief Vulkan copies of the OpenGL textures the Vulkan pass draws with, kept in step through LLImageGL's hooks.
 *
 * [VULKAN BACKEND 2026-11-08] OpenGL still builds and owns the scene's textures, and the Vulkan pass samples some of
 * them, so the Vulkan side holds its own copy of each texture it needs:
 *
 *   WolfVkMirror::texture(LLImageGL*)  a WolfVkImage (wolfvkimage.h) with the GL texture's internal format mapped by
 *       the format table and every mip level read back with glGetTexImage / glGetCompressedTexImage, plus the sampler
 *       matching its LLImageGL filter and address mode. LLImageGL::sWolfVkOnTexelsChanged marks it stale (rebuilt on
 *       next use) and sWolfVkOnDelete drops it. 2D textures only; cube maps, arrays and 3D textures return null and
 *       the draw uses the white fallback.
 *
 * [VK S6a 2026-11-26] Vertex and index buffers are no longer copied here: they live in the Vulkan-owned store
 * (wolfvkresources.h). The textures follow in stages 6b and 6c, and this file goes with them.
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

#ifndef WOLF_VK_MIRROR_H
#define WOLF_VK_MIRROR_H

#include "vk/wolfvkbuffer.h"
#include "vk/wolfvkimage.h"
#include <string>

class LLImageGL;
class WolfVkDevice;

namespace WolfVkMirror
{
    // Installs the LLImageGL hooks; the device must be live.
    void init(WolfVkDevice& device, WolfVkSamplerCache& samplers);
    // Removes the hooks and every copy.
    void shutdown();

    // [FLASH EVIDENCE 2026-11-08] running totals of the mirror's work, for the per-second log line
    struct Stats
    {
        uint64_t mTexturesMade = 0;       // texture copies created
        uint64_t mTexturesRemade = 0;     // ... of which replacing a stale copy (texels changed)
        uint64_t mTexturesDropped = 0;    // copies deleted with their GL texture
    };
    const Stats& stats();

    struct Texture
    {
        WolfVkImage* mImage = nullptr;
        VkSampler mSampler = VK_NULL_HANDLE;
    };
    // The Vulkan copy of a GL texture (made or remade on first use after a change); mImage null when it cannot be
    // mirrored (no GL texture, not 2D, unknown format). Uploads go on cmd, or on a one-time buffer when cmd is null.
    Texture texture(LLImageGL* image, VkCommandBuffer cmd);

    // Statistics for the floater and the log.
    uint32_t textureCount();
    uint64_t textureBytes();
}

#endif // WOLF_VK_MIRROR_H
