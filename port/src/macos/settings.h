#import <AppKit/AppKit.h>

#import "input.h"
#import "renderer.h"

NS_ASSUME_NONNULL_BEGIN

// NSUserDefaults keys shared with the menus in app.m.
extern NSString* const kFilterDefault;
extern NSString* const kIntegerScalingDefault;
extern NSString* const kGamepadLayoutDefault;
// The Debug menu (save state tools); on by default in development builds.
extern NSString* const kDebugMenuDefault;
// Posted when the Debug menu setting changes, to rebuild the menus.
extern NSString* const kDebugMenuChangedNotification;

/* Filter knobs saved in NSUserDefaults; missing values fall back to the defaults. */
FilterParams LoadFilterParams(void);

/*
 * Settings window: filter, integer scaling, every shader knob as a
 * slider, the gamepad button layout and the Debug menu. Changes apply to the running game
 * immediately and are saved.
 */
@interface SettingsWindowController : NSWindowController
- (instancetype)initWithRenderer:(Renderer*)renderer input:(Input*)input;
// Re-read the renderer's state (after a menu changed the filter or scaling).
- (void)refresh;
@end

NS_ASSUME_NONNULL_END
