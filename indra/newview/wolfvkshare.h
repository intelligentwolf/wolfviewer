/**
 * @file wolfvkshare.h
 * @brief Images and semaphores shared between the OpenGL context and the Vulkan device: the hand-over without read-back.
 *
 * [SHARED IMAGES 2026-11-08] Stage after the first hybrid: in the first cut OpenGL's finished world, its depth and the
 * UI layer were read back to the CPU through pixel buffer objects every frame and uploaded to Vulkan (16-27 ms a frame
 * at 2522x1520). Here the three layers are Vulkan images whose memory OpenGL imports (GL_EXT_memory_object +
 * GL_EXT_memory_object_fd / _win32), so GL writes them directly, and the two sides take turns through two exported
 * binary semaphores (GL_EXT_semaphore + GL_EXT_semaphore_fd / _win32): nothing crosses the bus, no one waits on the CPU.
 *
 * The rules come from the vkglshare probe of 2026-11-08 (memory vulkan-gl-image-sharing-probe-2026-11-08), which passed
 * every pixel round trip on NVIDIA 615.78 and RADV Mesa 26.2.5:
 *   - optimal tiling only (linear tiling fails on NVIDIA GL: GL_INVALID_VALUE);
 *   - one dedicated allocation per image (RADV requires it; GL_DEDICATED_MEMORY_OBJECT_EXT set TRUE before the import);
 *   - depth as D32_SFLOAT (RADV has no D24 format at all), never linear;
 *   - the Vulkan device is the one whose UUID matches GL_DEVICE_UUID_EXT (WolfVkDevice::Config::mGLDeviceUUID);
 *   - ownership crosses the boundary with queue family ownership transfers to/from VK_QUEUE_FAMILY_EXTERNAL
 *     (vulkan_core.h:5477) and the layouts GL is told in glSignalSemaphoreEXT / glWaitSemaphoreEXT;
 *   - the hand-over is a queue wait (semaphores), never a CPU wait.
 *
 * Frame protocol (one frame):
 *   GL, after renderFinalize (WolfVkScene::captureWorld):
 *     glWaitSemaphoreEXT(vk_done, {world, ui, depth}, layouts)   - Vulkan has finished reading last frame's layers
 *     glBlitFramebuffer(default framebuffer -> world)             - the finished world, GPU to GPU
 *     depth copy pass: deferredScreen's depth (GL_DEPTH_COMPONENT24, window-rect sized) -> the shared D32F image,
 *                      at the world view rect's offset inside the window
 *     (the caller clears the default framebuffer to (0,0,0,0); the UI then draws onto a transparent layer)
 *   GL, at swap (WolfVkScene::readBackUI):
 *     glBlitFramebuffer(default framebuffer -> ui)
 *     glSignalSemaphoreEXT(gl_done, {world, ui, depth}, layouts)  - releases the layers to Vulkan ("as if Flush")
 *   Vulkan, the frame's submit:
 *     waits gl_done; acquire barriers EXTERNAL -> graphics family (world/ui -> SHADER_READ_ONLY, depth stays
 *     DEPTH_STENCIL_ATTACHMENT); scene pass with the shared depth as its depth attachment; composite; release
 *     barriers graphics family -> EXTERNAL back to the hand layouts; signals vk_done (and the swapchain's render-done).
 *   The semaphores are binary: every signal is matched by exactly one wait. A frame Vulkan cannot draw (no swapchain
 *   image) still consumes gl_done and signals vk_done with the slot's own submit (WolfVkBridge closeFrameWithoutImage,
 *   WolfVkDevice::endFrame without an image) so the protocol stays balanced.
 *   [HAND-OVER 2026-11-08 8.2k] Vulkan waits on gl_done only after a glSignalSemaphoreEXT that raised no GL error
 *   (glCaptureUIAndSignal returns true), acquires the layers only then, and GL waits on vk_done only after a Vulkan submit
 *   that signals it succeeded (a failed submit falls back). These queue-side waits take no timeout - neither API offers
 *   one - so they are safe by construction only; every CPU-side wait around them is bounded (WolfVkDevice hang guard).
 *   At creation Vulkan transitions the fresh images (UNDEFINED -> hand layouts, released to EXTERNAL) and signals
 *   vk_done once, so GL's first wait has something to wait for.
 *
 * GL entry points are fetched at run time (SDL_GL_GetProcAddress / wglGetProcAddress), as wolfvkbridge.cpp does for
 * glGetUnsignedBytevEXT; typedefs and tokens from GL/glext.h (the system header on Linux, the viewer's copy elsewhere):
 *   PFNGLCREATEMEMORYOBJECTSEXTPROC :7743, PFNGLDELETEMEMORYOBJECTSEXTPROC :7741, PFNGLMEMORYOBJECTPARAMETERIVEXTPROC :7744,
 *   PFNGLTEXSTORAGEMEM2DEXTPROC :7746, PFNGLIMPORTMEMORYFDEXTPROC :7784, PFNGLIMPORTMEMORYWIN32HANDLEEXTPROC :7801,
 *   PFNGLGENSEMAPHORESEXTPROC :8073, PFNGLDELETESEMAPHORESEXTPROC :8074, PFNGLWAITSEMAPHOREEXTPROC :8078,
 *   PFNGLSIGNALSEMAPHOREEXTPROC :8079, PFNGLIMPORTSEMAPHOREFDEXTPROC :8093, PFNGLIMPORTSEMAPHOREWIN32HANDLEEXTPROC :8103;
 *   GL_TEXTURE_TILING_EXT 0x9580 :7728, GL_DEDICATED_MEMORY_OBJECT_EXT 0x9581 :7729, GL_OPTIMAL_TILING_EXT 0x9584 :7733,
 *   GL_HANDLE_TYPE_OPAQUE_FD_EXT 0x9586 :7783, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT 0x9587 :7792,
 *   GL_LAYOUT_COLOR_ATTACHMENT_EXT 0x958E :8065, GL_LAYOUT_DEPTH_STENCIL_ATTACHMENT_EXT 0x958F :8066.
 *
 * Platforms: Linux (fd) and Windows (win32 handles) have complete paths; the Windows path is compiled on Windows only and
 * has not been run. Apple's OpenGL has none of these extensions, so on macOS available() is false and WolfVkScene keeps
 * the read-back hand-over.
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

#ifndef WOLF_VK_SHARE_H
#define WOLF_VK_SHARE_H

#include "vk/wolfvkloader.h"
#include "vk/wolfvkdevice.h"
#include "vk/wolfvkimage.h"
#include "llrect.h"
#include <memory>
#include <string>

// One image shared between Vulkan and OpenGL: a Vulkan image with exported, dedicated memory and the GL texture imported
// over it (GL_EXT_memory_object). Vulkan owns the memory; GL's texture and memory object are deleted here as well.
class WolfVkSharedImage
{
public:
    WolfVkSharedImage() = default;
    ~WolfVkSharedImage();
    WolfVkSharedImage(const WolfVkSharedImage&) = delete;
    WolfVkSharedImage& operator=(const WolfVkSharedImage&) = delete;
    // gl_internal_format: the GL storage format for glTexStorageMem2DEXT (GL_RGBA8, GL_RGBA16, GL_RGBA16F, GL_DEPTH_COMPONENT32F);
    // GL's entry points must have been loaded (WolfVkShare::available).
    bool create(WolfVkDevice& device, const WolfVkImage::Format& format, uint32_t width, uint32_t height, VkImageUsageFlags usage,
                uint32_t gl_internal_format, std::string& error);
    // [VK S6 2026-11-26] Any kind of image, with layers and mip levels (WolfVkImage::createExternal(kind ...)): the GL
    // texture is made on the matching target and gets immutable storage over the image's memory,
    //   TEXTURE_2D -> GL_TEXTURE_2D, TEXTURE_CUBE -> GL_TEXTURE_CUBE_MAP                  glTexStorageMem2DEXT
    //   TEXTURE_2D_ARRAY -> GL_TEXTURE_2D_ARRAY, TEXTURE_CUBE_ARRAY -> GL_TEXTURE_CUBE_MAP_ARRAY (depth = layers, i.e.
    //   layer-faces), TEXTURE_3D -> GL_TEXTURE_3D (depth = layers)                        glTexStorageMem3DEXT
    // ("equivalent to calling TexStorage* ... except that ... the memory at <offset> in the memory object ... will be
    // used", EXT_external_objects; the targets are TexStorage2D's and TexStorage3D's, OpenGL 4.6 core 8.19).
    // layers: 1 for 2D, 6 for a cube, N, 6 x cubes, or the depth. False with error when the driver cannot export or
    // import it; nothing is left behind then. glTarget() says which target the texture has.
    bool createKind(WolfVkDevice& device, WolfVkImage::Kind kind, const WolfVkImage::Format& format, uint32_t width, uint32_t height,
                    uint32_t layers, uint32_t mip_levels, VkImageUsageFlags usage, uint32_t gl_internal_format, std::string& error);
    uint32_t glTarget() const { return mGlTarget; }
    // The hand-over of this image between the two renderers, for every level and layer at once. OpenGL is told the
    // layout in its semaphore call (glLayout() of the same VkImageLayout); Vulkan records a queue family transfer:
    //   acquireFromGL  VK_QUEUE_FAMILY_EXTERNAL -> the device's family, gl_layout -> vk_layout, for dst_stages/dst_access
    //   releaseToGL    the device's family -> VK_QUEUE_FAMILY_EXTERNAL, the image's current layout -> gl_layout
    // exactly as WolfVkShare::acquire/release do for the frame layers (one level, one layer there). The image must have
    // been released once (createKind leaves it in UNDEFINED, owned by Vulkan: the first releaseToGL starts from that).
    void acquireFromGL(VkCommandBuffer cmd, VkImageLayout gl_layout, VkImageLayout vk_layout, VkPipelineStageFlags dst_stages, VkAccessFlags dst_access);
    void releaseToGL(VkCommandBuffer cmd, VkImageLayout gl_layout);
    // [VK S6 2026-11-26] The FIRST hand-over of a fresh image (createKind leaves it UNDEFINED and Vulkan's), done here
    // and now, for textures OpenGL starts using at once: Vulkan releases it in gl_layout (one submit, waited for with the
    // device's bound), signalling a semaphore of this image's own, and OpenGL waits that semaphore with the texture and
    // the layout (glWaitSemaphoreEXT: "<srcLayouts> specifies the layouts the textures are in"). One signal, one wait,
    // the signal submitted and run before the wait is issued, so neither side can wait for ever. The semaphore lives as
    // long as the image and is never used again. False with error when a call failed (the image must then be destroyed).
    // glHandLayout(): the layout of that hand-over, i.e. the one a later acquireFromGL starts from when OpenGL has not
    // named another in a glSignalSemaphoreEXT since.
    bool handToGL(VkImageLayout gl_layout, std::string& error);
    VkImageLayout glHandLayout() const { return mGlHandLayout; }
    // The device is going away while OpenGL keeps using the texture (the renderer fell back to OpenGL): the Vulkan image
    // is released (deferred; left with a hung device) and the image leaves find(). The OpenGL texture and memory object
    // stay valid - OpenGL holds its own reference to the memory (probe 2026-11-26 section D, NVIDIA and RADV: texture
    // exact after the Vulkan image was destroyed and its memory freed) - until destroy().
    void releaseVulkanHalf();
    // The GL_LAYOUT_*_EXT token of a Vulkan layout (EXT_external_objects table 4.4; GL/glext.h:8064-8072); GL_NONE (0)
    // for VK_IMAGE_LAYOUT_UNDEFINED and for a layout the table does not list.
    static uint32_t glLayout(VkImageLayout layout);
    // The shared image whose OpenGL texture has this name (made by create or createKind and still alive), else null:
    // code that holds only a raw GL texture name (the Wolf sky, the terrain layers) finds the Vulkan image through it.
    static WolfVkSharedImage* find(uint32_t gl_texture);
    // glTexStorageMem3DEXT is present (looked up apart from the entry points the shared layers need).
    static bool arrayImportAvailable();
    void destroy();
    bool isValid() const { return mImage && mImage->isValid() && mGlTexture != 0; }
    WolfVkImage* image() { return mImage.get(); }
    const WolfVkImage* image() const { return mImage.get(); }
    uint32_t glTexture() const { return mGlTexture; }
    uint32_t width() const { return mImage ? mImage->width() : 0; }
    uint32_t height() const { return mImage ? mImage->height() : 0; }

    // [IMPORT SELF-TEST 2026-11-10] One round trip through the shared memory, with the GL context current and the image
    // released to GL in its hand layout (COLOR_ATTACHMENT / DEPTH_STENCIL_ATTACHMENT, as createLayer/primeVulkan and
    // WolfVkGBuffer::createImage leave it): GL clears the imported texture to a known value (colour 0.25, 0.5, 0.75, 1.0;
    // depth 0.25) through a framebuffer of its own and signals gl_sem_signal with the layout; Vulkan waits vk_wait,
    // acquires the image from VK_QUEUE_FAMILY_EXTERNAL, copies a 4x4 block at its centre to a host buffer, releases it
    // back in the hand layout and signals vk_signal; GL then waits gl_sem_wait (the protocol is balanced again: every
    // signal consumed, the image GL's). The 16 texels are decoded by the image's format and compared with the value
    // GL wrote. gl_waits_first: a vk_signal is already pending for GL to wait first (WolfVkShare after primeVulkan).
    // True = the bytes came back as written; false = not (the import does not carry GL's writes to Vulkan, or a GL
    // or Vulkan call failed). result: one plain line for the log either way. The Windows import (OPAQUE_WIN32 handles,
    // GL_EXT_memory_object_win32) had never been run anywhere when this was written (W102 Windows: black world).
    bool roundTrip(uint32_t gl_sem_signal, uint32_t gl_sem_wait, VkSemaphore vk_wait, VkSemaphore vk_signal, bool gl_waits_first,
                   std::string& result);
private:
    WolfVkDevice* mDevice = nullptr;
    std::unique_ptr<WolfVkImage> mImage;
    uint32_t mGlMemory = 0;
    uint32_t mGlTexture = 0;
    uint32_t mGlTarget = 0x0DE1;   // GL_TEXTURE_2D (GL/gl.h); createKind sets the target it used
    // [VK S6 2026-11-26] handToGL
    VkSemaphore mHandSemaphore = VK_NULL_HANDLE;
    uint32_t mGlHandSemaphore = 0;
    VkImageLayout mGlHandLayout = VK_IMAGE_LAYOUT_UNDEFINED;
};

class WolfVkShare
{
public:
    WolfVkShare() = default;
    ~WolfVkShare();
    WolfVkShare(const WolfVkShare&) = delete;
    WolfVkShare& operator=(const WolfVkShare&) = delete;

    // The GL context and the Vulkan device can share: GL has the memory-object and semaphore extensions of this
    // platform, the device has the export extensions. why: the first missing piece, for the log and the floater.
    static bool available(const WolfVkDevice& device, std::string& why);

    // Makes the three layers (width x height = the window) and, the first time, the two semaphores; false (with error)
    // when anything cannot be shared - the caller then stays on the read-back path.
    // self_test: run selfTest() before returning (false with its error when the round trip fails).
    bool init(WolfVkDevice& device, uint32_t width, uint32_t height, bool self_test, std::string& error);
    bool resize(uint32_t width, uint32_t height, std::string& error);
    void destroy();
    bool isValid() const { return mWorld.mShared.isValid() && mUI.mShared.isValid() && mDepth.mShared.isValid() && mGlDone != VK_NULL_HANDLE; }
    uint32_t width() const { return mWidth; }
    uint32_t height() const { return mHeight; }

    // ---- GL side (called with the GL context current) -------------------------------------------------------------
    // After renderFinalize: waits for Vulkan's last use, copies the default framebuffer into the world layer and
    // deferredScreen's depth (gl_depth_texture, depth_width x depth_height, GL_DEPTH_COMPONENT24) into the depth layer
    // at world_rect (the world view rect inside the window; gGLViewport in renderFinalize). The default framebuffer is
    // left as it was; the caller clears it for the UI. False when nothing was captured (layers not valid) or GL's wait
    // on vk_done raised a GL error (then broken() is set).
    bool glCaptureWorld(uint32_t gl_depth_texture, uint32_t depth_width, uint32_t depth_height, const LLRect& world_rect);
    // At swap: copies the default framebuffer (the UI on its clear layer) into the ui layer and signals gl_done.
    // [HAND-OVER 2026-11-08 8.2k] True only when gl_done was really signalled: the Vulkan submit may then wait on it and
    // acquire the layers. False (nothing signalled, glSignalledPending() stays false) when the layers are not valid, or
    // when a GL semaphore call raised a GL error - then broken() is set and the hand-over must not be used again.
    bool glCaptureUIAndSignal();
    // [HAND-OVER 2026-11-08 8.2k] A GL semaphore call (glWaitSemaphoreEXT / glSignalSemaphoreEXT) raised a GL error: which
    // signals and waits really happened is no longer known, and a Vulkan wait on a signal that never happened would wedge
    // the card (VUID-vkQueueSubmit-pWaitSemaphores-03238), so the owner falls back to OpenGL.
    bool broken() const { return !mBrokenReason.empty(); }
    const std::string& brokenReason() const { return mBrokenReason; }

    // [HANG GUARD 2026-11-08 8.2k] glFinish with a time limit: a fence sync waited at most WolfVkDevice::FRAME_WAIT_LIMIT_NS
    // (glClientWaitSync). GL's queue can wait on a Vulkan semaphore, so a plain glFinish can block for ever on a stuck
    // card; running out of time marks the device hung (WolfVkDevice::declareHung) and returns false.
    static bool glFinishBounded(WolfVkDevice* device, const char* what);

    // ---- Vulkan side -----------------------------------------------------------------------------------------------
    // True when GL signalled gl_done since Vulkan last waited on it: the frame's submit must wait on it (and must
    // signal vk_done). Both go through sync(): waits/stages/signals for WolfVkDevice::endFrame or submitSync.
    bool glSignalledPending() const { return mGlSignalled; }
    WolfVkDevice::ExtraSync sync();
    // Acquire the layers from GL (EXTERNAL -> graphics family): world and ui to SHADER_READ_ONLY_OPTIMAL for sampling,
    // depth kept in DEPTH_STENCIL_ATTACHMENT_OPTIMAL for the scene pass. Records barriers on cmd.
    void acquire(VkCommandBuffer cmd);
    // Release them back to GL (graphics family -> EXTERNAL, the hand layouts). Records barriers on cmd.
    void release(VkCommandBuffer cmd);

    WolfVkImage* world() { return mWorld.mImage; }
    WolfVkImage* ui() { return mUI.mImage; }
    WolfVkImage* depth() { return mDepth.mImage; }
    // [IMPORT SELF-TEST 2026-11-10] after init: the world (RGBA8) and depth (D32F) layers each make one round trip
    // (WolfVkSharedImage::roundTrip with the frame semaphores). False with the failing layer's line in error: the owner
    // drops the shared hand-over for the read-back one. Lines of both layers in result. Run by init() when
    // WolfVulkanShareSelfTest is on; the protocol state after it equals the state after primeVulkan's signal was consumed
    // (mVkSignalled false: the next frame's Vulkan submit signals vk_done without GL having to wait first).
    bool selfTest(std::string& result, std::string& error);
    const std::string& selfTestResult() const { return mSelfTestResult; }
    // [BLACK WORLD 2026-11-10] "GL_EXT_memory_object ... present" lines etc. for the log, once
    static void logExtensions(const WolfVkDevice& device);
    // The GL entry points for sharing, loaded once (false when any is missing). WolfVkSharedImage needs them.
    static bool loadGL();
    const LLRect& worldRect() const;

    // ---- [VK S6a 2026-11-26] the OpenGL half of the resource store (wolfvkresources.cpp) -----------------------------
    // All need the GL context current and available() true. GL_EXT_memory_object / GL_EXT_semaphore (texts read
    // 2026-11-26, registry.khronos.org EXT_external_objects.txt, _fd.txt, _win32.txt).
    // A GL memory object over the whole of a Vulkan allocation of size bytes that was made exportable
    // (VkExportMemoryAllocateInfo). dedicated: the allocation is a dedicated one (GL_DEDICATED_MEMORY_OBJECT_EXT "must
    // be set to TRUE" then). 0 with error when the export or the import fails.
    static uint32_t glImportMemory(WolfVkDevice& device, VkDeviceMemory memory, VkDeviceSize size, bool dedicated, std::string& error);
    static void glDeleteMemory(uint32_t gl_memory);
    // A GL buffer whose data store is size bytes of gl_memory from offset (glBufferStorageMemEXT). OpenGL may only
    // read it: such a buffer cannot be mapped (issue 12) and no write call is promised to work on it. 0 with error.
    // Bound through GL_COPY_WRITE_BUFFER, which LLVertexBuffer does not track; that binding is left at 0.
    static uint32_t glBufferOverMemory(uint32_t gl_memory, VkDeviceSize offset, VkDeviceSize size, std::string& error);
    // glBufferStorageMemEXT is present (it belongs to GL_EXT_memory_object, but the entry point is looked up apart from
    // the ones the shared layers need).
    static bool glBufferImportAvailable();
    // An exported binary Vulkan semaphore and the GL semaphore imported from it.
    static bool glCreateSemaphore(WolfVkDevice& device, VkSemaphore& vk_semaphore, uint32_t& gl_semaphore, std::string& error);
    static void glDeleteSemaphore(uint32_t gl_semaphore);
    // glSignalSemaphoreEXT / glWaitSemaphoreEXT with buffer barriers only (count names at buffers; may be 0/null).
    // False with error when GL raised an error: the semaphore's state is then unknown.
    static bool glSignalBuffers(uint32_t gl_semaphore, uint32_t count, const uint32_t* buffers, std::string& error);
    static bool glWaitBuffers(uint32_t gl_semaphore, uint32_t count, const uint32_t* buffers, std::string& error);
    // [VK S6 2026-11-26] The same with texture barriers only, for the hand-over of shared images (WolfVkSharedImage /
    // WolfVkTextures::find) between the two renderers: count (>= 1) OpenGL texture names at textures and, for each,
    // the Vulkan layout at layouts; OpenGL is given its GL_LAYOUT_*_EXT token (WolfVkSharedImage::glLayout; a layout
    // with no token, such as VK_IMAGE_LAYOUT_UNDEFINED, is refused).
    //   glSignalTextures: OpenGL's work on the textures so far is ahead of the signal and they are left in those
    //     layouts ("<dstLayouts> specifies the layouts the textures are transitioned to"); the Vulkan submit that
    //     waits the semaphore records acquireFromGL(that layout ...). The call flushes OpenGL's stream.
    //   glWaitTextures: OpenGL's later commands wait for the Vulkan submit that signals the semaphore and recorded
    //     releaseToGL(that layout) ("<srcLayouts> specifies the layouts the textures are in").
    // Order, always: issue the signal before the wait that consumes it (glSignalTextures, then the Vulkan submit
    // waiting it; the Vulkan submit signalling, then glWaitTextures), one wait per signal. False with error when
    // OpenGL raised an error: the semaphore's state is then unknown and it must not be used again.
    static bool glSignalTextures(uint32_t gl_semaphore, uint32_t count, const uint32_t* textures, const VkImageLayout* layouts, std::string& error);
    static bool glWaitTextures(uint32_t gl_semaphore, uint32_t count, const uint32_t* textures, const VkImageLayout* layouts, std::string& error);

private:
    struct Layer
    {
        WolfVkSharedImage mShared;
        WolfVkImage* mImage = nullptr;   // = mShared.image(), for the barriers
        uint32_t mGlFbo = 0;             // GL framebuffer with the texture attached (colour or depth)
        bool mDepth = false;
        uint32_t glTexture() const { return mShared.glTexture(); }
    };
    bool createLayer(Layer& layer, uint32_t width, uint32_t height, bool depth, std::string& error);
    void destroyLayer(Layer& layer);
    bool createSemaphores(std::string& error);
    bool createDepthCopyProgram(std::string& error);
    void destroyGLProgram();
    // first use: the images' layouts and the first vk_done signal
    bool primeVulkan(std::string& error);
    void imageBarrier(VkCommandBuffer cmd, Layer& layer, VkImageLayout old_layout, VkImageLayout new_layout,
                      VkAccessFlags src_access, VkAccessFlags dst_access, VkPipelineStageFlags src_stages, VkPipelineStageFlags dst_stages,
                      uint32_t src_family, uint32_t dst_family);
    void glHandLayouts(uint32_t textures[3], uint32_t layouts[3]) const;

    WolfVkDevice* mDevice = nullptr;
    uint32_t mWidth = 0, mHeight = 0;
    Layer mWorld, mUI, mDepth;
    VkSemaphore mGlDone = VK_NULL_HANDLE;   // GL signals, Vulkan waits
    VkSemaphore mVkDone = VK_NULL_HANDLE;   // Vulkan signals, GL waits
    uint32_t mGlSemGlDone = 0, mGlSemVkDone = 0;
    bool mGlSignalled = false;   // gl_done signalled, not yet waited by Vulkan
    bool mVkSignalled = false;   // vk_done signalled, not yet waited by GL
    std::string mBrokenReason;   // set when a GL semaphore call failed (broken())
    // the depth copy: a GL program writing gl_FragDepth from deferredScreen's depth texture
    uint32_t mDepthProgram = 0;
    uint32_t mDepthVao = 0;   // core profile: a vertex array must be bound to draw, even with no arrays
    int32_t mDepthTexLoc = -1, mDepthOriginLoc = -1;
    std::unique_ptr<LLRect> mWorldRect;
    std::string mSelfTestResult;   // [IMPORT SELF-TEST 2026-11-10]
};

#endif // WOLF_VK_SHARE_H
