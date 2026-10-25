#import <UIKit/UIKit.h>

#import "input.h"
#import "touchlayout.h"

NS_ASSUME_NONNULL_BEGIN

// Settings > Controls > Touch: the on-screen controls (off by default) and their
// haptics (on by default).
extern NSString* const kTouchControlsDefault;
extern NSString* const kTouchHapticsDefault;
// Posted when either changes.
extern NSString* const kTouchControlsChangedNotification;

/*
 * The on-screen controls, drawn flat in code over the game view where the
 * layout (touchlayout.h) puts them, lit while pressed. Every touch is tracked
 * on its own: one that starts on the D-pad steers by its angle until it lifts,
 * one that starts on a button presses whatever button it is over as it slides
 * (A and B, both between them), and ☰ opens the settings when let go on it. The
 * keys held go to Input as one more key source. Haptics: a light tap on each
 * button press, a softer tick when the D-pad's direction changes.
 */
@interface TouchControlsView : UIView
- (instancetype)initWithInput:(Input*)input;
@property(nonatomic) TouchLayout layout;
@property(nonatomic) BOOL haptics;
@property(nonatomic, copy, nullable) void (^menuTapped)(void);
// The GBA keys held now, and the haptic taps and ticks played so far (self-test).
@property(nonatomic, readonly) uint16_t keys;
@property(nonatomic, readonly) NSUInteger hapticCount;
// The touch handlers' path, for one touch (any object standing for it) at a
// point in this view; the self-test presses through it too.
- (void)trackTouch:(id)touch at:(CGPoint)point phase:(UITouchPhase)phase;
// Let go of everything (the settings open, the app goes inactive).
- (void)releaseAll;
@end

NS_ASSUME_NONNULL_END
