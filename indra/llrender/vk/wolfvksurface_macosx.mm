/**
 * @file wolfvksurface_macosx.mm
 * @brief CAMetalLayer on the viewer's content view, for VkMetalSurfaceCreateInfoEXT (MoltenVK).
 *
 * [VULKAN BACKEND 2026-11-08] UNTESTED: written on Linux, compiled by the macOS CI build only.
 *
 * MoltenVK draws into a CAMetalLayer. Apple's documentation for CAMetalLayer
 * (https://developer.apple.com/documentation/quartzcore/cametallayer) and for layer-backed views
 * (NSView wantsLayer / layer, https://developer.apple.com/documentation/appkit/nsview/1483695-wantslayer) describe
 * the two calls below: a view made layer-backed takes any CALayer subclass as its layer. MoltenVK's own
 * documentation (MoltenVK README, "Interacting with the MoltenVK Runtime": "the application must set the NSView's
 * layer to a CAMetalLayer and set wantsLayer to YES") prescribes the same.
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

namespace WolfVkSurface
{

void* metalLayerForView(void* nsview, std::string& error)
{
    NSView* view = (__bridge NSView*)nsview;
    if (!view)
    {
        error = "no NSView for the Metal layer";
        return nullptr;
    }
    __block CAMetalLayer* result = nil;
    void (^attach)(void) = ^{
        CALayer* existing = [view layer];
        if (existing && [existing isKindOfClass:[CAMetalLayer class]])
        {
            result = (CAMetalLayer*)existing;
            return;
        }
        CAMetalLayer* layer = [CAMetalLayer layer];
        // The layer's pixel size follows the view's backing scale (Retina), as NSView convertSizeToBacking does for
        // the GL drawable (llwindowmacosx-objc.mm getDeviceUnitSize).
        layer.contentsScale = [[view window] backingScaleFactor];
        [view setWantsLayer:YES];
        [view setLayer:layer];
        result = layer;
    };
    // AppKit view changes belong on the main thread.
    if ([NSThread isMainThread])
    {
        attach();
    }
    else
    {
        dispatch_sync(dispatch_get_main_queue(), attach);
    }
    if (!result)
    {
        error = "could not attach a CAMetalLayer to the view";
        return nullptr;
    }
    return (__bridge void*)result;
}

} // namespace WolfVkSurface
