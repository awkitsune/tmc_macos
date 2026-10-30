#import "touchlayout.h"

#include "port/hw.h"

#define GBA_WIDTH 240
#define GBA_HEIGHT 160

// Button sizes in points: thumb-sized on either device, a little larger on iPad.
typedef struct {
    CGFloat margin;          // from the safe area's edges
    CGFloat gap;             // between groups of controls
    CGFloat dpad;            // the D-pad's diameter
    CGFloat face;            // A and B's diameter
    CGFloat faceDX, faceDY;  // B's centre from A's: left and down
    CGSize shoulder;         // L and R in landscape
    CGFloat shoulderBar;     // L and R's width in portrait
    CGSize start;            // Start and Select
    CGFloat startGap;        // between them
    CGFloat menu;            // ☰
} Sizes;

static const Sizes kPhone = {
    .margin = 12, .gap = 16, .dpad = 132, .face = 64, .faceDX = 68, .faceDY = 36,
    .shoulder = { 88, 44 }, .shoulderBar = 140, .start = { 76, 44 }, .startGap = 8, .menu = 44,
};
static const Sizes kPad = {
    .margin = 20, .gap = 20, .dpad = 150, .face = 72, .faceDX = 78, .faceDY = 40,
    .shoulder = { 110, 48 }, .shoulderBar = 180, .start = { 84, 44 }, .startGap = 10, .menu = 48,
};

static CGRect Circle(CGPoint centre, CGFloat diameter) {
    return CGRectMake(centre.x - diameter / 2, centre.y - diameter / 2, diameter, diameter);
}

// The largest whole-number scale of the GBA screen that fits (at least 1).
static int WholeScale(CGFloat width, CGFloat height, CGFloat pixelsPerPoint) {
    int scale = (int)floor(MIN(width * pixelsPerPoint / GBA_WIDTH, height * pixelsPerPoint / GBA_HEIGHT));
    return MAX(scale, 1);
}

const TouchLayoutOptions TouchLayoutDefaults = { .size = 1, .integerScaling = YES };

// The sizes at the player's scale (the margins and gaps stay).
static Sizes Scaled(const Sizes* s, CGFloat k) {
    Sizes scaled = *s;
    scaled.dpad *= k;
    scaled.face *= k;
    scaled.faceDX *= k;
    scaled.faceDY *= k;
    scaled.shoulder = CGSizeMake(s->shoulder.width * k, s->shoulder.height * k);
    scaled.shoulderBar *= k;
    scaled.start = CGSizeMake(s->start.width * k, s->start.height * k);
    scaled.startGap *= k;
    scaled.menu *= k;
    return scaled;
}

static void Landscape(TouchLayout* layout, CGRect safe, const Sizes* s, CGFloat pixelsPerPoint, BOOL integerScaling) {
    // At the narrowest, the controls keep a margin from the edge and half that from the picture.
    CGFloat column = MAX(s->dpad, s->faceDX + s->face) + 1.5 * s->margin;
    CGFloat width;
    if (integerScaling) {
        width = GBA_WIDTH * WholeScale(safe.size.width - 2 * column, safe.size.height, pixelsPerPoint) / pixelsPerPoint;
    } else {
        width = MIN(safe.size.width - 2 * column, safe.size.height * GBA_WIDTH / GBA_HEIGHT);
        width = MAX(floor(width * pixelsPerPoint) / pixelsPerPoint, GBA_WIDTH / pixelsPerPoint);
    }
    column = (safe.size.width - width) / 2;
    layout->picture = CGRectMake(CGRectGetMinX(safe) + column, CGRectGetMinY(safe), width, safe.size.height);

    CGFloat top = CGRectGetMinY(safe) + s->margin, bottom = CGRectGetMaxY(safe) - s->margin;
    CGFloat left = CGRectGetMinX(safe), right = CGRectGetMaxX(safe);
    CGFloat leftCentre = left + column / 2, rightCentre = right - column / 2;
    layout->controls[TouchControlL] = CGRectMake(left + s->margin, top, s->shoulder.width, s->shoulder.height);
    layout->controls[TouchControlR] =
        CGRectMake(right - s->margin - s->shoulder.width, top, s->shoulder.width, s->shoulder.height);

    // The D-pad with Start and Select under it, centred in what is left of the column.
    CGFloat regionTop = top + s->shoulder.height + s->gap;
    CGFloat stack = s->dpad + s->gap + 2 * s->start.height + s->startGap;
    CGFloat stackTop = regionTop + MAX(0, (bottom - regionTop - stack) / 2);
    CGPoint dpad = CGPointMake(leftCentre, stackTop + s->dpad / 2);
    layout->controls[TouchControlDpad] = Circle(dpad, s->dpad);
    CGFloat startTop = stackTop + s->dpad + s->gap;
    layout->controls[TouchControlStart] =
        CGRectMake(leftCentre - s->start.width / 2, startTop, s->start.width, s->start.height);
    layout->controls[TouchControlSelect] = CGRectMake(leftCentre - s->start.width / 2,
                                                      startTop + s->start.height + s->startGap, s->start.width, s->start.height);

    // A and B level with the D-pad, B lower left of A.
    layout->controls[TouchControlA] = Circle(CGPointMake(rightCentre + s->faceDX / 2, dpad.y - s->faceDY / 2), s->face);
    layout->controls[TouchControlB] = Circle(CGPointMake(rightCentre - s->faceDX / 2, dpad.y + s->faceDY / 2), s->face);
    layout->controls[TouchControlMenu] = CGRectMake(right - s->margin - s->menu, bottom - s->menu, s->menu, s->menu);
    layout->controls[TouchControlFastForward] =
        CGRectMake(right - s->margin - 2 * s->menu - s->startGap, bottom - s->menu, s->menu, s->menu);
}

// The screen in halves: the picture centred in the top one, the controls in the
// bottom one (or more of the screen, if they need it).
static void Portrait(TouchLayout* layout, CGRect safe, const Sizes* s, CGFloat screenHeight) {
    CGFloat controls = 2 * s->margin + s->shoulder.height + 2 * s->gap + MAX(s->dpad, s->faceDY + s->face) + s->menu +
                       s->margin + s->start.height;
    CGFloat split = MAX(MIN(screenHeight / 2, CGRectGetMaxY(safe) - controls), CGRectGetMinY(safe));
    layout->picture = CGRectMake(CGRectGetMinX(safe), CGRectGetMinY(safe), safe.size.width, split - CGRectGetMinY(safe));

    CGFloat left = CGRectGetMinX(safe) + s->margin, right = CGRectGetMaxX(safe) - s->margin;
    CGFloat top = split + s->margin, bottom = CGRectGetMaxY(safe) - s->margin;
    CGFloat centre = CGRectGetMidX(safe);
    CGFloat bar = MIN(s->shoulderBar, (right - left - s->gap) / 2);
    layout->controls[TouchControlL] = CGRectMake(left, top, bar, s->shoulder.height);
    layout->controls[TouchControlR] = CGRectMake(right - bar, top, bar, s->shoulder.height);

    // Select and Start side by side at the bottom, ☰ above them.
    CGFloat row = bottom - s->start.height;
    layout->controls[TouchControlSelect] =
        CGRectMake(centre - s->startGap / 2 - s->start.width, row, s->start.width, s->start.height);
    layout->controls[TouchControlStart] = CGRectMake(centre + s->startGap / 2, row, s->start.width, s->start.height);
    layout->controls[TouchControlMenu] = CGRectMake(centre - s->menu / 2, row - s->margin - s->menu, s->menu, s->menu);
    layout->controls[TouchControlFastForward] = CGRectMake(left, bottom - s->menu, s->menu, s->menu);

    // The D-pad and A and B in between.
    CGFloat regionTop = top + s->shoulder.height + s->gap, regionBottom = row - s->gap;
    CGFloat middle = (regionTop + regionBottom) / 2;
    layout->controls[TouchControlDpad] = Circle(CGPointMake(left + s->dpad / 2, middle), s->dpad);
    CGFloat faceCentre = right - (s->faceDX + s->face) / 2;
    layout->controls[TouchControlA] = Circle(CGPointMake(faceCentre + s->faceDX / 2, middle - s->faceDY / 2), s->face);
    layout->controls[TouchControlB] = Circle(CGPointMake(faceCentre - s->faceDX / 2, middle + s->faceDY / 2), s->face);
}

// Each cluster moved by its offset, but no further than the safe area's edges.
static void Move(TouchLayout* layout, CGRect safe, const CGPoint offsets[TouchClusterCount]) {
    for (TouchCluster cluster = 0; cluster < TouchClusterCount; cluster++) {
        CGRect box = CGRectNull;
        for (TouchControl control = 0; control < TouchControlCount; control++) {
            if (TouchControlCluster(control) == cluster) {
                box = CGRectUnion(box, layout->controls[control]);
            }
        }
        CGFloat dx = MAX(MIN(offsets[cluster].x, CGRectGetMaxX(safe) - CGRectGetMaxX(box)), CGRectGetMinX(safe) - CGRectGetMinX(box));
        CGFloat dy = MAX(MIN(offsets[cluster].y, CGRectGetMaxY(safe) - CGRectGetMaxY(box)), CGRectGetMinY(safe) - CGRectGetMinY(box));
        for (TouchControl control = 0; control < TouchControlCount; control++) {
            if (TouchControlCluster(control) == cluster) {
                layout->controls[control] = CGRectOffset(layout->controls[control], dx, dy);
            }
        }
        layout->clusters[cluster] = CGRectOffset(box, dx, dy);
        layout->offsets[cluster] = CGPointMake(dx, dy);
    }
}

TouchLayout TouchLayoutMake(CGSize bounds, UIEdgeInsets safe, BOOL portrait, BOOL pad, CGFloat pixelsPerPoint,
                            const TouchLayoutOptions* options) {
    TouchLayout layout = { .portrait = portrait };
    if (options == NULL) {
        options = &TouchLayoutDefaults;
    }
    Sizes s = Scaled(pad ? &kPad : &kPhone, MAX(0.5, MIN(options->size, 2)));
    CGRect area = UIEdgeInsetsInsetRect(CGRectMake(0, 0, bounds.width, bounds.height), safe);
    pixelsPerPoint = MAX(pixelsPerPoint, 1);
    if (portrait) {
        Portrait(&layout, area, &s, bounds.height);
    } else {
        Landscape(&layout, area, &s, pixelsPerPoint, options->integerScaling);
    }
    Move(&layout, area, options->offsets);
    layout.deadZone = 0.18 * s.dpad / 2;
    return layout;
}

#pragma mark Hit-testing

static CGPoint Centre(CGRect rect) {
    return CGPointMake(CGRectGetMidX(rect), CGRectGetMidY(rect));
}

static BOOL IsRound(TouchControl control) {
    return control == TouchControlDpad || control == TouchControlA || control == TouchControlB;
}

// How far the point is outside the control (0 or less: on it).
static CGFloat Distance(const TouchLayout* layout, TouchControl control, CGPoint point) {
    CGRect rect = layout->controls[control];
    if (IsRound(control)) {
        CGPoint c = Centre(rect);
        return hypot(point.x - c.x, point.y - c.y) - rect.size.width / 2;
    }
    CGFloat dx = MAX(MAX(CGRectGetMinX(rect) - point.x, point.x - CGRectGetMaxX(rect)), 0);
    CGFloat dy = MAX(MAX(CGRectGetMinY(rect) - point.y, point.y - CGRectGetMaxY(rect)), 0);
    return hypot(dx, dy);
}

// The extra reach of the hit areas: a fifth of the smaller side, at least 12 pt.
static CGFloat Slop(const TouchLayout* layout, TouchControl control) {
    CGRect rect = layout->controls[control];
    return MAX(12, MIN(rect.size.width, rect.size.height) / 5);
}

// The nearest control within its reach, of those in the mask.
static TouchControl Nearest(const TouchLayout* layout, CGPoint point, unsigned mask) {
    TouchControl best = TouchControlCount;
    CGFloat bestDistance = CGFLOAT_MAX;
    for (TouchControl control = 0; control < TouchControlCount; control++) {
        if ((mask & (1u << control)) == 0) {
            continue;
        }
        CGFloat distance = Distance(layout, control, point);
        if (distance <= Slop(layout, control) && distance < bestDistance) {
            best = control;
            bestDistance = distance;
        }
    }
    return best;
}

TouchControl TouchLayoutHit(const TouchLayout* layout, CGPoint point) {
    return Nearest(layout, point, ~0u);
}

uint16_t TouchLayoutDpadKeys(const TouchLayout* layout, CGPoint point) {
    static const uint16_t sectors[8] = {
        HW_KEY_RIGHT, HW_KEY_RIGHT | HW_KEY_UP,  HW_KEY_UP,   HW_KEY_UP | HW_KEY_LEFT,
        HW_KEY_LEFT,  HW_KEY_LEFT | HW_KEY_DOWN, HW_KEY_DOWN, HW_KEY_DOWN | HW_KEY_RIGHT,
    };
    CGPoint c = Centre(layout->controls[TouchControlDpad]);
    CGFloat dx = point.x - c.x, dy = c.y - point.y; // up is positive
    if (hypot(dx, dy) < layout->deadZone) {
        return 0;
    }
    int sector = (int)lround(atan2(dy, dx) / (M_PI / 4));
    return sectors[(sector + 8) % 8];
}

uint16_t TouchLayoutButtonKeys(const TouchLayout* layout, CGPoint point) {
    if (Distance(layout, TouchControlA, point) <= 0) {
        return HW_KEY_A;
    }
    if (Distance(layout, TouchControlB, point) <= 0) {
        return HW_KEY_B;
    }
    // The gap between A and B presses both.
    CGPoint a = Centre(layout->controls[TouchControlA]), b = Centre(layout->controls[TouchControlB]);
    CGFloat ax = a.x - b.x, ay = a.y - b.y, length = hypot(ax, ay);
    CGFloat along = ((point.x - b.x) * ax + (point.y - b.y) * ay) / length;
    CGFloat across = fabs((point.x - b.x) * ay - (point.y - b.y) * ax) / length;
    if (along > 0 && along < length && across <= 0.4 * layout->controls[TouchControlA].size.width) {
        return HW_KEY_A | HW_KEY_B;
    }
    unsigned buttons = 1u << TouchControlA | 1u << TouchControlB | 1u << TouchControlL | 1u << TouchControlR |
                       1u << TouchControlStart | 1u << TouchControlSelect;
    TouchControl control = Nearest(layout, point, buttons);
    return control == TouchControlCount ? 0 : TouchControlKey(control);
}

NSString* TouchControlName(TouchControl control) {
    static NSString* const names[TouchControlCount] = { @"D-pad", @"A", @"B", @"L", @"R", @"START", @"SELECT", @"☰", @"Fast-forward" };
    return control < TouchControlCount ? names[control] : @"none";
}

uint16_t TouchControlKey(TouchControl control) {
    static const uint16_t keys[TouchControlCount] = { 0, HW_KEY_A, HW_KEY_B, HW_KEY_L, HW_KEY_R, HW_KEY_START, HW_KEY_SELECT, 0, 0 };
    return control < TouchControlCount ? keys[control] : 0;
}

TouchCluster TouchControlCluster(TouchControl control) {
    static const TouchCluster clusters[TouchControlCount] = {
        TouchClusterDpad,        TouchClusterFace,        TouchClusterFace, TouchClusterL,          TouchClusterR,
        TouchClusterStartSelect, TouchClusterStartSelect, TouchClusterMenu, TouchClusterFastForward,
    };
    return control < TouchControlCount ? clusters[control] : TouchClusterCount;
}

CGRect TouchLayoutPixelRect(CGRect points, CGFloat pixelsPerPoint) {
    CGFloat x = round(CGRectGetMinX(points) * pixelsPerPoint), y = round(CGRectGetMinY(points) * pixelsPerPoint);
    return CGRectMake(x, y, round(CGRectGetMaxX(points) * pixelsPerPoint) - x, round(CGRectGetMaxY(points) * pixelsPerPoint) - y);
}
