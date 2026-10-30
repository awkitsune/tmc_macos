#import <Metal/Metal.h>
#include "port/ppucapture.h"

NS_ASSUME_NONNULL_BEGIN

/*
 * The Metal PPU (gpuppu.metal): draws a frame from a compact scanline capture.
 * Sprites are drawn as one-line quads, each background into its own layer, and
 * a compute pass composes them with windows and effects. Each slot is one frame
 * in flight with its own input buffer and textures.
 */
@interface GpuPpu : NSObject
- (nullable instancetype)initWithDevice:(id<MTLDevice>)device
                                 source:(NSString*)source
                                  slots:(NSUInteger)slots
                                  error:(NSError**)error;
// Where the CPU writes the slot's capture; the GPU must be done with the slot.
- (PpuCompactCapture*)input:(NSUInteger)slot;
- (void)encode:(id<MTLCommandBuffer>)commands slot:(NSUInteger)slot;
// The slot's picture (RGBA8, 240x160), valid once the encoded work completes.
- (id<MTLTexture>)output:(NSUInteger)slot;
@end

NS_ASSUME_NONNULL_END
