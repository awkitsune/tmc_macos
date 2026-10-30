#import "backgroundpass.h"
#include "port/ppu.h"

@implementation BackgroundPass {
    id<MTLComputePipelineState> _pipeline;
    id<MTLTexture> _layers[2];
}
- (instancetype)initWithDevice:(id<MTLDevice>)device library:(id<MTLLibrary>)library error:(NSError**)error {
    if (!(self = [super init])) return nil;
    id<MTLFunction> function = [library newFunctionWithName:@"gpu_background_layers"];
    _pipeline = function ? [device newComputePipelineStateWithFunction:function error:error] : nil;
    if (!_pipeline) return nil;
    MTLTextureDescriptor* d = [MTLTextureDescriptor new];
    d.textureType = MTLTextureType2DArray;
    d.pixelFormat = MTLPixelFormatR16Uint;
    d.width = PPU_WIDTH;
    d.height = PPU_HEIGHT;
    d.arrayLength = 4;
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    for (int i = 0; i < 2; i++) {
        _layers[i] = [device newTextureWithDescriptor:d];
        if (!_layers[i]) return nil;
    }
    return self;
}
- (void)encode:(id<MTLCommandBuffer>)commands lines:(id<MTLBuffer>)lines poolOffset:(NSUInteger)offset slot:(NSUInteger)slot {
    id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
    [encoder setComputePipelineState:_pipeline];
    [encoder setBuffer:lines offset:0 atIndex:0];
    [encoder setBuffer:lines offset:offset atIndex:2];
    [encoder setTexture:_layers[slot] atIndex:0];
    [encoder dispatchThreads:MTLSizeMake(PPU_WIDTH, PPU_HEIGHT, 4) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
    [encoder endEncoding];
}
- (id<MTLTexture>)layers:(NSUInteger)slot { return _layers[slot]; }
@end
