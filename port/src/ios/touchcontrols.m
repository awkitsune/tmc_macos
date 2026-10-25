#import "touchcontrols.h"

#include "port/hw.h"

NSString* const kTouchControlsDefault = @"TouchControls";
NSString* const kTouchHapticsDefault = @"TouchHaptics";
NSString* const kTouchControlsChangedNotification = @"TMCTouchControlsChanged";

// One finger: the control it started on, and what it holds now.
@interface TrackedTouch : NSObject
@property(nonatomic) TouchControl control;
@property(nonatomic) uint16_t keys;
@property(nonatomic) BOOL onMenu; // ☰: still over it
@end

@implementation TrackedTouch
@end

// The D-pad's arms, lit by their key.
static const uint16_t kArmKeys[4] = { HW_KEY_UP, HW_KEY_DOWN, HW_KEY_LEFT, HW_KEY_RIGHT };

@implementation TouchControlsView {
    Input* _input;
    NSMapTable<id, TrackedTouch*>* _touches;
    CAShapeLayer* _shapes[TouchControlCount];
    CAShapeLayer* _arms[4];
    UILabel* _labels[TouchControlCount];
    UIImageView* _menuIcon;
    UIImpactFeedbackGenerator* _impact;
    UISelectionFeedbackGenerator* _selection;
    unsigned _lit; // controls drawn pressed, by bit
}

static CAShapeLayer* ShapeLayer(void) {
    CAShapeLayer* layer = [CAShapeLayer layer];
    // Presses show at once, not faded in.
    layer.actions = @{ @"fillColor" : NSNull.null, @"path" : NSNull.null, @"strokeColor" : NSNull.null };
    layer.lineWidth = 1.5;
    return layer;
}

static UIFont* RoundedFont(CGFloat size, UIFontWeight weight) {
    UIFont* font = [UIFont systemFontOfSize:size weight:weight];
    UIFontDescriptor* rounded = [font.fontDescriptor fontDescriptorWithDesign:UIFontDescriptorSystemDesignRounded];
    return rounded != nil ? [UIFont fontWithDescriptor:rounded size:size] : font;
}

- (instancetype)initWithInput:(Input*)input {
    if ((self = [super initWithFrame:CGRectZero]) != nil) {
        _input = input;
        _touches = [NSMapTable strongToStrongObjectsMapTable];
        self.multipleTouchEnabled = YES;
        self.backgroundColor = UIColor.clearColor;
        for (TouchControl control = 0; control < TouchControlCount; control++) {
            _shapes[control] = ShapeLayer();
            [self.layer addSublayer:_shapes[control]];
            if (control == TouchControlDpad) {
                for (int arm = 0; arm < 4; arm++) {
                    _arms[arm] = ShapeLayer();
                    _arms[arm].lineWidth = 0;
                    [self.layer addSublayer:_arms[arm]];
                }
            }
            if (control == TouchControlDpad || control == TouchControlMenu) {
                continue;
            }
            UILabel* label = [UILabel new];
            label.text = TouchControlName(control);
            label.textAlignment = NSTextAlignmentCenter;
            label.textColor = [UIColor colorWithWhite:1 alpha:0.8];
            BOOL small = control == TouchControlStart || control == TouchControlSelect;
            label.font = small ? RoundedFont(11, UIFontWeightBold) : RoundedFont(control <= TouchControlB ? 26 : 18, UIFontWeightBold);
            [self addSubview:label];
            _labels[control] = label;
        }
        _menuIcon = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"line.3.horizontal"]];
        _menuIcon.preferredSymbolConfiguration = [UIImageSymbolConfiguration configurationWithPointSize:17
                                                                                                 weight:UIImageSymbolWeightBold];
        _menuIcon.tintColor = [UIColor colorWithWhite:1 alpha:0.8];
        _menuIcon.contentMode = UIViewContentModeCenter;
        [self addSubview:_menuIcon];
        _impact = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight];
        _selection = [UISelectionFeedbackGenerator new];
        [self redraw];
    }
    return self;
}

#pragma mark Drawing

static UIBezierPath* Cross(CGRect rect) {
    CGFloat arm = rect.size.width / 3;
    CGFloat x0 = CGRectGetMinX(rect), x1 = x0 + arm, x2 = x1 + arm, x3 = CGRectGetMaxX(rect);
    CGFloat y0 = CGRectGetMinY(rect), y1 = y0 + arm, y2 = y1 + arm, y3 = CGRectGetMaxY(rect);
    UIBezierPath* path = [UIBezierPath bezierPath];
    const CGPoint points[] = {
        { x1, y0 }, { x2, y0 }, { x2, y1 }, { x3, y1 }, { x3, y2 }, { x2, y2 },
        { x2, y3 }, { x1, y3 }, { x1, y2 }, { x0, y2 }, { x0, y1 }, { x1, y1 },
    };
    [path moveToPoint:points[0]];
    for (size_t i = 1; i < sizeof(points) / sizeof(points[0]); i++) {
        [path addLineToPoint:points[i]];
    }
    [path closePath];
    return path;
}

// The arm of the cross towards up, down, left or right.
static CGRect Arm(CGRect rect, int arm) {
    CGFloat width = rect.size.width / 3, cx = CGRectGetMidX(rect), cy = CGRectGetMidY(rect);
    switch (arm) {
        case 0: return CGRectMake(cx - width / 2, CGRectGetMinY(rect), width, width);
        case 1: return CGRectMake(cx - width / 2, CGRectGetMaxY(rect) - width, width, width);
        case 2: return CGRectMake(CGRectGetMinX(rect), cy - width / 2, width, width);
        default: return CGRectMake(CGRectGetMaxX(rect) - width, cy - width / 2, width, width);
    }
}

// The drawn shape: the hit target, or smaller for Start, Select and ☰.
static UIBezierPath* Shape(TouchControl control, CGRect rect) {
    switch (control) {
        case TouchControlDpad: return Cross(CGRectInset(rect, 4, 4));
        case TouchControlA:
        case TouchControlB: return [UIBezierPath bezierPathWithOvalInRect:rect];
        case TouchControlStart:
        case TouchControlSelect: {
            CGRect capsule = CGRectInset(rect, 4, (rect.size.height - 28) / 2);
            return [UIBezierPath bezierPathWithRoundedRect:capsule cornerRadius:capsule.size.height / 2];
        }
        case TouchControlMenu: return [UIBezierPath bezierPathWithOvalInRect:CGRectInset(rect, 3, 3)];
        default: return [UIBezierPath bezierPathWithRoundedRect:rect cornerRadius:12];
    }
}

- (void)setLayout:(TouchLayout)layout {
    _layout = layout;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    for (TouchControl control = 0; control < TouchControlCount; control++) {
        CGRect rect = layout.controls[control];
        _shapes[control].path = Shape(control, rect).CGPath;
        _labels[control].frame = rect;
    }
    CGRect dpad = CGRectInset(layout.controls[TouchControlDpad], 4, 4);
    for (int arm = 0; arm < 4; arm++) {
        _arms[arm].path = [UIBezierPath bezierPathWithRect:Arm(dpad, arm)].CGPath;
    }
    [CATransaction commit];
    _menuIcon.frame = layout.controls[TouchControlMenu];
}

- (void)redraw {
    CGColorRef fill = [UIColor colorWithWhite:1 alpha:0.14].CGColor, lit = [UIColor colorWithWhite:1 alpha:0.45].CGColor;
    CGColorRef stroke = [UIColor colorWithWhite:1 alpha:0.32].CGColor;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    for (TouchControl control = 0; control < TouchControlCount; control++) {
        BOOL on = control != TouchControlDpad && (_lit & (1u << control)) != 0;
        _shapes[control].fillColor = on ? lit : fill;
        _shapes[control].strokeColor = stroke;
    }
    for (int arm = 0; arm < 4; arm++) {
        _arms[arm].fillColor = (_keys & kArmKeys[arm]) ? lit : UIColor.clearColor.CGColor;
    }
    [CATransaction commit];
}

#pragma mark Touches

- (void)prepareHaptics {
    if (_haptics) {
        [_impact prepare];
        [_selection prepare];
    }
}

- (void)tap:(BOOL)soft {
    if (!_haptics) {
        return;
    }
    _hapticCount++;
    if (soft) {
        [_selection selectionChanged];
        [_selection prepare];
    } else {
        [_impact impactOccurred];
        [_impact prepare];
    }
}

- (void)trackTouch:(id)touch at:(CGPoint)point phase:(UITouchPhase)phase {
    TrackedTouch* tracked = [_touches objectForKey:touch];
    if (phase == UITouchPhaseBegan) {
        TouchControl control = TouchLayoutHit(&_layout, point);
        if (control == TouchControlCount) {
            return;
        }
        [self prepareHaptics];
        tracked = [TrackedTouch new];
        tracked.control = control;
        [_touches setObject:tracked forKey:touch];
    }
    if (tracked == nil) {
        return;
    }
    uint16_t before = tracked.keys;
    BOOL wasOnMenu = tracked.onMenu;
    BOOL menuTapped = NO;
    if (phase == UITouchPhaseEnded || phase == UITouchPhaseCancelled) {
        menuTapped = phase == UITouchPhaseEnded && tracked.control == TouchControlMenu &&
                     TouchLayoutHit(&_layout, point) == TouchControlMenu;
        tracked.keys = 0;
        tracked.onMenu = NO;
        [_touches removeObjectForKey:touch];
    } else if (tracked.control == TouchControlDpad) {
        tracked.keys = TouchLayoutDpadKeys(&_layout, point);
    } else if (tracked.control == TouchControlMenu) {
        tracked.onMenu = TouchLayoutHit(&_layout, point) == TouchControlMenu;
    } else {
        tracked.keys = TouchLayoutButtonKeys(&_layout, point);
    }
    // Nothing on release; a tick for each new D-pad direction, a tap for a new button.
    if (tracked.control == TouchControlDpad) {
        if (tracked.keys != before && tracked.keys != 0) {
            [self tap:YES];
        }
    } else if ((tracked.keys & ~before) != 0 || (tracked.onMenu && !wasOnMenu)) {
        [self tap:NO];
    }
    [self update];
    if (menuTapped && self.menuTapped) {
        self.menuTapped();
    }
}

// What every finger holds, to Input and the drawing.
- (void)update {
    uint16_t keys = 0;
    unsigned lit = 0;
    for (id touch in _touches) {
        TrackedTouch* tracked = [_touches objectForKey:touch];
        keys |= tracked.keys;
        if (tracked.onMenu) {
            lit |= 1u << TouchControlMenu;
        }
    }
    for (TouchControl control = 0; control < TouchControlCount; control++) {
        uint16_t key = TouchControlKey(control);
        if (key != 0 && (keys & key) != 0) {
            lit |= 1u << control;
        }
    }
    if (keys != _keys) {
        _keys = keys;
        [_input setTouchKeys:keys];
    }
    _lit = lit;
    [self redraw];
}

- (void)releaseAll {
    [_touches removeAllObjects];
    [self update];
}

- (void)setHidden:(BOOL)hidden {
    [super setHidden:hidden];
    if (hidden) {
        [self releaseAll];
    } else {
        [self prepareHaptics];
    }
}

- (void)track:(NSSet<UITouch*>*)touches phase:(UITouchPhase)phase {
    for (UITouch* touch in touches) {
        [self trackTouch:touch at:[touch locationInView:self] phase:phase];
    }
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    [self track:touches phase:UITouchPhaseBegan];
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    [self track:touches phase:UITouchPhaseMoved];
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    [self track:touches phase:UITouchPhaseEnded];
}

- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    [self track:touches phase:UITouchPhaseCancelled];
}

@end
