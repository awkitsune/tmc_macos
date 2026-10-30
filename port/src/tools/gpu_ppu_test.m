#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "port/ppucapture.h"
#import "spritepass.h"
#import "backgroundpass.h"

#ifndef GPU_PPU_SHADER
#error GPU_PPU_SHADER must name the prototype shader
#endif

static double NowMs(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

int main(int argc, const char** argv) {
    @autoreleasepool {
        if (argc != 2) { fprintf(stderr, "usage: gpu_ppu_test capture.bin\n"); return 2; }
        FILE* file = fopen(argv[1], "rb");
        PpuCapture* capture = malloc(sizeof(*capture));
        if (!file || !capture || fread(capture, sizeof(*capture), 1, file) != 1 || fgetc(file) != EOF) {
            fprintf(stderr, "invalid capture: %s\n", argv[1]); return 2;
        }
        fclose(file);
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) { fprintf(stderr, "no Metal device available\n"); return 2; }
        NSError* error = nil;
        NSString* source = [NSString stringWithContentsOfFile:@GPU_PPU_SHADER encoding:NSUTF8StringEncoding error:&error];
        if (!source) { fprintf(stderr, "shader source unavailable: %s\n", error.localizedDescription.UTF8String); return 2; }
        BOOL compactMode = getenv("TMC_GPU_COMPACT_CAPTURE") != NULL;
        MTLCompileOptions* options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion2_4;
        if (compactMode) options.preprocessorMacros = @{ @"PPU_COMPACT": @1 };
        id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
        if (!library) { fprintf(stderr, "shader compile failed: %s\n", error.localizedDescription.UTF8String); return 2; }
        BOOL raster = getenv("TMC_GPU_SPRITE_QUADS") != NULL;
        SpritePass* sprites = raster ? [[SpritePass alloc] initWithDevice:device library:library error:&error] : nil;
        if (raster && !sprites) { fprintf(stderr, "sprite pipeline failed: %s\n", error.localizedDescription.UTF8String); return 2; }
        BOOL layersMode = getenv("TMC_GPU_BG_LAYERS") != NULL;
        BackgroundPass* backgrounds = layersMode ? [[BackgroundPass alloc] initWithDevice:device library:library error:&error] : nil;
        if (layersMode && !backgrounds) { fprintf(stderr, "background pipeline failed: %s\n", error.localizedDescription.UTF8String); return 2; }
        NSString* function = layersMode ? (raster ? @"gpu_ppu_raster_layers" : @"gpu_ppu_layers") :
                                         (raster ? @"gpu_ppu_raster" : @"gpu_ppu");
        id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:[library newFunctionWithName:function] error:&error];
        if (!pipeline) { fprintf(stderr, "pipeline failed: %s\n", error.localizedDescription.UTF8String); return 2; }
        id<MTLCommandQueue> queue = [device newCommandQueue];
        PpuCompactCapture* compact = compactMode ? malloc(sizeof(*compact)) : NULL;
        double compactMs = 0;
        if (compactMode) {
            if (!compact) { fprintf(stderr, "compact capture allocation failed\n"); return 2; }
            compact->pageCount = 0;
            double start = NowMs();
            for (int i = 0; i < PPU_HEIGHT; i++) {
                PpuCaptureMeta meta;
                memcpy(&meta, (const uint8_t*)&capture->lines[i] + offsetof(PpuCaptureLine, palette), sizeof(meta));
                PpuCompactAppend(compact, i, capture->lines[i].vram, &meta);
            }
            compactMs = NowMs() - start;
        }
        double uploadStart = NowMs();
        NSUInteger inputSize = compactMode ? PpuCompactSize(compact) : sizeof(capture->lines);
        id<MTLBuffer> lines = [device newBufferWithBytes:compactMode ? (void*)compact : (void*)capture->lines
                                                 length:inputSize options:MTLResourceStorageModeShared];
        NSUInteger poolOffset = compactMode ? offsetof(PpuCompactCapture, pages) : 0;
        id<MTLBuffer> output = [device newBufferWithLength:sizeof(capture->reference) options:MTLResourceStorageModeShared];
        double uploadMs = NowMs() - uploadStart;
        if (!queue || !lines || !output) { fprintf(stderr, "Metal allocation failed\n"); return 2; }
        double gpuMs = 0, wallMs = 0;
        const int runs = 8;
        for (int i = 0; i < runs; i++) {
            double start = NowMs();
            id<MTLCommandBuffer> commands = [queue commandBuffer];
            if (raster) [sprites encode:commands lines:lines poolOffset:poolOffset slot:0];
            if (layersMode) [backgrounds encode:commands lines:lines poolOffset:poolOffset slot:0];
            id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
            [encoder setComputePipelineState:pipeline];
            [encoder setBuffer:lines offset:0 atIndex:0];
            [encoder setBuffer:output offset:0 atIndex:1];
            [encoder setBuffer:lines offset:poolOffset atIndex:2];
            if (raster) { [encoder setTexture:[sprites colors:0] atIndex:0]; [encoder setTexture:[sprites windows:0] atIndex:1]; }
            if (layersMode) [encoder setTexture:[backgrounds layers:0] atIndex:3];
            [encoder dispatchThreads:MTLSizeMake(PPU_WIDTH, PPU_HEIGHT, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
            [encoder endEncoding];
            [commands commit];
            [commands waitUntilCompleted];
            if (commands.status != MTLCommandBufferStatusCompleted) {
                fprintf(stderr, "Metal execution failed: %s\n", commands.error.localizedDescription.UTF8String); return 2;
            }
            if (i) { wallMs += NowMs() - start; gpuMs += (commands.GPUEndTime - commands.GPUStartTime) * 1000; }
        }
        const uint16_t* got = output.contents;
        int bad = 0, first = -1;
        for (int i = 0; i < PPU_WIDTH * PPU_HEIGHT; i++) {
            if (got[i] != capture->reference[i]) { if (first < 0) first = i; bad++; }
        }
        printf("%s (%s, %s): %d differing pixels", device.name.UTF8String, raster ? "sprite quads" : "sprite scan", layersMode ? "background layers" : "single pass", bad);
        if (first >= 0) printf("; first (%d,%d): GPU %04x CPU %04x", first % PPU_WIDTH, first / PPU_WIDTH, got[first], capture->reference[first]);
        printf("\ninput %.2f MiB, upload %.3f ms, GPU %.3f ms, submit/wait %.3f ms (mean of %d warm runs)\n",
               inputSize / 1048576.0, uploadMs, gpuMs / (runs - 1), wallMs / (runs - 1), runs - 1);
        if (compactMode) printf("compact %.2f MiB, %u pages, CPU packing %.3f ms\n", PpuCompactSize(compact) / 1048576.0, compact->pageCount, compactMs);
        free(compact);
        free(capture);
        return bad != 0;
    }
}
