#import "input.h"

#import <Carbon/Carbon.h> // kVK_* virtual key codes
#import <GameController/GameController.h>

#include "latency.h"
#include "port/hw.h"

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

@implementation Input {
    uint16_t _keyboardKeys;
    BOOL _fastForwardKey;
}

- (instancetype)init {
    if ((self = [super init]) != nil) {
        NSNotificationCenter* centre = NSNotificationCenter.defaultCenter;
        [centre addObserver:self selector:@selector(controllersDidChange:) name:GCControllerDidConnectNotification object:nil];
        [centre addObserver:self selector:@selector(controllersDidChange:) name:GCControllerDidDisconnectNotification object:nil];
        [GCController startWirelessControllerDiscoveryWithCompletionHandler:nil];
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
    if (self.controllersChanged) {
        self.controllersChanged();
    }
}

- (NSString*)controllerName {
    for (GCController* controller in GCController.controllers) {
        if (controller.extendedGamepad != nil) {
            return controller.vendorName ?: @"Gamepad";
        }
    }
    return nil;
}

- (BOOL)handleKeyEvent:(NSEvent*)event down:(BOOL)down {
    unsigned short code = event.keyCode;

    if (code == kFastForwardKey) {
        _fastForwardKey = down;
        return YES;
    }
    for (size_t i = 0; i < sizeof(kKeyBindings) / sizeof(kKeyBindings[0]); i++) {
        if (kKeyBindings[i].keyCode == code) {
            uint16_t before = _keyboardKeys;
            if (down) {
                _keyboardKeys |= kKeyBindings[i].key;
            } else {
                _keyboardKeys &= ~kKeyBindings[i].key;
            }
            if (_keyboardKeys != before) {
                LatencyInputEvent(LATENCY_KEY, (uint64_t)(event.timestamp * 1e9));
            }
            return YES;
        }
    }
    return NO;
}

- (void)setKeyboardKeysForTesting:(uint16_t)keys {
    if (keys != _keyboardKeys) {
        _keyboardKeys = keys;
        LatencyInputEvent(LATENCY_KEY, LatencyNowNs());
    }
}

- (void)releaseAll {
    _keyboardKeys = 0;
    _fastForwardKey = NO;
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
    if (pad.buttonMenu.pressed) keys |= HW_KEY_START;
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

- (uint16_t)pollKeys {
    uint16_t keys = _keyboardKeys;
    for (GCController* controller in GCController.controllers) {
        if (controller.extendedGamepad != nil) {
            keys |= GamepadKeys(controller, IsNintendoPad(controller), self.gamepadLayout);
        }
    }
    return keys;
}

- (BOOL)fastForwardHeld {
    if (_fastForwardKey) {
        return YES;
    }
    for (GCController* controller in GCController.controllers) {
        // Right stick click doubles as fast-forward on a gamepad.
        if (controller.extendedGamepad.rightThumbstickButton.pressed) {
            return YES;
        }
    }
    return NO;
}

@end
