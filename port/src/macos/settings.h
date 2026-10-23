#import <AppKit/AppKit.h>

#import "input.h"
#import "renderer.h"

NS_ASSUME_NONNULL_BEGIN

// NSUserDefaults keys shared with the menus in app.m.
extern NSString* const kFilterDefault;
extern NSString* const kIntegerScalingDefault;
extern NSString* const kGamepadLayoutDefault;
// LCD response (pixel ghosting) on or off; its times are filter knobs.
extern NSString* const kLcdResponseDefault;
// How the game keeps its frame rate: 0 accurate (59.73 Hz timer), 1 display sync.
extern NSString* const kPacingDefault;
// Posted when the pacing setting changes.
extern NSString* const kPacingChangedNotification;
// The Debug menu (save state tools); on by default in development builds.
extern NSString* const kDebugMenuDefault;
// Posted when the Debug menu setting changes, to rebuild the menus.
extern NSString* const kDebugMenuChangedNotification;

/* Filter knobs saved in NSUserDefaults; missing values fall back to the defaults. */
FilterParams LoadFilterParams(void);

/*
 * Settings window: filter, integer scaling, every shader knob as a
 * slider, LCD response, frame pacing, the gamepad button layout and the Debug menu. Changes apply to the running game
 * immediately and are saved.
 */
@interface SettingsWindowController : NSWindowController
- (instancetype)initWithRenderer:(Renderer*)renderer input:(Input*)input pacingStatus:(nullable NSString* (^)(void))pacingStatus;
// Re-read the renderer's state (after a menu changed the filter or scaling).
- (void)refresh;
@end

NS_ASSUME_NONNULL_END
