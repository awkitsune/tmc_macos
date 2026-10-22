#import "renderer.h"

#include <os/lock.h>
#include <zlib.h>

#include "frontend.h"

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
            return @"Game frame (uploaded)";
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
// GPU may still be reading.
#define IN_FLIGHT 3

@implementation Renderer {
    id<MTLDevice> _device;
    id<MTLCommandQueue> _queue;
    id<MTLRenderPipelineState> _pipeline;
    id<MTLTexture> _textures[IN_FLIGHT];
    // LCD response: what the simulated LCD shows, in linear light (ping-pong,
    // _history[_historySlot] is current), and the same gamma-encoded for the
    // present pass. One queue with tracked resources, so a pass never writes
    // these while an earlier one still reads them.
    id<MTLComputePipelineState> _responsePipeline;
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
}

- (nullable instancetype)initWithView:(MTKView*)view {
    if ((self = [super init]) == nil) {
        return nil;
    }
    _device = view.device;
    _queue = [_device newCommandQueue];
    _inFlight = dispatch_semaphore_create(IN_FLIGHT);

    NSError* error = nil;
    NSString* path = [NSBundle.mainBundle pathForResource:@"shaders" ofType:@"metal"];
    NSString* source = path ? [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:&error] : nil;
    if (source == nil) {
        NSLog(@"shaders.metal missing from the app bundle: %@", error);
        return nil;
    }
    id<MTLLibrary> library = [_device newLibraryWithSource:source options:nil error:&error];
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

    id<MTLFunction> response = [library newFunctionWithName:@"lcd_response"];
    _responsePipeline = response ? [_device newComputePipelineStateWithFunction:response error:&error] : nil;
    if (_responsePipeline == nil) {
        NSLog(@"LCD response pipeline creation failed: %@", error);
        return nil;
    }

    MTLTextureDescriptor* texDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                       width:PPU_WIDTH
                                                                                      height:PPU_HEIGHT
                                                                                   mipmapped:NO];
    texDesc.usage = MTLTextureUsageShaderRead;
    texDesc.storageMode = MTLStorageModeShared;
    for (int i = 0; i < IN_FLIGHT; i++) {
        _textures[i] = [_device newTextureWithDescriptor:texDesc];
    }
    MTLTextureDescriptor* floatDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                                         width:PPU_WIDTH
                                                                                        height:PPU_HEIGHT
                                                                                     mipmapped:NO];
    floatDesc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    floatDesc.storageMode = MTLStorageModePrivate;
    _history[0] = [_device newTextureWithDescriptor:floatDesc];
    _history[1] = [_device newTextureWithDescriptor:floatDesc];
    _shown = [_device newTextureWithDescriptor:floatDesc];
    _integerScaling = YES;
    _params = FilterParamsDefaults();
    return self;
}

// The largest rect with the GBA's 3:2 shape that fits, centred, in whole pixels.
- (MTLViewport)viewportForDrawableSize:(CGSize)size {
    double scale = MIN(size.width / PPU_WIDTH, size.height / PPU_HEIGHT);
    if (self.integerScaling && scale >= 1.0) {
        scale = floor(scale);
    }
    double width = round(PPU_WIDTH * scale), height = round(PPU_HEIGHT * scale);
    return (MTLViewport){
        .originX = floor((size.width - width) / 2),
        .originY = floor((size.height - height) / 2),
        .width = width,
        .height = height,
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
static void EncodeResponse(id<MTLCommandBuffer> commands, id<MTLComputePipelineState> pipeline, id<MTLTexture> frame,
                           id<MTLTexture> history, id<MTLTexture> nextHistory, id<MTLTexture> shown,
                           ResponseUniforms uniforms) {
    id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
    [encoder setComputePipelineState:pipeline];
    [encoder setTexture:frame atIndex:0];
    [encoder setTexture:history atIndex:1];
    [encoder setTexture:nextHistory atIndex:2];
    [encoder setTexture:shown atIndex:3];
    [encoder setBytes:&uniforms length:sizeof(uniforms) atIndex:0];
    MTLSize group = MTLSizeMake(16, 16, 1);
    MTLSize groups = MTLSizeMake((frame.width + 15) / 16, (frame.height + 15) / 16, 1);
    [encoder dispatchThreadgroups:groups threadsPerThreadgroup:group];
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
    NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithCGImage:image];
    [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToURL:url atomically:YES];
    CGImageRelease(image);
    CGContextRelease(context);
    CGColorSpaceRelease(space);
}

- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size {
}

- (void)drawInMTKView:(MTKView*)view {
    if (self.beforeDraw) {
        self.beforeDraw();
    }
    dispatch_semaphore_wait(_inFlight, DISPATCH_TIME_FOREVER);

    // Upload into the next slot only when the game produced a new frame;
    // otherwise keep drawing the texture that already holds the newest one.
    NSUInteger next = (_slot + 1) % IN_FLIGHT;
    uint64_t previous = _sequence;
    bool cut = false;
    BOOL fresh = self.testPattern ? [self fillTestPattern] : FrontendCopyFrame(_pixels, &_sequence, &cut);
    if (fresh) {
        [_textures[next] replaceRegion:MTLRegionMake2D(0, 0, PPU_WIDTH, PPU_HEIGHT)
                           mipmapLevel:0
                             withBytes:_pixels
                           bytesPerRow:PPU_WIDTH * 4];
        _slot = next;
    }
    id<MTLTexture> texture = _textures[_slot];

    MTLRenderPassDescriptor* pass = view.currentRenderPassDescriptor;
    if (pass == nil) {
        // Nothing is drawn, so the next frame that is starts the history afresh.
        _historyValid = NO;
        dispatch_semaphore_signal(_inFlight);
        return;
    }
    id<MTLCommandBuffer> commands = [_queue commandBuffer];

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
    [commands addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
        if (captureTexture != nil) {
            WriteTexturePNG(captureTexture, captureURL);
        }
        dispatch_semaphore_signal(inFlight);
    }];
    [commands presentDrawable:view.currentDrawable];
    [commands commit];
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
            source = _textures[_slot];
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
    if (source == nil || (source.pixelFormat != MTLPixelFormatBGRA8Unorm && source.pixelFormat != MTLPixelFormatRGBA16Float)) {
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
    id<MTLTexture> readback = [self texture:_readbackTexture
                                     format:source.pixelFormat
                                      width:width
                                     height:height
                                      usage:MTLTextureUsageShaderRead
                                    storage:MTLStorageModeShared];
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
    desc.storageMode = MTLStorageModeShared;
    id<MTLTexture> frame = [_device newTextureWithDescriptor:desc];
    desc.pixelFormat = MTLPixelFormatRGBA16Float;
    desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
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
        NSLog(@"selftest: LCD response %@ (linear, per frame):%@; 10-90%% %.1f ms (setting %.1f ms) %@", name, values,
              measured, expectMs, ok ? @"ok" : @"FAILED");
        return ok;
    };
    BOOL rise = measure(@"black to white", 0xFF000000, 0xFFFFFFFF, riseMs);
    BOOL fall = measure(@"white to black", 0xFFFFFFFF, 0xFF000000, fallMs);
    return rise && fall;
}

@end
