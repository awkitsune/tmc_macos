#import "spritepass.h"
#include "port/ppu.h"

@implementation SpritePass {
    id<MTLRenderPipelineState> _colorPipeline, _windowPipeline;
    id<MTLDepthStencilState> _depth;
    id<MTLTexture> _colors[2], _windows[2], _depths[2];
}
- (instancetype)initWithDevice:(id<MTLDevice>)device library:(id<MTLLibrary>)library error:(NSError**)error {
    if (!(self = [super init])) return nil;
    MTLRenderPipelineDescriptor* pipeline = [MTLRenderPipelineDescriptor new];
    pipeline.vertexFunction = [library newFunctionWithName:@"sprite_vertex"];
    pipeline.fragmentFunction = [library newFunctionWithName:@"sprite_fragment"];
    pipeline.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    pipeline.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    _colorPipeline = [device newRenderPipelineStateWithDescriptor:pipeline error:error];
    pipeline.fragmentFunction = [library newFunctionWithName:@"sprite_window_fragment"];
    pipeline.colorAttachments[0].pixelFormat = MTLPixelFormatR8Unorm;
    pipeline.depthAttachmentPixelFormat = MTLPixelFormatInvalid;
    _windowPipeline = [device newRenderPipelineStateWithDescriptor:pipeline error:error];
    MTLDepthStencilDescriptor* ds = [MTLDepthStencilDescriptor new];
    ds.depthCompareFunction = MTLCompareFunctionLess;
    ds.depthWriteEnabled = YES;
    _depth = [device newDepthStencilStateWithDescriptor:ds];
    if (!_colorPipeline || !_windowPipeline || !_depth) return nil;
    for (int i = 0; i < 2; i++) {
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                     width:PPU_WIDTH height:PPU_HEIGHT mipmapped:NO];
        d.storageMode = MTLStorageModePrivate;
        d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        _colors[i] = [device newTextureWithDescriptor:d];
        d.pixelFormat = MTLPixelFormatR8Unorm;
        _windows[i] = [device newTextureWithDescriptor:d];
        d.pixelFormat = MTLPixelFormatDepth32Float;
        d.usage = MTLTextureUsageRenderTarget;
        _depths[i] = [device newTextureWithDescriptor:d];
        if (!_colors[i] || !_windows[i] || !_depths[i]) return nil;
    }
    return self;
}
- (void)encode:(id<MTLCommandBuffer>)commands lines:(id<MTLBuffer>)lines poolOffset:(NSUInteger)poolOffset slot:(NSUInteger)slot {
    for (uint32_t window = 0; window < 2; window++) {
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = window ? _windows[slot] : _colors[slot];
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
        [encoder setRenderPipelineState:window ? _windowPipeline : _colorPipeline];
        if (!window) [encoder setDepthStencilState:_depth];
        [encoder setVertexBuffer:lines offset:0 atIndex:0];
        [encoder setVertexBytes:&window length:sizeof(window) atIndex:1];
        [encoder setFragmentBuffer:lines offset:0 atIndex:0];
        [encoder setFragmentBuffer:lines offset:poolOffset atIndex:2];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4 instanceCount:PPU_HEIGHT * 128];
        [encoder endEncoding];
    }
}
- (id<MTLTexture>)colors:(NSUInteger)slot { return _colors[slot]; }
- (id<MTLTexture>)windows:(NSUInteger)slot { return _windows[slot]; }
@end
