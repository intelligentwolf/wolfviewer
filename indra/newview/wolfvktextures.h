/**
 * @file wolfvktextures.h
 * @brief The Vulkan renderer owns the memory of the textures OpenGL code holds by a raw name.
 *
 * [VK S6 2026-11-26] Stage 6 of the native Vulkan plan, "resource ownership": the Wolf sky tables, the terrain layer
 * arrays, the cloud maps, the reflection probe arrays and the other textures made with glGenTextures + glTexImage*
 * outside LLImageGL's own upload path. Their call sites now ask LLImageGL::wolfVkCreateStorage first; this file is
 * the owner behind it:
 *
 *   - each texture is a WolfVkSharedImage (wolfvkshare.h createKind): a Vulkan image of the same kind, size, levels
 *     and format with exportable memory, and the OpenGL texture imported over that memory with immutable storage
 *     (glTexStorageMem2DEXT / glTexStorageMem3DEXT). One copy on the card; OpenGL keeps filling and sampling it by
 *     its name exactly as before, and a Vulkan pass finds the image by that name (find()).
 *   - whether a kind / format / usage can be shared, and whether its memory must be a dedicated allocation, is the
 *     driver's own answer (WolfVkImage::externalFormatSupported). When it says no, createStorage returns 0, the
 *     caller makes its ordinary OpenGL texture and one line in the log names the format.
 *   - the image is handed to OpenGL at once (WolfVkSharedImage::handToGL). A Vulkan pass that wants to sample or
 *     render it takes it over with the hand-over of wolfvkshare.h: OpenGL signals a semaphore naming the texture and
 *     a layout, the pass's submit waits it and records acquireFromGL(that layout ...), and gives it back with
 *     releaseToGL + a semaphore OpenGL waits. glHandLayout() is the layout it was first handed over in.
 *
 * Falling back to the OpenGL renderer (shutdown()): the Vulkan images are released and the OpenGL textures stay as
 * they are, valid and filled - OpenGL holds its own reference to the memory (probe 2026-11-26 section D on NVIDIA
 * 615.78 and RADV 26.2.5) - until their owners delete them, which still comes through here.
 *
 * macOS: Apple's OpenGL has no GL_EXT_memory_object; init() returns false there and every texture stays OpenGL's.
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

#ifndef WOLF_VK_TEXTURES_H
#define WOLF_VK_TEXTURES_H

#include <cstdint>
#include <string>

class WolfVkDevice;
class WolfVkSharedImage;

namespace WolfVkTextures
{
    // Installs the owner in LLImageGL. The OpenGL context must be current. False with why when the device or the
    // OpenGL driver cannot share images; nothing is installed then.
    bool init(WolfVkDevice& device, std::string& why);
    // The device is going (or the renderer falls back to OpenGL): no new texture is made here, the Vulkan half of
    // every texture is released; the OpenGL textures live on (see above). Safe to call twice or without init.
    void shutdown();
    bool active();

    // The shared image behind an OpenGL texture name made here, else null (not made here, or shut down).
    WolfVkSharedImage* find(uint32_t gl_name);

    // For the log line.
    struct Stats
    {
        uint32_t mTextures = 0;      // textures alive whose memory is Vulkan's
        uint64_t mBytes = 0;         // their memory on the card
        uint64_t mMade = 0;          // made since init
        uint64_t mRefused = 0;       // requests the driver could not share (those textures are OpenGL's own)
    };
    Stats stats();
}

#endif // WOLF_VK_TEXTURES_H
