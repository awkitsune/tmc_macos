#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

/*
 * Where the on-screen touch controls and the picture go, as a pure function of
 * the screen (so the self-test can check every device's layout without one):
 *   landscape, as the original GBA: the picture in the middle column, the D-pad
 *     with Start and Select under it on the left, A and B on the right, L and R
 *     in the top corners, ☰ in the bottom right corner;
 *   portrait, as the GBA SP: the picture at the top, full width; under it L and
 *     R as bars, the D-pad left, A and B right, Start, Select and ☰ in the middle
 *     at the bottom.
 * Everything is in points, inside the safe area; the picture's area is as large
 * as a whole-number scale of the GBA screen that leaves the controls room.
 */
typedef NS_ENUM(NSInteger, TouchControl) {
    TouchControlDpad,
    TouchControlA,
    TouchControlB,
    TouchControlL,
    TouchControlR,
    TouchControlStart,
    TouchControlSelect,
    TouchControlMenu, // opens the settings
    TouchControlCount,
};

typedef struct {
    BOOL portrait;
    CGRect picture;                     // the area the picture is fitted in
    CGRect controls[TouchControlCount]; // each control's touch target (the D-pad, A and B: their circle's square)
    CGFloat deadZone;                   // the D-pad's centre radius that presses nothing
} TouchLayout;

// The layout for a screen of this size with these safe area insets; pad: iPad
// button sizes; pixelsPerPoint: the screen's scale, for whole-pixel picture sizes.
TouchLayout TouchLayoutMake(CGSize bounds, UIEdgeInsets safe, BOOL portrait, BOOL pad, CGFloat pixelsPerPoint);

// The control a touch that starts here takes, or TouchControlCount for none. The
// hit areas are a little larger than the controls.
TouchControl TouchLayoutHit(const TouchLayout* layout, CGPoint point);
// The D-pad's keys for a touch here: 8-way by the angle around its centre (anywhere,
// so a thumb that slides off still steers), nothing in the dead zone.
uint16_t TouchLayoutDpadKeys(const TouchLayout* layout, CGPoint point);
// The keys of the button here: A, B, both between them, L, R, Start or Select.
uint16_t TouchLayoutButtonKeys(const TouchLayout* layout, CGPoint point);

NSString* TouchControlName(TouchControl control);
uint16_t TouchControlKey(TouchControl control); // 0 for the D-pad and ☰
// A rect in points as whole drawable pixels.
CGRect TouchLayoutPixelRect(CGRect points, CGFloat pixelsPerPoint);

NS_ASSUME_NONNULL_END
