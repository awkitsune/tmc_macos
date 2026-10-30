#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

/*
 * Where the on-screen touch controls and the picture go, as a pure function of
 * the screen (so the self-test can check every device's layout without one):
 *   landscape, as the original GBA: the picture in the middle column, the D-pad
 *     with Start and Select under it on the left, A and B on the right, L and R
 *     in the top corners, fast-forward and ☰ in the bottom right corner;
 *   portrait, as the GBA SP: the picture at the top, full width; under it L and
 *     R as bars, the D-pad left, A and B right, Start, Select and ☰ in the middle
 *     at the bottom, fast-forward in the bottom left corner.
 * Everything is in points, inside the safe area. With integer scaling the
 * picture's area is as large as a whole-number scale of the GBA screen that
 * leaves the controls room; without, as large as the controls leave.
 *
 * The controls come in clusters that move together (the player can drag them,
 * TouchLayoutOptions); each cluster's box in the layout is where a skin would
 * draw around it.
 */
typedef NS_ENUM(NSInteger, TouchControl) {
    TouchControlDpad,
    TouchControlA,
    TouchControlB,
    TouchControlL,
    TouchControlR,
    TouchControlStart,
    TouchControlSelect,
    TouchControlMenu,        // opens the settings
    TouchControlFastForward, // a tap switches fast-forward, holding holds it
    TouchControlCount,
};

typedef NS_ENUM(NSInteger, TouchCluster) {
    TouchClusterDpad,
    TouchClusterFace, // A and B
    TouchClusterL,
    TouchClusterR,
    TouchClusterStartSelect,
    TouchClusterMenu,
    TouchClusterFastForward,
    TouchClusterCount,
};

// The player's changes to the layout.
typedef struct {
    CGFloat size;                       // the controls' scale, 1 as designed
    CGPoint offsets[TouchClusterCount]; // each cluster moved from its place, in points
    BOOL integerScaling;                // the picture at a whole-number scale (the renderer's setting)
} TouchLayoutOptions;

extern const TouchLayoutOptions TouchLayoutDefaults; // size 1, nothing moved, integer scaling

typedef struct {
    BOOL portrait;
    CGRect picture;                     // the area the picture is fitted in
    CGRect controls[TouchControlCount]; // each control's touch target (the D-pad, A and B: their circle's square)
    CGFloat deadZone;                   // the D-pad's centre radius that presses nothing
    CGRect clusters[TouchClusterCount]; // each cluster's controls' bounding box
    CGPoint offsets[TouchClusterCount]; // how far each cluster was moved (kept inside the safe area)
} TouchLayout;

// The layout for a screen of this size with these safe area insets; pad: iPad
// button sizes; pixelsPerPoint: the screen's scale, for whole-pixel picture sizes;
// options: NULL for the defaults.
TouchLayout TouchLayoutMake(CGSize bounds, UIEdgeInsets safe, BOOL portrait, BOOL pad, CGFloat pixelsPerPoint,
                            const TouchLayoutOptions* _Nullable options);

// The control a touch that starts here takes, or TouchControlCount for none. The
// hit areas are a little larger than the controls.
TouchControl TouchLayoutHit(const TouchLayout* layout, CGPoint point);
// The D-pad's keys for a touch here: 8-way by the angle around its centre (anywhere,
// so a thumb that slides off still steers), nothing in the dead zone.
uint16_t TouchLayoutDpadKeys(const TouchLayout* layout, CGPoint point);
// The keys of the button here: A, B, both between them, L, R, Start or Select.
uint16_t TouchLayoutButtonKeys(const TouchLayout* layout, CGPoint point);

NSString* TouchControlName(TouchControl control);
uint16_t TouchControlKey(TouchControl control); // 0 for the D-pad, ☰ and fast-forward
TouchCluster TouchControlCluster(TouchControl control);
// A rect in points as whole drawable pixels.
CGRect TouchLayoutPixelRect(CGRect points, CGFloat pixelsPerPoint);

NS_ASSUME_NONNULL_END
