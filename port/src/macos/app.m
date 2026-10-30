/*
 * macOS front end: the window, menus and key handling around the Metal view.
 * The game itself runs on its own thread (game.c).
 */

#import <AppKit/AppKit.h>
#import <MetalKit/MetalKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#import "debugwindows.h"
#import "displaysync.h"
#import "input.h"
#import "renderer.h"
#import "romsetup.h"
#import "selftest.h"
#import "settings.h"

#include <stdatomic.h>

#include "audio.h"
#include "frontend.h"
#include "latency.h"
#include "port/hw.h"
#include "port/romdata.h"
#include "port/state.h"
#include "roomname.h"

static NSString* const kMutedDefault = @"Muted";
static NSString* const kHighQualityAudioDefault = @"HighQualityAudio";
static NSString* const kStateSlotDefault = @"StateSlot";
static NSString* const kFrameCounterDefault = @"ShowFrameCounter";
#define STATE_SLOTS 4
// Before the toolbar the frame was saved as "GameWindow", with a thin title bar.
static NSString* const kWindowAutosave = @"GameWindowToolbar";
static NSString* const kOldWindowAutosave = @"GameWindow";

static NSToolbarItemIdentifier const kSaveLoadItem = @"SaveLoad";
static NSToolbarItemIdentifier const kFilterItem = @"Filter";
static NSToolbarItemIdentifier const kDebugItem = @"Debug";
static NSToolbarItemIdentifier const kSettingsItem = @"Settings";

// The game view takes the keyboard: game keys go to Input, everything else
// (menu shortcuts, Escape) goes up the responder chain.
@interface GameView : MTKView
@property(nonatomic, strong) Input* input;
@end

@implementation GameView

- (BOOL)acceptsFirstResponder {
    return YES;
}

- (void)keyDown:(NSEvent*)event {
    if (event.modifierFlags & NSEventModifierFlagCommand) {
        [super keyDown:event];
        return;
    }
    if (event.keyCode == 53 /* Escape */ && (self.window.styleMask & NSWindowStyleMaskFullScreen)) {
        [self.window toggleFullScreen:nil];
        return;
    }
    if (![self.input handleKeyEvent:event down:YES]) {
        [super keyDown:event];
    }
}

- (void)keyUp:(NSEvent*)event {
    if (![self.input handleKeyEvent:event down:NO]) {
        [super keyUp:event];
    }
}

@end

// A toolbar dropdown. It never takes the keyboard (Space and Return stay the
// game's), and leaves its items' shortcuts to the menu bar, which has them too.
@interface ToolbarPopUp : NSPopUpButton
@end

@implementation ToolbarPopUp

- (BOOL)performKeyEquivalent:(NSEvent*)event {
    return NO;
}

@end

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate, NSMenuDelegate, NSToolbarDelegate>
- (void)stateFinished:(BOOL)ok what:(NSString*)what message:(NSString*)message frame:(uint64_t)frame;
- (void)drawPublishedFrame;
@end

// The game thread's input poll, right before the game reads its keys.
static uint16_t PollInput(void* context) {
    Input* input = (__bridge Input*)context;
    FrontendSetFastForward(input.fastForwardHeld);
    return [input pollKeys];
}

// Called on the game thread after a frame paced by the display: draw it now.
static void FramePublished(void* context) {
    [(__bridge AppDelegate*)context drawPublishedFrame];
}

// FrontendRequestState's callback, on the game thread: report on the main one.
static void StateDone(bool ok, const char* message, uint64_t frame, void* context) {
    NSString* what = (__bridge_transfer NSString*)context;
    NSString* text = @(message);
    dispatch_async(dispatch_get_main_queue(), ^{
        [(AppDelegate*)NSApp.delegate stateFinished:ok what:what message:text frame:frame];
    });
}

@implementation AppDelegate {
    NSWindow* _window;
    GameView* _view;
    Renderer* _renderer;
    SettingsWindowController* _settings;
    DebugWindows* _debugWindows;
    Input* _input;
    RomSetup* _romSetup;
    NSString* _status;     // shown in the window's subtitle until _statusUntil
    NSDate* _statusUntil;
    BOOL _statusShown;
    int _roomArea, _roomIndex; // the room the subtitle names (-1: none)
    NSString* _roomName;
    BOOL _subtitleDirty;
    NSUInteger _drawCount;
    DisplaySync* _displaySync;
    atomic_bool _drawQueued;
    CFTimeInterval _lastDraw;
}

#pragma mark Launch

// ~/Library/Application Support/tmc_macos/tmc.sav (8 KB EEPROM image, the same
// layout as an emulator's .sav for this game).
- (NSString*)savePath {
    NSURL* support = [NSFileManager.defaultManager URLsForDirectory:NSApplicationSupportDirectory
                                                          inDomains:NSUserDomainMask].firstObject;
    NSURL* dir = [support URLByAppendingPathComponent:@"tmc_macos" isDirectory:YES];
    NSError* error = nil;
    if (![NSFileManager.defaultManager createDirectoryAtURL:dir withIntermediateDirectories:YES attributes:nil error:&error]) {
        NSLog(@"cannot create %@: %@", dir.path, error);
    }
    return [dir URLByAppendingPathComponent:@"tmc.sav"].path;
}

// ~/Library/Application Support/tmc_macos/states: quick save slots and the
// default place for states saved to a file.
- (NSString*)stateFolder {
    NSString* dir = [self.savePath.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"states"];
    [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    return dir;
}

- (NSInteger)stateSlot {
    return MAX(1, MIN(STATE_SLOTS, [NSUserDefaults.standardUserDefaults integerForKey:kStateSlotDefault]));
}

- (NSString*)slotPath {
    return [self.stateFolder stringByAppendingPathComponent:[NSString stringWithFormat:@"slot%ld.%s", (long)self.stateSlot,
                                                                                       STATE_EXTENSION]];
}

- (void)applicationDidFinishLaunching:(NSNotification*)note {
    NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
    [defaults registerDefaults:@{
        kFilterDefault : @(FilterModeSharpBilinear),
        kIntegerScalingDefault : @YES,
        kMutedDefault : @NO,
        kHighQualityAudioDefault : @NO,
        kStateSlotDefault : @1,
        kFrameCounterDefault : @NO,
        kPacingDefault : @(FRONTEND_PACING_DISPLAY_SYNC),
        kDebugMenuDefault : @(TMC_DEBUG_MENU_DEFAULT),
        kDebugToolbarDefault : @NO,
    }];
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(applyPacing)
                                               name:kPacingChangedNotification
                                             object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(debugMenuChanged:)
                                               name:kDebugMenuChangedNotification
                                             object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(syncDebugToolbarItem)
                                               name:kDebugToolbarChangedNotification
                                             object:nil];

    [self buildMenus];

    // Builds without the game data built in get it from the player's ROM first.
    _romSetup = [RomSetup new];
    [_romSetup prepareThen:^{
        [self launchGame];
    }];
}

- (void)launchGame {
    NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
    const CGFloat scale = 3;
    NSRect frame = NSMakeRect(0, 0, PPU_WIDTH * scale, PPU_HEIGHT * scale);
    _window = [[NSWindow alloc] initWithContentRect:frame
                                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                    NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    _window.title = @"The Minish Cap";
    _window.contentMinSize = NSMakeSize(PPU_WIDTH, PPU_HEIGHT);
    _window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
    _window.backgroundColor = NSColor.blackColor;
    _window.delegate = self;
    // The toolbar adds to the window's height: the game keeps its size.
    _window.toolbarStyle = NSWindowToolbarStyleUnified;
    _window.toolbar = [self makeToolbar];
    [self syncDebugToolbarItem];
    if (![_window setFrameUsingName:kWindowAutosave]) {
        // A frame saved before the toolbar ("x y w h ..." with a thin title
        // bar) gives the game's size and the window's top edge; AppKit would
        // add the toolbar to it and grow the game.
        NSArray<NSString*>* old = [[NSUserDefaults.standardUserDefaults
            stringForKey:[@"NSWindow Frame " stringByAppendingString:kOldWindowAutosave]] componentsSeparatedByString:@" "];
        if (old.count >= 4) {
            NSRect saved = NSMakeRect(old[0].doubleValue, old[1].doubleValue, old[2].doubleValue, old[3].doubleValue);
            NSRect content = [NSWindow contentRectForFrameRect:saved styleMask:_window.styleMask];
            [_window setContentSize:content.size];
            [_window setFrameTopLeftPoint:NSMakePoint(NSMinX(saved), NSMaxY(saved))];
        } else {
            [_window setContentSize:frame.size];
            [_window center];
        }
    }
    _window.frameAutosaveName = kWindowAutosave;
    _roomArea = _roomIndex = -1;
    _subtitleDirty = YES;

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil) {
        [self fail:@"This Mac has no Metal device."];
        return;
    }
    _input = [Input new];
    _input.gamepadLayout = [defaults integerForKey:kGamepadLayoutDefault] == GamepadLayoutByPosition
                               ? GamepadLayoutByPosition
                               : GamepadLayoutByLabel;
    _view = [[GameView alloc] initWithFrame:frame device:device];
    _view.input = _input;
    _view.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
    _view.clearColor = MTLClearColorMake(0, 0, 0, 1);
    _view.preferredFramesPerSecond = 120; // capped at the display's rate
    _renderer = [[Renderer alloc] initWithView:_view];
    if (_renderer == nil) {
        [self fail:@"The Metal renderer could not be created (see Console for details)."];
        return;
    }
    _renderer.filter = MIN((NSUInteger)[defaults integerForKey:kFilterDefault], FilterModeCount - 1);
    _renderer.integerScaling = [defaults boolForKey:kIntegerScalingDefault];
    _renderer.params = LoadFilterParams();
    _renderer.lcdResponse = [defaults boolForKey:kLcdResponseDefault];
    __weak AppDelegate* weakSelf = self;
    _renderer.gpuPpuFailed = ^(NSString* message) {
        AppDelegate* strongSelf = weakSelf;
        if (!strongSelf) return;
        strongSelf->_subtitleDirty = YES;
        [strongSelf updateSubtitle];
        NSAlert* alert = [NSAlert new];
        alert.messageText = @"GPU PPU stopped";
        alert.informativeText = [NSString stringWithFormat:@"The software renderer was restored. %@", message];
        [alert beginSheetModalForWindow:strongSelf->_window completionHandler:nil];
    };
    _renderer.beforeDraw = ^{
        AppDelegate* strongSelf = weakSelf;
        [strongSelf updateSubtitle];
        strongSelf->_lastDraw = CACurrentMediaTime();
    };
    FrontendSetInputPoll(PollInput, (__bridge void*)_input);
    FrontendSetFrameHandler(FramePublished, (__bridge void*)self);
    _debugWindows = [[DebugWindows alloc] initWithRenderer:_renderer];
    _view.delegate = _renderer;
    if (getenv("TMC_GPU_PPU_SELFTEST")) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            AppDelegate* strongSelf = weakSelf;
            if (!strongSelf) return;
            [strongSelf toggleGpuPpu:nil];
            NSLog(@"GPU PPU self-test: switch %@", strongSelf->_renderer.gpuPpuEnabled ? @"on" : @"FAILED");
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
                AppDelegate* strongSelf = weakSelf;
                if (!strongSelf) return;
                if (getenv("TMC_GPU_PPU_SELFTEST_PAUSE")) {
                    FrontendSetPaused(true);
                    NSLog(@"GPU PPU self-test: pause %@", FrontendWaitUntilPaused(1000) ? @"ok" : @"FAILED");
                }
                [strongSelf toggleGpuPpu:nil];
                NSLog(@"GPU PPU self-test: switch %@", strongSelf->_renderer.gpuPpuEnabled ? @"FAILED" : @"off");
            });
        });
    }
    _window.contentView = _view;
    [_window makeFirstResponder:_view];
    [_window makeKeyAndOrderFront:nil];

    [self startLatencyTools];
    _displaySync = [DisplaySync new];
    [_displaySync attachToWindow:_window];
    _displaySync.usableChanged = ^{
        [weakSelf updateDrawMode];
    };
    [self applyPacing];
    // With the view paused (display sync) nothing draws it unless a game frame
    // comes in: this covers resizing, pausing and a display that stalls.
    NSTimer* idle = [NSTimer timerWithTimeInterval:0.05
                                           repeats:YES
                                             block:^(NSTimer* timer) {
                                                 [weakSelf idleTick];
                                             }];
    [NSRunLoop.mainRunLoop addTimer:idle forMode:NSRunLoopCommonModes];

    FrontendSetSavePath(self.savePath.fileSystemRepresentation);
    AudioSetMuted([defaults boolForKey:kMutedDefault]);
    AudioSetHighQuality([defaults boolForKey:kHighQualityAudioDefault]);
    AudioStart(); // without an output device the game just runs silently
    FrontendStartGame();
    [NSApp activateIgnoringOtherApps:YES];

    // TMC_LOAD_STATE=path: start from a save state, as the headless runner does.
    NSString* loadState = NSProcessInfo.processInfo.environment[@"TMC_LOAD_STATE"];
    if (loadState.length != 0) {
        [self requestState:NO path:loadState what:[@"Loaded " stringByAppendingString:loadState.lastPathComponent]];
    }

    NSString* selfTest = NSProcessInfo.processInfo.environment[@"TMC_SELFTEST"];
    if (selfTest.length != 0) {
        SelfTestStart(_window, _view, _renderer, _input, _debugWindows, selfTest);
    }
}

#pragma mark Pacing

- (FrontendPacing)pacing {
    NSString* forced = NSProcessInfo.processInfo.environment[@"TMC_PACING"];
    if ([forced isEqualToString:@"accurate"]) {
        return FRONTEND_PACING_ACCURATE;
    }
    if ([forced isEqualToString:@"sync"]) {
        return FRONTEND_PACING_DISPLAY_SYNC;
    }
    return [NSUserDefaults.standardUserDefaults integerForKey:kPacingDefault] == FRONTEND_PACING_ACCURATE
               ? FRONTEND_PACING_ACCURATE
               : FRONTEND_PACING_DISPLAY_SYNC;
}

- (void)applyPacing {
    FrontendPacing pacing = self.pacing;

    FrontendSetPacing(pacing);
    _displaySync.enabled = pacing == FRONTEND_PACING_DISPLAY_SYNC;
    [self updateDrawMode];
    LatencyNote(pacing == FRONTEND_PACING_DISPLAY_SYNC ? "pacing: display sync" : "pacing: accurate");
}

// The view draws by itself, at the display's rate, except while the game
// follows the display: then it is drawn when a frame is ready (see below).
- (void)updateDrawMode {
    BOOL paused = _displaySync.enabled && DisplaySyncActive();
    if (_view.paused != paused) {
        _view.paused = paused;
    }
}

// Game thread: a frame that a display refresh is waiting for is ready.
- (void)drawPublishedFrame {
    if (!DisplaySyncActive() || atomic_exchange(&_drawQueued, true)) {
        return;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        atomic_store(&self->_drawQueued, false);
        if (self->_view.paused) {
            [self->_view draw];
        }
    });
}

- (void)idleTick {
    [self updateDrawMode];
    if (_view.paused && CACurrentMediaTime() - _lastDraw > 0.05) {
        [_view draw];
    }
}

- (NSString*)pacingStatus {
    if (self.pacing == FRONTEND_PACING_ACCURATE) {
        return @"The game keeps to the GBA's own 59.73 Hz.";
    }
    return _displaySync.statusText;
}

// Measurement helpers (see latency.h). TMC_LATENCY_SYNTH=1 presses and releases
// a key at random times by itself; TMC_QUIT_AFTER=seconds quits the app.
- (void)startLatencyTools {
    NSDictionary* env = NSProcessInfo.processInfo.environment;

    LatencyStart();
    if (!LatencyEnabled()) {
        return;
    }
    if ([env[@"TMC_LATENCY_SYNTH"] boolValue]) {
        [self scheduleSyntheticKey:YES];
    }
    // Unattended runs: keep the window in front, or nothing is shown and nothing measured.
    _window.level = NSStatusWindowLevel;
    _window.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorStationary;
    [NSTimer scheduledTimerWithTimeInterval:1 repeats:YES block:^(NSTimer* timer) {
        [NSApp activateIgnoringOtherApps:YES];
        [self->_window orderFrontRegardless];
    }];
    for (int seconds = 2; seconds <= 30; seconds += 14) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, seconds * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            NSString* text = [NSString stringWithFormat:@"at %d s: window %s, %s, active app %d, screen %@ %ld Hz", seconds,
                                                        self->_window.visible ? "visible" : "hidden",
                                                        (self->_window.occlusionState & NSWindowOcclusionStateVisible) ? "unoccluded" : "occluded",
                                                        NSApp.active, self->_window.screen.localizedName,
                                                        (long)self->_window.screen.maximumFramesPerSecond];
            LatencyNote(text.UTF8String);
        });
    }
    double quitAfter = [env[@"TMC_QUIT_AFTER"] doubleValue];
    if (quitAfter > 0) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(quitAfter * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            [NSApp terminate:nil];
        });
    }
}

- (void)scheduleSyntheticKey:(BOOL)press {
    double delay = 0.15 + 0.25 * (arc4random() / 4294967295.0);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        [self->_input setKeyboardKeysForTesting:press ? HW_KEY_L : 0];
        [self scheduleSyntheticKey:!press];
    });
}

- (void)fail:(NSString*)message {
    NSAlert* alert = [NSAlert new];
    alert.messageText = @"Cannot start";
    alert.informativeText = message;
    [alert runModal];
    [NSApp terminate:nil];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app {
    return YES;
}

- (void)applicationWillTerminate:(NSNotification*)note {
    FrontendStopGame();
    AudioStop();
    LatencyFinish();
}

- (void)applicationDidResignActive:(NSNotification*)note {
    [_input releaseAll]; // key-ups go elsewhere while another app has focus
}

#pragma mark Save states

- (void)requestState:(BOOL)save path:(NSString*)path what:(NSString*)what {
    void* context = (__bridge_retained void*)what;
    if (!FrontendRequestState(save, path.fileSystemRepresentation, StateDone, context)) {
        (void)(__bridge_transfer NSString*)context;
        [self showStatus:@"Busy with the last save or load"];
    }
}

- (void)stateFinished:(BOOL)ok what:(NSString*)what message:(NSString*)message frame:(uint64_t)frame {
    if (!ok) {
        NSAlert* alert = [NSAlert new];
        alert.messageText = @"Save state failed";
        alert.informativeText = message;
        [alert beginSheetModalForWindow:_window completionHandler:nil];
        return;
    }
    if (message.length != 0) {
        NSLog(@"%@: %@", what, message); // loaded from another build
    }
    NSLog(@"%@ (frame %llu)", what, (unsigned long long)frame);
    [self showStatus:[NSString stringWithFormat:@"%@%@", what, message.length != 0 ? @" (from another build)" : @""]];
}

- (void)showStatus:(NSString*)text {
    _status = text;
    _statusUntil = [NSDate dateWithTimeIntervalSinceNow:3];
    _subtitleDirty = YES;
    [self updateSubtitle];
}

// The window's subtitle: the room the player is in, then a recent save/load
// and the frame counter if shown, "Hyrule Town · Saved slot 2". Called every
// draw; it sets the text only when a part of it changed.
- (void)updateSubtitle {
    int area, room;
    if (!RoomCurrent(&area, &room)) {
        area = room = -1;
    }
    if (area != _roomArea || room != _roomIndex) {
        _roomArea = area;
        _roomIndex = room;
        const char* name = area >= 0 ? RoomName(area, room) : NULL;
        _roomName = name != NULL ? @(name) : nil;
        _subtitleDirty = YES;
    }
    BOOL status = _status != nil && _statusUntil.timeIntervalSinceNow > 0;
    if (status != _statusShown) {
        _statusShown = status;
        _subtitleDirty = YES;
    }
    BOOL counter = [NSUserDefaults.standardUserDefaults boolForKey:kFrameCounterDefault];
    if (counter && (_drawCount++ % 8 == 0 || FrontendIsPaused())) {
        _subtitleDirty = YES;
    }
    if (!_subtitleDirty) {
        return;
    }
    _subtitleDirty = NO;
    NSMutableArray<NSString*>* parts = [NSMutableArray array];
    if (_roomName != nil) {
        [parts addObject:_roomName];
    }
    if (status) {
        [parts addObject:_status];
    }
    if (counter) {
        [parts addObject:[NSString stringWithFormat:@"Frame %llu", (unsigned long long)FrontendFrame()]];
    }
    if (_renderer.gpuPpuEnabled) [parts addObject:@"GPU PPU (experimental)"];
    NSString* text = [parts componentsJoinedByString:@" · "];
    if (![_window.subtitle isEqualToString:text]) {
        _window.subtitle = text;
    }
}

- (void)quickSave:(id)sender {
    [self requestState:YES path:self.slotPath what:[NSString stringWithFormat:@"Saved slot %ld", (long)self.stateSlot]];
}

- (void)quickLoad:(id)sender {
    if (![NSFileManager.defaultManager fileExistsAtPath:self.slotPath]) {
        [self showStatus:[NSString stringWithFormat:@"Slot %ld is empty", (long)self.stateSlot]];
        return;
    }
    [self requestState:NO path:self.slotPath what:[NSString stringWithFormat:@"Loaded slot %ld", (long)self.stateSlot]];
}

- (void)selectSlot:(NSMenuItem*)sender {
    [NSUserDefaults.standardUserDefaults setInteger:sender.tag forKey:kStateSlotDefault];
    [self showStatus:[NSString stringWithFormat:@"Slot %ld", (long)sender.tag]];
}

- (UTType*)stateType {
    return [UTType typeWithFilenameExtension:@STATE_EXTENSION conformingToType:UTTypeData];
}

- (void)saveStateAs:(id)sender {
    NSSavePanel* panel = [NSSavePanel savePanel];
    panel.directoryURL = [NSURL fileURLWithPath:self.stateFolder];
    panel.nameFieldStringValue = [NSString stringWithFormat:@"frame_%06llu.%s", (unsigned long long)FrontendFrame(),
                                                            STATE_EXTENSION];
    panel.allowedContentTypes = @[ self.stateType ];
    [panel beginSheetModalForWindow:_window
                  completionHandler:^(NSModalResponse result) {
                      if (result == NSModalResponseOK) {
                          [self requestState:YES path:panel.URL.path
                                        what:[@"Saved " stringByAppendingString:panel.URL.lastPathComponent]];
                      }
                  }];
}

- (void)loadStateFromFile:(id)sender {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.directoryURL = [NSURL fileURLWithPath:self.stateFolder];
    panel.allowedContentTypes = @[ self.stateType ];
    [panel beginSheetModalForWindow:_window
                  completionHandler:^(NSModalResponse result) {
                      if (result == NSModalResponseOK) {
                          [self loadStateAtPath:panel.URL.path];
                      }
                  }];
}

- (void)loadStateAtPath:(NSString*)path {
    [self requestState:NO path:path what:[@"Loaded " stringByAppendingString:path.lastPathComponent]];
}

- (void)loadCheckpoint:(NSMenuItem*)sender {
    [self loadStateAtPath:sender.representedObject];
}

- (void)showStateFolder:(id)sender {
    [NSWorkspace.sharedWorkspace openURL:[NSURL fileURLWithPath:self.stateFolder]];
}

- (void)stepFrame:(id)sender {
    if (!FrontendIsPaused()) {
        [self togglePause:nil];
        return; // the first press pauses; the next ones step
    }
    FrontendStepFrame();
    [self updateSubtitle];
}

- (void)toggleFrameCounter:(id)sender {
    NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
    [defaults setBool:![defaults boolForKey:kFrameCounterDefault] forKey:kFrameCounterDefault];
    _subtitleDirty = YES;
    [self updateSubtitle];
}

- (void)toggleGpuPpu:(id)sender {
    if (![_renderer enableGpuPpu:!_renderer.gpuPpuEnabled]) {
        NSAlert* alert = [NSAlert new];
        alert.messageText = @"GPU PPU unavailable";
        alert.informativeText = @"The experimental renderer could not be initialized. The software renderer is still in use.";
        [alert beginSheetModalForWindow:_window completionHandler:nil];
        return;
    }
    _subtitleDirty = YES;
    [self updateSubtitle];
    [_view setNeedsDisplay:YES];
}

- (void)debugMenuChanged:(NSNotification*)note {
    [self buildMenus];
    if (![NSUserDefaults.standardUserDefaults boolForKey:kDebugMenuDefault]) {
        if (_renderer.gpuPpuEnabled) {
            [_renderer enableGpuPpu:NO];
            _subtitleDirty = YES;
            [self updateSubtitle];
        }
        [_debugWindows closeAll];
    }
}

- (void)showDebugPanel:(NSMenuItem*)sender {
    [_debugWindows showPanel:sender.tag];
}

// The checkpoints port/scripts/checkpoints.sh writes in the source tree.
- (void)menuNeedsUpdate:(NSMenu*)menu {
    NSString* dir = @TMC_CHECKPOINT_DIR;
    NSArray<NSString*>* names = [NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:nil];
    [menu removeAllItems];
    for (NSString* name in [names sortedArrayUsingSelector:@selector(compare:)]) {
        if ([name.pathExtension isEqualToString:@STATE_EXTENSION]) {
            NSMenuItem* item = [menu addItemWithTitle:name.stringByDeletingPathExtension
                                               action:@selector(loadCheckpoint:)
                                        keyEquivalent:@""];
            item.representedObject = [dir stringByAppendingPathComponent:name];
        }
    }
    if (menu.numberOfItems == 0) {
        [menu addItemWithTitle:@"None: run port/scripts/checkpoints.sh" action:nil keyEquivalent:@""].enabled = NO;
    }
}

#pragma mark Window

- (void)windowWillEnterFullScreen:(NSNotification*)note {
    [NSCursor setHiddenUntilMouseMoves:YES];
}

// In full screen the toolbar slides in with the menu bar, over the game.
- (NSApplicationPresentationOptions)window:(NSWindow*)window
      willUseFullScreenPresentationOptions:(NSApplicationPresentationOptions)proposed {
    return proposed | NSApplicationPresentationAutoHideToolbar;
}

#pragma mark Toolbar

// Right-aligned: Save/Load, Filter, Debug (when Settings shows it) and Settings.
- (NSToolbar*)makeToolbar {
    NSToolbar* toolbar = [[NSToolbar alloc] initWithIdentifier:@"GameToolbar"];
    toolbar.delegate = self;
    toolbar.displayMode = NSToolbarDisplayModeIconOnly;
    toolbar.allowsUserCustomization = YES;
    toolbar.autosavesConfiguration = YES;
    return toolbar;
}

- (BOOL)debugToolbarShown {
    return [NSUserDefaults.standardUserDefaults boolForKey:kDebugToolbarDefault];
}

- (NSArray<NSToolbarItemIdentifier>*)toolbarDefaultItemIdentifiers:(NSToolbar*)toolbar {
    NSMutableArray* ids = [NSMutableArray arrayWithObjects:NSToolbarFlexibleSpaceItemIdentifier, kSaveLoadItem, kFilterItem, nil];
    if (self.debugToolbarShown) {
        [ids addObject:kDebugItem];
    }
    [ids addObject:kSettingsItem];
    return ids;
}

- (NSArray<NSToolbarItemIdentifier>*)toolbarAllowedItemIdentifiers:(NSToolbar*)toolbar {
    NSMutableArray* ids = [NSMutableArray arrayWithObjects:kSaveLoadItem, kFilterItem, kSettingsItem,
                                                           NSToolbarFlexibleSpaceItemIdentifier, NSToolbarSpaceItemIdentifier, nil];
    if (self.debugToolbarShown) {
        [ids insertObject:kDebugItem atIndex:2];
    }
    return ids;
}

- (NSToolbarItem*)toolbar:(NSToolbar*)toolbar
        itemForItemIdentifier:(NSToolbarItemIdentifier)identifier
    willBeInsertedIntoToolbar:(BOOL)flag {
    if ([identifier isEqualToString:kSaveLoadItem]) {
        return [self dropdownItem:identifier label:@"Save/Load" symbol:@"square.and.arrow.down.on.square"
                          toolTip:@"Save states: quick save and load, slot, files"
                             fill:^(NSMenu* menu) { [self addStateItemsTo:menu]; }];
    }
    if ([identifier isEqualToString:kFilterItem]) {
        return [self dropdownItem:identifier label:@"Filter" symbol:@"camera.filters"
                          toolTip:@"Display filter and integer scaling"
                             fill:^(NSMenu* menu) { [self addFilterItemsTo:menu]; }];
    }
    if ([identifier isEqualToString:kDebugItem]) {
        return [self dropdownItem:identifier label:@"Debug" symbol:@"ant"
                          toolTip:@"Debug tools: frame stepping, checkpoints, graphics viewers"
                             fill:^(NSMenu* menu) { [self addDebugItemsTo:menu]; }];
    }
    if ([identifier isEqualToString:kSettingsItem]) {
        NSToolbarItem* item = [[NSToolbarItem alloc] initWithItemIdentifier:identifier];
        item.label = item.paletteLabel = @"Settings";
        item.toolTip = @"Settings";
        NSImage* image = [NSImage imageWithSystemSymbolName:@"gearshape" accessibilityDescription:@"Settings"];
        NSButton* button = [NSButton buttonWithImage:image target:self action:@selector(showSettings:)];
        button.bezelStyle = NSBezelStyleTexturedRounded;
        button.refusesFirstResponder = YES;
        button.toolTip = item.toolTip;
        item.view = button;
        item.menuFormRepresentation = [[NSMenuItem alloc] initWithTitle:@"Settings…" action:@selector(showSettings:)
                                                          keyEquivalent:@""];
        return item;
    }
    return nil;
}

// A toolbar button with a menu, filled by fill (the same items as the menu
// bar's, so they share actions and validateMenuItem: checkmarks).
- (NSToolbarItem*)dropdownItem:(NSToolbarItemIdentifier)identifier
                         label:(NSString*)label
                        symbol:(NSString*)symbol
                       toolTip:(NSString*)toolTip
                          fill:(void (^)(NSMenu*))fill {
    NSToolbarItem* item = [[NSToolbarItem alloc] initWithItemIdentifier:identifier];
    item.label = item.paletteLabel = label;
    item.toolTip = toolTip;
    ToolbarPopUp* popup = [[ToolbarPopUp alloc] initWithFrame:NSZeroRect pullsDown:YES];
    popup.bezelStyle = NSBezelStyleTexturedRounded;
    popup.refusesFirstResponder = YES;
    popup.toolTip = toolTip;
    NSMenu* menu = [[NSMenu alloc] initWithTitle:label];
    // A pull-down shows its first item on the button: the icon.
    NSMenuItem* face = [menu addItemWithTitle:@"" action:nil keyEquivalent:@""];
    face.image = [NSImage imageWithSystemSymbolName:symbol accessibilityDescription:label];
    fill(menu);
    popup.menu = menu;
    popup.imagePosition = NSImageOnly;
    item.view = popup;
    // In text-only mode and the overflow menu.
    NSMenu* form = [[NSMenu alloc] initWithTitle:label];
    fill(form);
    item.menuFormRepresentation = [[NSMenuItem alloc] initWithTitle:label action:nil keyEquivalent:@""];
    item.menuFormRepresentation.submenu = form;
    return item;
}

// The Debug button follows its setting, whatever the saved toolbar holds.
- (void)syncDebugToolbarItem {
    NSToolbar* toolbar = _window.toolbar;
    NSUInteger debug = NSNotFound, settings = NSNotFound;
    for (NSUInteger i = 0; i < toolbar.items.count; i++) {
        NSString* identifier = toolbar.items[i].itemIdentifier;
        if ([identifier isEqualToString:kDebugItem]) {
            debug = i;
        } else if ([identifier isEqualToString:kSettingsItem]) {
            settings = i;
        }
    }
    if (self.debugToolbarShown && debug == NSNotFound) {
        [toolbar insertItemWithItemIdentifier:kDebugItem atIndex:settings != NSNotFound ? settings : toolbar.items.count];
    } else if (!self.debugToolbarShown && debug != NSNotFound) {
        [toolbar removeItemAtIndex:debug];
    }
}

#pragma mark Menus

- (void)buildMenus {
    NSMenu* bar = [NSMenu new];
    NSString* appName = @"The Minish Cap";

    NSMenu* appMenu = [[NSMenu alloc] initWithTitle:appName];
    [appMenu addItemWithTitle:[@"About " stringByAppendingString:appName]
                       action:@selector(showAbout:)
                keyEquivalent:@""];
    [appMenu addItem:NSMenuItem.separatorItem];
    [appMenu addItemWithTitle:@"Settings…" action:@selector(showSettings:) keyEquivalent:@","];
    [appMenu addItem:NSMenuItem.separatorItem];
    [appMenu addItemWithTitle:@"Show Save Folder" action:@selector(showSaveFolder:) keyEquivalent:@""];
    if (RomSetup.needsRom) {
        [appMenu addItemWithTitle:@"Change ROM…" action:@selector(changeRom:) keyEquivalent:@""];
        [appMenu addItemWithTitle:@"Re-extract Game Data" action:@selector(reextractGameData:) keyEquivalent:@""];
    }
    [appMenu addItem:NSMenuItem.separatorItem];
    [appMenu addItemWithTitle:[@"Hide " stringByAppendingString:appName] action:@selector(hide:) keyEquivalent:@"h"];
    [appMenu addItemWithTitle:[@"Quit " stringByAppendingString:appName] action:@selector(terminate:) keyEquivalent:@"q"];
    [self addSubmenu:appMenu toBar:bar];

    NSMenu* game = [[NSMenu alloc] initWithTitle:@"Game"];
    [game addItemWithTitle:@"Pause" action:@selector(togglePause:) keyEquivalent:@"p"];
    [game addItemWithTitle:@"Reset" action:@selector(resetGame:) keyEquivalent:@"r"];
    [game addItem:NSMenuItem.separatorItem];
    [self addStateItemsTo:game];
    [game addItem:NSMenuItem.separatorItem];
    NSMenuItem* help = [game addItemWithTitle:@"Hold Tab to fast-forward" action:nil keyEquivalent:@""];
    help.enabled = NO;
    [self addSubmenu:game toBar:bar];

    NSMenu* sound = [[NSMenu alloc] initWithTitle:@"Sound"];
    NSMenuItem* mute = [sound addItemWithTitle:@"Mute" action:@selector(toggleMute:) keyEquivalent:@"m"];
    mute.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
    [sound addItemWithTitle:@"High-Quality Audio" action:@selector(toggleHighQualityAudio:) keyEquivalent:@""];
    [self addSubmenu:sound toBar:bar];

    NSMenu* view = [[NSMenu alloc] initWithTitle:@"View"];
    [self addFilterItemsTo:view];
    [view addItem:NSMenuItem.separatorItem];
    [view addItemWithTitle:@"Customize Toolbar…" action:@selector(runToolbarCustomizationPalette:) keyEquivalent:@""];
    NSMenuItem* fullScreen = [view addItemWithTitle:@"Toggle Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
    fullScreen.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagControl;
    [self addSubmenu:view toBar:bar];

    if ([NSUserDefaults.standardUserDefaults boolForKey:kDebugMenuDefault]) {
        NSMenu* debug = [[NSMenu alloc] initWithTitle:@"Debug"];
        [self addDebugItemsTo:debug];
        [self addSubmenu:debug toBar:bar];
    }

    NSMenu* window = [[NSMenu alloc] initWithTitle:@"Window"];
    [window addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    [window addItemWithTitle:@"Close" action:@selector(performClose:) keyEquivalent:@"w"];
    [self addSubmenu:window toBar:bar];
    NSApp.windowsMenu = window;

    NSApp.mainMenu = bar;
}

// The menu bar's items that the toolbar's dropdowns have too.

- (void)addStateItemsTo:(NSMenu*)menu {
    NSString* f5 = [NSString stringWithFormat:@"%C", (unichar)NSF5FunctionKey];
    NSString* f7 = [NSString stringWithFormat:@"%C", (unichar)NSF7FunctionKey];
    [menu addItemWithTitle:@"Quick Save" action:@selector(quickSave:) keyEquivalent:f5].keyEquivalentModifierMask = 0;
    [menu addItemWithTitle:@"Quick Load" action:@selector(quickLoad:) keyEquivalent:f7].keyEquivalentModifierMask = 0;
    NSMenu* slots = [[NSMenu alloc] initWithTitle:@"Quick Save Slot"];
    for (NSInteger slot = 1; slot <= STATE_SLOTS; slot++) {
        NSMenuItem* item = [slots addItemWithTitle:[NSString stringWithFormat:@"Slot %ld", (long)slot]
                                            action:@selector(selectSlot:)
                                     keyEquivalent:[NSString stringWithFormat:@"%ld", (long)slot]];
        item.keyEquivalentModifierMask = NSEventModifierFlagOption | NSEventModifierFlagCommand;
        item.tag = slot;
    }
    [menu addItemWithTitle:slots.title action:nil keyEquivalent:@""].submenu = slots;
    [menu addItemWithTitle:@"Save State As…" action:@selector(saveStateAs:) keyEquivalent:@"S"];
    [menu addItemWithTitle:@"Load State…" action:@selector(loadStateFromFile:) keyEquivalent:@"o"];
}

- (void)addFilterItemsTo:(NSMenu*)menu {
    for (NSUInteger mode = 0; mode < FilterModeCount; mode++) {
        NSString* key = [NSString stringWithFormat:@"%lu", (unsigned long)mode + 1];
        NSMenuItem* item = [menu addItemWithTitle:FilterModeName(mode) action:@selector(selectFilter:) keyEquivalent:key];
        item.tag = mode;
    }
    [menu addItem:NSMenuItem.separatorItem];
    [menu addItemWithTitle:@"Integer Scaling" action:@selector(toggleIntegerScaling:) keyEquivalent:@"i"];
}

- (void)addDebugItemsTo:(NSMenu*)menu {
    NSString* f8 = [NSString stringWithFormat:@"%C", (unichar)NSF8FunctionKey];
    [menu addItemWithTitle:@"Step Frame" action:@selector(stepFrame:) keyEquivalent:f8].keyEquivalentModifierMask = 0;
    [menu addItemWithTitle:@"Show Frame Counter" action:@selector(toggleFrameCounter:) keyEquivalent:@""];
    NSMenuItem* gpuPpu = [menu addItemWithTitle:@"GPU PPU (Experimental)" action:@selector(toggleGpuPpu:) keyEquivalent:@""];
    gpuPpu.toolTip = @"Render gameplay on the GPU; scanline state capture still uses CPU time and memory.";
    [menu addItem:NSMenuItem.separatorItem];
    NSMenu* checkpoints = [[NSMenu alloc] initWithTitle:@"Load Checkpoint"];
    checkpoints.delegate = self; // filled when opened (menuNeedsUpdate:)
    [menu addItemWithTitle:checkpoints.title action:nil keyEquivalent:@""].submenu = checkpoints;
    [menu addItemWithTitle:@"Show State Folder" action:@selector(showStateFolder:) keyEquivalent:@""];
    [menu addItem:NSMenuItem.separatorItem];
    // Graphics viewers: Control-Command-1 .. 7.
    for (NSInteger kind = 0; kind < DebugPanelCount; kind++) {
        NSMenuItem* item = [menu addItemWithTitle:[DebugWindows titleForPanel:kind]
                                           action:@selector(showDebugPanel:)
                                    keyEquivalent:[NSString stringWithFormat:@"%ld", (long)kind + 1]];
        item.keyEquivalentModifierMask = NSEventModifierFlagControl | NSEventModifierFlagCommand;
        item.tag = kind;
    }
}

- (void)addSubmenu:(NSMenu*)menu toBar:(NSMenu*)bar {
    NSMenuItem* item = [bar addItemWithTitle:menu.title action:nil keyEquivalent:@""];
    item.submenu = menu;
}

- (BOOL)validateMenuItem:(NSMenuItem*)item {
    if (item.action == @selector(selectFilter:)) {
        item.state = (NSUInteger)item.tag == _renderer.filter ? NSControlStateValueOn : NSControlStateValueOff;
    } else if (item.action == @selector(toggleIntegerScaling:)) {
        item.state = _renderer.integerScaling ? NSControlStateValueOn : NSControlStateValueOff;
    } else if (item.action == @selector(toggleMute:)) {
        item.state = AudioIsMuted() ? NSControlStateValueOn : NSControlStateValueOff;
    } else if (item.action == @selector(toggleHighQualityAudio:)) {
        item.state = AudioIsHighQuality() ? NSControlStateValueOn : NSControlStateValueOff;
    } else if (item.action == @selector(selectSlot:)) {
        item.state = item.tag == self.stateSlot ? NSControlStateValueOn : NSControlStateValueOff;
    } else if (item.action == @selector(toggleFrameCounter:)) {
        item.state = [NSUserDefaults.standardUserDefaults boolForKey:kFrameCounterDefault] ? NSControlStateValueOn
                                                                                           : NSControlStateValueOff;
    } else if (item.action == @selector(toggleGpuPpu:)) {
        item.state = _renderer.gpuPpuEnabled ? NSControlStateValueOn : NSControlStateValueOff;
    } else if (item.action == @selector(togglePause:)) {
        item.title = FrontendIsPaused() ? @"Resume" : @"Pause";
    } else if (item.action == @selector(changeRom:) || item.action == @selector(reextractGameData:)) {
        return _window != nil && [_romSetup validateMenuItem:item];
    }
    return _window != nil || item.action == @selector(terminate:) || item.action == @selector(hide:) ||
           item.action == @selector(showAbout:);
}

// The standard About panel (name, icon and version come from the bundle) with credits.
- (void)showAbout:(id)sender {
    NSFont* font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    NSMutableParagraphStyle* centered = [NSMutableParagraphStyle new];
    centered.alignment = NSTextAlignmentCenter;
    NSMutableAttributedString* credits = [NSMutableAttributedString new];
    void (^add)(NSString*, NSString*) = ^(NSString* text, NSString* link) {
        NSMutableDictionary* attributes = [@{
            NSFontAttributeName : font,
            NSForegroundColorAttributeName : NSColor.labelColor,
            NSParagraphStyleAttributeName : centered,
        } mutableCopy];
        if (link != nil) {
            attributes[NSLinkAttributeName] = [NSURL URLWithString:link];
        }
        [credits appendAttributedString:[[NSAttributedString alloc] initWithString:text attributes:attributes]];
    };
    add(@"A native macOS port of the ", nil);
    add(@"zeldaret/tmc", @"https://github.com/zeldaret/tmc");
    add(@" decompilation of The Legend of Zelda: The Minish Cap.\n\nPort: ", nil);
    add(@"awkitsune/tmc_macos", @"https://github.com/awkitsune/tmc_macos");
    add(RomDataIsBaked() ? @"\n\nThe game belongs to Nintendo and Capcom. This development build has its data built in; "
                           @"do not share it."
                         : @"\n\nThe game belongs to Nintendo and Capcom. This app contains none of its data: "
                           @"it reads your own copy of the game.",
        nil);
    [NSApp orderFrontStandardAboutPanelWithOptions:@{
        NSAboutPanelOptionCredits : credits,
        NSAboutPanelOptionApplicationName : @"The Minish Cap",
    }];
}

- (void)showSettings:(id)sender {
    if (_settings == nil) {
        _settings = [[SettingsWindowController alloc] initWithRenderer:_renderer
                                                         input:_input
                                                  pacingStatus:^NSString* {
                                                      return [self pacingStatus];
                                                  }];
    }
    [_settings refresh];
    [_settings showWindow:sender];
}

- (void)selectFilter:(NSMenuItem*)sender {
    _renderer.filter = sender.tag;
    [NSUserDefaults.standardUserDefaults setInteger:sender.tag forKey:kFilterDefault];
    [_settings refresh];
}


- (void)toggleIntegerScaling:(id)sender {
    _renderer.integerScaling = !_renderer.integerScaling;
    [NSUserDefaults.standardUserDefaults setBool:_renderer.integerScaling forKey:kIntegerScalingDefault];
    [_settings refresh];
}

- (void)toggleMute:(id)sender {
    AudioSetMuted(!AudioIsMuted());
    [NSUserDefaults.standardUserDefaults setBool:AudioIsMuted() forKey:kMutedDefault];
}

- (void)toggleHighQualityAudio:(id)sender {
    AudioSetHighQuality(!AudioIsHighQuality());
    [NSUserDefaults.standardUserDefaults setBool:AudioIsHighQuality() forKey:kHighQualityAudioDefault];
}

- (void)togglePause:(id)sender {
    FrontendSetPaused(!FrontendIsPaused());
    _window.title = FrontendIsPaused() ? @"The Minish Cap (Paused)" : @"The Minish Cap";
}

- (void)resetGame:(id)sender {
    FrontendRequestReset();
}

- (void)changeRom:(id)sender {
    [_romSetup changeRomForWindow:_window];
}

- (void)reextractGameData:(id)sender {
    [_romSetup reextractForWindow:_window];
}

- (void)showSaveFolder:(id)sender {
    [NSWorkspace.sharedWorkspace selectFile:self.savePath inFileViewerRootedAtPath:@""];
}

@end

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        NSApplication* app = NSApplication.sharedApplication;
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        AppDelegate* delegate = [AppDelegate new];
        app.delegate = delegate;
        [app run];
    }
    return 0;
}
