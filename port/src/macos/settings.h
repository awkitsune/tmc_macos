#import <AppKit/AppKit.h>

#import "input.h"
#import "renderer.h"
#import "settingsstore.h"

NS_ASSUME_NONNULL_BEGIN

/*
 * Settings window, in tabs: Display (filter, integer scaling, LCD response,
 * colour), Filters (the LCD and CRT knobs as sliders), Rendering (GPU PPU
 * experiments), Timing (frame pacing), Controls (the gamepad button layout) and Debug (the Debug menu and its
 * toolbar button). Each tab scrolls, so the window fits short screens; it
 * reopens on the last tab. Changes apply to the running game immediately and
 * are saved except the optional GPU rendering experiments, which reset on launch.
 * Each saved setting's control has its NSUserDefaults key as identifier.
 */
@interface SettingsWindowController : NSWindowController
- (instancetype)initWithRenderer:(Renderer*)renderer input:(Input*)input pacingStatus:(nullable NSString* (^)(void))pacingStatus;
// Re-read the renderer's state (after a menu changed the filter or scaling).
- (void)refresh;
// Size the window to its tallest tab, but no taller than maxHeight (0: the
// screen's visible frame); the tabs scroll.
- (void)fitToHeight:(CGFloat)maxHeight;
@property(nonatomic, readonly) NSTabView* tabView;
// Self-test the rendering controls and their enabled states.
- (BOOL)checkRenderingControls;
@end

NS_ASSUME_NONNULL_END
