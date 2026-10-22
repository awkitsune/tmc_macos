/*
 * First-run ROM selection and data extraction (see romsetup.h).
 */

#import "romsetup.h"

#import "selftest.h"

#include "port/romdata.h"

static NSString* const kRomBookmarkDefault = @"ROMBookmark";
static const NSTimeInterval kMinimumProgressTime = 0.8; // so the window does not just flash

// The progress window: the app icon, a title, the current step and a bar.
@interface ExtractionWindow : NSWindow
@property(nonatomic, readonly) NSTextField* status;
@property(nonatomic, readonly) NSProgressIndicator* bar;
@end

@implementation ExtractionWindow

- (instancetype)initWithTitle:(NSString*)title {
    NSRect frame = NSMakeRect(0, 0, 460, 128);
    self = [super initWithContentRect:frame styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    if (self == nil) {
        return nil;
    }
    self.title = @"The Minish Cap";
    self.releasedWhenClosed = NO;
    NSView* content = self.contentView;

    NSImageView* icon = [NSImageView imageViewWithImage:NSApp.applicationIconImage];
    icon.frame = NSMakeRect(20, 44, 64, 64);
    [content addSubview:icon];

    NSTextField* heading = [NSTextField labelWithString:title];
    heading.font = [NSFont boldSystemFontOfSize:NSFont.systemFontSize];
    heading.frame = NSMakeRect(100, 88, 340, 18);
    [content addSubview:heading];

    _status = [NSTextField labelWithString:@""];
    _status.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    _status.textColor = NSColor.secondaryLabelColor;
    _status.frame = NSMakeRect(100, 66, 340, 16);
    [content addSubview:_status];

    _bar = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(100, 36, 340, 20)];
    _bar.style = NSProgressIndicatorStyleBar;
    _bar.indeterminate = NO;
    _bar.minValue = 0;
    _bar.maxValue = 1;
    [content addSubview:_bar];
    return self;
}

@end

// Hands progress from the extracting thread to the window, a step at a time.
@interface ExtractionProgress : NSObject {
  @public
    RomDataStage lastStage;
    double lastFraction;
}
@property(nonatomic, strong) ExtractionWindow* window;
@property(nonatomic, copy) NSString* romName;
@property(nonatomic) BOOL snapshotTaken;
@end

@implementation ExtractionProgress

- (void)showStage:(RomDataStage)stage fraction:(double)fraction {
    NSString* name = @(RomDataStageName(stage));
    if (stage == ROMDATA_STAGE_VERIFY) {
        name = [NSString stringWithFormat:@"Verifying “%@”", self.romName];
    }
    self.window.status.stringValue = stage == ROMDATA_STAGE_DONE ? name : [name stringByAppendingString:@"…"];
    self.window.bar.doubleValue = fraction;
    if (stage == ROMDATA_STAGE_MAPS && !self.snapshotTaken) {
        self.snapshotTaken = YES;
        [self.window displayIfNeeded];
        SelfTestSnapshotWindow(self.window, @"romsetup_progress");
    }
}

@end

static void OnProgress(void* context, RomDataStage stage, double fraction) {
    ExtractionProgress* progress = (__bridge ExtractionProgress*)context;

    if (stage == progress->lastStage && fraction - progress->lastFraction < 0.01) {
        return;
    }
    progress->lastStage = stage;
    progress->lastFraction = fraction;
    dispatch_async(dispatch_get_main_queue(), ^{
        [progress showStage:stage fraction:fraction];
    });
}

@implementation RomSetup {
    void (^_ready)(void);
    NSString* _cachePath;
    BOOL _busy;
}

+ (BOOL)needsRom {
    return !RomDataIsBaked();
}

- (instancetype)init {
    self = [super init];
    if (self != nil) {
        char path[1024];
        _cachePath = RomDataCachePath(path, sizeof(path)) ? @(path) : nil;
    }
    return self;
}

- (BOOL)validateMenuItem:(NSMenuItem*)item {
    return !_busy;
}

#pragma mark The ROM

- (BOOL)automated {
    return NSProcessInfo.processInfo.environment[@"TMC_SELFTEST"].length != 0;
}

- (NSURL*)environmentRom {
    NSString* path = NSProcessInfo.processInfo.environment[@"TMC_ROM"];
    return path.length != 0 ? [NSURL fileURLWithPath:path.stringByExpandingTildeInPath] : nil;
}

// The ROM chosen last time, followed if it was moved or renamed.
- (NSURL*)rememberedRom {
    NSData* bookmark = [NSUserDefaults.standardUserDefaults dataForKey:kRomBookmarkDefault];
    if (bookmark == nil) {
        return nil;
    }
    BOOL stale = NO;
    NSURL* url = [NSURL URLByResolvingBookmarkData:bookmark
                                           options:NSURLBookmarkResolutionWithoutUI | NSURLBookmarkResolutionWithoutMounting
                                     relativeToURL:nil
                               bookmarkDataIsStale:&stale
                                             error:nil];
    if (url != nil && stale) {
        [self rememberRom:url];
    }
    return [url checkResourceIsReachableAndReturnError:nil] ? url : nil;
}

- (void)rememberRom:(NSURL*)url {
    if (self.environmentRom != nil) {
        return; // a test run: leave the player's choice alone
    }
    NSData* bookmark = [url bookmarkDataWithOptions:0 includingResourceValuesForKeys:nil relativeToURL:nil error:nil];
    if (bookmark != nil) {
        [NSUserDefaults.standardUserDefaults setObject:bookmark forKey:kRomBookmarkDefault];
    }
}

- (NSOpenPanel*)openPanelWithMessage:(NSString*)message {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.title = @"Select ROM";
    panel.message = message;
    panel.prompt = @"Use ROM";
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = NO;
    panel.directoryURL = [self rememberedRom].URLByDeletingLastPathComponent;
    return panel;
}

static NSString* const kPanelMessage =
    @"Select your The Minish Cap (USA) ROM. The game's graphics, maps and sound are extracted from it once.";

#pragma mark Extraction

// Check rom and write the cache on a background thread, with a progress window
// (a sheet on parent, if given). With load, also fill the game data from it.
- (void)extractFrom:(NSURL*)rom
             parent:(NSWindow*)parent
               load:(BOOL)load
           updating:(BOOL)updating
         completion:(void (^)(RomDataResult result, NSString* message))completion {
    ExtractionProgress* progress = [ExtractionProgress new];
    progress->lastStage = ROMDATA_STAGE_VERIFY;
    progress->lastFraction = -1;
    progress.romName = rom.lastPathComponent;
    progress.window = [[ExtractionWindow alloc]
        initWithTitle:updating ? @"Updating the game data for this version" : @"Preparing the game data"];
    [progress showStage:ROMDATA_STAGE_VERIFY fraction:0];
    if (parent != nil) {
        [parent beginSheet:progress.window completionHandler:nil];
    } else {
        [progress.window center];
        [progress.window makeKeyAndOrderFront:nil];
    }
    _busy = YES;

    NSDate* shown = [NSDate date];
    NSString* romPath = rom.path;
    NSString* cachePath = _cachePath;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        char message[1024] = "";
        RomDataResult result = ROMDATA_WRITE_FAILED;
        if (cachePath == nil) {
            snprintf(message, sizeof(message), "The game data folder's path is too long.");
        } else {
            result = RomDataExtract(romPath.fileSystemRepresentation, cachePath.fileSystemRepresentation, OnProgress,
                                    (__bridge void*)progress, message, sizeof(message));
            if (result == ROMDATA_OK && load) {
                result = RomDataLoad(cachePath.fileSystemRepresentation, message, sizeof(message));
            }
        }
        NSString* text = @(message);
        NSLog(@"game data from %@: %@", romPath, text);
        NSTimeInterval wait = result == ROMDATA_OK ? MAX(0.3, kMinimumProgressTime + shown.timeIntervalSinceNow) : 0;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(wait * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            if (parent != nil) {
                [parent endSheet:progress.window];
            }
            [progress.window orderOut:nil];
            self->_busy = NO;
            completion(result, text);
        });
    });
}

- (NSAlert*)alertForResult:(RomDataResult)result message:(NSString*)message {
    NSAlert* alert = [NSAlert new];
    alert.alertStyle = NSAlertStyleCritical;
    switch (result) {
        case ROMDATA_UNREADABLE:
            alert.messageText = @"The file can't be read";
            break;
        case ROMDATA_NOT_A_ROM:
            alert.messageText = @"This isn't a ROM of The Minish Cap";
            break;
        case ROMDATA_WRONG_VERSION:
            alert.messageText = @"This is a different version of the game";
            break;
        case ROMDATA_HEADERED:
            alert.messageText = @"This ROM has a copier header";
            break;
        case ROMDATA_MODIFIED:
            alert.messageText = @"This ROM has been modified";
            break;
        case ROMDATA_WRITE_FAILED:
            alert.messageText = @"The game data couldn't be saved";
            break;
        default:
            alert.messageText = @"The game data couldn't be prepared";
            break;
    }
    alert.informativeText = message;
    return alert;
}

// Automated runs cannot answer an alert: save it, say why and stop.
- (void)failAutomated:(NSAlert*)alert {
    [alert layout];
    SelfTestSnapshotWindow(alert.window, @"romsetup_error");
    fprintf(stderr, "TMC: %s %s\n", alert.messageText.UTF8String, alert.informativeText.UTF8String);
    exit(1);
}

#pragma mark Launch

- (void)prepareThen:(void (^)(void))ready {
    char message[1024];

    _ready = [ready copy];
    if (!RomSetup.needsRom) {
        ready();
        return;
    }
    RomDataResult result =
        _cachePath != nil ? RomDataLoad(_cachePath.fileSystemRepresentation, message, sizeof(message)) : ROMDATA_BAD_CACHE;
    if (result == ROMDATA_OK) {
        NSLog(@"game data: %s", message);
        ready();
        return;
    }
    NSLog(@"game data: %s", result == ROMDATA_BAD_CACHE && _cachePath == nil ? "path too long" : message);
    [NSApp activateIgnoringOtherApps:YES];
    NSURL* rom = self.environmentRom ?: [self rememberedRom];
    if (rom != nil) {
        [self launchExtractFrom:rom updating:result == ROMDATA_STALE_CACHE];
    } else {
        [self launchAskForRom];
    }
}

- (void)launchAskForRom {
    if (self.automated) {
        fprintf(stderr, "TMC: no game data and no ROM; set TMC_ROM=path/to/rom.gba\n");
        exit(1);
    }
    NSOpenPanel* panel = [self openPanelWithMessage:kPanelMessage];
    if ([panel runModal] == NSModalResponseOK) {
        [self launchExtractFrom:panel.URL updating:NO];
        return;
    }
    NSAlert* alert = [NSAlert new];
    alert.messageText = @"The Minish Cap needs your ROM to start";
    alert.informativeText = @"This app contains no game data. Its graphics, maps and sound are extracted from your "
                            @"own copy of The Legend of Zelda: The Minish Cap (USA).";
    [alert addButtonWithTitle:@"Choose ROM…"];
    [alert addButtonWithTitle:@"Quit"];
    if ([alert runModal] == NSAlertFirstButtonReturn) {
        [self launchAskForRom];
    } else {
        [NSApp terminate:nil];
    }
}

- (void)launchExtractFrom:(NSURL*)rom updating:(BOOL)updating {
    [self extractFrom:rom
               parent:nil
                 load:YES
             updating:updating
           completion:^(RomDataResult result, NSString* message) {
               if (result == ROMDATA_OK) {
                   [self rememberRom:rom];
                   self->_ready();
                   return;
               }
               NSAlert* alert = [self alertForResult:result message:message];
               BOOL retry = result == ROMDATA_WRITE_FAILED || result == ROMDATA_BAD_CACHE;
               [alert addButtonWithTitle:retry ? @"Try Again" : @"Choose Another File…"];
               [alert addButtonWithTitle:@"Quit"];
               if (self.automated) {
                   [self failAutomated:alert];
               }
               if ([alert runModal] != NSAlertFirstButtonReturn) {
                   [NSApp terminate:nil];
               } else if (retry) {
                   [self launchExtractFrom:rom updating:updating];
               } else {
                   [self launchAskForRom];
               }
           }];
}

#pragma mark Menu

- (void)changeRomForWindow:(NSWindow*)window {
    [self changeRomForWindow:window message:kPanelMessage];
}

- (void)changeRomForWindow:(NSWindow*)window message:(NSString*)message {
    NSOpenPanel* panel = [self openPanelWithMessage:message];
    [panel beginSheetModalForWindow:window
                  completionHandler:^(NSModalResponse response) {
                      if (response != NSModalResponseOK) {
                          return;
                      }
                      NSURL* rom = panel.URL;
                      dispatch_async(dispatch_get_main_queue(), ^{ // once the panel's sheet is gone
                          [self extractFrom:rom window:window];
                      });
                  }];
}

- (void)reextractForWindow:(NSWindow*)window {
    NSURL* rom = self.environmentRom ?: [self rememberedRom];
    if (rom == nil) {
        [self changeRomForWindow:window
                         message:@"The ROM used before can't be found. " @"Select your The Minish Cap (USA) ROM to "
                                 @"extract the game data from it again."];
        return;
    }
    [self extractFrom:rom window:window];
}

// While the game runs: the data in memory is the same (there is only one valid
// ROM), so only the cache is rewritten.
- (void)extractFrom:(NSURL*)rom window:(NSWindow*)window {
    [self extractFrom:rom
               parent:window
                 load:NO
             updating:NO
           completion:^(RomDataResult result, NSString* message) {
               NSAlert* alert;
               if (result == ROMDATA_OK) {
                   [self rememberRom:rom];
                   alert = [NSAlert new];
                   alert.messageText = @"Game data extracted";
                   alert.informativeText = [NSString
                       stringWithFormat:@"The game data was extracted from “%@”.", rom.lastPathComponent];
                   [alert beginSheetModalForWindow:window completionHandler:nil];
                   return;
               }
               alert = [self alertForResult:result message:message];
               [alert addButtonWithTitle:@"Choose Another File…"];
               [alert addButtonWithTitle:@"Cancel"];
               [alert beginSheetModalForWindow:window
                             completionHandler:^(NSModalResponse response) {
                                 if (response == NSAlertFirstButtonReturn) {
                                     dispatch_async(dispatch_get_main_queue(), ^{
                                         [self changeRomForWindow:window];
                                     });
                                 }
                             }];
           }];
}

@end
