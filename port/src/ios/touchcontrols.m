#import "touchcontrols.h"

#include "port/hw.h"

NSString* const kTouchControlsDefault = @"TouchControls";
NSString* const kTouchHapticsDefault = @"TouchHaptics";
NSString* const kTouchControlsChangedNotification = @"TMCTouchControlsChanged";

// Held this long, fast-forward is held rather than switched.
static const CFTimeInterval kFastForwardHold = 0.35;

static NSString* SettingsKey(BOOL portrait) {
    return portrait ? @"TouchLayoutPortrait" : @"TouchLayoutLandscape";
}

TouchSettings TouchSettingsLoad(BOOL portrait) {
    TouchSettings settings = { .opacity = 1, .layout = TouchLayoutDefaults };
    NSDictionary* saved = [NSUserDefaults.standardUserDefaults dictionaryForKey:SettingsKey(portrait)];
    NSNumber* opacity = saved[@"Opacity"];
    NSNumber* size = saved[@"Size"];
    NSArray* offsets = saved[@"Offsets"];
    if ([opacity isKindOfClass:NSNumber.class]) {
        settings.opacity = MAX(0.1, MIN(opacity.doubleValue, 1));
    }
    if ([size isKindOfClass:NSNumber.class]) {
        settings.layout.size = MAX(0.5, MIN(size.doubleValue, 2));
    }
    if ([offsets isKindOfClass:NSArray.class] && offsets.count == 2 * TouchClusterCount) {
        for (NSUInteger i = 0; i < TouchClusterCount; i++) {
            NSNumber* x = offsets[2 * i];
            NSNumber* y = offsets[2 * i + 1];
            if ([x isKindOfClass:NSNumber.class] && [y isKindOfClass:NSNumber.class]) {
                settings.layout.offsets[i] = CGPointMake(x.doubleValue, y.doubleValue);
            }
        }
    }
    return settings;
}

void TouchSettingsSave(BOOL portrait, const TouchSettings* settings) {
    NSMutableArray* offsets = [NSMutableArray array];
    for (NSUInteger i = 0; i < TouchClusterCount; i++) {
        [offsets addObject:@(round(settings->layout.offsets[i].x))];
        [offsets addObject:@(round(settings->layout.offsets[i].y))];
    }
    [NSUserDefaults.standardUserDefaults
        setObject:@{ @"Opacity" : @(settings->opacity), @"Size" : @(settings->layout.size), @"Offsets" : offsets }
           forKey:SettingsKey(portrait)];
}

void TouchSettingsReset(BOOL portrait) {
    [NSUserDefaults.standardUserDefaults removeObjectForKey:SettingsKey(portrait)];
}

// One finger: the control it started on, and what it holds now.
@interface TrackedTouch : NSObject
@property(nonatomic) TouchControl control;
@property(nonatomic) uint16_t keys;
@property(nonatomic) BOOL onMenu;          // ☰: still over it
@property(nonatomic) BOOL wasFastForward;  // fast-forward: switched on before this press
@property(nonatomic) CFTimeInterval began; // fast-forward: when pressed
@property(nonatomic) CGPoint start;        // edit mode: where the drag began
@property(nonatomic) CGPoint offset;       // edit mode: the cluster's offset then
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
    UIImageView* _fastForwardIcon;
    UIView* _board;                           // the controls, at the player's opacity
    CAShapeLayer* _outlines[TouchClusterCount]; // edit mode
    UIStackView* _editBar;
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
        _opacity = 1;
        _board = [[UIView alloc] initWithFrame:self.bounds];
        _board.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
        _board.userInteractionEnabled = NO;
        [self addSubview:_board];
        for (TouchControl control = 0; control < TouchControlCount; control++) {
            _shapes[control] = ShapeLayer();
            [_board.layer addSublayer:_shapes[control]];
            if (control == TouchControlDpad) {
                for (int arm = 0; arm < 4; arm++) {
                    _arms[arm] = ShapeLayer();
                    _arms[arm].lineWidth = 0;
                    [_board.layer addSublayer:_arms[arm]];
                }
            }
            if (control == TouchControlDpad || control == TouchControlMenu || control == TouchControlFastForward) {
                continue;
            }
            UILabel* label = [UILabel new];
            label.text = TouchControlName(control);
            label.textAlignment = NSTextAlignmentCenter;
            label.textColor = [UIColor colorWithWhite:1 alpha:0.8];
            BOOL small = control == TouchControlStart || control == TouchControlSelect;
            label.font = small ? RoundedFont(11, UIFontWeightBold) : RoundedFont(control <= TouchControlB ? 26 : 18, UIFontWeightBold);
            [_board addSubview:label];
            _labels[control] = label;
        }
        _menuIcon = [self iconNamed:@"line.3.horizontal"];
        _fastForwardIcon = [self iconNamed:@"forward.fill"];
        for (TouchCluster cluster = 0; cluster < TouchClusterCount; cluster++) {
            _outlines[cluster] = ShapeLayer();
            _outlines[cluster].fillColor = UIColor.clearColor.CGColor;
            _outlines[cluster].strokeColor = UIColor.systemYellowColor.CGColor;
            _outlines[cluster].lineDashPattern = @[ @6, @4 ];
            _outlines[cluster].hidden = YES;
            [self.layer addSublayer:_outlines[cluster]];
        }
        [self buildEditBar];
        _impact = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight];
        _selection = [UISelectionFeedbackGenerator new];
        [self redraw];
    }
    return self;
}

- (UIImageView*)iconNamed:(NSString*)name {
    UIImageView* icon = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:name]];
    icon.preferredSymbolConfiguration = [UIImageSymbolConfiguration configurationWithPointSize:17
                                                                                        weight:UIImageSymbolWeightBold];
    icon.tintColor = [UIColor colorWithWhite:1 alpha:0.8];
    icon.contentMode = UIViewContentModeCenter;
    [_board addSubview:icon];
    return icon;
}

- (UIButton*)editButton:(NSString*)title prominent:(BOOL)prominent action:(SEL)action {
    UIButtonConfiguration* config =
        prominent ? [UIButtonConfiguration filledButtonConfiguration] : [UIButtonConfiguration grayButtonConfiguration];
    config.title = title;
    config.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
    UIButton* button = [UIButton buttonWithConfiguration:config primaryAction:nil];
    [button addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
    return button;
}

// Reset and Done at the top, between the hints.
- (void)buildEditBar {
    UILabel* hint = [UILabel new];
    hint.text = @"Drag the controls";
    hint.textColor = UIColor.whiteColor;
    hint.font = RoundedFont(15, UIFontWeightSemibold);
    _editBar = [[UIStackView alloc] initWithArrangedSubviews:@[
        [self editButton:@"Reset" prominent:NO action:@selector(resetTapped)], hint,
        [self editButton:@"Done" prominent:YES action:@selector(doneTapped)]
    ]];
    _editBar.spacing = 12;
    _editBar.alignment = UIStackViewAlignmentCenter;
    _editBar.hidden = YES;
    _editBar.translatesAutoresizingMaskIntoConstraints = NO;
    [self addSubview:_editBar];
    [NSLayoutConstraint activateConstraints:@[
        [_editBar.centerXAnchor constraintEqualToAnchor:self.safeAreaLayoutGuide.centerXAnchor],
        [_editBar.topAnchor constraintEqualToAnchor:self.safeAreaLayoutGuide.topAnchor constant:12],
    ]];
}

- (void)resetTapped {
    if (self.editReset) {
        self.editReset();
    }
}

- (void)doneTapped {
    if (self.editDone) {
        self.editDone();
    }
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
        case TouchControlMenu:
        case TouchControlFastForward: return [UIBezierPath bezierPathWithOvalInRect:CGRectInset(rect, 3, 3)];
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
    for (TouchCluster cluster = 0; cluster < TouchClusterCount; cluster++) {
        _outlines[cluster].path =
            [UIBezierPath bezierPathWithRoundedRect:CGRectInset(layout.clusters[cluster], -4, -4) cornerRadius:10].CGPath;
    }
    [CATransaction commit];
    _menuIcon.frame = layout.controls[TouchControlMenu];
    _fastForwardIcon.frame = layout.controls[TouchControlFastForward];
    [self redraw];
}

- (void)setOpacity:(CGFloat)opacity {
    _opacity = opacity;
    _board.alpha = _editing ? MAX(opacity, 0.6) : opacity;
}

- (void)setEditing:(BOOL)editing {
    if (editing == _editing) {
        return;
    }
    [self releaseAll];
    _editing = editing;
    _editBar.hidden = !editing;
    for (TouchCluster cluster = 0; cluster < TouchClusterCount; cluster++) {
        _outlines[cluster].hidden = !editing;
    }
    self.opacity = _opacity;
}

- (void)redraw {
    CGColorRef fill = [UIColor colorWithWhite:1 alpha:0.14].CGColor, lit = [UIColor colorWithWhite:1 alpha:0.45].CGColor;
    CGColorRef stroke = [UIColor colorWithWhite:1 alpha:0.32].CGColor;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    for (TouchControl control = 0; control < TouchControlCount; control++) {
        BOOL on = control != TouchControlDpad && (_lit & (1u << control)) != 0;
        if (control == TouchControlFastForward) {
            on = _input.fastForwardLatched;
        }
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

// Edit mode: the cluster under the finger follows it.
- (void)dragTouch:(id)touch at:(CGPoint)point phase:(UITouchPhase)phase {
    TrackedTouch* tracked = [_touches objectForKey:touch];
    if (phase == UITouchPhaseBegan) {
        TouchControl control = TouchLayoutHit(&_layout, point);
        if (control == TouchControlCount) {
            return;
        }
        tracked = [TrackedTouch new];
        tracked.control = control;
        tracked.start = point;
        tracked.offset = _layout.offsets[TouchControlCluster(control)];
        [_touches setObject:tracked forKey:touch];
        return;
    }
    if (tracked == nil) {
        return;
    }
    if (phase == UITouchPhaseEnded || phase == UITouchPhaseCancelled) {
        [_touches removeObjectForKey:touch];
        return;
    }
    CGPoint offset = CGPointMake(tracked.offset.x + point.x - tracked.start.x, tracked.offset.y + point.y - tracked.start.y);
    if (self.clusterMoved) {
        self.clusterMoved(TouchControlCluster(tracked.control), offset);
    }
}

// Fast-forward: a short press switches it, a long one holds it till let go.
- (void)fastForwardPressed:(TrackedTouch*)tracked ended:(BOOL)ended {
    if (!ended) {
        tracked.wasFastForward = _input.fastForwardLatched;
        tracked.began = CACurrentMediaTime();
        _input.fastForwardLatched = YES;
        [self tap:NO];
    } else {
        BOOL held = CACurrentMediaTime() - tracked.began >= kFastForwardHold;
        _input.fastForwardLatched = held ? tracked.wasFastForward : !tracked.wasFastForward;
    }
}

- (void)trackTouch:(id)touch at:(CGPoint)point phase:(UITouchPhase)phase {
    if (_editing) {
        [self dragTouch:touch at:point phase:phase];
        return;
    }
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
        if (control == TouchControlFastForward) {
            [self fastForwardPressed:tracked ended:NO];
            [self redraw];
            return;
        }
    }
    if (tracked == nil) {
        return;
    }
    if (tracked.control == TouchControlFastForward) {
        if (phase == UITouchPhaseEnded || phase == UITouchPhaseCancelled) {
            [self fastForwardPressed:tracked ended:YES];
            [_touches removeObjectForKey:touch];
            [self redraw];
        }
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
    for (id touch in _touches) {
        TrackedTouch* tracked = [_touches objectForKey:touch];
        if (!_editing && tracked.control == TouchControlFastForward) {
            [self fastForwardPressed:tracked ended:YES];
        }
    }
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
