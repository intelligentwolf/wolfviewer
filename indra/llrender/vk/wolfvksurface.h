/**
 * @file wolfvksurface.h
 * @brief VkSurfaceKHR from the viewer's window on each platform.
 *
 * [VULKAN BACKEND 2026-11-08]
 *
 * Linux (LLWindowSDL, SDL 2.28): vkCreateXlibSurfaceKHR on the X11 Display/Window, or vkCreateWaylandSurfaceKHR on
 * the wl_display/wl_surface, whichever SDL is running on (LLWindow::getNativeHandles). SDL's own
 * SDL_Vulkan_CreateSurface is not usable: the viewer's window is made with SDL_WINDOW_OPENGL (llwindowsdl2.cpp:841),
 * and SDL 2 refuses a Vulkan surface on a window without SDL_WINDOW_VULKAN (SDL_vulkan.c SDL_Vulkan_CreateSurface:
 * "The specified window isn't a Vulkan window"), while the two flags cannot be combined on one window.
 * Windows (LLWindowWin32): vkCreateWin32SurfaceKHR on HINSTANCE/HWND.          UNTESTED here (no Windows machine).
 * macOS (LLWindowMacOSX): vkCreateMetalSurfaceEXT on a CAMetalLayer that wolfvksurface_macosx.mm attaches to the GL
 * content view; MoltenVK then presents through Metal.                          UNTESTED here (no Mac).
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

#ifndef WOLF_VK_SURFACE_H
#define WOLF_VK_SURFACE_H

#include "wolfvkloader.h"
#include "llwindow.h"
#include <string>

namespace WolfVkSurface
{
    // Creates the surface for the window; VK_NULL_HANDLE and a reason in error on failure.
    // instance: from WolfVkDevice::instance(). The surface is destroyed with destroy() (vkDestroySurfaceKHR).
    VkSurfaceKHR create(VkInstance instance, LLWindow* window, std::string& error);
    void destroy(VkInstance instance, VkSurfaceKHR surface);

    // "X11", "Wayland", "Win32" or "Metal" for the log and the renderer floater; empty before create.
    const std::string& kind();

#if LL_DARWIN
    // Implemented in wolfvksurface_macosx.mm: makes the NSView layer-backed with a CAMetalLayer and returns it
    // (the view keeps its own OpenGL drawing; the layer is what MoltenVK presents into). Null on failure.
    void* metalLayerForView(void* nsview, std::string& error);
#endif
}

#endif // WOLF_VK_SURFACE_H
