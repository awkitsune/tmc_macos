#import <MetalKit/MetalKit.h>
#import <UIKit/UIKit.h>

#import "input.h"
#import "renderer.h"

NS_ASSUME_NONNULL_BEGIN

// Why the game is paused; it runs while none applies.
typedef NS_OPTIONS(NSUInteger, PauseReason) {
    PauseReasonBackground = 1 << 0,   // the app is in the background
    PauseReasonInactive = 1 << 1,     // Control Center, a call, the app switcher
    PauseReasonSettings = 1 << 2,     // the settings sheet is open (and until its buttons are let go)
    PauseReasonNoController = 1 << 3, // no gamepad or keyboard to play with
};

/*
 * The game screen: the full-screen Metal view (status bar and home indicator
 * hidden), the game thread, sound and input. It pauses in the background,
 * while the settings are open and while no controller is connected (with an
 * overlay asking for one), and saves the EEPROM file to disk when the app
 * resigns active. The settings open from the controller (Home, or holding ☰),
 * Escape on a keyboard, or a tap on the screen, the only touch input.
 */
@interface GameViewController : UIViewController

@property(nonatomic, readonly, nullable) Renderer* renderer;
@property(nonatomic, readonly) Input* input;
@property(nonatomic, readonly) MTKView* gameView;
@property(nonatomic, readonly) PauseReason pauseReasons;
@property(nonatomic, readonly) BOOL controllerOverlayShown;
@property(nonatomic, readonly) BOOL settingsShown;
// The settings sheet (a UINavigationController), while shown.
@property(nonatomic, readonly, nullable) UIViewController* settingsController;

- (void)showSettings;
- (void)closeSettings;

// Scene life cycle.
- (void)sceneWillResignActive;
- (void)sceneDidBecomeActive;
- (void)sceneDidEnterBackground;
- (void)sceneWillEnterForeground;

// ~/Documents/tmc.sav, the save the game writes through to (8 KB EEPROM image,
// as the macOS app's and emulators'), and ~/Documents/states for save states.
@property(class, readonly) NSString* savePath;
@property(class, readonly) NSString* stateFolder;

// How the game follows the display (Settings > Timing).
- (NSString*)pacingStatus;

// A message over the game for a few seconds ("Saved slot 2").
- (void)showStatus:(NSString*)text;

@end

NS_ASSUME_NONNULL_END
