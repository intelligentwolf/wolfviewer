/**
 * @file wolfvkloader.h
 * @brief Run-time loading of the Vulkan loader library and every entry point the backend uses.
 *
 * [VULKAN BACKEND 2026-11-08] WolfViewer's native Vulkan renderer, stage 4 of ~/wolfviewer-releases/vulkan-plan.md.
 *
 * The viewer never links libvulkan: a PC without a Vulkan driver must still start the OpenGL viewer. The loader
 * library is opened at run time (dlopen "libvulkan.so.1" / LoadLibrary "vulkan-1.dll" / dlopen "libvulkan.1.dylib"
 * or "libMoltenVK.dylib") and every function is fetched through vkGetInstanceProcAddr / vkGetDeviceProcAddr, as
 * the Vulkan specification's "Vulkan Loader Interface" chapter prescribes
 * (https://registry.khronos.org/vulkan/specs/1.3-extensions/html/vkspec.html#initialization-functionpointers).
 *
 * The headers are the vendored Khronos Vulkan-Headers 1.4.363 under third_party/vulkan-headers (Apache-2.0 OR MIT,
 * LICENSE.txt beside them). VK_NO_PROTOTYPES keeps the compiler from seeing link-time prototypes.
 * Every PFN typedef below is cited by its line in that header set: "Source: vulkan_core.h:4530" means
 * third_party/vulkan-headers/include/vulkan/vulkan_core.h line 4530.
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

#ifndef WOLF_VK_LOADER_H
#define WOLF_VK_LOADER_H

#include <string>
#include <cstdint>

// Source: vulkan_core.h:4668 "#ifndef VK_NO_PROTOTYPES" guards every "VKAPI_ATTR VkResult VKAPI_CALL vk*(" prototype;
// with the macro defined only the PFN_ typedefs remain, so nothing here can resolve to a link-time symbol.
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES 1
#endif
#include "third_party/vulkan-headers/include/vulkan/vulkan_core.h"

// Functions fetched with vkGetInstanceProcAddr(NULL, name) before an instance exists.
// Source: vulkan_core.h:4539 PFN_vkGetInstanceProcAddr; spec "Vulkan Loader Interface": the global commands are
// vkEnumerateInstanceVersion, vkEnumerateInstanceExtensionProperties, vkEnumerateInstanceLayerProperties and
// vkCreateInstance.
#define WOLFVK_GLOBAL_FUNCS(X) \
    X(vkEnumerateInstanceVersion, 6169) \
    X(vkEnumerateInstanceExtensionProperties, 4543) \
    X(vkEnumerateInstanceLayerProperties, 4545) \
    X(vkCreateInstance, 4530)

// Functions fetched with vkGetInstanceProcAddr(instance, name).
// Platform surface creators (vkCreateXlibSurfaceKHR, vkCreateWaylandSurfaceKHR, vkCreateWin32SurfaceKHR,
// vkCreateMetalSurfaceEXT) are fetched inside wolfvksurface_*.cpp, which are the only files that include the
// platform headers (vulkan_win32.h pulls in windows.h).
#define WOLFVK_INSTANCE_FUNCS(X) \
    X(vkDestroyInstance, 4531) \
    X(vkEnumeratePhysicalDevices, 4532) \
    X(vkGetPhysicalDeviceFeatures, 4533) \
    X(vkGetPhysicalDeviceFeatures2, 6178) \
    X(vkGetPhysicalDeviceFormatProperties, 4534) \
    X(vkGetPhysicalDeviceProperties, 4536) \
    X(vkGetPhysicalDeviceProperties2, 6179) \
    X(vkGetPhysicalDeviceImageFormatProperties2, 6181) \
    X(vkGetPhysicalDeviceQueueFamilyProperties, 4537) \
    X(vkGetPhysicalDeviceMemoryProperties, 4538) \
    X(vkGetDeviceProcAddr, 4540) \
    X(vkCreateDevice, 4541) \
    X(vkEnumerateDeviceExtensionProperties, 4544) \
    X(vkDestroySurfaceKHR, 9020) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR, 9021) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR, 9022) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR, 9023) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR, 9024) \
    X(vkCreateDebugUtilsMessengerEXT, 16354) \
    X(vkDestroyDebugUtilsMessengerEXT, 16355)

// Functions fetched with vkGetDeviceProcAddr(device, name): the spec allows every device-level command to be
// fetched this way and such pointers skip the loader's dispatch trampoline.
#define WOLFVK_DEVICE_FUNCS(X) \
    X(vkDestroyDevice, 4542) \
    X(vkGetDeviceQueue, 4547) \
    X(vkQueueSubmit, 4548) \
    X(vkQueueWaitIdle, 4549) \
    X(vkDeviceWaitIdle, 4550) \
    X(vkAllocateMemory, 4551) \
    X(vkFreeMemory, 4552) \
    X(vkMapMemory, 4553) \
    X(vkUnmapMemory, 4554) \
    X(vkFlushMappedMemoryRanges, 4555) \
    X(vkInvalidateMappedMemoryRanges, 4556) \
    X(vkBindBufferMemory, 4558) \
    X(vkBindImageMemory, 4559) \
    X(vkGetBufferMemoryRequirements, 4560) \
    X(vkGetImageMemoryRequirements, 4561) \
    X(vkCreateFence, 4565) \
    X(vkDestroyFence, 4566) \
    X(vkResetFences, 4567) \
    X(vkGetFenceStatus, 4568) \
    X(vkWaitForFences, 4569) \
    X(vkCreateSemaphore, 4570) \
    X(vkDestroySemaphore, 4571) \
    X(vkCreateBuffer, 4575) \
    X(vkDestroyBuffer, 4576) \
    X(vkCreateImage, 4577) \
    X(vkDestroyImage, 4578) \
    X(vkCreateImageView, 4580) \
    X(vkDestroyImageView, 4581) \
    X(vkCreateCommandPool, 4582) \
    X(vkDestroyCommandPool, 4583) \
    X(vkResetCommandPool, 4584) \
    X(vkAllocateCommandBuffers, 4585) \
    X(vkFreeCommandBuffers, 4586) \
    X(vkBeginCommandBuffer, 4587) \
    X(vkEndCommandBuffer, 4588) \
    X(vkResetCommandBuffer, 4589) \
    X(vkCmdCopyBuffer, 4590) \
    X(vkCmdCopyBufferToImage, 4592) \
    X(vkCmdCopyImageToBuffer, 4593) \
    X(vkCmdPipelineBarrier, 4596) \
    X(vkCreateQueryPool, 4572) \
    X(vkDestroyQueryPool, 4573) \
    X(vkGetQueryPoolResults, 4574) \
    X(vkCmdResetQueryPool, 4599) \
    X(vkCmdWriteTimestamp, 4600) \
    X(vkCreateShaderModule, 4610) \
    X(vkDestroyShaderModule, 4611) \
    X(vkCreatePipelineCache, 4612) \
    X(vkDestroyPipelineCache, 4613) \
    X(vkGetPipelineCacheData, 4614) \
    X(vkDestroyPipeline, 4617) \
    X(vkCreatePipelineLayout, 4618) \
    X(vkDestroyPipelineLayout, 4619) \
    X(vkCreateSampler, 4620) \
    X(vkDestroySampler, 4621) \
    X(vkCreateDescriptorSetLayout, 4622) \
    X(vkDestroyDescriptorSetLayout, 4623) \
    X(vkCreateDescriptorPool, 4624) \
    X(vkDestroyDescriptorPool, 4625) \
    X(vkResetDescriptorPool, 4626) \
    X(vkAllocateDescriptorSets, 4627) \
    X(vkUpdateDescriptorSets, 4629) \
    X(vkCmdBindPipeline, 4630) \
    X(vkCmdBindDescriptorSets, 4631) \
    X(vkCmdClearColorImage, 4632) \
    X(vkCmdPushConstants, 4638) \
    X(vkCreateGraphicsPipelines, 4639) \
    X(vkCreateFramebuffer, 4640) \
    X(vkDestroyFramebuffer, 4641) \
    X(vkCreateRenderPass, 4642) \
    X(vkDestroyRenderPass, 4643) \
    X(vkCmdSetViewport, 4645) \
    X(vkCmdSetScissor, 4646) \
    X(vkCmdSetDepthBias, 4648) \
    X(vkCmdSetBlendConstants, 4649) \
    X(vkCmdBindIndexBuffer, 4654) \
    X(vkCmdBindVertexBuffers, 4655) \
    X(vkCmdDraw, 4656) \
    X(vkCmdDrawIndexed, 4657) \
    X(vkCmdBlitImage, 4660) \
    X(vkCmdBeginRenderPass, 4664) \
    X(vkCmdEndRenderPass, 4666) \
    X(vkCreateSwapchainKHR, 9171) \
    X(vkDestroySwapchainKHR, 9172) \
    X(vkGetSwapchainImagesKHR, 9173) \
    X(vkAcquireNextImageKHR, 9174) \
    X(vkQueuePresentKHR, 9175)

// Optional device functions, left null when the device lacks them; fetched by their core names first, then the
// KHR aliases (same signatures): vkGetBufferMemoryRequirements2 / vkGetImageMemoryRequirements2 are Vulkan 1.1 or
// VK_KHR_get_memory_requirements2 (vulkan_core.h:11338-11339), the semaphore three are Vulkan 1.2 or
// VK_KHR_timeline_semaphore (vulkan_core.h:11675-11677). vkGetImageSubresourceLayout2 is Vulkan 1.4, its KHR alias
// VK_KHR_maintenance5 (:13146); WolfVkDevice::createDevice also tries the EXT alias of VK_EXT_image_compression_control
// (:19498, vk.xml "vkGetImageSubresourceLayout2EXT" alias="vkGetImageSubresourceLayout2", same signature).
#define WOLFVK_DEVICE_OPTIONAL_FUNCS(X) \
    X(vkGetBufferMemoryRequirements2, 6176) \
    X(vkGetImageMemoryRequirements2, 6175) \
    X(vkGetSemaphoreCounterValue, 7026) \
    X(vkWaitSemaphores, 7027) \
    X(vkSignalSemaphore, 7028) \
    X(vkGetMemoryFdKHR, 10600) \
    X(vkGetSemaphoreFdKHR, 10682) \
    X(vkGetImageSubresourceLayout2, 8824)

// The function table. Members are named exactly as the Vulkan commands so a call reads gVk.vkCreateInstance(...).
struct WolfVkFunctions
{
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;   // Source: vulkan_core.h:4539
#define WOLFVK_DECLARE(name, line) PFN_##name name = nullptr;
    WOLFVK_GLOBAL_FUNCS(WOLFVK_DECLARE)
    WOLFVK_INSTANCE_FUNCS(WOLFVK_DECLARE)
    WOLFVK_DEVICE_FUNCS(WOLFVK_DECLARE)
    WOLFVK_DEVICE_OPTIONAL_FUNCS(WOLFVK_DECLARE)
#undef WOLFVK_DECLARE
};

extern WolfVkFunctions gVk;

namespace WolfVkLoader
{
    // Opens the platform loader library and fetches vkGetInstanceProcAddr and the four global commands.
    // Returns false (with a plain-English reason in error) when no Vulkan loader is installed: the caller then
    // leaves the OpenGL renderer in charge. Safe to call more than once.
    bool loadLibrary(std::string& error);

    // True after loadLibrary succeeded.
    bool isLoaded();

    // Fetches every WOLFVK_INSTANCE_FUNCS pointer from the instance. Returns false and names the first missing
    // function if a core one is absent (extension functions such as vkCreateDebugUtilsMessengerEXT may be null).
    bool loadInstanceFunctions(VkInstance instance, std::string& error);

    // Fetches every WOLFVK_DEVICE_FUNCS pointer from the device; WOLFVK_DEVICE_OPTIONAL_FUNCS are left null when
    // the device lacks them (the device layer then uses fences instead of a timeline semaphore, and the Vulkan 1.0
    // memory requirement queries).
    bool loadDeviceFunctions(VkDevice device, std::string& error);

    // Closes the library and clears the table. Only after the instance has been destroyed.
    void unloadLibrary();

    // The file name that was opened (for the log and About box), empty when not loaded.
    const std::string& libraryName();

    // vkResult as text, e.g. "VK_ERROR_OUT_OF_DATE_KHR"; the enum values are those of vulkan_core.h:140-190.
    const char* resultString(VkResult result);

    // "1.3.280" from a packed VkApiVersion (VK_API_VERSION_MAJOR/MINOR/PATCH, vulkan_core.h:88-90).
    std::string versionString(uint32_t packed);
}

#endif // WOLF_VK_LOADER_H
