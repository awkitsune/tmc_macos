# GPU PPU

On macOS, **Settings → Rendering → Draw the game on the GPU** (on by default)
draws the game picture with Metal instead of the software PPU (`port/src/ppu.c`).
The software PPU remains the reference, the renderer for headless tools, and the
fallback if Metal fails to start or to draw a frame.

## How a frame is drawn

1. **Capture (game thread, `ppu.c`, `ppucapture.c`).** `HwRunFrame` still
   steps every scanline with its HBlank DMA and interrupts, but records each
   line's state instead of drawing it: registers, palette, OAM, affine
   reference points, mosaic counters, the debug layer mask, and VRAM as 1 KiB
   pages shared between lines. A typical frame is about 0.5 MiB: 96 pages plus
   about 2 KiB of other state per line.
2. **Publish (`game.c`).** The finished capture is swapped into the shared slot
   under `sFrameLock`. The renderer copies it into the Metal buffer of the next
   frame in flight.
3. **Draw (`gpuppu.m`, `gpuppu.metal`).**
   - Sprites are drawn as one-pixel-high quads, one per OAM entry per line.
     Depth picks the winner by OBJ priority, then OAM index. A second pass
     builds the OBJ-window mask.
   - A compute pass draws BG0–BG3 into the four layers of an R16Uint texture
     array (0xffff is transparent), including affine sampling and mosaic.
   - A compute pass composes the layers with the sprites, windows, blending,
     brightness and forced blank, and writes an RGBA8 frame. That frame feeds
     the usual LCD-response and filter passes.

**When pages are shared.** VRAM pages are reused without comparison when no game
code could have run since the previous line: no HBlank or VCOUNT interrupt ran,
and any HBlank DMA wrote only to I/O registers (`HwDmaHBlank`). Otherwise the CPU
compares VRAM with its previous copy (first all of it, then page by page) and
copies the pages that changed. Every visible line still records its palette,
OAM and registers in full. The pool reserves space for every page of every line
changing, so a frame can never run out.

The shader is compiled from source when the GPU PPU starts, like
`shaders.metal`. Its `Line` struct must match `PpuCompactLine`; `ppucapture.c`
asserts the layout.

## Checking it

`gpu_ppu_test` draws a captured frame with the same `GpuPpu` class and compares
all 38,400 pixels with the software PPU's picture stored in the capture.

```sh
cmake --build build/native --target tmc gpu_ppu_test ppu_test scanline_test
TMC_SAVE=/tmp/gpu-ppu-test.sav TMC_FRAMES=5476 \
  TMC_INPUT=@port/scripts/play/01_new_game.txt \
  TMC_GPU_CAPTURE=5475:/tmp/gpu-iris.bin build/native/tmc
build/native/gpu_ppu_test /tmp/gpu-iris.bin

# Synthetic cases from the PPU and HBlank tests
mkdir -p /tmp/gpu-ppu-cases
TMC_GPU_CASE_DIR=/tmp/gpu-ppu-cases TMC_GPU_CASE_SKIP_CPU=1 build/native/ppu_test
TMC_GPU_CASE_DIR=/tmp/gpu-ppu-cases TMC_GPU_CASE_SKIP_CPU=1 build/native/scanline_test
for f in /tmp/gpu-ppu-cases/*.bin; do build/native/gpu_ppu_test "$f"; done
```

- **Capture files** hold full per-line snapshots (`PpuCapture`); the tool packs
  them as the game thread does. They are raw and specific to one build, so
  recreate them after changing `PpuCaptureLine`.
- **`ppu_test` and `scanline_test`** also check that the compact capture holds
  the same data as the full one, including HBlank DMA and interrupt writes to
  VRAM, palette and OAM, and the worst case where every page changes.
- **`TMC_GPU_PROFILE_FRAME=N`** (headless) prints frame N's software PPU time.

In the app:

- **`TMC_GPU_PPU_SELFTEST=1`** switches GPU drawing on and off. The game thread
  also draws the first 60 GPU frames in software, and the renderer compares one
  of them pixel by pixel. Results are logged as `GPU PPU self-test`. Add
  `TMC_GPU_PPU_SELFTEST_PAUSE=1` to switch back while paused.
- **`TMC_GPU_SETTINGS_SELFTEST=1`** checks the Settings checkbox and its saved
  preference.

## Measurements

On an Apple M4 on 2026-10-30, 3 replay frames (1000, 5475 with the iris
wipe, and 9000) and 34 synthetic cases matched the software PPU with zero
differing pixels. This does not cover the whole game or other GPUs.

The isolated GPU time for one frame was about 0.87 ms, measured with the
readback that `gpu_ppu_test` adds. In play, the game thread's PPU work drops
from drawing every pixel to recording state; see Debug → Performance.
