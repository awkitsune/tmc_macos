#import <MetalKit/MetalKit.h>
#import <UIKit/UIKit.h>

#import "input.h"
#import "renderer.h"
#import "touchcontrols.h"

NS_ASSUME_NONNULL_BEGIN

// Why the game is paused; it runs while none applies.
typedef NS_OPTIONS(NSUInteger, PauseReason) {
    PauseReasonBackground = 1 << 0,   // the app is in the background
    PauseReasonInactive = 1 << 1,     // Control Center, a call, the app switcher
    PauseReasonSettings = 1 << 2,     // the settings sheet is open (and until its buttons are let go)
    PauseReasonNoController = 1 << 3, // no gamepad or keyboard to play with
    PauseReasonEditing = 1 << 4,      // the touch controls are being moved
};

/*
 * The game screen: the full-screen Metal view (status bar and home indicator
 * hidden), the game thread, sound and input. It pauses in the background,
 * while the settings are open and while no controller is connected (with an
 * overlay asking for one, unless the touch controls are on), and saves the
 * EEPROM file to disk when the app resigns active. The settings open from the
 * controller (Home, or holding ☰), Escape on a keyboard, and a tap on the
 * screen, or with the touch controls on, their ☰ button.
 */
@interface GameViewController : UIViewController

@property(nonatomic, readonly, nullable) Renderer* renderer;
@property(nonatomic, readonly) Input* input;
@property(nonatomic, readonly) MTKView* gameView;
// The on-screen controls (hidden while off, or while a gamepad is connected).
@property(nonatomic, readonly) TouchControlsView* touchControls;
@property(nonatomic, readonly) BOOL touchControlsOn;
@property(nonatomic, readonly) BOOL tapOpensSettings;
@property(nonatomic, readonly) PauseReason pauseReasons;
@property(nonatomic, readonly) BOOL controllerOverlayShown;
@property(nonatomic, readonly) BOOL settingsShown;
// The settings sheet (a UINavigationController), while shown.
@property(nonatomic, readonly, nullable) UIViewController* settingsController;

- (void)showSettings;
- (void)closeSettings;

// The touch controls' edit mode (Settings > Controls > Move Controls): the game
// pauses while the player drags the controls about.
- (void)editTouchControls;
- (void)endTouchEditing;
// The layout is the portrait one (the touch settings are kept per orientation).
@property(nonatomic, readonly) BOOL portrait;

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
