/**
 * @file wolfvkresources.h
 * @brief The Vulkan renderer owns the scene's card memory: one copy of every vertex and index buffer, which OpenGL
 *        reads through an import while it still draws some passes.
 *
 * [VK S6a 2026-11-26] Stage 6 of the native Vulkan plan, "resource ownership", part a: vertex and index buffers.
 * Before this the Vulkan passes drew from COPIES of OpenGL's buffers (wolfvkmirror.cpp: glGetBufferSubData into a host
 * shadow, a second buffer on the card, every flush written twice). Now:
 *
 *   - Card memory comes from exportable Vulkan blocks (VkExportMemoryAllocateInfo), each imported into OpenGL ONCE
 *     (GL_EXT_memory_object: glImportMemoryFdEXT / glImportMemoryWin32HandleEXT). One VkBuffer spans a block.
 *   - LLVertexBuffer asks this store for its storage (LLVertexBuffer::WolfVkStore): it gets a range of a block and an
 *     OpenGL buffer name whose data store is that same range (glBufferStorageMemEXT(size, memory, offset)).
 *   - OpenGL never writes these buffers (EXT_external_objects issue 12: they cannot be mapped; glBufferStorageMemEXT
 *     takes no flags, so glBufferSubData is not promised). LLVertexBuffer::flush_vbo hands the changed bytes to
 *     write(): they are copied into host-visible staging memory and a vkCmdCopyBuffer is noted.
 *   - LLVertexBuffer::flushBuffers / setBuffer end the batch (batchEnd): ONE submit on the graphics queue carries the
 *     copies. It is bracketed by two binary semaphores shared with OpenGL (GL_EXT_semaphore):
 *         OpenGL signals "gl" (its earlier draws are ahead of the copy), the submit waits "gl", copies, signals "vk",
 *         OpenGL waits "vk" (its later draws are behind the copy)
 *     so OpenGL sees the bytes in command order, exactly as with glBufferSubData. Vulkan passes need no wait at all:
 *     their draws are submitted later on the same queue.
 *   - The Vulkan passes draw straight from the block's VkBuffer at the range's offset (vertexBuffer()).
 *
 * What stays OpenGL's own: LLRender's immediate-mode buffers (LLVertexBuffer::setWolfGLOnly) until the UI is drawn by
 * Vulkan, and every buffer on a computer where the store cannot start (see init). macOS: Apple's OpenGL has no
 * GL_EXT_memory_object, so there is nothing to import into and init() returns false there; the Mac gets this store
 * when OpenGL draws nothing (the whole frame through MoltenVK).
 *
 * Failure: if a block cannot be made or imported, or a semaphore call fails, failed() turns true: nothing more is
 * allocated here (LLVertexBuffer falls back to its OpenGL pool by itself) and the owner (WolfVkBridge) must fall back
 * to the OpenGL renderer, whose tear-down calls shutdown(): every buffer held here moves to an ordinary OpenGL buffer,
 * refilled from LLVertexBuffer's client copy (LLVertexBuffer::wolfVkReleaseAllToGL).
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

#ifndef WOLF_VK_RESOURCES_H
#define WOLF_VK_RESOURCES_H

#include "vk/wolfvkbuffer.h"
#include <string>

class LLVertexBuffer;
class WolfVkDevice;

namespace WolfVkResources
{
    // Starts the buffer store and installs it in LLVertexBuffer. The OpenGL context must be current and
    // WolfVkShare::available() true. A round trip is run first (Vulkan writes three ranges of a block at different offsets,
    // OpenGL DRAWS from the imported buffers and the fetched bytes are compared; then a partial rewrite): false with why when
    // the device, the driver or the round trip says no - then nothing is installed.
    bool init(WolfVkDevice& device, std::string& why);
    // Every buffer goes back to OpenGL's own pool (refilled from its client copy) and the store's memory is freed
    // (left with the device when it is hung). OpenGL context current. Safe to call when init failed or twice.
    void shutdown();
    // The store is installed and has not failed.
    bool active();
    // A failure since init that the renderer cannot continue from: the owner falls back to OpenGL (see above).
    bool failed();
    const std::string& failure();

    // The ranges of an LLVertexBuffer as the Vulkan passes bind them; null when the buffer does not live in the store
    // (made before init, GL-only, or the store failed). Bytes still pending in LLVertexBuffer are flushed first
    // (LLVertexBuffer::flushBuffers), so what is drawn is what OpenGL would draw. The object stays valid until the
    // LLVertexBuffer is re-allocated or destroyed. Main thread, OpenGL context current.
    WolfVkVertexBuffer* vertexBuffer(LLVertexBuffer* buffer);

    // Once a device frame (WolfVkScene::beginFrame, OpenGL context current): a store that failed since the last call
    // is shut down here, which moves every buffer to OpenGL's own pool; the Vulkan passes then find no buffer in the
    // store (vertexBuffer() null) and OpenGL draws those batches, as it does for any batch Vulkan skips.
    void frameBegin();

    // [POSE CHECK] WolfVulkanDebugPoseCheck: buffers a Vulkan pass recorded draws from this frame, and how many times
    // LLVertexBuffer rewrote one of them before the frame ended. The Vulkan draws are in a command buffer that is
    // submitted later, so they will read the rewritten bytes while OpenGL's later passes of this frame draw them too.
    void poseNoteDrawn(LLVertexBuffer* buffer);
    void poseFrameEnd();
    uint64_t poseLateFlushes();

    // For the log line and the floater.
    struct Stats
    {
        uint32_t mBlocks = 0;              // exportable blocks alive
        uint64_t mBlockBytes = 0;          // their size on the card
        uint64_t mBytesUsed = 0;           // handed out to buffers
        uint32_t mRanges = 0;              // buffers (vertex or index ranges) alive
        uint64_t mBatches = 0;             // upload batches submitted since init (one queue submit + one GL wait each)
        uint64_t mBytesUploaded = 0;       // bytes copied to the card since init
        uint64_t mStagingBytes = 0;        // host-visible staging memory held
        uint32_t mBatchesLastFrame = 0;    // batches in the last finished device frame
    };
    Stats stats();
}

#endif // WOLF_VK_RESOURCES_H
