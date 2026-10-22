#import "selftest.h"

#import <Carbon/Carbon.h>

#import "settings.h"

#include "port/platform.h"
#include "port/ppu.h"

static void PostKey(NSWindow* window, unsigned short keyCode, BOOL down) {
    NSEvent* event = [NSEvent keyEventWithType:down ? NSEventTypeKeyDown : NSEventTypeKeyUp
                                      location:NSZeroPoint
                                 modifierFlags:0
                                     timestamp:NSProcessInfo.processInfo.systemUptime
                                  windowNumber:window.windowNumber
                                       context:nil
                                    characters:@""
                   charactersIgnoringModifiers:@""
                                     isARepeat:NO
                                       keyCode:keyCode];
    [window sendEvent:event];
}

void SelfTestStart(NSWindow* window, MTKView* view, Renderer* renderer, NSString* dir) {
    NSURL* out = [NSURL fileURLWithPath:dir isDirectory:YES];
    [NSFileManager.defaultManager createDirectoryAtURL:out withIntermediateDirectories:YES attributes:nil error:nil];
    NSURL* (^file)(NSString*) = ^(NSString* name) {
        return [out URLByAppendingPathComponent:[name stringByAppendingString:@".png"]];
    };
    view.framebufferOnly = NO;
    window.frameAutosaveName = @""; // the resizing below must not replace the user's window frame
    // Test with the built-in knob values, not the ones saved for this user.
    renderer.params = FilterParamsDefaults();
    renderer.integerScaling = YES;

    NSMutableArray* steps = [NSMutableArray array];
    __block uint64_t at = 360; // title screen is up by now
    void (^add)(uint64_t, void (^)(void)) = ^(uint64_t delay, void (^action)(void)) {
        at += delay;
        [steps addObject:@[ @(at), action ]];
    };
    for (NSUInteger mode = 0; mode < FilterModeCount; mode++) {
        add(4, ^{ renderer.filter = mode; });
        add(4, ^{ [renderer captureNextFrameToURL:file([NSString stringWithFormat:@"title_%lu", (unsigned long)mode])]; });
    }
    add(4, ^{ renderer.filter = FilterModeNearest; });
    add(10, ^{ PostKey(window, kVK_Return, YES); }); // Start
    add(5, ^{ PostKey(window, kVK_Return, NO); });
    add(150, ^{ [renderer captureNextFrameToURL:file(@"after_start")]; });
    add(10, ^{ PostKey(window, kVK_ANSI_X, YES); }); // A
    add(5, ^{ PostKey(window, kVK_ANSI_X, NO); });
    add(150, ^{ [renderer captureNextFrameToURL:file(@"after_a")]; });

    // Filters on a grey test card (pattern_<filter>_<scale>.png) at a whole-number
    // scale, a fractional one, and full screen, to check neutral stays neutral.
    void (^patterns)(NSString*) = ^(NSString* scale) {
        for (NSUInteger mode = 0; mode < FilterModeCount; mode++) {
            add(3, ^{ renderer.filter = mode; });
            add(3, ^{
                [renderer captureNextFrameToURL:file([NSString stringWithFormat:@"pattern_%lu_%@", (unsigned long)mode, scale])];
            });
        }
    };
    add(5, ^{
        renderer.testPattern = YES;
        [window setContentSize:NSMakeSize(PPU_WIDTH * 3, PPU_HEIGHT * 3)];
    });
    patterns(@"integer");
    // Bloom comparisons at the same scale: CRT with fixed-width beams, and the
    // LCD grid with gaps that shrink on bright pixels.
    void (^withParams)(FilterMode, NSString*, void (^)(FilterParams*)) =
        ^(FilterMode mode, NSString* name, void (^change)(FilterParams*)) {
            add(3, ^{
                FilterParams params = FilterParamsDefaults();
                change(&params);
                renderer.params = params;
                renderer.filter = mode;
            });
            add(3, ^{ [renderer captureNextFrameToURL:file(name)]; });
            add(3, ^{ renderer.params = FilterParamsDefaults(); });
        };
    withParams(FilterModeCRT, @"pattern_4_integer_nobloom", ^(FilterParams* p) { p->crtBloom = 0; });
    withParams(FilterModeLCD, @"pattern_3_integer_gapbloom", ^(FilterParams* p) { p->lcdGapBloom = 1; });
    add(5, ^{
        renderer.integerScaling = NO;
        [window setContentSize:NSMakeSize(PPU_WIDTH * 4.37, PPU_HEIGHT * 4.37)];
    });
    patterns(@"fractional");
    add(5, ^{ [window toggleFullScreen:nil]; });
    add(120, ^{ });
    patterns(@"fullscreen");
    add(3, ^{
        FilterParams params = FilterParamsDefaults();
        params.colorCorrection = 1;
        renderer.params = params;
        renderer.filter = FilterModeLCD;
    });
    add(3, ^{ [renderer captureNextFrameToURL:file(@"pattern_3_fullscreen_corrected")]; });
    add(3, ^{
        renderer.params = FilterParamsDefaults();
        renderer.testPattern = NO;
        renderer.filter = FilterModeLCD;
    });
    add(5, ^{ [renderer captureNextFrameToURL:file(@"fullscreen_lcd_game")]; });
    add(5, ^{ [window toggleFullScreen:nil]; });

    // The settings window, drawn offscreen.
    __block SettingsWindowController* settings = nil;
    add(90, ^{
        settings = [[SettingsWindowController alloc] initWithRenderer:renderer];
        [settings showWindow:nil];
    });
    add(10, ^{
        NSView* content = settings.window.contentView;
        NSBitmapImageRep* rep = [content bitmapImageRepForCachingDisplayInRect:content.bounds];
        NSGraphicsContext* context = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
        [NSGraphicsContext saveGraphicsState];
        NSGraphicsContext.currentContext = context;
        [settings.window.effectiveAppearance performAsCurrentDrawingAppearance:^{
            [NSColor.windowBackgroundColor setFill];
            NSRectFill(content.bounds);
        }];
        [NSGraphicsContext restoreGraphicsState];
        [content displayRectIgnoringOpacity:content.bounds inContext:context];
        [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToURL:file(@"settings") atomically:YES];
    });
    add(5, ^{ [NSApp terminate:nil]; });

    __block NSUInteger next = 0;
    [NSTimer scheduledTimerWithTimeInterval:1.0 / 240
                                    repeats:YES
                                      block:^(NSTimer* timer) {
                                          while (next < steps.count && gPlatformFrame >= [steps[next][0] unsignedLongLongValue]) {
                                              ((void (^)(void))steps[next][1])();
                                              NSLog(@"selftest: step %lu at frame %llu", (unsigned long)next,
                                                    (unsigned long long)gPlatformFrame);
                                              next++;
                                          }
                                      }];
}
