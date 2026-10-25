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
// The Debug button in the game window's toolbar; off by default.
extern NSString* const kDebugToolbarDefault;
// Posted when the Debug button setting changes, to update the toolbar.
extern NSString* const kDebugToolbarChangedNotification;

/* Every NSUserDefaults key the Settings window sets, one control each. */
NSArray<NSString*>* SettingsKeys(void);

/* Filter knobs saved in NSUserDefaults; missing values fall back to the defaults. */
FilterParams LoadFilterParams(void);

/*
 * Settings window, in tabs: Display (filter, integer scaling, LCD response,
 * colour), Filters (the LCD and CRT knobs as sliders), Timing (frame pacing),
 * Controls (the gamepad button layout) and Debug (the Debug menu and its
 * toolbar button). Each tab scrolls, so the window fits short screens; it
 * reopens on the last tab. Changes apply to the running game immediately and
 * are saved. Each setting's control has its NSUserDefaults key as identifier.
 */
@interface SettingsWindowController : NSWindowController
- (instancetype)initWithRenderer:(Renderer*)renderer input:(Input*)input pacingStatus:(nullable NSString* (^)(void))pacingStatus;
// Re-read the renderer's state (after a menu changed the filter or scaling).
- (void)refresh;
// Size the window to its tallest tab, but no taller than maxHeight (0: the
// screen's visible frame); the tabs scroll.
- (void)fitToHeight:(CGFloat)maxHeight;
@property(nonatomic, readonly) NSTabView* tabView;
@end

NS_ASSUME_NONNULL_END
