#import "selftest.h"

#import <Carbon/Carbon.h>

#import "settings.h"

#include "frontend.h"
#include "port/platform.h"
#include "port/ppu.h"
#include "port/state.h"
#include "roomname.h"

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

// The toolbar's items, without spaces.
static NSArray<NSString*>* ToolbarItems(NSWindow* window) {
    NSMutableArray* ids = [NSMutableArray array];
    for (NSToolbarItem* item in window.toolbar.items) {
        if (![item.itemIdentifier isEqualToString:NSToolbarFlexibleSpaceItemIdentifier] &&
            ![item.itemIdentifier isEqualToString:NSToolbarSpaceItemIdentifier]) {
            [ids addObject:item.itemIdentifier];
        }
    }
    return ids;
}

static NSToolbarItem* ToolbarItem(NSWindow* window, NSString* identifier) {
    for (NSToolbarItem* item in window.toolbar.items) {
        if ([item.itemIdentifier isEqualToString:identifier]) {
            return item;
        }
    }
    return nil;
}

// Items [from, to] of a menu (to -1: the last) as text: title, action, tag,
// shortcut and checkmark of each, submenus in brackets, after validating them.
static NSString* MenuText(NSMenu* menu, NSInteger from, NSInteger to) {
    [menu update];
    NSMutableArray* items = [NSMutableArray array];
    if (to < 0) {
        to = menu.numberOfItems - 1;
    }
    for (NSInteger i = from; i <= to && i < menu.numberOfItems; i++) {
        NSMenuItem* item = [menu itemAtIndex:i];
        if (item.separatorItem) {
            [items addObject:@"-"];
            continue;
        }
        NSString* text = [NSString stringWithFormat:@"%@ %@ %ld %@/%lx%@", item.title,
                                                    item.action ? NSStringFromSelector(item.action) : @"-", (long)item.tag,
                                                    item.keyEquivalent, (unsigned long)item.keyEquivalentModifierMask,
                                                    item.state == NSControlStateValueOn ? @" ✓" : @""];
        if (item.submenu != nil) {
            text = [text stringByAppendingFormat:@" [%@]", MenuText(item.submenu, 0, -1)];
        }
        [items addObject:text];
    }
    return [items componentsJoinedByString:@"; "];
}

static NSMenu* BarMenu(NSString* title) {
    return [NSApp.mainMenu itemWithTitle:title].submenu;
}

// The dropdown's items (after its icon item) against the menu bar's.
static int CheckDropdown(NSWindow* window, NSString* identifier, NSMenu* bar, NSString* first, NSString* last) {
    NSPopUpButton* popup = (NSPopUpButton*)ToolbarItem(window, identifier).view;
    if (![popup isKindOfClass:NSPopUpButton.class] || bar == nil) {
        NSLog(@"selftest: FAIL toolbar %@: no dropdown or no menu bar menu", identifier);
        return 1;
    }
    NSInteger from = first ? [bar indexOfItemWithTitle:first] : 0;
    NSInteger to = last ? [bar indexOfItemWithTitle:last] : -1;
    NSString* toolbarText = MenuText(popup.menu, 1, -1);
    NSString* barText = MenuText(bar, from, to);
    BOOL same = [toolbarText isEqualToString:barText] && toolbarText.length != 0;
    NSLog(@"selftest: toolbar %@ menu %@: %@", identifier, same ? @"matches the menu bar" : @"FAILED", toolbarText);
    if (!same) {
        NSLog(@"selftest: menu bar has: %@", barText);
    }
    return !same;
}

// Toolbar: its items (Debug only with its setting on), that each dropdown has
// the menu bar's items with the same checkmarks, that no button takes the
// keyboard, and that the toolbar adds to the window rather than covering the game.
static void CheckToolbar(NSWindow* window, MTKView* view, Renderer* renderer) {
    int failures = 0;
    NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
    NSArray* withDebug = @[ @"SaveLoad", @"Filter", @"Debug", @"Settings" ];
    NSArray* withoutDebug = @[ @"SaveLoad", @"Filter", @"Settings" ];
    BOOL debugSetting = [defaults boolForKey:kDebugToolbarDefault];
    __block int itemFailures = 0;
    void (^setDebug)(BOOL) = ^(BOOL on) {
        [defaults setBool:on forKey:kDebugToolbarDefault];
        [NSNotificationCenter.defaultCenter postNotificationName:kDebugToolbarChangedNotification object:nil];
        NSArray* items = ToolbarItems(window);
        BOOL ok = [items isEqualToArray:on ? withDebug : withoutDebug] &&
                  [window.toolbar.items.lastObject.itemIdentifier isEqualToString:@"Settings"] &&
                  [window.toolbar.items.firstObject.itemIdentifier isEqualToString:NSToolbarFlexibleSpaceItemIdentifier];
        NSLog(@"selftest: toolbar with Debug setting %@: %@ %@", on ? @"on" : @"off", [items componentsJoinedByString:@", "],
              ok ? @"ok" : @"FAILED");
        itemFailures += !ok;
    };
    setDebug(debugSetting);
    setDebug(!debugSetting);
    setDebug(YES);

    failures += CheckDropdown(window, @"SaveLoad", BarMenu(@"Game"), @"Quick Save", @"Load State…");
    failures += CheckDropdown(window, @"Filter", BarMenu(@"View"), nil, @"Integer Scaling");
    if (BarMenu(@"Debug") != nil) {
        failures += CheckDropdown(window, @"Debug", BarMenu(@"Debug"), nil, nil);
    } else {
        NSLog(@"selftest: no Debug menu (its setting is off): toolbar Debug menu not compared");
    }
    NSButton* settings = (NSButton*)ToolbarItem(window, @"Settings").view;
    NSMenuItem* settingsMenu = [BarMenu(@"The Minish Cap") itemWithTitle:@"Settings…"];
    BOOL settingsOk = settings.action == settingsMenu.action && settings.action == @selector(showSettings:);
    NSLog(@"selftest: toolbar Settings button %@", settingsOk ? @"ok" : @"FAILED");
    failures += !settingsOk;

    // Choosing a filter from the toolbar checks it in the View menu, and back.
    NSInteger savedFilter = [defaults integerForKey:kFilterDefault];
    FilterMode before = renderer.filter;
    NSMenu* filterMenu = ((NSPopUpButton*)ToolbarItem(window, @"Filter").view).menu;
    FilterMode other = before == FilterModeCRT ? FilterModeLCD : FilterModeCRT;
    [filterMenu performActionForItemAtIndex:[filterMenu indexOfItemWithTag:other]];
    [BarMenu(@"View") update];
    BOOL synced = renderer.filter == other &&
                  [BarMenu(@"View") itemWithTag:other].state == NSControlStateValueOn &&
                  [BarMenu(@"View") itemWithTag:before].state == NSControlStateValueOff;
    NSLog(@"selftest: toolbar filter choice %@ in the View menu", synced ? @"checked" : @"NOT checked");
    failures += !synced;
    failures += CheckDropdown(window, @"Filter", BarMenu(@"View"), nil, @"Integer Scaling");
    renderer.filter = before;
    [defaults setInteger:savedFilter forKey:kFilterDefault];

    // No toolbar control can take the keyboard from the game: clicks and Tab
    // only focus a control that accepts it.
    for (NSToolbarItem* item in window.toolbar.items) {
        NSControl* control = (NSControl*)item.view;
        if (control == nil) {
            continue;
        }
        BOOL ok = control.refusesFirstResponder && !control.acceptsFirstResponder && !control.canBecomeKeyView;
        NSLog(@"selftest: toolbar %@ %@ the keyboard%@", item.itemIdentifier, ok ? @"refuses" : @"TAKES",
              item.toolTip.length && item.label.length ? @"" : @" (NO LABEL OR TOOLTIP)");
        failures += !ok || !item.toolTip.length || !item.label.length;
    }
    NSLog(@"selftest: game view %@ the keyboard", window.firstResponder == view ? @"has" : @"DOES NOT HAVE");
    failures += window.firstResponder != view;
    setDebug(debugSetting);
    failures += itemFailures;

    // The toolbar sits above the game, which keeps the whole content view.
    NSRect content = [window contentRectForFrameRect:window.frame];
    CGFloat bar = NSHeight(window.frame) - NSHeight(content);
    BOOL clear = NSEqualRects(window.contentLayoutRect, window.contentView.bounds) && bar > 40;
    NSLog(@"selftest: window content %.0fx%.0f, frame %.0fx%.0f (title bar and toolbar %.0f) %@", NSWidth(content),
          NSHeight(content), NSWidth(window.frame), NSHeight(window.frame), bar, clear ? @"ok" : @"FAILED");
    failures += !clear;
    NSLog(@"selftest: toolbar %@", failures ? @"FAILED" : @"ok");
}

// The subtitle: the room's name, then the parts after it (a status, the frame counter).
static void CheckSubtitle(NSWindow* window, NSString* room, NSString* status) {
    int area = -1, index = -1;
    BOOL inRoom = RoomCurrent(&area, &index);
    NSString* expected = status ? [NSString stringWithFormat:@"%@ · %@", room, status] : room;
    BOOL counter = [NSUserDefaults.standardUserDefaults boolForKey:@"ShowFrameCounter"];
    NSString* subtitle = window.subtitle;
    BOOL ok = counter ? [subtitle hasPrefix:[expected stringByAppendingString:@" · Frame "]] : [subtitle isEqualToString:expected];
    NSLog(@"selftest: subtitle \"%@\" in area %d room %d (%s) %@", subtitle, area, index, inRoom ? "game" : "not in game",
          ok ? @"ok" : [NSString stringWithFormat:@"FAILED, expected \"%@\"%@", expected, counter ? @" · Frame …" : @""]);
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
    NSLog(@"selftest: subtitle on the title screen \"%@\"", window.subtitle);
    // Added to every step's frame once a state from elsewhere is loaded (the
    // frame counter jumps to the state's), so later steps keep their spacing.
    __block int64_t shift = 0;
    __block uint64_t at = 360; // title screen is up by now
    void (^add)(uint64_t, void (^)(void)) = ^(uint64_t delay, void (^action)(void)) {
        at += delay;
        [steps addObject:@[ @(at), action ]];
    };
    add(0, ^{ CheckToolbar(window, view, renderer); });
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
    add(120, ^{
        // The toolbar hides with the menu bar: the game has the whole screen.
        NSSize content = window.contentView.bounds.size, whole = window.frame.size;
        BOOL ok = NSEqualSizes(content, whole) && NSEqualRects(window.contentLayoutRect, window.contentView.bounds);
        NSLog(@"selftest: full screen content %.0fx%.0f in a %.0fx%.0f window %@", content.width, content.height,
              whole.width, whole.height, ok ? @"ok" : @"FAILED");
    });
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
        // The subtitle names the room, then a status and the frame counter.
        add(60, ^{ CheckSubtitle(window, @"Festival Town", nil); });
        __block NSString* slotStatus = nil;
        add(2, ^{
            NSMenuItem* slot = [NSMenuItem new]; // the current slot again: shows its status, changes nothing
            slot.tag = MAX(1, [NSUserDefaults.standardUserDefaults integerForKey:@"StateSlot"]);
            slotStatus = [NSString stringWithFormat:@"Slot %ld", (long)slot.tag];
            [NSApp sendAction:NSSelectorFromString(@"selectSlot:") to:nil from:slot];
            CheckSubtitle(window, @"Festival Town", slotStatus);
        });
        add(2, ^{
            [NSApp sendAction:NSSelectorFromString(@"toggleFrameCounter:") to:nil from:nil];
            CheckSubtitle(window, @"Festival Town", slotStatus); // with the counter (or without, if it was on)
            [NSApp sendAction:NSSelectorFromString(@"toggleFrameCounter:") to:nil from:nil];
            CheckSubtitle(window, @"Festival Town", slotStatus);
        });
        add(26, ^{ // 90 frames after the load, as before
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
    if ([NSFileManager.defaultManager fileExistsAtPath:town]) {
        add(90, ^{ CheckSubtitle(window, @"Festival Town", nil); }); // the status is over
    }
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
