#import <AppKit/AppKit.h>

#import "debugwindows.h"
#import "input.h"
#import "renderer.h"

/*
 * TMC_SELFTEST=dir: drive the app through the title screen with synthesized key
 * events (the real keyboard path), switch filters and full screen, save what was
 * presented as PNGs in dir, then quit. For checking the front end without a
 * person at the keyboard or screen-recording permission.
 */
void SelfTestStart(NSWindow* window, MTKView* view, Renderer* renderer, Input* input, DebugWindows* debug,
                   NSString* dir);

// With TMC_SELFTEST set, draw a window's content offscreen to dir/name.png.
void SelfTestSnapshotWindow(NSWindow* window, NSString* name);

// NSLog for self-test results: a line that reports a failure ("FAIL", "NOT",
// "TAKES", "NO LABEL") is counted, and the self-test exits with status 1 if any was.
void SelfTestLog(NSString* format, ...) NS_FORMAT_FUNCTION(1, 2);
