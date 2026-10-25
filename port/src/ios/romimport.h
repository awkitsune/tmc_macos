#import <UIKit/UIKit.h>

#include "port/romdata.h"

NS_ASSUME_NONNULL_BEGIN

/*
 * The game data from the player's ROM (port/romdata.h) on iOS, for builds
 * without the data built in. At launch the extracted data is loaded from
 * ~/Library/Application Support/tmc_macos in the app's container; when there
 * is none (or it is from another build), the ROM is chosen in a document picker
 * (or the one chosen before is used again), checked and extracted exactly as
 * the macOS app does, with progress on the screen.
 *
 * TMC_ROM=path uses that ROM instead of asking (and does not remember it).
 * Under TMC_SELFTEST a failure is printed and the app exits with status 1.
 */
@interface RomImport : NSObject

// YES when the data has to come from a ROM.
@property(class, readonly) BOOL needsRom;

// Load the extracted data if it is there. YES: the game can start.
+ (BOOL)loadCache;

// What the launch found: the data was built in, loaded from the cache, or
// extracted from a ROM (self-test).
@property(class, readonly) NSString* launchSummary;

// The cache file (gamedata.bin), or nil if its path is too long.
@property(class, readonly, nullable) NSString* cachePath;

// Check rom and extract the data into cache; with load, also fill the game's
// data from it. Any thread. message gets a sentence for the player.
+ (RomDataResult)extractFrom:(NSURL*)rom
                       cache:(NSString*)cache
                        load:(BOOL)load
                    progress:(nullable void (^)(RomDataStage stage, double fraction))progress
                     message:(NSString* _Nullable* _Nonnull)message;

// A heading for a failed result ("This isn't a ROM of The Minish Cap").
+ (NSString*)titleForResult:(RomDataResult)result;

@end

/*
 * The first-launch screen: asks for the ROM, extracts the data with a
 * progress bar and calls ready on the main thread once the game can start.
 */
@interface RomImportViewController : UIViewController
- (instancetype)initWithReady:(void (^)(void))ready;
@end

NS_ASSUME_NONNULL_END
