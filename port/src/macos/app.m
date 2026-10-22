/*
 * macOS front end: the window, menus and key handling around the Metal view.
 * The game itself runs on its own thread (game.c).
 */

#import <AppKit/AppKit.h>
#import <MetalKit/MetalKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#import "input.h"
#import "renderer.h"
#import "romsetup.h"
#import "selftest.h"
#import "settings.h"

#include "audio.h"
#include "frontend.h"
#include "port/state.h"

static NSString* const kMutedDefault = @"Muted";
static NSString* const kHighQualityAudioDefault = @"HighQualityAudio";
static NSString* const kStateSlotDefault = @"StateSlot";
static NSString* const kFrameCounterDefault = @"ShowFrameCounter";
#define STATE_SLOTS 4

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

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate, NSMenuDelegate>
- (void)stateFinished:(BOOL)ok what:(NSString*)what message:(NSString*)message frame:(uint64_t)frame;
@end

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
    Input* _input;
    RomSetup* _romSetup;
    NSString* _status;     // shown as the window's subtitle until _statusUntil
    NSDate* _statusUntil;
    NSUInteger _drawCount;
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
        kDebugMenuDefault : @(TMC_DEBUG_MENU_DEFAULT),
    }];
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(debugMenuChanged:)
                                               name:kDebugMenuChangedNotification
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
    _window.frameAutosaveName = @"GameWindow";
    if (![_window setFrameUsingName:_window.frameAutosaveName]) {
        [_window center];
    }

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
    Input* input = _input;
    __weak AppDelegate* weakSelf = self;
    _renderer.beforeDraw = ^{
        FrontendSetKeys([input pollKeys]);
        FrontendSetFastForward(input.fastForwardHeld);
        [weakSelf updateSubtitle];
    };
    _view.delegate = _renderer;
    _window.contentView = _view;
    [_window makeFirstResponder:_view];
    [_window makeKeyAndOrderFront:nil];

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
        SelfTestStart(_window, _view, _renderer, _input, selfTest);
    }
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
    [self updateSubtitle];
}

// The window's subtitle: a recent save/load, else the frame counter if shown.
- (void)updateSubtitle {
    NSString* text = @"";
    if (_status != nil && _statusUntil.timeIntervalSinceNow > 0) {
        text = _status;
    } else if ([NSUserDefaults.standardUserDefaults boolForKey:kFrameCounterDefault]) {
        if (_drawCount++ % 8 != 0 && !FrontendIsPaused()) {
            return;
        }
        text = [NSString stringWithFormat:@"Frame %llu", (unsigned long long)FrontendFrame()];
    }
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
    [self updateSubtitle];
}

- (void)debugMenuChanged:(NSNotification*)note {
    [self buildMenus];
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

#pragma mark Menus

- (void)buildMenus {
    NSMenu* bar = [NSMenu new];
    NSString* appName = @"The Minish Cap";

    NSMenu* appMenu = [[NSMenu alloc] initWithTitle:appName];
    [appMenu addItemWithTitle:[@"About " stringByAppendingString:appName]
                       action:@selector(orderFrontStandardAboutPanel:)
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
    NSString* f5 = [NSString stringWithFormat:@"%C", (unichar)NSF5FunctionKey];
    NSString* f7 = [NSString stringWithFormat:@"%C", (unichar)NSF7FunctionKey];
    [game addItemWithTitle:@"Quick Save" action:@selector(quickSave:) keyEquivalent:f5].keyEquivalentModifierMask = 0;
    [game addItemWithTitle:@"Quick Load" action:@selector(quickLoad:) keyEquivalent:f7].keyEquivalentModifierMask = 0;
    NSMenu* slots = [[NSMenu alloc] initWithTitle:@"Quick Save Slot"];
    for (NSInteger slot = 1; slot <= STATE_SLOTS; slot++) {
        NSMenuItem* item = [slots addItemWithTitle:[NSString stringWithFormat:@"Slot %ld", (long)slot]
                                            action:@selector(selectSlot:)
                                     keyEquivalent:[NSString stringWithFormat:@"%ld", (long)slot]];
        item.keyEquivalentModifierMask = NSEventModifierFlagOption | NSEventModifierFlagCommand;
        item.tag = slot;
    }
    [game addItemWithTitle:slots.title action:nil keyEquivalent:@""].submenu = slots;
    [game addItemWithTitle:@"Save State As…" action:@selector(saveStateAs:) keyEquivalent:@"S"];
    [game addItemWithTitle:@"Load State…" action:@selector(loadStateFromFile:) keyEquivalent:@"o"];
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
    for (NSUInteger mode = 0; mode < FilterModeCount; mode++) {
        NSString* key = [NSString stringWithFormat:@"%lu", (unsigned long)mode + 1];
        NSMenuItem* item = [view addItemWithTitle:FilterModeName(mode) action:@selector(selectFilter:) keyEquivalent:key];
        item.tag = mode;
    }
    [view addItem:NSMenuItem.separatorItem];
    [view addItemWithTitle:@"Integer Scaling" action:@selector(toggleIntegerScaling:) keyEquivalent:@"i"];
    [view addItem:NSMenuItem.separatorItem];
    NSMenuItem* fullScreen = [view addItemWithTitle:@"Toggle Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
    fullScreen.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagControl;
    [self addSubmenu:view toBar:bar];

    if ([NSUserDefaults.standardUserDefaults boolForKey:kDebugMenuDefault]) {
        NSMenu* debug = [[NSMenu alloc] initWithTitle:@"Debug"];
        NSString* f8 = [NSString stringWithFormat:@"%C", (unichar)NSF8FunctionKey];
        [debug addItemWithTitle:@"Step Frame" action:@selector(stepFrame:) keyEquivalent:f8].keyEquivalentModifierMask = 0;
        [debug addItemWithTitle:@"Show Frame Counter" action:@selector(toggleFrameCounter:) keyEquivalent:@""];
        [debug addItem:NSMenuItem.separatorItem];
        NSMenu* checkpoints = [[NSMenu alloc] initWithTitle:@"Load Checkpoint"];
        checkpoints.delegate = self; // filled when opened (menuNeedsUpdate:)
        [debug addItemWithTitle:checkpoints.title action:nil keyEquivalent:@""].submenu = checkpoints;
        [debug addItemWithTitle:@"Show State Folder" action:@selector(showStateFolder:) keyEquivalent:@""];
        [self addSubmenu:debug toBar:bar];
    }

    NSMenu* window = [[NSMenu alloc] initWithTitle:@"Window"];
    [window addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    [window addItemWithTitle:@"Close" action:@selector(performClose:) keyEquivalent:@"w"];
    [self addSubmenu:window toBar:bar];
    NSApp.windowsMenu = window;

    NSApp.mainMenu = bar;
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
    } else if (item.action == @selector(togglePause:)) {
        item.title = FrontendIsPaused() ? @"Resume" : @"Pause";
    } else if (item.action == @selector(changeRom:) || item.action == @selector(reextractGameData:)) {
        return _window != nil && [_romSetup validateMenuItem:item];
    }
    return _window != nil || item.action == @selector(terminate:) || item.action == @selector(hide:) ||
           item.action == @selector(orderFrontStandardAboutPanel:);
}

- (void)showSettings:(id)sender {
    if (_settings == nil) {
        _settings = [[SettingsWindowController alloc] initWithRenderer:_renderer input:_input];
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
    // Same addresses every run, so the app's states load byte for byte.
    StateDisableAslr((char**)argv);
    @autoreleasepool {
        NSApplication* app = NSApplication.sharedApplication;
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        AppDelegate* delegate = [AppDelegate new];
        app.delegate = delegate;
        [app run];
    }
    return 0;
}
