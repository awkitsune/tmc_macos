#import <MetalKit/MetalKit.h>

NS_ASSUME_NONNULL_BEGIN

// Keep in sync with the enum in shaders.metal.
typedef NS_ENUM(NSUInteger, FilterMode) {
    FilterModeNearest = 0,
    FilterModeSharpBilinear = 1,
    FilterModeBilinear = 2,
    FilterModeLCD = 3,
    FilterModeCRT = 4,
    FilterModeCount,
};

NSString* FilterModeName(FilterMode mode);

/*
 * Presents the game's newest frame in an MTKView: uploads it to a texture when
 * it changes and draws it centred, scaled by a whole number (integerScaling) or
 * as large as the aspect ratio allows.
 */
@interface Renderer : NSObject <MTKViewDelegate>
@property(nonatomic) FilterMode filter;
@property(nonatomic) BOOL integerScaling;
// Called on the main thread at the start of every display refresh (input polling).
@property(nonatomic, copy) void (^beforeDraw)(void);
- (nullable instancetype)initWithView:(MTKView*)view;
// Save the next presented drawable as a PNG (needs view.framebufferOnly = NO).
- (void)captureNextFrameToURL:(NSURL*)url;
@end

NS_ASSUME_NONNULL_END
