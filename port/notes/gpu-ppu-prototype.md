# Metal PPU prototype

The standalone `gpu_ppu_test` renders a captured frame on Metal and compares all
38,400 15-bit pixels against the software PPU. This is a correctness and timing
experiment. On macOS, **Debug → GPU PPU (Experimental)** displays the Metal
picture during gameplay, without also drawing the frame on the CPU. The CPU
still handles display timing and scanline capture; software rendering remains
available for fallback and paused redraws. The option resets to off on launch.
The shader covers the CPU renderer's modes 0–2, tile and affine backgrounds,
sprites, mosaic, windows, priorities, blending, brightness and forced blank.

Each captured line contains the registers, VRAM, palettes, OAM, affine reference
points, mosaic counters and debug layer mask as the CPU saw them. This preserves
HBlank writes, including changes to memory, but costs 15.33 MiB per frame. That
is intentionally a simple reference representation, not a proposed upload
format. Capture is off unless requested. Capture files are raw, build-specific
internal data; recreate them after changing `PpuCaptureLine`.

## Reproduce

```sh
cmake --build build/native --target tmc gpu_ppu_test ppu_test scanline_test
TMC_SAVE=/tmp/gpu-ppu-test.sav TMC_FRAMES=5476 \
  TMC_INPUT=@port/scripts/play/01_new_game.txt \
  TMC_GPU_CAPTURE=5475:/tmp/gpu-iris.bin build/native/tmc
build/native/gpu_ppu_test /tmp/gpu-iris.bin
```

The test uses a Metal device, which must be accessible to the process. Its
reported upload is a one-time buffer allocation and copy; its GPU and submit/wait
timings average seven repeated draws from that same buffer, after one warm-up.
These are isolated measurements, not an end-to-end frame time or a power test.

Generate synthetic captures from the existing PPU and HBlank tests:

```sh
mkdir -p /tmp/gpu-ppu-cases
TMC_GPU_CASE_DIR=/tmp/gpu-ppu-cases TMC_GPU_CASE_SKIP_CPU=1 build/native/ppu_test
TMC_GPU_CASE_DIR=/tmp/gpu-ppu-cases TMC_GPU_CASE_SKIP_CPU=1 build/native/scanline_test
build/native/gpu_ppu_test /tmp/gpu-ppu-cases/objaff.bin
build/native/gpu_ppu_test /tmp/gpu-ppu-cases/scanline-affine.bin
```

For a single-frame CPU PPU timing in a **Release** build, run the same replay
with `TMC_GPU_PROFILE_FRAME=5475`. Add `TMC_GPU_CAPTURE=5475:/tmp/gpu-iris.bin`
to include the cost of recording every line. These timers themselves add some
overhead; the sample is one frame, so repeat runs and compare like builds.

## Sprite quad experiment

With GPU PPU enabled, **Debug → GPU Sprite Quads (Experimental)** replaces the
original per-pixel scan of all 128 sprite entries. Metal draws one thin quad for
each potentially visible sprite on each scanline; this uses each line's OAM and
memory snapshot, including mid-frame changes. A depth buffer selects the winner
by OBJ priority and OAM index. A separate pass builds the OBJ-window mask, and
the existing compositor applies backgrounds, windows and blending. Affine OBJ
sampling still uses the GBA's inverse fixed-point transform in the fragment
shader, with the original sprite bounding box and mosaic rules. Turn the option
off to compare the previous GPU sprite path; it starts off each launch.

To compare captures with the sprite-quad path, run `gpu_ppu_test` as above with
`TMC_GPU_SPRITE_QUADS=1`. Its reported GPU time includes both sprite render
passes and the final composition, but not frame capture or the app's display
filters. On an M4, three replay frames had no pixel differences and the isolated
GPU time was roughly 0.82–0.84 ms for quads versus 0.65–0.75 ms for the original
sprite scan. These are small, scene-specific samples, not a full-app benchmark.
The one-line quads preserve timing simply but still submit 128 instances for
each line in each pass; preparing fewer instances is a possible next experiment.
The full scanline capture is unchanged. The live app check can be run with both
`TMC_GPU_PPU_SELFTEST=1` and `TMC_GPU_SPRITE_SELFTEST=1`.

## Initial measurement

On one Apple M4, 2026-10-30, three captures from the new-game replay (frames
1000, 5475, 9000, including the iris at 5475) and 15 synthetic graphics cases
plus four HBlank DMA cases had **zero pixel differences**. This is not exhaustive
coverage of the game or other GPUs.

In a Release build, three single-frame CPU PPU samples were approximately
0.56–0.75 ms without capture and 1.57–1.82 ms with capture. For the same three
captured frames, the GPU kernel took 0.66–0.75 ms on average across warm runs;
one of those figures includes the other side's setup. The one-time Metal buffer
allocation/copy took 1.30–3.63 ms, while submit-and-wait took 1.07–1.15 ms for
warm runs. These observations do not establish an end-to-end speedup; the full
line snapshots are already more expensive to collect than the instrumented
software drawing they would replace. The sampling timers are enabled during
both CPU comparisons. The experiment did not measure power or input latency.

The macOS app can also check one live GPU texture against its matching software
frame and toggle the option on and off automatically with
`TMC_GPU_PPU_SELFTEST=1`. For that test only, the first 60 GPU frames also draw a
software reference; later frames skip CPU pixel drawing. The results are logged
as `GPU PPU self-test`. Add `TMC_GPU_PPU_SELFTEST_PAUSE=1` to test switching back
while paused.

A more efficient GPU path would use a smaller representation of memory
changes, preserve per-line semantics, and compare complete frame pacing,
latency and power against the software path. The software renderer remains the
reference and the renderer for headless tools. The prototype needs 15.33 MiB per
captured frame plus buffers for frames in flight; its first Metal compute encoder
can also increase driver memory (see `memory-profile.md`).
