#include <TargetConditionals.h>

#if TARGET_OS_IOS
#import <Foundation/Foundation.h>
#else
#import <AppKit/AppKit.h>
#endif
#import <GameController/GameController.h>

NS_ASSUME_NONNULL_BEGIN

/*
 * Player input: keyboard keys (by physical key, so the layout does not matter)
 * and any connected extended gamepads, combined into GBA key bits. On macOS the
 * keyboard comes as key events; on iOS it is read through GCKeyboard.
 */
// Which gamepad button counts as GBA A. Nintendo pads are the same either way.
typedef NS_ENUM(NSInteger, GamepadLayout) {
    GamepadLayoutByLabel = 0,    // the button labelled A (and X) is GBA A
    GamepadLayoutByPosition = 1, // the right (and top) button is GBA A, as on a GBA
};

@interface Input : NSObject
@property(nonatomic) GamepadLayout gamepadLayout;
#if !TARGET_OS_IOS
// Returns YES if the key is a game key (the event is consumed).
- (BOOL)handleKeyEvent:(NSEvent*)event down:(BOOL)down;
#endif
// Release everything held on the keyboard (focus lost).
- (void)releaseAll;
// Set the keyboard keys as a key event would (latency measurements).
- (void)setKeyboardKeysForTesting:(uint16_t)keys;
// Keyboard and gamepad state as HW_KEY_* bits; cheap; the game thread calls it right before each frame (any thread may).
- (uint16_t)pollKeys;
@property(nonatomic, readonly) BOOL fastForwardHeld;
// GBA keys held on one controller, with the current gamepadLayout (for tests).
- (uint16_t)keysForController:(GCController*)controller nintendo:(BOOL)nintendo;
// Name of the most recently connected gamepad, or nil.
@property(nonatomic, readonly, nullable) NSString* controllerName;
@property(nonatomic, copy, nullable) void (^controllersChanged)(void);
#if TARGET_OS_IOS
// Fast-forward switched on in the settings (in addition to holding its button).
@property(nonatomic) BOOL fastForwardLatched;
// A gamepad or a hardware keyboard is connected (as the connect and disconnect
// notifications have it).
@property(nonatomic, readonly) BOOL hasController;
// The names of the connected gamepads and keyboard.
@property(nonatomic, readonly) NSArray<NSString*>* controllerNames;
// Called on the main thread when the player asks for the settings: the
// controller's Home button, holding ☰ (Menu, also GBA Start), or Escape.
@property(nonatomic, copy, nullable) void (^settingsRequested)(void);
#endif
@end

#if TARGET_OS_IOS
// The keyboard bindings that differ from the GCKeyCode constants they stand for (self-test).
int InputCheckKeyCodes(void);
#endif

NS_ASSUME_NONNULL_END
