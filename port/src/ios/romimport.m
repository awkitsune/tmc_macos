/*
 * First-run ROM import and data extraction on iOS (see romimport.h).
 */

#import "romimport.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

static NSString* const kRomBookmarkDefault = @"ROMBookmark";
static const NSTimeInterval kMinimumProgressTime = 0.8; // so the progress does not just flash

static NSString* sLaunchSummary = @"not prepared";

static BOOL Automated(void) {
    return NSProcessInfo.processInfo.environment[@"TMC_SELFTEST"].length != 0;
}

static NSURL* EnvironmentRom(void) {
    NSString* path = NSProcessInfo.processInfo.environment[@"TMC_ROM"];
    return path.length != 0 ? [NSURL fileURLWithPath:path.stringByExpandingTildeInPath] : nil;
}

// The ROM chosen last time (a security-scoped bookmark: it may be outside the app's container).
static NSURL* RememberedRom(void) {
    NSData* bookmark = [NSUserDefaults.standardUserDefaults dataForKey:kRomBookmarkDefault];
    if (bookmark == nil) {
        return nil;
    }
    BOOL stale = NO;
    return [NSURL URLByResolvingBookmarkData:bookmark options:0 relativeToURL:nil bookmarkDataIsStale:&stale error:nil];
}

static void RememberRom(NSURL* url) {
    if (EnvironmentRom() != nil) {
        return; // a test run: leave the player's choice alone
    }
    BOOL accessing = [url startAccessingSecurityScopedResource];
    NSData* bookmark = [url bookmarkDataWithOptions:0 includingResourceValuesForKeys:nil relativeToURL:nil error:nil];
    if (accessing) {
        [url stopAccessingSecurityScopedResource];
    }
    if (bookmark != nil) {
        [NSUserDefaults.standardUserDefaults setObject:bookmark forKey:kRomBookmarkDefault];
    }
}

typedef struct {
    void (^block)(RomDataStage, double);
    RomDataStage lastStage;
    double lastFraction;
} ProgressContext;

static void OnProgress(void* context, RomDataStage stage, double fraction) {
    ProgressContext* progress = context;
    if (stage == progress->lastStage && fraction - progress->lastFraction < 0.01) {
        return;
    }
    progress->lastStage = stage;
    progress->lastFraction = fraction;
    progress->block(stage, fraction);
}

@implementation RomImport

+ (BOOL)needsRom {
    return !RomDataIsBaked();
}

+ (NSString*)cachePath {
    char path[1024];
    return RomDataCachePath(path, sizeof(path)) ? @(path) : nil;
}

+ (NSString*)launchSummary {
    return sLaunchSummary;
}

+ (BOOL)loadCache {
    char message[1024];

    if (!RomImport.needsRom) {
        sLaunchSummary = @"built in";
        return YES;
    }
    NSString* cache = RomImport.cachePath;
    RomDataResult result = cache != nil ? RomDataLoad(cache.fileSystemRepresentation, message, sizeof(message)) : ROMDATA_BAD_CACHE;
    NSLog(@"game data: %s", cache == nil ? "path too long" : message);
    if (result == ROMDATA_OK) {
        sLaunchSummary = [NSString stringWithFormat:@"loaded from the cache (%s)", message];
        return YES;
    }
    return NO;
}

+ (RomDataResult)extractFrom:(NSURL*)rom
                       cache:(NSString*)cache
                        load:(BOOL)load
                    progress:(void (^)(RomDataStage, double))progress
                     message:(NSString**)message {
    char text[1024] = "";
    ProgressContext context = { progress, ROMDATA_STAGE_VERIFY, -1 };
    // A file from the document picker (outside the container) is readable only while accessed.
    BOOL accessing = [rom startAccessingSecurityScopedResource];
    RomDataResult result = RomDataExtract(rom.fileSystemRepresentation, cache.fileSystemRepresentation,
                                          progress != nil ? OnProgress : NULL, &context, text, sizeof(text));
    if (accessing) {
        [rom stopAccessingSecurityScopedResource];
    }
    if (result == ROMDATA_OK && load) {
        result = RomDataLoad(cache.fileSystemRepresentation, text, sizeof(text));
    }
    *message = @(text);
    return result;
}

+ (NSString*)titleForResult:(RomDataResult)result {
    switch (result) {
        case ROMDATA_UNREADABLE:
            return @"The file can't be read";
        case ROMDATA_NOT_A_ROM:
            return @"This isn't a ROM of The Minish Cap";
        case ROMDATA_WRONG_VERSION:
            return @"This is a different version of the game";
        case ROMDATA_HEADERED:
            return @"This ROM has a copier header";
        case ROMDATA_MODIFIED:
            return @"This ROM has been modified";
        case ROMDATA_WRITE_FAILED:
            return @"The game data couldn't be saved";
        default:
            return @"The game data couldn't be prepared";
    }
}

@end

@interface RomImportViewController () <UIDocumentPickerDelegate>
@end

@implementation RomImportViewController {
    void (^_ready)(void);
    UILabel* _heading;
    UILabel* _body;
    UIButton* _choose;
    UIProgressView* _bar;
    UILabel* _status;
    BOOL _busy;
    BOOL _started;
}

- (instancetype)initWithReady:(void (^)(void))ready {
    if ((self = [super initWithNibName:nil bundle:nil]) != nil) {
        _ready = [ready copy];
    }
    return self;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.systemBackgroundColor;

    UIImageView* icon = [[UIImageView alloc] initWithImage:[UIImage imageNamed:@"AppIcon"]];
    icon.layer.magnificationFilter = kCAFilterNearest;
    icon.layer.cornerRadius = 18;
    icon.layer.cornerCurve = kCACornerCurveContinuous;
    icon.clipsToBounds = YES;
    [icon.widthAnchor constraintEqualToConstant:80].active = YES;
    [icon.heightAnchor constraintEqualToConstant:80].active = YES;
    icon.hidden = icon.image == nil;

    _heading = [UILabel new];
    _heading.font = [UIFont preferredFontForTextStyle:UIFontTextStyleTitle2];
    _heading.adjustsFontForContentSizeCategory = YES;
    _heading.textAlignment = NSTextAlignmentCenter;
    _heading.numberOfLines = 0;
    _heading.text = @"The Minish Cap needs your ROM";

    _body = [UILabel new];
    _body.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    _body.adjustsFontForContentSizeCategory = YES;
    _body.textColor = UIColor.secondaryLabelColor;
    _body.textAlignment = NSTextAlignmentCenter;
    _body.numberOfLines = 0;
    _body.text = @"This app contains no game data. Its graphics, maps and sound are extracted once from your own copy "
                 @"of The Legend of Zelda: The Minish Cap (USA). Put the .gba file in the Files app (iCloud Drive or "
                 @"On My iPad/iPhone), then choose it.";

    UIButtonConfiguration* config = [UIButtonConfiguration filledButtonConfiguration];
    config.title = @"Choose ROM…";
    config.image = [UIImage systemImageNamed:@"folder"];
    config.imagePadding = 8;
    config.buttonSize = UIButtonConfigurationSizeLarge;
    _choose = [UIButton buttonWithConfiguration:config primaryAction:[UIAction actionWithHandler:^(UIAction* action) {
                                                     [self askForRom];
                                                 }]];

    _bar = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault];
    _bar.hidden = YES;
    _status = [UILabel new];
    _status.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
    _status.adjustsFontForContentSizeCategory = YES;
    _status.textColor = UIColor.secondaryLabelColor;
    _status.textAlignment = NSTextAlignmentCenter;
    _status.numberOfLines = 0;

    UIStackView* stack = [[UIStackView alloc] initWithArrangedSubviews:@[ icon, _heading, _body, _choose, _bar, _status ]];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.alignment = UIStackViewAlignmentCenter;
    stack.spacing = 16;
    [stack setCustomSpacing:28 afterView:_body];
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:stack];
    UILayoutGuide* safe = self.view.safeAreaLayoutGuide;
    NSLayoutConstraint* width = [stack.widthAnchor constraintEqualToConstant:440];
    width.priority = UILayoutPriorityDefaultHigh;
    [NSLayoutConstraint activateConstraints:@[
        width,
        [stack.centerXAnchor constraintEqualToAnchor:safe.centerXAnchor],
        [stack.centerYAnchor constraintEqualToAnchor:safe.centerYAnchor],
        [stack.leadingAnchor constraintGreaterThanOrEqualToAnchor:safe.leadingAnchor constant:24],
        [stack.trailingAnchor constraintLessThanOrEqualToAnchor:safe.trailingAnchor constant:-24],
        [_heading.widthAnchor constraintEqualToAnchor:stack.widthAnchor],
        [_body.widthAnchor constraintEqualToAnchor:stack.widthAnchor],
        [_bar.widthAnchor constraintEqualToAnchor:stack.widthAnchor],
        [_status.widthAnchor constraintEqualToAnchor:stack.widthAnchor],
    ]];
}

// A ROM to use without asking: TMC_ROM, or the one chosen before (the cache
// is from another build of the app).
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    if (_started) {
        return;
    }
    _started = YES;
    NSURL* rom = EnvironmentRom() ?: RememberedRom();
    if (rom != nil) {
        [self extractFrom:rom];
    } else if (Automated()) {
        fprintf(stderr, "TMC: no game data and no ROM; set TMC_ROM=path/to/rom.gba\n");
        exit(1);
    }
}

- (void)askForRom {
    if (_busy) {
        return;
    }
    UIDocumentPickerViewController* picker =
        [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[ UTTypeData ] asCopy:NO];
    picker.delegate = self;
    picker.allowsMultipleSelection = NO;
    [self presentViewController:picker animated:YES completion:nil];
}

- (void)documentPicker:(UIDocumentPickerViewController*)controller didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
    if (urls.firstObject != nil) {
        [self extractFrom:urls.firstObject];
    }
}

- (void)showStage:(RomDataStage)stage fraction:(double)fraction rom:(NSURL*)rom {
    NSString* name = @(RomDataStageName(stage));
    if (stage == ROMDATA_STAGE_VERIFY) {
        name = [NSString stringWithFormat:@"Verifying “%@”", rom.lastPathComponent];
    }
    _status.text = stage == ROMDATA_STAGE_DONE ? name : [name stringByAppendingString:@"…"];
    [_bar setProgress:(float)fraction animated:NO];
}

- (void)extractFrom:(NSURL*)rom {
    NSString* cache = RomImport.cachePath;
    __weak RomImportViewController* weakSelf = self;

    _busy = YES;
    _choose.enabled = NO;
    _bar.hidden = NO;
    _heading.text = @"Preparing the game data";
    _heading.textColor = UIColor.labelColor;
    [self showStage:ROMDATA_STAGE_VERIFY fraction:0 rom:rom];
    NSDate* shown = [NSDate date];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSString* message = @"The game data folder's path is too long.";
        RomDataResult result = ROMDATA_WRITE_FAILED;
        if (cache != nil) {
            result = [RomImport extractFrom:rom
                                      cache:cache
                                       load:YES
                                   progress:^(RomDataStage stage, double fraction) {
                                       dispatch_async(dispatch_get_main_queue(), ^{
                                           [weakSelf showStage:stage fraction:fraction rom:rom];
                                       });
                                   }
                                    message:&message];
        }
        NSLog(@"game data from %@: %@", rom.path, message);
        NSTimeInterval wait = result == ROMDATA_OK ? MAX(0.3, kMinimumProgressTime + shown.timeIntervalSinceNow) : 0;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(wait * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            [weakSelf extractionFinished:result message:message rom:rom];
        });
    });
}

- (void)extractionFinished:(RomDataResult)result message:(NSString*)message rom:(NSURL*)rom {
    _busy = NO;
    _choose.enabled = YES;
    _bar.hidden = YES;
    if (result == ROMDATA_OK) {
        RememberRom(rom);
        sLaunchSummary = [NSString stringWithFormat:@"extracted from %@", rom.lastPathComponent];
        _ready();
        return;
    }
    NSString* title = [RomImport titleForResult:result];
    if (Automated()) {
        fprintf(stderr, "TMC: %s %s\n", title.UTF8String, message.UTF8String);
        exit(1);
    }
    _heading.text = title;
    _heading.textColor = UIColor.systemRedColor;
    _status.text = message;
    UIButtonConfiguration* config = _choose.configuration;
    config.title = result == ROMDATA_WRITE_FAILED || result == ROMDATA_BAD_CACHE ? @"Try Again…" : @"Choose Another File…";
    _choose.configuration = config;
}

@end
