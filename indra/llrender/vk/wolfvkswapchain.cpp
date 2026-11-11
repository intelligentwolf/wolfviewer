/**
 * @file wolfvkswapchain.cpp
 * @brief Swapchain on the window surface (see wolfvkswapchain.h).
 *
 * [VULKAN BACKEND 2026-11-08]
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

#include "linden_common.h"
#include "wolfvkswapchain.h"
#include "wolfvkdevice.h"
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <sstream>

WolfVkSwapchain::~WolfVkSwapchain()
{
    destroy();
}

bool WolfVkSwapchain::create(WolfVkDevice& device, VkSurfaceKHR surface, uint32_t width, uint32_t height, bool vsync, std::string& error)
{
    // [PRESENT FENCES 2026-11-08] after a rebuild that failed (minimised window: no swapchain, but retired ones may still
    // wait for their presents) this is a recreate, not a fresh start - destroy() would wait up to 8 s for those presents
    // on every retry while the window stays minimised
    if (mDevice == &device && mSurface == surface) return recreate(width, height, vsync, error);
    destroy();
    mDevice = &device;
    mSurface = surface;
    return build(width, height, vsync, VK_NULL_HANDLE, error);
}

bool WolfVkSwapchain::recreate(uint32_t width, uint32_t height, bool vsync, std::string& error)
{
    if (!mDevice || mSurface == VK_NULL_HANDLE)
    {
        error = "recreate before create";
        return false;
    }
    // Spec (VkSwapchainCreateInfoKHR::oldSwapchain): the old swapchain may still have images presented; its images
    // and views must not be in use by pending commands when destroyed, so the device goes idle first.
    mDevice->waitIdleAndFlushDeferred();
    if (mDevice->hung())
    {
        // [HANG GUARD 2026-11-08 8.2k] the bounded idle wait ran out: the images may still be in use, so nothing is
        // destroyed (VUID-vkDestroySwapchainKHR-swapchain-01282); the caller falls back on hung()
        error = "the graphics card did not finish its work before the swapchain was rebuilt (" + mDevice->hungReason() + ")";
        return false;
    }
    // [PRESENT FENCES 2026-11-08] the views are named only by our command buffers (finished above) and go now; the
    // swapchain itself and its render-done semaphores may still be held by presents, so they are retired, not destroyed
    // (header comment). The retired swapchain is passed as oldSwapchain as before.
    collectRetired();
    std::vector<VkSemaphore> old_semaphores = mRenderDone;
    mRenderDone.clear();
    destroyViews();
    VkSwapchainKHR old = mSwapchain;
    mSwapchain = VK_NULL_HANDLE;
    const bool ok = build(width, height, vsync, old, error);
    if (old != VK_NULL_HANDLE || !old_semaphores.empty())
    {
        Retired r;
        r.mSwapchain = old;
        r.mRenderDone = old_semaphores;
        r.mPresentFences.swap(mPendingPresentFences);
        if (!mFirstPresentDone && r.mPresentFences.empty())
        {
            // never presented: nothing can hold it (ANGLE SurfaceVk.cpp:1505-1518 destroys such a swapchain at once)
            releaseRetired(r);
        }
        else
        {
            mRetired.push_back(r);
        }
    }
    mPendingPresentFences.clear();
    mFirstPresentDone = false;
    // without present fences the older retired swapchains wait for the NEW swapchain's first present (ANGLE moves them
    // under it too, SurfaceVk.cpp:1540-1544)
    if (!mDevice->presentFences()) for (Retired& r : mRetired) r.mGateImage = -1;
    if (mRetired.size() > MAX_RETIRED) drainPresents();
    return ok;
}

bool WolfVkSwapchain::build(uint32_t width, uint32_t height, bool vsync, VkSwapchainKHR old, std::string& error)
{
    mNoSize = false;
    VkPhysicalDevice physical = mDevice->physical();
    VkDevice device = mDevice->device();

    // Source: vulkan_core.h:9002 VkSurfaceCapabilitiesKHR { minImageCount, maxImageCount, currentExtent, minImageExtent,
    // maxImageExtent, maxImageArrayLayers, supportedTransforms, currentTransform, supportedCompositeAlpha, supportedUsageFlags }
    VkSurfaceCapabilitiesKHR caps{};
    VkResult res = gVk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, mSurface, &caps);
    if (res != VK_SUCCESS)
    {
        error = std::string("vkGetPhysicalDeviceSurfaceCapabilitiesKHR: ") + WolfVkLoader::resultString(res);
        return false;
    }

    // Formats. Source: vulkan_core.h:9015 VkSurfaceFormatKHR { format, colorSpace }
    std::vector<VkSurfaceFormatKHR> formats;
    {
        uint32_t n = 0;
        gVk.vkGetPhysicalDeviceSurfaceFormatsKHR(physical, mSurface, &n, nullptr);
        formats.resize(n);
        if (n) gVk.vkGetPhysicalDeviceSurfaceFormatsKHR(physical, mSurface, &n, formats.data());
        formats.resize(n);
    }
    if (formats.empty())
    {
        error = "the surface offers no formats";
        return false;
    }
    // Preference order, see the header: UNORM 8-bit first, sRGB-encoding last.
    const VkFormat wanted[] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                                VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB };
    bool found = false;
    for (VkFormat f : wanted)
    {
        for (const VkSurfaceFormatKHR& sf : formats)
        {
            if (sf.format == f && sf.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            {
                mFormat = sf;
                found = true;
                break;
            }
        }
        if (found) break;
    }
    if (!found)
    {
        mFormat = formats.front();
        LL_WARNS("Vulkan") << "Swapchain: none of the preferred formats offered, using format " << mFormat.format
                           << " colour space " << mFormat.colorSpace << LL_ENDL;
    }

    // Present modes.
    std::vector<VkPresentModeKHR> modes;
    {
        uint32_t n = 0;
        gVk.vkGetPhysicalDeviceSurfacePresentModesKHR(physical, mSurface, &n, nullptr);
        modes.resize(n);
        if (n) gVk.vkGetPhysicalDeviceSurfacePresentModesKHR(physical, mSurface, &n, modes.data());
        modes.resize(n);
    }
    // Source: vulkan_core.h:8944-8947 VkPresentModeKHR; the spec requires FIFO to be supported.
    mPresentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (!vsync)
    {
        if (std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_MAILBOX_KHR) != modes.end())
        {
            mPresentMode = VK_PRESENT_MODE_MAILBOX_KHR;
        }
        else if (std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_IMMEDIATE_KHR) != modes.end())
        {
            mPresentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        }
    }
    if (mPresentOverride != VK_PRESENT_MODE_MAX_ENUM_KHR && std::find(modes.begin(), modes.end(), mPresentOverride) != modes.end())
    {
        mPresentMode = mPresentOverride;   // [AMD PERF] testing override, only a mode the surface offers
    }

    // Extent: the spec says currentExtent of 0xFFFFFFFF means the surface size is set by the swapchain (Wayland);
    // otherwise it must match currentExtent.
    if (caps.currentExtent.width != 0xFFFFFFFFu)
    {
        mExtent = caps.currentExtent;
    }
    else
    {
        mExtent.width = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
        mExtent.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    // [V1 2026-11-11] VkSwapchainCreateInfoKHR imageExtent: "On some platforms, it is normal that maxImageExtent may become
    // (0, 0), for example when the window is minimized" - then "it is not possible to create a swapchain" (VUID-
    // VkSwapchainCreateInfoKHR-imageExtent-01689: width and height must both be non-zero). Only this case is a wait for
    // the window to come back (noSize()); every other failure below is a real error the caller must not retry for ever.
    if (mExtent.width == 0 || mExtent.height == 0 || caps.maxImageExtent.width == 0 || caps.maxImageExtent.height == 0)
    {
        error = "window has no size (minimised)";
        mNoSize = true;
        return false;
    }

    // Image count: minImageCount + 1 for one spare, within maxImageCount (0 = no limit).
    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount)
    {
        image_count = caps.maxImageCount;
    }

    // Composite alpha: opaque when offered (vulkan_core.h:8995), else whatever the surface supports.
    VkCompositeAlphaFlagBitsKHR composite = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR))
    {
        const VkCompositeAlphaFlagBitsKHR order[] = { VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR };   // :8998, :8996
        for (VkCompositeAlphaFlagBitsKHR c : order)
        {
            if (caps.supportedCompositeAlpha & static_cast<VkCompositeAlphaFlagsKHR>(c)) { composite = c; break; }
        }
    }

    // Usage: colour attachment (drawn into), transfer dst (clears/blits) and transfer src (screenshots) when offered.
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;   // :2812, required by the spec for every surface
    if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;   // :2809
    if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;   // :2808
    mUsage = usage;

    // Source: vulkan_core.h:9095 VkSwapchainCreateInfoKHR { sType, pNext, flags, surface, minImageCount, imageFormat, imageColorSpace,
    // imageExtent, imageArrayLayers, imageUsage, imageSharingMode, queueFamilyIndexCount, pQueueFamilyIndices, preTransform,
    // compositeAlpha, presentMode, clipped, oldSwapchain }
    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;   // :473
    info.surface = mSurface;
    info.minImageCount = image_count;
    info.imageFormat = mFormat.format;
    info.imageColorSpace = mFormat.colorSpace;
    info.imageExtent = mExtent;
    info.imageArrayLayers = 1;
    info.imageUsage = usage;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;   // :2264, one queue family
    info.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;   // :8981
    info.compositeAlpha = composite;
    info.presentMode = mPresentMode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = old;

    res = gVk.vkCreateSwapchainKHR(device, &info, nullptr, &mSwapchain);
    if (res != VK_SUCCESS)
    {
        error = std::string("vkCreateSwapchainKHR: ") + WolfVkLoader::resultString(res);
        mSwapchain = VK_NULL_HANDLE;
        // [T724 2026-11-10] everything that was asked for and everything the surface offered, in one block, so a report
        // of a refused swapchain (ticket 724: VK_ERROR_INITIALIZATION_FAILED on NVIDIA X11, nothing else in the log) tells
        // which of the two the driver objected to. Hex for the flag words (VkSurfaceTransformFlagsKHR vulkan_core.h:8981-,
        // VkCompositeAlphaFlagsKHR :8995-, VkImageUsageFlags :2808-).
        auto hex = [](uint32_t v) { char b[16]; snprintf(b, sizeof(b), "0x%x", v); return std::string(b); };
        std::ostringstream o;
        o << "vkCreateSwapchainKHR failed: " << WolfVkLoader::resultString(res) << " (" << static_cast<int32_t>(res) << ")\n"
          << "  asked: minImageCount " << info.minImageCount << ", format " << info.imageFormat << " (" << formatString() << ")"
          << ", colour space " << info.imageColorSpace << ", extent " << info.imageExtent.width << "x" << info.imageExtent.height
          << ", layers " << info.imageArrayLayers << ", usage " << hex(info.imageUsage) << ", sharing " << info.imageSharingMode
          << ", preTransform " << hex(static_cast<uint32_t>(info.preTransform)) << ", compositeAlpha " << hex(static_cast<uint32_t>(info.compositeAlpha))
          << ", presentMode " << info.presentMode << " (" << presentModeString() << ")" << ", clipped " << info.clipped
          << ", oldSwapchain " << (old != VK_NULL_HANDLE ? "yes" : "none") << ", pNext none"
          << "; window drawable size given " << width << "x" << height << (vsync ? ", vsync on" : ", vsync off") << "\n"
          << "  surface capabilities: imageCount " << caps.minImageCount << ".." << caps.maxImageCount << " (0 = no limit)"
          << ", currentExtent " << caps.currentExtent.width << "x" << caps.currentExtent.height
          << ", imageExtent " << caps.minImageExtent.width << "x" << caps.minImageExtent.height << ".." << caps.maxImageExtent.width << "x" << caps.maxImageExtent.height
          << ", maxImageArrayLayers " << caps.maxImageArrayLayers << ", supportedTransforms " << hex(caps.supportedTransforms)
          << ", currentTransform " << hex(static_cast<uint32_t>(caps.currentTransform)) << ", supportedCompositeAlpha " << hex(caps.supportedCompositeAlpha)
          << ", supportedUsageFlags " << hex(caps.supportedUsageFlags) << "\n"
          << "  surface formats (" << formats.size() << "):";
        for (const VkSurfaceFormatKHR& sf : formats) o << " " << sf.format << "/" << sf.colorSpace;
        o << "\n  present modes (" << modes.size() << "):";
        for (VkPresentModeKHR m : modes) o << " " << m;
        LL_WARNS("Vulkan") << o.str() << LL_ENDL;
        return false;
    }

    uint32_t n = 0;
    gVk.vkGetSwapchainImagesKHR(device, mSwapchain, &n, nullptr);
    mImages.resize(n);
    if (n) gVk.vkGetSwapchainImagesKHR(device, mSwapchain, &n, mImages.data());
    mImages.resize(n);

    mViews.resize(n, VK_NULL_HANDLE);
    mRenderDone.resize(n, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < n; ++i)
    {
        // Source: vulkan_core.h:3872 VkSemaphoreCreateInfo (binary)
        VkSemaphoreCreateInfo sem{};
        sem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        res = gVk.vkCreateSemaphore(device, &sem, nullptr, &mRenderDone[i]);
        if (res != VK_SUCCESS)
        {
            error = std::string("vkCreateSemaphore (swapchain): ") + WolfVkLoader::resultString(res);
            discardPartialBuild();
            return false;
        }
        // Source: vulkan_core.h:3939 VkImageViewCreateInfo { sType, pNext, flags, image, viewType, format, components, subresourceRange };
        // :3931 VkImageSubresourceRange { aspectMask, baseMipLevel, levelCount, baseArrayLayer, layerCount }
        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;   // :222
        view.image = mImages[i];
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;                   // :2328
        view.format = mFormat.format;
        view.components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };   // :2316
        view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };   // :2967
        res = gVk.vkCreateImageView(device, &view, nullptr, &mViews[i]);
        if (res != VK_SUCCESS)
        {
            error = std::string("vkCreateImageView (swapchain): ") + WolfVkLoader::resultString(res);
            discardPartialBuild();
            return false;
        }
    }

    LL_INFOS("Vulkan") << "Swapchain: " << mExtent.width << "x" << mExtent.height << ", " << n << " images, "
                       << formatString() << ", " << presentModeString() << (vsync ? " (vsync on)" : " (vsync off)") << LL_ENDL;
    return true;
}

void WolfVkSwapchain::discardPartialBuild()
{
    // [V3 2026-11-11] build() created the swapchain but not all of its views/semaphores. Before, it called destroy(),
    // which nulled mDevice and mSurface - recreate() then dereferenced mDevice (retiring the old swapchain, the present
    // fence test) and crashed, and the swapchain passed as oldSwapchain (retired by the call whatever its result:
    // "oldSwapchain is retired - even if creation of the new swapchain fails", VkSwapchainCreateInfoKHR) was dropped
    // without being destroyed. Now only what this build made goes: its views and semaphores, and the new swapchain, which
    // was never acquired from or presented to (nothing can be using it: VUID-vkDestroySwapchainKHR-swapchain-01282). The
    // device, surface, retired list and present fences stay, so recreate() retires `old` as for any failed build.
    destroyViews();
    destroySemaphores(mRenderDone);
    if (mSwapchain != VK_NULL_HANDLE) gVk.vkDestroySwapchainKHR(mDevice->device(), mSwapchain, nullptr);   // :9172
    mSwapchain = VK_NULL_HANDLE;
}

void WolfVkSwapchain::destroyViews()
{
    if (mDevice && mDevice->device() != VK_NULL_HANDLE)
    {
        for (VkImageView v : mViews)
        {
            if (v != VK_NULL_HANDLE) gVk.vkDestroyImageView(mDevice->device(), v, nullptr);
        }
    }
    mViews.clear();
    mImages.clear();
}

void WolfVkSwapchain::destroySemaphores(std::vector<VkSemaphore>& semaphores)
{
    if (mDevice && mDevice->device() != VK_NULL_HANDLE)
    {
        for (VkSemaphore s : semaphores) if (s != VK_NULL_HANDLE) gVk.vkDestroySemaphore(mDevice->device(), s, nullptr);
    }
    semaphores.clear();
}

// [PRESENT FENCES 2026-11-08] ------------------------------------------------------------------------------------------

VkFence WolfVkSwapchain::takePresentFence()
{
    if (!mFreeFences.empty())
    {
        VkFence f = mFreeFences.back();
        mFreeFences.pop_back();
        return f;   // reset when it was collected: VUID-VkSwapchainPresentFenceInfoKHR-pFences-07758 (unsignaled)
    }
    // Source: vulkan_core.h:3866 VkFenceCreateInfo { sType, pNext, flags }; flags 0 = unsignalled
    VkFenceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence f = VK_NULL_HANDLE;
    if (gVk.vkCreateFence(mDevice->device(), &info, nullptr, &f) != VK_SUCCESS) return VK_NULL_HANDLE;
    return f;
}

void WolfVkSwapchain::releaseRetired(Retired& r)
{
    // every present of r is finished (its fences signalled, or the fallback gate passed): VUID-vkDestroySwapchainKHR-
    // swapchain-01282 and VUID-vkDestroySemaphore-semaphore-05149 hold
    VkDevice dev = mDevice->device();
    destroySemaphores(r.mRenderDone);
    if (r.mSwapchain != VK_NULL_HANDLE) gVk.vkDestroySwapchainKHR(dev, r.mSwapchain, nullptr);   // :9172
    r.mSwapchain = VK_NULL_HANDLE;
    for (VkFence f : r.mPresentFences)
    {
        // signalled, so not tied to an unfinished queue operation (VUID-vkResetFences-pFences-01123)
        gVk.vkResetFences(dev, 1, &f);
        mFreeFences.push_back(f);
    }
    r.mPresentFences.clear();
}

void WolfVkSwapchain::collectRetired()
{
    if (!mDevice || mDevice->device() == VK_NULL_HANDLE || mDevice->hung()) return;
    VkDevice dev = mDevice->device();
    // the current swapchain's present fences, oldest first; they signal in present order, so the first one not yet
    // signalled ends the scan. Source: vulkan_core.h:4568 PFN_vkGetFenceStatus (VK_SUCCESS signalled, VK_NOT_READY not)
    size_t done = 0;
    while (done < mPendingPresentFences.size())
    {
        const VkResult r = gVk.vkGetFenceStatus(dev, mPendingPresentFences[done]);
        if (r != VK_SUCCESS) { mDevice->lost(r, "vkGetFenceStatus (present)"); break; }
        done++;
    }
    for (size_t i = 0; i < done; ++i)
    {
        gVk.vkResetFences(dev, 1, &mPendingPresentFences[i]);
        mFreeFences.push_back(mPendingPresentFences[i]);
    }
    mPendingPresentFences.erase(mPendingPresentFences.begin(), mPendingPresentFences.begin() + static_cast<std::ptrdiff_t>(done));
    if (!mDevice->presentFences()) return;   // the fallback's retired swapchains go in acquire() (the gate)
    // retired swapchains in order of retirement: one whose last present fence has signalled is finished
    size_t gone = 0;
    while (gone < mRetired.size())
    {
        Retired& r = mRetired[gone];
        if (!r.mPresentFences.empty())
        {
            const VkResult res = gVk.vkGetFenceStatus(dev, r.mPresentFences.back());
            if (res != VK_SUCCESS) { mDevice->lost(res, "vkGetFenceStatus (retired present)"); break; }
        }
        releaseRetired(r);
        gone++;
    }
    mRetired.erase(mRetired.begin(), mRetired.begin() + static_cast<std::ptrdiff_t>(gone));
}

bool WolfVkSwapchain::drainPresents()
{
    if (!mDevice || mDevice->device() == VK_NULL_HANDLE || mDevice->hung()) return false;
    VkDevice dev = mDevice->device();
    if (mDevice->presentFences())
    {
        // bounded, as every CPU wait of the backend; a present that does not finish in that time is the presentation
        // engine holding it (e.g. a hidden window), not unfinished GPU work, so it is not declared a hang: the objects are
        // left as they are and the caller carries on (a later collectRetired or the device's destruction ends them)
        std::vector<VkFence> all;
        for (const Retired& r : mRetired) all.insert(all.end(), r.mPresentFences.begin(), r.mPresentFences.end());
        all.insert(all.end(), mPendingPresentFences.begin(), mPendingPresentFences.end());
        if (!all.empty())
        {
            const VkResult res = gVk.vkWaitForFences(dev, static_cast<uint32_t>(all.size()), all.data(), VK_TRUE, WolfVkDevice::FRAME_WAIT_LIMIT_NS);   // :4569
            if (res != VK_SUCCESS)
            {
                if (!mDevice->lost(res, "vkWaitForFences (present)"))
                    LL_WARNS("Vulkan") << "Presents not finished after 8 s (" << WolfVkLoader::resultString(res) << "): old swapchains kept" << LL_ENDL;
                return false;
            }
        }
    }
    else
    {
        // No present fence exists without swapchain_maintenance1. [V4/W2 2026-11-11] This was a bare vkQueueWaitIdle,
        // which takes no timeout. The specification gives no wait that covers a present here: vkQueueWaitIdle "is
        // equivalent to having submitted a valid fence to every previously executed queue submission command that
        // accepts a fence" (vkQueueWaitIdle refpage), and without the extension vkQueuePresentKHR accepts none. The
        // queue-idle wait is still the strongest wait on offer for the queue the presents went to (and what this code
        // relied on before), so it is kept, after the device's own bounded wait for every tracked submit, but
        // bounded too (queueWaitIdleBounded: a helper thread, 8 s). A timeout marks the device hung, and the caller
        // abandons the swapchains instead of destroying them.
        if (!mDevice->waitIdle()) return false;
        if (!mDevice->queueWaitIdleBounded("vkQueueWaitIdle (presents)")) return false;
    }
    for (Retired& r : mRetired) releaseRetired(r);
    mRetired.clear();
    for (VkFence f : mPendingPresentFences) { gVk.vkResetFences(dev, 1, &f); mFreeFences.push_back(f); }
    mPendingPresentFences.clear();
    return true;
}

bool WolfVkSwapchain::destroy()
{
    // [HANG GUARD 2026-11-08 8.2k] bounded idle (WolfVkDevice::waitIdle); on a hung device the handles are left to it
    // [PRESENT FENCES 2026-11-08] and then every present finished (drainPresents) before the swapchains, their semaphores
    // and the present fences go; if either wait runs out, they are left to the device
    // [V4 2026-11-11] ...and the caller is told (false), because a swapchain left alive pins its surface and native window
    // (VUID-vkDestroySurfaceKHR-surface-01266) and its device (VUID-vkDestroyDevice-device-05137): the owner then abandons
    // those as well (WolfVkSurface::abandon, WolfVkDevice::abandon) instead of destroying them under a live swapchain.
    const bool had_swapchains = mSwapchain != VK_NULL_HANDLE || !mRetired.empty();
    bool all_gone = !had_swapchains;
    if (mDevice && mDevice->device() != VK_NULL_HANDLE && mDevice->waitIdle() && drainPresents())
    {
        destroyViews();
        destroySemaphores(mRenderDone);
        if (mSwapchain != VK_NULL_HANDLE)
        {
            gVk.vkDestroySwapchainKHR(mDevice->device(), mSwapchain, nullptr);
        }
        for (VkFence f : mFreeFences) gVk.vkDestroyFence(mDevice->device(), f, nullptr);   // all signalled-and-reset or never used
        all_gone = true;
    }
    else if (had_swapchains)
    {
        LL_WARNS("Vulkan") << "Swapchain not destroyed (" << (mDevice && mDevice->hung() ? "the device is hung: " + mDevice->hungReason() : std::string("its presents did not finish in time"))
                           << "): it is left to the device with " << mRetired.size() << " retired swapchain(s)" << LL_ENDL;
    }
    mFreeFences.clear();
    mPendingPresentFences.clear();
    mRetired.clear();
    mFirstPresentDone = false;
    mSwapchain = VK_NULL_HANDLE;
    mViews.clear();
    mImages.clear();
    mRenderDone.clear();
    mDevice = nullptr;
    mSurface = VK_NULL_HANDLE;
    mNoSize = false;
    return all_gone;
}

void WolfVkSwapchain::forget()
{
    if (mSwapchain != VK_NULL_HANDLE || !mRetired.empty())
        LL_WARNS("Vulkan") << "Swapchain left to the hung device with " << mRetired.size() << " retired swapchain(s)" << LL_ENDL;
    mFreeFences.clear();
    mPendingPresentFences.clear();
    mRetired.clear();
    mFirstPresentDone = false;
    mSwapchain = VK_NULL_HANDLE;
    mViews.clear();
    mImages.clear();
    mRenderDone.clear();
    mDevice = nullptr;
    mSurface = VK_NULL_HANDLE;
    mNoSize = false;
}

bool WolfVkSwapchain::acquire(VkSemaphore signal, uint32_t& image_index, bool& out_of_date, bool& timed_out, std::string& error)
{
    out_of_date = false;
    timed_out = false;
    // Source: vulkan_core.h:9174 PFN_vkAcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, pImageIndex).
    // [HANG GUARD 2026-11-08 8.2k] Was UINT64_MAX, which VUID-vkAcquireNextImageKHR-surface-07783 forbids whenever forward
    // progress of the surface cannot be guaranteed. A timeout here is NOT a GPU hang (review 2026-11-08): with FIFO
    // (chosen when RenderVSyncEnable is on, build() above) acquire waits for the presentation engine to give an image
    // back, which a compositor may withhold from a hidden or minimised window for as long as it likes; MAILBOX /
    // IMMEDIATE (vsync off) return at once in normal running. So a timeout skips the frame and the next frame tries
    // again; a GPU that has really stopped is caught by beginFrame's bounded fence wait on the frames already submitted.
    // ACQUIRE_WAIT_NS is long against a refresh (6 at 60 Hz, 2.4 at 24 Hz) and short enough that a withheld window
    // keeps the viewer's main loop (network, audio, UI) moving.
    VkResult res = gVk.vkAcquireNextImageKHR(mDevice->device(), mSwapchain, ACQUIRE_WAIT_NS, signal, VK_NULL_HANDLE, &image_index);
    if (res == VK_TIMEOUT || res == VK_NOT_READY)   // vulkan_core.h:143 / :142: no image within the timeout
    {
        timed_out = true;
        error = "vkAcquireNextImageKHR: no image within the timeout";
        return false;
    }
    if (mDevice->lost(res, "vkAcquireNextImageKHR"))
    {
        error = "vkAcquireNextImageKHR: VK_ERROR_DEVICE_LOST";
        return false;
    }
    if (res == VK_ERROR_OUT_OF_DATE_KHR)
    {
        out_of_date = true;
        return false;
    }
    if ((res == VK_SUCCESS || res == VK_SUBOPTIMAL_KHR) && !mDevice->presentFences() && !mRetired.empty())
    {
        // [PRESENT FENCES 2026-11-08] no present fences: the retired swapchains wait for the first image presented on
        // this one to come back (header comment, ANGLE SurfaceVk.cpp:1521-1523); this is it
        size_t gone = 0;
        while (gone < mRetired.size() && mRetired[gone].mGateImage == static_cast<int32_t>(image_index)) { releaseRetired(mRetired[gone]); gone++; }
        mRetired.erase(mRetired.begin(), mRetired.begin() + static_cast<std::ptrdiff_t>(gone));
    }
    if (res == VK_SUBOPTIMAL_KHR)
    {
        // the image was acquired and the semaphore will be signalled; use it, recreate after the frame
        out_of_date = true;
        return true;
    }
    if (res != VK_SUCCESS)
    {
        error = std::string("vkAcquireNextImageKHR: ") + WolfVkLoader::resultString(res);
        return false;
    }
    return true;
}

bool WolfVkSwapchain::present(VkQueue queue, VkSemaphore render_done, uint32_t image_index, bool& out_of_date, std::string& error)
{
    out_of_date = false;
    // Source: vulkan_core.h:9116 VkPresentInfoKHR { sType, pNext, waitSemaphoreCount, pWaitSemaphores, swapchainCount, pSwapchains, pImageIndices, pResults }
    VkPresentInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;   // :474
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &render_done;
    info.swapchainCount = 1;
    info.pSwapchains = &mSwapchain;
    info.pImageIndices = &image_index;
    // [PRESENT FENCES 2026-11-08] Source: vulkan_core.h:13447 VkSwapchainPresentFenceInfoKHR { sType, pNext, swapchainCount,
    // pFences }; :1197 sType; swapchainCount = VkPresentInfoKHR::swapchainCount (VUID-VkSwapchainPresentFenceInfoKHR-
    // swapchainCount-07757); allowed only with the feature enabled (VUID-VkPresentInfoKHR-swapchainMaintenance1-10158)
    VkFence fence = VK_NULL_HANDLE;
    VkSwapchainPresentFenceInfoKHR fence_info{};
    if (mDevice->presentFences())
    {
        collectRetired();
        fence = takePresentFence();
        if (fence == VK_NULL_HANDLE)
        {
            error = "vkCreateFence (present)";
            return false;
        }
        fence_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR;
        fence_info.swapchainCount = 1;
        fence_info.pFences = &fence;
        info.pNext = &fence_info;
    }
    VkResult res = gVk.vkQueuePresentKHR(queue, &info);
    if (fence != VK_NULL_HANDLE)
    {
        if (res == VK_ERROR_DEVICE_LOST)
        {
            // its state is unknown on a lost device: left to it (the result is handled below)
        }
        else
        {
            // kept for every other result, as ANGLE does (SurfaceVk.cpp:2887-2903: the fence goes into the present history
            // before the result is looked at); a fence that never signals only makes a later bounded wait run out
            // [V12 2026-11-11] reviewed: for VK_ERROR_OUT_OF_DATE_KHR / VK_ERROR_SURFACE_LOST_KHR the spec says "the set of
            // queue operations is still considered to be enqueued and thus any semaphore wait operation specified in
            // VkPresentInfoKHR will execute when the corresponding queue operation is complete" (vkQueuePresentKHR), and the
            // fence is signalled "when the relevant operations on the associated swapchain have completed"
            // (VkSwapchainPresentFenceInfoKHR::pFences) - so it is kept, not reused. If a driver never signals it, the
            // worst case is drainPresents running out: the swapchain is then abandoned, not destroyed (V4).
            mPendingPresentFences.push_back(fence);
        }
    }
    if (res == VK_SUCCESS || res == VK_SUBOPTIMAL_KHR || res == VK_ERROR_OUT_OF_DATE_KHR)
    {
        // the gate: only a present that was queued (SUCCESS / SUBOPTIMAL); an out-of-date one is followed by a recreate,
        // which resets the gates anyway. mFirstPresentDone stays conservative: the swapchain is retired, not destroyed.
        if (!mFirstPresentDone && !mDevice->presentFences() && res != VK_ERROR_OUT_OF_DATE_KHR)
        {
            for (Retired& r : mRetired) if (r.mGateImage < 0) r.mGateImage = static_cast<int32_t>(image_index);
        }
        mFirstPresentDone = true;
    }
    if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR)
    {
        out_of_date = true;
        return res == VK_SUBOPTIMAL_KHR;
    }
    if (res != VK_SUCCESS)
    {
        mDevice->lost(res, "vkQueuePresentKHR");   // [DEVICE LOST 8.2k] the caller falls back on hung()
        error = std::string("vkQueuePresentKHR: ") + WolfVkLoader::resultString(res);
        return false;
    }
    return true;
}

const char* WolfVkSwapchain::presentModeString() const
{
    switch (mPresentMode)
    {
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "immediate";
    case VK_PRESENT_MODE_MAILBOX_KHR: return "mailbox";
    case VK_PRESENT_MODE_FIFO_KHR: return "fifo";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "fifo relaxed";
    default: return "other";
    }
}

const char* WolfVkSwapchain::formatString() const
{
    switch (mFormat.format)
    {
    case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case VK_FORMAT_B8G8R8A8_SRGB: return "B8G8R8A8_SRGB";
    case VK_FORMAT_R8G8B8A8_SRGB: return "R8G8B8A8_SRGB";
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "A2B10G10R10_UNORM";
    default: return "other";
    }
}
