#import <AppKit/AppKit.h>

/*
 * The game data from the player's ROM (port/romdata.h), for builds without the
 * data built in. On launch: load the extracted data, or else ask for the ROM,
 * check it and extract the data with a progress window, then start the game.
 * Later, from the app menu: point at the ROM again or re-extract.
 *
 * TMC_ROM=path uses that ROM instead of asking (and does not remember it);
 * TMC_CACHE_DIR=dir keeps the extracted data there. Under TMC_SELFTEST a failure
 * is printed and the app exits with status 1 instead of showing an alert, and
 * the progress window is saved as romsetup_progress.png.
 */
@interface RomSetup : NSObject

// YES when the data has to come from a ROM (the menu items apply).
@property(class, readonly) BOOL needsRom;

// Make the data ready, then call ready on the main thread. Quits if the player
// gives up.
- (void)prepareThen:(void (^)(void))ready;

// NO while an extraction is running.
- (BOOL)validateMenuItem:(NSMenuItem*)item;

// Menu actions while the game runs: extract again from a newly chosen ROM, or
// from the remembered one.
- (void)changeRomForWindow:(NSWindow*)window;
- (void)reextractForWindow:(NSWindow*)window;

@end
