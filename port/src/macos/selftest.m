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

// The newest readback of a buffer, or nil.
static DebugReadback* Readback(Renderer* renderer, DebugBuffer buffer) {
    DebugReadback* rb = renderer.latestReadback;
    return rb != nil && rb.buffer == buffer ? rb : nil;
}

static BOOL Near(simd_float4 a, simd_float4 b, float tolerance) {
    simd_float4 d = simd_abs(a - b);
    return d.x <= tolerance && d.y <= tolerance && d.z <= tolerance;
}

void SelfTestStart(NSWindow* window, MTKView* view, Renderer* renderer, Input* input, DebugWindows* debug,
                   NSString* dir) {
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
    // Added to every step's frame once a state from elsewhere is loaded (the
    // frame counter jumps to the state's), so later steps keep their spacing.
    __block int64_t shift = 0;
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

    // Shader buffers: copy every pipeline texture back (as the Shader Buffers
    // window does) on the test card at 3x and log its numbers. With nearest
    // filtering at a whole-number scale, the filter's raw output and the final
    // drawable have the same mean colour as the uploaded frame, and the LCD
    // response output (settled on a still picture) matches it too.
    __block simd_float4 frameMean = 0;
    __block int shaderFailures = 0;
    void (^logBuffer)(FilterMode, DebugBuffer) = ^(FilterMode mode, DebugBuffer buffer) {
        add(2, ^{ [debug selectShaderBuffer:buffer]; });
        add(10, ^{
            DebugReadback* rb = Readback(renderer, buffer);
            NSLog(@"selftest: %@ filter: %@", FilterModeName(mode), [debug summaryForPanel:DebugPanelShaderBuffers]);
            if (rb == nil) {
                shaderFailures++;
                return;
            }
            simd_float4 mean = rb.mean;
            if (buffer == DebugBufferFrame) {
                frameMean = mean;
            } else if (mode == FilterModeNearest &&
                       (buffer == DebugBufferFilterRaw || buffer == DebugBufferDrawable ||
                        buffer == DebugBufferResponseOutput || buffer == DebugBufferPreGamma)) {
                BOOL ok = Near(mean, frameMean, 0.004f);
                NSLog(@"selftest: %@ mean %.4f %.4f %.4f vs frame %.4f %.4f %.4f %@", DebugBufferName(buffer), mean.x,
                      mean.y, mean.z, frameMean.x, frameMean.y, frameMean.z, ok ? @"ok" : @"FAILED");
                shaderFailures += !ok;
            }
        });
    };
    add(3, ^{
        renderer.params = FilterParamsDefaults();
        renderer.filter = FilterModeNearest;
        renderer.lcdResponse = YES;
        [debug showPanel:DebugPanelShaderBuffers];
    });
    for (NSInteger buffer = 0; buffer < DebugBufferCount; buffer++) {
        logBuffer(FilterModeNearest, buffer);
    }
    add(3, ^{ SelfTestSnapshotWindow([debug windowForPanel:DebugPanelShaderBuffers], @"debug_shader_buffers"); });
    for (FilterMode mode = FilterModeLCD; mode <= FilterModeCRT; mode++) {
        add(3, ^{ renderer.filter = mode; });
        for (NSInteger buffer = DebugBufferFilterRaw; buffer < DebugBufferCount; buffer++) {
            logBuffer(mode, buffer);
        }
    }
    add(3, ^{
        [[debug windowForPanel:DebugPanelShaderBuffers] close];
        renderer.lcdResponse = NO;
        renderer.filter = FilterModeNearest;
        NSLog(@"selftest: shader buffers %@ (readback %@ after closing)", shaderFailures ? @"FAILED" : @"ok",
              renderer.debugBuffer == DebugBufferNone ? @"off" : @"STILL ON");
    });
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

    // Graphics debug windows after loading the hyrule_town checkpoint: log what each
    // shows, draw them offscreen, outline sprite 0 in the game window.
    NSString* town = @TMC_CHECKPOINT_DIR "/hyrule_town." STATE_EXTENSION;
    if ([NSFileManager.defaultManager fileExistsAtPath:town]) {
        add(10, ^{ FrontendRequestState(false, town.fileSystemRepresentation, SelfTestStateDone, "load town"); });
        __block uint64_t loadedAt = at + 5;
        add(5, ^{
            shift = (int64_t)gPlatformFrame - (int64_t)loadedAt; // runs right after the load
            for (NSInteger kind = 0; kind < DebugPanelShaderBuffers; kind++) {
                [debug showPanel:kind];
            }
        });
        add(90, ^{
            [debug selectSprite:0 highlight:YES];
            for (NSInteger kind = 0; kind < DebugPanelShaderBuffers; kind++) {
                NSLog(@"selftest: %@", [debug summaryForPanel:kind]);
            }
        });
        add(2, ^{ [renderer captureNextFrameToURL:file(@"debug_sprite_highlight")]; });
        static NSString* const names[] = { @"tiles", @"sprites", @"layers", @"palettes", @"io" };
        for (NSInteger kind = 0; kind < DebugPanelShaderBuffers; kind++) {
            add(1, ^{ SelfTestSnapshotWindow([debug windowForPanel:kind], [@"debug_" stringByAppendingString:names[kind]]); });
        }
        for (NSInteger bg = 0; bg < 4; bg++) {
            add(2, ^{
                [debug selectLayer:bg];
                NSLog(@"selftest: %@", [debug summaryForPanel:DebugPanelLayers]);
            });
        }
        add(30, ^{
            NSLog(@"selftest: %@ (later)", [debug summaryForPanel:DebugPanelSprites]);
            [debug closeAll];
            NSLog(@"selftest: debug windows closed, highlight %@", CGRectIsEmpty(renderer.highlightRect) ? @"off" : @"STILL ON");
        });
    }

    // The settings window, drawn offscreen.
    __block SettingsWindowController* settings = nil;
    add(90, ^{
        settings = [[SettingsWindowController alloc] initWithRenderer:renderer input:input pacingStatus:nil];
        [settings showWindow:nil];
    });
    add(10, ^{ SelfTestSnapshotWindow(settings.window, @"settings"); });
    add(5, ^{ [NSApp terminate:nil]; });

    __block NSUInteger next = 0;
    [NSTimer scheduledTimerWithTimeInterval:1.0 / 240
                                    repeats:YES
                                      block:^(NSTimer* timer) {
                                          while (next < steps.count &&
                                                 (int64_t)gPlatformFrame >= [steps[next][0] longLongValue] + shift) {
                                              ((void (^)(void))steps[next][1])();
                                              NSLog(@"selftest: step %lu at frame %llu", (unsigned long)next,
                                                    (unsigned long long)gPlatformFrame);
                                              next++;
                                          }
                                      }];
}
