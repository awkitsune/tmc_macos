#import "renderer.h"

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
    };
}

// Keep in sync with Uniforms in shaders.metal.
typedef struct {
    simd_float2 sourceSize;
    simd_float2 outputSize;
    uint32_t filter;
    FilterParams params;
} Uniforms;

// Frames in flight: each has its own texture so an upload never touches one the
// GPU may still be reading.
#define IN_FLIGHT 3

@implementation Renderer {
    id<MTLDevice> _device;
    id<MTLCommandQueue> _queue;
    id<MTLRenderPipelineState> _pipeline;
    id<MTLTexture> _textures[IN_FLIGHT];
    dispatch_semaphore_t _inFlight;
    NSUInteger _slot;
    uint64_t _sequence;
    uint32_t _pixels[PPU_WIDTH * PPU_HEIGHT];
    NSURL* _captureURL;
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

    MTLTextureDescriptor* texDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                       width:PPU_WIDTH
                                                                                      height:PPU_HEIGHT
                                                                                   mipmapped:NO];
    texDesc.usage = MTLTextureUsageShaderRead;
    texDesc.storageMode = MTLStorageModeShared;
    for (int i = 0; i < IN_FLIGHT; i++) {
        _textures[i] = [_device newTextureWithDescriptor:texDesc];
    }
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
    if (self.testPattern ? [self fillTestPattern] : FrontendCopyFrame(_pixels, &_sequence)) {
        [_textures[next] replaceRegion:MTLRegionMake2D(0, 0, PPU_WIDTH, PPU_HEIGHT)
                           mipmapLevel:0
                             withBytes:_pixels
                           bytesPerRow:PPU_WIDTH * 4];
        _slot = next;
    }
    id<MTLTexture> texture = _textures[_slot];

    id<MTLCommandBuffer> commands = [_queue commandBuffer];
    MTLRenderPassDescriptor* pass = view.currentRenderPassDescriptor;
    if (pass == nil) {
        dispatch_semaphore_signal(_inFlight);
        return;
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
    [encoder endEncoding];

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

@end
