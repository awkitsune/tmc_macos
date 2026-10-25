#ifndef PORT_APPLE_DISPLAYSYNCCORE_H
#define PORT_APPLE_DISPLAYSYNCCORE_H

/*
 * The part of display sync (displaysync.h) that both apps share: the refresh
 * period measured from the display link's ticks, and the refresh grid the game
 * thread aims at. Each app's displaysync.m drives it from its own display link
 * (CVDisplayLink on macOS, CADisplayLink on iOS).
 */

#import <Foundation/Foundation.h>

#include <stdbool.h>
#include <stdint.h>

// Once, before anything else here.
void DisplaySyncCoreInit(void);
// Display link thread: the link fired for the refresh at outputTime (mach_absolute_time ticks).
void DisplaySyncTick(uint64_t outputTime);
// A different display (or rate) may be in use: unusable until it is measured again.
void DisplaySyncForget(void);
// The link stopped: unusable.
void DisplaySyncStopped(void);
// Called on the main thread when DisplaySyncUsable() changes because of a tick.
void DisplaySyncSetUsableChanged(void (^_Nullable changed)(void));
// The Settings line for the measured display.
NSString* _Nonnull DisplaySyncStatus(bool enabled, bool haveLink);

#endif // PORT_APPLE_DISPLAYSYNCCORE_H
