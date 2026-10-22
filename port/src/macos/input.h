#import <AppKit/AppKit.h>
#import <GameController/GameController.h>

NS_ASSUME_NONNULL_BEGIN

/*
 * Player input: keyboard keys (by physical key, so the layout does not matter)
 * and any connected extended gamepads, combined into GBA key bits.
 */
// Which gamepad button counts as GBA A. Nintendo pads are the same either way.
typedef NS_ENUM(NSInteger, GamepadLayout) {
    GamepadLayoutByLabel = 0,    // the button labelled A (and X) is GBA A
    GamepadLayoutByPosition = 1, // the right (and top) button is GBA A, as on a GBA
};

@interface Input : NSObject
@property(nonatomic) GamepadLayout gamepadLayout;
// Returns YES if the key is a game key (the event is consumed).
- (BOOL)handleKeyEvent:(NSEvent*)event down:(BOOL)down;
// Release everything held on the keyboard (focus lost).
- (void)releaseAll;
// Keyboard and gamepad state as HW_KEY_* bits; cheap, call every refresh.
- (uint16_t)pollKeys;
@property(nonatomic, readonly) BOOL fastForwardHeld;
// GBA keys held on one controller, with the current gamepadLayout (for tests).
- (uint16_t)keysForController:(GCController*)controller nintendo:(BOOL)nintendo;
// Name of the most recently connected gamepad, or nil.
@property(nonatomic, readonly, nullable) NSString* controllerName;
@property(nonatomic, copy, nullable) void (^controllersChanged)(void);
@end

NS_ASSUME_NONNULL_END
