/*
 * Display sync on macOS: a CVDisplayLink on the game window's display drives
 * the shared timing (displaysynccore.h).
 */

#import "displaysync.h"
#import "displaysynccore.h"

#import <CoreVideo/CoreVideo.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

static CVReturn LinkCallback(CVDisplayLinkRef link, const CVTimeStamp* now, const CVTimeStamp* output, CVOptionFlags flagsIn,
                             CVOptionFlags* flagsOut, void* context) {
    DisplaySyncTick(output->hostTime);
    return kCVReturnSuccess;
}

@implementation DisplaySync {
    CVDisplayLinkRef _link;
    NSWindow* _window;
    BOOL _enabled;
}

- (instancetype)init {
    if ((self = [super init]) != nil) {
        DisplaySyncCoreInit();
    }
    return self;
}

- (void)dealloc {
    if (_link != NULL) {
        CVDisplayLinkStop(_link);
        CVDisplayLinkRelease(_link);
    }
    [NSNotificationCenter.defaultCenter removeObserver:self];
}

- (void)setUsableChanged:(void (^)(void))usableChanged {
    _usableChanged = [usableChanged copy];
    DisplaySyncSetUsableChanged(_usableChanged);
}

- (CGDirectDisplayID)displayID {
    NSNumber* number = _window.screen.deviceDescription[@"NSScreenNumber"];
    return number != nil ? (CGDirectDisplayID)number.unsignedIntValue : CGMainDisplayID();
}

- (void)attachToWindow:(NSWindow*)window {
    _window = window;
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(screenChanged:)
                                               name:NSWindowDidChangeScreenNotification
                                             object:window];
}

- (void)screenChanged:(NSNotification*)note {
    if (_link != NULL) {
        BOOL running = CVDisplayLinkIsRunning(_link);
        CVDisplayLinkSetCurrentCGDisplay(_link, self.displayID);
        [self forgetPeriod];
        if (running && !CVDisplayLinkIsRunning(_link)) {
            CVDisplayLinkStart(_link);
        }
    }
}

// A different display may refresh at another rate: measure again.
- (void)forgetPeriod {
    DisplaySyncForget();
    if (_usableChanged != nil) {
        _usableChanged();
    }
}

- (BOOL)enabled {
    return _enabled;
}

- (void)setEnabled:(BOOL)enabled {
    if (enabled == _enabled) {
        return;
    }
    _enabled = enabled;
    if (enabled) {
        if (_link == NULL && CVDisplayLinkCreateWithCGDisplay(self.displayID, &_link) == kCVReturnSuccess) {
            CVDisplayLinkSetOutputCallback(_link, LinkCallback, NULL);
        }
        if (_link != NULL) {
            [self forgetPeriod];
            CVDisplayLinkStart(_link);
        }
    } else {
        if (_link != NULL) {
            CVDisplayLinkStop(_link);
        }
        DisplaySyncStopped();
        if (_usableChanged != nil) {
            _usableChanged();
        }
    }
}

- (NSString*)statusText {
    return DisplaySyncStatus(_enabled, _link != NULL);
}

@end

#pragma clang diagnostic pop
