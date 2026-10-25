/*
 * Display sync on iOS: a CADisplayLink, on a thread of its own so a busy main
 * thread does not delay its ticks, drives the shared timing
 * (displaysynccore.h). It asks for 60 Hz, which a ProMotion display then
 * keeps to as well; the game frame follows the refresh.
 */

#import "displaysync.h"
#import "displaysynccore.h"

#import <QuartzCore/QuartzCore.h>
#include <mach/mach_time.h>

static mach_timebase_info_data_t sTimebase;

// Runs the display link's run loop.
@interface DisplaySyncThread : NSThread
@property(nonatomic, strong) CADisplayLink* link;
@end

@implementation DisplaySyncThread

- (void)main {
    @autoreleasepool {
        [self.link addToRunLoop:NSRunLoop.currentRunLoop forMode:NSRunLoopCommonModes];
        while (!self.cancelled) {
            @autoreleasepool {
                [NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.25]];
            }
        }
        [self.link invalidate];
    }
}

@end

// The link's target, so the link (which retains it) does not keep the DisplaySync alive.
@interface DisplaySyncTicker : NSObject
@end

@implementation DisplaySyncTicker

- (void)tick:(CADisplayLink*)link {
    // targetTimestamp is when the frame drawn now will be shown, as CVDisplayLink's output time.
    DisplaySyncTick((uint64_t)(link.targetTimestamp * 1e9 * sTimebase.denom / sTimebase.numer));
}

@end

@implementation DisplaySync {
    DisplaySyncThread* _thread;
    BOOL _enabled;
}

- (instancetype)init {
    if ((self = [super init]) != nil) {
        DisplaySyncCoreInit();
        mach_timebase_info(&sTimebase);
    }
    return self;
}

- (void)dealloc {
    [_thread cancel];
}

- (void)setUsableChanged:(void (^)(void))usableChanged {
    _usableChanged = [usableChanged copy];
    DisplaySyncSetUsableChanged(_usableChanged);
}

// CADisplayLink follows the screen the app is on by itself.
- (void)attachToWindow:(UIWindow*)window {
}

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
        CADisplayLink* link = [CADisplayLink displayLinkWithTarget:[DisplaySyncTicker new] selector:@selector(tick:)];
        link.preferredFrameRateRange = CAFrameRateRangeMake(60, 60, 60);
        _thread = [DisplaySyncThread new];
        _thread.name = @"display sync";
        _thread.qualityOfService = NSQualityOfServiceUserInteractive;
        _thread.link = link;
        [self forgetPeriod];
        [_thread start];
    } else {
        [_thread cancel];
        _thread = nil;
        DisplaySyncStopped();
        if (_usableChanged != nil) {
            _usableChanged();
        }
    }
}

- (NSString*)statusText {
    return DisplaySyncStatus(_enabled, _thread != nil);
}

@end
