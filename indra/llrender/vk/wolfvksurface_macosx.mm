/**
 * @file wolfvksurface_macosx.mm
 * @brief A child NSView with a CAMetalLayer over the viewer's GL view, for VkMetalSurfaceCreateInfoEXT (MoltenVK).
 *
 * [VULKAN BACKEND 2026-11-08] UNTESTED: written on Linux, compiled by the macOS CI build only.
 *
 * [W114-VULKAN MAC 2026-11-26] Rewritten from "the GL view's own layer replaced by a CAMetalLayer" to a CHILD VIEW, the
 * macOS twin of the X11 child window (T724) and the Win32 child HWND (WIN32 CHILD) in wolfvksurface.cpp:
 *  - The viewer's content view is LLOpenGLView, an NSOpenGLView whose NSOpenGLContext is attached to it
 *    (llopenglview-objc.mm:221-276 initWithFrame: setOpenGLContext / [glContext setView:self];
 *    llwindowmacosx-objc.mm:236-237 setContentView:glview). The old code made that view layer-backed and then replaced
 *    its backing layer (setWantsLayer:YES, then setLayer:), which takes the GL context's drawable away from OpenGL while
 *    OpenGL is still the fall-back renderer. It also never made the view the layer's delegate, which MoltenVK needs to
 *    see a replaced layer (MVKSurface.mm initLayer: it observes the "layer" key path of the layer's delegate when the
 *    delegate is an NSView, and reports VK_ERROR_SURFACE_LOST_KHR when it changes), and set contentsScale once only.
 *  - Now the GL view is never touched. A WolfVkMetalView is added as its subview, covering it and following its size
 *    (autoresizingMask width + height). The child is layer-backed with a CAMetalLayer made by makeBackingLayer, the
 *    pattern of SDL's Metal view (SDL 2.30 src/video/cocoa/SDL_cocoametalview.m: wantsUpdateLayer YES, makeBackingLayer
 *    returns a CAMetalLayer, wantsLayer YES, autoresizing width/height, hitTest: returns nil, addSubview: on the
 *    window's content view). A view that creates its own backing layer is that layer's delegate (MoltenVK Runtime
 *    User Guide, quoting Apple: "If the layer object was created by a view, the view typically assigns itself as the
 *    layer's delegate automatically"), so MoltenVK's layer observer works.
 *  - Input: hitTest: returns nil (as SDL's), so every mouse event goes to LLOpenGLView beneath, which handles all of the
 *    viewer's input; the child never becomes first responder (NSView acceptsFirstResponder is NO by default).
 *  - Size: MoltenVK's surface extent is the layer's bounds times its contentsScale (CAMetalLayer+MoltenVK.mm
 *    naturalDrawableSizeMVK; MVKDevice.mm getSurfaceCapabilities currentExtent = getNaturalExtent()), and it sets the
 *    layer's drawableSize itself at each swapchain (MVKSwapchain.mm initCAMetalLayer). contentsScale is therefore kept
 *    equal to the view's backing scale, the ratio convertSizeToBacking gives - the same conversion LLWindowMacOSX::getSize
 *    uses for the size WolfVkBridge builds swapchains for (llwindowmacosx.cpp:1047-1066 -> llwindowmacosx-objc.mm:272-275
 *    getBackingViewRect convertRectToBacking) - at attach, at every resize() before a swapchain build, and when the
 *    window moves to a screen of another scale (viewDidChangeBackingProperties). A layer size that no longer matches the
 *    swapchain makes MoltenVK return VK_SUBOPTIMAL_KHR (MVKSwapchain.mm hasOptimalSurface), which WolfVkBridge rebuilds on.
 *
 * Built without ARC (no -fobjc-arc in indra/cmake or llrender/CMakeLists.txt): the child is retained by hand.
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

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

@interface WolfVkMetalView : NSView
- (void)wolfSyncScale;
@end

@implementation WolfVkMetalView

- (BOOL)wantsUpdateLayer
{
    return YES;
}

- (CALayer*)makeBackingLayer
{
    return [CAMetalLayer layer];
}

- (NSView*)hitTest:(NSPoint)point
{
    (void)point;
    return nil;
}

- (BOOL)isOpaque
{
    return YES;
}

- (void)viewDidChangeBackingProperties
{
    [super viewDidChangeBackingProperties];
    [self wolfSyncScale];
}

// contentsScale = backing pixels per point of this view (see the file comment)
- (void)wolfSyncScale
{
    const NSSize points = [self bounds].size;
    if (points.width <= 0.0 || points.height <= 0.0 || ![self window]) return;
    const NSSize pixels = [self convertSizeToBacking:points];
    [[self layer] setContentsScale:pixels.height / points.height];
}

@end

namespace
{
    WolfVkMetalView* sMetalView = nil;   // retained by us (+1 from alloc) while attached

    // AppKit view changes belong on the main thread.
    void onMain(void (^block)(void))
    {
        if ([NSThread isMainThread]) block();
        else dispatch_sync(dispatch_get_main_queue(), block);
    }
}

namespace WolfVkSurface
{

void* metalLayerForView(void* nsview, std::string& error)
{
    NSView* parent = (__bridge NSView*)nsview;
    if (!parent)
    {
        error = "no NSView for the Metal layer";
        return nullptr;
    }
    __block CALayer* result = nil;
    onMain(^{
        if (sMetalView)
        {
            result = [sMetalView layer];
            return;
        }
        WolfVkMetalView* child = [[WolfVkMetalView alloc] initWithFrame:[parent bounds]];
        if (!child) return;
        [child setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
        [child setWantsLayer:YES];
        [parent addSubview:child];
        [child wolfSyncScale];
        CALayer* layer = [child layer];
        if (!layer || ![layer isKindOfClass:[CAMetalLayer class]])
        {
            [child removeFromSuperview];
            [child release];
            return;
        }
        sMetalView = child;
        result = layer;
    });
    if (!result)
    {
        error = "could not add a Metal child view to the viewer's view";
        return nullptr;
    }
    return (__bridge void*)result;
}

void metalLayerResize()
{
    if (!sMetalView) return;
    onMain(^{
        if (!sMetalView) return;
        NSView* parent = [sMetalView superview];
        if (parent) [sMetalView setFrame:[parent bounds]];
        [sMetalView wolfSyncScale];
    });
}

void metalLayerHide()
{
    if (!sMetalView) return;
    onMain(^{
        if (!sMetalView) return;
        // the parent keeps the child (and MoltenVK the layer) alive; our reference is dropped and the handle forgotten
        [sMetalView setHidden:YES];
        [sMetalView release];
        sMetalView = nil;
    });
}

void metalLayerDetach()
{
    if (!sMetalView) return;
    onMain(^{
        if (!sMetalView) return;
        [sMetalView removeFromSuperview];
        [sMetalView release];
        sMetalView = nil;
    });
}

} // namespace WolfVkSurface
