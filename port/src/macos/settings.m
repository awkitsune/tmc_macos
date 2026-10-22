#import "settings.h"

#include <stddef.h>

NSString* const kFilterDefault = @"Filter";
NSString* const kIntegerScalingDefault = @"IntegerScaling";
NSString* const kGamepadLayoutDefault = @"GamepadLayout";
NSString* const kLcdResponseDefault = @"LCDResponse";
NSString* const kDebugMenuDefault = @"ShowDebugMenu";
NSString* const kDebugMenuChangedNotification = @"TMCDebugMenuChanged";

typedef struct {
    NSString* section; // starts a new section when set
    NSString* title;
    NSString* key;
    size_t offset;     // into FilterParams
    float min, max;
    float step;        // values snap to this (0: 0.01)
    NSString* format;  // for the value label (nil: two decimals)
} Knob;

static const Knob kKnobs[] = {
    { @"LCD Grid", @"Gap strength", @"LCD.GridStrength", offsetof(FilterParams, lcdGridStrength), 0, 1 },
    { nil, @"Gap width", @"LCD.GridWidth", offsetof(FilterParams, lcdGridWidth), 0, 0.5f },
    { nil, @"Subpixel strength", @"LCD.Subpixel", offsetof(FilterParams, lcdSubpixel), 0, 1 },
    { nil, @"Gap bloom", @"LCD.GapBloom", offsetof(FilterParams, lcdGapBloom), 0, 1 },
    { nil, @"Brightness", @"LCD.Brightness", offsetof(FilterParams, lcdBrightness), 0.5f, 2 },
    { @"CRT Scanlines", @"Scanline strength", @"CRT.Scanlines", offsetof(FilterParams, crtScanlines), 0, 1 },
    { nil, @"Bloom", @"CRT.Bloom", offsetof(FilterParams, crtBloom), 0, 1 },
    { nil, @"Beam width (dark)", @"CRT.BeamMin", offsetof(FilterParams, crtBeamMin), 0.05f, 0.6f },
    { nil, @"Beam width (bright)", @"CRT.BeamMax", offsetof(FilterParams, crtBeamMax), 0.05f, 0.6f },
    { nil, @"Aperture grille", @"CRT.Mask", offsetof(FilterParams, crtMask), 0, 1 },
    { nil, @"Brightness", @"CRT.Brightness", offsetof(FilterParams, crtBrightness), 0.5f, 2 },
    { @"LCD Response (all filters)", @"Response time", @"LCD.ResponseMs", offsetof(FilterParams, lcdResponseMs), 0, 40, 1,
      @"%.0f ms" },
    { nil, @"Darkening slowdown", @"LCD.DarkSlowdown", offsetof(FilterParams, lcdDarkSlowdown), 1, 2, 0, @"%.2f×" },
    { @"Colour (all filters)", @"GBA colour correction", @"Color.Correction", offsetof(FilterParams, colorCorrection), 0, 1 },
    { nil, @"Gamma", @"Color.Gamma", offsetof(FilterParams, gamma), 0.5f, 2 },
};
#define KNOB_COUNT (sizeof(kKnobs) / sizeof(kKnobs[0]))

static float* KnobValue(FilterParams* params, const Knob* knob) {
    return (float*)((char*)params + knob->offset);
}

static NSString* KnobText(const Knob* knob, float value) {
    return [NSString stringWithFormat:knob->format ?: @"%.2f", value];
}

FilterParams LoadFilterParams(void) {
    NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
    FilterParams params = FilterParamsDefaults();
    for (size_t i = 0; i < KNOB_COUNT; i++) {
        NSNumber* stored = [defaults objectForKey:kKnobs[i].key];
        if ([stored isKindOfClass:NSNumber.class]) {
            *KnobValue(&params, &kKnobs[i]) = fminf(fmaxf(stored.floatValue, kKnobs[i].min), kKnobs[i].max);
        }
    }
    return params;
}

@implementation SettingsWindowController {
    Renderer* _renderer;
    Input* _input;
    NSPopUpButton* _filterPopup;
    NSPopUpButton* _gamepadPopup;
    NSButton* _integerScaling;
    NSButton* _debugMenu;
    NSButton* _lcdResponse;
    NSSlider* _sliders[KNOB_COUNT];
    NSTextField* _values[KNOB_COUNT];
}

- (instancetype)initWithRenderer:(Renderer*)renderer input:(Input*)input {
    NSPanel* panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 420, 400)
                                                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                          NSWindowStyleMaskUtilityWindow
                                                  backing:NSBackingStoreBuffered
                                                    defer:YES];
    panel.title = @"Settings";
    panel.hidesOnDeactivate = NO;
    panel.becomesKeyOnlyIfNeeded = YES; // keep the game window taking the keyboard
    if ((self = [super initWithWindow:panel]) != nil) {
        _renderer = renderer;
        _input = input;
        [self buildContent];
        [self refresh];
        [panel center];
    }
    return self;
}

static NSTextField* Label(NSString* text, BOOL bold) {
    NSTextField* label = [NSTextField labelWithString:text];
    if (bold) {
        label.font = [NSFont boldSystemFontOfSize:NSFont.systemFontSize];
    }
    return label;
}

- (void)buildContent {
    NSMutableArray<NSArray<NSView*>*>* rows = [NSMutableArray array];
    NSMutableIndexSet* headerRows = [NSMutableIndexSet indexSet];

    _filterPopup = [NSPopUpButton new];
    for (NSUInteger mode = 0; mode < FilterModeCount; mode++) {
        [_filterPopup addItemWithTitle:FilterModeName(mode)];
    }
    _filterPopup.target = self;
    _filterPopup.action = @selector(filterChanged:);
    [rows addObject:@[ Label(@"Filter", NO), _filterPopup, NSGridCell.emptyContentView ]];

    _integerScaling = [NSButton checkboxWithTitle:@"Integer scaling" target:self action:@selector(integerScalingChanged:)];
    [rows addObject:@[ NSGridCell.emptyContentView, _integerScaling, NSGridCell.emptyContentView ]];

    for (size_t i = 0; i < KNOB_COUNT; i++) {
        if (kKnobs[i].section != nil) {
            [headerRows addIndex:rows.count];
            [rows addObject:@[ Label(kKnobs[i].section, YES) ]];
            if (kKnobs[i].offset == offsetof(FilterParams, lcdResponseMs)) {
                _lcdResponse = [NSButton checkboxWithTitle:@"LCD response (pixel ghosting)" target:self
                                                    action:@selector(lcdResponseChanged:)];
                [rows addObject:@[ NSGridCell.emptyContentView, _lcdResponse, NSGridCell.emptyContentView ]];
            }
        }
        NSSlider* slider = [NSSlider sliderWithValue:0 minValue:kKnobs[i].min maxValue:kKnobs[i].max
                                              target:self action:@selector(sliderChanged:)];
        slider.tag = (NSInteger)i;
        slider.continuous = YES;
        [slider.widthAnchor constraintGreaterThanOrEqualToConstant:200].active = YES;
        NSTextField* value = Label(@"", NO);
        value.font = [NSFont monospacedDigitSystemFontOfSize:NSFont.smallSystemFontSize weight:NSFontWeightRegular];
        value.alignment = NSTextAlignmentRight;
        [value.widthAnchor constraintEqualToConstant:46].active = YES;
        _sliders[i] = slider;
        _values[i] = value;
        [rows addObject:@[ Label(kKnobs[i].title, NO), slider, value ]];
    }

    [headerRows addIndex:rows.count];
    [rows addObject:@[ Label(@"Controls", YES) ]];
    _gamepadPopup = [NSPopUpButton new];
    [_gamepadPopup addItemsWithTitles:@[ @"By label (A is A)", @"By position (right is A)" ]];
    _gamepadPopup.target = self;
    _gamepadPopup.action = @selector(gamepadLayoutChanged:);
    [rows addObject:@[ Label(@"Gamepad A/B", NO), _gamepadPopup, NSGridCell.emptyContentView ]];
    NSTextField* note = Label(@"Nintendo pads use their labels either way; this\nchanges Xbox and PlayStation pads.", NO);
    note.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    note.textColor = NSColor.secondaryLabelColor;
    [rows addObject:@[ NSGridCell.emptyContentView, note, NSGridCell.emptyContentView ]];

    [headerRows addIndex:rows.count];
    [rows addObject:@[ Label(@"Debug", YES) ]];
    _debugMenu = [NSButton checkboxWithTitle:@"Show Debug menu" target:self action:@selector(debugMenuChanged:)];
    [rows addObject:@[ NSGridCell.emptyContentView, _debugMenu, NSGridCell.emptyContentView ]];

    NSGridView* grid = [NSGridView gridViewWithViews:rows];
    grid.rowSpacing = 8;
    grid.columnSpacing = 10;
    [grid columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;
    [headerRows enumerateIndexesUsingBlock:^(NSUInteger row, BOOL* stop) {
        NSGridRow* gridRow = [grid rowAtIndex:row];
        gridRow.topPadding = 10;
        [gridRow mergeCellsInRange:NSMakeRange(0, 3)];
        [gridRow cellAtIndex:0].xPlacement = NSGridCellPlacementLeading;
    }];
    for (NSInteger row = 0; row < grid.numberOfRows; row++) {
        [grid rowAtIndex:row].yPlacement = NSGridCellPlacementCenter;
    }

    NSButton* reset = [NSButton buttonWithTitle:@"Reset to Defaults" target:self action:@selector(resetToDefaults:)];

    NSStackView* stack = [NSStackView stackViewWithViews:@[ grid, reset ]];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeTrailing;
    stack.spacing = 16;
    stack.translatesAutoresizingMaskIntoConstraints = NO;

    NSView* content = [NSView new];
    [content addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [stack.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:20],
        [stack.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-20],
        [stack.topAnchor constraintEqualToAnchor:content.topAnchor constant:20],
        [stack.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-20],
    ]];
    self.window.contentView = content;
    [self.window setContentSize:content.fittingSize];
}

- (void)refresh {
    FilterParams params = _renderer.params;
    [_filterPopup selectItemAtIndex:(NSInteger)_renderer.filter];
    _integerScaling.state = _renderer.integerScaling ? NSControlStateValueOn : NSControlStateValueOff;
    _lcdResponse.state = _renderer.lcdResponse ? NSControlStateValueOn : NSControlStateValueOff;
    [_gamepadPopup selectItemAtIndex:_input.gamepadLayout];
    _debugMenu.state = [NSUserDefaults.standardUserDefaults boolForKey:kDebugMenuDefault] ? NSControlStateValueOn
                                                                                          : NSControlStateValueOff;
    for (size_t i = 0; i < KNOB_COUNT; i++) {
        float value = *KnobValue(&params, &kKnobs[i]);
        _sliders[i].floatValue = value;
        _values[i].stringValue = KnobText(&kKnobs[i], value);
    }
}

- (void)filterChanged:(NSPopUpButton*)sender {
    _renderer.filter = (FilterMode)sender.indexOfSelectedItem;
    [NSUserDefaults.standardUserDefaults setInteger:sender.indexOfSelectedItem forKey:kFilterDefault];
}

- (void)integerScalingChanged:(NSButton*)sender {
    _renderer.integerScaling = sender.state == NSControlStateValueOn;
    [NSUserDefaults.standardUserDefaults setBool:_renderer.integerScaling forKey:kIntegerScalingDefault];
}

- (void)lcdResponseChanged:(NSButton*)sender {
    _renderer.lcdResponse = sender.state == NSControlStateValueOn;
    [NSUserDefaults.standardUserDefaults setBool:_renderer.lcdResponse forKey:kLcdResponseDefault];
}

- (void)gamepadLayoutChanged:(NSPopUpButton*)sender {
    _input.gamepadLayout = (GamepadLayout)sender.indexOfSelectedItem;
    [NSUserDefaults.standardUserDefaults setInteger:sender.indexOfSelectedItem forKey:kGamepadLayoutDefault];
}

- (void)debugMenuChanged:(NSButton*)sender {
    [NSUserDefaults.standardUserDefaults setBool:sender.state == NSControlStateValueOn forKey:kDebugMenuDefault];
    [NSNotificationCenter.defaultCenter postNotificationName:kDebugMenuChangedNotification object:nil];
}

- (void)sliderChanged:(NSSlider*)sender {
    const Knob* knob = &kKnobs[sender.tag];
    FilterParams params = _renderer.params;
    // Two decimals is as fine as the eye can tell; it also keeps saved values tidy.
    float step = knob->step > 0 ? knob->step : 0.01f;
    float value = roundf(sender.floatValue / step) * step;
    *KnobValue(&params, knob) = value;
    _renderer.params = params;
    _values[sender.tag].stringValue = KnobText(knob, value);
    [NSUserDefaults.standardUserDefaults setFloat:value forKey:knob->key];
}

- (void)resetToDefaults:(id)sender {
    for (size_t i = 0; i < KNOB_COUNT; i++) {
        [NSUserDefaults.standardUserDefaults removeObjectForKey:kKnobs[i].key];
    }
    _renderer.params = FilterParamsDefaults();
    [self refresh];
}

@end
