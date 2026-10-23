# Memory profile of TMC.app (macOS arm64)

Why Activity Monitor shows TMC.app at 120-300 MB when the game state is a few MB, and whether it grows.
Activity Monitor's "Memory" column is the physical footprint (`footprint`, `vmmap` "Physical footprint").
Measured 2026-10-23 on macOS 27.0.1 with a 1710x1073-point Retina display (2x).

## Verdict

- **Nothing grows.** 10 minutes of unattended gameplay: 41 / 41 / 39 / 42 MB with the window off screen, and
  142 / 143 / 154 / 147 MB at 10 s / 1 / 5 / 10 min with it on screen. The visible-run figures include 16 MB of
  MallocStackLogging data, so the real footprint is about 131 MB. The 154 MB is a transient 4th and 5th drawable that
  CAMetalLayer let go again. The malloc heap stays at 9.3-10.1 MB. `leaks` finds 26 KB in both runs, all in one
  system XPC connection cycle (`LNDaemonApplicationInterface`), none of it ours.
- **The game itself is ~5-9 MB.** Most of the rest is the Metal/AppKit baseline. A clear-only MTKView test app
  at the same window size uses 75 MB.
- **One fix was ours and worth making:** the LCD response pass was the app's only compute encoder, and the first
  compute encoder costs ~35 MB of driver memory for the life of the process. It is now a render pass with two
  colour targets: 122-125 MB -> 91 MB at the title screen.
- **200-300 MB comes from window size.** There are three CAMetalLayer drawables at the window's pixel size. The
  same title screen in a 1710x996-point window used 191 MB before the fix and 157 MB after it, and full screen on a
  larger external display adds more (see below).

## Where it goes (title screen, 720x512-point window, before -> after the fix)

| Region (`footprint`) | Before | After | What it is |
|---|---|---|---|
| Owned physical footprint (unmapped) (graphics) | 80 MB | 46 MB | Metal driver memory: 2 MB slabs (39 of them before). ~43 MB is there in any Metal app (clear-only test app: 43 MB); ~35 MB was the compute encoder. |
| IOSurface | 16 MB | 16 MB | 3 x 'CAMetalLayer Display Drawable', 1440x960 BGRA = 5.3 MB each. Scales with window size. |
| Malloc (Small/Tiny/Metadata) | 12 MB | 12 MB | 6.4 MB allocated, almost all AppKit/CF/objc/Metal (class caches, CFStrings, Metal binary archive). The clear-only test app has 9 MB. |
| IOAccelerator (graphics) | 4 MB | 4 MB | Metal command buffers and small textures (the 240x160 frame/history textures are < 2 MB in total). |
| __DATA / __DATA_DIRTY / __AUTH of all images | ~5 MB | ~5 MB | System frameworks' writable data. Clean shared-cache pages are not counted. |
| __TMCSTATE | 336 KB | 336 KB | GBA EWRAM/IWRAM/VRAM/OAM mirrors and the rest of the savestated game state. |
| Binary __DATA_CONST (baked ROM data, 14.5 MB) | 128 KB dirty | same | Clean file-backed pages, not counted in footprint. |
| Binary __bss (6.8 MB) | mostly untouched | same | 3 MB of it is latency-tool sample arrays (`latency.c`), only touched with TMC_LATENCY_LOG. Audio rings: 256 KB + 128 KB. |
| Stacks | 352 KB | 352 KB | 20 threads. |
| CoreAudio (caulk zones) | 32 KB resident | same | 10 MB virtual, not resident. |
| **Total** | **122-125 MB** | **91 MB** | |

## Game vs platform

| Run | Peak footprint | Notes |
|---|---|---|
| Headless `tmc`, Ezlo replay (3000 frames) | 5.1 MB | `/usr/bin/time -l`; max RSS 9.7 MB |
| Headless `tmc`, 01_new_game 24000 frames | 8.7 MB | max RSS 13.9 MB |
| App, window off screen, 10 min gameplay | 45 MB | No drawables or Metal slabs: they are only made while drawing |
| Clear-only MTKView test app, 720x480 window | 75 MB | The platform floor at this window size |
| App, title, 720x512 window | 125 -> 91 MB | before -> after |
| App, 10 min gameplay, visible | 156.6 MB (incl. 16 MB MallocStackLogging) | before the fix; no growth (see above) |
| App, title, 1710x996-point window | 191 -> 157 MB | Drawables: 77 MB |

The drawables cost 3 x width x height x 4 bytes at the window's pixel size. On this display full screen
(3420x2214) that is ~91 MB. On a 5K display it is ~177 MB, and on top of the ~80 MB above that makes 250+ MB.
That is the 200-300 MB seen. It does not grow, and the only way to shrink it is fewer or smaller drawables:
- `maximumDrawableCount` = 2 saves one drawable but works against the display-sync pacing.
- A lower-resolution drawable blurs the LCD/CRT filters.

Neither is worth it, so both are left alone. The debug windows add their own layers while open. They were not
open in these runs.

## Suspects checked and cleared

- Renderer: every texture and pipeline is made once in `-initWithView:`. Per frame there is one command buffer and
  `currentDrawable`, nothing else. The debug-readback textures are only made while a debug buffer is selected and
  are released when it is closed. The two-frames-in-flight change uses a semaphore over 2 fixed upload textures.
- Shaders are compiled once at launch (`newLibraryWithSource`); there is no pipeline cache to grow.
- Datagen tables are used in place from `__DATA_CONST` (clean pages), not copied to the heap.
- The heap has no per-frame mallocs: the 3,700-3,900 "calloc in dispatch_async" blocks (~300 KB) stay flat between
  1 and 10 minutes.
- The savestate, latency-log and self-test buffers are static and only touched when those features are used.

## How to reproduce

- Off screen, the window's MTKView does not draw, so a background launch measures ~41 MB. Keep the window
  on screen with `TMC_LATENCY_LOG=/path` (latency tools keep the window in front).
- `MallocStackLogging=1` only takes effect when TMC is launched directly, not under `/usr/bin/time`.
- Try other window sizes without touching saved preferences: `TMC -"NSWindow Frame GameWindow" "0 0 1710 1073 0 0 1710 1073"`.
  The app then autosaves that frame, so restore `defaults write io.github.awkitsune.tmc-macos "NSWindow Frame GameWindow" ...` afterwards.
- `footprint <pid>`, `footprint -v <pid> | grep IOSurface` (names the drawables), `vmmap --summary`, `heap`, `leaks`.
