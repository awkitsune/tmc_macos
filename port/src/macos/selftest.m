#import "selftest.h"

#import <Carbon/Carbon.h>

#import "settings.h"

#include "frontend.h"
#include "port/platform.h"
#include "port/ppu.h"
#include "port/state.h"

// On the game thread, at the boundary the state was saved or loaded at.
static void SelfTestStateDone(bool ok, const char* message, uint64_t frame, void* context) {
    NSLog(@"selftest: state %s %s at frame %llu, checksum %016llx %s", (const char*)context, ok ? "ok" : "FAILED",
          (unsigned long long)frame, (unsigned long long)StateChecksum(), message);
}

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

// Press each face button on a virtual pad and check the GBA key it gives, for
// both layouts and for Nintendo (label-reported) and other pads.
static void CheckGamepadMapping(Input* input) {
    GCController* controller = [GCController controllerWithExtendedGamepad];
    GCExtendedGamepad* pad = controller.extendedGamepad;
    GCControllerButtonInput* buttons[4] = { pad.buttonA, pad.buttonB, pad.buttonX, pad.buttonY };
    const char* names[4] = { "A", "B", "X", "Y" };
    // Expected GBA key per button: [nintendo][layout][button] (1 = A, 2 = B).
    static const uint16_t expected[2][2][4] = {
        { { 1, 2, 1, 2 }, { 2, 1, 2, 1 } }, // other pads: by label, by position
        { { 1, 2, 1, 2 }, { 1, 2, 1, 2 } }, // Nintendo pads: the same either way
    };
    GamepadLayout saved = input.gamepadLayout;
    int failures = 0;
    for (int nintendo = 0; nintendo < 2; nintendo++) {
        for (int layout = 0; layout < 2; layout++) {
            input.gamepadLayout = layout;
            for (int b = 0; b < 4; b++) {
                [buttons[b] setValue:1];
                uint16_t keys = [input keysForController:controller nintendo:nintendo];
                [buttons[b] setValue:0];
                if (keys != expected[nintendo][layout][b]) {
                    NSLog(@"selftest: FAIL %s pad, layout %d, button %s gave keys %#x", nintendo ? "Nintendo" : "other",
                          layout, names[b], keys);
                    failures++;
                }
            }
        }
    }
    input.gamepadLayout = saved;
    NSLog(@"selftest: gamepad mapping %s", failures ? "FAILED" : "ok");
}

void SelfTestSnapshotWindow(NSWindow* window, NSString* name) {
    NSString* dir = NSProcessInfo.processInfo.environment[@"TMC_SELFTEST"];
    if (dir.length == 0) {
        return;
    }
    [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    NSView* content = window.contentView;
    NSBitmapImageRep* rep = [content bitmapImageRepForCachingDisplayInRect:content.bounds];
    NSGraphicsContext* context = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
    [NSGraphicsContext saveGraphicsState];
    NSGraphicsContext.currentContext = context;
    [window.effectiveAppearance performAsCurrentDrawingAppearance:^{
        [NSColor.windowBackgroundColor setFill];
        NSRectFill(content.bounds);
    }];
    [NSGraphicsContext restoreGraphicsState];
    [content displayRectIgnoringOpacity:content.bounds inContext:context];
    NSURL* url = [[NSURL fileURLWithPath:dir] URLByAppendingPathComponent:[name stringByAppendingPathExtension:@"png"]];
    [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToURL:url atomically:YES];
}

void SelfTestStart(NSWindow* window, MTKView* view, Renderer* renderer, Input* input, NSString* dir) {
    CheckGamepadMapping(input);
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
    renderer.lcdResponse = NO;
    [renderer checkLcdResponse];

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
    // The game with LCD response on (restarts from the current frame, then ghosts).
    add(5, ^{ renderer.lcdResponse = YES; });
    add(30, ^{ [renderer captureNextFrameToURL:file(@"after_a_response")]; });
    add(5, ^{ renderer.lcdResponse = NO; });

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

    // Save states: save, play on, load it back. The log shows the same frame
    // and checksum for both.
    NSString* statePath = [dir stringByAppendingPathComponent:@"selftest." STATE_EXTENSION];
    add(5, ^{ FrontendRequestState(true, statePath.fileSystemRepresentation, SelfTestStateDone, "save"); });
    add(60, ^{ FrontendRequestState(false, statePath.fileSystemRepresentation, SelfTestStateDone, "load"); });

    // The settings window, drawn offscreen.
    __block SettingsWindowController* settings = nil;
    add(90, ^{
        settings = [[SettingsWindowController alloc] initWithRenderer:renderer input:input];
        [settings showWindow:nil];
    });
    add(10, ^{ SelfTestSnapshotWindow(settings.window, @"settings"); });
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
