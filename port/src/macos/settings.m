#import "settings.h"

#include <stddef.h>

NSString* const kFilterDefault = @"Filter";
NSString* const kIntegerScalingDefault = @"IntegerScaling";

typedef struct {
    NSString* section; // starts a new section when set
    NSString* title;
    NSString* key;
    size_t offset;     // into FilterParams
    float min, max;
} Knob;

static const Knob kKnobs[] = {
    { @"LCD Grid", @"Gap strength", @"LCD.GridStrength", offsetof(FilterParams, lcdGridStrength), 0, 1 },
    { nil, @"Gap width", @"LCD.GridWidth", offsetof(FilterParams, lcdGridWidth), 0, 0.5f },
    { nil, @"Subpixel strength", @"LCD.Subpixel", offsetof(FilterParams, lcdSubpixel), 0, 1 },
    { nil, @"Brightness", @"LCD.Brightness", offsetof(FilterParams, lcdBrightness), 0.5f, 2 },
    { @"CRT Scanlines", @"Scanline strength", @"CRT.Scanlines", offsetof(FilterParams, crtScanlines), 0, 1 },
    { nil, @"Aperture grille", @"CRT.Mask", offsetof(FilterParams, crtMask), 0, 1 },
    { nil, @"Brightness", @"CRT.Brightness", offsetof(FilterParams, crtBrightness), 0.5f, 2 },
    { @"Colour (all filters)", @"GBA colour correction", @"Color.Correction", offsetof(FilterParams, colorCorrection), 0, 1 },
    { nil, @"Gamma", @"Color.Gamma", offsetof(FilterParams, gamma), 0.5f, 2 },
};
#define KNOB_COUNT (sizeof(kKnobs) / sizeof(kKnobs[0]))

static float* KnobValue(FilterParams* params, const Knob* knob) {
    return (float*)((char*)params + knob->offset);
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
    NSPopUpButton* _filterPopup;
    NSButton* _integerScaling;
    NSSlider* _sliders[KNOB_COUNT];
    NSTextField* _values[KNOB_COUNT];
}

- (instancetype)initWithRenderer:(Renderer*)renderer {
    NSPanel* panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 420, 400)
                                                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                          NSWindowStyleMaskUtilityWindow
                                                  backing:NSBackingStoreBuffered
                                                    defer:YES];
    panel.title = @"Display Settings";
    panel.hidesOnDeactivate = NO;
    panel.becomesKeyOnlyIfNeeded = YES; // keep the game window taking the keyboard
    if ((self = [super initWithWindow:panel]) != nil) {
        _renderer = renderer;
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
        }
        NSSlider* slider = [NSSlider sliderWithValue:0 minValue:kKnobs[i].min maxValue:kKnobs[i].max
                                              target:self action:@selector(sliderChanged:)];
        slider.tag = (NSInteger)i;
        slider.continuous = YES;
        [slider.widthAnchor constraintGreaterThanOrEqualToConstant:200].active = YES;
        NSTextField* value = Label(@"", NO);
        value.font = [NSFont monospacedDigitSystemFontOfSize:NSFont.smallSystemFontSize weight:NSFontWeightRegular];
        value.alignment = NSTextAlignmentRight;
        [value.widthAnchor constraintEqualToConstant:40].active = YES;
        _sliders[i] = slider;
        _values[i] = value;
        [rows addObject:@[ Label(kKnobs[i].title, NO), slider, value ]];
    }

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
    for (size_t i = 0; i < KNOB_COUNT; i++) {
        float value = *KnobValue(&params, &kKnobs[i]);
        _sliders[i].floatValue = value;
        _values[i].stringValue = [NSString stringWithFormat:@"%.2f", value];
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

- (void)sliderChanged:(NSSlider*)sender {
    const Knob* knob = &kKnobs[sender.tag];
    FilterParams params = _renderer.params;
    // Two decimals is as fine as the eye can tell; it also keeps saved values tidy.
    float value = roundf(sender.floatValue * 100) / 100;
    *KnobValue(&params, knob) = value;
    _renderer.params = params;
    _values[sender.tag].stringValue = [NSString stringWithFormat:@"%.2f", value];
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
