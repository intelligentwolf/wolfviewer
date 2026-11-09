/**
 * @file wolfvksurface.cpp
 * @brief VkSurfaceKHR from the viewer's window (see wolfvksurface.h).
 *
 * [VULKAN BACKEND 2026-11-08] The only file (with wolfvksurface_macosx.mm) that includes the platform Vulkan headers.
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
#include "wolfvksurface.h"

#if LL_WINDOWS
#include <windows.h>
// Source: vulkan_win32.h:27-33 VkWin32SurfaceCreateInfoKHR { sType, pNext, flags, hinstance, hwnd }; :35 PFN_vkCreateWin32SurfaceKHR
#include "third_party/vulkan-headers/include/vulkan/vulkan_win32.h"
#elif LL_DARWIN
// Source: vulkan_metal.h:33-38 VkMetalSurfaceCreateInfoEXT { sType, pNext, flags, pLayer }; :40 PFN_vkCreateMetalSurfaceEXT
#include "third_party/vulkan-headers/include/vulkan/vulkan_metal.h"
#else
#include <X11/Xlib.h>
// Source: vulkan_xlib.h:27-33 VkXlibSurfaceCreateInfoKHR { sType, pNext, flags, dpy, window }; :35 PFN_vkCreateXlibSurfaceKHR
#include "third_party/vulkan-headers/include/vulkan/vulkan_xlib.h"
// vulkan_wayland.h only names struct wl_display / struct wl_surface (lines 31-32), so no wayland-client.h is needed.
struct wl_display;
struct wl_surface;
// Source: vulkan_wayland.h:27-33 VkWaylandSurfaceCreateInfoKHR { sType, pNext, flags, display, surface }; :35 PFN_vkCreateWaylandSurfaceKHR
#include "third_party/vulkan-headers/include/vulkan/vulkan_wayland.h"
#endif

namespace
{
    std::string sKind;
}

namespace WolfVkSurface
{

const std::string& kind()
{
    return sKind;
}

VkSurfaceKHR create(VkInstance instance, LLWindow* window, std::string& error)
{
    sKind.clear();
    if (instance == VK_NULL_HANDLE || !window)
    {
        error = "no instance or window";
        return VK_NULL_HANDLE;
    }
    LLWindow::NativeHandles handles;
    if (!window->getNativeHandles(handles))
    {
        error = "the window layer gave no native window handles";
        return VK_NULL_HANDLE;
    }

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkResult res = VK_ERROR_INITIALIZATION_FAILED;

#if LL_WINDOWS
    if (handles.mKind != LLWindow::NativeHandles::KIND_WIN32)
    {
        error = "expected Win32 window handles";
        return VK_NULL_HANDLE;
    }
    PFN_vkCreateWin32SurfaceKHR create_fn = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(gVk.vkGetInstanceProcAddr(instance, "vkCreateWin32SurfaceKHR"));
    if (!create_fn)
    {
        error = "vkCreateWin32SurfaceKHR is not available (VK_KHR_win32_surface missing)";
        return VK_NULL_HANDLE;
    }
    VkWin32SurfaceCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;   // vulkan_core.h:488
    info.hinstance = static_cast<HINSTANCE>(handles.mInstance);
    info.hwnd = static_cast<HWND>(handles.mHandle);
    res = create_fn(instance, &info, nullptr, &surface);
    sKind = "Win32";
#elif LL_DARWIN
    if (handles.mKind != LLWindow::NativeHandles::KIND_COCOA)
    {
        error = "expected a Cocoa view handle";
        return VK_NULL_HANDLE;
    }
    PFN_vkCreateMetalSurfaceEXT create_fn = reinterpret_cast<PFN_vkCreateMetalSurfaceEXT>(gVk.vkGetInstanceProcAddr(instance, "vkCreateMetalSurfaceEXT"));
    if (!create_fn)
    {
        error = "vkCreateMetalSurfaceEXT is not available (VK_EXT_metal_surface missing; is MoltenVK installed?)";
        return VK_NULL_HANDLE;
    }
    void* layer = metalLayerForView(handles.mHandle, error);
    if (!layer)
    {
        return VK_NULL_HANDLE;
    }
    VkMetalSurfaceCreateInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;   // vulkan_core.h:795
    info.pLayer = static_cast<const CAMetalLayer*>(layer);
    res = create_fn(instance, &info, nullptr, &surface);
    sKind = "Metal";
#else
    if (handles.mKind == LLWindow::NativeHandles::KIND_X11)
    {
        PFN_vkCreateXlibSurfaceKHR create_fn = reinterpret_cast<PFN_vkCreateXlibSurfaceKHR>(gVk.vkGetInstanceProcAddr(instance, "vkCreateXlibSurfaceKHR"));
        if (!create_fn)
        {
            error = "vkCreateXlibSurfaceKHR is not available (VK_KHR_xlib_surface missing)";
            return VK_NULL_HANDLE;
        }
        VkXlibSurfaceCreateInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;   // vulkan_core.h:484
        info.dpy = static_cast<Display*>(handles.mDisplay);
        info.window = static_cast<Window>(handles.mWindow);
        res = create_fn(instance, &info, nullptr, &surface);
        sKind = "X11";
    }
    else if (handles.mKind == LLWindow::NativeHandles::KIND_WAYLAND)
    {
        PFN_vkCreateWaylandSurfaceKHR create_fn = reinterpret_cast<PFN_vkCreateWaylandSurfaceKHR>(gVk.vkGetInstanceProcAddr(instance, "vkCreateWaylandSurfaceKHR"));
        if (!create_fn)
        {
            error = "vkCreateWaylandSurfaceKHR is not available (VK_KHR_wayland_surface missing)";
            return VK_NULL_HANDLE;
        }
        VkWaylandSurfaceCreateInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;   // vulkan_core.h:486
        info.display = static_cast<struct wl_display*>(handles.mDisplay);
        info.surface = static_cast<struct wl_surface*>(handles.mSurface);
        res = create_fn(instance, &info, nullptr, &surface);
        sKind = "Wayland";
    }
    else
    {
        error = "expected X11 or Wayland window handles";
        return VK_NULL_HANDLE;
    }
#endif

    if (res != VK_SUCCESS || surface == VK_NULL_HANDLE)
    {
        error = "vkCreate" + sKind + "Surface: " + WolfVkLoader::resultString(res);
        sKind.clear();
        return VK_NULL_HANDLE;
    }
    LL_INFOS("Vulkan") << "Window surface created (" << sKind << ")" << LL_ENDL;
    return surface;
}

void destroy(VkInstance instance, VkSurfaceKHR surface)
{
    if (instance != VK_NULL_HANDLE && surface != VK_NULL_HANDLE && gVk.vkDestroySurfaceKHR)
    {
        gVk.vkDestroySurfaceKHR(instance, surface, nullptr);
    }
    sKind.clear();
}

} // namespace WolfVkSurface
