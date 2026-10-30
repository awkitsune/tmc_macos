#import "renderer.h"
#include "port/performance.h"
#import "selftestlog.h"
#import "spritepass.h"

#include <os/lock.h>
#include <stdatomic.h>
#include <zlib.h>

#if TARGET_OS_IOS
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#endif

#include "displaysync.h"
#include "frontend.h"
#include "latency.h"

NSString* FilterModeName(FilterMode mode) {
    switch (mode) {
        case FilterModeNearest:
            return @"Nearest Neighbour";
        case FilterModeSharpBilinear:
            return @"Sharp Bilinear";
        case FilterModeBilinear:
            return @"Smooth (Bilinear)";
        case FilterModeLCD:
            return @"LCD Grid";
        case FilterModeCRT:
            return @"CRT Scanlines";
        default:
            return @"?";
    }
}

FilterParams FilterParamsDefaults(void) {
    return (FilterParams){
        .lcdGridStrength = 0.35f,
        .lcdGridWidth = 0.15f,
        .lcdSubpixel = 0.35f,
        .lcdBrightness = 1.3f,
        .crtScanlines = 1.0f,
        .crtMask = 0.1f,
        .crtBrightness = 1.35f,
        .colorCorrection = 0.0f,
        .gamma = 1.0f,
        .crtBloom = 0.8f,
        .crtBeamMin = 0.14f,
        .crtBeamMax = 0.34f,
        .lcdGapBloom = 0.0f,
        .lcdResponseMs = 22.0f,
        .lcdDarkSlowdown = 1.3f,
    };
}

float LcdResponseKeep(float responseMs, double frames) {
    if (responseMs <= 0) {
        return 0;
    }
    double tau = responseMs / log(9.0);
    return (float)exp(-frames * 1000.0 / FRONTEND_FRAME_RATE / tau);
}

// Keep in sync with Uniforms in shaders.metal.
typedef struct {
    simd_float2 sourceSize;
    simd_float2 outputSize;
    uint32_t filter;
    FilterParams params;
    uint32_t debugStage; // Stage* in shaders.metal; 0 draws the picture
    simd_float2 positionOffset;
} Uniforms;

// Keep in sync with OverlayUniforms in shaders.metal.
typedef struct {
    simd_float4 rect;
    simd_float2 sourceSize;
    simd_float2 outputSize;
    simd_float4 colour;
} OverlayUniforms;

NSString* DebugBufferName(DebugBuffer buffer) {
    switch (buffer) {
        case DebugBufferFrame:
            return @"Game frame";
        case DebugBufferResponseHistory:
            return @"LCD response: history (linear)";
        case DebugBufferResponseOutput:
            return @"LCD response: output";
        case DebugBufferFilterInput:
            return @"Filter pass: input";
        case DebugBufferFilterRaw:
            return @"Filter pass: output before colour/gamma";
        case DebugBufferBeams:
            return @"Filter pass: CRT scanline beams";
        case DebugBufferMask:
            return @"Filter pass: LCD grid/subpixel or CRT grille mask";
        case DebugBufferBloom:
            return @"Filter pass: bloom (brightness, beam width/gap, scanline strength)";
        case DebugBufferPreGamma:
            return @"Filter pass: colour corrected, before gamma";
        case DebugBufferDrawable:
            return @"Final drawable (viewport)";
        default:
            return @"None";
    }
}

// The filter pass's debug stage for a stage buffer (Stage* in shaders.metal), else 0.
static uint32_t DebugStage(DebugBuffer buffer) {
    switch (buffer) {
        case DebugBufferFilterRaw:
            return 1;
        case DebugBufferBeams:
            return 2;
        case DebugBufferMask:
            return 3;
        case DebugBufferBloom:
            return 4;
        case DebugBufferPreGamma:
            return 5;
        default:
            return 0;
    }
}

@implementation DebugReadback

- (instancetype)initWithBuffer:(DebugBuffer)buffer
                         width:(NSUInteger)width
                        height:(NSUInteger)height
                       isFloat:(BOOL)isFloat
                         bytes:(NSData*)bytes
                        format:(NSString*)format
                          note:(NSString*)note
                        serial:(uint64_t)serial {
    if ((self = [super init])) {
        _buffer = buffer;
        _width = width;
        _height = height;
        _isFloat = isFloat;
        _bytes = bytes;
        _format = format;
        _note = note;
        _serial = serial;
    }
    return self;
}

- (simd_float4)pixelAtX:(NSUInteger)x y:(NSUInteger)y {
    if (x >= _width || y >= _height) {
        return (simd_float4){ 0, 0, 0, 0 };
    }
    if (_isFloat) {
        const __fp16* p = (const __fp16*)_bytes.bytes + (y * _width + x) * 4;
        return (simd_float4){ p[0], p[1], p[2], p[3] };
    }
    const uint8_t* p = (const uint8_t*)_bytes.bytes + (y * _width + x) * 4; // B, G, R, A
    return (simd_float4){ p[2] / 255.0f, p[1] / 255.0f, p[0] / 255.0f, p[3] / 255.0f };
}

- (simd_float4)mean {
    simd_float4 sum = 0;
    for (NSUInteger y = 0; y < _height; y++) {
        for (NSUInteger x = 0; x < _width; x++) {
            sum += [self pixelAtX:x y:y];
        }
    }
    return sum / (float)MAX(_width * _height, 1);
}

- (NSString*)statistics {
    simd_float4 lo = { INFINITY, INFINITY, INFINITY, INFINITY }, hi = -lo, sum = 0;
    for (NSUInteger y = 0; y < _height; y++) {
        for (NSUInteger x = 0; x < _width; x++) {
            simd_float4 v = [self pixelAtX:x y:y];
            lo = simd_min(lo, v);
            hi = simd_max(hi, v);
            sum += v;
        }
    }
    simd_float4 mean = sum / (float)MAX(_width * _height, 1);
    uLong crc = crc32(0, _bytes.bytes, (uInt)_bytes.length);
    return [NSString stringWithFormat:@"%lux%lu %@ crc32 %08lx R %.4f..%.4f mean %.4f G %.4f..%.4f mean %.4f "
                                      @"B %.4f..%.4f mean %.4f A %.4f..%.4f",
                                      (unsigned long)_width, (unsigned long)_height, _format, crc, lo.x, hi.x, mean.x,
                                      lo.y, hi.y, mean.y, lo.z, hi.z, mean.z, lo.w, hi.w];
}

@end

// Keep in sync with ResponseUniforms in shaders.metal.
typedef struct {
    float keepRise;
    float keepFall;
} ResponseUniforms;

// Frames in flight: each has its own texture so an upload never touches one the
// GPU may still be reading. Two is the least that keeps the GPU busy; every
// further frame queued is another refresh of input lag. Frames waiting for the
// screen are limited separately (MAX_PENDING_PRESENTS), which is what holds the
// queue short: with it, two or three drawables on the layer measure the same,
// so the layer keeps its default.
#define IN_FLIGHT 2
#define MAX_PENDING_PRESENTS 2
#define PRESENT_STALL_NS 100000000ull

@implementation Renderer {
    id<MTLDevice> _device;
    // For textures the CPU writes or reads: Shared on Apple GPUs; Intel and AMD
    // GPUs allow Shared only for buffers, so Managed there (synchronized after
    // GPU writes before the CPU reads).
    MTLStorageMode _cpuStorage;
    id<MTLCommandQueue> _queue;
    id<MTLRenderPipelineState> _pipeline;
    id<MTLTexture> _textures[IN_FLIGHT];
    id<MTLLibrary> _gpuPpuLibrary, _gpuCompactLibrary;
    id<MTLComputePipelineState> _gpuPpuPipeline, _gpuSpritePipeline, _gpuCompactPipeline, _gpuCompactSpritePipeline;
    SpritePass* _spritePass;
    SpritePass* _compactSpritePass;
    id<MTLBuffer> _gpuPpuInputs[IN_FLIGHT], _gpuCompactInputs[IN_FLIGHT];
    id<MTLTexture> _gpuPpuTextures[IN_FLIGHT];
    BOOL _gpuPpuEnabled, _gpuSpriteQuadsEnabled, _compactGpuCaptureEnabled;
    BOOL _lastFrameGpu, _gpuSelfTestChecked, _gpuOnlyTestChecked, _gpuFallbackTestChecked;
    NSUInteger _gpuPpuGeneration;
    // LCD response: what the simulated LCD shows, in linear light (ping-pong,
    // _history[_historySlot] is current), and the same gamma-encoded for the
    // present pass. One queue with tracked resources, so a pass never writes
    // these while an earlier one still reads them.
    id<MTLRenderPipelineState> _responsePipeline;
    id<MTLTexture> _history[2];
    id<MTLTexture> _shown;
    NSUInteger _historySlot;
    BOOL _historyValid;
    dispatch_semaphore_t _inFlight;
    NSUInteger _slot;
    uint64_t _sequence;
    uint32_t _pixels[PPU_WIDTH * PPU_HEIGHT];
    NSURL* _captureURL;
    // Debugging: sprite outline, filter stage capture and the readback copy.
    id<MTLRenderPipelineState> _overlayPipeline;
    id<MTLRenderPipelineState> _stagePipeline;
    id<MTLTexture> _stageTexture;
    id<MTLTexture> _readbackTexture;
    os_unfair_lock _readbackLock; // guards _readbackBusy, _latestReadback, _latestTaken
    BOOL _readbackBusy;
    DebugReadback* _latestReadback;
    BOOL _latestTaken; // the viewer has fetched _latestReadback: time for another
    uint64_t _readbackSerial;
    NSUInteger _drawCount;
    _Atomic int _presentsPending;
    _Atomic uint64_t _lastPresentedNs; // committed frames not on the screen yet
}

- (nullable instancetype)initWithView:(MTKView*)view {
    if ((self = [super init]) == nil) {
        return nil;
    }
    _device = view.device;
#if TARGET_OS_IOS
    _cpuStorage = MTLStorageModeShared; // every iOS GPU is an Apple GPU
#else
    // TMC_MANAGED_TEXTURES=1 takes the Intel/AMD path on Apple silicon, to test it.
    _cpuStorage = [_device supportsFamily:MTLGPUFamilyApple1] && getenv("TMC_MANAGED_TEXTURES") == NULL
                      ? MTLStorageModeShared
                      : MTLStorageModeManaged;
#endif
    _queue = [_device newCommandQueue];
    _inFlight = dispatch_semaphore_create(IN_FLIGHT);

    NSError* error = nil;
    NSString* path = [NSBundle.mainBundle pathForResource:@"shaders" ofType:@"metal"];
    NSString* source = path ? [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:&error] : nil;
    if (source == nil) {
        NSLog(@"shaders.metal missing from the app bundle: %@", error);
        return nil;
    }
    // Metal 2.4 is what macOS 12, the oldest supported, compiles; pinned so
    // nothing newer slips into the shaders.
    MTLCompileOptions* options = [MTLCompileOptions new];
    options.languageVersion = MTLLanguageVersion2_4;
    id<MTLLibrary> library = [_device newLibraryWithSource:source options:options error:&error];
    if (library == nil) {
        NSLog(@"shader compile failed: %@", error);
        return nil;
    }

    MTLRenderPipelineDescriptor* desc = [MTLRenderPipelineDescriptor new];
    desc.vertexFunction = [library newFunctionWithName:@"present_vertex"];
    desc.fragmentFunction = [library newFunctionWithName:@"present_fragment"];
    desc.colorAttachments[0].pixelFormat = view.colorPixelFormat;
    _pipeline = [_device newRenderPipelineStateWithDescriptor:desc error:&error];
    if (_pipeline == nil) {
        NSLog(@"pipeline creation failed: %@", error);
        return nil;
    }

    desc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
    _stagePipeline = [_device newRenderPipelineStateWithDescriptor:desc error:&error];
    desc.vertexFunction = [library newFunctionWithName:@"overlay_vertex"];
    desc.fragmentFunction = [library newFunctionWithName:@"overlay_fragment"];
    desc.colorAttachments[0].pixelFormat = view.colorPixelFormat;
    _overlayPipeline = [_device newRenderPipelineStateWithDescriptor:desc error:&error];
    if (_stagePipeline == nil || _overlayPipeline == nil) {
        NSLog(@"debug pipeline creation failed: %@", error);
        return nil;
    }
    _readbackLock = OS_UNFAIR_LOCK_INIT;
    _debugBuffer = DebugBufferNone;
    _highlightRect = CGRectZero;

    desc.vertexFunction = [library newFunctionWithName:@"present_vertex"];
    desc.fragmentFunction = [library newFunctionWithName:@"lcd_response"];
    desc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
    desc.colorAttachments[1].pixelFormat = MTLPixelFormatRGBA16Float;
    _responsePipeline = [_device newRenderPipelineStateWithDescriptor:desc error:&error];
    if (_responsePipeline == nil) {
        NSLog(@"LCD response pipeline creation failed: %@", error);
        return nil;
    }

    MTLTextureDescriptor* texDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                       width:PPU_WIDTH
                                                                                      height:PPU_HEIGHT
                                                                                   mipmapped:NO];
    texDesc.usage = MTLTextureUsageShaderRead;
    texDesc.storageMode = _cpuStorage;
    for (int i = 0; i < IN_FLIGHT; i++) {
        _textures[i] = [_device newTextureWithDescriptor:texDesc];
    }
    MTLTextureDescriptor* floatDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                                         width:PPU_WIDTH
                                                                                        height:PPU_HEIGHT
                                                                                     mipmapped:NO];
    floatDesc.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    floatDesc.storageMode = MTLStorageModePrivate;
    _history[0] = [_device newTextureWithDescriptor:floatDesc];
    _history[1] = [_device newTextureWithDescriptor:floatDesc];
    _shown = [_device newTextureWithDescriptor:floatDesc];
    _integerScaling = YES;
    _params = FilterParamsDefaults();
    return self;
}

CGRect RendererFitPicture(CGRect area, BOOL integerScaling) {
    double scale = MIN(area.size.width / PPU_WIDTH, area.size.height / PPU_HEIGHT);
    if (integerScaling && scale >= 1.0) {
        scale = floor(scale);
    }
    double width = round(PPU_WIDTH * scale), height = round(PPU_HEIGHT * scale);
    return CGRectMake(area.origin.x + floor((area.size.width - width) / 2),
                      area.origin.y + floor((area.size.height - height) / 2), width, height);
}

// The picture in the drawable: fitted in pictureRect, or in the whole drawable.
- (MTLViewport)viewportForDrawableSize:(CGSize)size {
    CGRect area = CGRectMake(0, 0, size.width, size.height);
    CGRect inside = CGRectIntersection(area, self.pictureRect);
    CGRect fit = RendererFitPicture(CGRectIsEmpty(inside) ? area : inside, self.integerScaling);
    return (MTLViewport){
        .originX = fit.origin.x,
        .originY = fit.origin.y,
        .width = fit.size.width,
        .height = fit.size.height,
        .znear = 0,
        .zfar = 1,
    };
}

// Top half: white, light grey, mid grey, dark grey columns (for checking that a
// filter keeps neutral colours neutral). Bottom half: colour bars.
- (BOOL)fillTestPattern {
    static const uint32_t greys[] = { 0xFFFFFFFF, 0xFFC0C0C0, 0xFF808080, 0xFF404040 };
    static const uint32_t bars[] = { 0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFF00, 0xFF00FFFF, 0xFFFF00FF };
    for (int y = 0; y < PPU_HEIGHT; y++) {
        for (int x = 0; x < PPU_WIDTH; x++) {
            _pixels[y * PPU_WIDTH + x] = y < PPU_HEIGHT / 2 ? greys[x * 4 / PPU_WIDTH] : bars[x * 6 / PPU_WIDTH];
        }
    }
    return YES;
}

- (void)setLcdResponse:(BOOL)lcdResponse {
    if (lcdResponse && !_lcdResponse) {
        _historyValid = NO; // start from the next frame, not a stale picture
    }
    _lcdResponse = lcdResponse;
}

// One LCD response step: blend frame into history (keep = 0 copies it in).
static void EncodeResponse(id<MTLCommandBuffer> commands, id<MTLRenderPipelineState> pipeline, id<MTLTexture> frame,
                           id<MTLTexture> history, id<MTLTexture> nextHistory, id<MTLTexture> shown,
                           ResponseUniforms uniforms) {
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = nextHistory;
    pass.colorAttachments[1].texture = shown;
    for (int i = 0; i < 2; i++) {
        pass.colorAttachments[i].loadAction = MTLLoadActionDontCare; // every pixel is written
        pass.colorAttachments[i].storeAction = MTLStoreActionStore;
    }
    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:pipeline];
    [encoder setFragmentTexture:frame atIndex:0];
    [encoder setFragmentTexture:history atIndex:1];
    [encoder setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [encoder endEncoding];
}

- (void)captureNextFrameToURL:(NSURL*)url {
    _captureURL = url;
}

static void WriteTexturePNG(id<MTLTexture> texture, NSURL* url) {
    NSUInteger width = texture.width, height = texture.height;
    NSMutableData* data = [NSMutableData dataWithLength:width * height * 4];
    [texture getBytes:data.mutableBytes bytesPerRow:width * 4 fromRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0];
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGContextRef context = CGBitmapContextCreate(data.mutableBytes, width, height, 8, width * 4, space,
                                                 kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGImageRef image = CGBitmapContextCreateImage(context);
#if TARGET_OS_IOS
    CGImageDestinationRef png = CGImageDestinationCreateWithURL((__bridge CFURLRef)url,
                                                                (__bridge CFStringRef)UTTypePNG.identifier, 1, NULL);
    if (png != NULL) {
        CGImageDestinationAddImage(png, image, NULL);
        CGImageDestinationFinalize(png);
        CFRelease(png);
    }
#else
    NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithCGImage:image];
    [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToURL:url atomically:YES];
#endif
    CGImageRelease(image);
    CGContextRelease(context);
    CGColorSpaceRelease(space);
}

- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size {
}

- (BOOL)gpuPpuEnabled { return _gpuPpuEnabled; }

- (BOOL)enableGpuPpu:(BOOL)enabled {
    if (enabled == _gpuPpuEnabled) return YES;
    if (enabled) {
        NSError* error = nil;
        NSString* path = [NSBundle.mainBundle pathForResource:@"gpu_ppu" ofType:@"metal"];
        NSString* source = path ? [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:&error] : nil;
        MTLCompileOptions* options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion2_4;
        id<MTLLibrary> library = source ? [_device newLibraryWithSource:source options:options error:&error] : nil;
        _gpuPpuLibrary = library;
        id<MTLFunction> function = [library newFunctionWithName:@"gpu_ppu_texture"];
        _gpuPpuPipeline = function ? [_device newComputePipelineStateWithFunction:function error:&error] : nil;
        if (!_gpuPpuPipeline) {
            NSLog(@"GPU PPU shader unavailable: %@", error);
            return NO;
        }
        MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                       width:PPU_WIDTH height:PPU_HEIGHT mipmapped:NO];
        desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        desc.storageMode = MTLStorageModePrivate;
        for (int i = 0; i < IN_FLIGHT; i++) {
            _gpuPpuInputs[i] = [_device newBufferWithLength:sizeof(PpuCaptureLine) * PPU_HEIGHT
                                                   options:MTLResourceStorageModeShared];
            _gpuPpuTextures[i] = [_device newTextureWithDescriptor:desc];
            if (!_gpuPpuInputs[i] || !_gpuPpuTextures[i]) {
                NSLog(@"GPU PPU texture or buffer allocation failed");
                for (int j = 0; j < IN_FLIGHT; j++) { _gpuPpuInputs[j] = nil; _gpuPpuTextures[j] = nil; }
                _gpuPpuPipeline = nil;
                return NO;
            }
        }
        if (!FrontendSetGpuPpuEnabled(true)) {
            for (int i = 0; i < IN_FLIGHT; i++) { _gpuPpuInputs[i] = nil; _gpuPpuTextures[i] = nil; }
            _gpuPpuPipeline = nil;
            return NO;
        }
    } else {
        FrontendSetGpuPpuEnabled(false);
        _gpuSpriteQuadsEnabled = NO;
        _compactGpuCaptureEnabled = NO;
        _gpuSpritePipeline = _gpuCompactPipeline = _gpuCompactSpritePipeline = nil;
        _spritePass = _compactSpritePass = nil;
        _gpuPpuLibrary = _gpuCompactLibrary = nil;
        for (int i = 0; i < IN_FLIGHT; i++) _gpuCompactInputs[i] = nil;
        for (int i = 0; i < IN_FLIGHT; i++) _gpuPpuInputs[i] = nil;
        _gpuPpuPipeline = nil;
        // Keep the last GPU texture visible until a new software frame arrives.
    }
    _gpuPpuEnabled = enabled;
    _gpuPpuGeneration++;
    if (enabled) _sequence = 0; // show the latest software frame until a captured frame arrives
    _historyValid = NO;
    return YES;
}

- (BOOL)gpuSpriteQuadsEnabled { return _gpuSpriteQuadsEnabled; }

- (BOOL)enableGpuSpriteQuads:(BOOL)enabled {
    if (enabled == _gpuSpriteQuadsEnabled) return YES;
    if (enabled) {
        if (!_gpuPpuEnabled) return NO;
        NSError* error = nil;
        SpritePass* sprites = [[SpritePass alloc] initWithDevice:_device library:_gpuPpuLibrary error:&error];
        id<MTLFunction> function = [_gpuPpuLibrary newFunctionWithName:@"gpu_ppu_texture_raster"];
        id<MTLComputePipelineState> pipeline = function ? [_device newComputePipelineStateWithFunction:function error:&error] : nil;
        if (!sprites || !pipeline) {
            NSLog(@"GPU sprite quads unavailable: %@", error);
            return NO;
        }
        SpritePass* compactSprites = nil;
        id<MTLComputePipelineState> compactPipeline = nil;
        if (_compactGpuCaptureEnabled) {
            compactSprites = [[SpritePass alloc] initWithDevice:_device library:_gpuCompactLibrary error:&error];
            id<MTLFunction> compactFunction = [_gpuCompactLibrary newFunctionWithName:@"gpu_ppu_texture_raster"];
            compactPipeline = compactFunction ? [_device newComputePipelineStateWithFunction:compactFunction error:&error] : nil;
            if (!compactSprites || !compactPipeline) { NSLog(@"Compact sprite quads unavailable: %@", error); return NO; }
        }
        _spritePass = sprites;
        _gpuSpritePipeline = pipeline;
        _compactSpritePass = compactSprites;
        _gpuCompactSpritePipeline = compactPipeline;
    } else {
        _spritePass = _compactSpritePass = nil;
        _gpuSpritePipeline = _gpuCompactSpritePipeline = nil;
    }
    _gpuSpriteQuadsEnabled = enabled;
    _gpuPpuGeneration++;
    _sequence = 0; // redraw the latest captured frame using the selected sprite path
    _historyValid = NO;
    return YES;
}

- (BOOL)compactGpuCaptureEnabled { return _compactGpuCaptureEnabled; }

- (BOOL)enableCompactGpuCapture:(BOOL)enabled {
    if (enabled == _compactGpuCaptureEnabled) return YES;
    if (enabled) {
        if (!_gpuPpuEnabled) return NO;
        NSError* error = nil;
        NSString* path = [NSBundle.mainBundle pathForResource:@"gpu_ppu" ofType:@"metal"];
        NSString* source = path ? [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:&error] : nil;
        MTLCompileOptions* options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion2_4;
        options.preprocessorMacros = @{ @"PPU_COMPACT": @1 };
        id<MTLLibrary> library = source ? [_device newLibraryWithSource:source options:options error:&error] : nil;
        id<MTLFunction> function = [library newFunctionWithName:@"gpu_ppu_texture"];
        id<MTLComputePipelineState> pipeline = function ? [_device newComputePipelineStateWithFunction:function error:&error] : nil;
        SpritePass* compactSprites = nil;
        id<MTLComputePipelineState> spritePipeline = nil;
        if (_gpuSpriteQuadsEnabled && library) {
            compactSprites = [[SpritePass alloc] initWithDevice:_device library:library error:&error];
            id<MTLFunction> spriteFunction = [library newFunctionWithName:@"gpu_ppu_texture_raster"];
            spritePipeline = spriteFunction ? [_device newComputePipelineStateWithFunction:spriteFunction error:&error] : nil;
        }
        if (!pipeline || (_gpuSpriteQuadsEnabled && (!compactSprites || !spritePipeline))) {
            NSLog(@"Compact GPU capture shader unavailable: %@", error);
            return NO;
        }
        for (int j = 0; j < IN_FLIGHT; j++) {
            _gpuCompactInputs[j] = [_device newBufferWithLength:sizeof(PpuCompactCapture) options:MTLResourceStorageModeShared];
            if (!_gpuCompactInputs[j]) {
                for (int k = 0; k < IN_FLIGHT; k++) _gpuCompactInputs[k] = nil;
                return NO;
            }
        }
        if (!FrontendSetCompactGpuCapture(true)) {
            for (int j = 0; j < IN_FLIGHT; j++) _gpuCompactInputs[j] = nil;
            return NO;
        }
        _gpuCompactLibrary = library;
        _gpuCompactPipeline = pipeline;
        _compactSpritePass = compactSprites;
        _gpuCompactSpritePipeline = spritePipeline;
    } else {
        FrontendSetCompactGpuCapture(false);
        _gpuCompactLibrary = nil;
        _gpuCompactPipeline = _gpuCompactSpritePipeline = nil;
        _compactSpritePass = nil;
        for (int j = 0; j < IN_FLIGHT; j++) _gpuCompactInputs[j] = nil;
    }
    _compactGpuCaptureEnabled = enabled;
    _gpuPpuGeneration++;
    _sequence = 0;
    _historyValid = NO;
    return YES;
}

- (void)drawInMTKView:(MTKView*)view {
    if (self.beforeDraw) {
        self.beforeDraw();
    }
    uint64_t drawStart = LatencyNowNs();
    // Two frames waiting for the screen is all the compositor can use: the first
    // is shown at the next refresh, the second behind it. A third is only a
    // queue the player's input waits in, and it never drains while frames keep
    // coming at the display's rate, so this refresh is left out and the frame
    // not drawn is replaced by a newer one. A display that stopped reporting
    // (window hidden, asleep) must not hold the counter up: after PRESENT_STALL_NS
    // without a report the frames count as lost.
    if (!self.testPattern && atomic_load(&_presentsPending) >= MAX_PENDING_PRESENTS) {
        if (drawStart - atomic_load(&_lastPresentedNs) < PRESENT_STALL_NS) {
            return;
        }
        atomic_store(&_presentsPending, 0);
    }
    // Take the drawable first: that is where a draw waits when the display is
    // behind, and the game frame is copied only afterwards, so that the newest
    // one is shown rather than one that went stale while waiting.
    MTLRenderPassDescriptor* pass = view.currentRenderPassDescriptor;
    if (pass == nil) {
        // Nothing is drawn, so the next frame that is starts the history afresh.
        _historyValid = NO;
        return;
    }
    dispatch_semaphore_wait(_inFlight, DISPATCH_TIME_FOREVER);
    BOOL measurePerformance = PerformanceEnabled();
    uint64_t performanceStart = measurePerformance ? PerformanceNow() : 0;

    // Upload into the next slot only when the game produced a new frame;
    // otherwise keep drawing the texture that already holds the newest one.
    NSUInteger next = (_slot + 1) % IN_FLIGHT;
    uint64_t previous = _sequence;
    bool cut = false, gpuAvailable = false, cpuAvailable = false;
    PpuCaptureLine* gpuLines = _gpuPpuEnabled && !_compactGpuCaptureEnabled && !self.testPattern ? _gpuPpuInputs[next].contents : NULL;
    PpuCompactCapture* compact = _gpuPpuEnabled && _compactGpuCaptureEnabled && !self.testPattern ? _gpuCompactInputs[next].contents : NULL;
    BOOL fresh = self.testPattern ? [self fillTestPattern] : FrontendCopyFrame(_pixels, &_sequence, &cut, gpuLines, compact, &gpuAvailable, &cpuAvailable);
    id<MTLCommandBuffer> commands = [_queue commandBuffer];
    if (fresh) {
        if (gpuAvailable) {
            id<MTLBuffer> input = _compactGpuCaptureEnabled ? _gpuCompactInputs[next] : _gpuPpuInputs[next];
            NSUInteger poolOffset = _compactGpuCaptureEnabled ? offsetof(PpuCompactCapture, pages) : 0;
            SpritePass* spritePass = _compactGpuCaptureEnabled ? _compactSpritePass : _spritePass;
            if (_gpuSpriteQuadsEnabled) [spritePass encode:commands lines:input poolOffset:poolOffset slot:next];
            id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
            id<MTLComputePipelineState> pipeline = _compactGpuCaptureEnabled
                ? (_gpuSpriteQuadsEnabled ? _gpuCompactSpritePipeline : _gpuCompactPipeline)
                : (_gpuSpriteQuadsEnabled ? _gpuSpritePipeline : _gpuPpuPipeline);
            [encoder setComputePipelineState:pipeline];
            [encoder setBuffer:input offset:0 atIndex:0];
            [encoder setBuffer:input offset:poolOffset atIndex:2];
            [encoder setTexture:_gpuPpuTextures[next] atIndex:0];
            if (_gpuSpriteQuadsEnabled) {
                [encoder setTexture:[spritePass colors:next] atIndex:1];
                [encoder setTexture:[spritePass windows:next] atIndex:2];
            }
            [encoder dispatchThreads:MTLSizeMake(PPU_WIDTH, PPU_HEIGHT, 1)
                 threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
            [encoder endEncoding];
            NSUInteger generation = _gpuPpuGeneration;
            __weak Renderer* weakRenderer = self;
            [commands addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
                if (buffer.status != MTLCommandBufferStatusError) return;
                NSString* message = buffer.error.localizedDescription ?: @"Metal could not render the frame.";
                dispatch_async(dispatch_get_main_queue(), ^{
                    Renderer* strongRenderer = weakRenderer;
                    if (!strongRenderer || !strongRenderer->_gpuPpuEnabled || strongRenderer->_gpuPpuGeneration != generation) return;
                    [strongRenderer enableGpuPpu:NO];
                    if (strongRenderer.gpuPpuFailed) strongRenderer.gpuPpuFailed(message);
                });
            }];
            if (getenv("TMC_GPU_PPU_SELFTEST") && !cpuAvailable && !_gpuOnlyTestChecked) {
                _gpuOnlyTestChecked = YES;
                [commands addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
                    NSLog(@"GPU PPU self-test: GPU-only frame %@", buffer.status == MTLCommandBufferStatusCompleted ? @"ok" : @"FAILED");
                }];
            }
            // One live-frame check for the automated app self-test. Compare the
            // texture consumed by presentation with the matching software frame.
            if (getenv("TMC_GPU_PPU_SELFTEST") && !_gpuSelfTestChecked) {
                _gpuSelfTestChecked = YES;
                id<MTLBuffer> readback = [_device newBufferWithLength:PPU_WIDTH * PPU_HEIGHT * 4
                                                               options:MTLResourceStorageModeShared];
                NSData* expected = [NSData dataWithBytes:_pixels length:sizeof(_pixels)];
                id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
                [blit copyFromTexture:_gpuPpuTextures[next] sourceSlice:0 sourceLevel:0
                         sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(PPU_WIDTH, PPU_HEIGHT, 1)
                             toBuffer:readback destinationOffset:0 destinationBytesPerRow:PPU_WIDTH * 4
              destinationBytesPerImage:PPU_WIDTH * PPU_HEIGHT * 4];
                [blit endEncoding];
                [commands addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
                    int bad = 0;
                    const uint8_t* actual = readback.contents;
                    const uint32_t* reference = expected.bytes;
                    if (buffer.status != MTLCommandBufferStatusCompleted || actual == NULL) {
                        NSLog(@"GPU PPU self-test: FAILED to read the GPU frame");
                        return;
                    }
                    for (int i = 0; i < PPU_WIDTH * PPU_HEIGHT; i++) {
                        uint32_t rgb = reference[i];
                        if (actual[i * 4] != ((rgb >> 16) & 255) || actual[i * 4 + 1] != ((rgb >> 8) & 255) ||
                            actual[i * 4 + 2] != (rgb & 255) || actual[i * 4 + 3] != 255) bad++;
                    }
                    NSLog(@"GPU PPU self-test: %@, %d differing pixels", bad ? @"FAILED" : @"ok", bad);
                }];
            }
        } else {
            if (getenv("TMC_GPU_PPU_SELFTEST") && !_gpuPpuEnabled && _gpuOnlyTestChecked && !_gpuFallbackTestChecked && cpuAvailable) {
                _gpuFallbackTestChecked = YES;
                NSLog(@"GPU PPU self-test: software frame restored");
            }
            [_textures[next] replaceRegion:MTLRegionMake2D(0, 0, PPU_WIDTH, PPU_HEIGHT)
                               mipmapLevel:0
                                 withBytes:_pixels
                               bytesPerRow:PPU_WIDTH * 4];
        }
        _lastFrameGpu = gpuAvailable;
        _slot = next;
        if (!_gpuPpuEnabled) {
            for (int i = 0; i < IN_FLIGHT; i++) _gpuPpuTextures[i] = nil;
        }
    }
    id<MTLTexture> texture = _lastFrameGpu ? _gpuPpuTextures[_slot] : _textures[_slot];
    if (LatencyEnabled()) {
        LatencyStageAt(_sequence, 0, drawStart);
    }

    // LCD response: one step per new game frame, never per display refresh (so
    // it looks the same at 60 and 120 Hz); paused, nothing new arrives and the
    // picture holds. When the display missed game frames (fast-forward), the
    // step covers all of them: the ghost lasts the same number of game frames.
    // The history restarts from the frame itself when it has none (first frame,
    // just switched on) and after a state load or reset.
    if (self.lcdResponse) {
        if (fresh || !_historyValid) {
            double frames = self.testPattern ? 1 : MIN(MAX(_sequence - previous, 1), 8);
            BOOL restart = !_historyValid || cut;
            ResponseUniforms response = {
                .keepRise = restart ? 0 : LcdResponseKeep(self.params.lcdResponseMs, frames),
                .keepFall = restart ? 0 : LcdResponseKeep(self.params.lcdResponseMs * self.params.lcdDarkSlowdown, frames),
            };
            EncodeResponse(commands, _responsePipeline, texture, _history[_historySlot], _history[_historySlot ^ 1], _shown,
                           response);
            _historySlot ^= 1;
            _historyValid = YES;
        }
        texture = _shown;
    } else {
        _historyValid = NO;
    }
    MTLViewport viewport = [self viewportForDrawableSize:view.drawableSize];
    Uniforms uniforms = {
        .sourceSize = { PPU_WIDTH, PPU_HEIGHT },
        .outputSize = { (float)viewport.width, (float)viewport.height },
        .filter = (uint32_t)self.filter,
        .params = self.params,
    };

    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:_pipeline];
    [encoder setViewport:viewport];
    [encoder setFragmentTexture:texture atIndex:0];
    [encoder setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    if (!CGRectIsEmpty(self.highlightRect)) {
        CGRect r = self.highlightRect;
        OverlayUniforms overlay = {
            .rect = { r.origin.x, r.origin.y, r.size.width, r.size.height },
            .sourceSize = uniforms.sourceSize,
            .outputSize = uniforms.outputSize,
            .colour = { 1.0f, 0.1f, 0.9f, 1.0f },
        };
        [encoder setRenderPipelineState:_overlayPipeline];
        [encoder setVertexBytes:&overlay length:sizeof(overlay) atIndex:0];
        [encoder setFragmentBytes:&overlay length:sizeof(overlay) atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    }
    [encoder endEncoding];

    // Debugging: copy the chosen buffer back every few refreshes, one copy in
    // flight at a time. Nothing here runs while debugBuffer is DebugBufferNone.
    DebugBuffer debug = self.debugBuffer;
    if (debug != DebugBufferNone && _drawCount++ % 4 == 0) {
        [self encodeReadback:debug
                    commands:commands
                        view:view
                       input:texture
                    viewport:viewport
                    uniforms:uniforms];
    }

    dispatch_semaphore_t inFlight = _inFlight;
    id<MTLTexture> captureTexture = _captureURL ? view.currentDrawable.texture : nil;
    NSURL* captureURL = _captureURL;
    _captureURL = nil;
    uint64_t traced = _sequence;
    if (LatencyEnabled()) {
        [commands addScheduledHandler:^(id<MTLCommandBuffer> buffer) {
            LatencyStage(traced, 2);
        }];
    }
    [commands addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
        if (measurePerformance && buffer.GPUEndTime > buffer.GPUStartTime && buffer.GPUStartTime > 0)
            PerformancePresentation(-1, (buffer.GPUEndTime - buffer.GPUStartTime) * 1000.0);
        LatencyStage(traced, 3);
        if (captureTexture != nil) {
            WriteTexturePNG(captureTexture, captureURL);
        }
        dispatch_semaphore_signal(inFlight);
    }];
    id<CAMetalDrawable> drawable = view.currentDrawable;
    uint64_t presentedSequence = _sequence;
    atomic_fetch_add(&_presentsPending, 1);
    [drawable addPresentedHandler:^(id<MTLDrawable> shown) {
        atomic_store(&self->_lastPresentedNs, LatencyNowNs());
        int pending = atomic_load(&self->_presentsPending);
        while (pending > 0 && !atomic_compare_exchange_weak(&self->_presentsPending, &pending, pending - 1)) {
        }
        // When the frame went onto the screen: display sync paces the game by it.
        DisplaySyncNotePresented(shown.presentedTime);
        LatencyPresented(presentedSequence, shown.presentedTime);
        if (measurePerformance) PerformancePresented(shown.presentedTime);
    }];
    [commands presentDrawable:drawable];
    [commands commit];
    if (measurePerformance) PerformancePresentation((PerformanceNow() - performanceStart) / 1e6, -1);
    LatencyStage(traced, 1);
}

- (nullable DebugReadback*)latestReadback {
    os_unfair_lock_lock(&_readbackLock);
    DebugReadback* latest = _latestReadback;
    _latestTaken = YES;
    os_unfair_lock_unlock(&_readbackLock);
    return latest;
}

- (void)setDebugBuffer:(DebugBuffer)debugBuffer {
    _debugBuffer = debugBuffer;
    if (debugBuffer == DebugBufferNone) {
        // Let go of the big textures while nobody looks.
        _stageTexture = nil;
        os_unfair_lock_lock(&_readbackLock);
        _latestReadback = nil;
        if (!_readbackBusy) {
            _readbackTexture = nil;
        }
        os_unfair_lock_unlock(&_readbackLock);
    }
}

// A private texture of this size and format, kept if the one we have fits.
- (id<MTLTexture>)texture:(id<MTLTexture>)current
                   format:(MTLPixelFormat)format
                    width:(NSUInteger)width
                   height:(NSUInteger)height
                    usage:(MTLTextureUsage)usage
                  storage:(MTLStorageMode)storage {
    if (current != nil && current.pixelFormat == format && current.width == width && current.height == height) {
        return current;
    }
    MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                    width:width
                                                                                   height:height
                                                                                mipmapped:NO];
    desc.usage = usage;
    desc.storageMode = storage;
    return [_device newTextureWithDescriptor:desc];
}

- (void)encodeReadback:(DebugBuffer)debug
              commands:(id<MTLCommandBuffer>)commands
                  view:(MTKView*)view
                 input:(id<MTLTexture>)input
              viewport:(MTLViewport)viewport
              uniforms:(Uniforms)uniforms {
    // One copy in flight, and a new one only once the viewer took the last
    // (so copies follow the viewer's refresh rate, not the display's).
    os_unfair_lock_lock(&_readbackLock);
    BOOL skip = _readbackBusy || (_latestReadback != nil && !_latestTaken);
    if (!skip) {
        _readbackBusy = YES;
    }
    os_unfair_lock_unlock(&_readbackLock);
    if (skip) {
        return;
    }

    id<MTLTexture> source = nil;
    NSString* format = @"BGRA8Unorm, gamma-encoded";
    NSString* note = @"";
    NSUInteger x = 0, y = 0;
    switch (debug) {
        case DebugBufferFrame:
            source = _lastFrameGpu ? _gpuPpuTextures[_slot] : _textures[_slot];
            break;
        case DebugBufferResponseHistory:
            source = _history[_historySlot];
            format = @"RGBA16Float, linear light";
            note = self.lcdResponse ? @"" : @"LCD response is off: this buffer is not updated";
            break;
        case DebugBufferResponseOutput:
            source = _shown;
            format = @"RGBA16Float, gamma-encoded";
            note = self.lcdResponse ? @"" : @"LCD response is off: this buffer is not updated";
            break;
        case DebugBufferFilterInput:
            source = input;
            format = input.pixelFormat == MTLPixelFormatRGBA16Float ? @"RGBA16Float, gamma-encoded" : format;
            note = input == _shown ? @"= LCD response output" : @"= game frame";
            break;
        case DebugBufferFilterRaw:
        case DebugBufferBeams:
        case DebugBufferMask:
        case DebugBufferBloom:
        case DebugBufferPreGamma: {
            // The filter pass again, into a float texture, with the stage selected.
            NSUInteger width = MAX((NSUInteger)viewport.width, 1), height = MAX((NSUInteger)viewport.height, 1);
            _stageTexture = [self texture:_stageTexture
                                   format:MTLPixelFormatRGBA16Float
                                    width:width
                                   height:height
                                    usage:MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead
                                  storage:MTLStorageModePrivate];
            MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = _stageTexture;
            pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            uniforms.debugStage = DebugStage(debug);
            uniforms.positionOffset = (simd_float2){ (float)viewport.originX, (float)viewport.originY };
            id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
            [encoder setRenderPipelineState:_stagePipeline];
            [encoder setViewport:(MTLViewport){ 0, 0, width, height, 0, 1 }];
            [encoder setFragmentTexture:input atIndex:0];
            [encoder setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            [encoder endEncoding];
            source = _stageTexture;
            format = debug == DebugBufferPreGamma ? @"RGBA16Float, before gamma" : @"RGBA16Float, filter values";
            note = [NSString stringWithFormat:@"%@ filter, viewport %lux%lu", FilterModeName(self.filter),
                                              (unsigned long)width, (unsigned long)height];
            break;
        }
        case DebugBufferDrawable: {
            id<MTLTexture> drawable = view.currentDrawable.texture;
            if (drawable.framebufferOnly) {
                view.framebufferOnly = NO; // readable from the next drawable on
                note = @"waiting for a readable drawable";
            } else {
                source = drawable;
                x = (NSUInteger)viewport.originX;
                y = (NSUInteger)viewport.originY;
                note = [NSString stringWithFormat:@"drawable %lux%lu, viewport at %lu,%lu", (unsigned long)drawable.width,
                                                  (unsigned long)drawable.height, (unsigned long)x, (unsigned long)y];
            }
            break;
        }
        default:
            break;
    }
    if (source == nil || (source.pixelFormat != MTLPixelFormatBGRA8Unorm &&
                          source.pixelFormat != MTLPixelFormatRGBA8Unorm && source.pixelFormat != MTLPixelFormatRGBA16Float)) {
        os_unfair_lock_lock(&_readbackLock);
        _readbackBusy = NO;
        os_unfair_lock_unlock(&_readbackLock);
        return;
    }
    NSUInteger width = source.width - MIN(x, source.width), height = source.height - MIN(y, source.height);
    if (debug == DebugBufferDrawable) {
        width = MIN(width, (NSUInteger)viewport.width);
        height = MIN(height, (NSUInteger)viewport.height);
    }
    BOOL isFloat = source.pixelFormat == MTLPixelFormatRGBA16Float;
    BOOL isRGBA = source.pixelFormat == MTLPixelFormatRGBA8Unorm;
    if (isRGBA) format = @"RGBA8Unorm, gamma-encoded";
    id<MTLTexture> readback = [self texture:_readbackTexture
                                     format:source.pixelFormat
                                      width:width
                                     height:height
                                      usage:MTLTextureUsageShaderRead
                                    storage:_cpuStorage];
    _readbackTexture = readback;
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    [blit copyFromTexture:source
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(x, y, 0)
               sourceSize:MTLSizeMake(width, height, 1)
                toTexture:readback
         destinationSlice:0
         destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
#if !TARGET_OS_IOS
    if (readback.storageMode == MTLStorageModeManaged) {
        [blit synchronizeResource:readback];
    }
#endif
    [blit endEncoding];

    uint64_t serial = ++_readbackSerial;
    __weak Renderer* weakSelf = self;
    [commands addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
        Renderer* strongSelf = weakSelf;
        if (strongSelf == nil) {
            return;
        }
        NSUInteger bytesPerPixel = isFloat ? 8 : 4;
        NSMutableData* bytes = [NSMutableData dataWithLength:width * height * bytesPerPixel];
        [readback getBytes:bytes.mutableBytes
               bytesPerRow:width * bytesPerPixel
                fromRegion:MTLRegionMake2D(0, 0, width, height)
               mipmapLevel:0];
        if (isRGBA) {
            uint8_t* pixels = bytes.mutableBytes;
            for (NSUInteger i = 0; i < width * height; i++) {
                uint8_t red = pixels[i * 4];
                pixels[i * 4] = pixels[i * 4 + 2];
                pixels[i * 4 + 2] = red;
            }
        }
        DebugReadback* result = [[DebugReadback alloc] initWithBuffer:debug
                                                                width:width
                                                               height:height
                                                              isFloat:isFloat
                                                                bytes:bytes
                                                               format:format
                                                                 note:note
                                                               serial:serial];
        os_unfair_lock_lock(&strongSelf->_readbackLock);
        strongSelf->_readbackBusy = NO;
        if (strongSelf->_debugBuffer == debug) {
            strongSelf->_latestReadback = result;
            strongSelf->_latestTaken = NO;
        }
        os_unfair_lock_unlock(&strongSelf->_readbackLock);
    }];
}

- (BOOL)checkLcdResponse {
    MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                    width:1
                                                                                   height:1
                                                                                mipmapped:NO];
    desc.storageMode = _cpuStorage;
    id<MTLTexture> frame = [_device newTextureWithDescriptor:desc];
    desc.pixelFormat = MTLPixelFormatRGBA16Float;
    desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    NSArray<id<MTLTexture>>* history = @[ [_device newTextureWithDescriptor:desc], [_device newTextureWithDescriptor:desc] ];
    id<MTLTexture> shown = [_device newTextureWithDescriptor:desc];
    FilterParams params = self.params;
    float riseMs = params.lcdResponseMs, fallMs = params.lcdResponseMs * params.lcdDarkSlowdown;
    double frameMs = 1000.0 / FRONTEND_FRAME_RATE;
    __block NSUInteger slot = 0;

    // Show `pixel` for one game frame (restart = copy it in); returns the
    // history's red channel (linear light).
    float (^step)(uint32_t, BOOL) = ^(uint32_t pixel, BOOL restart) {
        [frame replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&pixel bytesPerRow:4];
        ResponseUniforms response = {
            .keepRise = restart ? 0 : LcdResponseKeep(riseMs, 1),
            .keepFall = restart ? 0 : LcdResponseKeep(fallMs, 1),
        };
        id<MTLCommandBuffer> commands = [self->_queue commandBuffer];
        EncodeResponse(commands, self->_responsePipeline, frame, history[slot], history[slot ^ 1], shown, response);
#if !TARGET_OS_IOS
        if (history[slot ^ 1].storageMode == MTLStorageModeManaged) {
            id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
            [blit synchronizeResource:history[slot ^ 1]];
            [blit endEncoding];
        }
#endif
        [commands commit];
        [commands waitUntilCompleted];
        slot ^= 1;
        __fp16 value[4];
        [history[slot] getBytes:value bytesPerRow:8 fromRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0];
        return (float)value[0];
    };

    // A step from `from` to `to`; the 10-90% time comes from where the
    // remaining distance crosses 90% and 10%, interpolated on a log scale
    // between frames (exact for an exponential response).
    BOOL (^measure)(NSString*, uint32_t, uint32_t, float) = ^(NSString* name, uint32_t from, uint32_t to, float expectMs) {
        float start = step(from, YES), end = (float)pow(((to >> 16) & 0xFF) / 255.0, 2.2);
        NSMutableString* values = [NSMutableString string];
        double previous = 1, cross90 = -1, cross10 = -1;
        for (int n = 1; n <= 8; n++) {
            float value = step(to, NO);
            [values appendFormat:@" %.3f", value];
            double remaining = fmax((end - value) / (end - start), 1e-6);
            if (cross90 < 0 && remaining <= 0.9) {
                cross90 = (n - 1 + log(0.9 / previous) / log(remaining / previous)) * frameMs;
            }
            if (cross10 < 0 && remaining <= 0.1) {
                cross10 = (n - 1 + log(0.1 / previous) / log(remaining / previous)) * frameMs;
            }
            previous = remaining;
        }
        double measured = cross10 - cross90;
        BOOL ok = cross90 >= 0 && cross10 >= 0 && fabs(measured - expectMs) < 0.5;
        SelfTestLog(@"selftest: LCD response %@ (linear, per frame):%@; 10-90%% %.1f ms (setting %.1f ms) %@", name, values,
              measured, expectMs, ok ? @"ok" : @"FAILED");
        return ok;
    };
    BOOL rise = measure(@"black to white", 0xFF000000, 0xFFFFFFFF, riseMs);
    BOOL fall = measure(@"white to black", 0xFFFFFFFF, 0xFF000000, fallMs);
    return rise && fall;
}

@end
