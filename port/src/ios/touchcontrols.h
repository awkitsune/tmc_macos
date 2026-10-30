#import <UIKit/UIKit.h>

#import "input.h"
#import "touchlayout.h"

NS_ASSUME_NONNULL_BEGIN

// Settings > Controls > Touch: the on-screen controls (off by default) and their
// haptics (on by default).
extern NSString* const kTouchControlsDefault;
extern NSString* const kTouchHapticsDefault;
// Posted when either changes, or the settings below.
extern NSString* const kTouchControlsChangedNotification;

// Settings > Controls > Touch, kept apart for portrait and landscape (in one
// dictionary each): the controls' opacity and size, and where the player moved
// their clusters (Move Controls).
typedef struct {
    CGFloat opacity;           // 0.1 to 1
    TouchLayoutOptions layout; // size and offsets; integerScaling is the renderer's, not kept here
} TouchSettings;

TouchSettings TouchSettingsLoad(BOOL portrait);
void TouchSettingsSave(BOOL portrait, const TouchSettings* settings);
void TouchSettingsReset(BOOL portrait);

/*
 * The on-screen controls, drawn flat in code over the game view where the
 * layout (touchlayout.h) puts them, lit while pressed. Every touch is tracked
 * on its own: one that starts on the D-pad steers by its angle until it lifts,
 * one that starts on a button presses whatever button it is over as it slides
 * (A and B, both between them), and ☰ opens the settings when let go on it. The
 * keys held go to Input as one more key source. Fast-forward switches Input's
 * fastForwardLatched (as the setting does) when tapped, and holds it while held.
 * Haptics: a light tap on each button press, a softer tick when the D-pad's
 * direction changes.
 *
 * In edit mode the controls press nothing: a finger drags the cluster it starts
 * on, and a bar at the top has Reset and Done.
 */
@interface TouchControlsView : UIView
- (instancetype)initWithInput:(Input*)input;
@property(nonatomic) TouchLayout layout;
@property(nonatomic) BOOL haptics;
@property(nonatomic) CGFloat opacity;
@property(nonatomic) BOOL editing;
// Edit mode: a cluster dragged to this offset (the owner relayouts), Reset and Done.
@property(nonatomic, copy, nullable) void (^clusterMoved)(TouchCluster cluster, CGPoint offset);
@property(nonatomic, copy, nullable) void (^editReset)(void);
@property(nonatomic, copy, nullable) void (^editDone)(void);
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
