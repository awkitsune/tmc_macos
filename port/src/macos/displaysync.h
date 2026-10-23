#ifndef PORT_MACOS_DISPLAYSYNC_H
#define PORT_MACOS_DISPLAYSYNC_H

/*
 * "Display sync" pacing: one game frame per display refresh (or per second
 * refresh on a 120 Hz display, and so on), with the frame finished just before
 * the refresh that shows it, so the input it reads is as young as possible.
 *
 * A CVDisplayLink measures the display's refresh period and tells whether it
 * is a multiple of ~60 Hz (otherwise the display sync is unusable and the game
 * keeps to its own timer). The refresh times themselves come from the frames
 * presented (DisplaySyncNotePresented), so the game thread can ask when the
 * refresh it can still make will be (DisplaySyncPredictVsync). All times are
 * mach_absolute_time ticks.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __OBJC__
#import <AppKit/AppKit.h>
#endif

// Enabled, and the display refreshes at a multiple of ~60 Hz.
bool DisplaySyncUsable(void);
// Usable, and the display link is ticking right now (not while the window is hidden).
bool DisplaySyncActive(void);
// The first refresh at or after earliest that a game frame may be shown at.
bool DisplaySyncPredictVsync(uint64_t earliest, uint64_t* vsync);
// Ticks from one game frame to the next (the refresh period times the divisor); 0 if unusable.
uint64_t DisplaySyncFramePeriod(void);
// A frame was shown at this time (a drawable's presentedTime, seconds).
void DisplaySyncNotePresented(double seconds);

#ifdef __OBJC__
@interface DisplaySync : NSObject
@property(nonatomic) BOOL enabled;
// Called on the main thread when DisplaySyncUsable() may have changed.
@property(nonatomic, copy) void (^usableChanged)(void);
// Follow the display this window is on.
- (void)attachToWindow:(NSWindow*)window;
// One line for the Settings window: the display's rate and how the game follows it.
@property(nonatomic, readonly) NSString* statusText;
@end
#endif

#endif // PORT_MACOS_DISPLAYSYNC_H
