/**
 * @file wolfvkdevice.cpp
 * @brief Vulkan instance, device, queues, frames (see wolfvkdevice.h).
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
#include "wolfvkdevice.h"
#include <chrono>   // [AMD PERF] endOneTimeAndWait timing
#include <cstring>
#include <cctype>
#include <algorithm>
#if LL_WINDOWS
// [SHARED IMAGES 2026-11-08] the win32 export entry points (vulkan_win32.h needs windows.h first); fetched by name below
#include "llwin32headers.h"
#include "third_party/vulkan-headers/include/vulkan/vulkan_win32.h"
#else
#include <unistd.h>   // close() of an exported fd that no one imported
#endif

WolfVkDevice gWolfVkDevice;
bool WolfVkDevice::sCompressionQueryWanted = false;

namespace
{
    // Instance extension names for the window surface of each platform. Literal strings here so this file needs no
    // platform header: vulkan_xlib.h:25, vulkan_wayland.h:25, vulkan_win32.h:25, vulkan_metal.h:31.
#if LL_WINDOWS
    const char* const sSurfaceExtensions[] = { "VK_KHR_win32_surface" };
#elif LL_DARWIN
    const char* const sSurfaceExtensions[] = { "VK_EXT_metal_surface" };
#else
    const char* const sSurfaceExtensions[] = { "VK_KHR_xlib_surface", "VK_KHR_wayland_surface" };
#endif

    // Source: https://vulkan.lunarg.com/doc/view/latest/linux/khronos_validation_layer.html - the layer's name.
    const char* const sValidationLayer = "VK_LAYER_KHRONOS_validation";

    // Source: vulkan_beta.h:23 VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME "VK_KHR_portability_subset" (a beta header,
    // behind VK_ENABLE_BETA_EXTENSIONS in vulkan.h, so the literal is repeated here).
    const char* const sPortabilitySubset = "VK_KHR_portability_subset";
    // [SHARED IMAGES 2026-11-08] the platform's export extensions (wolfvkdevice.h mHasExternalSharing)
#if LL_WINDOWS
    const char* const sExternalMemoryExt = "VK_KHR_external_memory_win32";        // vulkan_win32.h:58
    const char* const sExternalSemaphoreExt = "VK_KHR_external_semaphore_win32";  // vulkan_win32.h:130
#else
    const char* const sExternalMemoryExt = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;        // vulkan_core.h:10579
    const char* const sExternalSemaphoreExt = VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME;  // vulkan_core.h:10664
#endif

    bool hasExtension(const std::vector<VkExtensionProperties>& list, const char* name)
    {
        for (const VkExtensionProperties& e : list)
        {
            // Source: vulkan_core.h:3751 VkExtensionProperties { char extensionName[VK_MAX_EXTENSION_NAME_SIZE]; uint32_t specVersion; }
            if (strcmp(e.extensionName, name) == 0) return true;
        }
        return false;
    }

    // Source: vulkan_core.h:16320 PFN_vkDebugUtilsMessengerCallbackEXT(severity, types, pCallbackData, pUserData) -> VkBool32;
    // the spec says the callback must return VK_FALSE (a VK_TRUE return is reserved for layer development).
    VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                 VkDebugUtilsMessageTypeFlagsEXT types,
                                                 const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                 void* user)
    {
        WolfVkDevice* device = static_cast<WolfVkDevice*>(user);
        if (device && data)
        {
            // Source: vulkan_core.h:16305 VkDebugUtilsMessengerCallbackDataEXT { ..., pMessageIdName, messageIdNumber, pMessage, ... }
            device->onValidationMessage(severity, types, data->pMessageIdName ? data->pMessageIdName : "", data->pMessage ? data->pMessage : "");
        }
        return VK_FALSE;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// WolfVkPhysicalDeviceInfo

std::string WolfVkPhysicalDeviceInfo::driverString() const
{
    if (mHasDriverProperties && mDriver.driverName[0])
    {
        // Source: vulkan_core.h:6475 VkPhysicalDeviceDriverProperties { driverID, driverName[256], driverInfo[256], conformanceVersion }
        std::string s = mDriver.driverName;
        if (mDriver.driverInfo[0])
        {
            s += " ";
            s += mDriver.driverInfo;
        }
        return s;
    }
    // Without driver properties only the packed driverVersion (vulkan_core.h:3710) is known; its layout is the
    // vendor's own. NVIDIA packs 10.8.8.6 bits (major.minor.secondary.tertiary), as vulkaninfo decodes it
    // (Vulkan-Tools vulkaninfo.h: "NVIDIA ... (version >> 22) ... (version >> 14) & 0x0ff ... (version >> 6) & 0x0ff ... version & 0x003f").
    if (mProperties.vendorID == WolfVkDevice::VENDOR_NVIDIA)
    {
        const uint32_t v = mProperties.driverVersion;
        return llformat("NVIDIA %u.%u.%u.%u", v >> 22, (v >> 14) & 0x0ffu, (v >> 6) & 0x0ffu, v & 0x003fu);
    }
    // Everyone else: the Vulkan version macros, which is what Mesa and AMD use (vulkaninfo.h default branch).
    return llformat("%u.%u.%u", VK_API_VERSION_MAJOR(mProperties.driverVersion), VK_API_VERSION_MINOR(mProperties.driverVersion),
                    VK_API_VERSION_PATCH(mProperties.driverVersion));
}

std::string WolfVkPhysicalDeviceInfo::apiVersionString() const
{
    return WolfVkLoader::versionString(mProperties.apiVersion);
}

const char* WolfVkPhysicalDeviceInfo::typeString() const
{
    // Source: vulkan_core.h:2233-2237 VkPhysicalDeviceType
    switch (mProperties.deviceType)
    {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return "software";
    case VK_PHYSICAL_DEVICE_TYPE_OTHER:
    default: return "other";
    }
}

// ---------------------------------------------------------------------------------------------------------------
// WolfVkDevice

WolfVkDevice::~WolfVkDevice()
{
    destroy();
}

bool WolfVkDevice::createInstance(const Config& config, std::string& error)
{
    if (mInstance != VK_NULL_HANDLE)
    {
        return true;
    }
    mConfig = config;

    if (!WolfVkLoader::loadLibrary(error))
    {
        return false;
    }

    // Loader / instance version. vkEnumerateInstanceVersion is Vulkan 1.1 (vulkan_core.h:6169); its absence means 1.0.
    mInstanceVersion = VK_API_VERSION_1_0;
    if (gVk.vkEnumerateInstanceVersion)
    {
        uint32_t v = 0;
        if (gVk.vkEnumerateInstanceVersion(&v) == VK_SUCCESS)
        {
            mInstanceVersion = v;
        }
    }

    // Available instance extensions and layers.
    std::vector<VkExtensionProperties> available;
    {
        uint32_t count = 0;
        gVk.vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
        available.resize(count);
        if (count) gVk.vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());
        available.resize(count);
    }
    std::vector<VkLayerProperties> layers;
    {
        uint32_t count = 0;
        gVk.vkEnumerateInstanceLayerProperties(&count, nullptr);
        layers.resize(count);
        if (count) gVk.vkEnumerateInstanceLayerProperties(&count, layers.data());
        layers.resize(count);
    }

    std::vector<const char*> extensions;
    // Source: vulkan_core.h:8941 VK_KHR_SURFACE_EXTENSION_NAME "VK_KHR_surface"
    if (!hasExtension(available, VK_KHR_SURFACE_EXTENSION_NAME))
    {
        error = "The Vulkan loader offers no VK_KHR_surface (no window output possible)";
        return false;
    }
    extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
    bool any_surface = false;
    for (const char* name : sSurfaceExtensions)
    {
        if (hasExtension(available, name))
        {
            extensions.push_back(name);
            any_surface = true;
        }
    }
    if (!any_surface)
    {
        error = std::string("No window surface extension for this platform (") + sSurfaceExtensions[0] + ")";
        return false;
    }
    // [PRESENT FENCES 2026-11-08] surface_maintenance1 (KHR vulkan_core.h:13389, else EXT :19588) when offered, with
    // VK_KHR_get_surface_capabilities2 (:11166) that both depend on (vk.xml: depends="VK_KHR_surface+
    // VK_KHR_get_surface_capabilities2"): the device's swapchain_maintenance1 needs it (run nv-hang2: 20 x
    // VUID-vkDestroySwapchainKHR-swapchain-01282 and 20 x VUID-vkDestroySemaphore-semaphore-05149 at swapchain recreates,
    // because nothing could tell when a present had finished with its swapchain and semaphore)
    mSurfaceMaintenanceKHR = mSurfaceMaintenanceEXT = false;
    if (hasExtension(available, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME))
    {
        mSurfaceMaintenanceKHR = hasExtension(available, VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
        mSurfaceMaintenanceEXT = !mSurfaceMaintenanceKHR && hasExtension(available, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
        if (mSurfaceMaintenanceKHR || mSurfaceMaintenanceEXT)
        {
            extensions.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
            extensions.push_back(mSurfaceMaintenanceKHR ? VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME : VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
        }
    }
    // Source: vulkan_core.h:10341 VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME; needed on a 1.0 instance for
    // vkGetPhysicalDeviceProperties2KHR (device UUID, driver properties). Promoted to core in 1.1.
    if (mInstanceVersion < VK_API_VERSION_1_1 && hasExtension(available, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
    {
        extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    }
    // Source: vulkan_core.h:13051 VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME and :2840
    // VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR: without both, a loader hides MoltenVK (a portability driver).
    mPortabilityEnumeration = hasExtension(available, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    if (mPortabilityEnumeration)
    {
        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    }

    std::vector<const char*> enabled_layers;
    mValidationEnabled = false;
    if (config.mValidation)
    {
        bool have_layer = false;
        for (const VkLayerProperties& l : layers)
        {
            // Source: vulkan_core.h:3756 VkLayerProperties { char layerName[VK_MAX_EXTENSION_NAME_SIZE]; ... }
            if (strcmp(l.layerName, sValidationLayer) == 0) have_layer = true;
        }
        // Source: vulkan_core.h:16269 VK_EXT_DEBUG_UTILS_EXTENSION_NAME "VK_EXT_debug_utils"
        const bool have_debug_utils = hasExtension(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        if (have_layer && have_debug_utils)
        {
            enabled_layers.push_back(sValidationLayer);
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            mValidationEnabled = true;
        }
        else
        {
            LL_WARNS("Vulkan") << "Validation asked for but " << (have_layer ? "VK_EXT_debug_utils" : sValidationLayer)
                               << " is not installed; running without it" << LL_ENDL;
        }
    }

    // Source: vulkan_core.h:3481 VkApplicationInfo { sType, pNext, pApplicationName, applicationVersion, pEngineName, engineVersion, apiVersion }
    // apiVersion: the highest version the application is written for; a 1.0 loader rejects anything above 1.0
    // (VK_ERROR_INCOMPATIBLE_DRIVER), so ask for what the loader reports, capped at 1.3 (vulkan_core.h:7115), the
    // version this backend is written against.
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = config.mApplicationName.c_str();
    app.applicationVersion = config.mApplicationVersion;
    app.pEngineName = "WolfViewer Vulkan backend";
    app.engineVersion = 1;
    app.apiVersion = std::min<uint32_t>(mInstanceVersion, VK_API_VERSION_1_3);

    // Source: vulkan_core.h:3505 VkInstanceCreateInfo { sType, pNext, flags, pApplicationInfo, enabledLayerCount, ppEnabledLayerNames, enabledExtensionCount, ppEnabledExtensionNames }
    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    info.enabledLayerCount = static_cast<uint32_t>(enabled_layers.size());
    info.ppEnabledLayerNames = enabled_layers.empty() ? nullptr : enabled_layers.data();
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    if (mPortabilityEnumeration)
    {
        info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }

    // Source: vulkan_core.h:16326 VkDebugUtilsMessengerCreateInfoEXT { sType, pNext, flags, messageSeverity, messageType, pfnUserCallback, pUserData }
    // Chained into the instance create info as well, so messages during vkCreateInstance itself are caught (spec:
    // "VK_EXT_debug_utils ... can be passed in the pNext chain of VkInstanceCreateInfo").
    VkDebugUtilsMessengerCreateInfoEXT messenger{};
    messenger.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;   // :16275-16276
    messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT       // :16282-16283
                          | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;                                                              // :16281
    messenger.pfnUserCallback = debugCallback;
    messenger.pUserData = this;
    if (mValidationEnabled)
    {
        info.pNext = &messenger;
    }

    VkResult res = gVk.vkCreateInstance(&info, nullptr, &mInstance);
    if (res != VK_SUCCESS)
    {
        error = std::string("vkCreateInstance: ") + WolfVkLoader::resultString(res);
        mInstance = VK_NULL_HANDLE;
        return false;
    }

    if (!WolfVkLoader::loadInstanceFunctions(mInstance, error))
    {
        gVk.vkDestroyInstance(mInstance, nullptr);
        mInstance = VK_NULL_HANDLE;
        return false;
    }

    if (mValidationEnabled && gVk.vkCreateDebugUtilsMessengerEXT)
    {
        res = gVk.vkCreateDebugUtilsMessengerEXT(mInstance, &messenger, nullptr, &mMessenger);
        if (res != VK_SUCCESS)
        {
            LL_WARNS("Vulkan") << "vkCreateDebugUtilsMessengerEXT: " << WolfVkLoader::resultString(res) << LL_ENDL;
            mMessenger = VK_NULL_HANDLE;
        }
    }

    LL_INFOS("Vulkan") << "Instance created: loader " << WolfVkLoader::libraryName() << ", instance version "
                       << WolfVkLoader::versionString(mInstanceVersion) << ", asked for " << WolfVkLoader::versionString(app.apiVersion)
                       << (mValidationEnabled ? ", validation layer on" : "") << LL_ENDL;

    if (!queryPhysicalDevices(error))
    {
        destroy();
        return false;
    }
    return true;
}

bool WolfVkDevice::queryPhysicalDevices(std::string& error)
{
    mPhysicalDevices.clear();
    uint32_t count = 0;
    VkResult res = gVk.vkEnumeratePhysicalDevices(mInstance, &count, nullptr);
    if (res != VK_SUCCESS || count == 0)
    {
        error = count == 0 ? "No Vulkan device found (the graphics driver has no Vulkan part)"
                           : std::string("vkEnumeratePhysicalDevices: ") + WolfVkLoader::resultString(res);
        return false;
    }
    std::vector<VkPhysicalDevice> handles(count);
    gVk.vkEnumeratePhysicalDevices(mInstance, &count, handles.data());
    handles.resize(count);

    for (VkPhysicalDevice handle : handles)
    {
        WolfVkPhysicalDeviceInfo d;
        d.mHandle = handle;
        gVk.vkGetPhysicalDeviceProperties(handle, &d.mProperties);
        gVk.vkGetPhysicalDeviceMemoryProperties(handle, &d.mMemory);
        {
            uint32_t n = 0;
            gVk.vkGetPhysicalDeviceQueueFamilyProperties(handle, &n, nullptr);
            d.mQueueFamilies.resize(n);
            if (n) gVk.vkGetPhysicalDeviceQueueFamilyProperties(handle, &n, d.mQueueFamilies.data());
        }

        std::vector<VkExtensionProperties> exts;
        {
            uint32_t n = 0;
            gVk.vkEnumerateDeviceExtensionProperties(handle, nullptr, &n, nullptr);
            exts.resize(n);
            if (n) gVk.vkEnumerateDeviceExtensionProperties(handle, nullptr, &n, exts.data());
            exts.resize(n);
        }
        d.mHasSwapchain = hasExtension(exts, VK_KHR_SWAPCHAIN_EXTENSION_NAME);                       // vulkan_core.h:9071
        d.mHasPortabilitySubset = hasExtension(exts, sPortabilitySubset);
        const bool api11 = d.mProperties.apiVersion >= VK_API_VERSION_1_1;
        const bool api12 = d.mProperties.apiVersion >= VK_API_VERSION_1_2;
        d.mHasDriverProperties = api12 || hasExtension(exts, VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME); // vulkan_core.h:11612
        d.mHasDedicatedAllocation = api11 || (hasExtension(exts, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME)      // :11291
                                              && hasExtension(exts, VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME)); // :11327
        const bool timeline_ext = api12 || hasExtension(exts, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);        // :11656
        d.mHasExternalSharing = api11 && hasExtension(exts, sExternalMemoryExt) && hasExtension(exts, sExternalSemaphoreExt);

        if (gVk.vkGetPhysicalDeviceProperties2 && (api11 || mInstanceVersion >= VK_API_VERSION_1_1))
        {
            // Source: vulkan_core.h:5809 VkPhysicalDeviceProperties2 { sType, pNext, properties }; :291 ID properties sType; :321 driver properties sType
            d.mIds.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
            d.mDriver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
            d.mIds.pNext = d.mHasDriverProperties ? &d.mDriver : nullptr;
            VkPhysicalDeviceProperties2 props2{};
            props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            props2.pNext = &d.mIds;
            gVk.vkGetPhysicalDeviceProperties2(handle, &props2);
            d.mIds.pNext = nullptr;
        }
        // [PRESENT FENCES 2026-11-08] the swapchain_maintenance1 that matches the instance's surface_maintenance1, if the
        // device has its feature (queried below with the timeline feature, both through vkGetPhysicalDeviceFeatures2)
        const char* maint_ext = nullptr;
        if (mSurfaceMaintenanceKHR && hasExtension(exts, VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME)) maint_ext = VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME;
        else if (mSurfaceMaintenanceEXT && hasExtension(exts, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME)) maint_ext = VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME;
        if (maint_ext && gVk.vkGetPhysicalDeviceFeatures2 && (api11 || mInstanceVersion >= VK_API_VERSION_1_1))
        {
            // Source: vulkan_core.h:13441 VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR { sType, pNext, swapchainMaintenance1 };
            // :1196 sType (the EXT struct is the same type, :19609)
            VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR sm{};
            sm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR;
            VkPhysicalDeviceFeatures2 f2{};
            f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;   // :273
            f2.pNext = &sm;
            gVk.vkGetPhysicalDeviceFeatures2(handle, &f2);
            if (sm.swapchainMaintenance1 == VK_TRUE) d.mSwapchainMaintenance1 = maint_ext;
        }
        // [GL DEPTH 2026-11-08] VK_EXT_depth_clip_control (vk.xml: depends "VK_KHR_get_physical_device_properties2,VK_VERSION_1_1",
        // specialuse "glemulation") and its feature, queried like the others
        if (hasExtension(exts, VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME) && gVk.vkGetPhysicalDeviceFeatures2 && (api11 || mInstanceVersion >= VK_API_VERSION_1_1))
        {
            // Source: vulkan_core.h:21209 VkPhysicalDeviceDepthClipControlFeaturesEXT { sType, pNext, depthClipControl }; :999 sType
            VkPhysicalDeviceDepthClipControlFeaturesEXT dc{};
            dc.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT;
            VkPhysicalDeviceFeatures2 f2{};
            f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;   // :273
            f2.pNext = &dc;
            gVk.vkGetPhysicalDeviceFeatures2(handle, &f2);
            d.mHasDepthClipControl = dc.depthClipControl == VK_TRUE;
        }
        // [AMD PERF 2026-11-09] VK_EXT_image_compression_control, same dependencies (vk.xml); its feature read alike
        if (hasExtension(exts, VK_EXT_IMAGE_COMPRESSION_CONTROL_EXTENSION_NAME) && gVk.vkGetPhysicalDeviceFeatures2 && (api11 || mInstanceVersion >= VK_API_VERSION_1_1))
        {
            d.mHasImageCompressionControl = true;
            // Source: vulkan_core.h:20957 VkPhysicalDeviceImageCompressionControlFeaturesEXT { sType, pNext, imageCompressionControl }; :983 sType
            VkPhysicalDeviceImageCompressionControlFeaturesEXT cc{};
            cc.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_COMPRESSION_CONTROL_FEATURES_EXT;
            VkPhysicalDeviceFeatures2 f2{};
            f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;   // :273
            f2.pNext = &cc;
            gVk.vkGetPhysicalDeviceFeatures2(handle, &f2);
            d.mImageCompressionControlFeature = cc.imageCompressionControl == VK_TRUE;
        }
        if (timeline_ext && gVk.vkGetPhysicalDeviceFeatures2 && (api11 || mInstanceVersion >= VK_API_VERSION_1_1))
        {
            // Source: vulkan_core.h:6651 VkPhysicalDeviceTimelineSemaphoreFeatures { sType, pNext, timelineSemaphore }; sType :329
            VkPhysicalDeviceTimelineSemaphoreFeatures tl{};
            tl.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
            VkPhysicalDeviceFeatures2 f2{};
            f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;   // :273
            f2.pNext = &tl;
            gVk.vkGetPhysicalDeviceFeatures2(handle, &f2);
            d.mHasTimelineSemaphore = tl.timelineSemaphore == VK_TRUE;
        }

        LL_INFOS("Vulkan") << "Device " << mPhysicalDevices.size() << ": " << d.name() << " (" << d.typeString()
                           << ", vendor 0x" << std::hex << d.mProperties.vendorID << std::dec << "), driver " << d.driverString()
                           << ", Vulkan " << d.apiVersionString()
                           << (d.mHasSwapchain ? "" : ", NO swapchain") << (d.mHasTimelineSemaphore ? ", timeline semaphores" : "")
                           << (d.mHasPortabilitySubset ? ", portability subset" : "")
                           << (d.mSwapchainMaintenance1 ? std::string(", ") + d.mSwapchainMaintenance1 : std::string())
                           << (d.mHasDepthClipControl ? ", VK_EXT_depth_clip_control" : "")
                           << (d.mHasImageCompressionControl ? ", VK_EXT_image_compression_control" : "") << LL_ENDL;
        mPhysicalDevices.push_back(d);
    }
    return true;
}

bool WolfVkDevice::choosePhysicalDevice(VkSurfaceKHR surface, std::string& error)
{
    // Candidates: a graphics queue family that can present to the surface, and VK_KHR_swapchain.
    std::vector<WolfVkPhysicalDeviceInfo> candidates;
    for (WolfVkPhysicalDeviceInfo d : mPhysicalDevices)
    {
        if (!d.mHasSwapchain)
        {
            continue;
        }
        // [V7 2026-11-11] never a CPU implementation (llvmpipe/lavapipe, SwiftShader): VK_PHYSICAL_DEVICE_TYPE_CPU
        // (vulkan_core.h:2237), "the device is typically running on the same processors as the host" (VkPhysicalDeviceType)
        if (d.mProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)
        {
            LL_INFOS("Vulkan") << "Device skipped, it runs on the processor (VK_PHYSICAL_DEVICE_TYPE_CPU): " << d.name() << LL_ENDL;
            continue;
        }
        for (uint32_t family = 0; family < d.mQueueFamilies.size(); ++family)
        {
            // Source: vulkan_core.h:3720 VkQueueFamilyProperties { queueFlags, queueCount, ... }; :2869 VK_QUEUE_GRAPHICS_BIT
            if (!(d.mQueueFamilies[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) || d.mQueueFamilies[family].queueCount == 0)
            {
                continue;
            }
            VkBool32 present = VK_TRUE;
            if (surface != VK_NULL_HANDLE)
            {
                // Source: vulkan_core.h:9021 PFN_vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, queueFamilyIndex, surface, pSupported)
                present = VK_FALSE;
                gVk.vkGetPhysicalDeviceSurfaceSupportKHR(d.mHandle, family, surface, &present);
            }
            if (present == VK_TRUE)
            {
                d.mGraphicsPresentFamily = family;
                break;
            }
        }
        if (d.mGraphicsPresentFamily != UINT32_MAX)
        {
            candidates.push_back(d);
        }
    }
    if (candidates.empty())
    {
        error = "No Vulkan graphics card can draw to this window (no graphics queue with presentation, no VK_KHR_swapchain, or only a processor-based device)";
        return false;
    }

    auto lower = [](std::string s) { std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); }); return s; };

    // a. the setting
    if (!mConfig.mDeviceOverride.empty())
    {
        const std::string want = lower(mConfig.mDeviceOverride);
        for (const WolfVkPhysicalDeviceInfo& d : candidates)
        {
            if (lower(d.name()).find(want) != std::string::npos)
            {
                mChosen = d;
                LL_INFOS("Vulkan") << "Device chosen by the WolfVulkanDevice setting \"" << mConfig.mDeviceOverride << "\": " << d.name() << LL_ENDL;
                return true;
            }
        }
        LL_WARNS("Vulkan") << "WolfVulkanDevice \"" << mConfig.mDeviceOverride << "\" matches no Vulkan device; choosing automatically" << LL_ENDL;
    }
    // b. the OpenGL context's device UUID
    if (mConfig.mHaveGLDeviceUUID)
    {
        for (const WolfVkPhysicalDeviceInfo& d : candidates)
        {
            if (memcmp(d.mIds.deviceUUID, mConfig.mGLDeviceUUID, VK_UUID_SIZE) == 0)
            {
                mChosen = d;
                LL_INFOS("Vulkan") << "Device chosen by the OpenGL device UUID: " << d.name() << LL_ENDL;
                return true;
            }
        }
        // [V7 2026-11-11] OpenGL named its card and no Vulkan graphics card is that one (OpenGL on a software renderer, or
        // on a card this Vulkan loader does not list): Vulkan on another card could share no image with OpenGL, so it is
        // not started at all (the caller falls back to OpenGL with this reason). Was: c.-e. picked any other device.
        error = "no Vulkan graphics card is the one OpenGL draws with (" + std::to_string(candidates.size()) + " other Vulkan device(s) offered)";
        LL_WARNS("Vulkan") << error << LL_ENDL;
        return false;
    }
    // c. the OpenGL vendor
    if (mConfig.mGLVendorID)
    {
        for (const WolfVkPhysicalDeviceInfo& d : candidates)
        {
            if (d.mProperties.vendorID == mConfig.mGLVendorID)
            {
                mChosen = d;
                LL_INFOS("Vulkan") << "Device chosen by the OpenGL vendor: " << d.name() << LL_ENDL;
                return true;
            }
        }
        // [V7 2026-11-11] as for b.: OpenGL's maker is known and none of the Vulkan graphics cards is by that maker
        error = llformat("no Vulkan graphics card is by the maker of the one OpenGL draws with (vendor 0x%04x)", mConfig.mGLVendorID);
        LL_WARNS("Vulkan") << error << LL_ENDL;
        return false;
    }
    // d. first discrete
    for (const WolfVkPhysicalDeviceInfo& d : candidates)
    {
        if (d.mProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
        {
            mChosen = d;
            LL_INFOS("Vulkan") << "Device chosen as the first discrete card: " << d.name() << LL_ENDL;
            return true;
        }
    }
    // e. first
    mChosen = candidates.front();
    LL_INFOS("Vulkan") << "Device chosen as the first available: " << mChosen.name() << LL_ENDL;
    return true;
}

bool WolfVkDevice::createDevice(VkSurfaceKHR surface, std::string& error)
{
    if (mDevice != VK_NULL_HANDLE)
    {
        return true;
    }
    if (mInstance == VK_NULL_HANDLE)
    {
        error = "createDevice before createInstance";
        return false;
    }
    if (!choosePhysicalDevice(surface, error))
    {
        return false;
    }

    std::vector<const char*> extensions;
    extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    if (mChosen.mHasPortabilitySubset)
    {
        // Spec (VK_KHR_portability_subset): "If the VK_KHR_portability_subset extension is included in
        // pProperties of vkEnumerateDeviceExtensionProperties, ppEnabledExtensionNames must include it".
        extensions.push_back(sPortabilitySubset);
    }
    const bool api11 = mChosen.mProperties.apiVersion >= VK_API_VERSION_1_1;
    const bool api12 = mChosen.mProperties.apiVersion >= VK_API_VERSION_1_2;
    if (!api12 && mChosen.mHasTimelineSemaphore)
    {
        extensions.push_back(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
    }
    if (!api11 && mChosen.mHasDedicatedAllocation)
    {
        extensions.push_back(VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME);
        extensions.push_back(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME);
    }
    if (!api12 && mChosen.mHasDriverProperties)
    {
        extensions.push_back(VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME);
    }
    if (mChosen.mHasExternalSharing)
    {
        extensions.push_back(sExternalMemoryExt);
        extensions.push_back(sExternalSemaphoreExt);
    }
    // [PRESENT FENCES 2026-11-08] its dependency VK_KHR_get_physical_device_properties2 is core from 1.1 (vk.xml depends
    // "(VK_KHR_get_physical_device_properties2,VK_VERSION_1_1)"); the feature query above already required 1.1
    if (mChosen.mSwapchainMaintenance1) extensions.push_back(mChosen.mSwapchainMaintenance1);
    if (mChosen.mHasDepthClipControl) extensions.push_back(VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME);   // vulkan_core.h:21208
    // [AMD PERF 2026-11-09] debug only: VkImageCompressionPropertiesEXT may only be chained when its extension is enabled
    const bool compression_query = sCompressionQueryWanted && mChosen.mHasImageCompressionControl;
    if (compression_query) extensions.push_back(VK_EXT_IMAGE_COMPRESSION_CONTROL_EXTENSION_NAME);   // vulkan_core.h:20917

    // Features: only what the backend uses. samplerAnisotropy for the viewer's TFO_ANISOTROPIC filter
    // (llrender.cpp:546 glTexParameterf GL_TEXTURE_MAX_ANISOTROPY), fillModeNonSolid for the viewer's wireframe
    // (LLPipeline::sRenderWireframe), textureCompressionBC for the DXT textures (llimagegl.cpp:318-323).
    VkPhysicalDeviceFeatures supported{};
    gVk.vkGetPhysicalDeviceFeatures(mChosen.mHandle, &supported);
    VkPhysicalDeviceFeatures features{};
    features.samplerAnisotropy = supported.samplerAnisotropy;
    features.fillModeNonSolid = supported.fillModeNonSolid;
    features.textureCompressionBC = supported.textureCompressionBC;
    features.independentBlend = supported.independentBlend;
    features.depthClamp = supported.depthClamp;

    // Source: vulkan_core.h:6651 VkPhysicalDeviceTimelineSemaphoreFeatures; enabling the feature on the device
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline{};
    timeline.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    timeline.timelineSemaphore = VK_TRUE;
    // [PRESENT FENCES 2026-11-08] the feature must be enabled for a present to carry a fence
    // (VUID-VkPresentInfoKHR-swapchainMaintenance1-10158). Source: vulkan_core.h:13441, :1196
    VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR maintenance{};
    maintenance.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR;
    maintenance.swapchainMaintenance1 = VK_TRUE;
    // [GL DEPTH 2026-11-08] depthClipControl enabled, or negativeOneToOne may not be used
    // (VUID-VkPipelineViewportDepthClipControlCreateInfoEXT-negativeOneToOne-06470). Source: vulkan_core.h:21209, :999
    VkPhysicalDeviceDepthClipControlFeaturesEXT depth_clip{};
    depth_clip.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT;
    depth_clip.depthClipControl = VK_TRUE;
    // [AMD PERF 2026-11-09] the extension's feature with it when offered (it covers compression controls at image
    // creation, which nothing here chains, so enabling it changes no image). Source: vulkan_core.h:20957, :983
    VkPhysicalDeviceImageCompressionControlFeaturesEXT compression{};
    compression.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_COMPRESSION_CONTROL_FEATURES_EXT;
    compression.imageCompressionControl = VK_TRUE;
    void* feature_chain = nullptr;
    if (compression_query && mChosen.mImageCompressionControlFeature) { compression.pNext = feature_chain; feature_chain = &compression; }
    if (mChosen.mHasTimelineSemaphore) { timeline.pNext = feature_chain; feature_chain = &timeline; }
    if (mChosen.mSwapchainMaintenance1) { maintenance.pNext = feature_chain; feature_chain = &maintenance; }
    if (mChosen.mHasDepthClipControl) { depth_clip.pNext = feature_chain; feature_chain = &depth_clip; }

    // Source: vulkan_core.h:3727 VkDeviceQueueCreateInfo { sType, pNext, flags, queueFamilyIndex, queueCount, pQueuePriorities }
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = mChosen.mGraphicsPresentFamily;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;

    // Source: vulkan_core.h:3736 VkDeviceCreateInfo { sType, pNext, flags, queueCreateInfoCount, pQueueCreateInfos, enabledLayerCount,
    // ppEnabledLayerNames, enabledExtensionCount, ppEnabledExtensionNames, pEnabledFeatures }
    VkDeviceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.pNext = feature_chain;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue_info;
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    info.pEnabledFeatures = &features;

    VkResult res = gVk.vkCreateDevice(mChosen.mHandle, &info, nullptr, &mDevice);
    if (res != VK_SUCCESS)
    {
        error = std::string("vkCreateDevice: ") + WolfVkLoader::resultString(res);
        mDevice = VK_NULL_HANDLE;
        return false;
    }
    mPresentFences = mChosen.mSwapchainMaintenance1 != nullptr;
    mGlDepthRange = mChosen.mHasDepthClipControl;
    mCompressionQuery = compression_query;
    if (!WolfVkLoader::loadDeviceFunctions(mDevice, error))
    {
        gVk.vkDestroyDevice(mDevice, nullptr);
        mDevice = VK_NULL_HANDLE;
        return false;
    }
    if (compression_query)
    {
        // the extension's own name for the query (vulkan_core.h:19498, vk.xml alias of vkGetImageSubresourceLayout2):
        // the core name is only there when the effective API version is 1.4, the KHR one with VK_KHR_maintenance5
        if (PFN_vkGetImageSubresourceLayout2 ext = reinterpret_cast<PFN_vkGetImageSubresourceLayout2>(gVk.vkGetDeviceProcAddr(mDevice, "vkGetImageSubresourceLayout2EXT")))
        {
            gVk.vkGetImageSubresourceLayout2 = ext;
        }
        LL_INFOS("Vulkan") << "Image compression query (WolfVulkanDebugCompressionQuery): VK_EXT_image_compression_control enabled"
                           << (mChosen.mImageCompressionControlFeature ? " with its feature" : " (feature not offered)")
                           << (gVk.vkGetImageSubresourceLayout2 ? "" : ", but vkGetImageSubresourceLayout2 not found: nothing will be logged") << LL_ENDL;
    }
    // Source: vulkan_core.h:4547 PFN_vkGetDeviceQueue(device, queueFamilyIndex, queueIndex, pQueue)
    gVk.vkGetDeviceQueue(mDevice, mChosen.mGraphicsPresentFamily, 0, &mQueue);

    mAllocator.init(mChosen.mHandle, mDevice, mChosen.mProperties, mChosen.mMemory);

    if (!createFrames(error))
    {
        // [V11 2026-11-11] the device only: destroy() would also end the instance, under the window surface the caller
        // still holds (VUID-vkDestroyInstance-instance-00629) - the caller destroys the surface, then the instance
        destroyDeviceObjects();
        return false;
    }

    LL_INFOS("Vulkan") << "Device created: " << aboutLine() << ", queue family " << mChosen.mGraphicsPresentFamily
                       << ", " << mFrames.size() << " frames in flight" << (mTimeline != VK_NULL_HANDLE ? ", timeline semaphore" : ", fences only") << LL_ENDL;
    return true;
}

bool WolfVkDevice::createFrames(std::string& error)
{
    const uint32_t n = std::max<uint32_t>(1, std::min<uint32_t>(mConfig.mFramesInFlight, 4));
    mFrames.resize(n);
    for (uint32_t i = 0; i < n; ++i)
    {
        Frame& f = mFrames[i];
        // Source: vulkan_core.h:3950 VkCommandPoolCreateInfo { sType, pNext, flags, queueFamilyIndex }; :3157 TRANSIENT (short-lived buffers)
        VkCommandPoolCreateInfo pool{};
        pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pool.queueFamilyIndex = mChosen.mGraphicsPresentFamily;
        VkResult res = gVk.vkCreateCommandPool(mDevice, &pool, nullptr, &f.mPool);
        if (res != VK_SUCCESS) { error = std::string("vkCreateCommandPool: ") + WolfVkLoader::resultString(res); return false; }

        // Source: vulkan_core.h:3957 VkCommandBufferAllocateInfo { sType, pNext, commandPool, level, commandBufferCount }; :2338 PRIMARY
        VkCommandBufferAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc.commandPool = f.mPool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        res = gVk.vkAllocateCommandBuffers(mDevice, &alloc, &f.mCmd);
        if (res != VK_SUCCESS) { error = std::string("vkAllocateCommandBuffers: ") + WolfVkLoader::resultString(res); return false; }

        // Source: vulkan_core.h:3866 VkFenceCreateInfo { sType, pNext, flags }; :3002 SIGNALED so the first beginFrame does not wait
        VkFenceCreateInfo fence{};
        fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        res = gVk.vkCreateFence(mDevice, &fence, nullptr, &f.mFence);
        if (res != VK_SUCCESS) { error = std::string("vkCreateFence: ") + WolfVkLoader::resultString(res); return false; }

        // Source: vulkan_core.h:3872 VkSemaphoreCreateInfo { sType, pNext, flags } (binary)
        VkSemaphoreCreateInfo sem{};
        sem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        res = gVk.vkCreateSemaphore(mDevice, &sem, nullptr, &f.mAcquire);
        if (res != VK_SUCCESS) { error = std::string("vkCreateSemaphore: ") + WolfVkLoader::resultString(res); return false; }
    }

    // One-time command pool (uploads, read-backs), reset per buffer.
    VkCommandPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;   // :3157-3158
    pool.queueFamilyIndex = mChosen.mGraphicsPresentFamily;
    VkResult res = gVk.vkCreateCommandPool(mDevice, &pool, nullptr, &mOneTimePool);
    if (res != VK_SUCCESS) { error = std::string("vkCreateCommandPool (one-time): ") + WolfVkLoader::resultString(res); return false; }

    // Timeline semaphore when the device has the feature and the function pointers.
    if (mChosen.mHasTimelineSemaphore && gVk.vkWaitSemaphores && gVk.vkGetSemaphoreCounterValue)
    {
        // Source: vulkan_core.h:6663 VkSemaphoreTypeCreateInfo { sType, pNext, semaphoreType, initialValue }; :331 sType; :6413 VK_SEMAPHORE_TYPE_TIMELINE
        VkSemaphoreTypeCreateInfo type{};
        type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        type.initialValue = 0;
        VkSemaphoreCreateInfo sem{};
        sem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        sem.pNext = &type;
        res = gVk.vkCreateSemaphore(mDevice, &sem, nullptr, &mTimeline);
        if (res != VK_SUCCESS)
        {
            LL_WARNS("Vulkan") << "Timeline semaphore: " << WolfVkLoader::resultString(res) << "; using fences" << LL_ENDL;
            mTimeline = VK_NULL_HANDLE;
        }
        mTimelineNext = 0;
    }
    mFrameIndex = 0;
    mFrameCounter = 0;
    return true;
}

void WolfVkDevice::destroyFrames()
{
    for (Frame& f : mFrames)
    {
        if (f.mPool) gVk.vkResetCommandPool(mDevice, f.mPool, 0);
    }
    if (mOneTimePool) gVk.vkResetCommandPool(mDevice, mOneTimePool, 0);
    for (Frame& f : mFrames)
    {
        runDeferred(f);
        if (f.mAcquire) gVk.vkDestroySemaphore(mDevice, f.mAcquire, nullptr);
        if (f.mFence) gVk.vkDestroyFence(mDevice, f.mFence, nullptr);
        for (VkFence mf : f.mMidFences) if (mf) gVk.vkDestroyFence(mDevice, mf, nullptr);
        if (f.mPool) gVk.vkDestroyCommandPool(mDevice, f.mPool, nullptr);   // frees its command buffers
        f = Frame();
    }
    mFrames.clear();
    if (mTimeline)
    {
        gVk.vkDestroySemaphore(mDevice, mTimeline, nullptr);
        mTimeline = VK_NULL_HANDLE;
    }
    if (mOneTimePool)
    {
        gVk.vkDestroyCommandPool(mDevice, mOneTimePool, nullptr);
        mOneTimePool = VK_NULL_HANDLE;
    }
}

void WolfVkDevice::runDeferred(Frame& frame)
{
    std::vector<std::function<void()>> work;
    {
        std::lock_guard<std::mutex> lock(mDeferredMutex);
        work.swap(frame.mDeferred);
    }
    for (auto& fn : work)
    {
        fn();
    }
}

WolfVkDevice::Frame& WolfVkDevice::beginFrame()
{
    Frame& f = mFrames[mFrameIndex];
    // [HANG GUARD 2026-11-08 8.2k] once hung, no wait may be started again (it would cost another 8 s and then fail the
    // same way); the caller checks hung() after this call and falls back
    if (mHung) return f;
    if (f.mSubmitted && mTimeline != VK_NULL_HANDLE)
    {
        // Source: vulkan_core.h:6679 VkSemaphoreWaitInfo { sType, pNext, flags, semaphoreCount, pSemaphores, pValues }; :333 sType;
        // :7027 PFN_vkWaitSemaphores(device, pWaitInfo, timeout)
        VkSemaphoreWaitInfo wait{};
        wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wait.semaphoreCount = 1;
        wait.pSemaphores = &mTimeline;
        wait.pValues = &f.mTimelineValue;
        VkResult res = gVk.vkWaitSemaphores(mDevice, &wait, FRAME_WAIT_LIMIT_NS);
        if (res == VK_TIMEOUT)   // vulkan_core.h:143
        {
            declareHung(llformat("frame %llu (timeline value %llu) has not finished after %llu s", static_cast<unsigned long long>(mFrameCounter),
                                 static_cast<unsigned long long>(f.mTimelineValue), static_cast<unsigned long long>(FRAME_WAIT_LIMIT_NS / 1000000000ull)));
            return f;
        }
        if (lost(res, "beginFrame vkWaitSemaphores")) return f;
        if (res != VK_SUCCESS)
        {
            LL_WARNS_ONCE("Vulkan") << "vkWaitSemaphores: " << WolfVkLoader::resultString(res) << LL_ENDL;
        }
    }
    // The fences are waited too: the slot's command buffers must be finished before its pool is reset, and on a device
    // without timeline semaphores the fences are the only way to know. Only fences that were actually submitted: the
    // frame fence is reset below and handed to the queue by endFrame alone (mFenceSubmitted), the mid-frame fences by
    // submitMidFrame on a device without a timeline.
    std::vector<VkFence> fences;
    if (f.mFenceSubmitted) fences.push_back(f.mFence);
    for (uint32_t i = 0; i < f.mMidFencesUsed; ++i) fences.push_back(f.mMidFences[i]);
    if (!fences.empty())
    {
        // Source: vulkan_core.h:4569 PFN_vkWaitForFences(device, fenceCount, pFences, waitAll, timeout)
        VkResult res = gVk.vkWaitForFences(mDevice, static_cast<uint32_t>(fences.size()), fences.data(), VK_TRUE, FRAME_WAIT_LIMIT_NS);
        if (res == VK_TIMEOUT)
        {
            declareHung(llformat("frame %llu's fence has not signalled after %llu s", static_cast<unsigned long long>(mFrameCounter),
                                 static_cast<unsigned long long>(FRAME_WAIT_LIMIT_NS / 1000000000ull)));
            return f;
        }
        if (lost(res, "beginFrame vkWaitForFences")) return f;
        if (res != VK_SUCCESS)
        {
            LL_WARNS_ONCE("Vulkan") << "vkWaitForFences: " << WolfVkLoader::resultString(res) << LL_ENDL;
        }
    }
    f.mSubmitted = false;
    f.mFenceSubmitted = false;
    // VUID-vkQueueSubmit-fence-00063: a fence handed to vkQueueSubmit must be unsignaled - all of them are reset here;
    // VUID-vkResetFences-pFences-01123 only asks that none is tied to an unfinished submit, which the waits above ensure
    // (the frame fence that was not submitted is tied to nothing)
    gVk.vkResetFences(mDevice, 1, &f.mFence);
    if (f.mMidFencesUsed > 0) gVk.vkResetFences(mDevice, f.mMidFencesUsed, f.mMidFences.data());
    f.mMidFencesUsed = 0;
    // the pool first: a recorded command buffer keeps every object it names "in use" until it is reset
    gVk.vkResetCommandPool(mDevice, f.mPool, 0);
    f.mMidUsed = 0;

    // Source: vulkan_core.h:3976 VkCommandBufferBeginInfo { sType, pNext, flags, pInheritanceInfo }; :3177 ONE_TIME_SUBMIT
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    // [V10 2026-11-11] vkBeginCommandBuffer can fail (VK_ERROR_OUT_OF_HOST_MEMORY / _DEVICE_MEMORY, vkBeginCommandBuffer
    // refpage); recording into or ending a buffer that did not begin is invalid, so the device is marked unusable the way
    // a hang marks it (every caller of beginFrame checks hung() and falls back to OpenGL)
    const VkResult begun = gVk.vkBeginCommandBuffer(f.mCmd, &begin);
    if (begun != VK_SUCCESS)
    {
        declareHung(std::string("vkBeginCommandBuffer (frame): ") + WolfVkLoader::resultString(begun));
        return f;
    }
    mRecording = true;
    return f;
}

void WolfVkDevice::declareHung(const std::string& reason)
{
    if (mHung) return;
    mHung = true;
    mHungReason = reason;
    LL_WARNS("Vulkan") << "The graphics card did not finish a Vulkan frame: " << mHungReason << LL_ENDL;
}

bool WolfVkDevice::lost(VkResult res, const char* what)
{
    // [DEVICE LOST 2026-11-08 nv-hang1] VK_ERROR_DEVICE_LOST (vulkan_core.h:150) from any call: the device is as unusable as a
    // hung one. Run nv-hang1 (19:09:10Z) got it from mirrorTexture's one-time fence wait; nothing marked the device, the
    // next texture's vkQueueSubmit went to the lost device and the validation layer crashed (SIGSEGV) inside it, with no
    // fall-back. From here on the hung flag stops every submit and wait, and the owner falls back to OpenGL.
    if (res != VK_ERROR_DEVICE_LOST) return false;
    declareHung(std::string(what) + ": VK_ERROR_DEVICE_LOST");
    return true;
}

bool WolfVkDevice::endFrame(bool wait_acquire, VkSemaphore render_done, std::string& error)
{
    return endFrame(wait_acquire, render_done, ExtraSync(), error);
}

bool WolfVkDevice::endFrame(bool wait_acquire, VkSemaphore render_done, const ExtraSync& extra, std::string& error)
{
    Frame& f = mFrames[mFrameIndex];
    if (mHung) { error = "the device is hung or lost (" + mHungReason + ")"; return false; }   // [DEVICE LOST 8.2k] nothing submits again
    VkResult res = gVk.vkEndCommandBuffer(f.mCmd);
    if (res != VK_SUCCESS)
    {
        lost(res, "vkEndCommandBuffer");
        error = std::string("vkEndCommandBuffer: ") + WolfVkLoader::resultString(res);
        return false;
    }

    // Waits: the acquire semaphore gates the first write to the swapchain image (colour attachment output,
    // vulkan_core.h:2932), then the caller's extra waits at their stages.
    std::vector<VkSemaphore> waits;
    std::vector<VkPipelineStageFlags> wait_stages;
    if (wait_acquire)
    {
        waits.push_back(f.mAcquire);
        wait_stages.push_back(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    }
    for (size_t i = 0; i < extra.mWaits.size(); ++i)
    {
        waits.push_back(extra.mWaits[i]);
        wait_stages.push_back(i < extra.mWaitStages.size() ? extra.mWaitStages[i] : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
    }
    // Signals: the swapchain image's render-done, the caller's extras, and the timeline LAST (its value sits at the
    // same index in the timeline info; values at binary semaphores' indices are ignored, spec 7.4.2).
    std::vector<VkSemaphore> signals;
    std::vector<uint64_t> signal_values;
    if (render_done != VK_NULL_HANDLE) { signals.push_back(render_done); signal_values.push_back(0); }
    for (VkSemaphore s : extra.mSignals) { signals.push_back(s); signal_values.push_back(0); }
    // Source: vulkan_core.h:6670 VkTimelineSemaphoreSubmitInfo { sType, pNext, waitSemaphoreValueCount, pWaitSemaphoreValues,
    // signalSemaphoreValueCount, pSignalSemaphoreValues }; :332 sType.
    VkTimelineSemaphoreSubmitInfo timeline_info{};
    timeline_info.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    // [HANG GUARD 2026-11-08 8.2k] the value is taken only once the submit succeeded: waitIdle waits for mTimelineNext,
    // and a value no submit will ever signal would read as a hang
    const uint64_t timeline_value = mTimelineNext + 1;
    if (mTimeline != VK_NULL_HANDLE)
    {
        signals.push_back(mTimeline);
        signal_values.push_back(timeline_value);
        timeline_info.signalSemaphoreValueCount = static_cast<uint32_t>(signal_values.size());
        timeline_info.pSignalSemaphoreValues = signal_values.data();
    }

    // Source: vulkan_core.h:3763 VkSubmitInfo { sType, pNext, waitSemaphoreCount, pWaitSemaphores, pWaitDstStageMask, commandBufferCount,
    // pCommandBuffers, signalSemaphoreCount, pSignalSemaphores }
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.pNext = (mTimeline != VK_NULL_HANDLE) ? &timeline_info : nullptr;
    submit.waitSemaphoreCount = static_cast<uint32_t>(waits.size());
    submit.pWaitSemaphores = waits.empty() ? nullptr : waits.data();
    submit.pWaitDstStageMask = wait_stages.empty() ? nullptr : wait_stages.data();
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &f.mCmd;
    submit.signalSemaphoreCount = static_cast<uint32_t>(signals.size());
    submit.pSignalSemaphores = signals.empty() ? nullptr : signals.data();

    // Source: vulkan_core.h:4548 PFN_vkQueueSubmit(queue, submitCount, pSubmits, fence)
    res = gVk.vkQueueSubmit(mQueue, 1, &submit, f.mFence);
    if (res != VK_SUCCESS)
    {
        lost(res, "vkQueueSubmit (frame)");
        error = std::string("vkQueueSubmit: ") + WolfVkLoader::resultString(res);
        return false;
    }
    if (mTimeline != VK_NULL_HANDLE)
    {
        mTimelineNext = timeline_value;
        f.mTimelineValue = timeline_value;
    }
    f.mSubmitted = true;
    f.mFenceSubmitted = true;
    mFrameCounter++;
    mRecording = false;
    mFrameIndex = (mFrameIndex + 1) % static_cast<uint32_t>(mFrames.size());
    return true;
}

VkCommandBuffer WolfVkDevice::beginMidFrame()
{
    if (!mRecording || mFrames.empty() || mHung) return VK_NULL_HANDLE;
    Frame& f = mFrames[mFrameIndex];
    if (f.mMidUsed >= f.mMidCmds.size())
    {
        // Source: vulkan_core.h:3957 VkCommandBufferAllocateInfo { sType, pNext, commandPool, level, commandBufferCount }; :4585 PFN_vkAllocateCommandBuffers
        VkCommandBufferAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc.commandPool = f.mPool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkResult res = gVk.vkAllocateCommandBuffers(mDevice, &alloc, &cmd);
        if (res != VK_SUCCESS)
        {
            LL_WARNS("Vulkan") << "vkAllocateCommandBuffers (mid-frame): " << WolfVkLoader::resultString(res) << LL_ENDL;
            return VK_NULL_HANDLE;
        }
        f.mMidCmds.push_back(cmd);
    }
    VkCommandBuffer cmd = f.mMidCmds[f.mMidUsed++];
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    // [V10 2026-11-11] a buffer that did not begin is not handed out (the caller then lets OpenGL draw the pass); it stays
    // in the slot's pool in the initial state, which beginFrame's pool reset allows
    const VkResult begun = gVk.vkBeginCommandBuffer(cmd, &begin);
    if (begun != VK_SUCCESS)
    {
        LL_WARNS("Vulkan") << "vkBeginCommandBuffer (mid-frame): " << WolfVkLoader::resultString(begun) << LL_ENDL;
        return VK_NULL_HANDLE;
    }
    return cmd;
}

bool WolfVkDevice::submitMidFrame(VkCommandBuffer cmd, const ExtraSync& extra, std::string& error)
{
    if (cmd == VK_NULL_HANDLE || mFrames.empty()) { error = "no mid-frame command buffer"; return false; }
    if (mHung) { error = "the device is hung or lost (" + mHungReason + ")"; return false; }   // [DEVICE LOST 8.2k]
    VkResult res = gVk.vkEndCommandBuffer(cmd);
    if (res != VK_SUCCESS) { lost(res, "vkEndCommandBuffer (mid-frame)"); error = std::string("vkEndCommandBuffer (mid-frame): ") + WolfVkLoader::resultString(res); return false; }
    Frame& f = mFrames[mFrameIndex];
    std::vector<VkPipelineStageFlags> stages = extra.mWaitStages;
    while (stages.size() < extra.mWaits.size()) stages.push_back(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
    std::vector<VkSemaphore> signals = extra.mSignals;
    std::vector<uint64_t> values(signals.size(), 0);
    VkTimelineSemaphoreSubmitInfo timeline_info{};
    timeline_info.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    const uint64_t timeline_value = mTimelineNext + 1;   // taken on success only (endFrame)
    VkFence fence = VK_NULL_HANDLE;
    if (mTimeline != VK_NULL_HANDLE)
    {
        // the slot's last value: beginFrame waits for it, so this submit is covered too
        signals.push_back(mTimeline);
        values.push_back(timeline_value);
        timeline_info.signalSemaphoreValueCount = static_cast<uint32_t>(values.size());
        timeline_info.pSignalSemaphoreValues = values.data();
    }
    else
    {
        // [HANG GUARD 2026-11-08 8.2k] no timeline: this submit gets a fence of its own, so beginFrame / waitIdle can wait
        // for it with a timeout (the frame fence covers only the frame's own batch)
        fence = midFrameFence(f);
        if (fence == VK_NULL_HANDLE) { error = "no fence for the mid-frame submit"; return false; }
    }
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.pNext = (mTimeline != VK_NULL_HANDLE) ? &timeline_info : nullptr;
    submit.waitSemaphoreCount = static_cast<uint32_t>(extra.mWaits.size());
    submit.pWaitSemaphores = extra.mWaits.empty() ? nullptr : extra.mWaits.data();
    submit.pWaitDstStageMask = extra.mWaits.empty() ? nullptr : stages.data();
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = static_cast<uint32_t>(signals.size());
    submit.pSignalSemaphores = signals.empty() ? nullptr : signals.data();
    res = gVk.vkQueueSubmit(mQueue, 1, &submit, fence);
    if (res != VK_SUCCESS) { lost(res, "vkQueueSubmit (mid-frame)"); error = std::string("vkQueueSubmit (mid-frame): ") + WolfVkLoader::resultString(res); return false; }
    if (mTimeline != VK_NULL_HANDLE)
    {
        mTimelineNext = timeline_value;
        f.mTimelineValue = timeline_value;
    }
    else
    {
        f.mMidFencesUsed++;   // midFrameFence handed out mMidFences[mMidFencesUsed]
    }
    f.mSubmitted = true;   // something of this slot is on the queue: the next reuse waits the timeline (or its fences)
    return true;
}

VkFence WolfVkDevice::midFrameFence(Frame& frame)
{
    if (frame.mMidFencesUsed < frame.mMidFences.size()) return frame.mMidFences[frame.mMidFencesUsed];   // reset by beginFrame
    // Source: vulkan_core.h:3866 VkFenceCreateInfo { sType, pNext, flags }; flags 0 = unsignalled (VUID-vkQueueSubmit-fence-00063)
    VkFenceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    if (gVk.vkCreateFence(mDevice, &info, nullptr, &fence) != VK_SUCCESS) return VK_NULL_HANDLE;
    frame.mMidFences.push_back(fence);
    return fence;
}

void WolfVkDevice::discardMidFrame(VkCommandBuffer cmd)
{
    // [HAND-OVER 2026-11-08 8.2k] ended, never submitted: an executable command buffer may stay in its pool until the pool
    // is reset (VUID-vkResetCommandPool-commandPool-00040 forbids only pending ones), which beginFrame does
    if (cmd != VK_NULL_HANDLE) gVk.vkEndCommandBuffer(cmd);
}

// [SHARED IMAGES 2026-11-08] -------------------------------------------------------------------------------------

VkExternalMemoryHandleTypeFlags WolfVkDevice::externalHandleType() const
{
    if (!externalSharing()) return 0;
#if LL_WINDOWS
    return VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;   // vulkan_core.h:5580
#else
    return VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;      // vulkan_core.h:5579
#endif
}

bool WolfVkDevice::exportMemory(VkDeviceMemory memory, ExternalHandle& out, std::string& error)
{
    out = ExternalHandle();
    if (!externalSharing())
    {
        error = "the device has no external memory extension";
        return false;
    }
#if LL_WINDOWS
    // Source: vulkan_win32.h:81 VkMemoryGetWin32HandleInfoKHR { sType, pNext, memory, handleType }; vulkan_core.h:575 sType;
    // vulkan_win32.h:88 PFN_vkGetMemoryWin32HandleKHR(device, pGetWin32HandleInfo, pHandle)
    auto get = reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(gVk.vkGetDeviceProcAddr(mDevice, "vkGetMemoryWin32HandleKHR"));
    if (!get) { error = "vkGetMemoryWin32HandleKHR missing"; return false; }
    VkMemoryGetWin32HandleInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR;
    info.memory = memory;
    info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    HANDLE h = nullptr;
    VkResult res = get(mDevice, &info, &h);
    if (res != VK_SUCCESS) { error = std::string("vkGetMemoryWin32HandleKHR: ") + WolfVkLoader::resultString(res); return false; }
    out.mWin32 = h;
#else
    // Source: vulkan_core.h:10593 VkMemoryGetFdInfoKHR { sType, pNext, memory, handleType }; :578 sType; :10600 PFN_vkGetMemoryFdKHR(device, pGetFdInfo, pFd).
    // "Each call to vkGetMemoryFdKHR must create a new file descriptor holding a reference to the memory object's payload and
    // transfer ownership of the file descriptor to the application" (spec); GL's import takes it over.
    if (!gVk.vkGetMemoryFdKHR) { error = "vkGetMemoryFdKHR missing"; return false; }
    VkMemoryGetFdInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    info.memory = memory;
    info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    int fd = -1;
    VkResult res = gVk.vkGetMemoryFdKHR(mDevice, &info, &fd);
    if (res != VK_SUCCESS) { error = std::string("vkGetMemoryFdKHR: ") + WolfVkLoader::resultString(res); return false; }
    out.mFd = fd;
#endif
    return true;
}

bool WolfVkDevice::createExportedSemaphore(VkSemaphore& out, ExternalHandle& handle, std::string& error)
{
    out = VK_NULL_HANDLE;
    handle = ExternalHandle();
    if (!externalSharing())
    {
        error = "the device has no external semaphore extension";
        return false;
    }
    // Source: vulkan_core.h:5989 VkExportSemaphoreCreateInfo { sType, pNext, handleTypes }; :298 sType; :3872 VkSemaphoreCreateInfo;
    // :4570 PFN_vkCreateSemaphore. OPAQUE_FD / OPAQUE_WIN32 have reference transference: the handle and the VkSemaphore share one payload.
    VkExportSemaphoreCreateInfo exp{};
    exp.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
#if LL_WINDOWS
    exp.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;   // :5657
#else
    exp.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;      // :5656
#endif
    VkSemaphoreCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    info.pNext = &exp;
    VkResult res = gVk.vkCreateSemaphore(mDevice, &info, nullptr, &out);
    if (res != VK_SUCCESS)
    {
        error = std::string("vkCreateSemaphore (exported): ") + WolfVkLoader::resultString(res);
        out = VK_NULL_HANDLE;
        return false;
    }
#if LL_WINDOWS
    // Source: vulkan_win32.h:158 VkSemaphoreGetWin32HandleInfoKHR { sType, pNext, semaphore, handleType }; vulkan_core.h:583 sType; vulkan_win32.h:166 PFN
    auto get = reinterpret_cast<PFN_vkGetSemaphoreWin32HandleKHR>(gVk.vkGetDeviceProcAddr(mDevice, "vkGetSemaphoreWin32HandleKHR"));
    if (!get) { error = "vkGetSemaphoreWin32HandleKHR missing"; gVk.vkDestroySemaphore(mDevice, out, nullptr); out = VK_NULL_HANDLE; return false; }
    VkSemaphoreGetWin32HandleInfoKHR gi{};
    gi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR;
    gi.semaphore = out;
    gi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    HANDLE h = nullptr;
    res = get(mDevice, &gi, &h);
    if (res != VK_SUCCESS) { error = std::string("vkGetSemaphoreWin32HandleKHR: ") + WolfVkLoader::resultString(res); gVk.vkDestroySemaphore(mDevice, out, nullptr); out = VK_NULL_HANDLE; return false; }
    handle.mWin32 = h;
#else
    // Source: vulkan_core.h:10674 VkSemaphoreGetFdInfoKHR { sType, pNext, semaphore, handleType }; :585 sType; :10682 PFN_vkGetSemaphoreFdKHR
    if (!gVk.vkGetSemaphoreFdKHR) { error = "vkGetSemaphoreFdKHR missing"; gVk.vkDestroySemaphore(mDevice, out, nullptr); out = VK_NULL_HANDLE; return false; }
    VkSemaphoreGetFdInfoKHR gi{};
    gi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
    gi.semaphore = out;
    gi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    int fd = -1;
    res = gVk.vkGetSemaphoreFdKHR(mDevice, &gi, &fd);
    if (res != VK_SUCCESS) { error = std::string("vkGetSemaphoreFdKHR: ") + WolfVkLoader::resultString(res); gVk.vkDestroySemaphore(mDevice, out, nullptr); out = VK_NULL_HANDLE; return false; }
    handle.mFd = fd;
#endif
    return true;
}

void WolfVkDevice::closeExternalHandle(ExternalHandle& handle)
{
#if LL_WINDOWS
    if (handle.mWin32) CloseHandle(static_cast<HANDLE>(handle.mWin32));
#else
    if (handle.mFd >= 0) close(handle.mFd);
#endif
    handle = ExternalHandle();
}

bool WolfVkDevice::submitSync(VkCommandBuffer cmd, const ExtraSync& extra, bool wait, std::string& error)
{
    if (mDevice == VK_NULL_HANDLE) { error = "no device"; return false; }
    if (cmd != VK_NULL_HANDLE && mOneTimeOpen > 0) mOneTimeOpen--;
    // [DEVICE LOST 8.2k] nothing submits to a hung or lost device; the unsubmitted buffer stays with it
    if (mHung) { error = "the device is hung or lost (" + mHungReason + ")"; return false; }
    if (cmd != VK_NULL_HANDLE)
    {
        VkResult r = gVk.vkEndCommandBuffer(cmd);
        if (r != VK_SUCCESS)
        {
            error = std::string("vkEndCommandBuffer: ") + WolfVkLoader::resultString(r);
            gVk.vkFreeCommandBuffers(mDevice, mOneTimePool, 1, &cmd);
            return false;
        }
    }
    std::vector<VkPipelineStageFlags> stages = extra.mWaitStages;
    while (stages.size() < extra.mWaits.size()) stages.push_back(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
    // [HANG GUARD 2026-11-08 8.2k] tracked like every other submit: the timeline's next value when there is one (taken on
    // success only), so waitIdle covers this batch with its timeout
    std::vector<VkSemaphore> signals = extra.mSignals;
    std::vector<uint64_t> values(signals.size(), 0);
    const uint64_t timeline_value = mTimelineNext + 1;
    VkTimelineSemaphoreSubmitInfo timeline_info{};   // Source: vulkan_core.h:6670; :332 sType
    timeline_info.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    if (mTimeline != VK_NULL_HANDLE)
    {
        signals.push_back(mTimeline);
        values.push_back(timeline_value);
        timeline_info.signalSemaphoreValueCount = static_cast<uint32_t>(values.size());
        timeline_info.pSignalSemaphoreValues = values.data();
    }
    // Source: vulkan_core.h:3763 VkSubmitInfo; a submit with no command buffers is allowed (commandBufferCount may be 0)
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.pNext = (mTimeline != VK_NULL_HANDLE) ? &timeline_info : nullptr;
    submit.waitSemaphoreCount = static_cast<uint32_t>(extra.mWaits.size());
    submit.pWaitSemaphores = extra.mWaits.empty() ? nullptr : extra.mWaits.data();
    submit.pWaitDstStageMask = extra.mWaits.empty() ? nullptr : stages.data();
    submit.commandBufferCount = cmd != VK_NULL_HANDLE ? 1u : 0u;
    submit.pCommandBuffers = cmd != VK_NULL_HANDLE ? &cmd : nullptr;
    submit.signalSemaphoreCount = static_cast<uint32_t>(signals.size());
    submit.pSignalSemaphores = signals.empty() ? nullptr : signals.data();
    // the one-time command buffer is freed here, so its execution must be over: a fence is waited whenever there is one
    // or the caller asks. A pure semaphore submit on a device with a timeline is tracked by its value; without a timeline
    // nothing else could track it, so it is waited here too (bounded below)
    const bool must_wait = wait || cmd != VK_NULL_HANDLE || mTimeline == VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    if (must_wait)
    {
        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        // [V10 2026-11-11] checked: without the fence the submit could not be waited for (nor the buffer freed after it)
        const VkResult made = gVk.vkCreateFence(mDevice, &fi, nullptr, &fence);
        if (made != VK_SUCCESS)
        {
            error = std::string("vkCreateFence (sync): ") + WolfVkLoader::resultString(made);
            if (cmd != VK_NULL_HANDLE) gVk.vkFreeCommandBuffers(mDevice, mOneTimePool, 1, &cmd);
            return false;
        }
    }
    VkResult res = gVk.vkQueueSubmit(mQueue, 1, &submit, fence);
    bool ok = res == VK_SUCCESS;
    if (!ok) { lost(res, "vkQueueSubmit (sync)"); error = std::string("vkQueueSubmit (sync): ") + WolfVkLoader::resultString(res); }
    if (ok && mTimeline != VK_NULL_HANDLE) mTimelineNext = timeline_value;
    if (ok && must_wait)
    {
        // [HANG GUARD 2026-11-08 8.2k] was UINT64_MAX: a submit that waits on a GL semaphore GL never signalled would have
        // frozen the viewer here. vulkan_core.h:4569 PFN_vkWaitForFences takes the timeout; :143 VK_TIMEOUT
        res = gVk.vkWaitForFences(mDevice, 1, &fence, VK_TRUE, FRAME_WAIT_LIMIT_NS);
        if (res == VK_TIMEOUT)
        {
            declareHung("a hand-over submit has not finished after 8 s");
            error = "vkWaitForFences (sync): timed out";
            // still pending: VUID-vkDestroyFence-fence-01120 / VUID-vkFreeCommandBuffers-pCommandBuffers-00047 forbid
            // destroying the fence or freeing the buffer, so both are left to the abandoned device
            return false;
        }
        if (lost(res, "vkWaitForFences (sync)"))
        {
            error = "vkWaitForFences (sync): VK_ERROR_DEVICE_LOST";
            return false;   // the fence's state is unknown: left to the lost device, as on a timeout
        }
        if (res != VK_SUCCESS) { ok = false; error = std::string("vkWaitForFences (sync): ") + WolfVkLoader::resultString(res); }
    }
    if (fence) gVk.vkDestroyFence(mDevice, fence, nullptr);
    if (cmd != VK_NULL_HANDLE) gVk.vkFreeCommandBuffers(mDevice, mOneTimePool, 1, &cmd);
    return ok;
}

void WolfVkDevice::flushDeferred()
{
    if (!mFrames.empty())
    {
        runDeferred(mFrames[mFrameIndex]);
    }
}

void WolfVkDevice::deferDestroy(std::function<void()> destroy)
{
    if (mFrames.empty())
    {
        destroy();
        return;
    }
    // The work runs when the slot of the LAST SUBMITTED frame comes round again, i.e. after beginFrame has waited for
    // that frame's fence: that submit is the last one that can reference the object. While recording, that is the
    // current slot (its own fence). Between endFrame and the next beginFrame - where every destroy raised by the GL
    // side of the frame lands - mFrameIndex already names the NEXT slot, whose fence belongs to the frame before
    // last; filing there destroyed objects the just-submitted frame still read (validation 2026-11-08:
    // VUID-vkDestroyBuffer-buffer-00922 / VUID-vkDestroyImageView-imageView-01026, 20 + 18 a session).
    std::lock_guard<std::mutex> lock(mDeferredMutex);
    const uint32_t n = static_cast<uint32_t>(mFrames.size());
    const uint32_t slot = mRecording ? mFrameIndex : (mFrameIndex + n - 1) % n;
    mFrames[slot].mDeferred.push_back(std::move(destroy));
}

VkCommandBuffer WolfVkDevice::beginOneTime()
{
    if (mHung || mDevice == VK_NULL_HANDLE) return VK_NULL_HANDLE;   // [HANG GUARD 8.2k] its submit could only time out again
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = mOneTimePool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkResult res = gVk.vkAllocateCommandBuffers(mDevice, &alloc, &cmd);
    if (res != VK_SUCCESS)
    {
        LL_WARNS("Vulkan") << "vkAllocateCommandBuffers (one-time): " << WolfVkLoader::resultString(res) << LL_ENDL;
        return VK_NULL_HANDLE;
    }
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    const VkResult begun = gVk.vkBeginCommandBuffer(cmd, &begin);   // [V10 2026-11-11] checked
    if (begun != VK_SUCCESS)
    {
        LL_WARNS("Vulkan") << "vkBeginCommandBuffer (one-time): " << WolfVkLoader::resultString(begun) << LL_ENDL;
        gVk.vkFreeCommandBuffers(mDevice, mOneTimePool, 1, &cmd);
        return VK_NULL_HANDLE;
    }
    mOneTimeOpen++;
    return cmd;
}

bool WolfVkDevice::endOneTimeAndWait(VkCommandBuffer cmd, std::string& error)
{
    // [AMD PERF 2026-11-08] CPU time of the whole call (submit and fence wait), counted for takeOneTimeStats
    struct Timed
    {
        WolfVkDevice& d; std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        ~Timed() { d.mOneTimeCount++; d.mOneTimeMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); }
    } timed{ *this };
    if (cmd != VK_NULL_HANDLE && mOneTimeOpen > 0) mOneTimeOpen--;
    if (cmd == VK_NULL_HANDLE)
    {
        error = "no command buffer";
        return false;
    }
    if (mHung) { error = "the device is hung or lost (" + mHungReason + ")"; return false; }   // [DEVICE LOST 8.2k]
    VkResult res = gVk.vkEndCommandBuffer(cmd);
    if (res != VK_SUCCESS)
    {
        error = std::string("vkEndCommandBuffer: ") + WolfVkLoader::resultString(res);
        gVk.vkFreeCommandBuffers(mDevice, mOneTimePool, 1, &cmd);
        return false;
    }
    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    // [V10 2026-11-11] checked: a null fence would make the wait below wait on nothing and free a buffer still executing
    res = gVk.vkCreateFence(mDevice, &fence_info, nullptr, &fence);
    if (res != VK_SUCCESS)
    {
        error = std::string("vkCreateFence (one-time): ") + WolfVkLoader::resultString(res);
        gVk.vkFreeCommandBuffers(mDevice, mOneTimePool, 1, &cmd);
        return false;
    }

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    res = gVk.vkQueueSubmit(mQueue, 1, &submit, fence);
    bool ok = res == VK_SUCCESS;
    if (!ok)
    {
        lost(res, "vkQueueSubmit (one-time)");
        error = std::string("vkQueueSubmit (one-time): ") + WolfVkLoader::resultString(res);
    }
    else
    {
        // [HANG GUARD 2026-11-08 8.2k] was UINT64_MAX (texture mirror uploads, G-buffer image set-up, read-backs)
        res = gVk.vkWaitForFences(mDevice, 1, &fence, VK_TRUE, FRAME_WAIT_LIMIT_NS);
        if (res == VK_TIMEOUT)
        {
            declareHung("a one-time upload has not finished after 8 s");
            error = "vkWaitForFences (one-time): timed out";
            return false;   // fence and buffer still pending: left to the abandoned device (as in submitSync)
        }
        if (lost(res, "vkWaitForFences (one-time)"))   // the nv-hang1 case (19:09:10Z)
        {
            error = "vkWaitForFences (one-time): VK_ERROR_DEVICE_LOST";
            return false;
        }
        ok = res == VK_SUCCESS;
        if (!ok) error = std::string("vkWaitForFences (one-time): ") + WolfVkLoader::resultString(res);
    }
    if (fence) gVk.vkDestroyFence(mDevice, fence, nullptr);
    gVk.vkFreeCommandBuffers(mDevice, mOneTimePool, 1, &cmd);
    return ok;
}

bool WolfVkDevice::waitAllSubmitted(const char* what)
{
    if (mDevice == VK_NULL_HANDLE) return true;
    if (mHung) return false;
    // Every batch this class submits is tracked: endFrame (frame fence + timeline), submitMidFrame (timeline, or a fence
    // of its own without one), submitSync (timeline, or waited inline without one), the one-time submits (waited inline).
    if (mTimeline != VK_NULL_HANDLE && mTimelineNext > 0)
    {
        // Source: vulkan_core.h:6679 VkSemaphoreWaitInfo; :7027 PFN_vkWaitSemaphores(device, pWaitInfo, timeout)
        VkSemaphoreWaitInfo wait{};
        wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wait.semaphoreCount = 1;
        wait.pSemaphores = &mTimeline;
        wait.pValues = &mTimelineNext;
        const VkResult res = gVk.vkWaitSemaphores(mDevice, &wait, FRAME_WAIT_LIMIT_NS);
        if (res == VK_TIMEOUT)
        {
            declareHung(llformat("%s: timeline value %llu not reached after %llu s", what, static_cast<unsigned long long>(mTimelineNext),
                                 static_cast<unsigned long long>(FRAME_WAIT_LIMIT_NS / 1000000000ull)));
            return false;
        }
        if (lost(res, what)) return false;
        if (res != VK_SUCCESS) LL_WARNS_ONCE("Vulkan") << what << ": vkWaitSemaphores: " << WolfVkLoader::resultString(res) << LL_ENDL;
    }
    std::vector<VkFence> fences;
    for (const Frame& f : mFrames)
    {
        if (f.mFenceSubmitted) fences.push_back(f.mFence);
        for (uint32_t i = 0; i < f.mMidFencesUsed; ++i) fences.push_back(f.mMidFences[i]);
    }
    if (!fences.empty())
    {
        const VkResult res = gVk.vkWaitForFences(mDevice, static_cast<uint32_t>(fences.size()), fences.data(), VK_TRUE, FRAME_WAIT_LIMIT_NS);   // :4569
        if (res == VK_TIMEOUT)
        {
            declareHung(llformat("%s: a frame fence has not signalled after %llu s", what, static_cast<unsigned long long>(FRAME_WAIT_LIMIT_NS / 1000000000ull)));
            return false;
        }
        if (lost(res, what)) return false;
        if (res != VK_SUCCESS) LL_WARNS_ONCE("Vulkan") << what << ": vkWaitForFences: " << WolfVkLoader::resultString(res) << LL_ENDL;
    }
    return true;
}

bool WolfVkDevice::waitIdle()
{
    if (mDevice == VK_NULL_HANDLE) return true;
    // [HANG GUARD 2026-11-08 8.2k] This replaces vkDeviceWaitIdle (vulkan_core.h:4550), which takes no timeout. A call
    // after the bounded wait was kept at first; it was dropped (review, 2026-11-08) because what it would still wait for
    // is queue presentation, which a compositor may hold back indefinitely for a hidden window. The destroys that follow
    // need only the batches: VUID-vkDestroySwapchainKHR-swapchain-01282 "All uses of presentable images acquired from
    // swapchain must have completed execution", VUID-vkDestroyImage-image-01000 / VUID-vkFreeMemory-memory-00677 /
    // VUID-vkDestroySemaphore-semaphore-05149 "submitted commands / batches" - all of them tracked and waited here.
    return waitAllSubmitted("waitIdle");
}

void WolfVkDevice::waitIdleAndFlushDeferred()
{
    if (mDevice == VK_NULL_HANDLE || mHung)   // a hung device never goes idle; its objects are left to the driver
    {
        return;
    }
    if (!waitIdle()) return;   // [HANG GUARD 8.2k] timed out: now hung, nothing is destroyed
    // every submitted command buffer is finished: reset the pools so nothing they named counts as in use, then destroy.
    // The slot being RECORDED keeps its pool and its deferred list: resetting the pool would put its open command buffer
    // back to the initial state (the frame's later commands then land on an unbegun buffer - "vkCmdPipelineBarrier was
    // called before vkBeginCommandBuffer", the crash of 15:37Z on a window resize mid-frame), and its deferred objects
    // may be named by commands already recorded in it. Likewise the one-time pool while a one-time buffer is open.
    for (size_t i = 0; i < mFrames.size(); ++i)
    {
        Frame& f = mFrames[i];
        const bool recording_now = mRecording && i == mFrameIndex;
        f.mSubmitted = false;
        f.mFenceSubmitted = false;   // signalled now; beginFrame resets it without waiting (the mid fences it resets too)
        if (!recording_now) gVk.vkResetCommandPool(mDevice, f.mPool, 0);
    }
    if (mOneTimePool != VK_NULL_HANDLE && mOneTimeOpen == 0)
    {
        gVk.vkResetCommandPool(mDevice, mOneTimePool, 0);
    }
    for (size_t i = 0; i < mFrames.size(); ++i)
    {
        if (mRecording && i == mFrameIndex) continue;
        runDeferred(mFrames[i]);
    }
}

void WolfVkDevice::destroyDeviceObjects()
{
    // [V11 2026-11-11] createDevice's failure path: the frames made so far (destroyFrames skips the null handles of a
    // half-made slot), the allocator and the device; nothing was submitted, so nothing is waited for. The instance stays.
    if (mDevice != VK_NULL_HANDLE)
    {
        destroyFrames();
        mAllocator.destroy();
        gVk.vkDestroyDevice(mDevice, nullptr);
    }
    mDevice = VK_NULL_HANDLE;
    mQueue = VK_NULL_HANDLE;
    mPresentFences = false;
    mGlDepthRange = false;
    mCompressionQuery = false;
}

void WolfVkDevice::abandon(const std::string& why)
{
    // [V4 2026-11-11] see the header: a swapchain the owner could not destroy pins the device and the instance
    LL_WARNS("Vulkan") << "Vulkan device and instance abandoned without destruction: " << why << LL_ENDL;
    mAbandoned = true;
    mAllocator.abandon();
    mDevice = VK_NULL_HANDLE;
    mQueue = VK_NULL_HANDLE;
    mFrames.clear();
}

void WolfVkDevice::destroy()
{
    mPresentFences = false;
    mGlDepthRange = false;
    mCompressionQuery = false;
    if (mDevice != VK_NULL_HANDLE && mHung)
    {
        // nothing can be waited for or safely destroyed: the handles are abandoned to the driver (process exit cleans up)
        LL_WARNS("Vulkan") << "Vulkan device abandoned without destruction (hung): " << mHungReason << LL_ENDL;
        mAbandoned = true;
        mAllocator.abandon();
        mDevice = VK_NULL_HANDLE;
        mFrames.clear();
        return;
    }
    if (mDevice != VK_NULL_HANDLE && !waitIdle())
    {
        // [HANG GUARD 2026-11-08 8.2k] the bounded wait ran out: abandoned exactly as above
        LL_WARNS("Vulkan") << "Vulkan device abandoned without destruction (hung at shutdown): " << mHungReason << LL_ENDL;
        mAbandoned = true;
        mAllocator.abandon();
        mDevice = VK_NULL_HANDLE;
        mFrames.clear();
        return;
    }
    if (mAbandoned)
    {
        // [V4 2026-11-11] a later call (shutdown after a fall-back): the abandoned device's swapchain and surface are still
        // alive, so the instance they belong to stays too (VUID-vkDestroyInstance-instance-00629) and the loader stays
        // loaded under it; the process ends them
        return;
    }
    if (mDevice != VK_NULL_HANDLE)
    {
        destroyFrames();
        mAllocator.destroy();
        gVk.vkDestroyDevice(mDevice, nullptr);
        mDevice = VK_NULL_HANDLE;
        mQueue = VK_NULL_HANDLE;
    }
    if (mMessenger != VK_NULL_HANDLE && gVk.vkDestroyDebugUtilsMessengerEXT)
    {
        gVk.vkDestroyDebugUtilsMessengerEXT(mInstance, mMessenger, nullptr);
        mMessenger = VK_NULL_HANDLE;
    }
    if (mInstance != VK_NULL_HANDLE)
    {
        gVk.vkDestroyInstance(mInstance, nullptr);
        mInstance = VK_NULL_HANDLE;
    }
    mPhysicalDevices.clear();
    mChosen = WolfVkPhysicalDeviceInfo();
    WolfVkLoader::unloadLibrary();
}

void WolfVkDevice::onValidationMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types, const char* id, const char* message)
{
    mValidationMessages++;
    mLastValidationMessage = std::string(id) + ": " + message;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
    {
        LL_WARNS("VulkanValidation") << "ERROR " << (types & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT ? "(performance) " : "") << id << ": " << message << LL_ENDL;
    }
    else
    {
        LL_WARNS("VulkanValidation") << "warning " << (types & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT ? "(performance) " : "") << id << ": " << message << LL_ENDL;
    }
}

std::string WolfVkDevice::aboutLine() const
{
    if (mChosen.mHandle == VK_NULL_HANDLE)
    {
        return std::string();
    }
    return mChosen.name() + ", driver " + mChosen.driverString() + ", Vulkan " + mChosen.apiVersionString();
}
