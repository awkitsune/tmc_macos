#import <AppKit/AppKit.h>

NS_ASSUME_NONNULL_BEGIN

/*
 * Player input: keyboard keys (by physical key, so the layout does not matter)
 * and any connected extended gamepads, combined into GBA key bits.
 */
@interface Input : NSObject
// Returns YES if the key is a game key (the event is consumed).
- (BOOL)handleKeyEvent:(NSEvent*)event down:(BOOL)down;
// Release everything held on the keyboard (focus lost).
- (void)releaseAll;
// Keyboard and gamepad state as HW_KEY_* bits; cheap, call every refresh.
- (uint16_t)pollKeys;
@property(nonatomic, readonly) BOOL fastForwardHeld;
// Name of the most recently connected gamepad, or nil.
@property(nonatomic, readonly, nullable) NSString* controllerName;
@property(nonatomic, copy, nullable) void (^controllersChanged)(void);
@end

NS_ASSUME_NONNULL_END
