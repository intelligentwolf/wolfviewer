/**
 * @file wolfvkswapchain.h
 * @brief Swapchain on the window surface: creation, recreation on resize, vsync, format choice.
 *
 * [VULKAN BACKEND 2026-11-08]
 *
 * Format: the viewer's OpenGL default framebuffer is 8-bit RGBA with no sRGB encoding (SDL_GL_RED_SIZE 8 ...,
 * llwindowsdl2.cpp:862-872; the viewer applies gamma itself in its final pass, gammaF.glsl / renderFinalize). So the
 * swapchain asks for a UNORM 8-bit format first: VK_FORMAT_B8G8R8A8_UNORM (vulkan_core.h:1903) or
 * VK_FORMAT_R8G8B8A8_UNORM (:1896) with VK_COLOR_SPACE_SRGB_NONLINEAR_KHR (:8956), which every driver offers
 * (spec: "the surface must support at least one format"). Writing already-gamma-encoded colour to a UNORM image
 * shows it as is, exactly like the GL window. An _SRGB image (:1909/:1902) would re-encode and double the gamma; it
 * is accepted only as a last resort, and then the composite shader is told to write linear (WolfVkSwapchain::isSrgb).
 *
 * Present mode (vsync): RenderVSyncEnable (settings.xml:5046) maps to VK_PRESENT_MODE_FIFO_KHR (:8946, always
 * available, waits for the display) when on, and to VK_PRESENT_MODE_MAILBOX_KHR (:8945) or
 * VK_PRESENT_MODE_IMMEDIATE_KHR (:8944), whichever the surface offers, when off; FIFO if neither exists.
 *
 * Recreation: VK_ERROR_OUT_OF_DATE_KHR (:170) or VK_SUBOPTIMAL_KHR (:169) from acquire/present, or a size change,
 * rebuilds the swapchain after the device went idle; the old one is passed as oldSwapchain.
 *
 * [PRESENT FENCES 2026-11-08] Retiring the old swapchain. Our command buffers are covered by the device's bounded idle
 * wait, but a present is not a command buffer: run nv-hang2 had VUID-vkDestroySwapchainKHR-swapchain-01282 ("in use by
 * VkQueue") and VUID-vkDestroySemaphore-semaphore-05149 at every recreate, because the old swapchain and its render-done
 * semaphores were destroyed while presents could still hold them. Now the old swapchain and its semaphores are kept
 * (Retired) and destroyed only once its presents are known to be finished:
 *   - with VK_KHR/EXT_swapchain_maintenance1 (WolfVkDevice::presentFences, both test GPUs): every present carries a fence
 *     (VkSwapchainPresentFenceInfoKHR, vulkan_core.h:13447); a retired swapchain goes when its last present fence has
 *     signalled - they signal in present order ("Fences associated with presentations to the same swapchain on the same
 *     VkQueue must be signaled in the same order as the present operations", VkSwapchainPresentFenceInfoEXT, as quoted by
 *     ANGLE SurfaceVk.cpp:930-932, chromium third_party/angle on this machine);
 *   - without it there is no fence to wait for; the old swapchain goes when the first image presented on its successor
 *     has been acquired again, which is ANGLE's rule for the same case (SurfaceVk.cpp:1521-1523 "scheduled for destruction
 *     when the semaphore of the first image of the next swapchain can be recycled"), and at shutdown or past the retired
 *     cap after WolfVkDevice::waitIdle ([V4 2026-11-11] was vkQueueWaitIdle, which takes no timeout; the spec defines
 *     it as a fence wait on every submit that accepts a fence, which waitIdle does with the 8 s limit; that path is not
 *     used on either test GPU).
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

#ifndef WOLF_VK_SWAPCHAIN_H
#define WOLF_VK_SWAPCHAIN_H

#include "wolfvkloader.h"
#include <string>
#include <vector>

class WolfVkDevice;

class WolfVkSwapchain
{
public:
    WolfVkSwapchain() = default;
    ~WolfVkSwapchain();

    // width/height: the window's drawable size in pixels (LLWindow::getSize(LLCoordWindow)), used when the surface
    // reports no fixed extent (Wayland). vsync: RenderVSyncEnable.
    bool create(WolfVkDevice& device, VkSurfaceKHR surface, uint32_t width, uint32_t height, bool vsync, std::string& error);
    // Same parameters; waits for the device, keeps the old swapchain as oldSwapchain, rebuilds views.
    bool recreate(uint32_t width, uint32_t height, bool vsync, std::string& error);
    // [AMD PERF 2026-11-08] testing: the present mode to use instead of the vsync rule when the surface offers it
    // (VK_PRESENT_MODE_MAX_ENUM_KHR = no override; WolfVulkanPresentMode in wolfvkbridge.cpp)
    void setPresentModeOverride(VkPresentModeKHR mode) { mPresentOverride = mode; }
    // [V4 2026-11-11] true when no swapchain (current or retired) is left alive; false when one had to be left to the
    // device (hung, or its presents did not finish within the bounded wait) - the surface must then not be destroyed
    bool destroy();
    // [V1 2026-11-11] the last create/recreate failed only because the window has no size (minimised): wait, not an error
    bool noSize() const { return mNoSize; }

    // Acquires the next image, signalling the frame's acquire semaphore. Returns false with out_of_date = true when
    // the swapchain must be recreated (nothing acquired); false with timed_out = true when no image came within
    // ACQUIRE_WAIT_NS (skip the frame, try again); false with both false on a real error.
    bool acquire(VkSemaphore signal, uint32_t& image_index, bool& out_of_date, bool& timed_out, std::string& error);
    static constexpr uint64_t ACQUIRE_WAIT_NS = 100ull * 1000000ull;   // 100 ms (reasons at acquire())
    // Presents image_index after render_done; out_of_date as above (the frame was still presented when the result was
    // VK_SUBOPTIMAL_KHR).
    bool present(VkQueue queue, VkSemaphore render_done, uint32_t image_index, bool& out_of_date, std::string& error);

    bool isValid() const { return mSwapchain != VK_NULL_HANDLE; }
    VkFormat format() const { return mFormat.format; }
    VkImageUsageFlags usage() const { return mUsage; }   // [G2 2026-11-11] TRANSFER_SRC when the surface offers it
    bool isSrgb() const { return mFormat.format == VK_FORMAT_B8G8R8A8_SRGB || mFormat.format == VK_FORMAT_R8G8B8A8_SRGB; }
    VkExtent2D extent() const { return mExtent; }
    uint32_t imageCount() const { return static_cast<uint32_t>(mImages.size()); }
    VkImage image(uint32_t i) const { return mImages[i]; }
    VkImageView view(uint32_t i) const { return mViews[i]; }
    // The "rendering finished" binary semaphore of image i: signalled by the frame's submit, waited by its present.
    // One per swapchain image, not per frame in flight: a frame slot comes round again after its fence, but the present
    // of two frames ago may still hold the wait on a per-frame semaphore (VUID-vkQueueSubmit-pSignalSemaphores-00067, seen
    // with the immediate present mode on NVIDIA 2026-11-08); the presentation engine releases an image's semaphore
    // before that image can be acquired again, so per-image semaphores are always unsignalled when re-signalled.
    VkSemaphore renderDone(uint32_t i) const { return mRenderDone[i]; }
    VkPresentModeKHR presentMode() const { return mPresentMode; }
    const char* presentModeString() const;
    const char* formatString() const;

private:
    bool build(uint32_t width, uint32_t height, bool vsync, VkSwapchainKHR old, std::string& error);
    void destroyViews();
    void discardPartialBuild();   // [V3 2026-11-11] build() failed after vkCreateSwapchainKHR succeeded
    void destroySemaphores(std::vector<VkSemaphore>& semaphores);

    // [PRESENT FENCES 2026-11-08] see the header comment
    struct Retired
    {
        VkSwapchainKHR mSwapchain = VK_NULL_HANDLE;
        std::vector<VkSemaphore> mRenderDone;   // its images' render-done semaphores, still possibly waited by its presents
        std::vector<VkFence> mPresentFences;    // with maintenance1: its presents' fences, in present order
        int32_t mGateImage = -1;                // without: the image index of the first present on its successor
    };
    // fences of the current swapchain's presents, in present order, not yet seen signalled
    std::vector<VkFence> mPendingPresentFences;
    std::vector<VkFence> mFreeFences;           // signalled and reset, for the next presents
    std::vector<Retired> mRetired;
    bool mFirstPresentDone = false;             // a present on the current swapchain went to the queue
    // Moves fences that have signalled (checked in order, without waiting) back to mFreeFences and destroys every
    // retired swapchain whose presents are finished. Called after each present and before each recreate.
    void collectRetired();
    // Waits (bounded where the API allows) until every retired swapchain can go, then destroys them; also the current
    // swapchain's own present fences. False when a fence wait ran out (the objects are then left alone).
    bool drainPresents();
    VkFence takePresentFence();
    void releaseRetired(Retired& r);
    static constexpr size_t MAX_RETIRED = 4;   // ANGLE SurfaceVk.cpp:1559-1563 (kMaxOldSwapchains 5): NVIDIA fails swapchain creation past ~20 live
    WolfVkDevice* mDevice = nullptr;
    VkSurfaceKHR mSurface = VK_NULL_HANDLE;
    VkSwapchainKHR mSwapchain = VK_NULL_HANDLE;
    VkSurfaceFormatKHR mFormat{};
    VkPresentModeKHR mPresentMode = VK_PRESENT_MODE_FIFO_KHR;
    VkPresentModeKHR mPresentOverride = VK_PRESENT_MODE_MAX_ENUM_KHR;
    VkExtent2D mExtent{};
    bool mNoSize = false;
    VkImageUsageFlags mUsage = 0;
    std::vector<VkImage> mImages;
    std::vector<VkImageView> mViews;
    std::vector<VkSemaphore> mRenderDone;
};

#endif // WOLF_VK_SWAPCHAIN_H
