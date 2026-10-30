#import "selftest.h"

#import "audiosession.h"
#import "displaysync.h"
#import "gameview.h"
#import "romimport.h"
#import "settingsstore.h"
#import "settingsview.h"
#import "touchcontrols.h"
#import "touchlayout.h"

#include <stdatomic.h>

#include "frontend.h"
#include "port/hw.h"
#include "port/state.h"

static atomic_int sFailures;

void SelfTestLog(NSString* format, ...) {
    va_list args;
    va_start(args, format);
    NSString* line = [[NSString alloc] initWithFormat:format arguments:args];
    va_end(args);
    // devicectl --console shows the app's standard output.
    printf("%s\n", line.UTF8String);
    fflush(stdout);
    for (NSString* word in @[ @"FAIL", @"NOT ", @"TAKES", @"NO LABEL" ]) {
        if ([line containsString:word]) {
            atomic_fetch_add(&sFailures, 1);
            break;
        }
    }
}

// Every result line below goes through SelfTestLog.
#define NSLog SelfTestLog

static void Check(BOOL ok, NSString* what) {
    NSLog(@"selftest: %@ %@", what, ok ? @"ok" : @"FAILED");
}

// The save and load of the round trip, on the game thread.
static _Atomic uint64_t sSavedChecksum, sLoadedChecksum;
static atomic_int sStatesDone;

static void StateDone(bool ok, const char* message, uint64_t frame, void* context) {
    BOOL save = context != NULL;
    uint64_t checksum = ok ? StateChecksum() : 0;
    atomic_store(save ? &sSavedChecksum : &sLoadedChecksum, checksum);
    atomic_fetch_add(&sStatesDone, 1);
    NSLog(@"selftest: state %s %s at frame %llu, checksum %016llx %s", save ? "save" : "load", ok ? "ok" : "FAILED",
          (unsigned long long)frame, (unsigned long long)checksum, message);
}

// Each face button of a virtual pad gives the GBA key it should, for both
// layouts and for Nintendo (label-reported) and other pads (as on macOS).
static void CheckGamepadMapping(Input* input) {
    GCController* controller = [GCController controllerWithExtendedGamepad];
    GCExtendedGamepad* pad = controller.extendedGamepad;
    GCControllerButtonInput* buttons[4] = { pad.buttonA, pad.buttonB, pad.buttonX, pad.buttonY };
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
                failures += [input keysForController:controller nintendo:nintendo] != expected[nintendo][layout][b];
                [buttons[b] setValue:0];
            }
        }
    }
    // ☰ is Start only when let go before it opens the settings (Input's handler).
    [pad.buttonMenu setValue:1];
    failures += [input keysForController:controller nintendo:NO] != 0;
    [pad.buttonMenu setValue:0];
    input.gamepadLayout = saved;
    Check(failures == 0, @"gamepad mapping");
}

static BOOL PlistFlag(NSString* key) {
    return [[NSBundle.mainBundle objectForInfoDictionaryKey:key] boolValue];
}

static void CheckFiles(void) {
    NSString* home = NSHomeDirectory();
    NSString* documents = [home stringByAppendingPathComponent:@"Documents"];
    NSLog(@"selftest: save %@, states %@", GameViewController.savePath, GameViewController.stateFolder);
    Check([GameViewController.savePath isEqualToString:[documents stringByAppendingPathComponent:@"tmc.sav"]] &&
              [GameViewController.stateFolder hasPrefix:documents],
          @"save and states in Documents");
    Check(PlistFlag(@"UIFileSharingEnabled") && PlistFlag(@"LSSupportsOpeningDocumentsInPlace"),
          @"Documents shown in the Files app");
    Check(PlistFlag(@"GCSupportsControllerUserInteraction") &&
              [[NSBundle.mainBundle objectForInfoDictionaryKey:@"GCSupportedGameControllers"] count] > 0,
          @"game controller Info.plist keys");
}

// The import path: where the data goes, what the launch found, and that a
// file that is no ROM is turned away the way the document picker's would be.
static void CheckRomImport(void) {
    NSString* cache = RomImport.cachePath;
    NSString* support = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/tmc_macos"];
    NSLog(@"selftest: game data %@, cache %@", RomImport.launchSummary, cache);
    // NSHomeDirectory() is /var/mobile/..., the cache path its /private/var/mobile/... spelling.
    Check([cache.stringByResolvingSymlinksInPath
              isEqualToString:[support stringByAppendingPathComponent:@"gamedata.bin"].stringByResolvingSymlinksInPath],
          @"game data cache in the app's Application Support");
    Check(![RomImport.launchSummary isEqualToString:@"not prepared"], @"game data ready at launch");

    NSString* tmp = NSTemporaryDirectory();
    NSString* junk = [tmp stringByAppendingPathComponent:@"selftest_not_a_rom.gba"];
    NSString* junkCache = [tmp stringByAppendingPathComponent:@"selftest_gamedata.bin"];
    [[NSMutableData dataWithLength:16 << 20] writeToFile:junk atomically:YES];
    [NSFileManager.defaultManager removeItemAtPath:junkCache error:nil];
    NSString* message = nil;
    RomDataResult result = [RomImport extractFrom:[NSURL fileURLWithPath:junk] cache:junkCache load:NO progress:nil message:&message];
    NSLog(@"selftest: a file of zeros: %@ (%@)", [RomImport titleForResult:result], message);
    Check(result == ROMDATA_NOT_A_ROM && ![NSFileManager.defaultManager fileExistsAtPath:junkCache],
          @"import turns away a file that is no ROM");
    result = [RomImport extractFrom:[NSURL fileURLWithPath:[tmp stringByAppendingPathComponent:@"missing.gba"]]
                              cache:junkCache
                               load:NO
                           progress:nil
                            message:&message];
    Check(result == ROMDATA_UNREADABLE, @"import reports a missing file");
    [NSFileManager.defaultManager removeItemAtPath:junk error:nil];
}

// The screens the touch layouts are checked on, in points, with the safe area
// insets the game screen has on them (status bar hidden), portrait and landscape.
typedef struct {
    NSString* name;
    CGSize portrait;
    CGFloat scale;
    BOOL pad;
    UIEdgeInsets portraitSafe, landscapeSafe; // top, left, bottom, right
} Screen;

static const Screen kScreens[] = {
    { @"iPhone SE", { 375, 667 }, 2, NO, { 0, 0, 0, 0 }, { 0, 0, 0, 0 } },
    { @"iPhone 15", { 393, 852 }, 3, NO, { 59, 0, 34, 0 }, { 0, 59, 21, 59 } },
    { @"iPhone 15 Pro Max", { 430, 932 }, 3, NO, { 59, 0, 34, 0 }, { 0, 59, 21, 59 } },
    { @"iPad mini 7", { 744, 1133 }, 2, YES, { 0, 0, 20, 0 }, { 0, 0, 20, 0 } },
    { @"iPad Pro 13", { 1032, 1376 }, 2, YES, { 0, 0, 20, 0 }, { 0, 0, 20, 0 } },
};
#define SCREEN_COUNT (sizeof(kScreens) / sizeof(kScreens[0]))

static TouchLayout ScreenLayout(const Screen* screen, BOOL portrait) {
    CGSize size = portrait ? screen->portrait : CGSizeMake(screen->portrait.height, screen->portrait.width);
    return TouchLayoutMake(size, portrait ? screen->portraitSafe : screen->landscapeSafe, portrait, screen->pad, screen->scale);
}

static BOOL Overlap(CGRect a, CGRect b) {
    CGRect both = CGRectIntersection(a, b);
    return !CGRectIsNull(both) && both.size.width > 0.01 && both.size.height > 0.01;
}

// The whole-number scale the renderer gives the picture in the layout's picture rect.
static int PictureScale(const TouchLayout* layout, CGFloat pixelsPerPoint) {
    return (int)(RendererFitPicture(TouchLayoutPixelRect(layout->picture, pixelsPerPoint), YES).size.width / 240);
}

// Every layout on every screen: the picture clear of the controls, the controls
// inside the safe area, apart, and at least 44 pt.
static void CheckTouchLayouts(void) {
    for (size_t i = 0; i < SCREEN_COUNT; i++) {
        const Screen* screen = &kScreens[i];
        for (int portrait = 0; portrait < 2; portrait++) {
            TouchLayout layout = ScreenLayout(screen, portrait);
            CGSize size = portrait ? screen->portrait : CGSizeMake(screen->portrait.height, screen->portrait.width);
            CGRect safe = UIEdgeInsetsInsetRect(CGRectMake(0, 0, size.width, size.height),
                                                portrait ? screen->portraitSafe : screen->landscapeSafe);
            CGRect slack = CGRectInset(safe, -0.01, -0.01);
            NSMutableArray<NSString*>* problems = [NSMutableArray array];
            if (!CGRectContainsRect(slack, layout.picture)) {
                [problems addObject:@"picture outside the safe area"];
            }
            for (TouchControl c = 0; c < TouchControlCount; c++) {
                CGRect rect = layout.controls[c];
                NSString* name = TouchControlName(c);
                if (Overlap(layout.picture, rect)) {
                    [problems addObject:[name stringByAppendingString:@" over the picture"]];
                }
                if (!CGRectContainsRect(slack, rect)) {
                    [problems addObject:[name stringByAppendingString:@" outside the safe area"]];
                }
                if (MIN(rect.size.width, rect.size.height) < 44) {
                    [problems addObject:[name stringByAppendingString:@" under 44 pt"]];
                }
                for (TouchControl d = c + 1; d < TouchControlCount; d++) {
                    if (Overlap(rect, layout.controls[d])) {
                        [problems addObject:[NSString stringWithFormat:@"%@ over %@", name, TouchControlName(d)]];
                    }
                }
            }
            int scale = PictureScale(&layout, screen->scale);
            CGRect picture = RendererFitPicture(TouchLayoutPixelRect(layout.picture, screen->scale), YES);
            NSLog(@"selftest: touch layout %@ %@ %.0fx%.0f pt: picture %.0fx%.0f px, %dx%@", screen->name,
                  portrait ? @"portrait" : @"landscape", size.width, size.height, picture.size.width, picture.size.height,
                  scale, problems.count ? [@"; " stringByAppendingString:[problems componentsJoinedByString:@", "]] : @"");
            Check(problems.count == 0 && scale >= 2,
                  [NSString stringWithFormat:@"touch layout %@ %@", screen->name, portrait ? @"portrait" : @"landscape"]);
        }
    }
    // The sizes asked for on the iPhone 15: 5x (1200x800) in landscape, 4x (960x640) in portrait.
    TouchLayout landscape = ScreenLayout(&kScreens[1], NO), portrait = ScreenLayout(&kScreens[1], YES);
    Check(PictureScale(&landscape, 3) == 5 && PictureScale(&portrait, 3) == 4, @"touch layout: iPhone 15 at 5x and 4x");
}

static CGPoint Middle(CGRect rect) {
    return CGPointMake(CGRectGetMidX(rect), CGRectGetMidY(rect));
}

static CGPoint Offset(CGPoint point, CGFloat dx, CGFloat dy) {
    return CGPointMake(point.x + dx, point.y + dy);
}

// Sample points give the keys they should, on the iPhone 15 and iPad layouts.
static void CheckTouchHits(void) {
    int failures = 0, samples = 0;
    for (size_t i = 1; i < SCREEN_COUNT; i += 2) {
        for (int portrait = 0; portrait < 2; portrait++) {
            TouchLayout layout = ScreenLayout(&kScreens[i], portrait);
            CGPoint dpad = Middle(layout.controls[TouchControlDpad]);
            CGFloat r = layout.controls[TouchControlDpad].size.width / 2;
            CGPoint a = Middle(layout.controls[TouchControlA]), b = Middle(layout.controls[TouchControlB]);
            CGPoint between = CGPointMake((a.x + b.x) / 2, (a.y + b.y) / 2);
            CGFloat face = layout.controls[TouchControlA].size.width / 2;
            struct {
                CGPoint point;
                BOOL dpad;
                uint16_t keys;
                TouchControl hit;
            } cases[] = {
                { dpad, YES, 0, TouchControlDpad },                                        // the dead zone
                { Offset(dpad, 0, -0.6 * r), YES, HW_KEY_UP, TouchControlDpad },
                { Offset(dpad, 0.6 * r, 0), YES, HW_KEY_RIGHT, TouchControlDpad },
                { Offset(dpad, -0.45 * r, -0.45 * r), YES, HW_KEY_UP | HW_KEY_LEFT, TouchControlDpad },
                { Offset(dpad, 0.45 * r, 0.45 * r), YES, HW_KEY_DOWN | HW_KEY_RIGHT, TouchControlDpad },
                { Offset(dpad, -0.6 * r, 0.1 * r), YES, HW_KEY_LEFT, TouchControlDpad },   // a little off the axis
                { Offset(dpad, -1.15 * r, 0), YES, HW_KEY_LEFT, TouchControlDpad },        // just past the edge
                { Offset(dpad, -3 * r, 0), YES, HW_KEY_LEFT, TouchControlCount },          // slid far off: still steers
                { a, NO, HW_KEY_A, TouchControlA },
                { b, NO, HW_KEY_B, TouchControlB },
                { between, NO, HW_KEY_A | HW_KEY_B, TouchControlA },
                { Offset(a, 0, -face - 8), NO, HW_KEY_A, TouchControlA },                  // just above A
                { Middle(layout.controls[TouchControlL]), NO, HW_KEY_L, TouchControlL },
                { Middle(layout.controls[TouchControlR]), NO, HW_KEY_R, TouchControlR },
                { Middle(layout.controls[TouchControlStart]), NO, HW_KEY_START, TouchControlStart },
                { Middle(layout.controls[TouchControlSelect]), NO, HW_KEY_SELECT, TouchControlSelect },
                { Middle(layout.controls[TouchControlMenu]), NO, 0, TouchControlMenu },
                { Middle(layout.picture), NO, 0, TouchControlCount },
            };
            for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
                // The hit for the A-B gap may be either button: both give A|B.
                TouchControl hit = TouchLayoutHit(&layout, cases[c].point);
                BOOL hitOk = hit == cases[c].hit || (c == 10 && hit == TouchControlB);
                uint16_t keys = cases[c].dpad ? TouchLayoutDpadKeys(&layout, cases[c].point)
                                              : TouchLayoutButtonKeys(&layout, cases[c].point);
                samples++;
                if (!hitOk || keys != cases[c].keys) {
                    failures++;
                    NSLog(@"selftest: touch sample %zu on %@ %@: hit %@, keys %03x, expected %@ %03x", c, kScreens[i].name,
                          portrait ? @"portrait" : @"landscape", TouchControlName(hit), keys,
                          TouchControlName(cases[c].hit), cases[c].keys);
                }
            }
        }
    }
    NSLog(@"selftest: %d touch samples, %d wrong", samples, failures);
    Check(failures == 0, @"touch hit-testing");
}

// This device's real safe area insets agree with the table the layouts are checked with.
static void CheckThisScreen(GameViewController* game) {
    CGSize size = game.view.bounds.size;
    UIEdgeInsets safe = game.view.safeAreaInsets;
    BOOL portrait = size.height > size.width;
    NSLog(@"selftest: this screen %.0fx%.0f pt at %.0fx, safe insets %.0f %.0f %.0f %.0f (top left bottom right)", size.width,
          size.height, game.gameView.contentScaleFactor, safe.top, safe.left, safe.bottom, safe.right);
    for (size_t i = 0; i < SCREEN_COUNT; i++) {
        CGSize expected = portrait ? kScreens[i].portrait : CGSizeMake(kScreens[i].portrait.height, kScreens[i].portrait.width);
        if (CGSizeEqualToSize(size, expected)) {
            Check(UIEdgeInsetsEqualToEdgeInsets(safe, portrait ? kScreens[i].portraitSafe : kScreens[i].landscapeSafe),
                  [NSString stringWithFormat:@"safe insets of the %@ as in the layout table", kScreens[i].name]);
        }
    }
}

static void SetTouchControls(BOOL on, BOOL haptics) {
    // As the switches in Settings > Controls do.
    [NSUserDefaults.standardUserDefaults setBool:on forKey:kTouchControlsDefault];
    [NSUserDefaults.standardUserDefaults setBool:haptics forKey:kTouchHapticsDefault];
    [NSNotificationCenter.defaultCenter postNotificationName:kTouchControlsChangedNotification object:nil];
}

// The steps, each run that many seconds after the one before.
static NSMutableArray<void (^)(void)>* sSteps;
static NSMutableArray<NSNumber*>* sDelays;

static void RunNextStep(void) {
    if (sSteps.count == 0) {
        return;
    }
    void (^step)(void) = sSteps.firstObject;
    double delay = sDelays.firstObject.doubleValue;
    [sSteps removeObjectAtIndex:0];
    [sDelays removeObjectAtIndex:0];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        step();
        RunNextStep();
    });
}

void SelfTestStart(GameViewController* game) {
    sSteps = [NSMutableArray array];
    sDelays = [NSMutableArray array];
    void (^add)(double, void (^)(void)) = ^(double seconds, void (^step)(void)) {
        [sDelays addObject:@(seconds)];
        [sSteps addObject:step];
    };
    Input* input = game.input;
    GCController* virtualPad = [GCController controllerWithExtendedGamepad];
    // A real controller can connect while the test runs (a Bluetooth pad waking up).
    BOOL (^realController)(void) = ^BOOL {
        for (GCController* controller in GCController.controllers) {
            if (controller != virtualPad && controller.extendedGamepad != nil) {
                return YES;
            }
        }
        return NO;
    };
    NSString* statePath = [NSTemporaryDirectory() stringByAppendingPathComponent:@"selftest." STATE_EXTENSION];
    __block uint64_t frameMark = 0;
    __block CFTimeInterval timeMark = 0;
    // The touch control settings the player had; the steps below change them.
    BOOL savedTouch = game.touchControlsOn;
    BOOL savedHaptics = [NSUserDefaults.standardUserDefaults boolForKey:kTouchHapticsDefault];

    add(0.5, ^{
        UIDevice* device = UIDevice.currentDevice;
        NSLog(@"selftest: %@ %@ %@, real controller %s", device.model, device.systemName, device.systemVersion,
              realController() ? "connected" : "none");
        Renderer* renderer = game.renderer;
        CGSize size = game.gameView.drawableSize;
        NSLog(@"selftest: renderer on %@, drawable %.0fx%.0f, filter %@", game.gameView.device.name, size.width, size.height,
              renderer != nil ? FilterModeName(renderer.filter) : @"-");
        Check(renderer != nil && size.width > 0 && size.height > 0, @"renderer created");
        // The picture can use the whole screen, safe area insets included.
        CGSize screen = game.view.window.screen.nativeBounds.size;
        NSLog(@"selftest: screen %.0fx%.0f", screen.width, screen.height);
        Check(MAX(size.width, size.height) == MAX(screen.width, screen.height) &&
                  MIN(size.width, size.height) == MIN(screen.width, screen.height),
              @"game view fills the screen");
        // The kernel with the built-in knob values, not the ones saved for this user.
        FilterParams saved = renderer.params;
        renderer.params = FilterParamsDefaults();
        Check(renderer != nil && [renderer checkLcdResponse], @"LCD response kernel");
        renderer.params = saved;
        NSLog(@"selftest: audio %@", AudioSessionDescription());
        Check(AudioSessionIsActive(), @"audio session active");
        CheckRomImport();
        CheckFiles();
        Check(InputCheckKeyCodes() == 0, @"keyboard bindings match GCKeyCode");
        CheckGamepadMapping(input);
        CheckTouchLayouts();
        CheckTouchHits();
        CheckThisScreen(game);
        BOOL waiting = !realController() && !game.touchControlsOn;
        Check(game.controllerOverlayShown == waiting && ((game.pauseReasons & PauseReasonNoController) != 0) == waiting,
              @"overlay and pause follow the controllers at launch");
        // The checks below start with the touch controls off.
        SetTouchControls(NO, savedHaptics);
    });
    // Attach a controller: the overlay goes and the game runs.
    add(0.2, ^{
        [NSNotificationCenter.defaultCenter postNotificationName:GCControllerDidConnectNotification object:virtualPad];
        Check(input.hasController && !game.controllerOverlayShown && (game.pauseReasons & PauseReasonNoController) == 0,
              @"controller attach resumes the game");
        frameMark = FrontendFrame();
        timeMark = CACurrentMediaTime();
    });
    add(3, ^{
        double rate = (FrontendFrame() - frameMark) / (CACurrentMediaTime() - timeMark);
        NSLog(@"selftest: %.2f game frames per second; %@", rate, game.pacingStatus);
        Check(rate > 57 && rate < 62, @"the game runs at ~60 Hz");
        Check([NSUserDefaults.standardUserDefaults integerForKey:@"FramePacing"] != FRONTEND_PACING_DISPLAY_SYNC ||
                  DisplaySyncUsable(),
              @"display sync follows the display");
        // Detach it again: back to the overlay and the pause (unless a real one is there).
        [NSNotificationCenter.defaultCenter postNotificationName:GCControllerDidDisconnectNotification object:virtualPad];
        BOOL real = realController();
        Check(input.hasController == real && game.controllerOverlayShown == !real && FrontendIsPaused() == !real,
              @"controller detach pauses with the overlay");
        [NSNotificationCenter.defaultCenter postNotificationName:GCControllerDidConnectNotification object:virtualPad];
    });
    // A save state round trip: save, play a second, load, and the state is the saved one.
    add(0.5, ^{
        FrontendRequestState(true, statePath.fileSystemRepresentation, StateDone, (void*)1);
    });
    add(1, ^{
        FrontendRequestState(false, statePath.fileSystemRepresentation, StateDone, NULL);
    });
    add(0.5, ^{
        uint64_t saved = atomic_load(&sSavedChecksum), loaded = atomic_load(&sLoadedChecksum);
        Check(atomic_load(&sStatesDone) == 2 && saved != 0 && saved == loaded, @"save state round trip");
        [NSFileManager.defaultManager removeItemAtPath:statePath error:nil];
    });
    // The settings sheet, driven by controller keys.
    add(0.2, ^{
        [game showSettings];
    });
    add(1, ^{
        SettingsController* settings = (SettingsController*)game.settingsController;
        Check(game.settingsShown && FrontendIsPaused() && FrontendWaitUntilPaused(200), @"settings open and pause the game");
        NSDictionary<NSString*, NSArray<NSString*>*>* pages = settings.allPageRows;
        for (NSString* page in @[ @"Settings", @"Display", @"Controls", @"Timing", @"Game", @"About" ]) {
            NSLog(@"selftest: settings page %@: %@", page, [pages[page] componentsJoinedByString:@", "]);
        }
        Check(pages.count == 6 && pages[@"Display"].count == KNOB_COUNT + 4 && [pages[@"Game"] containsObject:@"Quick Save"] &&
                  [pages[@"Controls"] containsObject:@"Touch controls"] && [pages[@"Controls"] containsObject:@"Haptics"],
              @"settings pages");
        uint16_t presses[] = { HW_KEY_DOWN, 0, HW_KEY_DOWN, 0 };
        for (size_t i = 0; i < sizeof(presses) / sizeof(presses[0]); i++) {
            [settings handleKeys:presses[i]];
        }
        Check([settings.focusedRowTitle isEqualToString:@"Controls"], @"settings: D-pad moves");
        [settings handleKeys:HW_KEY_A];
        [settings handleKeys:0];
    });
    add(1, ^{
        SettingsController* settings = (SettingsController*)game.settingsController;
        Check([settings.pageTitle isEqualToString:@"Controls"], @"settings: A opens a page");
        [settings handleKeys:HW_KEY_B];
        [settings handleKeys:0];
    });
    add(1, ^{
        SettingsController* settings = (SettingsController*)game.settingsController;
        Check([settings.pageTitle isEqualToString:@"Settings"], @"settings: B goes back");
        [settings handleKeys:HW_KEY_B];
        [settings handleKeys:0];
    });
    add(1.5, ^{
        Check(!game.settingsShown && !FrontendIsPaused(), @"settings: B closes and the game resumes");
        [game sceneWillResignActive];
        Check(FrontendIsPaused() && FrontendWaitUntilPaused(200), @"paused when the app resigns active");
        [game sceneDidBecomeActive];
        Check(!FrontendIsPaused(), @"resumed when the app is active again");
    });
    // The touch controls: on, they take the place of a controller and leave the
    // picture its own rect; a gamepad hides them; a press goes into pollKeys.
    add(0.5, ^{
        [NSNotificationCenter.defaultCenter postNotificationName:GCControllerDidDisconnectNotification object:virtualPad];
        Renderer* renderer = game.renderer;
        TouchControlsView* touch = game.touchControls;
        Check(CGRectIsEmpty(renderer.pictureRect) && touch.hidden && game.tapOpensSettings,
              @"touch controls off: whole drawable, a tap opens the settings");
        SetTouchControls(YES, YES);
        BOOL real = realController();
        Check(!game.controllerOverlayShown && (game.pauseReasons & PauseReasonNoController) == 0 && !FrontendIsPaused() &&
                  !game.tapOpensSettings,
              @"touch controls on: no overlay, no pause, no tap for the settings");
        CGSize drawable = game.gameView.drawableSize;
        CGRect rect = renderer.pictureRect;
        CGRect picture = RendererFitPicture(rect, renderer.integerScaling);
        NSLog(@"selftest: touch controls on: picture rect %.0f,%.0f %.0fx%.0f px in %.0fx%.0f, picture %.0fx%.0f (%.2fx)%@",
              rect.origin.x, rect.origin.y, rect.size.width, rect.size.height, drawable.width, drawable.height,
              picture.size.width, picture.size.height, picture.size.width / 240, real ? @"; a real gamepad hides them" : @"");
        Check(real || (!touch.hidden && !CGRectIsEmpty(rect) && CGRectContainsRect(CGRectMake(0, 0, drawable.width, drawable.height), rect) &&
                       !CGSizeEqualToSize(rect.size, drawable)),
              @"touch controls on: the picture has its own rect");
        TouchLayout layout = touch.layout;
        BOOL clear = YES;
        for (TouchControl c = 0; c < TouchControlCount; c++) {
            clear &= !Overlap(picture, TouchLayoutPixelRect(layout.controls[c], game.gameView.contentScaleFactor));
        }
        Check(clear, @"touch controls on: the picture is clear of every control");

        [NSNotificationCenter.defaultCenter postNotificationName:GCControllerDidConnectNotification object:virtualPad];
        Check(touch.hidden && CGRectIsEmpty(renderer.pictureRect) && !game.controllerOverlayShown,
              @"a gamepad hides the touch controls; the picture has the whole drawable");
        [NSNotificationCenter.defaultCenter postNotificationName:GCControllerDidDisconnectNotification object:virtualPad];
        Check(real || (!touch.hidden && CGRectEqualToRect(renderer.pictureRect, rect)), @"the touch controls come back when it goes");

        // Presses, through the touch handlers' own path.
        NSUInteger taps = touch.hapticCount;
        NSObject* finger = [NSObject new];
        CGPoint a = Middle(layout.controls[TouchControlA]), b = Middle(layout.controls[TouchControlB]);
        [touch trackTouch:finger at:a phase:UITouchPhaseBegan];
        uint16_t keys = [input pollKeys];
        Check((keys & HW_KEY_A) && !(keys & HW_KEY_B) && touch.hapticCount == taps + 1,
              @"touch: A goes into pollKeys with a haptic tap");
        [touch trackTouch:finger at:CGPointMake((a.x + b.x) / 2, (a.y + b.y) / 2) phase:UITouchPhaseMoved];
        keys = [input pollKeys];
        Check((keys & (HW_KEY_A | HW_KEY_B)) == (HW_KEY_A | HW_KEY_B) && touch.hapticCount == taps + 2,
              @"touch: sliding between A and B presses both");
        [touch trackTouch:finger at:b phase:UITouchPhaseMoved];
        keys = [input pollKeys];
        Check((keys & (HW_KEY_A | HW_KEY_B)) == HW_KEY_B && touch.hapticCount == taps + 2, @"touch: sliding onto B moves the press");
        [touch trackTouch:finger at:b phase:UITouchPhaseEnded];
        Check(([input pollKeys] & (HW_KEY_A | HW_KEY_B)) == 0 && touch.keys == 0 && touch.hapticCount == taps + 2,
              @"touch: let go, no haptic");

        // The D-pad, with a second finger holding R.
        NSObject* thumb = [NSObject new], *other = [NSObject new];
        CGPoint dpad = Middle(layout.controls[TouchControlDpad]);
        CGFloat r = layout.controls[TouchControlDpad].size.width / 2;
        [touch trackTouch:other at:Middle(layout.controls[TouchControlR]) phase:UITouchPhaseBegan];
        [touch trackTouch:thumb at:Offset(dpad, 0, -0.6 * r) phase:UITouchPhaseBegan];
        uint16_t up = [input pollKeys];
        [touch trackTouch:thumb at:Offset(dpad, -0.45 * r, -0.45 * r) phase:UITouchPhaseMoved];
        uint16_t upLeft = [input pollKeys];
        [touch trackTouch:thumb at:dpad phase:UITouchPhaseMoved];
        uint16_t centre = [input pollKeys];
        NSLog(@"selftest: touch D-pad keys up %03x, up-left %03x, centre %03x; %lu haptics", up, upLeft, centre,
              (unsigned long)(touch.hapticCount - taps));
        Check(up == (HW_KEY_UP | HW_KEY_R) && upLeft == (HW_KEY_UP | HW_KEY_LEFT | HW_KEY_R) && centre == HW_KEY_R &&
                  touch.hapticCount == taps + 5,
              @"touch: the D-pad slides between directions, with ticks, while R is held");
        [touch trackTouch:thumb at:dpad phase:UITouchPhaseEnded];
        [touch trackTouch:other at:Middle(layout.controls[TouchControlR]) phase:UITouchPhaseCancelled];
        Check(touch.keys == 0 && [input pollKeys] == 0, @"touch: every finger let go");

        // ☰ opens the settings.
        [touch trackTouch:finger at:Middle(layout.controls[TouchControlMenu]) phase:UITouchPhaseBegan];
        [touch trackTouch:finger at:Middle(layout.controls[TouchControlMenu]) phase:UITouchPhaseEnded];
        Check(game.settingsShown, @"touch: ☰ opens the settings");
    });
    add(1, ^{
        [game closeSettings];
    });
    add(1.5, ^{
        Check(!game.settingsShown && !FrontendIsPaused(), @"touch: the game resumes after the settings");
        SetTouchControls(savedTouch, savedHaptics);
        [NSNotificationCenter.defaultCenter postNotificationName:GCControllerDidConnectNotification object:virtualPad];
        Check(!FrontendIsPaused() && game.touchControlsOn == savedTouch, @"touch controls back to the player's settings");
    });
    // One slow frame (an in-game save writing tmc.sav): once it has dropped out of
    // the frame-time samples, the game still keeps to the display.
    add(1, ^{
        FrontendStallNextFrameForTesting(120);
    });
    add(2, ^{
        frameMark = FrontendFrame();
        timeMark = CACurrentMediaTime();
    });
    add(3, ^{
        double rate = (FrontendFrame() - frameMark) / (CACurrentMediaTime() - timeMark);
        NSLog(@"selftest: %.2f game frames per second after a slow frame", rate);
        Check(rate > 57 && rate < 62, @"the game runs at ~60 Hz after a slow frame");
    });
    add(0.5, ^{
        int failed = atomic_load(&sFailures);
        NSLog(@"selftest: %d failed check%s", failed, failed == 1 ? "" : "s");
        exit(failed ? 1 : 0);
    });

    NSLog(@"selftest: start");
    RunNextStep();
}
