#import <UIKit/UIKit.h>

#import "selftestlog.h"

@class GameViewController;

/*
 * TMC_SELFTEST=1: check the app on the device without anyone at it, then
 * quit with "N failed checks" (status 1 if any): the renderer and its LCD
 * response kernel, the audio session, the ROM import path and where the files
 * go, the keyboard bindings, the gamepad mapping, controller attach and detach
 * (with a virtual controller) and the pause it drives, the frame rate, a save
 * state round trip, the settings sheet driven by controller keys, and pausing
 * when the app resigns active. Results go to standard output (devicectl --console).
 */
void SelfTestStart(GameViewController* game);
