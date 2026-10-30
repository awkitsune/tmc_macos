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
HBlank writes, including changes to memory. The original full capture costs
15.33 MiB per frame; the optional compact capture below shares unchanged pages.
Capture is off unless requested. Capture files are raw, build-specific
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

## Compact scanline capture experiment

With GPU PPU enabled, **Debug → Compact GPU Capture (Experimental)** shares
unchanged 1 KiB VRAM pages between scanlines. If no HBlank DMA, HBlank interrupt
or VCOUNT interrupt ran since the previous line, the game thread cannot have
changed VRAM and the page indices are reused directly. After any such event, the
CPU compares VRAM with a contiguous previous snapshot, then compares individual
pages only if it changed; changed pages are copied into the frame's pool. This
catches ordinary writes by interrupt handlers as well as DMA without modifying
game code. Outside the frame loop, capture conservatively compares VRAM on each
line because callers may write memory directly. Registers, palette RAM, OAM,
affine references, mosaic counters and layer masks are still recorded per
line. Metal shaders read the page pool directly. The full capture remains the
default and both sprite renderers work with either mode.

The Performance window reports the latest capture size. Three captured replay
frames on an M4 had no mid-frame VRAM changes and used 96 pages, reducing each
input from 15.33 MiB to about 0.48 MiB. In a separate synthetic test with direct
VRAM writes between lines, the compact input was 0.79 MiB and matched the
software frame. These are input sizes, not proof of a full-app speedup. The CPU still compares VRAM after possible writes. To handle arbitrary changes
safely, compact mode reserves worst-case capacity; it can use slightly more space than
the full input when every page changes on every line, and the experimental app
keeps full-mode buffers for switching. Compare game-thread and GPU timings in
actual play before drawing conclusions about speed or memory.

The standalone check accepts `TMC_GPU_COMPACT_CAPTURE=1`; combine it with
`TMC_GPU_SPRITE_QUADS=1` for the quad path. `ppu_test` checks direct per-line
writes, metadata and the full-capacity worst case. `scanline_test` compares full
and compact capture and checks HBlank VRAM, palette and OAM DMA writes plus
VRAM writes from HBlank and VCOUNT interrupt handlers. The app's live comparison can be run with
`TMC_GPU_PPU_SELFTEST=1 TMC_GPU_COMPACT_SELFTEST=1`; add
`TMC_GPU_SPRITE_SELFTEST=1` to exercise both options together.

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
The sprite pass works with full or compact scanline capture. The live app check can be run with both
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

Future work can reduce comparison and worst-case buffer costs and should compare
complete frame pacing, latency and power against the software path. The software renderer remains the
reference and the renderer for headless tools. The prototype needs 15.33 MiB per
captured frame plus buffers for frames in flight; its first Metal compute encoder
can also increase driver memory (see `memory-profile.md`).
