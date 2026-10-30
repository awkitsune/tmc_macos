/*
 * The settings sheet (see settingsview.h). Each page is a list of sections of
 * rows described by SettingsRow, which also tells how a controller changes it.
 */

#import "settingsview.h"

#import "settingsstore.h"
#import "touchcontrols.h"

#include "audio.h"
#include "frontend.h"
#include "port/hw.h"
#include "port/romdata.h"

static NSString* const kMutedDefault = @"Muted";
static NSString* const kHighQualityAudioDefault = @"HighQualityAudio";

// Held this long, a direction repeats every kRepeatSeconds.
static const CFTimeInterval kRepeatDelay = 0.35;
static const CFTimeInterval kRepeatSeconds = 0.08;
// A slider moves 1/kSliderSteps of its range a step, and a held direction
// speeds up to kSliderMaxSpeed steps a repeat (one every kSliderSpeedUp repeats).
static const float kSliderSteps = 100;
static const NSInteger kSliderMaxSpeed = 8;
static const NSInteger kSliderSpeedUp = 5;

typedef NS_ENUM(NSInteger, RowKind) {
    RowKindToggle, // a switch
    RowKindChoice, // one of options, chosen on a page of its own
    RowKindSlider, // a filter knob
    RowKindAction, // runs something (a button)
    RowKindPage,   // opens a page
    RowKindInfo,   // title and value, not selectable
    RowKindText,   // a paragraph, not selectable
};

@interface SettingsRow : NSObject
@property(nonatomic) RowKind kind;
@property(nonatomic, copy) NSString* title;
@property(nonatomic, copy, nullable) NSString* symbol;              // SF Symbol (pages)
@property(nonatomic, copy, nullable) NSString* (^detail)(void);     // the value shown on the right
@property(nonatomic, copy, nullable) BOOL (^isOn)(void);            // toggle
@property(nonatomic, copy, nullable) void (^setOn)(BOOL on);        // toggle
@property(nonatomic, copy, nullable) NSArray<NSString*>* options;   // choice
@property(nonatomic, copy, nullable) NSInteger (^selected)(void);   // choice
@property(nonatomic, copy, nullable) void (^select)(NSInteger index); // choice
@property(nonatomic) const Knob* knob;                              // slider
@property(nonatomic, copy, nullable) void (^run)(void);             // action
@property(nonatomic) BOOL destructive;                              // action: the first press asks, the second runs
@property(nonatomic, copy, nullable) NSString* confirmTitle;        // its title while asking
@property(nonatomic) BOOL armed;
@property(nonatomic, copy, nullable) BOOL (^checked)(void);         // action: shows a checkmark
@property(nonatomic, copy, nullable) UIViewController* (^open)(void); // page
@end

@implementation SettingsRow

+ (instancetype)rowWithKind:(RowKind)kind title:(NSString*)title {
    SettingsRow* row = [self new];
    row.kind = kind;
    row.title = title;
    return row;
}

- (BOOL)focusable {
    return self.kind != RowKindInfo && self.kind != RowKindText;
}

@end

@interface SettingsSection : NSObject
@property(nonatomic, copy, nullable) NSString* header;
@property(nonatomic, copy, nullable) NSString* (^footer)(void);
@property(nonatomic, copy) NSArray<SettingsRow*>* rows;
@end

@implementation SettingsSection

+ (instancetype)header:(NSString*)header rows:(NSArray<SettingsRow*>*)rows footer:(NSString*)footer {
    SettingsSection* section = [self new];
    section.header = header;
    section.rows = rows;
    if (footer != nil) {
        section.footer = ^NSString* {
            return footer;
        };
    }
    return section;
}

@end

// A knob's slider with its title and value.
@interface SliderCell : UITableViewCell
@property(nonatomic, readonly) UILabel* title;
@property(nonatomic, readonly) UILabel* value;
@property(nonatomic, readonly) UISlider* slider;
@end

@implementation SliderCell

- (instancetype)initWithStyle:(UITableViewCellStyle)style reuseIdentifier:(NSString*)reuseIdentifier {
    if ((self = [super initWithStyle:style reuseIdentifier:reuseIdentifier]) != nil) {
        _title = [UILabel new];
        _title.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
        _title.adjustsFontForContentSizeCategory = YES;
        _value = [UILabel new];
        _value.font = [UIFont monospacedDigitSystemFontOfSize:[UIFont preferredFontForTextStyle:UIFontTextStyleBody].pointSize
                                                       weight:UIFontWeightRegular];
        _value.textColor = UIColor.secondaryLabelColor;
        _value.textAlignment = NSTextAlignmentRight;
        [_value setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
        _slider = [UISlider new];
        UIStackView* top = [[UIStackView alloc] initWithArrangedSubviews:@[ _title, _value ]];
        top.spacing = 8;
        UIStackView* stack = [[UIStackView alloc] initWithArrangedSubviews:@[ top, _slider ]];
        stack.axis = UILayoutConstraintAxisVertical;
        stack.spacing = 4;
        stack.translatesAutoresizingMaskIntoConstraints = NO;
        [self.contentView addSubview:stack];
        UILayoutGuide* margins = self.contentView.layoutMarginsGuide;
        [NSLayoutConstraint activateConstraints:@[
            [stack.leadingAnchor constraintEqualToAnchor:margins.leadingAnchor],
            [stack.trailingAnchor constraintEqualToAnchor:margins.trailingAnchor],
            [stack.topAnchor constraintEqualToAnchor:margins.topAnchor],
            [stack.bottomAnchor constraintEqualToAnchor:margins.bottomAnchor],
        ]];
    }
    return self;
}

@end

#pragma mark Pages

@class SettingsController;

@interface SettingsPage : UITableViewController
@property(nonatomic, weak) id<SettingsHost> host;
@property(nonatomic, copy) NSArray<SettingsSection*>* sections;
@property(nonatomic, strong, nullable) NSIndexPath* focus; // the row a controller is on
@property(nonatomic) BOOL popsOnChoice;                    // a choice's options: choosing goes back
@end

@implementation SettingsPage {
    BOOL _touch; // the row being activated was tapped (not reached with a controller)
}

- (instancetype)initWithTitle:(NSString*)title host:(id<SettingsHost>)host {
    if ((self = [super initWithStyle:UITableViewStyleInsetGrouped]) != nil) {
        self.title = title;
        _host = host;
    }
    return self;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.tableView.rowHeight = UITableViewAutomaticDimension;
    self.tableView.estimatedRowHeight = 48;
    // The keyboard drives the list through GCKeyboard, as a controller does; UIKit's own
    // keyboard focus would move it a second time.
    self.tableView.allowsFocus = NO;
    self.navigationItem.rightBarButtonItem =
        [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemClose target:self action:@selector(close)];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self refresh];
}

- (void)close {
    [self.host closeSettings];
}

- (SettingsRow*)rowAt:(NSIndexPath*)path {
    return self.sections[path.section].rows[path.row];
}

// Reload every row's value, keeping the controller's place.
- (void)refresh {
    [self.tableView reloadData];
    if (self.focus != nil) {
        [self.tableView selectRowAtIndexPath:self.focus animated:NO scrollPosition:UITableViewScrollPositionNone];
    }
}

- (NSInteger)numberOfSectionsInTableView:(UITableView*)tableView {
    return (NSInteger)self.sections.count;
}

- (NSInteger)tableView:(UITableView*)tableView numberOfRowsInSection:(NSInteger)section {
    return (NSInteger)self.sections[section].rows.count;
}

- (NSString*)tableView:(UITableView*)tableView titleForHeaderInSection:(NSInteger)section {
    return self.sections[section].header;
}

- (NSString*)tableView:(UITableView*)tableView titleForFooterInSection:(NSInteger)section {
    return self.sections[section].footer != nil ? self.sections[section].footer() : nil;
}

- (UITableViewCell*)tableView:(UITableView*)tableView cellForRowAtIndexPath:(NSIndexPath*)path {
    SettingsRow* row = [self rowAt:path];

    if (row.kind == RowKindSlider) {
        SliderCell* cell = [tableView dequeueReusableCellWithIdentifier:@"slider"]
                               ?: [[SliderCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:@"slider"];
        const Knob* knob = row.knob;
        FilterParams params = self.host.renderer.params;
        float value = *KnobValue(&params, knob);
        cell.title.text = row.title;
        cell.value.text = KnobText(knob, value);
        cell.slider.minimumValue = knob->min;
        cell.slider.maximumValue = knob->max;
        cell.slider.value = value;
        cell.slider.tag = (NSInteger)(knob - kKnobs);
        [cell.slider removeTarget:nil action:NULL forControlEvents:UIControlEventAllEvents];
        [cell.slider addTarget:self action:@selector(sliderChanged:) forControlEvents:UIControlEventValueChanged];
        return cell;
    }

    UITableViewCell* cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1 reuseIdentifier:nil];
    UIListContentConfiguration* content = [UIListContentConfiguration valueCellConfiguration];
    content.text = row.armed && row.confirmTitle != nil ? row.confirmTitle : row.title;
    content.secondaryText = row.detail != nil ? row.detail() : nil;
    if (row.symbol != nil) {
        content.image = [UIImage systemImageNamed:row.symbol];
    }
    switch (row.kind) {
        case RowKindToggle: {
            UISwitch* toggle = [UISwitch new];
            toggle.on = row.isOn();
            [toggle addAction:[UIAction actionWithHandler:^(UIAction* action) {
                        row.setOn(((UISwitch*)action.sender).on);
                    }]
                forControlEvents:UIControlEventValueChanged];
            cell.accessoryView = toggle;
            break;
        }
        case RowKindChoice:
            content.secondaryText = row.options[(NSUInteger)row.selected()];
            cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
            break;
        case RowKindPage:
            cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
            break;
        case RowKindAction:
            content.textProperties.color = row.destructive ? UIColor.systemRedColor : self.view.tintColor;
            if (row.checked != nil) {
                content.textProperties.color = UIColor.labelColor;
                cell.accessoryType = row.checked() ? UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone;
            }
            break;
        case RowKindText:
            content = [UIListContentConfiguration cellConfiguration];
            content.text = row.title;
            content.secondaryText = row.detail != nil ? row.detail() : nil;
            content.secondaryTextProperties.color = UIColor.secondaryLabelColor;
            content.textProperties.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
            break;
        default:
            break;
    }
    cell.contentConfiguration = content;
    if (!row.focusable) {
        cell.selectionStyle = UITableViewCellSelectionStyleNone;
    }
    return cell;
}

- (NSIndexPath*)tableView:(UITableView*)tableView willSelectRowAtIndexPath:(NSIndexPath*)path {
    return [self rowAt:path].focusable ? path : nil;
}

// A tap: as A on that row.
- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)path {
    self.focus = path;
    _touch = YES;
    [self activate];
    _touch = NO;
    if ([self rowAt:path].kind != RowKindSlider) {
        [tableView deselectRowAtIndexPath:path animated:YES];
        self.focus = nil;
    }
}

- (void)sliderChanged:(UISlider*)slider {
    const Knob* knob = &kKnobs[slider.tag];
    [self setKnob:knob value:slider.value updateSlider:NO];
}

- (void)setKnob:(const Knob*)knob value:(float)value updateSlider:(BOOL)updateSlider {
    Renderer* renderer = self.host.renderer;
    FilterParams params = renderer.params;
    value = KnobSnap(knob, fminf(fmaxf(value, knob->min), knob->max));
    *KnobValue(&params, knob) = value;
    renderer.params = params;
    [NSUserDefaults.standardUserDefaults setFloat:value forKey:knob->key];
    for (UITableViewCell* cell in self.tableView.visibleCells) {
        if ([cell isKindOfClass:SliderCell.class] && ((SliderCell*)cell).slider.tag == knob - kKnobs) {
            ((SliderCell*)cell).value.text = KnobText(knob, value);
            if (updateSlider) {
                ((SliderCell*)cell).slider.value = value;
            }
        }
    }
}

#pragma mark Controller

- (NSIndexPath*)firstFocusable {
    for (NSUInteger s = 0; s < self.sections.count; s++) {
        for (NSUInteger r = 0; r < self.sections[s].rows.count; r++) {
            if (self.sections[s].rows[r].focusable) {
                return [NSIndexPath indexPathForRow:(NSInteger)r inSection:(NSInteger)s];
            }
        }
    }
    return nil;
}

- (void)setFocusTo:(NSIndexPath*)path {
    if (self.focus != nil && ![self.focus isEqual:path]) {
        SettingsRow* old = [self rowAt:self.focus];
        if (old.armed) {
            old.armed = NO;
            [self.tableView reloadRowsAtIndexPaths:@[ self.focus ] withRowAnimation:UITableViewRowAnimationNone];
        }
    }
    self.focus = path;
    [self.tableView selectRowAtIndexPath:path animated:NO scrollPosition:UITableViewScrollPositionNone];
    [self.tableView scrollToRowAtIndexPath:path atScrollPosition:UITableViewScrollPositionNone animated:YES];
}

// Up and down: the next row that can be selected, across sections; the first
// press puts the focus on the page.
- (void)moveFocus:(NSInteger)delta {
    if (self.focus == nil) {
        NSIndexPath* first = [self firstFocusable];
        if (first != nil) {
            [self setFocusTo:first];
        }
        return;
    }
    NSMutableArray<NSIndexPath*>* paths = [NSMutableArray array];
    for (NSUInteger s = 0; s < self.sections.count; s++) {
        for (NSUInteger r = 0; r < self.sections[s].rows.count; r++) {
            if (self.sections[s].rows[r].focusable) {
                [paths addObject:[NSIndexPath indexPathForRow:(NSInteger)r inSection:(NSInteger)s]];
            }
        }
    }
    NSInteger at = (NSInteger)[paths indexOfObject:self.focus];
    NSInteger to = MAX(0, MIN((NSInteger)paths.count - 1, at + delta));
    if (at != NSNotFound && to != at) {
        [self setFocusTo:paths[(NSUInteger)to]];
    }
}

// The first row of the section after (delta 1) or before (-1) the focused one.
- (void)jumpSection:(NSInteger)delta {
    if (self.focus == nil) {
        [self moveFocus:1];
        return;
    }
    for (NSInteger s = self.focus.section + delta; s >= 0 && s < (NSInteger)self.sections.count; s += delta) {
        NSArray<SettingsRow*>* rows = self.sections[(NSUInteger)s].rows;
        for (NSUInteger r = 0; r < rows.count; r++) {
            if (rows[r].focusable) {
                [self setFocusTo:[NSIndexPath indexPathForRow:(NSInteger)r inSection:s]];
                return;
            }
        }
    }
}

// Left and right: switch, step through the options, move a slider (speed
// steps at a time; the rest take one).
- (void)adjust:(NSInteger)delta speed:(NSInteger)speed {
    if (self.focus == nil) {
        return;
    }
    SettingsRow* row = [self rowAt:self.focus];
    switch (row.kind) {
        case RowKindToggle:
            if (row.isOn() != (delta > 0)) {
                row.setOn(delta > 0);
                [self refresh];
            }
            break;
        case RowKindChoice: {
            NSInteger index = MAX(0, MIN((NSInteger)row.options.count - 1, row.selected() + delta));
            if (index != row.selected()) {
                row.select(index);
                [self refresh];
            }
            break;
        }
        case RowKindSlider: {
            FilterParams params = self.host.renderer.params;
            float step = fmaxf(row.knob->step, (row.knob->max - row.knob->min) / kSliderSteps);
            [self setKnob:row.knob value:*KnobValue(&params, row.knob) + step * (float)(delta * speed) updateSlider:YES];
            break;
        }
        default:
            break;
    }
}

// A (or a tap).
- (void)activate {
    if (self.focus == nil) {
        [self moveFocus:1];
        return;
    }
    SettingsRow* row = [self rowAt:self.focus];
    switch (row.kind) {
        case RowKindToggle:
            row.setOn(!row.isOn());
            [self refresh];
            break;
        case RowKindChoice:
            [self.navigationController pushViewController:[self choicePageFor:row] animated:YES];
            break;
        case RowKindPage:
            [self.navigationController pushViewController:row.open() animated:YES];
            break;
        case RowKindAction:
            if (row.destructive && !row.armed) {
                row.armed = YES;
                [self refresh];
                break;
            }
            row.armed = NO;
            row.run();
            if (self.popsOnChoice) {
                [self.navigationController popViewControllerAnimated:YES];
            } else if (self.isViewLoaded && self.view.window != nil) {
                [self refresh];
            }
            break;
        default:
            break;
    }
}

// A choice's options, the current one checked; choosing goes back.
- (SettingsPage*)choicePageFor:(SettingsRow*)choice {
    SettingsPage* page = [[SettingsPage alloc] initWithTitle:choice.title host:self.host];
    NSMutableArray<SettingsRow*>* rows = [NSMutableArray array];
    for (NSUInteger i = 0; i < choice.options.count; i++) {
        SettingsRow* option = [SettingsRow rowWithKind:RowKindAction title:choice.options[i]];
        option.checked = ^BOOL {
            return choice.selected() == (NSInteger)i;
        };
        option.run = ^{
            choice.select((NSInteger)i);
        };
        [rows addObject:option];
    }
    page.sections = @[ [SettingsSection header:nil rows:rows footer:nil] ];
    page.popsOnChoice = YES;
    page.focus = _touch ? nil : [NSIndexPath indexPathForRow:choice.selected() inSection:0];
    return page;
}

@end

#pragma mark The pages

static SettingsRow* Toggle(NSString* title, BOOL (^isOn)(void), void (^setOn)(BOOL)) {
    SettingsRow* row = [SettingsRow rowWithKind:RowKindToggle title:title];
    row.isOn = isOn;
    row.setOn = setOn;
    return row;
}

static SettingsRow* DefaultsToggle(NSString* title, NSString* key, void (^apply)(BOOL)) {
    return Toggle(
        title, ^BOOL { return [NSUserDefaults.standardUserDefaults boolForKey:key]; },
        ^(BOOL on) {
            [NSUserDefaults.standardUserDefaults setBool:on forKey:key];
            apply(on);
        });
}

static SettingsRow* Choice(NSString* title, NSArray<NSString*>* options, NSInteger (^selected)(void), void (^select)(NSInteger)) {
    SettingsRow* row = [SettingsRow rowWithKind:RowKindChoice title:title];
    row.options = options;
    row.selected = selected;
    row.select = select;
    return row;
}

static SettingsRow* Action(NSString* title, void (^run)(void)) {
    SettingsRow* row = [SettingsRow rowWithKind:RowKindAction title:title];
    row.run = run;
    return row;
}

static SettingsRow* Info(NSString* title, NSString* value) {
    SettingsRow* row = [SettingsRow rowWithKind:RowKindInfo title:title];
    row.detail = ^NSString* {
        return value;
    };
    return row;
}

static SettingsRow* Link(NSString* title, NSString* url) {
    SettingsRow* row = Action(title, ^{
        [UIApplication.sharedApplication openURL:[NSURL URLWithString:url] options:@{} completionHandler:nil];
    });
    row.detail = ^NSString* {
        return [NSURL URLWithString:url].host;
    };
    return row;
}

static SettingsPage* DisplayPage(id<SettingsHost> host) {
    SettingsPage* page = [[SettingsPage alloc] initWithTitle:@"Display" host:host];
    __weak id<SettingsHost> weakHost = host;
    NSMutableArray<NSString*>* filters = [NSMutableArray array];
    for (NSUInteger mode = 0; mode < FilterModeCount; mode++) {
        [filters addObject:FilterModeName(mode)];
    }
    NSMutableArray<SettingsSection*>* sections = [NSMutableArray array];
    [sections addObject:[SettingsSection header:@"Picture"
                                           rows:@[
                                               Choice(
                                                   @"Filter", filters, ^NSInteger { return (NSInteger)weakHost.renderer.filter; },
                                                   ^(NSInteger index) {
                                                       weakHost.renderer.filter = (FilterMode)index;
                                                       [NSUserDefaults.standardUserDefaults setInteger:index forKey:kFilterDefault];
                                                   }),
                                               Toggle(
                                                   @"Integer scaling", ^BOOL { return weakHost.renderer.integerScaling; },
                                                   ^(BOOL on) {
                                                       weakHost.renderer.integerScaling = on;
                                                       [NSUserDefaults.standardUserDefaults setBool:on forKey:kIntegerScalingDefault];
                                                   }),
                                               Toggle(
                                                   @"Draw on the GPU", ^BOOL { return weakHost.renderer.gpuPpuEnabled; },
                                                   ^(BOOL on) {
                                                       [weakHost.renderer enableGpuPpu:on];
                                                       [NSUserDefaults.standardUserDefaults setBool:weakHost.renderer.gpuPpuEnabled
                                                                                             forKey:kGpuPpuDefault];
                                                   }),
                                           ]
                                         footer:@"Integer scaling keeps every GBA pixel the same size. "
                                                 @"Off draws the game on the CPU."]];
    // The knobs, in their sections (as the macOS Display and Filters tabs).
    NSMutableArray<SettingsRow*>* rows = nil;
    for (size_t i = 0; i < KNOB_COUNT; i++) {
        if (kKnobs[i].section != nil) {
            rows = [NSMutableArray array];
            [sections addObject:[SettingsSection header:kKnobs[i].section rows:@[] footer:nil]];
            if (kKnobs[i].offset == offsetof(FilterParams, lcdResponseMs)) {
                [rows addObject:Toggle(
                                    @"LCD response (ghosting)", ^BOOL { return weakHost.renderer.lcdResponse; },
                                    ^(BOOL on) {
                                        weakHost.renderer.lcdResponse = on;
                                        [NSUserDefaults.standardUserDefaults setBool:on forKey:kLcdResponseDefault];
                                    })];
            }
        }
        SettingsRow* slider = [SettingsRow rowWithKind:RowKindSlider title:kKnobs[i].title];
        slider.knob = &kKnobs[i];
        [rows addObject:slider];
        sections.lastObject.rows = rows;
    }
    __weak SettingsPage* weakPage = page;
    SettingsRow* reset = Action(@"Reset Filter Settings", ^{
        for (size_t i = 0; i < KNOB_COUNT; i++) {
            [NSUserDefaults.standardUserDefaults removeObjectForKey:kKnobs[i].key];
        }
        weakHost.renderer.params = FilterParamsDefaults();
        [weakPage refresh];
    });
    [sections addObject:[SettingsSection header:nil rows:@[ reset ] footer:nil]];
    page.sections = sections;
    return page;
}

static SettingsPage* ControlsPage(id<SettingsHost> host) {
    SettingsPage* page = [[SettingsPage alloc] initWithTitle:@"Controls" host:host];
    __weak id<SettingsHost> weakHost = host;
    NSMutableArray<SettingsRow*>* connected = [NSMutableArray array];
    for (NSString* name in host.input.controllerNames) {
        [connected addObject:Info(name, @"")];
    }
    if (connected.count == 0) {
        [connected addObject:Info(@"None", @"")];
    }
    NSString* home = host.input.homeButtonStatus;
    if (home != nil) {
        SettingsRow* row = Action(@"Home Button…", ^{
            [weakHost.input openHomeButtonSettings];
        });
        row.detail = ^NSString* {
            return [home isEqualToString:@"System"] ? @"Opens the system overlay" : @"Opens these settings";
        };
        [connected addObject:row];
    }
    void (^touchChanged)(BOOL) = ^(BOOL on) {
        [NSNotificationCenter.defaultCenter postNotificationName:kTouchControlsChangedNotification object:nil];
    };
    page.sections = @[
        [SettingsSection header:@"Touch"
                           rows:@[
                               DefaultsToggle(@"Touch controls", kTouchControlsDefault, touchChanged),
                               DefaultsToggle(@"Haptics", kTouchHapticsDefault, touchChanged),
                           ]
                         footer:@"On-screen buttons, hidden while a gamepad is connected. Their ☰ opens the settings."],
        [SettingsSection header:@"Gamepad"
                           rows:@[ Choice(
                                    @"A and B", @[ @"By label (A is A)", @"By position (right is A)" ],
                                    ^NSInteger { return weakHost.input.gamepadLayout; },
                                    ^(NSInteger index) {
                                        weakHost.input.gamepadLayout = (GamepadLayout)index;
                                        [NSUserDefaults.standardUserDefaults setInteger:index forKey:kGamepadLayoutDefault];
                                    }) ]
                         footer:@"Nintendo pads use their labels either way; this changes Xbox and PlayStation pads."],
        [SettingsSection header:@"Connected"
                           rows:connected
                         footer:[home isEqualToString:@"System"]
                                    ? @"The system keeps Home for itself. In Home Button…, let apps handle it, and Home "
                                      @"opens these settings."
                                    : nil],
        [SettingsSection header:@"Gamepad Buttons"
                           rows:@[
                               Info(@"A, B (and X, Y)", @"A, B"),
                               Info(@"Shoulders and triggers", @"L, R"),
                               Info(@"D-pad or left stick", @"D-pad"),
                               Info(@"☰ Menu (press)", @"Start"),
                               Info(@"Options / View / Share", @"Select"),
                               Info(@"Right stick click (hold)", @"Fast-forward"),
                               Info(@"Home, or hold ☰", @"Settings"),
                           ]
                         footer:@"Start is given when ☰ is let go; held, ☰ opens the settings and the game sees no "
                                @"Start."],
        [SettingsSection header:@"Keyboard"
                           rows:@[
                               Info(@"Arrow keys", @"D-pad"),
                               Info(@"X, Z", @"A, B"),
                               Info(@"A, S", @"L, R"),
                               Info(@"Return", @"Start"),
                               Info(@"Delete or \\", @"Select"),
                               Info(@"Tab (hold)", @"Fast-forward"),
                               Info(@"Escape", @"Settings"),
                           ]
                         footer:nil],
    ];
    return page;
}

static SettingsPage* TimingPage(id<SettingsHost> host) {
    SettingsPage* page = [[SettingsPage alloc] initWithTitle:@"Timing" host:host];
    __weak id<SettingsHost> weakHost = host;
    __weak SettingsPage* weakPage = page;
    SettingsSection* section = [SettingsSection
        header:@"Timing"
          rows:@[ Choice(
                   @"Frame pacing", @[ @"Accurate (59.73 Hz)", @"Display sync (60 Hz, less lag)" ],
                   ^NSInteger { return [NSUserDefaults.standardUserDefaults integerForKey:kPacingDefault] == 0 ? 0 : 1; },
                   ^(NSInteger index) {
                       [NSUserDefaults.standardUserDefaults setInteger:index forKey:kPacingDefault];
                       [NSNotificationCenter.defaultCenter postNotificationName:kPacingChangedNotification object:nil];
                       // The display link needs a moment to measure the display.
                       dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 2), dispatch_get_main_queue(), ^{
                           [weakPage refresh];
                       });
                   }) ]
        footer:nil];
    section.footer = ^NSString* {
        return [weakHost pacingStatus];
    };
    page.sections = @[ section ];
    return page;
}

static SettingsPage* GamePage(id<SettingsHost> host) {
    SettingsPage* page = [[SettingsPage alloc] initWithTitle:@"Game" host:host];
    __weak id<SettingsHost> weakHost = host;
    NSDateFormatter* format = [NSDateFormatter new];
    format.dateStyle = NSDateFormatterShortStyle;
    format.timeStyle = NSDateFormatterShortStyle;
    format.doesRelativeDateFormatting = YES;

    SettingsRow* save = Action(@"Quick Save", ^{
        [weakHost quickSave];
    });
    SettingsRow* load = Action(@"Quick Load", ^{
        [weakHost quickLoad];
    });
    load.detail = ^NSString* {
        NSDate* date = [weakHost dateOfSlot:weakHost.stateSlot];
        return date != nil ? [format stringFromDate:date] : @"Empty";
    };
    SettingsRow* reset = Action(@"Reset Game", ^{
        [weakHost resetGame];
    });
    reset.destructive = YES;
    reset.confirmTitle = @"Press Again to Reset";
    NSMutableArray<SettingsSection*>* sections = [NSMutableArray array];
    [sections addObjectsFromArray:@[
        [SettingsSection header:@"Save States"
                           rows:@[
                               Choice(
                                   @"Slot", @[ @"Slot 1", @"Slot 2", @"Slot 3", @"Slot 4" ],
                                   ^NSInteger { return weakHost.stateSlot - 1; },
                                   ^(NSInteger index) { weakHost.stateSlot = index + 1; }),
                               save,
                               load,
                           ]
                         footer:@"A save state keeps the game exactly as it is. The game's own save, tmc.sav, and the "
                                @"states are in the Files app under The Minish Cap, to copy to and from a Mac."],
        [SettingsSection header:@"Play"
                           rows:@[ Toggle(
                                    @"Fast-forward", ^BOOL { return weakHost.input.fastForwardLatched; },
                                    ^(BOOL on) { weakHost.input.fastForwardLatched = on; }) ]
                         footer:@"Or hold the right stick button (Tab on a keyboard)."],
        [SettingsSection header:@"Sound"
                           rows:@[
                               DefaultsToggle(@"Mute", kMutedDefault, ^(BOOL on) { AudioSetMuted(on); }),
                               DefaultsToggle(@"High-quality audio", kHighQualityAudioDefault,
                                              ^(BOOL on) { AudioSetHighQuality(on); }),
                           ]
                         footer:@"High quality mixes at 48 kHz instead of the GBA's 15.7 kHz."],
        [SettingsSection header:nil rows:@[ reset ] footer:@"Back to the title screen, as A+B+Select+Start. The save is kept."],
    ]];
    if (!RomDataIsBaked()) {
        [sections addObject:[SettingsSection header:@"Game Data"
                                               rows:@[
                                                   Action(@"Change ROM…", ^{ [weakHost changeRom:NO]; }),
                                                   Action(@"Re-extract Game Data", ^{ [weakHost changeRom:YES]; }),
                                               ]
                                             footer:@"The graphics, maps and sound are extracted from your ROM. "
                                                    @"Re-extract uses the ROM chosen before."]];
    }
    page.sections = sections;
    return page;
}

static SettingsPage* AboutPage(id<SettingsHost> host) {
    SettingsPage* page = [[SettingsPage alloc] initWithTitle:@"About" host:host];
    NSString* version = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"] ?: @"?";
    SettingsRow* text = [SettingsRow rowWithKind:RowKindText title:@"The Minish Cap"];
    text.detail = ^NSString* {
        return [NSString stringWithFormat:@"Version %@\n\nA native port of the zeldaret/tmc decompilation of The Legend "
                                          @"of Zelda: The Minish Cap, for iPhone, iPad and Mac.",
                                          version];
    };
    NSString* note = RomDataIsBaked() ? @"The game belongs to Nintendo and Capcom. This development build has its data "
                                        @"built in; do not share it."
                                      : @"The game belongs to Nintendo and Capcom. This app contains none of its data: "
                                        @"it reads your own copy of the game.";
    page.sections = @[
        [SettingsSection header:nil rows:@[ text ] footer:nil],
        [SettingsSection header:@"Credits"
                           rows:@[
                               Link(@"Decompilation: zeldaret/tmc", @"https://github.com/zeldaret/tmc"),
                               Link(@"Port: awkitsune/tmc_macos", @"https://github.com/awkitsune/tmc_macos"),
                           ]
                         footer:note],
    ];
    return page;
}

static SettingsRow* PageRow(NSString* title, NSString* symbol, SettingsPage* (*make)(id<SettingsHost>), id<SettingsHost> host) {
    SettingsRow* row = [SettingsRow rowWithKind:RowKindPage title:title];
    row.symbol = symbol;
    __weak id<SettingsHost> weakHost = host;
    row.open = ^UIViewController* {
        return make(weakHost);
    };
    return row;
}

#pragma mark The sheet

@implementation SettingsController {
    __weak id<SettingsHost> _host;
    CADisplayLink* _poll;
    uint16_t _held;
    uint16_t _repeating;
    NSInteger _repeats; // of the direction held now
    CFTimeInterval _repeatAt;
}

- (instancetype)initWithHost:(id<SettingsHost>)host {
    SettingsPage* root = [[SettingsPage alloc] initWithTitle:@"Settings" host:host];
    NSString* room = [host roomName];
    root.sections = @[
        [SettingsSection header:room
                           rows:@[
                               PageRow(@"Display", @"display", DisplayPage, host),
                               PageRow(@"Controls", @"gamecontroller", ControlsPage, host),
                               PageRow(@"Timing", @"timer", TimingPage, host),
                               PageRow(@"Game", @"square.and.arrow.down.on.square", GamePage, host),
                               PageRow(@"About", @"info.circle", AboutPage, host),
                           ]
                         footer:@"D-pad moves, L and R jump between sections, A selects, B goes back and closes. In "
                                @"the game, press Home or hold ☰ (Escape on a keyboard), or tap the screen (☰ with "
                                @"touch controls on), to come back here."],
    ];
    if ((self = [super initWithRootViewController:root]) != nil) {
        _host = host;
        // Buttons already held (the one that opened the settings) count only once let go.
        _held = [host.input pollKeys];
    }
    return self;
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    if (_poll == nil) {
        _poll = [CADisplayLink displayLinkWithTarget:self selector:@selector(poll:)];
        [_poll addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
    }
}

- (void)viewDidDisappear:(BOOL)animated {
    [super viewDidDisappear:animated];
    [_poll invalidate];
    _poll = nil;
}

- (void)poll:(CADisplayLink*)link {
    [self handleKeys:[_host.input pollKeys]];
}

- (SettingsPage*)page {
    return [self.topViewController isKindOfClass:SettingsPage.class] ? (SettingsPage*)self.topViewController : nil;
}

- (void)handleKeys:(uint16_t)keys {
    uint16_t pressed = keys & ~_held;
    const uint16_t directions = HW_KEY_UP | HW_KEY_DOWN | HW_KEY_LEFT | HW_KEY_RIGHT;
    CFTimeInterval now = CACurrentMediaTime();

    _held = keys;
    // A direction held down repeats.
    if (pressed & directions) {
        _repeating = pressed & directions;
        _repeats = 0;
        _repeatAt = now + kRepeatDelay;
    } else if (_repeating != 0 && (keys & _repeating) == _repeating && now >= _repeatAt) {
        pressed |= _repeating;
        _repeats++;
        _repeatAt = now + kRepeatSeconds;
    } else if ((keys & _repeating) != _repeating) {
        _repeating = 0;
    }
    // Nothing while a ROM picker or an alert is over the sheet.
    if (pressed == 0 || self.transitionCoordinator != nil || self.presentedViewController != nil) {
        return;
    }
    SettingsPage* page = self.page;
    if (pressed & HW_KEY_UP) {
        [page moveFocus:-1];
    } else if (pressed & HW_KEY_DOWN) {
        [page moveFocus:1];
    } else if (pressed & (HW_KEY_LEFT | HW_KEY_RIGHT)) {
        NSInteger speed = MIN(kSliderMaxSpeed, 1 + _repeats / kSliderSpeedUp);
        [page adjust:(pressed & HW_KEY_LEFT) ? -1 : 1 speed:speed];
    } else if (pressed & HW_KEY_L) {
        [page jumpSection:-1];
    } else if (pressed & HW_KEY_R) {
        [page jumpSection:1];
    } else if (pressed & HW_KEY_A) {
        [page activate];
    } else if (pressed & HW_KEY_B) {
        if (self.viewControllers.count > 1) {
            [self popViewControllerAnimated:YES];
        } else {
            [_host closeSettings];
        }
    } else if (pressed & HW_KEY_START) {
        [_host closeSettings];
    }
}

- (NSString*)pageTitle {
    return self.topViewController.title ?: @"";
}

- (NSString*)focusedRowTitle {
    SettingsPage* page = self.page;
    return page.focus != nil ? [page rowAt:page.focus].title : nil;
}

- (NSDictionary<NSString*, NSArray<NSString*>*>*)allPageRows {
    NSMutableDictionary* pages = [NSMutableDictionary dictionary];
    SettingsPage* root = (SettingsPage*)self.viewControllers.firstObject;
    NSMutableArray<SettingsPage*>* todo = [NSMutableArray arrayWithObject:root];
    while (todo.count > 0) {
        SettingsPage* page = todo.lastObject;
        [todo removeLastObject];
        NSMutableArray<NSString*>* titles = [NSMutableArray array];
        for (SettingsSection* section in page.sections) {
            for (SettingsRow* row in section.rows) {
                [titles addObject:row.title];
                if (row.kind == RowKindPage) {
                    [todo addObject:(SettingsPage*)row.open()];
                }
            }
        }
        pages[page.title] = titles;
    }
    return pages;
}

@end
