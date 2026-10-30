#import <AppKit/AppKit.h>

#import "renderer.h"

NS_ASSUME_NONNULL_BEGIN

/*
 * Graphics debug windows (Debug menu): Tiles, Sprites, Layers, Palettes, IO
 * Registers, Shader Buffers and Map. The first five read the game thread's
 * hardware snapshot (FrontendCopyGfxSnapshot), taken only while one of them is
 * open; the Shader Buffers window has the renderer copy one pipeline texture
 * back; the Map window reads the game thread's view of the room
 * (FrontendCopyMapState, port/mapview.h), taken only while it is open, and asks
 * the game thread to move Link or warp. All refresh at about 12 Hz on the main
 * thread while open.
 */
typedef NS_ENUM(NSInteger, DebugPanelKind) {
    DebugPanelTiles,
    DebugPanelSprites,
    DebugPanelLayers,
    DebugPanelPalettes,
    DebugPanelIo,
    DebugPanelShaderBuffers,
    DebugPanelMap,
    DebugPanelPerformance,
    DebugPanelCount,
};

@interface DebugWindows : NSObject
- (instancetype)initWithRenderer:(Renderer*)renderer;
+ (NSString*)titleForPanel:(DebugPanelKind)kind;
- (void)showPanel:(DebugPanelKind)kind;
- (NSWindow*)windowForPanel:(DebugPanelKind)kind; // creates it (hidden) if needed
- (void)closeAll;

// Self-test hooks: a line of numbers describing what a panel shows, and its
// options (Sprites: selected entry and highlight; Layers: selected layer;
// Shader Buffers: buffer, channel, display mode). Map: area, room, picture size,
// sequence and checksum, entities, camera, player (MapViewSummary).
- (NSString*)summaryForPanel:(DebugPanelKind)kind;
- (void)selectSprite:(NSInteger)index highlight:(BOOL)highlight;
- (void)selectLayer:(NSInteger)bg;
- (void)selectShaderBuffer:(DebugBuffer)buffer;
@end

NS_ASSUME_NONNULL_END
