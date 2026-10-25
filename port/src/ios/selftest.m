#import "selftest.h"

#import "audiosession.h"
#import "displaysync.h"
#import "gameview.h"
#import "romimport.h"
#import "settingsstore.h"
#import "settingsview.h"

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
    [pad.buttonMenu setValue:1];
    failures += [input keysForController:controller nintendo:NO] != HW_KEY_START;
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
        BOOL real = realController();
        Check(game.controllerOverlayShown == !real && ((game.pauseReasons & PauseReasonNoController) != 0) == !real,
              @"overlay and pause follow the controllers at launch");
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
        Check(pages.count == 6 && pages[@"Display"].count == KNOB_COUNT + 4 && [pages[@"Game"] containsObject:@"Quick Save"],
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
