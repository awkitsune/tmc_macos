#import "input.h"

#if !TARGET_OS_IOS
#import <Carbon/Carbon.h> // kVK_* virtual key codes
#endif
#import <GameController/GameController.h>

#include <stdatomic.h>
#include <time.h>

#include "latency.h"
#include "port/hw.h"

#if TARGET_OS_IOS
// The keyboard layout of the macOS app, by HID usage (GCKeyCode), read through
// GCKeyboard. Escape asks for the settings.
typedef struct {
    GCKeyCode keyCode;
    uint16_t key;
} KeyBinding;

static const KeyBinding kKeyBindings[] = {
    { 0x52, HW_KEY_UP },     // GCKeyCodeUpArrow
    { 0x51, HW_KEY_DOWN },   // GCKeyCodeDownArrow
    { 0x50, HW_KEY_LEFT },   // GCKeyCodeLeftArrow
    { 0x4F, HW_KEY_RIGHT },  // GCKeyCodeRightArrow
    { 0x1B, HW_KEY_A },      // GCKeyCodeKeyX
    { 0x1D, HW_KEY_B },      // GCKeyCodeKeyZ
    { 0x04, HW_KEY_L },      // GCKeyCodeKeyA
    { 0x16, HW_KEY_R },      // GCKeyCodeKeyS
    { 0x28, HW_KEY_START },  // GCKeyCodeReturnOrEnter
    { 0x58, HW_KEY_START },  // GCKeyCodeKeypadEnter
    { 0x2A, HW_KEY_SELECT }, // GCKeyCodeDeleteOrBackspace
    { 0x31, HW_KEY_SELECT }, // GCKeyCodeBackslash
};

static const GCKeyCode kFastForwardKey = 0x2B; // GCKeyCodeTab
static const GCKeyCode kSettingsKey = 0x29;    // GCKeyCodeEscape
// Holding ☰ this long opens the settings; a shorter press is GBA Start, given
// to the game on release for kStartPulseNs (the settings never see a Start).
static const NSTimeInterval kMenuHoldSeconds = 0.6;
static const uint64_t kStartPulseNs = 100000000ull;
#else
typedef struct {
    unsigned short keyCode;
    uint16_t key;
} KeyBinding;

// Default keyboard layout (physical keys, shown here as on a US keyboard).
static const KeyBinding kKeyBindings[] = {
    { kVK_UpArrow, HW_KEY_UP },
    { kVK_DownArrow, HW_KEY_DOWN },
    { kVK_LeftArrow, HW_KEY_LEFT },
    { kVK_RightArrow, HW_KEY_RIGHT },
    { kVK_ANSI_X, HW_KEY_A },
    { kVK_ANSI_Z, HW_KEY_B },
    { kVK_ANSI_A, HW_KEY_L },
    { kVK_ANSI_S, HW_KEY_R },
    { kVK_Return, HW_KEY_START },
    { kVK_ANSI_KeypadEnter, HW_KEY_START },
    { kVK_Delete, HW_KEY_SELECT },
    { kVK_ANSI_Backslash, HW_KEY_SELECT },
};

static const unsigned short kFastForwardKey = kVK_Tab;
#endif

static BOOL IsNintendoPad(GCController* controller);

// The keyboard state is written by the main thread (key events) and read by the
// game thread, right before each frame.
@implementation Input {
    _Atomic uint16_t _keyboardKeys;
    atomic_bool _fastForwardKey;
    dispatch_queue_t _padQueue; // latency log only
#if TARGET_OS_IOS
    _Atomic uint16_t _touchKeys;
    NSMutableSet<GCController*>* _connected; // main thread
    BOOL _keyboardConnected;
    BOOL _menuDown;
    BOOL _menuOpenedSettings; // this press of ☰ was a hold: no Start on release
    NSUInteger _menuPresses;
    _Atomic uint64_t _startUntilNs; // ☰ was tapped: Start until then (CLOCK_UPTIME_RAW)
    id _homeSettings;               // GCControllerHomeButtonSettingsManager (iOS 27)
#endif
}

- (instancetype)init {
    if ((self = [super init]) != nil) {
        NSNotificationCenter* centre = NSNotificationCenter.defaultCenter;
        [centre addObserver:self selector:@selector(controllersDidChange:) name:GCControllerDidConnectNotification object:nil];
        [centre addObserver:self selector:@selector(controllersDidChange:) name:GCControllerDidDisconnectNotification object:nil];
        [GCController startWirelessControllerDiscoveryWithCompletionHandler:nil];
#if TARGET_OS_IOS
        [centre addObserver:self selector:@selector(keyboardDidChange:) name:GCKeyboardDidConnectNotification object:nil];
        [centre addObserver:self selector:@selector(keyboardDidChange:) name:GCKeyboardDidDisconnectNotification object:nil];
        _connected = [NSMutableSet set];
        for (GCController* controller in GCController.controllers) {
            if (controller.extendedGamepad != nil) {
                [_connected addObject:controller];
                [self watchSettingsButtons:controller];
            }
        }
        if (GCKeyboard.coalescedKeyboard != nil) {
            _keyboardConnected = YES;
            [self watchKeyboard:GCKeyboard.coalescedKeyboard];
        }
#endif
        if (LatencyEnabled()) {
            dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0);
            _padQueue = dispatch_queue_create("gamepad latency", attr);
            for (GCController* controller in GCController.controllers) {
                [self watchController:controller];
            }
        }
    }
    return self;
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
}

- (void)controllersDidChange:(NSNotification*)note {
    GCController* controller = note.object;
    BOOL connected = [note.name isEqualToString:GCControllerDidConnectNotification];
    NSLog(@"controller %@: %@ (%@)", connected ? @"connected" : @"disconnected", controller.vendorName,
          controller.productCategory);
    if (connected && LatencyEnabled()) {
        [self watchController:controller];
    }
#if TARGET_OS_IOS
    if (connected && controller.extendedGamepad != nil) {
        [_connected addObject:controller];
        [self watchSettingsButtons:controller];
    } else if (!connected) {
        [_connected removeObject:controller];
    }
#endif
    if (self.controllersChanged) {
        self.controllersChanged();
    }
}

// Latency log: time the moment a pad's held keys change (off the main thread,
// so a busy main thread does not skew it).
- (void)watchController:(GCController*)controller {
    if (controller.extendedGamepad == nil) {
        return;
    }
    controller.handlerQueue = _padQueue;
    __block uint16_t last = 0;
    __weak Input* weakSelf = self;
    controller.extendedGamepad.valueChangedHandler = ^(GCExtendedGamepad* pad, GCControllerElement* element) {
        uint64_t now = LatencyNowNs();
        uint64_t stamp = (uint64_t)(pad.lastEventTimestamp * 1e9);
        Input* strongSelf = weakSelf;
        GCController* owner = pad.controller; // not captured: the pad keeps its handler
        uint16_t keys = [strongSelf keysForController:owner nintendo:IsNintendoPad(owner)];
        if (keys != last) {
            last = keys;
            // The pad's own timestamp, when it is on the clock we compare with.
            LatencyInputEvent(LATENCY_PAD, stamp > now - 1000000000ull && stamp <= now ? stamp : now);
        }
    };
}

- (NSString*)controllerName {
    for (GCController* controller in GCController.controllers) {
        if (controller.extendedGamepad != nil) {
            return controller.vendorName ?: @"Gamepad";
        }
    }
    return nil;
}

#if !TARGET_OS_IOS
- (BOOL)handleKeyEvent:(NSEvent*)event down:(BOOL)down {
    unsigned short code = event.keyCode;

    if (code == kFastForwardKey) {
        atomic_store(&_fastForwardKey, down);
        return YES;
    }
    for (size_t i = 0; i < sizeof(kKeyBindings) / sizeof(kKeyBindings[0]); i++) {
        if (kKeyBindings[i].keyCode == code) {
            uint16_t before = atomic_load(&_keyboardKeys), after = down ? before | kKeyBindings[i].key : before & ~kKeyBindings[i].key;
            atomic_store(&_keyboardKeys, after);
            if (after != before) {
                LatencyInputEvent(LATENCY_KEY, (uint64_t)(event.timestamp * 1e9));
            }
            return YES;
        }
    }
    return NO;
}

#endif

- (void)setKeyboardKeysForTesting:(uint16_t)keys {
    if (keys != atomic_load(&_keyboardKeys)) {
        atomic_store(&_keyboardKeys, keys);
        LatencyInputEvent(LATENCY_KEY, LatencyNowNs());
    }
}

- (void)releaseAll {
    atomic_store(&_keyboardKeys, 0);
    atomic_store(&_fastForwardKey, false);
#if TARGET_OS_IOS
    atomic_store(&_startUntilNs, 0);
#endif
}

// GameController reports Nintendo pads by printed label (buttonA is the button
// labelled A, on the right), others by Xbox position (buttonA at the bottom).
// productCategory has no Nintendo constants; seen: "Switch Pro Controller".
static BOOL IsNintendoPad(GCController* controller) {
    NSString* category = controller.productCategory;
    for (NSString* name in @[ @"Switch", @"Joy-Con", @"Nintendo" ]) {
        if ([category rangeOfString:name options:NSCaseInsensitiveSearch].location != NSNotFound) {
            return YES;
        }
    }
    return NO;
}

static uint16_t GamepadKeys(GCController* controller, BOOL nintendo, GamepadLayout layout) {
    GCExtendedGamepad* pad = controller.extendedGamepad;
    uint16_t keys = 0;
    const float threshold = 0.5f;
    float x = pad.leftThumbstick.xAxis.value, y = pad.leftThumbstick.yAxis.value;

    // Nintendo labels and Nintendo positions agree, so both layouts treat a
    // Nintendo pad alike: A (right) and X (top) are GBA A, B and Y are GBA B.
    // On other pads, by label means A/X are GBA A and B/Y GBA B; by position
    // means the Nintendo spots: right (B) and top (Y) are GBA A.
    BOOL byLabel = nintendo || layout == GamepadLayoutByLabel;
    GCControllerButtonInput* forA[2] = { byLabel ? pad.buttonA : pad.buttonB, byLabel ? pad.buttonX : pad.buttonY };
    GCControllerButtonInput* forB[2] = { byLabel ? pad.buttonB : pad.buttonA, byLabel ? pad.buttonY : pad.buttonX };
    if (forA[0].pressed || forA[1].pressed) keys |= HW_KEY_A;
    if (forB[0].pressed || forB[1].pressed) keys |= HW_KEY_B;
    if (pad.leftShoulder.pressed || pad.leftTrigger.pressed) keys |= HW_KEY_L;
    if (pad.rightShoulder.pressed || pad.rightTrigger.pressed) keys |= HW_KEY_R;
#if !TARGET_OS_IOS
    // On iOS ☰ is also the settings when held: Input gives its Start on release.
    if (pad.buttonMenu.pressed) keys |= HW_KEY_START;
#endif
    if (pad.buttonOptions.pressed) keys |= HW_KEY_SELECT;
    if (pad.dpad.up.pressed || y > threshold) keys |= HW_KEY_UP;
    if (pad.dpad.down.pressed || y < -threshold) keys |= HW_KEY_DOWN;
    if (pad.dpad.left.pressed || x < -threshold) keys |= HW_KEY_LEFT;
    if (pad.dpad.right.pressed || x > threshold) keys |= HW_KEY_RIGHT;
    // Opposite directions at once confuse the game; the GBA's d-pad cannot do it.
    if ((keys & (HW_KEY_LEFT | HW_KEY_RIGHT)) == (HW_KEY_LEFT | HW_KEY_RIGHT)) keys &= ~(HW_KEY_LEFT | HW_KEY_RIGHT);
    if ((keys & (HW_KEY_UP | HW_KEY_DOWN)) == (HW_KEY_UP | HW_KEY_DOWN)) keys &= ~(HW_KEY_UP | HW_KEY_DOWN);
    return keys;
}

- (uint16_t)keysForController:(GCController*)controller nintendo:(BOOL)nintendo {
    return GamepadKeys(controller, nintendo, self.gamepadLayout);
}

// Called by the game thread right before each frame: no autorelease pool there.
- (uint16_t)pollKeys {
    @autoreleasepool {
        uint16_t keys = atomic_load(&_keyboardKeys);
#if TARGET_OS_IOS
        keys |= atomic_load(&_touchKeys);
        if (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) < atomic_load(&_startUntilNs)) {
            keys |= HW_KEY_START;
        }
        GCKeyboardInput* keyboard = GCKeyboard.coalescedKeyboard.keyboardInput;
        if (keyboard != nil) {
            for (size_t i = 0; i < sizeof(kKeyBindings) / sizeof(kKeyBindings[0]); i++) {
                if ([keyboard buttonForKeyCode:kKeyBindings[i].keyCode].pressed) {
                    keys |= kKeyBindings[i].key;
                }
            }
        }
#endif
        for (GCController* controller in GCController.controllers) {
            if (controller.extendedGamepad != nil) {
                keys |= GamepadKeys(controller, IsNintendoPad(controller), self.gamepadLayout);
            }
        }
        return keys;
    }
}

- (BOOL)fastForwardHeld {
    if (atomic_load(&_fastForwardKey)) {
        return YES;
    }
#if TARGET_OS_IOS
    if (self.fastForwardLatched) {
        return YES;
    }
#endif
    @autoreleasepool {
#if TARGET_OS_IOS
        if ([GCKeyboard.coalescedKeyboard.keyboardInput buttonForKeyCode:kFastForwardKey].pressed) {
            return YES;
        }
#endif
        for (GCController* controller in GCController.controllers) {
            // Right stick click doubles as fast-forward on a gamepad.
            if (controller.extendedGamepad.rightThumbstickButton.pressed) {
                return YES;
            }
        }
    }
    return NO;
}

#if TARGET_OS_IOS

int InputCheckKeyCodes(void) {
    const GCKeyCode named[] = { GCKeyCodeUpArrow, GCKeyCodeDownArrow, GCKeyCodeLeftArrow, GCKeyCodeRightArrow,
                                GCKeyCodeKeyX, GCKeyCodeKeyZ, GCKeyCodeKeyA, GCKeyCodeKeyS, GCKeyCodeReturnOrEnter,
                                GCKeyCodeKeypadEnter, GCKeyCodeDeleteOrBackspace, GCKeyCodeBackslash };
    int wrong = kFastForwardKey != GCKeyCodeTab;
    wrong += kSettingsKey != GCKeyCodeEscape;
    for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
        wrong += kKeyBindings[i].keyCode != named[i];
    }
    return wrong;
}

- (void)setTouchKeys:(uint16_t)keys {
    if (keys != atomic_exchange(&_touchKeys, keys)) {
        LatencyInputEvent(LATENCY_KEY, LatencyNowNs());
    }
}

- (BOOL)hasGamepad {
    return _connected.count > 0;
}

- (BOOL)hasController {
    return _connected.count > 0 || _keyboardConnected;
}

- (NSArray<NSString*>*)controllerNames {
    NSMutableArray<NSString*>* names = [NSMutableArray array];
    for (GCController* controller in _connected) {
        [names addObject:controller.vendorName ?: @"Gamepad"];
    }
    if (_keyboardConnected) {
        [names addObject:@"Keyboard"];
    }
    return names;
}

- (void)keyboardDidChange:(NSNotification*)note {
    BOOL connected = [note.name isEqualToString:GCKeyboardDidConnectNotification];
    NSLog(@"keyboard %@", connected ? @"connected" : @"disconnected");
    _keyboardConnected = connected;
    if (connected) {
        [self watchKeyboard:note.object];
    } else {
        [self releaseAll];
    }
    if (self.controllersChanged) {
        self.controllersChanged();
    }
}

- (void)requestSettings {
    if (self.settingsRequested) {
        self.settingsRequested();
    }
}

- (void)watchKeyboard:(GCKeyboard*)keyboard {
    __weak Input* weakSelf = self;
    keyboard.keyboardInput.keyChangedHandler = ^(GCKeyboardInput* input, GCControllerButtonInput* key, GCKeyCode code,
                                                 BOOL pressed) {
        if (pressed && code == kSettingsKey) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [weakSelf requestSettings];
            });
        }
    };
}

// Ask the system to leave Home to the app instead of showing its own (Game)
// overlay. Set on both profiles, as iOS 27's Home button settings document it;
// the system may still keep the button (see homeButtonStatus).
static void ClaimHomeButton(GCController* controller) {
    controller.extendedGamepad.buttonHome.preferredSystemGestureState = GCSystemGestureStateDisabled;
    controller.physicalInputProfile.buttons[GCInputButtonHome].preferredSystemGestureState = GCSystemGestureStateDisabled;
}

- (void)claimHomeButtons {
    for (GCController* controller in _connected) {
        ClaimHomeButton(controller);
    }
}

- (GCControllerHomeButtonSettingsManager*)homeSettings API_AVAILABLE(ios(27.0)) {
    if (_homeSettings == nil) {
        _homeSettings = [[GCControllerHomeButtonSettingsManager alloc] initWithQueue:dispatch_get_main_queue()];
    }
    return _homeSettings;
}

- (NSString*)homeButtonStatus {
    if (_connected.count == 0) {
        return nil;
    }
    if (@available(iOS 27.0, *)) {
        switch ([self.homeSettings readControllerHomeButtonInAppActionWithError:nil]) {
            case GCControllerHomeButtonSettingInAppActionDefault:
                return @"System";
            case GCControllerHomeButtonSettingInAppActionDefer:
                return @"Settings";
            default:
                return nil;
        }
    }
    return nil;
}

- (void)openHomeButtonSettings {
    if (@available(iOS 27.0, *)) {
        NSError* error = nil;
        if (![self.homeSettings openControllerHomeButtonSettingsForActivity:GCControllerHomeButtonSettingsCustomizeInAppActionActivity
                                                                      error:&error]) {
            NSLog(@"cannot open the Home button settings: %@", error);
        }
    }
}

// The settings open from the Home button (taken from the system, which would
// show its own overlay) or from holding ☰.
- (void)watchSettingsButtons:(GCController*)controller {
    GCExtendedGamepad* pad = controller.extendedGamepad;
    __weak Input* weakSelf = self;
    ClaimHomeButton(controller);
    pad.buttonHome.pressedChangedHandler = ^(GCControllerButtonInput* button, float value, BOOL pressed) {
        if (pressed) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [weakSelf requestSettings];
            });
        }
    };
    pad.buttonMenu.pressedChangedHandler = ^(GCControllerButtonInput* button, float value, BOOL pressed) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [weakSelf menuButtonPressed:pressed];
        });
    };
}

- (void)menuButtonPressed:(BOOL)pressed {
    BOOL wasDown = _menuDown;
    _menuDown = pressed;
    NSUInteger press = ++_menuPresses;
    if (!pressed) {
        if (wasDown && !_menuOpenedSettings) {
            atomic_store(&_startUntilNs, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + kStartPulseNs);
            LatencyInputEvent(LATENCY_PAD, LatencyNowNs());
        }
        _menuOpenedSettings = NO;
        return;
    }
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(kMenuHoldSeconds * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        if (self->_menuDown && self->_menuPresses == press) {
            self->_menuOpenedSettings = YES;
            [self requestSettings];
        }
    });
}

#endif

@end
