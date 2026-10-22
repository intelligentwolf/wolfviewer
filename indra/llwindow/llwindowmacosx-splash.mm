/**
 * @file llwindowmacosx-splash.mm
 * @brief [WOLF SPLASH 2026-10-21] the WolfViewer splash's helper process on macOS (wolfsplash.h).
 *
 * This same executable started with --wolf-splash (llappviewermacosx.cpp main -> wolf_splash_helper_main, before
 * createNSApp, so nothing of the viewer runs here). A borderless, transparent, floating window shows the picture
 * WolfSplashRenderer draws (rounded corners, as on Windows), redrawn ~30 times a second by a timer; commands arrive on
 * stdin from LLSplashScreenMacOSX (llwindowmacosx.cpp) through WolfSplashChild. "F", or the viewer going away, fades
 * it out over 280 ms and ends the process.
 */

#import <Cocoa/Cocoa.h>

#include "linden_common.h"
#include "wolfsplash.h"
#include "llwindowmacosx.h"

#include <vector>

@interface WolfSplashView : NSView
{
@public
    WolfSplashRenderer* renderer;
    std::vector<uint32_t>* pixels;
    int pw, ph;
    CFAbsoluteTime start;
}
@end

@implementation WolfSplashView
- (BOOL) isOpaque { return NO; }
- (void) drawRect:(NSRect)dirty
{
    if (!renderer || !pixels || pixels->empty()) return;
    renderer->render(pixels->data(), CFAbsoluteTimeGetCurrent() - start);
    // premultiplied BGRA in little-endian uint32s (wolfsplash.h)
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef dp = CGDataProviderCreateWithData(NULL, pixels->data(), pixels->size() * 4, NULL);
    CGImageRef img = CGImageCreate(pw, ph, 8, 32, pw * 4, cs,
                                   (CGBitmapInfo)kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little, dp, NULL, false,   // CGBitmapInfo cast: Xcode 16 -Werror,-Wdeprecated-enum-enum-conversion
                                   kCGRenderingIntentDefault);
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    CGContextClearRect(ctx, NSRectToCGRect([self bounds]));
    CGContextDrawImage(ctx, NSRectToCGRect([self bounds]), img);
    CGImageRelease(img);
    CGDataProviderRelease(dp);
    CGColorSpaceRelease(cs);
}
- (NSView*) hitTest:(NSPoint)p { return nil; }   // the window is dragged by its background (below)
@end

int wolf_splash_helper_main(int argc, char** argv)
{
    @autoreleasepool
    {
        WolfSplashConfig cfg;
        if (!WolfSplash::fromArgs(argc, argv, 2, cfg)) return 1;

        [NSApplication sharedApplication];
        // no Dock icon or menu of its own: the viewer is the app
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

        // the screen holding the viewer's saved window position (Cocoa screen coordinates), else the main screen
        NSScreen* screen = [NSScreen mainScreen];
        if (cfg.has_hint)
        {
            for (NSScreen* s in [NSScreen screens])
            {
                if (NSPointInRect(NSMakePoint(cfg.hint_x, cfg.hint_y), [s frame])) { screen = s; break; }
            }
        }
        const CGFloat scale = [screen backingScaleFactor] > 0 ? [screen backingScaleFactor] : 1.0;
        const NSRect vis = [screen visibleFrame];
        const NSSize size = NSMakeSize(WolfSplash::LOGICAL_W, WolfSplash::LOGICAL_H);
        const NSRect frame = NSMakeRect(NSMidX(vis) - size.width / 2, NSMidY(vis) - size.height / 2, size.width, size.height);

        NSWindow* win = [[NSWindow alloc] initWithContentRect:frame styleMask:NSWindowStyleMaskBorderless
                                                      backing:NSBackingStoreBuffered defer:NO];
        [win setOpaque:NO];
        [win setBackgroundColor:[NSColor clearColor]];
        [win setHasShadow:YES];
        [win setLevel:NSFloatingWindowLevel];
        [win setMovableByWindowBackground:YES];
        [win setCollectionBehavior:NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorStationary];

        WolfSplashRenderer renderer;
        const int pw = (int)lround(size.width * scale), ph = (int)lround(size.height * scale);
        renderer.init(cfg, pw, ph, (float)scale, true);
        std::vector<uint32_t> pixels(size_t(pw) * ph);

        WolfSplashView* view = [[WolfSplashView alloc] initWithFrame:NSMakeRect(0, 0, size.width, size.height)];
        view->renderer = &renderer;
        view->pixels = &pixels;
        view->pw = pw;
        view->ph = ph;
        view->start = CFAbsoluteTimeGetCurrent();
        [win setContentView:view];
        [win orderFrontRegardless];

        // the block keeps a pointer: a block copies the C++ objects it names, and the renderer owns FreeType handles
        WolfSplashRenderer* rp = &renderer;
        __block std::string inbuf, status;
        __block bool fading = false;
        NSTimer* timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 30.0 repeats:YES block:^(NSTimer* t) {
            bool changed = false;
            WolfSplashChild::readCommands(inbuf, status, changed, fading);
            if (changed) rp->setStatus(status);
            if (fading)
            {
                [t invalidate];
                [NSAnimationContext runAnimationGroup:^(NSAnimationContext* ctx) {
                    ctx.duration = 0.28;
                    [[win animator] setAlphaValue:0.0];
                } completionHandler:^{
                    [win orderOut:nil];
                    [NSApp stop:nil];
                    // stop: takes effect after an event; post one so the run loop returns now
                    NSEvent* e = [NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint
                                               modifierFlags:0 timestamp:0 windowNumber:0 context:nil subtype:0
                                                       data1:0 data2:0];
                    [NSApp postEvent:e atStart:YES];
                }];
                return;
            }
            [view setNeedsDisplay:YES];
        }];
        [[NSRunLoop currentRunLoop] addTimer:timer forMode:NSRunLoopCommonModes];
        [NSApp run];
        [view release];
        [win release];
    }
    return 0;
}
