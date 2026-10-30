#import <Metal/Metal.h>

// Experimental OBJ rasterization; slot belongs to one frame in flight.
@interface SpritePass : NSObject
- (instancetype)initWithDevice:(id<MTLDevice>)device library:(id<MTLLibrary>)library error:(NSError**)error;
- (void)encode:(id<MTLCommandBuffer>)commands lines:(id<MTLBuffer>)lines slot:(NSUInteger)slot;
- (id<MTLTexture>)colors:(NSUInteger)slot;
- (id<MTLTexture>)windows:(NSUInteger)slot;
@end
