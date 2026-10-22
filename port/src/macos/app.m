/*
 * macOS front end: the window, menus and key handling around the Metal view.
 * The game itself runs on its own thread (game.c).
 */

#import <AppKit/AppKit.h>
#import <MetalKit/MetalKit.h>

#import "input.h"
#import "renderer.h"
#import "selftest.h"
#import "settings.h"

#include "audio.h"
#include "frontend.h"

static NSString* const kMutedDefault = @"Muted";
static NSString* const kHighQualityAudioDefault = @"HighQualityAudio";

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

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end

@implementation AppDelegate {
    NSWindow* _window;
    GameView* _view;
    Renderer* _renderer;
    SettingsWindowController* _settings;
    Input* _input;
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

- (void)applicationDidFinishLaunching:(NSNotification*)note {
    NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
    [defaults registerDefaults:@{
        kFilterDefault : @(FilterModeSharpBilinear),
        kIntegerScalingDefault : @YES,
        kMutedDefault : @NO,
        kHighQualityAudioDefault : @NO,
    }];

    [self buildMenus];

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
    _renderer.beforeDraw = ^{
        FrontendSetKeys([input pollKeys]);
        FrontendSetFastForward(input.fastForwardHeld);
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

    NSString* selfTest = NSProcessInfo.processInfo.environment[@"TMC_SELFTEST"];
    if (selfTest.length != 0) {
        SelfTestStart(_window, _view, _renderer, selfTest);
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
    [appMenu addItemWithTitle:@"Show Save Folder" action:@selector(showSaveFolder:) keyEquivalent:@""];
    [appMenu addItem:NSMenuItem.separatorItem];
    [appMenu addItemWithTitle:[@"Hide " stringByAppendingString:appName] action:@selector(hide:) keyEquivalent:@"h"];
    [appMenu addItemWithTitle:[@"Quit " stringByAppendingString:appName] action:@selector(terminate:) keyEquivalent:@"q"];
    [self addSubmenu:appMenu toBar:bar];

    NSMenu* game = [[NSMenu alloc] initWithTitle:@"Game"];
    [game addItemWithTitle:@"Pause" action:@selector(togglePause:) keyEquivalent:@"p"];
    [game addItemWithTitle:@"Reset" action:@selector(resetGame:) keyEquivalent:@"r"];
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
    [view addItemWithTitle:@"Display Settings…" action:@selector(showDisplaySettings:) keyEquivalent:@","];
    [view addItem:NSMenuItem.separatorItem];
    NSMenuItem* fullScreen = [view addItemWithTitle:@"Toggle Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
    fullScreen.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagControl;
    [self addSubmenu:view toBar:bar];

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
    } else if (item.action == @selector(togglePause:)) {
        item.title = FrontendIsPaused() ? @"Resume" : @"Pause";
    }
    return YES;
}

- (void)showDisplaySettings:(id)sender {
    if (_settings == nil) {
        _settings = [[SettingsWindowController alloc] initWithRenderer:_renderer];
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
