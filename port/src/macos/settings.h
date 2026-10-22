#import <AppKit/AppKit.h>

#import "renderer.h"

NS_ASSUME_NONNULL_BEGIN

// NSUserDefaults keys shared with the menus in app.m.
extern NSString* const kFilterDefault;
extern NSString* const kIntegerScalingDefault;

/* Filter knobs saved in NSUserDefaults; missing values fall back to the defaults. */
FilterParams LoadFilterParams(void);

/*
 * Display Settings window: filter, integer scaling and every shader knob as a
 * slider. Changes apply to the running game immediately and are saved.
 */
@interface SettingsWindowController : NSWindowController
- (instancetype)initWithRenderer:(Renderer*)renderer;
// Re-read the renderer's state (after a menu changed the filter or scaling).
- (void)refresh;
@end

NS_ASSUME_NONNULL_END
