/**
 * @file wolfvkloader.cpp
 * @brief Run-time loading of the Vulkan loader library (see wolfvkloader.h).
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
#include "wolfvkloader.h"

#if LL_WINDOWS
// Source: llviewerstats.cpp:815-846 (Linden's Vulkan probe) loads "vulkan-1.dll" with LoadLibraryA and fetches
// vkGetInstanceProcAddr with GetProcAddress; the same two calls here. windows.h stays inside this one file
// (00-Common.cmake adds WIN32_LEAN_AND_MEAN and NOMINMAX for the whole build).
#include <windows.h>
#else
#include <dlfcn.h>
#endif

WolfVkFunctions gVk;

namespace
{
#if LL_WINDOWS
    HMODULE sLibrary = nullptr;
#else
    void* sLibrary = nullptr;
#endif
    std::string sLibraryName;

    // Platform candidates, tried in order.
    //  Linux: the Khronos loader's soname is libvulkan.so.1 (same name Linden's Windows probe counterpart would
    //         use; the viewer's lib/ carries CEF's copy of the same loader, viewer_manifest.py:2231 "libvulkan*",
    //         so LD_LIBRARY_PATH in wrapper.sh:155 finds that first; both are the Khronos loader and read the
    //         system ICD manifests in /usr/share/vulkan/icd.d).
    //  Windows: vulkan-1.dll in System32, installed by every GPU driver with Vulkan (llviewerstats.cpp:826-829).
    //  macOS: libvulkan.1.dylib is the LunarG loader shipped with the Vulkan SDK / bundled by apps; MoltenVK's own
    //         libMoltenVK.dylib exports vkGetInstanceProcAddr directly (MoltenVK README "Using MoltenVK directly").
    //         Both are tried; the bundle must carry one of them beside the viewer (UNTESTED: no Mac here).
    const char* const sCandidates[] =
    {
#if LL_WINDOWS
        "vulkan-1.dll",
#elif LL_DARWIN
        "@executable_path/../Frameworks/libvulkan.1.dylib",
        "@executable_path/../Frameworks/libMoltenVK.dylib",
        "libvulkan.1.dylib",
        "libMoltenVK.dylib",
#else
        "libvulkan.so.1",
        "libvulkan.so",
#endif
    };

    void* symbol(const char* name)
    {
#if LL_WINDOWS
        return sLibrary ? reinterpret_cast<void*>(GetProcAddress(sLibrary, name)) : nullptr;
#else
        return sLibrary ? dlsym(sLibrary, name) : nullptr;
#endif
    }
}

namespace WolfVkLoader
{

bool loadLibrary(std::string& error)
{
    if (sLibrary)
    {
        return true;
    }

    for (const char* candidate : sCandidates)
    {
#if LL_WINDOWS
        sLibrary = LoadLibraryA(candidate);
#else
        // RTLD_LOCAL keeps the loader's symbols out of the global namespace so nothing in the viewer can bind to
        // them by accident; RTLD_NOW so a broken loader fails here rather than in the first call.
        sLibrary = dlopen(candidate, RTLD_NOW | RTLD_LOCAL);
#endif
        if (sLibrary)
        {
            sLibraryName = candidate;
            break;
        }
    }

    if (!sLibrary)
    {
        error = "No Vulkan loader library found (";
        for (size_t i = 0; i < LL_ARRAY_SIZE(sCandidates); ++i)
        {
            if (i) error += ", ";
            error += sCandidates[i];
        }
        error += ")";
#if !LL_WINDOWS
        const char* dl = dlerror();
        if (dl) { error += ": "; error += dl; }
#endif
        return false;
    }

    // Source: vulkan_core.h:4539 PFN_vkGetInstanceProcAddr. The loader exports this one symbol for applications;
    // every other command is reached through it (spec: initialization-functionpointers).
    gVk.vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(symbol("vkGetInstanceProcAddr"));
    if (!gVk.vkGetInstanceProcAddr)
    {
        error = sLibraryName + " has no vkGetInstanceProcAddr";
        unloadLibrary();
        return false;
    }

    // Global commands: vkGetInstanceProcAddr(NULL, name). vkEnumerateInstanceVersion is Vulkan 1.1 and may be
    // null on a 1.0 loader (vulkan_core.h:6169, "Provided by VK_VERSION_1_1").
#define WOLFVK_LOAD_GLOBAL(name, line) \
    gVk.name = reinterpret_cast<PFN_##name>(gVk.vkGetInstanceProcAddr(nullptr, #name));
    WOLFVK_GLOBAL_FUNCS(WOLFVK_LOAD_GLOBAL)
#undef WOLFVK_LOAD_GLOBAL

    if (!gVk.vkCreateInstance || !gVk.vkEnumerateInstanceExtensionProperties || !gVk.vkEnumerateInstanceLayerProperties)
    {
        error = sLibraryName + " did not provide the Vulkan 1.0 global commands";
        unloadLibrary();
        return false;
    }
    return true;
}

bool isLoaded()
{
    return sLibrary != nullptr && gVk.vkGetInstanceProcAddr != nullptr;
}

bool loadInstanceFunctions(VkInstance instance, std::string& error)
{
    if (!isLoaded() || instance == VK_NULL_HANDLE)
    {
        error = "loadInstanceFunctions without a loader or instance";
        return false;
    }
#define WOLFVK_LOAD_INSTANCE(name, line) \
    gVk.name = reinterpret_cast<PFN_##name>(gVk.vkGetInstanceProcAddr(instance, #name));
    WOLFVK_INSTANCE_FUNCS(WOLFVK_LOAD_INSTANCE)
#undef WOLFVK_LOAD_INSTANCE

    // Vulkan 1.1 names first; on a 1.0 instance with VK_KHR_get_physical_device_properties2 the KHR aliases
    // (vulkan_core.h:10356-10357 PFN_vkGetPhysicalDeviceFeatures2KHR / Properties2KHR) have identical signatures.
    if (!gVk.vkGetPhysicalDeviceFeatures2)
    {
        gVk.vkGetPhysicalDeviceFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
            gVk.vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2KHR"));
    }
    if (!gVk.vkGetPhysicalDeviceProperties2)
    {
        gVk.vkGetPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            gVk.vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2KHR"));
    }

    // Core 1.0 + VK_KHR_surface commands must exist; the debug messenger pair is an extension and may be null.
    struct Required { const char* name; void* ptr; };
    const Required required[] =
    {
        { "vkDestroyInstance", reinterpret_cast<void*>(gVk.vkDestroyInstance) },
        { "vkEnumeratePhysicalDevices", reinterpret_cast<void*>(gVk.vkEnumeratePhysicalDevices) },
        { "vkGetPhysicalDeviceProperties", reinterpret_cast<void*>(gVk.vkGetPhysicalDeviceProperties) },
        { "vkGetPhysicalDeviceFeatures", reinterpret_cast<void*>(gVk.vkGetPhysicalDeviceFeatures) },
        { "vkGetPhysicalDeviceFormatProperties", reinterpret_cast<void*>(gVk.vkGetPhysicalDeviceFormatProperties) },
        { "vkGetPhysicalDeviceQueueFamilyProperties", reinterpret_cast<void*>(gVk.vkGetPhysicalDeviceQueueFamilyProperties) },
        { "vkGetPhysicalDeviceMemoryProperties", reinterpret_cast<void*>(gVk.vkGetPhysicalDeviceMemoryProperties) },
        { "vkGetDeviceProcAddr", reinterpret_cast<void*>(gVk.vkGetDeviceProcAddr) },
        { "vkCreateDevice", reinterpret_cast<void*>(gVk.vkCreateDevice) },
        { "vkEnumerateDeviceExtensionProperties", reinterpret_cast<void*>(gVk.vkEnumerateDeviceExtensionProperties) },
        { "vkDestroySurfaceKHR", reinterpret_cast<void*>(gVk.vkDestroySurfaceKHR) },
        { "vkGetPhysicalDeviceSurfaceSupportKHR", reinterpret_cast<void*>(gVk.vkGetPhysicalDeviceSurfaceSupportKHR) },
        { "vkGetPhysicalDeviceSurfaceCapabilitiesKHR", reinterpret_cast<void*>(gVk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR) },
        { "vkGetPhysicalDeviceSurfaceFormatsKHR", reinterpret_cast<void*>(gVk.vkGetPhysicalDeviceSurfaceFormatsKHR) },
        { "vkGetPhysicalDeviceSurfacePresentModesKHR", reinterpret_cast<void*>(gVk.vkGetPhysicalDeviceSurfacePresentModesKHR) },
    };
    for (const Required& r : required)
    {
        if (!r.ptr)
        {
            error = std::string("vkGetInstanceProcAddr returned null for ") + r.name;
            return false;
        }
    }
    return true;
}

bool loadDeviceFunctions(VkDevice device, std::string& error)
{
    if (!gVk.vkGetDeviceProcAddr || device == VK_NULL_HANDLE)
    {
        error = "loadDeviceFunctions without vkGetDeviceProcAddr or a device";
        return false;
    }
#define WOLFVK_LOAD_DEVICE(name, line) \
    gVk.name = reinterpret_cast<PFN_##name>(gVk.vkGetDeviceProcAddr(device, #name)); \
    if (!gVk.name) { error = std::string("vkGetDeviceProcAddr returned null for " #name); return false; }
    WOLFVK_DEVICE_FUNCS(WOLFVK_LOAD_DEVICE)
#undef WOLFVK_LOAD_DEVICE

    // Optional: core names first, then the KHR aliases (vulkan_core.h:11338-11339 and 11675-11677, same
    // signatures). Left null when neither exists.
#define WOLFVK_LOAD_OPTIONAL(name, line) \
    gVk.name = reinterpret_cast<PFN_##name>(gVk.vkGetDeviceProcAddr(device, #name)); \
    if (!gVk.name) { gVk.name = reinterpret_cast<PFN_##name>(gVk.vkGetDeviceProcAddr(device, #name "KHR")); }
    WOLFVK_DEVICE_OPTIONAL_FUNCS(WOLFVK_LOAD_OPTIONAL)
#undef WOLFVK_LOAD_OPTIONAL
    return true;
}

void unloadLibrary()
{
    if (sLibrary)
    {
#if LL_WINDOWS
        FreeLibrary(sLibrary);
#else
        dlclose(sLibrary);
#endif
        sLibrary = nullptr;
    }
    sLibraryName.clear();
    gVk = WolfVkFunctions();
}

const std::string& libraryName()
{
    return sLibraryName;
}

const char* resultString(VkResult result)
{
    // Source: vulkan_core.h:140-190 typedef enum VkResult; the common ones named, the rest numeric.
    switch (result)
    {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_EVENT_SET: return "VK_EVENT_SET";
    case VK_EVENT_RESET: return "VK_EVENT_RESET";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
    case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
    case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
    case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
    case VK_ERROR_FRAGMENTATION: return "VK_ERROR_FRAGMENTATION";
    case VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS: return "VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS";
    case VK_PIPELINE_COMPILE_REQUIRED: return "VK_PIPELINE_COMPILE_REQUIRED";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_INCOMPATIBLE_DISPLAY_KHR: return "VK_ERROR_INCOMPATIBLE_DISPLAY_KHR";
    case VK_ERROR_VALIDATION_FAILED: return "VK_ERROR_VALIDATION_FAILED";
    case VK_ERROR_INVALID_SHADER_NV: return "VK_ERROR_INVALID_SHADER_NV";
    case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT: return "VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT";
    default: break;
    }
    static thread_local char buf[32];
    snprintf(buf, sizeof(buf), "VkResult(%d)", static_cast<int>(result));
    return buf;
}

std::string versionString(uint32_t packed)
{
    // Source: vulkan_core.h:88-90 VK_API_VERSION_MAJOR/MINOR/PATCH (bits 28-22, 21-12, 11-0).
    return llformat("%u.%u.%u", VK_API_VERSION_MAJOR(packed), VK_API_VERSION_MINOR(packed), VK_API_VERSION_PATCH(packed));
}

} // namespace WolfVkLoader
