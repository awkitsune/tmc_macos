#import <AppKit/AppKit.h>

#import "input.h"
#import "renderer.h"

/*
 * TMC_SELFTEST=dir: drive the app through the title screen with synthesized key
 * events (the real keyboard path), switch filters and full screen, save what was
 * presented as PNGs in dir, then quit. For checking the front end without a
 * person at the keyboard or screen-recording permission.
 */
void SelfTestStart(NSWindow* window, MTKView* view, Renderer* renderer, Input* input, NSString* dir);
