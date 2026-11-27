/**
 * @file wolfvkbrackettarget.h
 * @brief What the draw interceptor's bracket needs of a render target that OpenGL and Vulkan take turns on.
 *
 * [W114-VULKAN S5 2026-11-26] The interceptor (wolfvkbridge.cpp openBracket / closeBracket / interceptDraw, docs
 * 8.6) records OpenGL's draw calls into "the bracket's target". That was the shared G-buffer alone; it is now any
 * WolfVkBracketTarget: the G-buffer (wolfvkbridge.cpp GBufferBracket over WolfVkGBuffer), a shadow map
 * (wolfvkshadow.h), and the targets other stages add. Stage code never calls a hand-over function itself: it makes
 * its target selectable (wolfvkbridge.cpp recordableTarget) and the bracket makes every call below.
 * Every OpenGL call of the hand-over is behind this interface, so a target with no OpenGL side at all (the
 * full-Vulkan frame, the only mode on macOS) implements it with glHandToVulkan / glTakeBack returning true and
 * doing nothing, an empty sync(), and acquire / release / glAfterTakeBack as no-ops.
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

#ifndef WOLF_VK_BRACKET_TARGET_H
#define WOLF_VK_BRACKET_TARGET_H

#include "vk/wolfvkloader.h"
#include "vk/wolfvkdevice.h"
#include "vk/wolfvkimage.h"
#include "vk/wolfvkrendertarget.h"
#include <string>
#include <vector>

class WolfVkSharedImage;

// A render target the interceptor's bracket can record into. The calls are the pass hand-over of wolfvkgbuffer.h, in the
// order the bracket makes them:
//   open:   prepare, glHandToVulkan (GL signals; false = nothing handed over, nothing may be recorded or submitted),
//           acquire(cmd)
//   record: target() - the Vulkan target the draws go into (LOAD: what OpenGL drew or cleared stays)
//   close:  release(cmd), sync() (the submit's waits and signals, exactly once per bracket), glTakeBack (GL waits),
//           glAfterTakeBack (what OpenGL must do before it draws into the target again; called only while the target is
//           still OpenGL's bound target)
// broken(): a GL semaphore call failed and the semaphores' state is unknown - the owner falls back to OpenGL.
struct WolfVkBracketTarget
{
    virtual ~WolfVkBracketTarget() = default;
    virtual const char* bracketName() const = 0;   // for the log: "G-buffer", "shadow map"
    virtual bool prepare() = 0;
    virtual bool glHandToVulkan() = 0;
    virtual void acquire(VkCommandBuffer cmd) = 0;
    virtual WolfVkRenderTarget* target() = 0;
    virtual void release(VkCommandBuffer cmd) = 0;
    virtual WolfVkDevice::ExtraSync sync() = 0;
    virtual bool glTakeBack() = 0;
    virtual void glAfterTakeBack() = 0;
    // the target is OpenGL's bound render target right now (the bracket records only then, and glAfterTakeBack runs only then)
    virtual bool boundInGL() = 0;
    // Images the recorded draws SAMPLE that OpenGL also owns (a WolfVkSharedImage found by its GL texture name:
    // terrain layers, an impostor, a shadow map, a lighting input). EXT_external_objects makes OpenGL name every
    // texture it hands over in its signal and takes back in its wait, so they travel with the bracket:
    //   wantSampled(name)    from any time on, the image of that name joins the hand-over of this target's NEXT
    //                        bracket (read-only for Vulkan), and of every later one, while such an image exists.
    //   sampledImage(name)   in an open bracket: the image, readable by Vulkan (SHADER_READ_ONLY_OPTIMAL, or
    //                        DEPTH_STENCIL_READ_ONLY_OPTIMAL for a depth image), when it was in the hand-over at
    //                        open. Null when it was not: it is then wanted from the next bracket on, and the
    //                        recorder refuses that draw - the bracket closes, OpenGL draws the call, and the next
    //                        recorded draw opens a bracket that has the image. Null also for a name with no
    //                        shared image.
    // An implementation with no OpenGL side (the full-Vulkan frame) returns the image at once from sampledImage.
    virtual void wantSampled(uint32_t gl_texture) = 0;
    virtual WolfVkImage* sampledImage(uint32_t gl_texture) = 0;
    virtual bool broken() const = 0;
    virtual const std::string& brokenReason() const = 0;
};

// The sampled images of a shared-with-OpenGL bracket target (wantSampled / sampledImage above), for the target's own
// hand-over functions to use: the wanted names, and the list fixed for the open bracket.
class WolfVkBracketExtras
{
public:
    void want(uint32_t gl_texture);
    // glHandToVulkan: fixes the open bracket's list (every wanted name that has a shared image now and whose read
    // layout OpenGL can name) and appends it to the textures / GL layout tokens of the signal. Names whose image
    // has gone are dropped from the wanted list.
    void appendForSignal(std::vector<uint32_t>& textures, std::vector<uint32_t>& layouts);
    // glTakeBack: the same list for the wait, and the bracket's list ends
    void appendForWait(std::vector<uint32_t>& textures, std::vector<uint32_t>& layouts);
    // a hand-over that did not happen (the signal failed): the bracket's list ends, nothing was handed over
    void cancel() { mOpen.clear(); }
    // acquire / release: the queue family transfers of the bracket's list (WolfVkSharedImage::acquireFromGL /
    // releaseToGL), read-only for Vulkan in between
    void acquire(VkCommandBuffer cmd);
    void release(VkCommandBuffer cmd);
    // sampledImage: the image when it is in the open bracket's list; else null, and the name is wanted from now on
    WolfVkImage* inBracket(uint32_t gl_texture);
    bool empty() const { return mOpen.empty(); }

private:
    struct Entry { uint32_t mName; VkImageLayout mLayout; };
    std::vector<uint32_t> mWanted;
    std::vector<Entry> mOpen;   // by name, not by pointer: an image destroyed since is found gone, never dereferenced
};

#endif // WOLF_VK_BRACKET_TARGET_H
