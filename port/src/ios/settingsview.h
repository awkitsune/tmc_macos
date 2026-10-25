#import <UIKit/UIKit.h>

#import "input.h"
#import "renderer.h"

NS_ASSUME_NONNULL_BEGIN

// What the settings work on: the game screen (gameview.m).
@protocol SettingsHost <NSObject>
@property(nonatomic, readonly, nullable) Renderer* renderer;
@property(nonatomic, readonly) Input* input;
// One line on how the game follows the display (Timing).
- (NSString*)pacingStatus;
// The room the player is in, or nil (title screen, file select).
- (nullable NSString*)roomName;
// Quick save slots, 1 to 4.
@property(nonatomic) NSInteger stateSlot;
- (nullable NSDate*)dateOfSlot:(NSInteger)slot;
- (void)quickSave;
- (void)quickLoad;
- (void)resetGame;
- (void)closeSettings;
@end

/*
 * The settings: a sheet (medium and large detents on iPhone, a card on iPad)
 * with an inset-grouped list in pages as the macOS Settings tabs: Display,
 * Controls, Timing, Game and About. It works by touch and by controller: the
 * D-pad (or arrow keys) moves, left and right change a value, A selects, B goes
 * back and closes. Changes apply at once and are saved.
 */
@interface SettingsController : UINavigationController
- (instancetype)initWithHost:(id<SettingsHost>)host;
// Controller input: the HW_KEY_* bits held now (polled every display frame
// while shown; the self-test calls it directly).
- (void)handleKeys:(uint16_t)keys;
// The page shown and its focused row (self-test).
@property(nonatomic, readonly) NSString* pageTitle;
@property(nonatomic, readonly, nullable) NSString* focusedRowTitle;
// Every page's row titles, by page title (self-test).
- (NSDictionary<NSString*, NSArray<NSString*>*>*)allPageRows;
@end

NS_ASSUME_NONNULL_END
