#import <Metal/Metal.h>

// Experimental per-background textures; slot belongs to one frame in flight.
@interface BackgroundPass : NSObject
- (instancetype)initWithDevice:(id<MTLDevice>)device library:(id<MTLLibrary>)library error:(NSError**)error;
- (void)encode:(id<MTLCommandBuffer>)commands lines:(id<MTLBuffer>)lines poolOffset:(NSUInteger)offset slot:(NSUInteger)slot;
- (id<MTLTexture>)layers:(NSUInteger)slot;
@end
