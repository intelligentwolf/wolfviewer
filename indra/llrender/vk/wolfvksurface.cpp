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
#include <cstdio>
#include <chrono>   // [WIN32 CHILD 2026-11-10] the wait for the window thread
#include <future>
#include <atomic>
#include <memory>

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

#if LL_WINDOWS
    // [WIN32 CHILD 2026-11-10] The child window the Win32 surface is made on, as the X11 child of T724 (wolfvksurface.h):
    // the viewer's HWND carries OpenGL's pixel format and WGL context (llwindowwin32.cpp SetPixelFormat :1545,
    // wglMakeCurrent :1560), and the Vulkan specification makes a native window "already associated with a non-Vulkan
    // graphics API surface" an error of vkCreateSwapchainKHR (vkCreateSwapchainKHR refpage, VK_ERROR_NATIVE_WINDOW_IN_USE_KHR).
    // The child has no pixel format and no GL context; only the Vulkan swapchain presents into it.
    // All three HWNDs are only read here on the main thread; the child is created, moved and destroyed on the window
    // thread through NativeHandles::mPostToWindowThread (llwindow.h).
    HWND sWin32Parent = nullptr;
    HWND sWin32Child = nullptr;
    std::function<void(std::function<void()>)> sWin32Post;
    const wchar_t* const sWin32ChildClass = L"WolfVulkanSurface";   // llwindowwin32.cpp WM_SIZE finds the child by this name
    bool sWin32ClassRegistered = false;

    // The child's window procedure, on the window thread.
    LRESULT CALLBACK wolfVkChildProc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param)
    {
        switch (msg)
        {
        case WM_NCHITTEST:
            // WM_NCHITTEST documentation: "HTTRANSPARENT: In a window currently covered by another window in the same
            // thread (the message will be sent to underlying windows in the same thread until one of them returns a code
            // that is not HTTRANSPARENT)". The viewer's window is beneath and on the same thread, so every mouse message
            // and WM_SETCURSOR go to it, with coordinates relative to it (the child sits at 0,0), exactly as without
            // the child. Keyboard input goes to the focus window, which the child never takes (no WS_TABSTOP, no SetFocus).
            return HTTRANSPARENT;
        case WM_ERASEBKGND:
            return 1;   // nothing of GDI's is painted: the swapchain owns every pixel (WM_ERASEBKGND: non-zero = erased)
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);   // validates the update region (BeginPaint documentation); no drawing
            EndPaint(hwnd, &ps);
            return 0;
        }
        default:
            return DefWindowProcW(hwnd, msg, w_param, l_param);
        }
    }

    // On the window thread: registers the class once and creates the child over the parent's client area.
    HWND createWin32ChildOnWindowThread(HINSTANCE instance, HWND parent, std::string& why)
    {
        if (!sWin32ClassRegistered)
        {
            // WNDCLASSEXW (winuser.h): no background brush (the child never paints), no cursor (WM_SETCURSOR goes to the
            // parent through HTTRANSPARENT), no icon, no CS_OWNDC (no GL on this window)
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.style = 0;
            wc.lpfnWndProc = wolfVkChildProc;
            wc.hInstance = instance;
            wc.lpszClassName = sWin32ChildClass;
            if (!RegisterClassExW(&wc))
            {
                const DWORD err = GetLastError();
                if (err != ERROR_CLASS_ALREADY_EXISTS)
                {
                    char buf[96];
                    snprintf(buf, sizeof(buf), "RegisterClassExW(WolfVulkanSurface) failed, error %lu", static_cast<unsigned long>(err));
                    why = buf;
                    return nullptr;
                }
            }
            sWin32ClassRegistered = true;
        }
        RECT client{};
        if (!GetClientRect(parent, &client))
        {
            why = "GetClientRect on the viewer's window failed";
            return nullptr;
        }
        // WS_CHILD | WS_VISIBLE: a child shown with its parent (hidden under the splash until the parent is shown);
        // WS_CLIPSIBLINGS as the parent has. The parent was created with WS_CLIPCHILDREN (llwindowwin32.cpp:1945), so
        // nothing the parent paints reaches the child's area - and OpenGL never swaps the parent in Vulkan mode anyway.
        HWND child = CreateWindowExW(0, sWin32ChildClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                     0, 0, client.right - client.left, client.bottom - client.top,
                                     parent, nullptr, instance, nullptr);
        if (!child)
        {
            char buf[96];
            snprintf(buf, sizeof(buf), "CreateWindowExW(WolfVulkanSurface) failed, error %lu", static_cast<unsigned long>(GetLastError()));
            why = buf;
            return nullptr;
        }
        return child;
    }

    // Main thread: asks the window thread for the child and waits for it (bounded). The window thread is a message
    // pump that never waits on the main thread (llwindowwin32.cpp LLWindowWin32Thread::run: GetMessage / PeekMessage and
    // its work queue; what it sends the main thread goes through a queue, WINDOW_IMP_POST), so the wait cannot deadlock;
    // on a timeout the request is abandoned (the shared flag) and the surface goes on the viewer's window.
    HWND createWin32Child(HINSTANCE instance, HWND parent, std::string& why)
    {
        if (!sWin32Post)
        {
            why = "the window layer gave no way to reach its window thread";
            return nullptr;
        }
        struct Request { std::promise<HWND> mResult; std::string mWhy; std::atomic<bool> mAbandoned{ false }; };
        auto request = std::make_shared<Request>();
        std::future<HWND> future = request->mResult.get_future();
        sWin32Post([request, instance, parent]()
        {
            if (request->mAbandoned.load()) return;   // the main thread gave up: nothing is created
            HWND child = createWin32ChildOnWindowThread(instance, parent, request->mWhy);
            request->mResult.set_value(child);
        });
        if (future.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
        {
            request->mAbandoned.store(true);
            why = "the window thread did not answer within 5 s";
            return nullptr;
        }
        HWND child = future.get();
        if (!child) why = request->mWhy;
        return child;
    }

    std::string win32WindowLine(HWND hwnd)
    {
        char buf[160];
        RECT client{};
        if (!IsWindow(hwnd) || !GetClientRect(hwnd, &client))
        {
            snprintf(buf, sizeof(buf), "HWND %p (not a window)", static_cast<void*>(hwnd));
            return buf;
        }
        snprintf(buf, sizeof(buf), "HWND %p client %ldx%ld %s", static_cast<void*>(hwnd), static_cast<long>(client.right - client.left),
                 static_cast<long>(client.bottom - client.top), IsWindowVisible(hwnd) ? "visible" : "hidden");
        return buf;
    }
#endif

#if !LL_WINDOWS && !LL_DARWIN
    // [T724 2026-11-10] the X11 child window the surface is made on (wolfvksurface.h); None when the surface is on the
    // viewer's own window (shared_x11_window, or the child could not be made)
    Display* sX11Display = nullptr;
    Window sX11Parent = 0;
    Window sX11Child = 0;

    // Xlib reports protocol errors asynchronously through the error handler (Xlib.h:1848 XSetErrorHandler); the viewer's
    // own handler (llwindowsdl2.cpp wolfX11ErrorHandler) logs and carries on. During the child window's creation this one
    // records the code instead, and XSync (Xlib.h:3419) makes the server answer before the handler is put back.
    int sX11ErrorCode = 0;
    int recordX11Error(Display*, XErrorEvent* event)
    {
        if (event && sX11ErrorCode == 0) sX11ErrorCode = event->error_code;
        return 0;
    }

    std::string x11WindowLine(Display* dpy, Window w)
    {
        // Source: Xlib.h:2701 XGetWindowAttributes; :306-308 XWindowAttributes width, height, depth; map_state (X.h:445-447
        // IsUnmapped 0 / IsUnviewable 1 / IsViewable 2)
        XWindowAttributes attr{};
        char buf[160];
        if (XGetWindowAttributes(dpy, w, &attr) == 0)
        {
            snprintf(buf, sizeof(buf), "window 0x%lx (attributes unavailable)", w);
            return buf;
        }
        const char* state = attr.map_state == IsViewable ? "viewable" : attr.map_state == IsUnviewable ? "mapped, parent unmapped" : "unmapped";
        snprintf(buf, sizeof(buf), "window 0x%lx depth %d %dx%d %s", w, attr.depth, attr.width, attr.height, state);
        return buf;
    }

    // Creates and maps the child window; 0 with a reason when the server refused it (the surface then goes on parent).
    Window createX11Child(Display* dpy, Window parent, std::string& why)
    {
        // Source: Xlib.h:2701 XGetWindowAttributes; the child takes the parent's geometry
        XWindowAttributes attr{};
        if (XGetWindowAttributes(dpy, parent, &attr) == 0 || attr.width <= 0 || attr.height <= 0)
        {
            why = "XGetWindowAttributes on the viewer's window failed";
            return 0;
        }
        // Source: Xlib.h:1632 XCreateWindow(display, parent, x, y, width, height, border_width, depth, class, visual,
        // valuemask, attributes). Depth and visual CopyFromParent (X.h:121): the same visual as the viewer's window,
        // so no colormap or border pixel is needed (a window of a different depth or visual than its parent needs
        // both, or BadMatch). InputOutput (X.h:387): a window with contents, which a swapchain needs (vkcube's own
        // window is XCB_WINDOW_CLASS_INPUT_OUTPUT, Vulkan-Tools cube.c:3006-3007). Background black as vkcube's
        // (cube.c:3002-3003 XCB_CW_BACK_PIXEL black_pixel): the server paints it black on exposure until the next
        // present. Event mask none (X.h:150 NoEventMask): a pointer event in the child that the child did not select
        // propagates to the nearest ancestor that did (X11 protocol, "event propagation"), which is the viewer's
        // window (SDL selects ButtonPress/Release, PointerMotion, Enter/Leave on it, SDL_x11window.c:674), so clicks and
        // mouse movement reach SDL as before, with coordinates relative to the viewer's window. The keyboard goes to
        // the focus window (the viewer's top-level) regardless. The cursor is the parent's (no cursor set on the child).
        XSetWindowAttributes swa{};
        swa.background_pixel = BlackPixel(dpy, DefaultScreen(dpy));
        swa.event_mask = NoEventMask;
        sX11ErrorCode = 0;
        XErrorHandler previous = XSetErrorHandler(recordX11Error);
        Window child = XCreateWindow(dpy, parent, 0, 0, static_cast<unsigned int>(attr.width), static_cast<unsigned int>(attr.height),
                                     0, CopyFromParent, InputOutput, CopyFromParent, CWBackPixel | CWEventMask, &swa);   // X.h:393, :403
        if (child != 0)
        {
            XMapWindow(dpy, child);   // Xlib.h:2816; viewable once the viewer's window is (it is created hidden under the splash)
        }
        XSync(dpy, False);   // Xlib.h:3419: every request above answered, errors delivered to recordX11Error
        XSetErrorHandler(previous);
        if (child == 0 || sX11ErrorCode != 0)
        {
            char text[128] = { 0 };
            if (sX11ErrorCode != 0) XGetErrorText(dpy, sX11ErrorCode, text, sizeof(text) - 1);
            why = std::string("XCreateWindow/XMapWindow of the Vulkan child window failed: ") + (sX11ErrorCode != 0 ? text : "no window id");
            if (child != 0)
            {
                XDestroyWindow(dpy, child);   // Xlib.h:2253
                XFlush(dpy);                   // Xlib.h:2517
            }
            return 0;
        }
        return child;
    }
#endif
}

namespace WolfVkSurface
{

const std::string& kind()
{
    return sKind;
}

VkSurfaceKHR create(VkInstance instance, LLWindow* window, bool shared_x11_window, std::string& error)
{
    sKind.clear();
#if !LL_WINDOWS && !LL_DARWIN
    sX11Display = nullptr;
    sX11Parent = 0;
    sX11Child = 0;
#elif LL_WINDOWS
    sWin32Parent = nullptr;
    sWin32Child = nullptr;
    sWin32Post = nullptr;
#else
    (void)shared_x11_window;
#endif
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
    HWND target = static_cast<HWND>(handles.mHandle);
    sWin32Parent = target;
    sWin32Post = handles.mPostToWindowThread;
    // [WIN32 CHILD 2026-11-10] the surface's own child window (above): WGL never touches it
    if (shared_x11_window)
    {
        LL_WARNS("Vulkan") << "WolfVulkanDebugSharedWindow: the Vulkan surface shares the viewer's window with OpenGL (W101/W102 behaviour)" << LL_ENDL;
    }
    else
    {
        std::string why;
        sWin32Child = createWin32Child(static_cast<HINSTANCE>(handles.mInstance), target, why);
        if (sWin32Child)
        {
            target = sWin32Child;
        }
        else
        {
            LL_WARNS("Vulkan") << why << "; the Vulkan surface goes on the viewer's window itself" << LL_ENDL;
        }
    }
    VkWin32SurfaceCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;   // vulkan_core.h:488
    info.hinstance = static_cast<HINSTANCE>(handles.mInstance);
    info.hwnd = target;
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
        Display* dpy = static_cast<Display*>(handles.mDisplay);
        Window target = static_cast<Window>(handles.mWindow);
        sX11Display = dpy;
        sX11Parent = target;
        // [T724 2026-11-10] the surface's own child window (wolfvksurface.h): GLX never touches it
        if (shared_x11_window)
        {
            LL_WARNS("Vulkan") << "WolfVulkanDebugSharedWindow: the Vulkan surface shares the viewer's X window with OpenGL (W101/W102 behaviour)" << LL_ENDL;
        }
        else
        {
            std::string why;
            sX11Child = createX11Child(dpy, target, why);
            if (sX11Child != 0)
            {
                target = sX11Child;
            }
            else
            {
                LL_WARNS("Vulkan") << why << "; the Vulkan surface goes on the viewer's X window itself" << LL_ENDL;
            }
        }
        VkXlibSurfaceCreateInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;   // vulkan_core.h:484
        info.dpy = dpy;
        info.window = target;
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
        destroy(instance, VK_NULL_HANDLE);   // [T724] the child window, if one was made
        return VK_NULL_HANDLE;
    }
    const std::string where = describe();
    LL_INFOS("Vulkan") << "Window surface created (" << sKind << ")" << (where.empty() ? "" : ": " + where) << LL_ENDL;
    return surface;
}

void destroy(VkInstance instance, VkSurfaceKHR surface)
{
    if (instance != VK_NULL_HANDLE && surface != VK_NULL_HANDLE && gVk.vkDestroySurfaceKHR)
    {
        gVk.vkDestroySurfaceKHR(instance, surface, nullptr);
    }
#if !LL_WINDOWS && !LL_DARWIN
    // [T724] after the surface (VUID-vkDestroySurfaceKHR-surface-01266: the surface after its swapchains; the window after
    // the surface that was made on it)
    if (sX11Display && sX11Child != 0)
    {
        XDestroyWindow(sX11Display, sX11Child);   // Xlib.h:2253
        XFlush(sX11Display);                       // Xlib.h:2517
    }
    sX11Display = nullptr;
    sX11Parent = 0;
    sX11Child = 0;
#elif LL_WINDOWS
    // [WIN32 CHILD 2026-11-10] after the surface, on the window thread (DestroyWindow must be called by the thread that
    // created the window - DestroyWindow documentation). A parent already gone took the child with it: IsWindow checks.
    if (sWin32Child && sWin32Post)
    {
        HWND child = sWin32Child;
        sWin32Post([child]() { if (IsWindow(child)) DestroyWindow(child); });
    }
    sWin32Parent = nullptr;
    sWin32Child = nullptr;
    sWin32Post = nullptr;
#endif
    sKind.clear();
}

void resize(uint32_t width, uint32_t height)
{
#if !LL_WINDOWS && !LL_DARWIN
    if (sX11Display && sX11Child != 0 && width > 0 && height > 0)
    {
        // Source: Xlib.h:2835 XMoveResizeWindow(display, w, x, y, width, height); XSync (:3419) so the server holds the new
        // size before the swapchain build asks for currentExtent
        XMoveResizeWindow(sX11Display, sX11Child, 0, 0, static_cast<unsigned int>(width), static_cast<unsigned int>(height));
        XSync(sX11Display, False);
    }
#elif LL_WINDOWS
    // [WIN32 CHILD 2026-11-10] on the window thread, asynchronously: llwindowwin32.cpp's WM_SIZE already sized the child
    // when the parent changed (before the main thread heard of it), so this is the first size and a safety net. A
    // swapchain built before a pending move lands comes back VK_ERROR_OUT_OF_DATE_KHR at the next present and is rebuilt
    // (WolfVkBridge sResizePending), the same as any window resize.
    if (sWin32Child && sWin32Post && width > 0 && height > 0)
    {
        HWND child = sWin32Child;
        const int w = static_cast<int>(width), h = static_cast<int>(height);
        sWin32Post([child, w, h]()
        {
            if (IsWindow(child)) SetWindowPos(child, nullptr, 0, 0, w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        });
    }
#else
    (void)width;
    (void)height;
#endif
}

std::string describe()
{
#if !LL_WINDOWS && !LL_DARWIN
    if (!sX11Display || sX11Parent == 0) return std::string();
    std::string line = "viewer's " + x11WindowLine(sX11Display, sX11Parent);
    if (sX11Child != 0) line += "; Vulkan child " + x11WindowLine(sX11Display, sX11Child);
    else line += "; the surface is on the viewer's window itself";
    return line;
#elif LL_WINDOWS
    if (!sWin32Parent) return std::string();
    std::string line = "viewer's " + win32WindowLine(sWin32Parent);
    if (sWin32Child) line += "; Vulkan child " + win32WindowLine(sWin32Child);
    else line += "; the surface is on the viewer's window itself";
    return line;
#else
    return std::string();
#endif
}

} // namespace WolfVkSurface
