#import "selftest.h"

#import <Carbon/Carbon.h>

#include "port/platform.h"

static void PostKey(NSWindow* window, unsigned short keyCode, BOOL down) {
    NSEvent* event = [NSEvent keyEventWithType:down ? NSEventTypeKeyDown : NSEventTypeKeyUp
                                      location:NSZeroPoint
                                 modifierFlags:0
                                     timestamp:NSProcessInfo.processInfo.systemUptime
                                  windowNumber:window.windowNumber
                                       context:nil
                                    characters:@""
                   charactersIgnoringModifiers:@""
                                     isARepeat:NO
                                       keyCode:keyCode];
    [window sendEvent:event];
}

void SelfTestStart(NSWindow* window, MTKView* view, Renderer* renderer, NSString* dir) {
    NSURL* out = [NSURL fileURLWithPath:dir isDirectory:YES];
    [NSFileManager.defaultManager createDirectoryAtURL:out withIntermediateDirectories:YES attributes:nil error:nil];
    NSURL* (^file)(NSString*) = ^(NSString* name) {
        return [out URLByAppendingPathComponent:[name stringByAppendingString:@".png"]];
    };
    view.framebufferOnly = NO;

    NSMutableArray* steps = [NSMutableArray array];
    __block uint64_t at = 360; // title screen is up by now
    void (^add)(uint64_t, void (^)(void)) = ^(uint64_t delay, void (^action)(void)) {
        at += delay;
        [steps addObject:@[ @(at), action ]];
    };
    for (NSUInteger mode = 0; mode < FilterModeCount; mode++) {
        add(4, ^{ renderer.filter = mode; });
        add(4, ^{ [renderer captureNextFrameToURL:file([NSString stringWithFormat:@"title_%lu", (unsigned long)mode])]; });
    }
    add(4, ^{ renderer.filter = FilterModeNearest; });
    add(10, ^{ PostKey(window, kVK_Return, YES); }); // Start
    add(5, ^{ PostKey(window, kVK_Return, NO); });
    add(150, ^{ [renderer captureNextFrameToURL:file(@"after_start")]; });
    add(10, ^{ PostKey(window, kVK_ANSI_X, YES); }); // A
    add(5, ^{ PostKey(window, kVK_ANSI_X, NO); });
    add(150, ^{ [renderer captureNextFrameToURL:file(@"after_a")]; });
    add(5, ^{ [window toggleFullScreen:nil]; });
    add(120, ^{ [renderer captureNextFrameToURL:file(@"fullscreen")]; });
    add(5, ^{ [window toggleFullScreen:nil]; });
    add(90, ^{ [NSApp terminate:nil]; });

    __block NSUInteger next = 0;
    [NSTimer scheduledTimerWithTimeInterval:1.0 / 240
                                    repeats:YES
                                      block:^(NSTimer* timer) {
                                          while (next < steps.count && gPlatformFrame >= [steps[next][0] unsignedLongLongValue]) {
                                              ((void (^)(void))steps[next][1])();
                                              NSLog(@"selftest: step %lu at frame %llu", (unsigned long)next,
                                                    (unsigned long long)gPlatformFrame);
                                              next++;
                                          }
                                      }];
}
