#import "gpuppu.h"

@implementation GpuPpu {
    NSUInteger _slots;
    id<MTLRenderPipelineState> _spritePipeline, _windowPipeline;
    id<MTLDepthStencilState> _depth;
    id<MTLComputePipelineState> _backgroundPipeline, _composePipeline;
    NSMutableArray<id<MTLBuffer>>* _inputs;
    NSMutableArray<id<MTLTexture>> *_sprites, *_windows, *_depths, *_layers, *_outputs;
}

static id<MTLTexture> NewTexture(id<MTLDevice> device, MTLPixelFormat format, MTLTextureUsage usage, NSUInteger layers) {
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                 width:PPU_WIDTH
                                                                                height:PPU_HEIGHT
                                                                             mipmapped:NO];
    if (layers > 1) {
        d.textureType = MTLTextureType2DArray;
        d.arrayLength = layers;
    }
    d.storageMode = MTLStorageModePrivate;
    d.usage = usage;
    return [device newTextureWithDescriptor:d];
}

- (instancetype)initWithDevice:(id<MTLDevice>)device source:(NSString*)source slots:(NSUInteger)slots error:(NSError**)error {
    if (!(self = [super init])) return nil;
    MTLCompileOptions* options = [MTLCompileOptions new];
    options.languageVersion = MTLLanguageVersion2_4;
    id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:error];
    if (!library) return nil;

    MTLRenderPipelineDescriptor* pipeline = [MTLRenderPipelineDescriptor new];
    pipeline.vertexFunction = [library newFunctionWithName:@"sprite_vertex"];
    pipeline.fragmentFunction = [library newFunctionWithName:@"sprite_fragment"];
    pipeline.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    pipeline.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    _spritePipeline = [device newRenderPipelineStateWithDescriptor:pipeline error:error];
    pipeline.fragmentFunction = [library newFunctionWithName:@"sprite_window_fragment"];
    pipeline.colorAttachments[0].pixelFormat = MTLPixelFormatR8Unorm;
    pipeline.depthAttachmentPixelFormat = MTLPixelFormatInvalid;
    _windowPipeline = [device newRenderPipelineStateWithDescriptor:pipeline error:error];
    MTLDepthStencilDescriptor* depth = [MTLDepthStencilDescriptor new];
    depth.depthCompareFunction = MTLCompareFunctionLess;
    depth.depthWriteEnabled = YES;
    _depth = [device newDepthStencilStateWithDescriptor:depth];
    id<MTLFunction> backgrounds = [library newFunctionWithName:@"gpu_background_layers"];
    id<MTLFunction> compose = [library newFunctionWithName:@"gpu_compose"];
    _backgroundPipeline = backgrounds ? [device newComputePipelineStateWithFunction:backgrounds error:error] : nil;
    _composePipeline = compose ? [device newComputePipelineStateWithFunction:compose error:error] : nil;
    if (!_spritePipeline || !_windowPipeline || !_depth || !_backgroundPipeline || !_composePipeline) return nil;

    _slots = slots;
    _inputs = [NSMutableArray new];
    _sprites = [NSMutableArray new];
    _windows = [NSMutableArray new];
    _depths = [NSMutableArray new];
    _layers = [NSMutableArray new];
    _outputs = [NSMutableArray new];
    MTLTextureUsage target = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    MTLTextureUsage compute = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    for (NSUInteger i = 0; i < slots; i++) {
        id<MTLBuffer> input = [device newBufferWithLength:sizeof(PpuCompactCapture) options:MTLResourceStorageModeShared];
        id<MTLTexture> sprites = NewTexture(device, MTLPixelFormatRGBA8Unorm, target, 1);
        id<MTLTexture> windows = NewTexture(device, MTLPixelFormatR8Unorm, target, 1);
        id<MTLTexture> depths = NewTexture(device, MTLPixelFormatDepth32Float, MTLTextureUsageRenderTarget, 1);
        id<MTLTexture> layers = NewTexture(device, MTLPixelFormatR16Uint, compute, 4);
        id<MTLTexture> output = NewTexture(device, MTLPixelFormatRGBA8Unorm, compute, 1);
        if (!input || !sprites || !windows || !depths || !layers || !output) return nil;
        [_inputs addObject:input];
        [_sprites addObject:sprites];
        [_windows addObject:windows];
        [_depths addObject:depths];
        [_layers addObject:layers];
        [_outputs addObject:output];
    }
    return self;
}

- (PpuCompactCapture*)input:(NSUInteger)slot {
    return _inputs[slot].contents;
}

- (id<MTLTexture>)output:(NSUInteger)slot {
    return _outputs[slot];
}

- (void)encode:(id<MTLCommandBuffer>)commands slot:(NSUInteger)slot {
    id<MTLBuffer> lines = _inputs[slot];
    NSUInteger pool = offsetof(PpuCompactCapture, pages);

    // Sprites: the winning color by priority and OAM index, then the OBJ window.
    for (uint32_t window = 0; window < 2; window++) {
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = window ? _windows[slot] : _sprites[slot];
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        if (!window) {
            pass.depthAttachment.texture = _depths[slot];
            pass.depthAttachment.loadAction = MTLLoadActionClear;
            pass.depthAttachment.storeAction = MTLStoreActionDontCare;
            pass.depthAttachment.clearDepth = 1;
        }
        id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
        [encoder setRenderPipelineState:window ? _windowPipeline : _spritePipeline];
        if (!window) [encoder setDepthStencilState:_depth];
        [encoder setVertexBuffer:lines offset:0 atIndex:0];
        [encoder setVertexBytes:&window length:sizeof(window) atIndex:1];
        [encoder setFragmentBuffer:lines offset:0 atIndex:0];
        [encoder setFragmentBuffer:lines offset:pool atIndex:2];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4 instanceCount:PPU_HEIGHT * 128];
        [encoder endEncoding];
    }

    id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
    [encoder setComputePipelineState:_backgroundPipeline];
    [encoder setBuffer:lines offset:0 atIndex:0];
    [encoder setBuffer:lines offset:pool atIndex:2];
    [encoder setTexture:_layers[slot] atIndex:0];
    [encoder dispatchThreads:MTLSizeMake(PPU_WIDTH, PPU_HEIGHT, 4) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
    // The compute encoder orders the composition after the layers it reads.
    [encoder setComputePipelineState:_composePipeline];
    [encoder setTexture:_outputs[slot] atIndex:0];
    [encoder setTexture:_sprites[slot] atIndex:1];
    [encoder setTexture:_windows[slot] atIndex:2];
    [encoder setTexture:_layers[slot] atIndex:3];
    [encoder dispatchThreads:MTLSizeMake(PPU_WIDTH, PPU_HEIGHT, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
    [encoder endEncoding];
}

@end
