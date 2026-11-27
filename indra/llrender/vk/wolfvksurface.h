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
 * Windows (LLWindowWin32): vkCreateWin32SurfaceKHR on HINSTANCE and a child HWND of the viewer's window
 * ([WIN32 CHILD 2026-11-10], the Win32 twin of T724's X11 child: the viewer's HWND carries WGL's pixel format and
 * context; the child has neither, class "WolfVulkanSurface", made/moved/destroyed on the window thread through
 * NativeHandles::mPostToWindowThread, mouse input passed down by HTTRANSPARENT).  UNTESTED here (no Windows machine).
 * macOS (LLWindowMacOSX): vkCreateMetalSurfaceEXT on the CAMetalLayer of a child NSView that wolfvksurface_macosx.mm
 * puts over the GL content view ([W114-VULKAN MAC 2026-11-26], the twin of the X11/Win32 children; the GL view itself is
 * left alone); MoltenVK then presents through Metal.                          UNTESTED here (no Mac).
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
#include <cstdint>
#include <string>

namespace WolfVkSurface
{
    // Creates the surface for the window; VK_NULL_HANDLE and a reason in error on failure.
    // instance: from WolfVkDevice::instance(). The surface is destroyed with destroy() (vkDestroySurfaceKHR).
    //
    // [T724 2026-11-10] X11 (and [WIN32 CHILD 2026-11-10] Win32, the same way): the surface goes on a child window of the
    // viewer's window, not on the viewer's window itself, unless shared_x11_window is true. The viewer's window carries OpenGL's GLX drawable (SDL_GL_CreateContext /
    // SDL_GL_MakeCurrent bind the context to it, llwindowsdl2.cpp:935/3575), and the Vulkan specification makes a
    // native window "already associated with a non-Vulkan graphics API surface" an error of vkCreateSwapchainKHR
    // (VK_ERROR_NATIVE_WINDOW_IN_USE_KHR, vkCreateSwapchainKHR refpage). NVIDIA's driver on a pure X11 session refused
    // it with VK_ERROR_INITIALIZATION_FAILED (ticket 724, RTX 4060 Ti, 580.178.04, Linux Mint 22.2 X11), while vkcube on
    // a window of its own works there with the same loader and driver. The child window is a plain X window like
    // vkcube's (same depth and visual as its parent, no GLX drawable, no events of its own), covers the viewer's window
    // and follows its size (resize()); the pointer events it receives propagate to the viewer's window, which selected
    // them. OpenGL keeps drawing into the viewer's window's own back buffer (never swapped in Vulkan mode) and the
    // frame's layers are taken from there as before. shared_x11_window = true is the W101/W102 behaviour, for the test
    // setting WolfVulkanDebugSharedWindow.
    VkSurfaceKHR create(VkInstance instance, LLWindow* window, bool shared_x11_window, std::string& error);
    // Healthy device, every swapchain on the surface destroyed: the surface, then its child window / Metal layer.
    void destroy(VkInstance instance, VkSurfaceKHR surface);
    // [V2 2026-11-11] The surface cannot be destroyed (hung device, or a swapchain still alive): it is left, but the
    // child window is unmapped (X11) / hidden (Win32) / the Metal child view hidden (macOS), so that the
    // viewer's own window - where OpenGL carries on - is never covered. Needs no device. Wayland has no child.
    void abandon();

    // [T724] X11 child window: moved to (0,0) and resized to width x height (the viewer's window's drawable size), then
    // XSync, so that vkGetPhysicalDeviceSurfaceCapabilitiesKHR's currentExtent is the new size. Called before every
    // swapchain build. [W114-VULKAN MAC 2026-11-26] macOS: the Metal child view's frame and contentsScale (the size comes
    // from the GL view, whose backing size is width x height). No-op on Wayland.
    void resize(uint32_t width, uint32_t height);

    // [T724] One line about the native window(s) behind the surface for the log when a swapchain cannot be made: X11
    // "window 0x.. depth N WxH map_state S; Vulkan child 0x.. WxH map_state S". Empty on the other surfaces.
    std::string describe();

    // "X11", "Wayland", "Win32" or "Metal" for the log and the renderer floater; empty before create.
    const std::string& kind();

#if LL_DARWIN
    // Implemented in wolfvksurface_macosx.mm ([W114-VULKAN MAC 2026-11-26] child view):
    // adds a child view with a CAMetalLayer over the GL view nsview and returns the layer (what MoltenVK presents into;
    // the GL view and its OpenGL drawable are not touched). Null on failure. A second call returns the same layer.
    void* metalLayerForView(void* nsview, std::string& error);
    // the child takes the GL view's bounds again and its layer's contentsScale the backing scale (before a swapchain build)
    void metalLayerResize();
    // abandon(): the child is hidden and forgotten, never removed (MoltenVK's live swapchain keeps its layer)
    void metalLayerHide();
    // destroy(): the child is removed from the GL view and released (no-op if none)
    void metalLayerDetach();
#endif
}

#endif // WOLF_VK_SURFACE_H
