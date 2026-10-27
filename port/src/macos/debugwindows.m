#import "debugwindows.h"

#include <stdio.h>
#include <stdlib.h>

#include "frontend.h"
#include "port/gfxdebug.h"
#include "port/mapoverview.h"
#include "port/mapview.h"
#include "roomname.h"

#define REFRESH_HZ 12.0

static NSFont* MonoFont(void) {
    return [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
}

// A CGImage of 0xFFRRGGBB pixels (BGRA8 in memory).
static CGImageRef CreateImage(const uint32_t* pixels, size_t width, size_t height) {
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate((void*)pixels, width, height, 8, width * 4, space,
                                                 kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
    CGImageRef image = context ? CGBitmapContextCreateImage(context) : NULL;
    CGContextRelease(context);
    CGColorSpaceRelease(space);
    return image;
}

// Text written by a FILE*-based dump.
static NSString* DumpText(void (^dump)(FILE* out)) {
    char* buffer = NULL;
    size_t size = 0;
    FILE* out = open_memstream(&buffer, &size);
    dump(out);
    fclose(out);
    NSString* text = [[NSString alloc] initWithBytes:buffer length:size encoding:NSUTF8StringEncoding];
    free(buffer);
    return text ?: @"";
}

#pragma mark - Pixel view

// A picture drawn with nearest-neighbour scaling, with outlines on top and
// the pixel under the mouse reported.
@interface PixelView : NSView
@property(nonatomic) CGFloat zoom;
@property(nonatomic, readonly) NSInteger imageWidth, imageHeight;
@property(nonatomic, copy, nullable) void (^hover)(NSInteger x, NSInteger y); // -1, -1 when outside
@property(nonatomic, copy, nullable) void (^click)(NSInteger x, NSInteger y);
@property(nonatomic, copy, nullable) void (^doubleClick)(NSInteger x, NSInteger y);
// The menu for a right-click on picture pixel x, y (nil: none).
@property(nonatomic, copy, nullable) NSMenu* _Nullable (^contextMenu)(NSInteger x, NSInteger y);
@property(nonatomic, copy) NSArray<NSBezierPath*>* overlays; // in picture pixels
@property(nonatomic, strong) NSColor* overlayColor;
@property(nonatomic, copy, nullable) NSArray<NSColor*>* overlayColors; // per overlay, else overlayColor
- (void)setPixels:(const uint32_t*)pixels width:(NSInteger)width height:(NSInteger)height;
@end

@implementation PixelView {
    CGImageRef _image;
    NSTrackingArea* _tracking;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        _zoom = 2;
        _overlays = @[];
        _overlayColor = [NSColor colorWithSRGBRed:1 green:0.1 blue:0.9 alpha:1];
    }
    return self;
}

- (void)dealloc {
    CGImageRelease(_image);
}

- (BOOL)isFlipped {
    return YES;
}

- (void)setPixels:(const uint32_t*)pixels width:(NSInteger)width height:(NSInteger)height {
    CGImageRelease(_image);
    _image = width > 0 && height > 0 ? CreateImage(pixels, width, height) : NULL;
    _imageWidth = width;
    _imageHeight = height;
    [self updateSize];
    self.needsDisplay = YES;
}

- (void)setZoom:(CGFloat)zoom {
    _zoom = zoom;
    [self updateSize];
    self.needsDisplay = YES;
}

- (void)setOverlays:(NSArray<NSBezierPath*>*)overlays {
    _overlays = [overlays copy];
    self.needsDisplay = YES;
}

- (void)updateSize {
    NSSize size = NSMakeSize(ceil(_imageWidth * _zoom), ceil(_imageHeight * _zoom));
    if (!NSEqualSizes(size, self.frame.size)) {
        [self setFrameSize:size];
    }
}

- (void)drawRect:(NSRect)dirty {
    [[NSColor colorWithWhite:0.12 alpha:1] setFill];
    NSRectFill(dirty);
    if (_image == NULL) {
        return;
    }
    CGContextRef context = NSGraphicsContext.currentContext.CGContext;
    CGContextSaveGState(context);
    CGContextSetInterpolationQuality(context, kCGInterpolationNone);
    CGContextSetShouldAntialias(context, false);
    CGContextTranslateCTM(context, 0, _imageHeight * _zoom);
    CGContextScaleCTM(context, 1, -1);
    CGContextDrawImage(context, CGRectMake(0, 0, _imageWidth * _zoom, _imageHeight * _zoom), _image);
    CGContextRestoreGState(context);

    NSAffineTransform* scale = [NSAffineTransform transform];
    [scale scaleBy:_zoom];
    for (NSUInteger i = 0; i < _overlays.count; i++) {
        NSBezierPath* path = _overlays[i];
        [(i < _overlayColors.count ? _overlayColors[i] : _overlayColor) setStroke];
        NSBezierPath* scaled = [path copy];
        [scaled transformUsingAffineTransform:scale];
        scaled.lineWidth = 1.5;
        [scaled stroke];
    }
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (_tracking != nil) {
        [self removeTrackingArea:_tracking];
    }
    _tracking = [[NSTrackingArea alloc] initWithRect:NSZeroRect
                                             options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                                                     NSTrackingActiveAlways | NSTrackingInVisibleRect
                                               owner:self
                                            userInfo:nil];
    [self addTrackingArea:_tracking];
}

- (BOOL)pixelForEvent:(NSEvent*)event x:(NSInteger*)x y:(NSInteger*)y {
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    *x = (NSInteger)floor(p.x / _zoom);
    *y = (NSInteger)floor(p.y / _zoom);
    return *x >= 0 && *y >= 0 && *x < _imageWidth && *y < _imageHeight;
}

- (void)mouseMoved:(NSEvent*)event {
    NSInteger x, y;
    if (self.hover) {
        if ([self pixelForEvent:event x:&x y:&y]) {
            self.hover(x, y);
        } else {
            self.hover(-1, -1);
        }
    }
}

- (void)mouseExited:(NSEvent*)event {
    if (self.hover) {
        self.hover(-1, -1);
    }
}

- (void)mouseDown:(NSEvent*)event {
    NSInteger x, y;
    if (event.clickCount == 2 && self.doubleClick && [self pixelForEvent:event x:&x y:&y]) {
        self.doubleClick(x, y);
    } else if (self.click && [self pixelForEvent:event x:&x y:&y]) {
        self.click(x, y);
    }
}

- (NSMenu*)menuForEvent:(NSEvent*)event {
    NSInteger x, y;
    if (self.contextMenu && [self pixelForEvent:event x:&x y:&y]) {
        return self.contextMenu(x, y);
    }
    return [super menuForEvent:event];
}

@end

#pragma mark - Panel base

@interface DebugPanel : NSWindowController <NSWindowDelegate>
@property(nonatomic, readonly) GfxSnapshot* snap;
@property(nonatomic, readonly) BOOL hasSnapshot;
@property(nonatomic, readonly) BOOL usesSnapshot;
@property(nonatomic, strong) NSStackView* controls;
@property(nonatomic, strong) NSTextField* info;
@property(nonatomic, strong) NSString* name;
- (instancetype)initWithTitle:(NSString*)title name:(NSString*)name size:(NSSize)size usesSnapshot:(BOOL)usesSnapshot;
// Lay out: the controls row, then `main` (stretching), then the info line.
- (void)layoutWithMain:(NSView*)main;
- (NSPopUpButton*)popup:(NSArray<NSString*>*)items key:(NSString*)key fallback:(NSInteger)fallback;
- (NSScrollView*)scrollViewFor:(NSView*)document;
// Subclasses
- (void)refresh;        // a new snapshot arrived (or the options changed)
- (void)tick;           // every refresh period while open
- (void)panelOpened;
- (void)panelClosed;
- (NSString*)summary;
@end

@implementation DebugPanel {
    NSTimer* _timer;
    BOOL _open;
    uint64_t _sequence;
}

- (instancetype)initWithTitle:(NSString*)title name:(NSString*)name size:(NSSize)size usesSnapshot:(BOOL)usesSnapshot {
    NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, size.width, size.height)
                                                   styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                             NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                     backing:NSBackingStoreBuffered
                                                       defer:YES];
    if ((self = [super initWithWindow:window])) {
        _name = name;
        _usesSnapshot = usesSnapshot;
        _snap = calloc(1, sizeof(GfxSnapshot));
        window.title = title;
        window.releasedWhenClosed = NO;
        window.delegate = self;
        window.contentMinSize = NSMakeSize(320, 200);
        window.frameAutosaveName = [@"Debug" stringByAppendingString:name];
        if (![window setFrameUsingName:window.frameAutosaveName]) {
            [window center];
        }
        _controls = [NSStackView stackViewWithViews:@[]];
        _controls.orientation = NSUserInterfaceLayoutOrientationHorizontal;
        _controls.spacing = 8;
        _info = [NSTextField wrappingLabelWithString:@""];
        _info.font = MonoFont();
        _info.selectable = YES;
    }
    return self;
}

- (void)dealloc {
    free(_snap);
}

- (void)layoutWithMain:(NSView*)main {
    NSStackView* stack = [NSStackView stackViewWithViews:@[ _controls, main, _info ]];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    stack.spacing = 6;
    [main setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    [main setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                   forOrientation:NSLayoutConstraintOrientationVertical];
    [main.widthAnchor constraintEqualToAnchor:stack.widthAnchor constant:-16].active = YES;
    [_info.widthAnchor constraintEqualToAnchor:stack.widthAnchor constant:-16].active = YES;
    [_info setContentCompressionResistancePriority:NSLayoutPriorityRequired
                                    forOrientation:NSLayoutConstraintOrientationVertical];
    self.window.contentView = stack;
}

- (NSScrollView*)scrollViewFor:(NSView*)document {
    NSScrollView* scroll = [NSScrollView new];
    scroll.documentView = document;
    scroll.hasVerticalScroller = YES;
    scroll.hasHorizontalScroller = YES;
    scroll.autohidesScrollers = YES;
    scroll.backgroundColor = [NSColor colorWithWhite:0.12 alpha:1];
    scroll.drawsBackground = YES;
    return scroll;
}

// A pop-up whose choice is kept in the user defaults under key.
- (NSPopUpButton*)popup:(NSArray<NSString*>*)items key:(NSString*)key fallback:(NSInteger)fallback {
    NSPopUpButton* popup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [popup addItemsWithTitles:items];
    popup.identifier = [NSString stringWithFormat:@"Debug%@%@", _name, key];
    NSNumber* saved = [NSUserDefaults.standardUserDefaults objectForKey:popup.identifier];
    NSInteger index = saved != nil ? saved.integerValue : fallback;
    [popup selectItemAtIndex:index >= 0 && index < (NSInteger)items.count ? index : fallback];
    popup.target = self;
    popup.action = @selector(popupChanged:);
    popup.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    popup.controlSize = NSControlSizeSmall;
    [_controls addView:popup inGravity:NSStackViewGravityLeading];
    return popup;
}

- (void)popupChanged:(NSPopUpButton*)sender {
    [NSUserDefaults.standardUserDefaults setInteger:sender.indexOfSelectedItem forKey:sender.identifier];
    [self refresh];
}

- (void)showWindow:(id)sender {
    if (!_open) {
        _open = YES;
        if (_usesSnapshot) {
            FrontendGfxRetain();
        }
        [self panelOpened];
        _timer = [NSTimer timerWithTimeInterval:1.0 / REFRESH_HZ
                                         target:self
                                       selector:@selector(timerFired:)
                                       userInfo:nil
                                        repeats:YES];
        [NSRunLoop.mainRunLoop addTimer:_timer forMode:NSRunLoopCommonModes];
    }
    [super showWindow:sender];
}

- (void)windowWillClose:(NSNotification*)note {
    if (!_open) {
        return;
    }
    _open = NO;
    [_timer invalidate];
    _timer = nil;
    if (_usesSnapshot) {
        FrontendGfxRelease();
    }
    [self panelClosed];
}

- (void)timerFired:(NSTimer*)timer {
    [self tick];
}

- (void)tick {
    if (_usesSnapshot && FrontendCopyGfxSnapshot(_snap, &_sequence)) {
        _hasSnapshot = YES;
        [self refresh];
    }
}

- (void)refresh {
}

- (void)panelOpened {
}

- (void)panelClosed {
}

- (NSString*)summary {
    return @"";
}

- (NSString*)frameText {
    if (!_hasSnapshot) {
        return @"waiting for a frame";
    }
    return [NSString stringWithFormat:@"frame %llu%@", (unsigned long long)_snap->frame,
                                      _snap->hasLines ? @"" : @" (paused/loaded: registers as they stand)"];
}

@end

#pragma mark - Tiles

typedef struct {
    const char* name;
    uint32_t base, size;
} TileRegion;

static const TileRegion sTileRegions[] = {
    { "BG char blocks 0-3", 0x0000, 0x10000 },
    { "Char block 0", 0x0000, 0x4000 },
    { "Char block 1", 0x4000, 0x4000 },
    { "Char block 2", 0x8000, 0x4000 },
    { "Char block 3", 0xC000, 0x4000 },
    { "OBJ tiles (0x06010000)", 0x10000, 0x8000 },
    { "All VRAM", 0x0000, 0x18000 },
};
#define TILE_REGIONS (sizeof(sTileRegions) / sizeof(sTileRegions[0]))

@interface TilesPanel : DebugPanel
@end

@implementation TilesPanel {
    NSPopUpButton* _region;
    NSPopUpButton* _depth;
    NSPopUpButton* _palette;
    NSPopUpButton* _zoom;
    PixelView* _view;
    uint32_t* _pixels;
    NSString* _hoverText;
    uint64_t _imageHash;
}

static const CGFloat sZooms[] = { 1, 2, 3, 4, 6, 8 };

- (instancetype)init {
    if ((self = [super initWithTitle:@"Tiles (VRAM)" name:@"Tiles" size:NSMakeSize(560, 640) usesSnapshot:YES])) {
        NSMutableArray* regions = [NSMutableArray array];
        for (size_t i = 0; i < TILE_REGIONS; i++) {
            [regions addObject:@(sTileRegions[i].name)];
        }
        _region = [self popup:regions key:@"Region" fallback:0];
        _depth = [self popup:@[ @"4bpp", @"8bpp" ] key:@"Depth" fallback:0];
        NSMutableArray* palettes = [NSMutableArray arrayWithObject:@"Greyscale"];
        for (int i = 0; i < 32; i++) {
            [palettes addObject:[NSString stringWithFormat:@"%@ palette %d", i < 16 ? @"BG" : @"OBJ", i & 15]];
        }
        _palette = [self popup:palettes key:@"Palette" fallback:0];
        _zoom = [self popup:@[ @"1x", @"2x", @"3x", @"4x", @"6x", @"8x" ] key:@"Zoom" fallback:1];
        _pixels = calloc(256 * (HW_VRAM_SIZE / 32 / 32) * 8, sizeof(uint32_t));
        _view = [[PixelView alloc] initWithFrame:NSMakeRect(0, 0, 256, 256)];
        _view.overlayColor = [NSColor colorWithWhite:1 alpha:0.35];
        __weak TilesPanel* weakSelf = self;
        _view.hover = ^(NSInteger x, NSInteger y) {
            [weakSelf hoverAtX:x y:y];
        };
        [self layoutWithMain:[self scrollViewFor:_view]];
    }
    return self;
}

- (void)dealloc {
    free(_pixels);
}

- (BOOL)is8bpp {
    return _depth.indexOfSelectedItem == 1;
}

- (int)tileBytes {
    return self.is8bpp ? 64 : 32;
}

- (void)refresh {
    if (!self.hasSnapshot) {
        return;
    }
    const TileRegion* region = &sTileRegions[_region.indexOfSelectedItem];
    int count = region->size / self.tileBytes;
    int rows = (count + 31) / 32;
    GfxRenderTiles(self.snap, region->base, count, self.is8bpp, (int)_palette.indexOfSelectedItem - 1, _pixels);
    _imageHash = GfxHash(_pixels, 256 * rows * 8 * sizeof(uint32_t));
    [_view setPixels:_pixels width:256 height:rows * 8];
    _view.zoom = sZooms[_zoom.indexOfSelectedItem];
    // A line between char blocks.
    NSMutableArray* lines = [NSMutableArray array];
    int rowsPerBlock = 0x4000 / self.tileBytes / 32;
    for (int row = rowsPerBlock; row < rows; row += rowsPerBlock) {
        NSBezierPath* path = [NSBezierPath bezierPath];
        [path moveToPoint:NSMakePoint(0, row * 8)];
        [path lineToPoint:NSMakePoint(256, row * 8)];
        [lines addObject:path];
    }
    _view.overlays = lines;
    [self updateInfo];
}

- (void)hoverAtX:(NSInteger)x y:(NSInteger)y {
    _hoverText = nil;
    if (x >= 0 && self.hasSnapshot) {
        const TileRegion* region = &sTileRegions[_region.indexOfSelectedItem];
        int tile = (int)(y / 8) * 32 + (int)(x / 8);
        uint32_t address = region->base + tile * self.tileBytes;
        if (address < region->base + region->size) {
            int tx = x & 7, ty = y & 7, value;
            uint32_t pixelAddr = address + (self.is8bpp ? ty * 8 + tx : ty * 4 + tx / 2);
            value = self.is8bpp ? self.snap->vram[pixelAddr]
                                : (tx & 1 ? self.snap->vram[pixelAddr] >> 4 : self.snap->vram[pixelAddr] & 0xF);
            if (address >= 0x10000) {
                int number = (address - 0x10000) / 32; // OBJ tile numbers count 32-byte units
                _hoverText = [NSString stringWithFormat:@"OBJ tile %d (0x%03x) at 0x%08x (char block %d), pixel %d,%d = %d",
                                                        number, number, 0x06000000 + address, address / 0x4000, tx, ty, value];
            } else {
                int block = address / 0x4000;
                int number = (address - block * 0x4000) / self.tileBytes;
                _hoverText = [NSString stringWithFormat:@"BG tile %d (0x%03x) of char block %d at 0x%08x, pixel %d,%d = %d",
                                                        number, number, block, 0x06000000 + address, tx, ty, value];
            }
        }
    }
    [self updateInfo];
}

- (void)updateInfo {
    NSString* hint = self.is8bpp ? @"8bpp uses the whole BG or OBJ palette" : @"index 0 drawn as colour 0";
    self.info.stringValue = [NSString stringWithFormat:@"%@ — %@\n%@", self.frameText, hint,
                                                       _hoverText ?: @"hover a tile for its number and address"];
}

- (NSString*)summary {
    return [NSString stringWithFormat:@"tiles: %@, %s %@, %@, image 256x%ld hash %016llx", self.frameText,
                                      sTileRegions[_region.indexOfSelectedItem].name, _depth.titleOfSelectedItem,
                                      _palette.titleOfSelectedItem, (long)_view.imageHeight,
                                      (unsigned long long)_imageHash];
}

@end

#pragma mark - Sprites

@interface SpritesPanel : DebugPanel <NSTableViewDataSource, NSTableViewDelegate>
- (void)selectSprite:(NSInteger)index highlight:(BOOL)highlight;
@end

@implementation SpritesPanel {
    Renderer* _renderer;
    NSTableView* _table;
    PixelView* _preview;
    NSTextField* _detail;
    NSButton* _highlight;
    GfxObj _objs[128];
    uint32_t _previewPixels[64 * 64];
}

- (instancetype)initWithRenderer:(Renderer*)renderer {
    if ((self = [super initWithTitle:@"Sprites (OAM)" name:@"Sprites" size:NSMakeSize(980, 560) usesSnapshot:YES])) {
        _renderer = renderer;
        _table = [NSTableView new];
        _table.dataSource = self;
        _table.delegate = self;
        _table.rowHeight = 15;
        _table.usesAlternatingRowBackgroundColors = YES;
        _table.allowsEmptySelection = YES;
        _table.columnAutoresizingStyle = NSTableViewNoColumnAutoresizing;
        static const struct {
            const char* id;
            const char* title;
            CGFloat width;
        } columns[] = {
            { "index", "#", 30 }, { "x", "X", 38 }, { "y", "Y", 34 }, { "size", "Size", 44 },
            { "shape", "Shape", 50 }, { "tile", "Tile", 46 }, { "pal", "Pal", 30 }, { "pri", "Pri", 26 },
            { "mode", "Mode", 52 }, { "affine", "Aff", 28 }, { "double", "Dbl", 28 }, { "matrix", "Matrix", 210 },
            { "mosaic", "Mos", 30 }, { "depth", "bpp", 28 }, { "flip", "Flip", 32 }, { "hidden", "Hidden", 44 },
            { "attrs", "attr0 attr1 attr2", 120 },
        };
        for (size_t i = 0; i < sizeof(columns) / sizeof(columns[0]); i++) {
            NSTableColumn* column = [[NSTableColumn alloc] initWithIdentifier:@(columns[i].id)];
            column.title = @(columns[i].title);
            column.width = columns[i].width;
            NSTextFieldCell* cell = [NSTextFieldCell new];
            cell.font = MonoFont();
            column.dataCell = cell;
            [_table addTableColumn:column];
        }
        NSScrollView* tableScroll = [self scrollViewFor:_table];
        tableScroll.drawsBackground = NO;

        _preview = [[PixelView alloc] initWithFrame:NSMakeRect(0, 0, 64, 64)];
        _preview.zoom = 4;
        NSScrollView* previewScroll = [self scrollViewFor:_preview];
        [previewScroll.widthAnchor constraintEqualToConstant:260].active = YES;
        [previewScroll.heightAnchor constraintEqualToConstant:260].active = YES;
        _detail = [NSTextField wrappingLabelWithString:@""];
        _detail.font = MonoFont();
        _detail.selectable = YES;
        [_detail.widthAnchor constraintEqualToConstant:260].active = YES;
        _highlight = [NSButton checkboxWithTitle:@"Highlight in game window" target:self action:@selector(highlightChanged:)];
        _highlight.state = [NSUserDefaults.standardUserDefaults boolForKey:@"DebugSpritesHighlight"];
        NSStackView* side = [NSStackView stackViewWithViews:@[ previewScroll, _highlight, _detail ]];
        side.orientation = NSUserInterfaceLayoutOrientationVertical;
        side.alignment = NSLayoutAttributeLeading;
        [side setHuggingPriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];

        NSStackView* main = [NSStackView stackViewWithViews:@[ tableScroll, side ]];
        main.orientation = NSUserInterfaceLayoutOrientationHorizontal;
        main.alignment = NSLayoutAttributeTop;
        [tableScroll.heightAnchor constraintEqualToAnchor:main.heightAnchor].active = YES;
        [tableScroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
        NSTextField* help = [NSTextField labelWithString:@"Select an entry for its preview (unrotated) and details."];
        help.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
        [self.controls addView:help inGravity:NSStackViewGravityLeading];
        [self layoutWithMain:main];
    }
    return self;
}

- (NSInteger)numberOfRowsInTableView:(NSTableView*)tableView {
    return self.hasSnapshot ? 128 : 0;
}

- (id)tableView:(NSTableView*)tableView objectValueForTableColumn:(NSTableColumn*)column row:(NSInteger)row {
    static const char* const modes[4] = { "normal", "semi", "objwin", "bad" };
    static const char* const shapes[4] = { "square", "wide", "tall", "bad" };
    const GfxObj* o = &_objs[row];
    NSString* id = column.identifier;
    if ([id isEqualToString:@"index"]) {
        return @(row);
    } else if ([id isEqualToString:@"x"]) {
        return @(o->x);
    } else if ([id isEqualToString:@"y"]) {
        return @(o->y);
    } else if ([id isEqualToString:@"size"]) {
        return [NSString stringWithFormat:@"%dx%d", o->width, o->height];
    } else if ([id isEqualToString:@"shape"]) {
        return @(shapes[o->shape]);
    } else if ([id isEqualToString:@"tile"]) {
        return [NSString stringWithFormat:@"0x%03x", o->tile];
    } else if ([id isEqualToString:@"pal"]) {
        return o->is8bpp ? @"-" : @(o->palette);
    } else if ([id isEqualToString:@"pri"]) {
        return @(o->priority);
    } else if ([id isEqualToString:@"mode"]) {
        return @(modes[o->mode]);
    } else if ([id isEqualToString:@"affine"]) {
        return o->affine ? @"yes" : @"";
    } else if ([id isEqualToString:@"double"]) {
        return o->doubleSize ? @"yes" : @"";
    } else if ([id isEqualToString:@"matrix"]) {
        return o->affine ? [NSString stringWithFormat:@"%2d: %6.3f %6.3f %6.3f %6.3f", o->matrix, o->pa / 256.0,
                                                      o->pb / 256.0, o->pc / 256.0, o->pd / 256.0]
                         : @"";
    } else if ([id isEqualToString:@"mosaic"]) {
        return o->mosaic ? @"yes" : @"";
    } else if ([id isEqualToString:@"depth"]) {
        return o->is8bpp ? @"8" : @"4";
    } else if ([id isEqualToString:@"flip"]) {
        return [NSString stringWithFormat:@"%s%s", o->hflip ? "H" : "", o->vflip ? "V" : ""];
    } else if ([id isEqualToString:@"hidden"]) {
        return o->hidden ? @"hidden" : o->visible ? @"" : @"off";
    } else if ([id isEqualToString:@"attrs"]) {
        return [NSString stringWithFormat:@"%04x  %04x  %04x", o->attr0, o->attr1, o->attr2];
    }
    return @"";
}

- (void)tableView:(NSTableView*)tableView willDisplayCell:(id)cell forTableColumn:(NSTableColumn*)column row:(NSInteger)row {
    ((NSTextFieldCell*)cell).textColor = _objs[row].visible ? NSColor.labelColor : NSColor.tertiaryLabelColor;
}

- (void)tableViewSelectionDidChange:(NSNotification*)note {
    [self updateSelection];
}

- (void)highlightChanged:(NSButton*)sender {
    [NSUserDefaults.standardUserDefaults setBool:sender.state == NSControlStateValueOn forKey:@"DebugSpritesHighlight"];
    [self updateSelection];
}

- (void)selectSprite:(NSInteger)index highlight:(BOOL)highlight {
    _highlight.state = highlight ? NSControlStateValueOn : NSControlStateValueOff;
    [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:index] byExtendingSelection:NO];
    [self updateSelection];
}

- (void)refresh {
    for (int i = 0; i < 128; i++) {
        GfxDecodeObj(self.snap, i, &_objs[i]);
    }
    [_table reloadData];
    [self updateSelection];
}

- (void)updateSelection {
    NSInteger row = _table.selectedRow;
    int visible = 0;
    for (int i = 0; i < 128; i++) {
        visible += _objs[i].visible;
    }
    self.info.stringValue = [NSString stringWithFormat:@"%@, %s mapping, %d of 128 entries on screen", self.frameText,
                                                       self.hasSnapshot && (GfxReg16(self.snap, 0) & 0x40) ? "1D" : "2D",
                                                       visible];
    if (row < 0 || !self.hasSnapshot) {
        [_preview setPixels:_previewPixels width:0 height:0];
        _detail.stringValue = @"";
        _renderer.highlightRect = CGRectZero;
        return;
    }
    const GfxObj* o = &_objs[row];
    GfxRenderObj(self.snap, (int)row, _previewPixels, 0xFF303030);
    [_preview setPixels:_previewPixels width:o->width height:o->height];
    uint32_t address = 0x06010000 + (o->tile & 0x3FF) * 32;
    _detail.stringValue = [NSString
        stringWithFormat:@"OBJ %ld: %dx%d at %d,%d (box %dx%d)\ntiles from 0x%03x (0x%08x), %s\npriority %d, %s%s%s",
                         (long)row, o->width, o->height, o->x, o->y, o->boxWidth, o->boxHeight, o->tile, address,
                         o->is8bpp ? "8bpp" : [NSString stringWithFormat:@"palette %d", o->palette].UTF8String,
                         o->priority, o->affine ? [NSString stringWithFormat:@"affine matrix %d", o->matrix].UTF8String : "",
                         o->doubleSize ? ", double size" : "", o->hidden ? ", hidden" : ""];
    BOOL highlight = _highlight.state == NSControlStateValueOn && !o->hidden;
    _renderer.highlightRect = highlight ? CGRectMake(o->x, o->y, o->boxWidth, o->boxHeight) : CGRectZero;
}

- (void)panelClosed {
    _renderer.highlightRect = CGRectZero;
}

- (NSString*)summary {
    NSInteger row = _table.selectedRow;
    int visible = 0;
    for (int i = 0; i < 128; i++) {
        visible += _objs[i].visible;
    }
    CGRect r = _renderer.highlightRect;
    return [NSString stringWithFormat:@"sprites: %@, %d on screen, selected %ld, highlight %.0f,%.0f %.0fx%.0f, preview %ldx%ld",
                                      self.frameText, visible, (long)row, r.origin.x, r.origin.y, r.size.width,
                                      r.size.height, (long)_preview.imageWidth, (long)_preview.imageHeight];
}

@end

#pragma mark - Layers

@interface LayersPanel : DebugPanel
- (void)selectLayer:(NSInteger)bg;
@end

@implementation LayersPanel {
    NSSegmentedControl* _layer;
    NSPopUpButton* _zoom;
    NSButton* _shown[5];
    PixelView* _view;
    uint32_t* _pixels;
    GfxBg _bg;
    NSString* _hoverText;
    uint64_t _imageHash;
    NSString* _cameraText;
}

- (instancetype)init {
    if ((self = [super initWithTitle:@"Layers" name:@"Layers" size:NSMakeSize(640, 720) usesSnapshot:YES])) {
        _layer = [NSSegmentedControl segmentedControlWithLabels:@[ @"BG0", @"BG1", @"BG2", @"BG3" ]
                                                   trackingMode:NSSegmentSwitchTrackingSelectOne
                                                         target:self
                                                         action:@selector(layerChanged:)];
        _layer.selectedSegment = MAX(0, MIN(3, [NSUserDefaults.standardUserDefaults integerForKey:@"DebugLayersLayer"]));
        _layer.controlSize = NSControlSizeSmall;
        [self.controls addView:_layer inGravity:NSStackViewGravityLeading];
        _zoom = [self popup:@[ @"1x", @"2x", @"3x", @"4x" ] key:@"Zoom" fallback:1];
        NSTextField* label = [NSTextField labelWithString:@"Draw:"];
        label.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
        [self.controls addView:label inGravity:NSStackViewGravityLeading];
        NSArray* names = @[ @"BG0", @"BG1", @"BG2", @"BG3", @"OBJ" ];
        for (int i = 0; i < 5; i++) {
            _shown[i] = [NSButton checkboxWithTitle:names[i] target:self action:@selector(maskChanged:)];
            _shown[i].controlSize = NSControlSizeSmall;
            _shown[i].font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
            _shown[i].toolTip = @"Draw this layer in the game window (debug only; all are drawn again when this window closes)";
            [self.controls addView:_shown[i] inGravity:NSStackViewGravityLeading];
        }
        _pixels = calloc(1024 * 1024, sizeof(uint32_t));
        _view = [[PixelView alloc] initWithFrame:NSMakeRect(0, 0, 256, 256)];
        _view.overlayColor = [NSColor colorWithSRGBRed:1 green:0.85 blue:0 alpha:1];
        __weak LayersPanel* weakSelf = self;
        _view.hover = ^(NSInteger x, NSInteger y) {
            [weakSelf hoverAtX:x y:y];
        };
        [self layoutWithMain:[self scrollViewFor:_view]];
    }
    return self;
}

- (void)dealloc {
    free(_pixels);
}

- (void)panelOpened {
    unsigned mask = PpuLayerMask();
    for (int i = 0; i < 5; i++) {
        _shown[i].state = (mask >> i) & 1 ? NSControlStateValueOn : NSControlStateValueOff;
    }
}

- (void)panelClosed {
    PpuSetLayerMask(PPU_LAYERS_ALL);
}

- (void)maskChanged:(id)sender {
    unsigned mask = 0;
    for (int i = 0; i < 5; i++) {
        mask |= _shown[i].state == NSControlStateValueOn ? 1u << i : 0;
    }
    PpuSetLayerMask(mask);
}

- (void)layerChanged:(id)sender {
    [NSUserDefaults.standardUserDefaults setInteger:_layer.selectedSegment forKey:@"DebugLayersLayer"];
    [self refresh];
}

- (void)selectLayer:(NSInteger)bg {
    _layer.selectedSegment = bg;
    [self refresh];
}

// Rectangles covering [x, x + w) x [y, y + h) on a map that wraps at width x height.
static void AddWrappedRect(NSMutableArray* paths, CGFloat x, CGFloat y, CGFloat w, CGFloat h, CGFloat width, CGFloat height) {
    x = fmod(fmod(x, width) + width, width);
    y = fmod(fmod(y, height) + height, height);
    for (int dy = 0; dy < 2; dy++) {
        for (int dx = 0; dx < 2; dx++) {
            NSRect r = NSIntersectionRect(NSMakeRect(x - dx * width, y - dy * height, w, h), NSMakeRect(0, 0, width, height));
            if (!NSIsEmptyRect(r)) {
                [paths addObject:[NSBezierPath bezierPathWithRect:NSInsetRect(r, 0.5, 0.5)]];
            }
        }
    }
}

- (void)refresh {
    if (!self.hasSnapshot) {
        return;
    }
    int bg = (int)_layer.selectedSegment;
    GfxDecodeBg(self.snap, bg, &_bg);
    NSMutableArray* paths = [NSMutableArray array];
    if (GfxRenderBg(self.snap, bg, _pixels, 0xFF303030)) {
        _imageHash = GfxHash(_pixels, (size_t)_bg.width * _bg.height * sizeof(uint32_t));
        [_view setPixels:_pixels width:_bg.width height:_bg.height];
    } else {
        _imageHash = 0;
        [_view setPixels:_pixels width:0 height:0];
    }
    _view.zoom = _zoom.indexOfSelectedItem + 1;

    // The part of the map on screen at the current scroll (as line 0 drew it),
    // and for text layers whose scroll changes between lines, each line's span.
    if (_bg.kind == GFX_BG_TEXT) {
        BOOL perLine = NO;
        if (self.snap->hasLines) {
            for (int line = 1; line < PPU_HEIGHT && !perLine; line++) {
                perLine = memcmp(&self.snap->lines[line][0x10 + bg * 4], &self.snap->lines[0][0x10 + bg * 4], 4) != 0;
            }
        }
        if (perLine) {
            for (int line = 0; line < PPU_HEIGHT; line++) {
                const uint8_t* r = &self.snap->lines[line][0x10 + bg * 4];
                int hofs = (r[0] | (r[1] << 8)) & 0x1FF, vofs = (r[2] | (r[3] << 8)) & 0x1FF;
                AddWrappedRect(paths, hofs, vofs + line, PPU_WIDTH, 1, _bg.width, _bg.height);
            }
            _cameraText = [NSString stringWithFormat:@"scroll %d,%d at line 0, changes between lines (one outline per line)",
                                                     _bg.hofs, _bg.vofs];
        } else {
            AddWrappedRect(paths, _bg.hofs, _bg.vofs, PPU_WIDTH, PPU_HEIGHT, _bg.width, _bg.height);
            _cameraText = [NSString stringWithFormat:@"scroll %d,%d", _bg.hofs, _bg.vofs];
        }
    } else if (_bg.kind == GFX_BG_AFFINE) {
        // Screen corners mapped into the map: (refX, refY) + (pa, pc) * x + (pb, pd) * y.
        NSBezierPath* path = [NSBezierPath bezierPath];
        int corners[4][2] = { { 0, 0 }, { PPU_WIDTH, 0 }, { PPU_WIDTH, PPU_HEIGHT }, { 0, PPU_HEIGHT } };
        for (int i = 0; i < 4; i++) {
            double x = (_bg.refX + _bg.pa * corners[i][0] + _bg.pb * corners[i][1]) / 256.0;
            double y = (_bg.refY + _bg.pc * corners[i][0] + _bg.pd * corners[i][1]) / 256.0;
            i == 0 ? [path moveToPoint:NSMakePoint(x, y)] : [path lineToPoint:NSMakePoint(x, y)];
        }
        [path closePath];
        [paths addObject:path];
        _cameraText = [NSString stringWithFormat:@"ref %.2f,%.2f, pa %.3f pb %.3f pc %.3f pd %.3f", _bg.refX / 256.0,
                                                 _bg.refY / 256.0, _bg.pa / 256.0, _bg.pb / 256.0, _bg.pc / 256.0,
                                                 _bg.pd / 256.0];
    } else {
        _cameraText = @"not a layer in this display mode";
    }
    _view.overlays = paths;
    [self updateInfo];
}

- (void)hoverAtX:(NSInteger)x y:(NSInteger)y {
    _hoverText = nil;
    if (x >= 0 && self.hasSnapshot && _bg.kind == GFX_BG_TEXT) {
        uint32_t block = (x >= 256 ? 1 : 0) + (y >= 256 ? (_bg.width == 512 ? 2 : 1) : 0);
        uint32_t addr = (_bg.screenBase + block * 0x800 + (((y & 255) >> 3) * 32 + ((x & 255) >> 3)) * 2) % HW_VRAM_SIZE;
        uint16_t entry = self.snap->vram[addr] | (self.snap->vram[addr + 1] << 8);
        _hoverText = [NSString stringWithFormat:@"map %ld,%ld (cell %ld,%ld): entry %04x at 0x%08x = tile 0x%03x palette %d%s%s",
                                                (long)x, (long)y, (long)x / 8, (long)y / 8, entry, 0x06000000 + addr,
                                                entry & 0x3FF, entry >> 12, entry & 0x400 ? " H" : "",
                                                entry & 0x800 ? " V" : ""];
    } else if (x >= 0 && self.hasSnapshot && _bg.kind == GFX_BG_AFFINE) {
        uint32_t addr = (_bg.screenBase + (y >> 3) * (_bg.width >> 3) + (x >> 3)) % HW_VRAM_SIZE;
        _hoverText = [NSString stringWithFormat:@"map %ld,%ld (cell %ld,%ld): tile 0x%02x at 0x%08x", (long)x, (long)y,
                                                (long)x / 8, (long)y / 8, self.snap->vram[addr], 0x06000000 + addr];
    }
    [self updateInfo];
}

- (void)updateInfo {
    static const char* const kinds[3] = { "not shown in this mode", "text", "affine" };
    NSString* settings = self.hasSnapshot
                             ? [NSString stringWithFormat:@"BG%d %@, %s, %dx%d, priority %d, char base 0x%05x (block %d), "
                                                          @"screen base 0x%05x (block %d), %s%s%s, cnt %04x\n%@",
                                                          _bg.bg, _bg.enabled ? @"on" : @"off", kinds[_bg.kind], _bg.width,
                                                          _bg.height, _bg.priority, _bg.charBase, _bg.charBase / 0x4000,
                                                          _bg.screenBase, _bg.screenBase / 0x800,
                                                          _bg.is8bpp ? "8bpp (256 colours)" : "4bpp (16 colours)",
                                                          _bg.mosaic ? ", mosaic" : "",
                                                          _bg.kind == GFX_BG_AFFINE ? (_bg.wrap ? ", wraps" : ", no wrap") : "",
                                                          _bg.cnt, _cameraText ?: @""]
                             : @"";
    self.info.stringValue = [NSString stringWithFormat:@"%@, mode %d (transparent pixels dark grey)\n%@\n%@", self.frameText,
                                                       self.hasSnapshot ? GfxReg16(self.snap, 0) & 7 : 0, settings,
                                                       _hoverText ?: @"hover the map for its entries"];
}

- (NSString*)summary {
    return [NSString stringWithFormat:@"layers: %@, BG%ld %dx%d kind %d prio %d, %@, image hash %016llx, mask %02x",
                                      self.frameText, (long)_layer.selectedSegment, _bg.width, _bg.height, _bg.kind,
                                      _bg.priority, _cameraText, (unsigned long long)_imageHash, PpuLayerMask()];
}

@end

#pragma mark - Palettes

@interface PalettesPanel : DebugPanel
@end

@implementation PalettesPanel {
    NSPopUpButton* _zoom;
    PixelView* _view;
    uint32_t _pixels[33 * 16];
    NSString* _hoverText;
}

- (instancetype)init {
    if ((self = [super initWithTitle:@"Palettes" name:@"Palettes" size:NSMakeSize(560, 380) usesSnapshot:YES])) {
        _zoom = [self popup:@[ @"8x", @"12x", @"16x", @"24x" ] key:@"Zoom" fallback:2];
        NSTextField* label = [NSTextField labelWithString:@"BG palettes left, OBJ right; one row per palette"];
        label.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
        [self.controls addView:label inGravity:NSStackViewGravityLeading];
        _view = [[PixelView alloc] initWithFrame:NSMakeRect(0, 0, 264, 128)];
        __weak PalettesPanel* weakSelf = self;
        _view.hover = ^(NSInteger x, NSInteger y) {
            [weakSelf hoverAtX:x y:y];
        };
        [self layoutWithMain:[self scrollViewFor:_view]];
    }
    return self;
}

- (void)refresh {
    if (!self.hasSnapshot) {
        return;
    }
    static const CGFloat zooms[] = { 8, 12, 16, 24 };
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 33; x++) {
            _pixels[y * 33 + x] = x == 16 ? 0xFF1E1E1E : GfxRgb(GfxColor555(self.snap, (x > 16 ? 256 : 0) + y * 16 + (x % 17)));
        }
    }
    [_view setPixels:_pixels width:33 height:16];
    _view.zoom = zooms[_zoom.indexOfSelectedItem];
    NSMutableArray* grid = [NSMutableArray array];
    for (int side = 0; side < 2; side++) {
        NSBezierPath* path = [NSBezierPath bezierPathWithRect:NSMakeRect(side * 17, 0, 16, 16)];
        [grid addObject:path];
    }
    _view.overlayColor = [NSColor colorWithWhite:0.5 alpha:0.6];
    _view.overlays = grid;
    [self updateInfo];
}

- (void)hoverAtX:(NSInteger)x y:(NSInteger)y {
    _hoverText = nil;
    if (x >= 0 && x != 16 && self.hasSnapshot) {
        BOOL obj = x > 16;
        int index = (obj ? 256 : 0) + (int)y * 16 + (int)(x % 17);
        uint16_t c = GfxColor555(self.snap, index);
        uint32_t rgb = GfxRgb(c);
        _hoverText = [NSString stringWithFormat:@"%@ colour %d (0x%03x): palette %ld index %ld, at 0x%08x\nBGR555 0x%04x = R %d G %d "
                                                @"B %d of 31 -> #%06X (%d, %d, %d)",
                                                obj ? @"OBJ" : @"BG", index & 0xFF, index, (long)y, (long)(x % 17),
                                                0x05000000 + index * 2, c, c & 0x1F, (c >> 5) & 0x1F, (c >> 10) & 0x1F,
                                                rgb & 0xFFFFFF, (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF];
    }
    [self updateInfo];
}

- (void)updateInfo {
    self.info.stringValue = [NSString stringWithFormat:@"%@ — index 0 is transparent (BG colour 0 is the backdrop)\n%@",
                                                       self.frameText, _hoverText ?: @"hover a colour\n"];
}

- (NSString*)summary {
    return [NSString stringWithFormat:@"palettes: %@, BG hash %016llx OBJ hash %016llx, backdrop %04x", self.frameText,
                                      (unsigned long long)GfxHash(self.snap->pltt, 0x200),
                                      (unsigned long long)GfxHash(self.snap->pltt + 0x200, 0x200), GfxColor555(self.snap, 0)];
}

@end

#pragma mark - IO registers

@interface IoPanel : DebugPanel
@end

@implementation IoPanel {
    NSTextView* _text;
    NSString* _shown;
}

- (instancetype)init {
    if ((self = [super initWithTitle:@"IO Registers" name:@"Io" size:NSMakeSize(720, 620) usesSnapshot:YES])) {
        NSScrollView* scroll = [NSTextView scrollableTextView];
        _text = scroll.documentView;
        _text.editable = NO;
        _text.font = MonoFont();
        _text.textContainerInset = NSMakeSize(4, 4);
        NSTextField* label = [NSTextField labelWithString:@"Display registers as line 0 was drawn, then the lines where they changed"];
        label.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
        [self.controls addView:label inGravity:NSStackViewGravityLeading];
        [self layoutWithMain:scroll];
    }
    return self;
}

- (void)refresh {
    GfxSnapshot* snap = self.snap;
    NSString* text = DumpText(^(FILE* out) {
        GfxDumpIo(snap, out);
        fprintf(out, "\n");
        GfxDumpLines(snap, out);
    });
    // Only replace the text when it changed, so scrolling and selecting work.
    NSString* body = [text substringFromIndex:[text rangeOfString:@"\n"].location];
    if (![body isEqualToString:_shown]) {
        _shown = body;
        NSDictionary* attributes = @{ NSFontAttributeName : MonoFont(), NSForegroundColorAttributeName : NSColor.textColor };
        [_text.textStorage setAttributedString:[[NSAttributedString alloc] initWithString:text attributes:attributes]];
    }
    self.info.stringValue = self.frameText;
}

- (NSString*)summary {
    NSString* text = _text.string;
    __block NSUInteger lines = 0;
    [text enumerateLinesUsingBlock:^(NSString* line, BOOL* stop) {
        lines++;
    }];
    NSRange dispcnt = [text rangeOfString:@"DISPCNT"];
    NSString* first = dispcnt.location != NSNotFound ? [text substringWithRange:[text lineRangeForRange:dispcnt]] : @"";
    return [NSString stringWithFormat:@"io: %@, %lu lines; %@", self.frameText, (unsigned long)lines,
                                      [first stringByTrimmingCharactersInSet:NSCharacterSet.newlineCharacterSet]];
}

@end

#pragma mark - Shader buffers

@interface ShaderPanel : DebugPanel
- (void)selectBuffer:(DebugBuffer)buffer;
@end

@implementation ShaderPanel {
    Renderer* _renderer;
    NSPopUpButton* _buffer;
    NSPopUpButton* _zoom;
    NSPopUpButton* _channel;
    NSPopUpButton* _display;
    NSSlider* _exposure;
    NSTextField* _exposureLabel;
    PixelView* _view;
    DebugReadback* _readback;
    uint64_t _serial;
    uint32_t* _pixels;
    size_t _pixelCapacity;
    NSString* _hoverText;
    NSString* _stats;
    NSInteger _hoverX, _hoverY;
}

static const CGFloat sShaderZooms[] = { 0.25, 0.5, 1, 2, 4, 8 };

- (instancetype)initWithRenderer:(Renderer*)renderer {
    if ((self = [super initWithTitle:@"Shader Buffers" name:@"Shader" size:NSMakeSize(860, 640) usesSnapshot:NO])) {
        _renderer = renderer;
        NSMutableArray* names = [NSMutableArray array];
        for (NSInteger i = 0; i < DebugBufferCount; i++) {
            [names addObject:DebugBufferName(i)];
        }
        _buffer = [self popup:names key:@"Buffer" fallback:0];
        _zoom = [self popup:@[ @"1/4x", @"1/2x", @"1x", @"2x", @"4x", @"8x" ] key:@"Zoom" fallback:3];
        _channel = [self popup:@[ @"RGB", @"R", @"G", @"B", @"A", @"Luma" ] key:@"Channel" fallback:0];
        _display = [self popup:@[ @"Clamped", @"False colour" ] key:@"Display" fallback:0];
        _exposure = [NSSlider sliderWithValue:[NSUserDefaults.standardUserDefaults doubleForKey:@"DebugShaderExposure"]
                                     minValue:-8
                                     maxValue:8
                                       target:self
                                       action:@selector(exposureChanged:)];
        _exposure.controlSize = NSControlSizeSmall;
        [_exposure.widthAnchor constraintEqualToConstant:110].active = YES;
        _exposureLabel = [NSTextField labelWithString:@""];
        _exposureLabel.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
        [self.controls addView:_exposure inGravity:NSStackViewGravityLeading];
        [self.controls addView:_exposureLabel inGravity:NSStackViewGravityLeading];
        _view = [[PixelView alloc] initWithFrame:NSMakeRect(0, 0, 240, 160)];
        _hoverX = _hoverY = -1;
        __weak ShaderPanel* weakSelf = self;
        _view.hover = ^(NSInteger x, NSInteger y) {
            [weakSelf hoverAtX:x y:y];
        };
        [self layoutWithMain:[self scrollViewFor:_view]];
        [self updateExposureLabel];
    }
    return self;
}

- (void)dealloc {
    free(_pixels);
}

- (void)panelOpened {
    _renderer.debugBuffer = _buffer.indexOfSelectedItem;
}

- (void)panelClosed {
    _renderer.debugBuffer = DebugBufferNone;
    _readback = nil;
}

// (Self-test: not remembered in the user's defaults.)
- (void)selectBuffer:(DebugBuffer)buffer {
    [_buffer selectItemAtIndex:buffer];
    _renderer.debugBuffer = buffer;
    _readback = nil;
    _serial = 0;
}

- (void)popupChanged:(NSPopUpButton*)sender {
    [super popupChanged:sender];
    if (sender == _buffer) {
        _renderer.debugBuffer = _buffer.indexOfSelectedItem;
        _readback = nil;
        _serial = 0;
    } else {
        [self rebuildImage];
    }
}

- (void)exposureChanged:(id)sender {
    [NSUserDefaults.standardUserDefaults setDouble:_exposure.doubleValue forKey:@"DebugShaderExposure"];
    [self updateExposureLabel];
    [self rebuildImage];
}

- (void)updateExposureLabel {
    _exposureLabel.stringValue = [NSString stringWithFormat:@"exposure %+.1f EV", _exposure.doubleValue];
}

- (void)tick {
    DebugReadback* latest = _renderer.latestReadback;
    if (latest != nil && latest.serial != _serial && latest.buffer == _buffer.indexOfSelectedItem) {
        _readback = latest;
        _serial = latest.serial;
        _stats = nil;
        [self rebuildImage];
        if (_hoverX >= 0) {
            [self hoverAtX:_hoverX y:_hoverY];
        }
    }
}

// Heat map for a value: black, blue, cyan, green, yellow, red, then white
// above 1; magenta below 0.
static uint32_t FalseColour(float v) {
    static const float stops[6][3] = { { 0, 0, 0 }, { 0, 0, 1 }, { 0, 1, 1 }, { 0, 1, 0 }, { 1, 1, 0 }, { 1, 0, 0 } };
    float rgb[3];
    if (v < 0) {
        return 0xFFFF00FF;
    }
    if (v > 1) {
        return 0xFFFFFFFF;
    }
    float t = v * 5;
    int i = MIN((int)t, 4);
    float f = t - i;
    for (int c = 0; c < 3; c++) {
        rgb[c] = stops[i][c] + (stops[i + 1][c] - stops[i][c]) * f;
    }
    return 0xFF000000u | ((uint32_t)(rgb[0] * 255) << 16) | ((uint32_t)(rgb[1] * 255) << 8) | (uint32_t)(rgb[2] * 255);
}

static uint32_t Unorm(float v) {
    return (uint32_t)(fminf(fmaxf(v, 0), 1) * 255 + 0.5f);
}

- (void)rebuildImage {
    DebugReadback* rb = _readback;
    if (rb == nil) {
        [self updateInfo];
        return;
    }
    size_t count = rb.width * rb.height;
    if (count > _pixelCapacity) {
        free(_pixels);
        _pixels = malloc(count * sizeof(uint32_t));
        _pixelCapacity = count;
    }
    NSInteger channel = _channel.indexOfSelectedItem;
    BOOL heat = _display.indexOfSelectedItem == 1;
    float scale = exp2f(_exposure.floatValue);
    const __fp16* halves = rb.bytes.bytes;
    const uint8_t* bytes = rb.bytes.bytes;
    // A plain loop over the bytes: drawable-sized buffers have millions of pixels.
    for (size_t i = 0; i < count; i++) {
        float r, g, b, a;
        if (rb.isFloat) {
            r = halves[i * 4], g = halves[i * 4 + 1], b = halves[i * 4 + 2], a = halves[i * 4 + 3];
        } else {
            b = bytes[i * 4] / 255.0f, g = bytes[i * 4 + 1] / 255.0f, r = bytes[i * 4 + 2] / 255.0f;
            a = bytes[i * 4 + 3] / 255.0f;
        }
        r *= scale, g *= scale, b *= scale; // alpha is not exposed
        float single;
        switch (channel) {
            case 1: single = r; break;
            case 2: single = g; break;
            case 3: single = b; break;
            case 4: single = a; break;
            case 5: single = 0.2126f * r + 0.7152f * g + 0.0722f * b; break;
            default: single = fmaxf(r, fmaxf(g, b)); break;
        }
        uint32_t out;
        if (heat) {
            out = FalseColour(single);
        } else if (channel == 0) {
            out = 0xFF000000u | (Unorm(r) << 16) | (Unorm(g) << 8) | Unorm(b);
        } else {
            uint32_t v = Unorm(single);
            out = 0xFF000000u | (v << 16) | (v << 8) | v;
        }
        _pixels[i] = out;
    }
    [_view setPixels:_pixels width:rb.width height:rb.height];
    _view.zoom = sShaderZooms[_zoom.indexOfSelectedItem];
    [self updateInfo];
}

- (void)hoverAtX:(NSInteger)x y:(NSInteger)y {
    _hoverX = x;
    _hoverY = y;
    _hoverText = nil;
    DebugReadback* rb = _readback;
    if (x >= 0 && rb != nil) {
        simd_float4 v = [rb pixelAtX:x y:y];
        if (rb.isFloat) {
            _hoverText = [NSString stringWithFormat:@"pixel %ld,%ld: R %.5f G %.5f B %.5f A %.5f (float16)", (long)x, (long)y,
                                                    v.x, v.y, v.z, v.w];
        } else {
            _hoverText = [NSString stringWithFormat:@"pixel %ld,%ld: R %u G %u B %u A %u (unorm8: %.4f %.4f %.4f %.4f)",
                                                    (long)x, (long)y, Unorm(v.x), Unorm(v.y), Unorm(v.z), Unorm(v.w), v.x,
                                                    v.y, v.z, v.w];
        }
    }
    [self updateInfo];
}

- (void)updateInfo {
    DebugReadback* rb = _readback;
    NSString* head = rb == nil ? @"waiting for the renderer"
                               : [NSString stringWithFormat:@"%lux%lu %@%@%@", (unsigned long)rb.width,
                                                            (unsigned long)rb.height, rb.format,
                                                            rb.note.length ? @" — " : @"", rb.note];
    self.info.stringValue = [NSString stringWithFormat:@"%@\n%@", head, _hoverText ?: @"hover for the stored value"];
}

- (NSString*)summary {
    DebugReadback* rb = _renderer.latestReadback;
    if (rb == nil || rb.buffer != _buffer.indexOfSelectedItem) {
        return [NSString stringWithFormat:@"shader buffer %@: no readback yet", DebugBufferName(_buffer.indexOfSelectedItem)];
    }
    return [NSString stringWithFormat:@"shader buffer %@: %@%@%@", DebugBufferName(rb.buffer), rb.statistics,
                                      rb.note.length ? @"; " : @"", rb.note];
}

@end

#pragma mark - Map

// The whole current room (port/mapview.h): its two tile layers as the game
// thread last drew them, with the camera, the entities and, on request, the
// collision and a grid; right-click moves Link, the room list warps. Or an
// overview of a whole area or dungeon floor (port/mapoverview.h), drawn from
// the game's data off the main thread: hover names a room, a double click or
// the right-click menu warps there.
@interface MapPanel : DebugPanel
@end

typedef NS_ENUM(NSInteger, MapMode) { MapModeRoom, MapModeArea, MapModeFloor };

// An overview drawn on the panel's queue, freed with the object.
@interface MapOverviewImage : NSObject {
  @public
    MapOverview overview;
}
@end

@implementation MapOverviewImage
- (void)dealloc {
    MapOverviewFree(&overview);
}
@end

// Overviews kept for the areas and floors drawn last (each at most 16 MB, MAP_OVERVIEW_MAX_PIXELS).
#define MAP_OVERVIEW_CACHE_BYTES (48 << 20)

@implementation MapPanel {
    NSSegmentedControl* _mode;
    NSPopUpButton* _picker; // Follow Link, then every area or dungeon floor
    NSButton* _warp;
    NSButton* _redraw;
    dispatch_queue_t _overviewQueue;
    NSMutableDictionary<NSString*, MapOverviewImage*>* _overviews;
    NSMutableArray<NSString*>* _overviewOrder; // least recently shown first
    NSMutableSet<NSString*>* _drawing;
    NSMutableSet<NSString*>* _undrawable;
    NSString* _wantedKey; // the overview the view should show; read by the queue under @synchronized(self)
    MapOverviewImage* _shown;
    NSString* _scrolledTo; // overview key and room the view last scrolled to Link in
    int _hoverRoom;        // index into the shown overview's rooms, or -1
    MapViewState* _state;
    MapViewImage* _image;
    uint64_t _stateSequence, _imageSequence;
    BOOL _hasState;
    NSPopUpButton* _zoom;
    NSButton* _bottom;
    NSButton* _top;
    NSButton* _collision;
    NSButton* _grid;
    NSPopUpButton* _rooms;
    int _roomsArea, _roomsRoom; // what the room list was last set for
    int _shownRoom;             // area << 8 | room the view last scrolled to
    PixelView* _view;
    uint32_t* _pixels;
    int _composedLayer; // the collision layer the picture shows (the player's)
    NSInteger _hoverX, _hoverY;
    NSString* _title;
}

static const CGFloat sMapZooms[] = { 0.5, 1, 2, 3, 4 };

static NSString* const sMapToggles[4] = { @"Bottom", @"Top", @"Collision", @"Grid" };

- (instancetype)init {
    if ((self = [super initWithTitle:@"Map" name:@"Map" size:NSMakeSize(820, 680) usesSnapshot:NO])) {
        _overviewQueue = dispatch_queue_create("tmc.map.overview", DISPATCH_QUEUE_SERIAL);
        _overviews = [NSMutableDictionary dictionary];
        _overviewOrder = [NSMutableArray array];
        _drawing = [NSMutableSet set];
        _undrawable = [NSMutableSet set];
        _hoverRoom = -1;
        _mode = [NSSegmentedControl segmentedControlWithLabels:@[ @"Room", @"Area", @"Floor" ]
                                                  trackingMode:NSSegmentSwitchTrackingSelectOne
                                                        target:self
                                                        action:@selector(modeChanged:)];
        _mode.controlSize = NSControlSizeSmall;
        _mode.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
        _mode.toolTip = @"The current room as the game drew it, or the whole area or dungeon floor from the game's data";
        NSInteger savedMode = [NSUserDefaults.standardUserDefaults integerForKey:@"DebugMapMode"];
        _mode.selectedSegment = savedMode >= MapModeRoom && savedMode <= MapModeFloor ? savedMode : MapModeRoom;
        [self.controls addView:_mode inGravity:NSStackViewGravityLeading];
        _state = calloc(1, sizeof(MapViewState));
        _image = calloc(1, sizeof(MapViewImage));
        _pixels = calloc(MAPVIEW_MAX_SIZE * MAPVIEW_MAX_SIZE, sizeof(uint32_t));
        _roomsArea = _roomsRoom = _shownRoom = -1;
        _hoverX = _hoverY = -1;
        _zoom = [self popup:@[ @"1/2x", @"1x", @"2x", @"3x", @"4x" ] key:@"Zoom" fallback:1];
        _bottom = [self toggle:0 on:YES];
        _top = [self toggle:1 on:YES];
        _collision = [self toggle:2 on:NO];
        _grid = [self toggle:3 on:NO];
        _collision.toolTip = @"Darken solid tiles, tint partly solid ones (collision of Link's layer)";
        _rooms = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
        _rooms.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
        _rooms.controlSize = NSControlSizeSmall;
        _rooms.toolTip = @"A room of the current area";
        [_rooms.widthAnchor constraintLessThanOrEqualToConstant:240].active = YES;
        [self.controls addView:_rooms inGravity:NSStackViewGravityLeading];
        _warp = [NSButton buttonWithTitle:@"Warp" target:self action:@selector(warp:)];
        _warp.controlSize = NSControlSizeSmall;
        _warp.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
        _warp.toolTip = @"Go to the chosen room through the game's own exit transition, once Link has control";
        [self.controls addView:_warp inGravity:NSStackViewGravityLeading];
        _redraw = [NSButton buttonWithTitle:@"Redraw" target:self action:@selector(redraw:)];
        _redraw.controlSize = NSControlSizeSmall;
        _redraw.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
        _redraw.toolTip = @"Draw the room again from VRAM now (otherwise only when its tiles change; "
                          @"animated tiles stay as they were)";
        [self.controls addView:_redraw inGravity:NSStackViewGravityLeading];
        _picker = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
        _picker.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
        _picker.controlSize = NSControlSizeSmall;
        _picker.target = self;
        _picker.action = @selector(pickerChanged:);
        [_picker.widthAnchor constraintLessThanOrEqualToConstant:280].active = YES;
        [self.controls addView:_picker inGravity:NSStackViewGravityLeading];

        _view = [[PixelView alloc] initWithFrame:NSMakeRect(0, 0, 256, 256)];
        _view.zoom = sMapZooms[_zoom.indexOfSelectedItem];
        __weak MapPanel* weakSelf = self;
        _view.hover = ^(NSInteger x, NSInteger y) {
            [weakSelf hoverAtX:x y:y];
        };
        _view.contextMenu = ^NSMenu*(NSInteger x, NSInteger y) {
            return [weakSelf menuAtX:x y:y];
        };
        _view.doubleClick = ^(NSInteger x, NSInteger y) {
            [weakSelf doubleClickAtX:x y:y];
        };
        [self layoutWithMain:[self scrollViewFor:_view]];
        [self applyMode];
    }
    return self;
}

- (MapMode)mode {
    return (MapMode)_mode.selectedSegment;
}

- (void)modeChanged:(id)sender {
    [NSUserDefaults.standardUserDefaults setInteger:self.mode forKey:@"DebugMapMode"];
    [self applyMode];
    [self refresh];
}

// The controls of the mode; the picker lists the mode's areas or floors.
- (void)applyMode {
    MapMode mode = self.mode;
    BOOL room = mode == MapModeRoom;
    for (NSButton* box in @[ _bottom, _top, _collision, _grid ]) {
        box.enabled = room;
    }
    _rooms.hidden = _warp.hidden = _redraw.hidden = !room;
    _picker.hidden = room;
    _shown = nil;
    _scrolledTo = nil;
    _hoverRoom = -1;
    [_view setPixels:NULL width:0 height:0]; // the room is composed again, the overview shown once it is there
    [_picker removeAllItems];
    [_picker addItemWithTitle:@"Follow Link"];
    _picker.lastItem.tag = -1;
    if (mode == MapModeArea) {
        _picker.toolTip = @"The area to show: Link's, or any area with rooms the game can load";
        [_picker.menu addItem:NSMenuItem.separatorItem];
        for (int area = 0; area < AreaMapAreaCount(); area++) {
            int rooms = 0;
            char name[96];
            for (int room = 0; room < AreaMapRoomCount(area); room++) {
                rooms += AreaMapRoomIsDrawable(area, room);
            }
            if (rooms == 0) {
                continue;
            }
            MapOverviewAreaName(area, name, sizeof(name));
            [_picker addItemWithTitle:[NSString stringWithFormat:@"%02x %s (%d room%@)", area,
                                                                 name[0] ? name : "unnamed", rooms,
                                                                 rooms == 1 ? @"" : @"s"]];
            _picker.lastItem.tag = area;
        }
    } else if (mode == MapModeFloor) {
        _picker.toolTip = @"The dungeon floor to show: Link's, or any (floor 0 is the highest)";
        [_picker.menu addItem:NSMenuItem.separatorItem];
        for (int dungeon = 1; dungeon < AreaMapDungeonCount(); dungeon++) {
            for (int floor = 0; floor < AreaMapFloorCount(dungeon); floor++) {
                [_picker addItemWithTitle:[NSString stringWithFormat:@"%s — floor %d of %d",
                                                                     MapOverviewDungeonName(dungeon), floor,
                                                                     AreaMapFloorCount(dungeon)]];
                _picker.lastItem.tag = dungeon << 8 | floor;
            }
        }
    }
    [_picker selectItemWithTag:-1];
}

- (void)pickerChanged:(id)sender {
    [self update];
}

- (void)dealloc {
    free(_state);
    free(_image);
    free(_pixels);
}

// A check box whose state is kept in the user defaults.
- (NSButton*)toggle:(int)index on:(BOOL)on {
    NSString* key = [@"DebugMap" stringByAppendingString:sMapToggles[index]];
    NSButton* box = [NSButton checkboxWithTitle:sMapToggles[index] target:self action:@selector(toggleChanged:)];
    box.identifier = key;
    box.controlSize = NSControlSizeSmall;
    box.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    NSNumber* saved = [NSUserDefaults.standardUserDefaults objectForKey:key];
    box.state = (saved != nil ? saved.boolValue : on) ? NSControlStateValueOn : NSControlStateValueOff;
    [self.controls addView:box inGravity:NSStackViewGravityLeading];
    return box;
}

- (void)toggleChanged:(NSButton*)sender {
    [NSUserDefaults.standardUserDefaults setBool:sender.state == NSControlStateValueOn forKey:sender.identifier];
    [self refresh];
}

- (void)panelOpened {
    FrontendMapRetain();
}

- (void)panelClosed {
    FrontendMapRelease();
    // Only the overviews being drawn stay; nothing is kept while the window is closed.
    [_overviews removeAllObjects];
    [_overviewOrder removeAllObjects];
    _shown = nil;
    _scrolledTo = nil;
    [self.class wanted:nil by:self];
    if (self.mode != MapModeRoom) {
        [_view setPixels:NULL width:0 height:0];
    }
}

- (void)tick {
    BOOL fresh = FrontendCopyMapState(_state, &_stateSequence);
    BOOL drawn = FrontendCopyMapImage(_image, &_imageSequence);
    _hasState = _hasState || fresh;
    if (drawn || (fresh && self.collisionLayer != _composedLayer)) {
        [self compose];
    }
    if (fresh || drawn) {
        [self update];
    }
}

- (void)refresh {
    _view.zoom = sMapZooms[_zoom.indexOfSelectedItem];
    [self compose];
    [self update];
}

// Whether the picture is of the room the player is in now.
- (BOOL)imageCurrent {
    return _hasState && _state->inGame && _image->sequence != 0 && _image->area == _state->area &&
           _image->room == _state->room;
}

- (BOOL)on:(NSButton*)box {
    return box.state == NSControlStateValueOn;
}

// GetLayerByIndex: layer 2 is the top layer, anything else the bottom one.
- (int)collisionLayer {
    return _hasState && _state->playerLayer == 2 ? 1 : 0;
}

- (void)compose {
    int w = _image->width, h = _image->height;
    if (_image->sequence == 0) {
        return;
    }
    BOOL bottom = [self on:_bottom], top = [self on:_top], collision = [self on:_collision], grid = [self on:_grid];
    int layer = self.collisionLayer;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int i = y * w + x;
            uint16_t c = _image->backdrop;
            if (bottom && _image->layer[0][i] != MAPVIEW_TRANSPARENT) {
                c = _image->layer[0][i];
            }
            if (top && _image->layer[1][i] != MAPVIEW_TRANSPARENT) {
                c = _image->layer[1][i];
            }
            uint8_t solid = collision ? _image->collision[layer][(y / 16) * MAPVIEW_TILES + x / 16] : 0;
            if (solid == 0x0F) {
                c = ((c >> 1) & 0x3DEF) & ~0x1F; // as TMC_MAPDUMP: half brightness, red dropped
            }
            uint32_t rgb = GfxRgb(c);
            if (solid != 0 && solid != 0x0F) {
                rgb = 0xFF000000u | (((rgb & 0xFEFEFE) >> 1) + 0x7F4000); // half way to orange
            }
            if (grid) { // as TMC_MAPDUMP: every 16 pixels dotted, every 64 white, marks every 256
                if (x % 64 == 0 || y % 64 == 0) {
                    rgb = (x % 256 == 0 && y % 256 == 0) ? 0xFFFF00FF : 0xFFFFFFFF;
                } else if ((x % 16 == 0 || y % 16 == 0) && ((x + y) & 1)) {
                    rgb = 0xFF848484;
                }
            }
            _pixels[i] = rgb;
        }
    }
    _composedLayer = layer;
    if (self.mode == MapModeRoom) {
        [_view setPixels:_pixels width:w height:h];
    }
}

static NSColor* KindColor(int kind) {
    switch (kind) { // TMC_MAPDUMP's marker colours
        case 1: return [NSColor colorWithSRGBRed:1 green:1 blue:1 alpha:1];       // player
        case 7: return [NSColor colorWithSRGBRed:0 green:8 / 31.0 blue:1 alpha:1]; // NPC
        case 3: return [NSColor colorWithSRGBRed:1 green:0 blue:0 alpha:1];       // enemy
        case 6: return [NSColor colorWithSRGBRed:1 green:1 blue:0 alpha:1];       // object
        default: return [NSColor colorWithWhite:20 / 31.0 alpha:1];
    }
}

static NSString* KindName(int kind) {
    switch (kind) {
        case 1: return @"player";
        case 3: return @"enemy";
        case 4: return @"projectile";
        case 6: return @"object";
        case 7: return @"NPC";
        case 8: return @"player item";
        default: return [NSString stringWithFormat:@"kind %d", kind];
    }
}

// The entity under picture pixel x, y: inside its marker or its hitbox, the nearest.
- (const MapViewEntity*)entityAtX:(NSInteger)x y:(NSInteger)y {
    const MapViewEntity* best = NULL;
    long bestDistance = LONG_MAX;
    for (int i = 0; i < _state->entityCount; i++) {
        const MapViewEntity* e = &_state->entities[i];
        int size = e->kind == 1 ? 6 : 4;
        long dx = x - e->x, dy = y - e->y;
        BOOL inMarker = labs(dx) <= size && labs(dy) <= size;
        BOOL inHitbox =
            e->hasHitbox && labs(dx - e->hitboxX) <= e->hitboxWidth && labs(dy - e->hitboxY) <= e->hitboxHeight;
        if ((inMarker || inHitbox) && dx * dx + dy * dy < bestDistance) {
            best = e;
            bestDistance = dx * dx + dy * dy;
        }
    }
    return best;
}

- (void)update {
    if (self.mode != MapModeRoom) {
        [self updateOverview];
        return;
    }
    BOOL current = self.imageCurrent;
    NSMutableArray<NSBezierPath*>* paths = [NSMutableArray array];
    NSMutableArray<NSColor*>* colors = [NSMutableArray array];
    const MapViewEntity* hovered = current && _hoverX >= 0 ? [self entityAtX:_hoverX y:_hoverY] : NULL;
    if (current) {
        for (int i = 0; i < _state->entityCount; i++) {
            const MapViewEntity* e = &_state->entities[i];
            int size = e->kind == 1 ? 6 : 4;
            // TMC_MAPDUMP's squares: pixels x - size .. x + size, outlined.
            NSRect marker = NSMakeRect(e->x - size + 0.5, e->y - size + 0.5, size * 2, size * 2);
            [paths addObject:[NSBezierPath bezierPathWithRect:marker]];
            [colors addObject:KindColor(e->kind)];
            if (e == hovered && e->hasHitbox) {
                NSRect hitbox = NSMakeRect(e->x + e->hitboxX - e->hitboxWidth, e->y + e->hitboxY - e->hitboxHeight,
                                           e->hitboxWidth * 2, e->hitboxHeight * 2);
                NSBezierPath* box = [NSBezierPath bezierPathWithRect:hitbox];
                CGFloat dash[2] = { 3, 2 };
                [box setLineDash:dash count:2 phase:0];
                [paths addObject:box];
                [colors addObject:KindColor(e->kind)];
            }
        }
        [paths addObject:[NSBezierPath bezierPathWithRect:NSMakeRect(_state->cameraX + 0.5, _state->cameraY + 0.5,
                                                                     PPU_WIDTH - 1, PPU_HEIGHT - 1)]];
        [colors addObject:[NSColor colorWithSRGBRed:0.2 green:1 blue:0.45 alpha:1]];
        // A new room: show where the camera is.
        if (_shownRoom != (_state->area << 8 | _state->room)) {
            _shownRoom = _state->area << 8 | _state->room;
            CGFloat zoom = _view.zoom;
            [_view scrollRectToVisible:NSMakeRect(_state->cameraX * zoom, _state->cameraY * zoom, PPU_WIDTH * zoom,
                                                  PPU_HEIGHT * zoom)];
        }
    }
    _view.overlayColors = colors;
    _view.overlays = paths;
    [self updateRooms];
    [self updateTitle];
    [self updateInfoWithEntity:hovered];
}

- (void)updateTitle {
    NSString* title = @"Map";
    if (_hasState && _state->inGame) {
        const char* name = RoomName(_state->area, _state->room);
        title = [NSString stringWithFormat:@"Map — %s (%02x:%02x)", name ?: "unnamed room", _state->area,
                                           _state->room];
    }
    if (self.mode != MapModeRoom && _shown != nil) {
        const MapOverview* o = &_shown->overview;
        char overview[160];
        MapOverviewTitle(o->kind, o->kind == MAP_OVERVIEW_AREA ? o->area : o->dungeon, o->floor, overview,
                         sizeof(overview));
        title = [title stringByAppendingFormat:@" — %s", overview];
    }
    if (![title isEqualToString:_title]) {
        _title = title;
        self.window.title = title;
    }
}

// The rooms of the current area, current room selected (rebuilt when the area changes).
- (void)updateRooms {
    if (!_hasState || !_state->inGame) {
        return;
    }
    if (_roomsArea != _state->area) {
        _roomsArea = _state->area;
        _shownRoom = -1;
        [_rooms removeAllItems];
        for (int room = 0; room < WarpRoomCount(_roomsArea); room++) {
            if (!WarpRoomIsValid(_roomsArea, room)) {
                continue;
            }
            const char* name = RoomName(_roomsArea, room);
            [_rooms addItemWithTitle:[NSString stringWithFormat:@"%02x %s", room, name ?: ""]];
            _rooms.lastItem.tag = room;
        }
        _roomsRoom = -1;
    }
    if (_roomsRoom != _state->room) {
        _roomsRoom = _state->room;
        [_rooms selectItemWithTag:_state->room];
    }
}

- (void)updateInfoWithEntity:(const MapViewEntity*)e {
    NSString* head;
    NSString* hover = @"hover a tile or an entity; right-click a tile to move Link there";
    if (!_hasState) {
        head = @"waiting for the game";
    } else if (!_state->inGame) {
        head = [NSString stringWithFormat:@"frame %llu — not in gameplay", (unsigned long long)_state->frame];
    } else {
        head = [NSString stringWithFormat:@"frame %llu — %dx%d (%dx%d tiles), %d entities, camera %d,%d, "
                                          @"Link %d,%d layer %d%@%@",
                                          (unsigned long long)_state->frame, _state->width, _state->height,
                                          (_state->width + 15) / 16, (_state->height + 15) / 16, _state->entityCount,
                                          _state->cameraX, _state->cameraY, _state->playerX, _state->playerY,
                                          _state->playerLayer, _state->control ? @"" : @", no control",
                                          self.imageCurrent ? @"" : @" — drawing the room once it has settled"];
    }
    if (e != NULL) {
        hover = [NSString stringWithFormat:@"%@ id 0x%02x type %d action %d/%d — at %d,%d, list %d, layer %d",
                                           KindName(e->kind), e->id, e->type, e->action, e->subAction, e->x, e->y,
                                           e->list, e->layer];
        if (e->hasHitbox) {
            hover = [hover stringByAppendingFormat:@", hitbox %+d,%+d %dx%d", e->hitboxX, e->hitboxY,
                                                   e->hitboxWidth * 2, e->hitboxHeight * 2];
        }
    } else if (_hoverX >= 0 && _image->sequence != 0) {
        int t = (int)(_hoverY / 16) * MAPVIEW_TILES + (int)(_hoverX / 16);
        hover = [NSString stringWithFormat:@"tile %ld,%ld (pixel %ld,%ld): bottom metatile 0x%03x collision %02x, "
                                           @"top metatile 0x%03x collision %02x",
                                           (long)_hoverX / 16, (long)_hoverY / 16, (long)_hoverX, (long)_hoverY,
                                           _image->metatile[0][t], _image->collision[0][t], _image->metatile[1][t],
                                           _image->collision[1][t]];
    }
    NSString* status = _hasState && _state->status[0] ? @(_state->status) : @"";
    self.info.stringValue = [NSString stringWithFormat:@"%@\n%@\n%@", head, hover, status];
}

- (void)hoverAtX:(NSInteger)x y:(NSInteger)y {
    _hoverX = x;
    _hoverY = y;
    if (self.mode != MapModeRoom) {
        int room = _shown != nil && x >= 0 ? MapOverviewRoomAt(&_shown->overview, (int)x, (int)y) : -1;
        if (room == _hoverRoom) {
            return;
        }
        _hoverRoom = room;
    }
    [self update];
}

- (NSMenu*)menuAtX:(NSInteger)x y:(NSInteger)y {
    if (self.mode != MapModeRoom) {
        return [self overviewMenuAtX:x y:y];
    }
    if (!self.imageCurrent) {
        return nil;
    }
    int t = (int)(y / 16) * MAPVIEW_TILES + (int)(x / 16);
    uint8_t collision = _image->collision[self.collisionLayer][t];
    NSMenu* menu = [[NSMenu alloc] initWithTitle:@"Map"];
    menu.autoenablesItems = NO;
    NSString* title = [NSString stringWithFormat:@"Move Link Here (tile %ld,%ld)", (long)x / 16, (long)y / 16];
    if (collision != 0) {
        title = [title stringByAppendingFormat:@" — not walkable (collision %02x)", collision];
    } else if (!_state->control) {
        title = [title stringByAppendingString:@" — Link has no control"];
    }
    NSMenuItem* move = [menu addItemWithTitle:title action:@selector(moveLink:) keyEquivalent:@""];
    move.target = self;
    move.representedObject = @[ @(x), @(y) ];
    move.enabled = collision == 0 && _state->control;
    return menu;
}

- (void)moveLink:(NSMenuItem*)sender {
    NSArray<NSNumber*>* at = sender.representedObject;
    if (!FrontendRequestMap(FRONTEND_MAP_MOVE, at[0].intValue, at[1].intValue)) {
        NSBeep();
    }
}

- (void)warp:(id)sender {
    if (_roomsArea < 0 || _rooms.selectedItem == nil ||
        !FrontendRequestMap(FRONTEND_MAP_WARP, _roomsArea, (int)_rooms.selectedItem.tag)) {
        NSBeep();
    }
}

- (void)redraw:(id)sender {
    if (!FrontendRequestMap(FRONTEND_MAP_REDRAW, 0, 0)) {
        NSBeep();
    }
}

- (NSString*)summary {
    static NSString* const modes[] = { @"room", @"area", @"floor" };
    char line[512];
    MapViewSummary(_state, _image, line, sizeof(line));
    NSString* summary = [NSString stringWithFormat:@"%s; mode %@", line, modes[self.mode]];
    if (self.mode == MapModeRoom) {
        return summary;
    }
    if (_shown == nil) {
        return [summary stringByAppendingFormat:@"; overview %@ not drawn yet%@", _wantedKey ?: @"(none)",
                                                [_undrawable containsObject:_wantedKey ?: @""] ? @" (nothing to draw)"
                                                                                                 : @""];
    }
    // The test point: Link, when the overview has his room, else the overview's centre.
    const MapOverview* o = &_shown->overview;
    int i = [self currentRoomIndex], x = o->width / 2, y = o->height / 2;
    if (i >= 0) {
        x = (o->map.rooms[i].x + _state->playerX) / o->scale;
        y = (o->map.rooms[i].y + _state->playerY) / o->scale;
    }
    MapOverviewSummary(o, x, y, line, sizeof(line));
    return [summary stringByAppendingFormat:@"; %s (test point: %@)", line, i >= 0 ? @"Link" : @"centre"];
}

#pragma mark Overview

// What the mode and the picker ask for: false if nothing (Follow Link outside gameplay or a dungeon).
- (BOOL)wantedOverview:(MapOverviewKind*)kind a:(int*)a b:(int*)b {
    NSInteger tag = _picker.selectedItem != nil ? _picker.selectedItem.tag : -1;
    BOOL follow = tag < 0, inGame = _hasState && _state->inGame;
    *b = 0;
    if (self.mode == MapModeArea) {
        *kind = MAP_OVERVIEW_AREA;
        *a = follow ? _state->area : (int)tag;
        return !follow || inGame;
    }
    *kind = MAP_OVERVIEW_FLOOR;
    if (follow) {
        return inGame && AreaMapFloorOf(_state->area, _state->room, a, b);
    }
    *a = (int)(tag >> 8);
    *b = (int)(tag & 0xFF);
    return YES;
}

static NSString* OverviewKey(MapOverviewKind kind, int a, int b) {
    return kind == MAP_OVERVIEW_AREA ? [NSString stringWithFormat:@"area %02x", a]
                                     : [NSString stringWithFormat:@"dungeon %d floor %d", a, b];
}

// The key the queue should still draw (nil: none), shared with the queue.
+ (void)wanted:(NSString*)key by:(MapPanel*)panel {
    @synchronized(panel) {
        panel->_wantedKey = key;
    }
}

- (NSString*)wantedKeyLocked {
    @synchronized(self) {
        return _wantedKey;
    }
}

// Draw on the queue, one at a time (the full-size picture can be 75 MB until it is scaled), unless the window
// wants another one by the time the queue gets to it.
- (void)drawOverview:(MapOverviewKind)kind a:(int)a b:(int)b key:(NSString*)key {
    if ([_drawing containsObject:key] || [_undrawable containsObject:key]) {
        return;
    }
    [_drawing addObject:key];
    __weak MapPanel* weakSelf = self;
    dispatch_async(_overviewQueue, ^{
        MapPanel* panel = weakSelf;
        MapOverviewImage* image = nil;
        BOOL drawn = NO;
        if (panel != nil && [key isEqualToString:[panel wantedKeyLocked]]) {
            image = [MapOverviewImage new];
            drawn = MapOverviewRender(kind, a, b, &image->overview);
        }
        panel = nil;
        dispatch_async(dispatch_get_main_queue(), ^{
            [weakSelf overviewDrawn:image drawn:drawn key:key];
        });
    });
}

- (void)overviewDrawn:(MapOverviewImage*)image drawn:(BOOL)drawn key:(NSString*)key {
    [_drawing removeObject:key];
    if (image != nil && !drawn) {
        [_undrawable addObject:key];
    }
    if (_wantedKey == nil) { // the window was closed meanwhile (or wants nothing)
        return;
    }
    if (drawn) {
        _overviews[key] = image;
        [_overviewOrder removeObject:key];
        [_overviewOrder addObject:key];
        [self trimOverviews];
    }
    [self update];
}

// Forget the least recently shown overviews beyond MAP_OVERVIEW_CACHE_BYTES, never the one shown.
- (void)trimOverviews {
    size_t bytes = 0;
    for (NSString* key in _overviewOrder) {
        bytes += (size_t)_overviews[key]->overview.width * _overviews[key]->overview.height * 4;
    }
    for (NSUInteger i = 0; i < _overviewOrder.count && bytes > MAP_OVERVIEW_CACHE_BYTES;) {
        MapOverviewImage* image = _overviews[_overviewOrder[i]];
        if (image == _shown) {
            i++;
            continue;
        }
        bytes -= (size_t)image->overview.width * image->overview.height * 4;
        [_overviews removeObjectForKey:_overviewOrder[i]];
        [_overviewOrder removeObjectAtIndex:i];
    }
}

// Index of Link's room in the shown overview, or -1.
- (int)currentRoomIndex {
    if (_shown == nil || !_hasState || !_state->inGame) {
        return -1;
    }
    return MapOverviewFindRoom(&_shown->overview, _state->area, _state->room);
}

static NSRect OverviewRect(const MapOverview* o, int x, int y, int width, int height) {
    CGFloat s = o->scale;
    return NSMakeRect(x / s + 0.5, y / s + 0.5, MAX(width / s - 1, 1), MAX(height / s - 1, 1));
}

- (void)updateOverview {
    MapOverviewKind kind;
    int a, b;
    NSString* key = nil;
    if ([self wantedOverview:&kind a:&a b:&b]) {
        key = OverviewKey(kind, a, b);
    }
    if (!(key == _wantedKey || [key isEqualToString:_wantedKey])) {
        [self.class wanted:key by:self];
    }
    MapOverviewImage* image = key != nil ? _overviews[key] : nil;
    if (key != nil && image == nil) {
        [self drawOverview:kind a:a b:b key:key]; // the old picture stays until the new one is there
    }
    if (image != nil && image != _shown) {
        _shown = image;
        _hoverRoom = -1;
        [_overviewOrder removeObject:key];
        [_overviewOrder addObject:key];
        [_view setPixels:image->overview.pixels width:image->overview.width height:image->overview.height];
    } else if (key == nil && _shown != nil) {
        _shown = nil;
        _hoverRoom = -1;
        [_view setPixels:NULL width:0 height:0];
    }

    NSMutableArray<NSBezierPath*>* paths = [NSMutableArray array];
    NSMutableArray<NSColor*>* colors = [NSMutableArray array];
    int current = [self currentRoomIndex];
    if (_shown != nil) {
        const MapOverview* o = &_shown->overview;
        if (_hoverRoom >= 0 && _hoverRoom != current) {
            const AreaMapRoom* r = &o->map.rooms[_hoverRoom];
            NSBezierPath* box = [NSBezierPath bezierPathWithRect:OverviewRect(o, r->x, r->y, r->width, r->height)];
            CGFloat dash[2] = { 4, 3 };
            [box setLineDash:dash count:2 phase:0];
            [paths addObject:box];
            [colors addObject:[NSColor colorWithSRGBRed:1 green:1 blue:0 alpha:1]];
        }
        if (current >= 0) {
            const AreaMapRoom* r = &o->map.rooms[current];
            CGFloat s = o->scale, size = MAX(6 / s, 2);
            [paths addObject:[NSBezierPath bezierPathWithRect:OverviewRect(o, r->x, r->y, r->width, r->height)]];
            [colors addObject:[NSColor colorWithSRGBRed:1 green:0.1 blue:0.9 alpha:1]];
            [paths addObject:[NSBezierPath bezierPathWithRect:OverviewRect(o, r->x + _state->cameraX,
                                                                           r->y + _state->cameraY, PPU_WIDTH,
                                                                           PPU_HEIGHT)]];
            [colors addObject:[NSColor colorWithSRGBRed:0.2 green:1 blue:0.45 alpha:1]];
            NSPoint link = NSMakePoint((r->x + _state->playerX) / s, (r->y + _state->playerY) / s);
            [paths addObject:[NSBezierPath bezierPathWithRect:NSMakeRect(link.x - size, link.y - size, size * 2,
                                                                         size * 2)]];
            [colors addObject:NSColor.whiteColor];
            // A new overview or a new room: show where Link is.
            NSString* scrolled = [NSString stringWithFormat:@"%@ %02x:%02x", key, _state->area, _state->room];
            if (![scrolled isEqualToString:_scrolledTo]) {
                _scrolledTo = scrolled;
                CGFloat zoom = _view.zoom;
                [_view scrollRectToVisible:NSMakeRect((link.x - PPU_WIDTH / s) * zoom, (link.y - PPU_HEIGHT / s) * zoom,
                                                      PPU_WIDTH * 2 / s * zoom, PPU_HEIGHT * 2 / s * zoom)];
            }
        }
    }
    _view.overlayColors = colors;
    _view.overlays = paths;
    [self updateTitle];
    [self updateOverviewInfoForKey:key];
}

- (void)updateOverviewInfoForKey:(NSString*)key {
    NSString* head;
    NSString* hover = @"hover a room for its name; double-click it, or right-click it and choose Warp, to go there";
    if (key == nil) {
        head = self.mode == MapModeFloor && _hasState && _state->inGame
                   ? @"Link is not on a dungeon floor: choose one from the list"
                   : @"not in gameplay: choose an area or floor from the list";
    } else if (_shown == nil || [_undrawable containsObject:key]) {
        head = [NSString stringWithFormat:@"%@: %@", key,
                                          [_undrawable containsObject:key] ? @"nothing to draw" : @"drawing…"];
    } else {
        const MapOverview* o = &_shown->overview;
        char title[160];
        MapOverviewTitle(o->kind, o->kind == MAP_OVERVIEW_AREA ? o->area : o->dungeon, o->floor, title,
                         sizeof(title));
        head = [NSString stringWithFormat:@"%s: %d rooms, %dx%d%@%@", title, o->map.roomCount, o->map.width,
                                          o->map.height,
                                          o->scale > 1 ? [NSString stringWithFormat:@" shown at 1/%d (%dx%d)",
                                                                                    o->scale, o->width, o->height]
                                                       : @"",
                                          [self currentRoomIndex] >= 0 ? @"" : @" — Link is not in it"];
        if (![key isEqualToString:OverviewKey(o->kind, o->kind == MAP_OVERVIEW_AREA ? o->area : o->dungeon,
                                              o->floor)]) {
            head = [head stringByAppendingFormat:@" — drawing %@…", key];
        }
        if (_hoverRoom >= 0) {
            const AreaMapRoom* r = &o->map.rooms[_hoverRoom];
            hover = [NSString stringWithFormat:@"room %02x:%02x %s — %dx%d, map position %d,%d%@%@", r->area, r->room,
                                               r->name ?: "(unnamed)", r->width, r->height, r->worldX, r->worldY,
                                               r->atMapPosition ? @"" : @" (shares it: shown below the others)",
                                               WarpRoomIsValid(r->area, r->room) ? @" — double-click to warp" : @""];
        }
    }
    NSString* status = _hasState && _state->status[0] ? @(_state->status) : @"";
    self.info.stringValue = [NSString stringWithFormat:@"%@\n%@\n%@", head, hover, status];
}

- (void)warpToArea:(int)area room:(int)room {
    if (!FrontendRequestMap(FRONTEND_MAP_WARP, area, room)) {
        NSBeep();
    }
}

- (void)doubleClickAtX:(NSInteger)x y:(NSInteger)y {
    int area, room;
    if (self.mode == MapModeRoom || _shown == nil) {
        return;
    }
    if (MapOverviewWarpTarget(&_shown->overview, (int)x, (int)y, &area, &room)) {
        [self warpToArea:area room:room];
    } else {
        NSBeep();
    }
}

- (NSMenu*)overviewMenuAtX:(NSInteger)x y:(NSInteger)y {
    int i = _shown != nil ? MapOverviewRoomAt(&_shown->overview, (int)x, (int)y) : -1, area, room;
    if (i < 0) {
        return nil;
    }
    const AreaMapRoom* r = &_shown->overview.map.rooms[i];
    BOOL ok = MapOverviewWarpTarget(&_shown->overview, (int)x, (int)y, &area, &room);
    NSMenu* menu = [[NSMenu alloc] initWithTitle:@"Map"];
    menu.autoenablesItems = NO;
    NSString* title = [NSString stringWithFormat:@"Warp to %02x:%02x %s", r->area, r->room, r->name ?: "(unnamed)"];
    if (!ok) {
        title = [title stringByAppendingString:@" — the game cannot load it"];
    } else if (!_state->control) {
        title = [title stringByAppendingString:@" — once Link has control"];
    }
    NSMenuItem* item = [menu addItemWithTitle:title action:@selector(warpFromMenu:) keyEquivalent:@""];
    item.target = self;
    item.representedObject = @[ @(r->area), @(r->room) ];
    item.enabled = ok;
    return menu;
}

- (void)warpFromMenu:(NSMenuItem*)sender {
    NSArray<NSNumber*>* target = sender.representedObject;
    [self warpToArea:target[0].intValue room:target[1].intValue];
}

@end

#pragma mark - Registry

@implementation DebugWindows {
    Renderer* _renderer;
    DebugPanel* _panels[DebugPanelCount];
}

- (instancetype)initWithRenderer:(Renderer*)renderer {
    if ((self = [super init])) {
        _renderer = renderer;
    }
    return self;
}

+ (NSString*)titleForPanel:(DebugPanelKind)kind {
    static NSString* const titles[DebugPanelCount] = { @"Tiles", @"Sprites", @"Layers", @"Palettes", @"IO Registers",
                                                       @"Shader Buffers", @"Map" };
    return titles[kind];
}

- (DebugPanel*)panel:(DebugPanelKind)kind {
    if (_panels[kind] == nil) {
        switch (kind) {
            case DebugPanelTiles:
                _panels[kind] = [TilesPanel new];
                break;
            case DebugPanelSprites:
                _panels[kind] = [[SpritesPanel alloc] initWithRenderer:_renderer];
                break;
            case DebugPanelLayers:
                _panels[kind] = [LayersPanel new];
                break;
            case DebugPanelPalettes:
                _panels[kind] = [PalettesPanel new];
                break;
            case DebugPanelIo:
                _panels[kind] = [IoPanel new];
                break;
            case DebugPanelMap:
                _panels[kind] = [MapPanel new];
                break;
            default:
                _panels[kind] = [[ShaderPanel alloc] initWithRenderer:_renderer];
                break;
        }
    }
    return _panels[kind];
}

- (void)showPanel:(DebugPanelKind)kind {
    [[self panel:kind] showWindow:nil];
}

- (NSWindow*)windowForPanel:(DebugPanelKind)kind {
    return [self panel:kind].window;
}

- (void)closeAll {
    for (int i = 0; i < DebugPanelCount; i++) {
        [_panels[i].window close];
    }
}

- (NSString*)summaryForPanel:(DebugPanelKind)kind {
    return [[self panel:kind] summary];
}

- (void)selectSprite:(NSInteger)index highlight:(BOOL)highlight {
    [(SpritesPanel*)[self panel:DebugPanelSprites] selectSprite:index highlight:highlight];
}

- (void)selectLayer:(NSInteger)bg {
    [(LayersPanel*)[self panel:DebugPanelLayers] selectLayer:bg];
}

- (void)selectShaderBuffer:(DebugBuffer)buffer {
    [(ShaderPanel*)[self panel:DebugPanelShaderBuffers] selectBuffer:buffer];
}

@end
