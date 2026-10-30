// Draws a captured frame (TMC_GPU_CAPTURE, or the cases ppu_test and
// scanline_test write) with the app's Metal PPU and compares every pixel with
// the software PPU's picture stored in the capture.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "port/ppucapture.h"
#import "gpuppu.h"

#ifndef GPU_PPU_SHADER
#error GPU_PPU_SHADER must name the Metal PPU shader
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
        GpuPpu* ppu = source ? [[GpuPpu alloc] initWithDevice:device source:source slots:1 error:&error] : nil;
        if (!ppu) { fprintf(stderr, "Metal PPU unavailable: %s\n", error.localizedDescription.UTF8String); return 2; }

        // The capture file holds full lines; pack them as the app's game thread does.
        PpuCompactCapture* compact = [ppu input:0];
        compact->pageCount = 0;
        double start = NowMs();
        for (int i = 0; i < PPU_HEIGHT; i++) {
            const PpuCaptureLine* line = &capture->lines[i];
            PpuCaptureMeta meta;
            memcpy(&meta, line->regs, sizeof(meta));
            PpuCompactAppend(compact, i, line->vram, line->palette, line->oam, &meta);
        }
        double packMs = NowMs() - start;

        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLBuffer> output = [device newBufferWithLength:PPU_WIDTH * PPU_HEIGHT * 4 options:MTLResourceStorageModeShared];
        if (!queue || !output) { fprintf(stderr, "Metal allocation failed\n"); return 2; }
        double gpuMs = 0, wallMs = 0;
        const int runs = 8;
        for (int i = 0; i < runs; i++) {
            start = NowMs();
            id<MTLCommandBuffer> commands = [queue commandBuffer];
            [ppu encode:commands slot:0];
            id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
            [blit copyFromTexture:[ppu output:0] sourceSlice:0 sourceLevel:0
                     sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(PPU_WIDTH, PPU_HEIGHT, 1)
                         toBuffer:output destinationOffset:0 destinationBytesPerRow:PPU_WIDTH * 4
          destinationBytesPerImage:PPU_WIDTH * PPU_HEIGHT * 4];
            [blit endEncoding];
            [commands commit];
            [commands waitUntilCompleted];
            if (commands.status != MTLCommandBufferStatusCompleted) {
                fprintf(stderr, "Metal execution failed: %s\n", commands.error.localizedDescription.UTF8String); return 2;
            }
            if (i) { wallMs += NowMs() - start; gpuMs += (commands.GPUEndTime - commands.GPUStartTime) * 1000; }
        }
        // The output is RGBA8 with each 5-bit channel widened by (c << 3) | (c >> 2).
        const uint8_t* got = output.contents;
        int bad = 0, first = -1;
        for (int i = 0; i < PPU_WIDTH * PPU_HEIGHT; i++) {
            uint16_t want = capture->reference[i];
            bool same = got[i * 4 + 3] == 255;
            for (int c = 0; c < 3; c++) same &= got[i * 4 + c] >> 3 == ((want >> (c * 5)) & 31);
            if (!same) { if (first < 0) first = i; bad++; }
        }
        printf("%s: %d differing pixels", device.name.UTF8String, bad);
        if (first >= 0) {
            const uint8_t* p = got + first * 4;
            printf("; first (%d,%d): GPU %04x CPU %04x", first % PPU_WIDTH, first / PPU_WIDTH,
                   (p[0] >> 3) | (p[1] >> 3) << 5 | (p[2] >> 3) << 10, capture->reference[first]);
        }
        printf("\ninput %.2f MiB (%u pages, packed in %.3f ms), GPU %.3f ms, submit/wait %.3f ms (mean of %d warm runs)\n",
               PpuCompactSize(compact) / 1048576.0, compact->pageCount, packMs, gpuMs / (runs - 1), wallMs / (runs - 1), runs - 1);
        free(capture);
        return bad != 0;
    }
}
