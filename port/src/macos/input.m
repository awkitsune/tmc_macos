#import "input.h"

#import <Carbon/Carbon.h> // kVK_* virtual key codes
#import <GameController/GameController.h>

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
    NSLog(@"controller %@: %@", connected ? @"connected" : @"disconnected", controller.vendorName);
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
            if (down) {
                _keyboardKeys |= kKeyBindings[i].key;
            } else {
                _keyboardKeys &= ~kKeyBindings[i].key;
            }
            return YES;
        }
    }
    return NO;
}

- (void)releaseAll {
    _keyboardKeys = 0;
    _fastForwardKey = NO;
}

static uint16_t GamepadKeys(GCExtendedGamepad* pad) {
    uint16_t keys = 0;
    const float threshold = 0.5f;
    float x = pad.leftThumbstick.xAxis.value, y = pad.leftThumbstick.yAxis.value;

    // Face buttons by position, Nintendo-style: right = A, bottom = B.
    if (pad.buttonB.pressed) keys |= HW_KEY_A;
    if (pad.buttonA.pressed) keys |= HW_KEY_B;
    if (pad.buttonY.pressed) keys |= HW_KEY_A; // top
    if (pad.buttonX.pressed) keys |= HW_KEY_B; // left
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

- (uint16_t)pollKeys {
    uint16_t keys = _keyboardKeys;
    for (GCController* controller in GCController.controllers) {
        GCExtendedGamepad* pad = controller.extendedGamepad;
        if (pad != nil) {
            keys |= GamepadKeys(pad);
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
