/*
 * The game screen on iOS (see gameview.h). The game itself runs on its own
 * thread (apple/game.c), as on macOS.
 */

#import "gameview.h"

#import "audiosession.h"
#import "displaysync.h"
#import "selftest.h"
#import "settingsstore.h"
#import "settingsview.h"
#import "touchcontrols.h"

#include <fcntl.h>
#include <stdatomic.h>
#include <unistd.h>

#include "audio.h"
#include "frontend.h"
#include "latency.h"
#include "port/state.h"
#include "roomname.h"

static NSString* const kMutedDefault = @"Muted";
static NSString* const kHighQualityAudioDefault = @"HighQualityAudio";
static NSString* const kStateSlotDefault = @"StateSlot";
#define STATE_SLOTS 4

@interface GameViewController () <SettingsHost, UIAdaptivePresentationControllerDelegate>
- (void)drawPublishedFrame;
- (void)stateFinished:(BOOL)ok what:(NSString*)what message:(NSString*)message frame:(uint64_t)frame;
@end

// The game thread's input poll, right before the game reads its keys.
static uint16_t PollInput(void* context) {
    Input* input = (__bridge Input*)context;
    FrontendSetFastForward(input.fastForwardHeld);
    return [input pollKeys];
}

// Called on the game thread after a frame paced by the display: draw it now.
static void FramePublished(void* context) {
    [(__bridge GameViewController*)context drawPublishedFrame];
}

// FrontendRequestState's callback, on the game thread: report on the main one.
typedef struct {
    void* controller; // GameViewController, retained
    void* what;       // NSString, retained
} StateContext;

static void StateDone(bool ok, const char* message, uint64_t frame, void* context) {
    StateContext* state = context;
    GameViewController* controller = (__bridge_transfer GameViewController*)state->controller;
    NSString* what = (__bridge_transfer NSString*)state->what;
    NSString* text = @(message);
    free(state);
    dispatch_async(dispatch_get_main_queue(), ^{
        [controller stateFinished:ok what:what message:text frame:frame];
    });
}

@implementation GameViewController {
    Input* _input;
    MTKView* _view;
    Renderer* _renderer;
    TouchControlsView* _touch;
    UITapGestureRecognizer* _tap;
    BOOL _hadController;
    DisplaySync* _displaySync;
    atomic_bool _drawQueued;
    CFTimeInterval _lastDraw;
    NSTimer* _idle;
    PauseReason _pause;
    UIView* _overlay;
    UILabel* _overlayDetail;
    UIView* _toast;
    UILabel* _toastLabel;
    NSUInteger _toastSerial;
    SettingsController* _settings;
    CFTimeInterval _resumeBy; // after the settings close: resume once the buttons are let go, or by then
    BOOL _started;
}

#pragma mark Files

+ (NSString*)documents {
    return [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject.path;
}

+ (NSString*)savePath {
    return [self.documents stringByAppendingPathComponent:@"tmc.sav"];
}

+ (NSString*)stateFolder {
    NSString* dir = [self.documents stringByAppendingPathComponent:@"states"];
    [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    return dir;
}

- (NSString*)pathOfSlot:(NSInteger)slot {
    return [GameViewController.stateFolder
        stringByAppendingPathComponent:[NSString stringWithFormat:@"slot%ld.%s", (long)slot, STATE_EXTENSION]];
}

#pragma mark View

- (void)loadView {
    UIView* root = [UIView new];
    root.backgroundColor = UIColor.blackColor;
    self.view = root;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
    [defaults registerDefaults:@{
        kFilterDefault : @(FilterModeSharpBilinear),
        kIntegerScalingDefault : @YES,
        kMutedDefault : @NO,
        kHighQualityAudioDefault : @NO,
        kStateSlotDefault : @1,
        kPacingDefault : @(FRONTEND_PACING_DISPLAY_SYNC),
        kTouchControlsDefault : @NO,
        kTouchHapticsDefault : @YES,
        kGpuPpuDefault : @YES,
    }];
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(applyPacing)
                                               name:kPacingChangedNotification
                                             object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(applyControllers)
                                               name:kTouchControlsChangedNotification
                                             object:nil];

    _input = [Input new];
    _input.gamepadLayout = [defaults integerForKey:kGamepadLayoutDefault] == GamepadLayoutByPosition ? GamepadLayoutByPosition
                                                                                                     : GamepadLayoutByLabel;
    __weak GameViewController* weakSelf = self;
    _input.controllersChanged = ^{
        [weakSelf controllersChanged];
    };
    _input.settingsRequested = ^{
        GameViewController* strongSelf = weakSelf;
        if (strongSelf.settingsShown) {
            [strongSelf closeSettings];
        } else {
            [strongSelf showSettings];
        }
    };

    // The game keeps clear of the notch and the rounded corners.
    _view = [[MTKView alloc] initWithFrame:CGRectZero device:MTLCreateSystemDefaultDevice()];
    _view.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
    _view.clearColor = MTLClearColorMake(0, 0, 0, 1);
    _view.preferredFramesPerSecond = 60;
    _view.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:_view];
    // The whole screen, not the safe area: the picture is centred in it, and the
    // home indicator (hidden while playing) may sit over its bottom edge.
    [NSLayoutConstraint activateConstraints:@[
        [_view.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [_view.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [_view.topAnchor constraintEqualToAnchor:self.view.topAnchor],
        [_view.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
    ]];
    _tap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(tapped:)];
    _tap.enabled = !self.touchControlsOn;
    [self.view addGestureRecognizer:_tap];
    _touch = [[TouchControlsView alloc] initWithInput:_input];
    _touch.frame = self.view.bounds;
    _touch.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    _touch.hidden = YES;
    _touch.menuTapped = ^{
        [weakSelf showSettings];
    };
    _touch.clusterMoved = ^(TouchCluster cluster, CGPoint offset) {
        GameViewController* strongSelf = weakSelf;
        TouchSettings settings = TouchSettingsLoad(strongSelf.portrait);
        settings.layout.offsets[cluster] = offset;
        TouchSettingsSave(strongSelf.portrait, &settings);
        [strongSelf layoutPicture];
    };
    _touch.editReset = ^{
        GameViewController* strongSelf = weakSelf;
        TouchSettings settings = TouchSettingsLoad(strongSelf.portrait);
        memset(settings.layout.offsets, 0, sizeof(settings.layout.offsets));
        TouchSettingsSave(strongSelf.portrait, &settings);
        [strongSelf layoutPicture];
    };
    _touch.editDone = ^{
        [weakSelf endTouchEditing];
    };
    [self.view addSubview:_touch];
    [self buildOverlay];
    [self buildToast];

    if (_view.device == nil) {
        [self fail:@"This device has no Metal GPU."];
        return;
    }
    _renderer = [[Renderer alloc] initWithView:_view];
    if (_renderer == nil) {
        [self fail:@"The Metal renderer could not be created."];
        return;
    }
    _renderer.filter = MIN((NSUInteger)[defaults integerForKey:kFilterDefault], FilterModeCount - 1);
    _renderer.integerScaling = [defaults boolForKey:kIntegerScalingDefault];
    _renderer.params = LoadFilterParams();
    _renderer.lcdResponse = [defaults boolForKey:kLcdResponseDefault];
    // Without the GPU PPU (or if Metal later fails to draw a frame) the
    // software renderer draws the game.
    if ([defaults boolForKey:kGpuPpuDefault]) {
        [_renderer enableGpuPpu:YES];
    }
    _renderer.beforeDraw = ^{
        GameViewController* strongSelf = weakSelf;
        if (strongSelf != nil) {
            strongSelf->_lastDraw = CACurrentMediaTime();
        }
    };
    _view.delegate = _renderer;
    [self startGame];
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    [self becomeFirstResponder];
}

- (BOOL)canBecomeFirstResponder {
    return YES;
}

- (BOOL)prefersStatusBarHidden {
    return YES;
}

- (BOOL)prefersHomeIndicatorAutoHidden {
    return YES;
}

// A press near the bottom edge goes to the touch controls at once, not after
// the system has waited to see whether it is the home gesture.
- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures {
    return _touch.hidden ? UIRectEdgeNone : UIRectEdgeAll;
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    [self layoutPicture];
}

- (UIInterfaceOrientationMask)supportedInterfaceOrientations {
    return UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad ? UIInterfaceOrientationMaskAll
                                                                                 : UIInterfaceOrientationMaskAllButUpsideDown;
}

- (Renderer*)renderer {
    return _renderer;
}

- (Input*)input {
    return _input;
}

- (MTKView*)gameView {
    return _view;
}

- (TouchControlsView*)touchControls {
    return _touch;
}

- (void)fail:(NSString*)message {
    NSLog(@"cannot start: %@", message);
    UILabel* label = [UILabel new];
    label.text = message;
    label.textColor = UIColor.whiteColor;
    label.textAlignment = NSTextAlignmentCenter;
    label.numberOfLines = 0;
    label.frame = self.view.bounds;
    label.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [self.view addSubview:label];
    _overlay.hidden = YES;
}

#pragma mark Game

- (void)startGame {
    NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
    __weak GameViewController* weakSelf = self;

    FrontendSetInputPoll(PollInput, (__bridge void*)_input);
    FrontendSetFrameHandler(FramePublished, (__bridge void*)self);
    LatencyStart();
    _displaySync = [DisplaySync new];
    _displaySync.usableChanged = ^{
        [weakSelf updateDrawMode];
    };
    [self applyPacing];
    // With the view paused (display sync) nothing draws it unless a game frame
    // comes in: this covers rotation, pausing and a display that stalls.
    [self startIdleTimer];

    FrontendSetSavePath(GameViewController.savePath.fileSystemRepresentation);
    AudioSetMuted([defaults boolForKey:kMutedDefault]);
    AudioSetHighQuality([defaults boolForKey:kHighQualityAudioDefault]);
    if (!AudioSessionStart()) {
        NSLog(@"no sound output: the game runs silently");
    }
    _hadController = _input.hasController;
    [self applyControllers];
    FrontendStartGame();
    _started = YES;

    // TMC_LOAD_STATE=path: start from a save state, as the headless runner does.
    NSString* loadState = NSProcessInfo.processInfo.environment[@"TMC_LOAD_STATE"];
    if (loadState.length != 0) {
        [self requestState:NO path:loadState.stringByExpandingTildeInPath
                      what:[@"Loaded " stringByAppendingString:loadState.lastPathComponent]];
    }
    if (NSProcessInfo.processInfo.environment[@"TMC_SELFTEST"].length != 0) {
        SelfTestStart(self);
    }
}

- (void)startIdleTimer {
    if (_idle != nil) {
        return;
    }
    __weak GameViewController* weakSelf = self;
    _idle = [NSTimer timerWithTimeInterval:0.05
                                   repeats:YES
                                     block:^(NSTimer* timer) {
                                         [weakSelf idleTick];
                                     }];
    [NSRunLoop.mainRunLoop addTimer:_idle forMode:NSRunLoopCommonModes];
}

- (void)stopIdleTimer {
    [_idle invalidate];
    _idle = nil;
}

- (PauseReason)pauseReasons {
    return _pause;
}

- (void)setPause:(PauseReason)reason on:(BOOL)on {
    _pause = on ? _pause | reason : _pause & ~reason;
    [self applyPause];
}

- (void)applyPause {
    BOOL paused = _pause != 0;
    if (FrontendIsPaused() != paused) {
        FrontendSetPaused(paused);
    }
    _overlay.hidden = (_pause & PauseReasonNoController) == 0 || _renderer == nil;
    // A gamepad does not count as touching the screen: keep it on while playing.
    UIApplication.sharedApplication.idleTimerDisabled = !paused;
}

#pragma mark Pacing

- (FrontendPacing)pacing {
    NSString* forced = NSProcessInfo.processInfo.environment[@"TMC_PACING"];
    if ([forced isEqualToString:@"accurate"]) {
        return FRONTEND_PACING_ACCURATE;
    }
    if ([forced isEqualToString:@"sync"]) {
        return FRONTEND_PACING_DISPLAY_SYNC;
    }
    return [NSUserDefaults.standardUserDefaults integerForKey:kPacingDefault] == FRONTEND_PACING_ACCURATE
               ? FRONTEND_PACING_ACCURATE
               : FRONTEND_PACING_DISPLAY_SYNC;
}

- (void)applyPacing {
    FrontendPacing pacing = self.pacing;

    FrontendSetPacing(pacing);
    _displaySync.enabled = pacing == FRONTEND_PACING_DISPLAY_SYNC;
    [self updateDrawMode];
    LatencyNote(pacing == FRONTEND_PACING_DISPLAY_SYNC ? "pacing: display sync" : "pacing: accurate");
}

// The view draws by itself, at the display's rate, except while the game
// follows the display: then it is drawn when a frame is ready (see below).
- (void)updateDrawMode {
    BOOL paused = _displaySync.enabled && DisplaySyncActive();
    if (_view.paused != paused) {
        _view.paused = paused;
    }
}

// Game thread: a frame that a display refresh is waiting for is ready.
- (void)drawPublishedFrame {
    if (!DisplaySyncActive() || atomic_exchange(&_drawQueued, true)) {
        return;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        atomic_store(&self->_drawQueued, false);
        if (self->_view.paused && (self->_pause & PauseReasonBackground) == 0) {
            [self->_view draw];
        }
    });
}

- (void)idleTick {
    [self updateDrawMode];
    if (_view.paused && CACurrentMediaTime() - _lastDraw > 0.05) {
        [_view draw];
    }
    // After the settings: play on once the button that closed them is let go.
    if (_resumeBy != 0 && ([_input pollKeys] == 0 || CACurrentMediaTime() > _resumeBy)) {
        _resumeBy = 0;
        [self setPause:PauseReasonSettings on:NO];
    }
}

- (NSString*)pacingStatus {
    if (self.pacing == FRONTEND_PACING_ACCURATE) {
        return @"The game keeps to the GBA's own 59.73 Hz.";
    }
    return _displaySync.statusText;
}

#pragma mark Controllers

- (void)buildOverlay {
    UIImageView* icon = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"gamecontroller"]];
    icon.tintColor = UIColor.whiteColor;
    icon.preferredSymbolConfiguration = [UIImageSymbolConfiguration configurationWithPointSize:56];
    UILabel* title = [UILabel new];
    title.text = @"Connect a controller";
    title.font = [UIFont preferredFontForTextStyle:UIFontTextStyleTitle2];
    title.adjustsFontForContentSizeCategory = YES;
    title.textColor = UIColor.whiteColor;
    _overlayDetail = [UILabel new];
    _overlayDetail.text = @"Pair a gamepad in Settings › Bluetooth, or attach a keyboard. The game goes on when one "
                          @"is connected.\nTap the screen for the settings, where Controls can turn on touch controls.";
    _overlayDetail.font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
    _overlayDetail.adjustsFontForContentSizeCategory = YES;
    _overlayDetail.textColor = [UIColor colorWithWhite:1 alpha:0.75];
    _overlayDetail.textAlignment = NSTextAlignmentCenter;
    _overlayDetail.numberOfLines = 0;
    UIStackView* stack = [[UIStackView alloc] initWithArrangedSubviews:@[ icon, title, _overlayDetail ]];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.alignment = UIStackViewAlignmentCenter;
    stack.spacing = 12;
    stack.translatesAutoresizingMaskIntoConstraints = NO;

    _overlay = [UIView new];
    _overlay.backgroundColor = [UIColor colorWithWhite:0 alpha:0.72];
    _overlay.translatesAutoresizingMaskIntoConstraints = NO;
    _overlay.hidden = YES;
    [_overlay addSubview:stack];
    [self.view addSubview:_overlay];
    [NSLayoutConstraint activateConstraints:@[
        [_overlay.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [_overlay.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [_overlay.topAnchor constraintEqualToAnchor:self.view.topAnchor],
        [_overlay.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
        [stack.centerXAnchor constraintEqualToAnchor:_overlay.centerXAnchor],
        [stack.centerYAnchor constraintEqualToAnchor:_overlay.centerYAnchor],
        [stack.widthAnchor constraintLessThanOrEqualToConstant:420],
        [stack.leadingAnchor constraintGreaterThanOrEqualToAnchor:_overlay.safeAreaLayoutGuide.leadingAnchor constant:24],
        [stack.trailingAnchor constraintLessThanOrEqualToAnchor:_overlay.safeAreaLayoutGuide.trailingAnchor constant:-24],
    ]];
}

- (BOOL)controllerOverlayShown {
    return !_overlay.hidden;
}

- (void)controllersChanged {
    BOOL has = _input.hasController;
    if (has != _hadController) {
        _hadController = has;
        [self showStatus:has ? [NSString stringWithFormat:@"%@ connected", _input.controllerNames.firstObject ?: @"Controller"]
                             : @"Controller disconnected"];
    }
    [self applyControllers];
    [_input releaseAll];
}

#pragma mark Touch controls

- (BOOL)touchControlsOn {
    return [NSUserDefaults.standardUserDefaults boolForKey:kTouchControlsDefault];
}

- (BOOL)tapOpensSettings {
    return _tap.enabled;
}

// With touch controls on, the game needs no controller and ☰ takes the place of
// a tap; they hide (and the picture has the screen) while a gamepad is there.
- (void)applyControllers {
    BOOL touch = self.touchControlsOn;
    _tap.enabled = !touch;
    _touch.haptics = [NSUserDefaults.standardUserDefaults boolForKey:kTouchHapticsDefault];
    _touch.hidden = !touch || _input.hasGamepad || _renderer == nil;
    if (_touch.hidden) {
        [self endTouchEditing];
    }
    [self setPause:PauseReasonNoController on:!touch && !_input.hasController];
    [self layoutPicture];
    [self setNeedsUpdateOfScreenEdgesDeferringSystemGestures];
}

- (BOOL)portrait {
    CGSize size = self.view.bounds.size;
    return size.height > size.width;
}

- (void)editTouchControls {
    [self closeSettings];
    if (_touch.hidden) {
        [self showStatus:_input.hasGamepad ? @"The touch controls are hidden while a gamepad is connected"
                                           : @"Turn on the touch controls first"];
        return;
    }
    _touch.editing = YES;
    [self setPause:PauseReasonEditing on:YES];
}

- (void)endTouchEditing {
    _touch.editing = NO;
    [self setPause:PauseReasonEditing on:NO];
}

// The picture and the touch controls, with the player's touch settings for
// this orientation and the renderer's integer scaling.
- (void)layoutPicture {
    CGSize size = self.view.bounds.size;
    if (size.width <= 0 || size.height <= 0) {
        return;
    }
    CGFloat scale = _view.contentScaleFactor;
    TouchSettings settings = TouchSettingsLoad(self.portrait);
    settings.layout.integerScaling = _renderer == nil || _renderer.integerScaling;
    TouchLayout layout = TouchLayoutMake(size, self.view.safeAreaInsets, self.portrait,
                                         UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad, scale,
                                         &settings.layout);
    _touch.layout = layout;
    _touch.opacity = settings.opacity;
    _renderer.pictureRect = _touch.hidden ? CGRectZero : TouchLayoutPixelRect(layout.picture, scale);
}

#pragma mark Status

- (void)buildToast {
    _toast = [UIView new];
    _toast.backgroundColor = [UIColor colorWithWhite:0.1 alpha:0.85];
    _toast.layer.cornerRadius = 14;
    _toast.layer.cornerCurve = kCACornerCurveContinuous;
    _toast.alpha = 0;
    _toast.userInteractionEnabled = NO;
    _toast.translatesAutoresizingMaskIntoConstraints = NO;
    _toastLabel = [UILabel new];
    _toastLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
    _toastLabel.textColor = UIColor.whiteColor;
    _toastLabel.translatesAutoresizingMaskIntoConstraints = NO;
    [_toast addSubview:_toastLabel];
    [self.view addSubview:_toast];
    [NSLayoutConstraint activateConstraints:@[
        [_toastLabel.leadingAnchor constraintEqualToAnchor:_toast.leadingAnchor constant:14],
        [_toastLabel.trailingAnchor constraintEqualToAnchor:_toast.trailingAnchor constant:-14],
        [_toastLabel.topAnchor constraintEqualToAnchor:_toast.topAnchor constant:8],
        [_toastLabel.bottomAnchor constraintEqualToAnchor:_toast.bottomAnchor constant:-8],
        [_toast.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor],
        [_toast.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor constant:12],
    ]];
}

- (void)showStatus:(NSString*)text {
    NSLog(@"status: %@", text);
    NSUInteger serial = ++_toastSerial;
    _toastLabel.text = text;
    [UIView animateWithDuration:0.2 animations:^{
        self->_toast.alpha = 1;
    }];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        if (self->_toastSerial == serial) {
            [UIView animateWithDuration:0.4 animations:^{
                self->_toast.alpha = 0;
            }];
        }
    });
}

#pragma mark Settings

- (void)tapped:(UITapGestureRecognizer*)tap {
    if (tap.state == UIGestureRecognizerStateEnded && _started) {
        [self showSettings];
    }
}

- (BOOL)settingsShown {
    return _settings != nil;
}

- (UIViewController*)settingsController {
    return _settings;
}

- (void)showSettings {
    if (_settings != nil || _renderer == nil || self.presentedViewController != nil) {
        return;
    }
    _resumeBy = 0;
    [self endTouchEditing];
    [_touch releaseAll];
    [self setPause:PauseReasonSettings on:YES];
    _settings = [[SettingsController alloc] initWithHost:self];
    // A sheet that can be pulled up on iPhone; a centred card on iPad.
    _settings.modalPresentationStyle = UIModalPresentationFormSheet;
    UISheetPresentationController* sheet = _settings.sheetPresentationController;
    sheet.detents = @[ UISheetPresentationControllerDetent.mediumDetent, UISheetPresentationControllerDetent.largeDetent ];
    sheet.prefersGrabberVisible = YES;
    sheet.prefersEdgeAttachedInCompactHeight = YES;
    sheet.widthFollowsPreferredContentSizeWhenEdgeAttached = YES;
    _settings.presentationController.delegate = self;
    [self presentViewController:_settings animated:YES completion:nil];
}

- (void)closeSettings {
    if (_settings == nil) {
        return;
    }
    [self dismissViewControllerAnimated:YES completion:nil];
    [self settingsClosed];
}

// Swiped down.
- (void)presentationControllerDidDismiss:(UIPresentationController*)presentationController {
    [self settingsClosed];
}

- (void)settingsClosed {
    _settings = nil;
    _resumeBy = CACurrentMediaTime() + 1;
    [self layoutPicture]; // ⏩ shows the Fast-forward setting
}

- (NSString*)roomName {
    int area, room;
    const char* name = RoomCurrent(&area, &room) ? RoomName(area, room) : NULL;
    return name != NULL ? @(name) : nil;
}

- (NSInteger)stateSlot {
    return MAX(1, MIN(STATE_SLOTS, [NSUserDefaults.standardUserDefaults integerForKey:kStateSlotDefault]));
}

- (void)setStateSlot:(NSInteger)slot {
    [NSUserDefaults.standardUserDefaults setInteger:MAX(1, MIN(STATE_SLOTS, slot)) forKey:kStateSlotDefault];
}

- (NSDate*)dateOfSlot:(NSInteger)slot {
    return [NSFileManager.defaultManager attributesOfItemAtPath:[self pathOfSlot:slot] error:nil].fileModificationDate;
}

- (void)quickSave {
    [self requestState:YES path:[self pathOfSlot:self.stateSlot]
                  what:[NSString stringWithFormat:@"Saved slot %ld", (long)self.stateSlot]];
}

- (void)quickLoad {
    NSString* path = [self pathOfSlot:self.stateSlot];
    if (![NSFileManager.defaultManager fileExistsAtPath:path]) {
        [self showStatus:[NSString stringWithFormat:@"Slot %ld is empty", (long)self.stateSlot]];
        return;
    }
    [self requestState:NO path:path what:[NSString stringWithFormat:@"Loaded slot %ld", (long)self.stateSlot]];
    [self closeSettings];
}

- (void)resetGame {
    FrontendRequestReset();
    [self closeSettings];
}

#pragma mark Save states

- (void)requestState:(BOOL)save path:(NSString*)path what:(NSString*)what {
    StateContext* context = malloc(sizeof(StateContext));
    context->controller = (__bridge_retained void*)self;
    context->what = (__bridge_retained void*)what;
    if (!FrontendRequestState(save, path.fileSystemRepresentation, StateDone, context)) {
        (void)(__bridge_transfer GameViewController*)context->controller;
        (void)(__bridge_transfer NSString*)context->what;
        free(context);
        [self showStatus:@"Busy with the last save or load"];
    }
}

- (void)stateFinished:(BOOL)ok what:(NSString*)what message:(NSString*)message frame:(uint64_t)frame {
    if (!ok) {
        NSLog(@"%@ failed: %@", what, message);
        [self showStatus:[NSString stringWithFormat:@"Save state failed: %@", message]];
        return;
    }
    if (message.length != 0) {
        NSLog(@"%@: %@", what, message); // loaded from another build
    }
    NSLog(@"%@ (frame %llu)", what, (unsigned long long)frame);
    [self showStatus:[NSString stringWithFormat:@"%@%@", what, message.length != 0 ? @" (from another build)" : @""]];
}

#pragma mark Life cycle

// The EEPROM file is written through as the game saves; make sure it is on
// disk before iOS may end the app.
- (void)flushSave {
    if (!_started) {
        return;
    }
    if (!FrontendWaitUntilPaused(100)) {
        NSLog(@"the game did not pause in time; the save is flushed as it stands");
    }
    int fd = open(GameViewController.savePath.fileSystemRepresentation, O_RDONLY);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
}

- (void)sceneWillResignActive {
    [self setPause:PauseReasonInactive on:YES];
    [self flushSave];
    [_input releaseAll];
    [_touch releaseAll];
}

- (void)sceneDidBecomeActive {
    [self setPause:PauseReasonInactive on:NO];
}

- (void)sceneDidEnterBackground {
    [self setPause:PauseReasonBackground on:YES];
    [self stopIdleTimer];
    AudioSessionSetForeground(NO);
}

- (void)sceneWillEnterForeground {
    AudioSessionSetForeground(YES);
    if (_started) {
        [self startIdleTimer];
    }
    [self setPause:PauseReasonBackground on:NO];
}

@end
