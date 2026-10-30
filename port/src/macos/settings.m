#import "settings.h"

#include <stddef.h>

static NSTextField* Label(NSString* text, BOOL bold) {
    NSTextField* label = [NSTextField labelWithString:text];
    if (bold) {
        label.font = [NSFont boldSystemFontOfSize:NSFont.systemFontSize];
    }
    return label;
}

// One tab's rows, laid out as a grid: label, control, value.
@interface SettingsPage : NSObject
@property(nonatomic, copy) NSString* title;
@property(nonatomic, strong) NSMutableArray<NSArray<NSView*>*>* rows;
@property(nonatomic, strong) NSMutableIndexSet* headerRows;
@end

@implementation SettingsPage

- (instancetype)initWithTitle:(NSString*)title {
    if ((self = [super init]) != nil) {
        _title = [title copy];
        _rows = [NSMutableArray array];
        _headerRows = [NSMutableIndexSet indexSet];
    }
    return self;
}

- (void)addHeader:(NSString*)text {
    [_headerRows addIndex:_rows.count];
    [_rows addObject:@[ Label(text, YES) ]];
}

- (void)addRow:(nullable NSString*)label control:(NSView*)control value:(nullable NSView*)value {
    [_rows addObject:@[ label ? Label(label, NO) : NSGridCell.emptyContentView, control,
                        value ?: NSGridCell.emptyContentView ]];
}

- (NSGridView*)grid {
    NSGridView* grid = [NSGridView gridViewWithViews:_rows];
    grid.rowSpacing = 8;
    grid.columnSpacing = 10;
    [grid columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;
    [_headerRows enumerateIndexesUsingBlock:^(NSUInteger row, BOOL* stop) {
        NSGridRow* gridRow = [grid rowAtIndex:row];
        gridRow.topPadding = row == 0 ? 0 : 10;
        [gridRow mergeCellsInRange:NSMakeRange(0, 3)];
        [gridRow cellAtIndex:0].xPlacement = NSGridCellPlacementLeading;
    }];
    for (NSInteger row = 0; row < grid.numberOfRows; row++) {
        [grid rowAtIndex:row].yPlacement = NSGridCellPlacementCenter;
    }
    return grid;
}

@end

// Scrolled content starts at the top.
@interface FlippedView : NSView
@end

@implementation FlippedView

- (BOOL)isFlipped {
    return YES;
}

@end

static NSTextField* Note(NSString* text) {
    NSTextField* note = Label(text, NO);
    note.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    note.textColor = NSColor.secondaryLabelColor;
    return note;
}

static NSString* const kSettingsTabDefault = @"SettingsTab";

@interface SettingsWindowController () <NSTabViewDelegate, NSWindowDelegate>
@end

@implementation SettingsWindowController {
    Renderer* _renderer;
    Input* _input;
    NSPopUpButton* _filterPopup;
    NSPopUpButton* _gamepadPopup;
    NSPopUpButton* _pacingPopup;
    NSTextField* _pacingNote;
    NSString* (^_pacingStatus)(void);
    NSButton* _integerScaling;
    NSButton* _debugMenu;
    NSButton* _debugToolbar;
    NSButton* _lcdResponse;
    NSButton *_gpuPpu, *_gpuCompact, *_gpuSprites, *_gpuBackgrounds;
    NSSlider* _sliders[KNOB_COUNT];
    NSTextField* _values[KNOB_COUNT];
    NSTabView* _tabs;
    NSLayoutConstraint* _tabsHeight;
    CGFloat _fullTabsHeight;
    BOOL _renderingChanged;
}

- (instancetype)initWithRenderer:(Renderer*)renderer input:(Input*)input pacingStatus:(NSString* (^)(void))pacingStatus {
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
        _pacingStatus = [pacingStatus copy];
        panel.delegate = self;
        [self buildContent];
        [self refresh];
        [panel center];
    }
    return self;
}

- (void)buildContent {
    SettingsPage* display = [[SettingsPage alloc] initWithTitle:@"Display"];
    SettingsPage* filters = [[SettingsPage alloc] initWithTitle:@"Filters"];
    SettingsPage* rendering = [[SettingsPage alloc] initWithTitle:@"Rendering"];
    SettingsPage* timing = [[SettingsPage alloc] initWithTitle:@"Timing"];
    SettingsPage* controls = [[SettingsPage alloc] initWithTitle:@"Controls"];
    SettingsPage* debug = [[SettingsPage alloc] initWithTitle:@"Debug"];
    // The knob sections' tabs: one filter's knobs on Filters, the ones for every filter on Display.
    NSDictionary<NSString*, SettingsPage*>* knobPages = @{
        @"LCD Grid" : filters,
        @"CRT Scanlines" : filters,
        @"LCD Response (all filters)" : display,
        @"Colour (all filters)" : display,
    };

    [display addHeader:@"Picture"];
    _filterPopup = [NSPopUpButton new];
    for (NSUInteger mode = 0; mode < FilterModeCount; mode++) {
        [_filterPopup addItemWithTitle:FilterModeName(mode)];
    }
    _filterPopup.target = self;
    _filterPopup.action = @selector(filterChanged:);
    _filterPopup.identifier = kFilterDefault;
    [display addRow:@"Filter" control:_filterPopup value:nil];
    _integerScaling = [NSButton checkboxWithTitle:@"Integer scaling" target:self action:@selector(integerScalingChanged:)];
    _integerScaling.identifier = kIntegerScalingDefault;
    [display addRow:nil control:_integerScaling value:nil];

    SettingsPage* page = nil;
    for (size_t i = 0; i < KNOB_COUNT; i++) {
        if (kKnobs[i].section != nil) {
            page = knobPages[kKnobs[i].section];
            NSAssert(page != nil, @"no tab for %@", kKnobs[i].section);
            [page addHeader:kKnobs[i].section];
            if (kKnobs[i].offset == offsetof(FilterParams, lcdResponseMs)) {
                _lcdResponse = [NSButton checkboxWithTitle:@"LCD response (pixel ghosting)" target:self
                                                    action:@selector(lcdResponseChanged:)];
                _lcdResponse.identifier = kLcdResponseDefault;
                [page addRow:nil control:_lcdResponse value:nil];
            }
        }
        NSSlider* slider = [NSSlider sliderWithValue:0 minValue:kKnobs[i].min maxValue:kKnobs[i].max
                                              target:self action:@selector(sliderChanged:)];
        slider.tag = (NSInteger)i;
        slider.continuous = YES;
        slider.identifier = kKnobs[i].key;
        [slider.widthAnchor constraintGreaterThanOrEqualToConstant:200].active = YES;
        NSTextField* value = Label(@"", NO);
        value.font = [NSFont monospacedDigitSystemFontOfSize:NSFont.smallSystemFontSize weight:NSFontWeightRegular];
        value.alignment = NSTextAlignmentRight;
        [value.widthAnchor constraintEqualToConstant:46].active = YES;
        _sliders[i] = slider;
        _values[i] = value;
        [page addRow:kKnobs[i].title control:slider value:value];
    }

    [rendering addHeader:@"Game rendering"];
    _gpuPpu = [NSButton checkboxWithTitle:@"GPU PPU (experimental)" target:self action:@selector(renderingChanged:)];
    [rendering addRow:@"Renderer" control:_gpuPpu value:nil];
    _gpuCompact = [NSButton checkboxWithTitle:@"Compact scanline capture" target:self action:@selector(renderingChanged:)];
    [rendering addRow:@"Capture" control:_gpuCompact value:nil];
    _gpuSprites = [NSButton checkboxWithTitle:@"Sprite quads" target:self action:@selector(renderingChanged:)];
    [rendering addRow:@"Sprites" control:_gpuSprites value:nil];
    _gpuBackgrounds = [NSButton checkboxWithTitle:@"Separate background layers" target:self action:@selector(renderingChanged:)];
    [rendering addRow:@"Backgrounds" control:_gpuBackgrounds value:nil];
    [rendering addRow:nil control:Note(@"Rendering changes are saved when Settings closes.\nUse Debug → Performance to compare them.") value:nil];

    [timing addHeader:@"Timing"];
    _pacingPopup = [NSPopUpButton new];
    [_pacingPopup addItemsWithTitles:@[ @"Accurate (59.73 Hz)", @"Display sync (60 Hz, less lag)" ]];
    _pacingPopup.target = self;
    _pacingPopup.action = @selector(pacingChanged:);
    _pacingPopup.identifier = kPacingDefault;
    [timing addRow:@"Frame pacing" control:_pacingPopup value:nil];
    _pacingNote = Note(@"");
    _pacingNote.preferredMaxLayoutWidth = 260;
    [timing addRow:nil control:_pacingNote value:nil];

    [controls addHeader:@"Controls"];
    _gamepadPopup = [NSPopUpButton new];
    [_gamepadPopup addItemsWithTitles:@[ @"By label (A is A)", @"By position (right is A)" ]];
    _gamepadPopup.target = self;
    _gamepadPopup.action = @selector(gamepadLayoutChanged:);
    _gamepadPopup.identifier = kGamepadLayoutDefault;
    [controls addRow:@"Gamepad A/B" control:_gamepadPopup value:nil];
    [controls addRow:nil
             control:Note(@"Nintendo pads use their labels either way; this\nchanges Xbox and PlayStation pads.")
               value:nil];

    [debug addHeader:@"Debug"];
    _debugMenu = [NSButton checkboxWithTitle:@"Show Debug menu" target:self action:@selector(debugMenuChanged:)];
    _debugMenu.identifier = kDebugMenuDefault;
    [debug addRow:@"Menu" control:_debugMenu value:nil];
    _debugToolbar = [NSButton checkboxWithTitle:@"Show debug tools in toolbar" target:self
                                         action:@selector(debugToolbarChanged:)];
    _debugToolbar.identifier = kDebugToolbarDefault;
    [debug addRow:@"Toolbar" control:_debugToolbar value:nil];

    // Each tab scrolls, so the window can be as short as the screen needs.
    _tabs = [NSTabView new];
    _tabs.translatesAutoresizingMaskIntoConstraints = NO;
    NSSize largest = NSZeroSize;
    for (SettingsPage* each in @[ display, filters, rendering, timing, controls, debug ]) {
        NSGridView* grid = each.grid;
        grid.translatesAutoresizingMaskIntoConstraints = NO;
        FlippedView* document = [FlippedView new];
        document.translatesAutoresizingMaskIntoConstraints = NO;
        [document addSubview:grid];
        NSScrollView* scroll = [NSScrollView new];
        scroll.hasVerticalScroller = YES;
        scroll.autohidesScrollers = YES;
        scroll.drawsBackground = NO;
        scroll.documentView = document;
        [NSLayoutConstraint activateConstraints:@[
            [grid.leadingAnchor constraintEqualToAnchor:document.leadingAnchor constant:16],
            [grid.trailingAnchor constraintLessThanOrEqualToAnchor:document.trailingAnchor constant:-16],
            [grid.topAnchor constraintEqualToAnchor:document.topAnchor constant:12],
            [grid.bottomAnchor constraintEqualToAnchor:document.bottomAnchor constant:-12],
            [document.widthAnchor constraintEqualToAnchor:scroll.contentView.widthAnchor],
        ]];
        NSSize fitting = grid.fittingSize;
        largest.width = MAX(largest.width, fitting.width + 32);
        largest.height = MAX(largest.height, fitting.height + 24);
        NSTabViewItem* item = [[NSTabViewItem alloc] initWithIdentifier:each.title];
        item.label = each.title;
        item.view = scroll;
        [_tabs addTabViewItem:item];
    }
    NSString* tab = [NSUserDefaults.standardUserDefaults stringForKey:kSettingsTabDefault];
    if (tab != nil && [_tabs indexOfTabViewItemWithIdentifier:tab] != NSNotFound) {
        [_tabs selectTabViewItemWithIdentifier:tab];
    }
    _tabs.delegate = self;

    NSButton* reset = [NSButton buttonWithTitle:@"Reset to Defaults" target:self action:@selector(resetToDefaults:)];
    reset.translatesAutoresizingMaskIntoConstraints = NO;

    NSView* content = [NSView new];
    [content addSubview:_tabs];
    [content addSubview:reset];
    // The tab view's frame around its content (the tabs on top, the border).
    _tabs.frame = NSMakeRect(0, 0, 1000, 1000);
    NSSize chrome = NSMakeSize(1000 - NSWidth(_tabs.contentRect), 1000 - NSHeight(_tabs.contentRect));
    _fullTabsHeight = largest.height + chrome.height;
    _tabsHeight = [_tabs.heightAnchor constraintEqualToConstant:_fullTabsHeight];
    [NSLayoutConstraint activateConstraints:@[
        [_tabs.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:12],
        [_tabs.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-12],
        [_tabs.topAnchor constraintEqualToAnchor:content.topAnchor constant:12],
        [_tabs.widthAnchor constraintGreaterThanOrEqualToConstant:largest.width + chrome.width],
        _tabsHeight,
        [reset.topAnchor constraintEqualToAnchor:_tabs.bottomAnchor constant:12],
        [reset.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-20],
        [reset.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-16],
    ]];
    self.window.contentView = content;
    [self fitToHeight:0];
}

- (void)tabView:(NSTabView*)tabView didSelectTabViewItem:(NSTabViewItem*)item {
    [NSUserDefaults.standardUserDefaults setObject:item.identifier forKey:kSettingsTabDefault];
}

// As tall as the tallest tab, or less to keep the window within maxHeight (0:
// the screen's visible frame); a tab that does not fit scrolls.
- (void)fitToHeight:(CGFloat)maxHeight {
    NSWindow* window = self.window;
    if (maxHeight <= 0) {
        NSScreen* screen = window.screen ?: NSScreen.mainScreen;
        maxHeight = NSHeight(screen.visibleFrame);
    }
    NSView* content = window.contentView;
    _tabsHeight.constant = _fullTabsHeight;
    CGFloat around = content.fittingSize.height - _fullTabsHeight; // margins and the button
    CGFloat titleBar = NSHeight(window.frame) - NSHeight([window contentRectForFrameRect:window.frame]);
    _tabsHeight.constant = MAX(80, MIN(_fullTabsHeight, maxHeight - titleBar - around));
    NSRect frame = window.frame;
    [window setContentSize:content.fittingSize];
    // Keep the top edge, and the whole window on its screen.
    [window setFrameTopLeftPoint:NSMakePoint(NSMinX(frame), NSMaxY(frame))];
    if (window.screen != nil) {
        [window setFrame:[window constrainFrameRect:window.frame toScreen:window.screen] display:NO];
    }
}

// Fitted to the screen it opens on.
- (void)showWindow:(id)sender {
    [self fitToHeight:0];
    [super showWindow:sender];
}

- (void)windowWillClose:(NSNotification*)notification {
    if (!_renderingChanged) return;
    [NSUserDefaults.standardUserDefaults setObject:@{
        @"enabled": @(_renderer.gpuPpuEnabled),
        @"compact": @(_renderer.compactGpuCaptureEnabled),
        @"sprites": @(_renderer.gpuSpriteQuadsEnabled),
        @"backgrounds": @(_renderer.gpuBackgroundLayersEnabled)
    } forKey:kGpuRenderingDefault];
    _renderingChanged = NO;
}

- (NSTabView*)tabView {
    return _tabs;
}

- (void)refresh {
    FilterParams params = _renderer.params;
    [_filterPopup selectItemAtIndex:(NSInteger)_renderer.filter];
    _integerScaling.state = _renderer.integerScaling ? NSControlStateValueOn : NSControlStateValueOff;
    _lcdResponse.state = _renderer.lcdResponse ? NSControlStateValueOn : NSControlStateValueOff;
    _gpuPpu.state = _renderer.gpuPpuEnabled ? NSControlStateValueOn : NSControlStateValueOff;
    _gpuCompact.state = _renderer.compactGpuCaptureEnabled ? NSControlStateValueOn : NSControlStateValueOff;
    _gpuSprites.state = _renderer.gpuSpriteQuadsEnabled ? NSControlStateValueOn : NSControlStateValueOff;
    _gpuBackgrounds.state = _renderer.gpuBackgroundLayersEnabled ? NSControlStateValueOn : NSControlStateValueOff;
    _gpuCompact.enabled = _gpuSprites.enabled = _gpuBackgrounds.enabled = _renderer.gpuPpuEnabled;
    [_gamepadPopup selectItemAtIndex:_input.gamepadLayout];
    [_pacingPopup selectItemAtIndex:[NSUserDefaults.standardUserDefaults integerForKey:kPacingDefault] == 0 ? 0 : 1];
    _pacingNote.stringValue = _pacingStatus != nil ? _pacingStatus() : @"";
    _debugMenu.state = [NSUserDefaults.standardUserDefaults boolForKey:kDebugMenuDefault] ? NSControlStateValueOn
                                                                                          : NSControlStateValueOff;
    _debugToolbar.state = [NSUserDefaults.standardUserDefaults boolForKey:kDebugToolbarDefault] ? NSControlStateValueOn
                                                                                                : NSControlStateValueOff;
    for (size_t i = 0; i < KNOB_COUNT; i++) {
        float value = *KnobValue(&params, &kKnobs[i]);
        _sliders[i].floatValue = value;
        _values[i].stringValue = KnobText(&kKnobs[i], value);
    }
}

- (void)renderingChanged:(NSButton*)sender {
    BOOL before = sender == _gpuPpu ? _renderer.gpuPpuEnabled : sender == _gpuCompact ? _renderer.compactGpuCaptureEnabled :
                  sender == _gpuSprites ? _renderer.gpuSpriteQuadsEnabled : _renderer.gpuBackgroundLayersEnabled;
    NSString* action = sender == _gpuPpu ? @"toggleGpuPpu:" : sender == _gpuCompact ? @"toggleCompactGpuCapture:" :
                       sender == _gpuSprites ? @"toggleGpuSpriteQuads:" : @"toggleGpuBackgroundLayers:";
    [NSApp sendAction:NSSelectorFromString(action) to:NSApp.delegate from:sender];
    BOOL after = sender == _gpuPpu ? _renderer.gpuPpuEnabled : sender == _gpuCompact ? _renderer.compactGpuCaptureEnabled :
                 sender == _gpuSprites ? _renderer.gpuSpriteQuadsEnabled : _renderer.gpuBackgroundLayersEnabled;
    _renderingChanged |= before != after;
    [self refresh];
}

- (BOOL)checkRenderingControls {
    BOOL wasEnabled = _renderer.gpuPpuEnabled;
    BOOL wasCompact = _renderer.compactGpuCaptureEnabled, wasSprites = _renderer.gpuSpriteQuadsEnabled;
    BOOL wasBackgrounds = _renderer.gpuBackgroundLayersEnabled;
    NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
    id saved = [defaults objectForKey:kGpuRenderingDefault];
    if (wasEnabled) [NSApp sendAction:NSSelectorFromString(@"toggleGpuPpu:") to:NSApp.delegate from:nil];
    [self refresh];
    BOOL ok = !_gpuCompact.enabled && !_gpuSprites.enabled && !_gpuBackgrounds.enabled;
    [_gpuPpu performClick:nil];
    ok &= _renderer.gpuPpuEnabled && _gpuCompact.enabled && _gpuSprites.enabled && _gpuBackgrounds.enabled;
    if (_renderer.gpuPpuEnabled) {
        [_gpuCompact performClick:nil];
        [_gpuSprites performClick:nil];
        [_gpuBackgrounds performClick:nil];
        ok &= _renderer.compactGpuCaptureEnabled && _renderer.gpuSpriteQuadsEnabled && _renderer.gpuBackgroundLayersEnabled;
    }
    id beforeClose = [defaults objectForKey:kGpuRenderingDefault];
    ok &= saved == beforeClose || [saved isEqual:beforeClose];
    [self close];
    NSDictionary* committed = [defaults dictionaryForKey:kGpuRenderingDefault];
    ok &= [committed[@"enabled"] boolValue] && [committed[@"compact"] boolValue] &&
          [committed[@"sprites"] boolValue] && [committed[@"backgrounds"] boolValue];
    ok &= [LoadGpuRenderingSettings() isEqual:committed];
    [self showWindow:nil];
    [self resetToDefaults:nil];
    ok &= !_renderer.gpuPpuEnabled && !_renderer.compactGpuCaptureEnabled &&
          !_renderer.gpuSpriteQuadsEnabled && !_renderer.gpuBackgroundLayersEnabled &&
          !_gpuCompact.enabled && !_gpuSprites.enabled && !_gpuBackgrounds.enabled;
    ok &= [[defaults dictionaryForKey:kGpuRenderingDefault] isEqual:committed];
    [self close];
    committed = [defaults dictionaryForKey:kGpuRenderingDefault];
    ok &= ![committed[@"enabled"] boolValue] && ![committed[@"compact"] boolValue] &&
          ![committed[@"sprites"] boolValue] && ![committed[@"backgrounds"] boolValue];
    if (wasEnabled) [NSApp sendAction:NSSelectorFromString(@"toggleGpuPpu:") to:NSApp.delegate from:nil];
    if (wasCompact) [NSApp sendAction:NSSelectorFromString(@"toggleCompactGpuCapture:") to:NSApp.delegate from:nil];
    if (wasSprites) [NSApp sendAction:NSSelectorFromString(@"toggleGpuSpriteQuads:") to:NSApp.delegate from:nil];
    if (wasBackgrounds) [NSApp sendAction:NSSelectorFromString(@"toggleGpuBackgroundLayers:") to:NSApp.delegate from:nil];
    if (saved) [defaults setObject:saved forKey:kGpuRenderingDefault];
    else [defaults removeObjectForKey:kGpuRenderingDefault];
    [self showWindow:nil];
    return ok;
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

- (void)pacingChanged:(NSPopUpButton*)sender {
    [NSUserDefaults.standardUserDefaults setInteger:sender.indexOfSelectedItem forKey:kPacingDefault];
    [NSNotificationCenter.defaultCenter postNotificationName:kPacingChangedNotification object:nil];
    // The display link needs a moment to measure the display.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 2), dispatch_get_main_queue(), ^{
        self->_pacingNote.stringValue = self->_pacingStatus != nil ? self->_pacingStatus() : @"";
    });
    _pacingNote.stringValue = _pacingStatus != nil ? _pacingStatus() : @"";
}

- (void)debugMenuChanged:(NSButton*)sender {
    [NSUserDefaults.standardUserDefaults setBool:sender.state == NSControlStateValueOn forKey:kDebugMenuDefault];
    [NSNotificationCenter.defaultCenter postNotificationName:kDebugMenuChangedNotification object:nil];
}

- (void)debugToolbarChanged:(NSButton*)sender {
    [NSUserDefaults.standardUserDefaults setBool:sender.state == NSControlStateValueOn forKey:kDebugToolbarDefault];
    [NSNotificationCenter.defaultCenter postNotificationName:kDebugToolbarChangedNotification object:nil];
}

- (void)sliderChanged:(NSSlider*)sender {
    const Knob* knob = &kKnobs[sender.tag];
    FilterParams params = _renderer.params;
    // Two decimals is as fine as the eye can tell; it also keeps saved values tidy.
    float value = KnobSnap(knob, sender.floatValue);
    *KnobValue(&params, knob) = value;
    _renderer.params = params;
    _values[sender.tag].stringValue = KnobText(knob, value);
    [NSUserDefaults.standardUserDefaults setFloat:value forKey:knob->key];
}

- (void)resetToDefaults:(id)sender {
    if ([_tabs.selectedTabViewItem.identifier isEqual:@"Rendering"]) {
        if (_renderer.gpuPpuEnabled) [NSApp sendAction:NSSelectorFromString(@"toggleGpuPpu:") to:NSApp.delegate from:sender];
        _renderingChanged = YES;
        [self refresh];
        return;
    }
    for (size_t i = 0; i < KNOB_COUNT; i++) {
        [NSUserDefaults.standardUserDefaults removeObjectForKey:kKnobs[i].key];
    }
    _renderer.params = FilterParamsDefaults();
    [self refresh];
}

@end
