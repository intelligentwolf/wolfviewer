/**
 * @file wolfvkmirror.h
 * @brief Vulkan copies of the OpenGL resources the Vulkan pass draws with: vertex buffers and textures, kept in step
 *        through LLVertexBuffer's and LLImageGL's hooks.
 *
 * [VULKAN BACKEND 2026-11-08] Stage 4 is a hand-over: OpenGL still builds and owns the scene's buffers and textures,
 * and the Vulkan pass draws some of them. Until images are shared between the two (GL_EXT_memory_object, the next
 * stage, proven possible by the vkglshare probe), the Vulkan side holds its own copy of each resource it needs:
 *
 *   WolfVkMirror::vertexBuffer(LLVertexBuffer*)  a WolfVkVertexBuffer (wolfvkbuffer.h) with the same layout, filled
 *       once from the GL buffer (glGetBufferSubData, so the copy is what GL draws, not a guess at the client data) and
 *       then kept current by LLVertexBuffer::sWolfVkOnFlush (every flush_vbo lands in both) and dropped by
 *       sWolfVkOnDestroy. Pending uploads are recorded at the start of the Vulkan frame.
 *   WolfVkMirror::texture(LLImageGL*)  a WolfVkImage (wolfvkimage.h) with the GL texture's internal format mapped by
 *       the format table and every mip level read back with glGetTexImage / glGetCompressedTexImage, plus the sampler
 *       matching its LLImageGL filter and address mode. LLImageGL::sWolfVkOnTexelsChanged marks it stale (rebuilt on
 *       next use) and sWolfVkOnDelete drops it. 2D textures only in this cut (the fullbright pass uses nothing else);
 *       cube maps, arrays and 3D textures return null and the draw uses the white fallback.
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

class LLVertexBuffer;
class LLImageGL;
class WolfVkDevice;

namespace WolfVkMirror
{
    // Installs the LLVertexBuffer / LLImageGL hooks; the device must be live.
    void init(WolfVkDevice& device, WolfVkSamplerCache& samplers);
    // Removes the hooks and every copy.
    void shutdown();

    // The Vulkan copy of a GL vertex buffer (made on first use); null when the buffer has no GL storage yet.
    WolfVkVertexBuffer* vertexBuffer(LLVertexBuffer* buffer);
    // Records every pending buffer upload into cmd (call once per frame before the draws).
    void recordUploads(VkCommandBuffer cmd);
    // [STAGING RACE 2026-11-08] the per-frame transient staging the copies read from (owned by WolfVkScene); null = direct copies
    void setStagingRing(WolfVkStagingRing* ring);

    // [FLASH EVIDENCE 2026-11-08] running totals of the mirror's work, for the per-second log line
    struct Stats
    {
        uint64_t mBuffersMade = 0;        // Vulkan copies of GL vertex buffers created
        uint64_t mBuffersRemade = 0;      // ... of which replacing a copy whose GL buffer was re-sized
        uint64_t mBufferBytesWritten = 0; // bytes written into copies by the flush hook
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

    // [POSE CHECK 2026-11-08] WolfVulkanDebugPoseCheck (wolfvkscene.h): the copy of buffer if one exists (never makes one);
    // buffers the Vulkan pass drew this frame; GL flushes into one of them after that, until poseFrameEnd (present)
    WolfVkVertexBuffer* existingVertexBuffer(LLVertexBuffer* buffer);
    void poseNoteDrawn(LLVertexBuffer* buffer);
    void poseFrameEnd();
    uint64_t poseLateFlushes();

    // Statistics for the floater and the log.
    uint32_t vertexBufferCount();
    uint32_t textureCount();
    uint64_t textureBytes();
}

#endif // WOLF_VK_MIRROR_H
