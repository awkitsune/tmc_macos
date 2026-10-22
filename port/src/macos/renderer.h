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

/* Shader knobs, adjustable at runtime (Settings window). Same order as Uniforms in shaders.metal. */
typedef struct {
    float lcdGridStrength;
    float lcdGridWidth;
    float lcdSubpixel;
    float lcdBrightness;
    float crtScanlines;
    float crtMask;
    float crtBrightness;
    float colorCorrection; // all filters
    float gamma;           // all filters
    float crtBloom;
    float crtBeamMin;
    float crtBeamMax;
    float lcdGapBloom;
    float lcdResponseMs;   // LCD response: 10-90% time of a pixel getting brighter
    float lcdDarkSlowdown; // how much longer getting darker takes
} FilterParams;

/*
 * Share of the old picture an LCD with this response keeps after `frames` game
 * frames: a first-order (exponential) response whose 10-90% time is
 * responseMs, so tau = responseMs / ln 9. 0 means no ghosting.
 */
float LcdResponseKeep(float responseMs, double frames);

FilterParams FilterParamsDefaults(void);

/*
 * Debugging (Shader Buffers window): the intermediate textures of the
 * presentation pipeline, any of which can be copied back to the CPU.
 *   upload -> [LCD response: history (linear), output] -> filter pass -> drawable
 * The filter pass is one fragment shader; its stages (DebugBufferFilterRaw ..
 * DebugBufferPreGamma) are captured by drawing it again into a float16 texture
 * of the viewport's size with a debug stage selected, so the picture itself is
 * drawn exactly as without the window.
 */
typedef NS_ENUM(NSInteger, DebugBuffer) {
    DebugBufferNone = -1,
    DebugBufferFrame = 0,       // the uploaded game frame (BGRA8, 240x160)
    DebugBufferResponseHistory, // LCD response history (RGBA16F, linear light)
    DebugBufferResponseOutput,  // LCD response output (RGBA16F, gamma-encoded)
    DebugBufferFilterInput,     // what the filter pass sampled (one of the above)
    DebugBufferFilterRaw,       // filter output before colour correction and gamma (RGBA16F)
    DebugBufferBeams,           // CRT: summed scanline beams, before the brightness mix
    DebugBufferMask,            // LCD grid x subpixel mask, or the CRT aperture grille
    DebugBufferBloom,           // R spot brightness, G beam width / LCD gap, B scanline strength
    DebugBufferPreGamma,        // after colour correction, before gamma (saturated)
    DebugBufferDrawable,        // the final drawable, viewport region (BGRA8)
    DebugBufferCount,
};

NSString* DebugBufferName(DebugBuffer buffer);

/* One copied-back debug buffer. */
@interface DebugReadback : NSObject
@property(nonatomic, readonly) DebugBuffer buffer;
@property(nonatomic, readonly) NSUInteger width, height;
@property(nonatomic, readonly) BOOL isFloat;     // RGBA16F; otherwise BGRA8 unorm
@property(nonatomic, readonly) NSData* bytes;    // rows packed, 8 (float) or 4 bytes per pixel
@property(nonatomic, readonly) NSString* format; // e.g. "RGBA16Float, linear"
@property(nonatomic, readonly) NSString* note;   // why it may be stale or empty; "" if live
@property(nonatomic, readonly) uint64_t serial;  // increases with every readback
// RGBA as stored (floats as they are; unorm as 0-1).
- (simd_float4)pixelAtX:(NSUInteger)x y:(NSUInteger)y;
// Per channel minimum, maximum, mean, and a checksum of the bytes.
- (NSString*)statistics;
- (simd_float4)mean;
@end

/*
 * Presents the game's newest frame in an MTKView: uploads it to a texture when
 * it changes and draws it centred, scaled by a whole number (integerScaling) or
 * as large as the aspect ratio allows.
 */
@interface Renderer : NSObject <MTKViewDelegate>
@property(nonatomic) FilterMode filter;
@property(nonatomic) BOOL integerScaling;
@property(nonatomic) FilterParams params;
// Show a fixed grey/white/colour test card instead of the game (for checking filters).
@property(nonatomic) BOOL testPattern;
// LCD response (pixel ghosting, all filters); the times are in params.
@property(nonatomic) BOOL lcdResponse;
// Debugging: the buffer to copy back each few refreshes (DebugBufferNone: none,
// at no cost), and the newest copy.
@property(nonatomic) DebugBuffer debugBuffer;
- (nullable DebugReadback*)latestReadback;
// Debugging: outline this rectangle of the frame (GBA pixels); empty for none.
@property(nonatomic) CGRect highlightRect;
// Called on the main thread at the start of every display refresh (input polling).
@property(nonatomic, copy) void (^beforeDraw)(void);
- (nullable instancetype)initWithView:(MTKView*)view;
// Save the next presented drawable as a PNG (needs view.framebufferOnly = NO).
- (void)captureNextFrameToURL:(NSURL*)url;
// Self-test: run the LCD response kernel on one pixel stepping black to white
// and back with the current params; log the values per frame and the measured
// 10-90% times. Returns whether they match the setting.
- (BOOL)checkLcdResponse;
@end

NS_ASSUME_NONNULL_END
