# The Minish Cap: native macOS and iOS port

This is a fork of [zeldaret/tmc](https://github.com/zeldaret/tmc), the decompilation of The Legend of Zelda: The
Minish Cap. The decompilation project's own README, with the supported ROMs and its links, is
[README.OLD.md](README.OLD.md); setting up the repository for the GBA build is [INSTALL.md](INSTALL.md).

The fork builds the decompiled game code natively for macOS (Apple silicon and Intel, macOS 12
or later): a Metal app with keyboard
and gamepad input, plus a headless runner for automated testing. The same game
core, renderer and sound also build as an iPhone and iPad app (iOS 16 or later;
see [iPhone and iPad](#iphone-and-ipad)). The GBA build in the repository root stays
byte-matching: every port change is either under `port/` or behind
`#ifdef PLATFORM_NATIVE`.

No game data is in this repository. Building needs your own ROM of The Minish Cap (USA), and the shared app asks for
one on first launch.

## Setup

One-time machine setup. The native build reads the GBA build's data objects, so
both toolchains are needed.

1. **Tools:** Xcode Command Line Tools, CMake ≥ 3.24 and Python 3.

   ```bash
   brew install cmake python3
   ```

2. **ROM:** place an original USA ROM (SHA1
   `b4bd50e4131b027c334547b4524e2dbbd4227130`) at `baserom.gba` in the
   repository root. A symlink works and `*.gba` is gitignored:

   ```bash
   ln -s ~/tmc_usa.gba baserom.gba
   ```

3. **Host tools and assets:** see [Building](#building) below.

4. **GBA toolchain:** `arm-none-eabi-gcc` and agbcc.
   agbcc is cloned next to this repository and installs into `tools/agbcc`; its
   install also leaves untracked `tools/include/` and `tools/lib/`, which are not
   committed.

   ```bash
   brew install arm-none-eabi-gcc arm-none-eabi-binutils
   ```

   ```bash
   git clone https://github.com/pret/agbcc ../agbcc
   ```

   ```bash
   (cd ../agbcc && sh build.sh && sh install.sh ../tmc_macos)
   ```

   Replace `../tmc_macos` with the path to this checkout if it lives elsewhere.

5. **Python packages:** the GBA `make` needs `pycparser`, and the native data
   converter needs `pyelftools` and `libclang`. Homebrew's Python refuses global
   installs, so use the repository's `.venv` (gitignored):

   ```bash
   python3 -m venv .venv && .venv/bin/pip install -r port/requirements.txt
   ```

## Building

Build the host tools and extract assets (CMake 4 needs the policy override for a
bundled dependency; recent clang needs the warning relaxed):

```bash
CMAKE_POLICY_VERSION_MINIMUM=3.5 cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=tools -DCMAKE_CXX_FLAGS=-Wno-error=deprecated-literal-operator -S tools -B tools/cmake-build
```

```bash
cmake --build tools/cmake-build -j --target install
```

Build the GBA ROM. This also extracts the assets, and its objects and `tmc.elf`
are the input to the native data converter. A matching build ends with
`tmc.gba: OK`:

```bash
PATH="$PWD/.venv/bin:$PATH" make -j$(sysctl -n hw.ncpu)
```

Configure and build the native game. The first configure converts the game data
(about a minute). Builds convert it again by themselves when datagen's code,
`hints.txt`, `tmc.elf` or `linker.ld` is newer than the last conversion (so
rebuild the GBA game first after changing game data); `cmake --build build/native
--target datagen` forces a rerun:

```bash
cmake -S port -B build/native
```

```bash
cmake --build build/native -j
```

This builds three programs from the same game code:

- `build/native/The Minish Cap.app`, the macOS app (see [Playing](#playing)).
- `build/native/tmc`, the headless runner for automated runs (see below).
- `build/native/tmc-songs`, the song exporter (see [Exporting the music](#exporting-the-music)).

Build just one with `--target tmc_app`, `--target tmc` or `--target tmc_songs`. If the link ever breaks, `cmake --build build/native --target
link-report` groups the unresolved symbols by where the GBA build defines them.

### Development and distributable builds

The build above is a development build: the game's assets are built into the
programs, so they run as they are, but they must not be shared.
`-DTMC_BAKE_DATA=OFF` makes a distributable build without them; each program
extracts them from the player's own ROM instead (see
[First launch](#first-launch)). Use a separate build folder:

```bash
cmake -S port -B build/native-rom -DTMC_BAKE_DATA=OFF
```

```bash
cmake --build build/native-rom -j
```

What is left out is decided by where each byte comes from, not by whether it
equals the ROM:

- **Assets** are what asset extraction (`tools/asset_processor`, `assets.json`)
  copies out of `baserom.gba` into `build/USA/assets`, and the data sources
  `.incbin`: graphics, tilesets, palettes, maps, entity and room data blobs,
  sound samples and songs, text. They come from the player's ROM: 13.1 of the
  13.8 MB of game data.
- **Everything written in the repository** stays compiled in: the C code and
  its tables, data spelled out in `data/*.s` (`.byte`/`.4byte` lists and
  macros, about 670 KB), and the pointer tables datagen lays out. All of it is
  public in the decomp.

To find the assets' bytes, datagen assembles every data object twice more:
once with each extracted asset file replaced by a file of the same size full
of `0x55`, once full of `0xaa`. The bytes that differ are asset bytes
(`port/tools/datagen/provenance.py`). Those become zeros plus a run each
(native address, ROM offset, length), about 1,000 runs; every run is checked
against `tmc.gba`. Building still needs the GBA build and its extracted
assets. The programs are about 15 MB: the zeros take file space, because
nearly every data section also holds source-written bytes. They compress to
almost nothing.

To check that a program carries no asset content:

```bash
python3 port/scripts/asset_overlap.py "build/native-rom/The Minish Cap.app/Contents/MacOS/The Minish Cap" build/USA/assets
```

It lists every stretch of 64 bytes or more that the program shares with an
extracted asset file, with the file and the program's symbol, and exits with 1
if there is one. A development build shares 12.7 MB. A distributable build
shares 5 stretches, 710 bytes. These are source-written tables whose bytes
also occur inside asset dumps: entity lists from `data/map/entity_headers.s`
that the ROM holds again inside a blob, one equal to a USA-demo-only asset, and
a C table in `src/manager/diggingCaveEntranceManager.c`.

In a development build with `TMC_ROM=path` set, `tmc` checks every run of the
manifest against that ROM before it starts (`built-in data: all 46923 manifest
runs ... match the ROM`).

### Universal builds and a package to share

The programs run on macOS 12 and later (`CMAKE_OSX_DEPLOYMENT_TARGET`, which
also sets the app's `LSMinimumSystemVersion`). Using anything newer than that
is a build error (`-Werror=unguarded-availability-new`) unless it is behind an
`@available` check. They build for this Mac's architecture unless
`CMAKE_OSX_ARCHITECTURES` says otherwise; `"arm64;x86_64"` makes universal
programs that also run on Intel Macs. Only Python runs during the build
(datagen, the name tables), and datagen's output suits both architectures,
since the native data layouts are the same.

A universal distributable app, zipped with its ad hoc signature intact and its read-me
(`port/dist/Read me first.txt`), as `build/TheMinishCap-1.0-macos-universal.zip` (the version is the one in
`port/CMakeLists.txt`):

```bash
port/scripts/package_macos.sh
```

By hand, without the read-me:

```bash
cmake -S port -B build/dist -DTMC_BAKE_DATA=OFF -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build/dist -j4 && ditto -c -k --norsrc --noextattr --keepParent "build/dist/The Minish Cap.app" build/TMC-macos-universal.zip
```

Check it before sending it: `lipo -archs` and `vtool -show-build` on
`The Minish Cap.app/Contents/MacOS/The Minish Cap` should say `x86_64 arm64` and `minos 12.0`, and
`asset_overlap.py` (above) the 5 known stretches.

What the person you send it to does on first launch:

1. Unzip it and move `The Minish Cap.app` to Applications (or anywhere).
2. Open it with right-click (or Control-click) → Open, then Open in the alert.
   It is signed ad hoc, not by an identified developer, so a double-click only
   says it cannot be checked. Recent macOS versions instead need System
   Settings → Privacy & Security → Open Anyway after the first try.
3. Choose their own *The Minish Cap* (USA) ROM when asked
   ([First launch](#first-launch)). Its data goes to
   `~/Library/Application Support/tmc_macos`; the app holds no game assets.

The x86_64 programs were checked under Rosetta 2 on Apple silicon: the
24000-frame playthrough's frame checksums and player trace, the Ezlo replay and
the app self-test (`TMC_SELFTEST`) match the arm64 programs. Save states load
across the two (by symbol), both ways, and carry on with the same frames. The audio
mixer's samples differ in the last bit, as arm64 fuses multiply-adds and
baseline x86_64 has no FMA instructions. Rosetta cannot show:

- **An Intel or AMD GPU.** Under Rosetta the app draws on the Apple GPU. On
  those GPUs textures cannot use shared storage, so the renderer uses managed
  textures there and synchronizes them before reading them back.
  `TMC_MANAGED_TEXTURES=1` takes that path on Apple silicon too, and the
  self-test passes with it, with the same shader buffer checksums. Drivers can
  still differ: the shaders (compiled at launch as Metal 2.4, the version
  macOS 12 has) and full-screen and display sync behaviour are untested on such
  a GPU.
- **macOS 12 itself.** Only the compiler's availability checks stand for it.
- **Speed on an older Intel Mac**, CPU or GPU.

Before 1.0 the universal app was played on a real Intel Mac with its own GPU: the Metal renderer (the GPU PPU
included), the filters and the debug tools all worked as on Apple silicon. macOS 12 itself is still unchecked.

### Playing

```bash
open "build/native/The Minish Cap.app"
```

The app runs the game at the GBA's 59.73 Hz (or, in display sync, once per
display refresh: see [Input latency](#input-latency)) and shows it scaled by a
whole number (or, with integer scaling off, as large as the 3:2 shape allows).
Sound comes from the game's own m4a engine (see [Sound](#sound)). The Sound
menu has Mute (⌥⌘M) and High-Quality Audio, which mixes at 48 kHz in floating
point instead of the GBA's 15.7 kHz 8-bit mix; both are remembered.

| Keyboard | GBA | Gamepad |
|---|---|---|
| Arrow keys | D-pad | D-pad or left stick |
| X | A | Button labelled A (Xbox A, PlayStation ✕, Nintendo A), or X |
| Z | B | Button labelled B (Xbox B, PlayStation ○, Nintendo B), or Y |
| A / S | L / R | Shoulders or triggers |
| Return | Start | Menu |
| Backspace or `\` | Select | Options |
| Tab (hold) | Fast-forward (4×) | Right stick click |

Keys are matched by position, so other keyboard layouts use the same physical
keys. Any extended gamepad (Xbox, PlayStation, Switch Pro, MFi) works and can be
connected or disconnected while the game runs. Settings → Controls → Gamepad A/B
switches Xbox and PlayStation pads to the GBA's positions instead (right and top
buttons = A, bottom and left = B); Nintendo pads already have A on the right, so
they are the same either way.

Menu shortcuts: ⌘1–⌘5 pick the filter (nearest neighbour, sharp bilinear,
smooth bilinear, LCD grid, CRT scanlines), ⌘I toggles integer scaling, ⌃⌘F
toggles full screen (Escape leaves it), ⌘P pauses, ⌘R resets (the game's own
A+B+Select+Start reset). The filter and scaling choice are remembered.

The Minish Cap → Settings… (⌘,) adjusts the filters live; values are saved and
Reset to Defaults restores these:

| Knob | Default | Range | Effect |
|---|---|---|---|
| LCD gap strength | 0.35 | 0–1 | How dark the gaps between pixels are |
| LCD gap width | 0.15 | 0–0.5 | Gap width, as a fraction of a GBA pixel |
| LCD subpixel strength | 0.35 | 0–1 | How strongly each RGB stripe dims the other two channels |
| LCD gap bloom | 0.00 | 0–1 | Bright pixels shrink their gaps (1 = no gap on white) |
| LCD brightness | 1.30 | 0.5–2 | Makes up for the light the gaps and stripes take |
| CRT scanline strength | 1.00 | 0–1 | Depth of the dark lines between scanlines |
| CRT bloom | 0.80 | 0–1 | How much a line's height follows its brightness (0 = all lines alike) |
| CRT beam width (dark) | 0.14 | 0.05–0.6 | Beam width on black, as a Gaussian sigma in scanlines |
| CRT beam width (bright) | 0.34 | 0.05–0.6 | Beam width on white |
| CRT aperture grille | 0.10 | 0–1 | RGB column mask, one screen pixel per column |
| CRT brightness | 1.35 | 0.5–2 | Brightness of the scanline beams |
| LCD response time | 22 ms | 0–40 ms | 10–90% time of a pixel getting brighter (with LCD response on) |
| LCD darkening slowdown | 1.30 | 1–2 | Getting darker takes this much longer (1.3 × 22 = 28.6 ms) |
| GBA colour correction | 0.00 | 0–1 | Blend towards the GBA screen's own colours (Pokefan531's gba-color matrix): less saturated and warmer |
| Gamma | 1.00 | 0.5–2 | Output = colour^gamma; above 1 darkens mid-tones |

The LCD and CRT masks are box-filtered over each screen pixel, so they average
to neutral at any scale: greys stay grey in a window, at fractional scales and
in full screen. Beam and gap sizes follow one brightness value per pixel
(halfway between luma and the brightest channel), the same for all three
channels, so bloom adds no colour fringes. Each CRT pixel sums the beams of its
own scanline and the two around it, so bright lines spread into dark ones.

**LCD response** (Settings → Display → LCD response, off by default) imitates the GBA
screen's slow pixels, which some effects count on: sprites flickered on
alternate frames look see-through and flashes leave a short afterglow. A
compute pass keeps what the simulated LCD shows in a 240×160 float16 texture,
in linear light (2.2 gamma), and for every new game frame moves each channel
towards the new picture by `shown = mix(new, shown, k)`, with
`k = exp(-frame / τ)`, frame = 1/59.73 s and τ = response time / ln 9 (so the
10–90% time is the setting). A channel getting darker uses the response time ×
the darkening slowdown. At the defaults k is 0.19 rising and 0.28 falling: 81%
of a black-to-white step shows in the first frame, 96% in the second; a 30 Hz
on/off flicker settles at 86% / 24% of full brightness (linear) instead of
100% / 0%. Whichever filter is chosen then draws that picture.

It steps once per game frame, never per display refresh, so it looks the same
at 60 and 120 Hz, and it holds while paused (Step Frame steps it once). When the
display misses game frames, as with fast-forward (up to 4x), one step covers
all of them (k^n for n frames, using the newest): the ghost lasts the same
number of game frames, so it is shorter in real time, and fast-forwarded
flicker can alias. The picture starts afresh from the frame itself on the first
frame, when the option is switched on, after a state load and after a reset.

The save is `~/Library/Application Support/tmc_macos/tmc.sav`, an 8 KB EEPROM
image (*The Minish Cap* menu → Show Save Folder). `TMC_SAVE=path` overrides it.

Save states (see [Save states](#save-states-and-checkpoints)) are in the Game
menu: Quick Save (F5) and Quick Load (F7) use one of four slots (⌥⌘1–⌥⌘4),
Save State As… (⇧⌘S) and Load State… (⌘O) use any file. They live in
`~/Library/Application Support/tmc_macos/states`. The Debug menu (shown except
in Release builds; Settings → Debug → Show Debug menu switches it) has Step Frame (F8:
pauses, then advances one frame per press; ⌘P resumes), Show Frame Counter (in
the window's subtitle), Load Checkpoint (the states `port/scripts/checkpoints.sh`
wrote in this source tree) and Show State Folder. `TMC_LOAD_STATE=path` starts
the app from a state. It also opens the graphics viewers (see
[Graphics debugging](#graphics-debugging)): Tiles (⌃⌘1), Sprites (⌃⌘2), Layers
(⌃⌘3), Palettes (⌃⌘4), IO Registers (⌃⌘5) and Shader Buffers (⌃⌘6).

The window's toolbar has Save/Load (the Game menu's state items), Filter (the
View menu's filters and Integer Scaling) and Settings buttons on the right, and
a Debug button with the Debug menu's items when Settings → Debug → Show debug tools in
toolbar is on. The buttons never take the keyboard, and View → Customize
Toolbar… rearranges them. The subtitle names the room the player is in
("Hyrule Field – Lon Lon Ranch", or the area's name for rooms without one, from
`include/area.h` and `include/roomid.h` via `port/scripts/room_names.py`),
followed by a recent save or load and the frame counter: "Deepwood Shrine –
Madderpillar · Saved slot 2". In full screen the toolbar hides with the menu bar.

### Input latency

From a key press to the first picture that shows it takes about 105 ms in the
first version of the app, measured as described below. Three things shortened
it:

- **Late input.** The game thread reads the keyboard and the gamepad itself,
  right before the game reads its input, rather than using what was last
  delivered to the main thread. The game thread then waits until shortly before
  the frame has to be ready (the time the last 32 frames took, plus a margin)
  and only then reads, so the input is as fresh as the pacing allows.
- **Less buffering.** Two frames in flight, and no more than two frames waiting
  for the screen: when a third would queue behind them, that refresh is left out
  and the next one shows the newest game frame instead of an older one. The
  compositor of a windowed Metal app needs about two refreshes between a frame
  being committed and being on the screen; anything beyond that is just lag.
- **Display sync** (below).

**Settings → Timing → Frame pacing** chooses how the game is paced:

| Mode | Behaviour |
|---|---|
| Accurate (59.73 Hz) | The game's own timer runs it at the GBA's rate, independent of the display. Every ~3.7 s at 60 Hz the display shows one game frame twice. |
| Display sync (60 Hz, less lag) | One game frame per display refresh (every other refresh on a 120 Hz display), started so that it is ready for a refresh it can still reach. The game runs 0.45% faster than a GBA, and the sound follows (the resampler is told the real rate, so nothing crackles or drifts). Used only when the display's rate is a multiple of ~60 Hz (60, 120, 180, 240: within 1.5 Hz); otherwise, and while fast-forwarding, it falls back to the timer. |

It can be switched while the game runs, and the note under it says whether
display sync is in use. The default is **Display sync**: the lag is what the
mode is for, a 0.45% speed difference cannot be heard or felt, and it falls back
to the accurate timer by itself where it cannot work. Pick Accurate for
recordings or comparisons with hardware timing.

To measure: `TMC_LATENCY_LOG=path` writes a CSV with one line per input change
(`source,frame,event_to_latch_ms,latch_to_present_ms,total_ms`, source `key` or
`pad`) and, at exit, percentiles, how many frames were presented, never shown,
shown twice or skipped, the present interval and the game-frame interval. The
clock starts at the key or gamepad event (the event's own timestamp) and stops
when the drawable showing the first game frame that saw it is presented.
`TMC_LATENCY_SYNTH=1` presses and releases a key at random by itself,
`TMC_QUIT_AFTER=seconds` quits, and `TMC_LATENCY_TRACE=1` prints the timeline
of every 60th frame. `TMC_PACING=accurate|sync` overrides the saved setting and
`TMC_SYNC_MARGIN_MS` (default 7) is the room display sync leaves after a frame's
work for the draw and the GPU.

### First launch

A distributable build asks for the ROM the first time it starts:

1. If there is no extracted data, an open panel asks for *The Minish Cap* (USA)
   ROM.
2. The app checks the file's size and SHA1. It names the problem when the file
   is the European, Japanese or a demo release, has a 512-byte copier header,
   is patched or a bad dump, is another game or cannot be read. The alert
   offers another file or quitting.
3. A progress window shows the extraction, on a background thread: verifying,
   then tables, graphics, maps and sound, then saving. It takes well under a
   second.
4. The game window opens.

The data goes to `~/Library/Application Support/tmc_macos/gamedata.bin`
(13.1 MB). It records the ROM's SHA1 and the version of the build's data
manifest. Each launch fills the game data from it in about 5–20 ms.

The app remembers where the ROM is, following it if it moves. When a new build
needs different data, it extracts again from that ROM without asking, or asks
again if the ROM is gone. *The Minish Cap* menu has Change ROM… and Re-extract
Game Data, which rewrite the data file while the game keeps running. Deleting
`gamedata.bin` brings back the first-launch flow.

For tests, `TMC_ROM=path` uses that ROM without asking (and without remembering
it), and `TMC_CACHE_DIR=dir` keeps the data file in `dir`. With `TMC_SELFTEST`,
a failure prints the alert's text and exits with status 1 instead of waiting
for a click. The progress window and any alert are saved as
`romsetup_progress.png` and `romsetup_error.png`.

A first launch from a clean data folder, through to the title screen:

```bash
TMC_CACHE_DIR=/tmp/tmc-data TMC_ROM=baserom.gba TMC_SAVE=/tmp/selftest.sav TMC_SELFTEST=/tmp/tmc-selftest "build/native-rom/The Minish Cap.app/Contents/MacOS/The Minish Cap"
```

`tmc` and `tmc-songs` from a distributable build read the same data file. If
it is missing or outdated, they extract it from `TMC_ROM`, or stop and say so.

`TMC_SELFTEST=dir` makes the app play itself through the title screen with
synthesized key presses, saving what it presented as PNGs in `dir`, then quit;
it needs no screen-recording permission. Besides the game screens it draws a
grey/colour test card (`pattern_<filter>_<integer|fractional|fullscreen>.png`)
with each filter at a whole-number scale, a fractional one and in full screen,
using the default knob values, and an image of each settings tab
(`settings_<tab>.png`). It checks that every setting is on one control in some
tab and that using it sets its default (`selftest: settings: … ok`), that the
window fits the screen and scrolls on a short one, and that it reopens on the
last tab. It also
runs the LCD response pass on one pixel through a black-to-white and a
white-to-black step and logs the values per frame and the 10–90% times it
measures against the setting (`selftest: LCD response … ok`). It copies back
every Shader Buffers texture on the test card and logs its size, crc32 and
per-channel range and mean (`selftest: … shader buffer …`), checking that the
filter output, the LCD response output and the drawable keep the frame's mean
colour with nearest filtering (`selftest: shader buffers ok`). Then it loads the
`hyrule_town` checkpoint, opens the graphics viewers, logs a line of numbers
from each and draws them as `debug_*.png`, with sprite 0 outlined in
`debug_sprite_highlight.png`. It checks the toolbar (`selftest: toolbar ok`: its
items with the Debug setting off and on, each dropdown against its menu bar
menu with the same checkmarks, no button taking the keyboard, the game keeping
its size below the toolbar) and the subtitle in the checkpoint's room
(`selftest: subtitle "Festival Town · Slot 1" … ok`):

```bash
TMC_SAVE=/tmp/selftest.sav TMC_SELFTEST=/tmp/tmc-selftest "build/native/The Minish Cap.app/Contents/MacOS/The Minish Cap"
```

### iPhone and iPad

The port also runs on iPhone and iPad (iOS/iPadOS 16 or later), played with a game controller, a hardware
keyboard or optional on-screen [touch controls](#touch-controls). The app contains none of the game's data: on first
launch it asks for your own ROM of The Minish Cap (USA) and extracts what it needs.

A free Apple ID is enough to install it on your own device. It needs a Mac with Xcode (the iOS platform, from
Xcode > Settings > Components; the Simulator is not needed) and this repository set up as in
[Setup](#setup) (assets extracted, the GBA build done, the Python environment).

1. **Add your Apple ID to Xcode.** Xcode > Settings > Accounts > **+** > Apple ID, and sign in. A free account shows
   up as "*Your Name* (Personal Team)".
2. **Connect the device.** Plug the iPhone or iPad into the Mac, unlock it and tap **Trust**.
3. **Turn on Developer Mode** on the device: Settings > Privacy & Security > Developer Mode > On, restart, then confirm
   with **Turn On**. (The switch appears once the device has been connected to a Mac with Xcode; if it is missing,
   open Xcode > Window > Devices and Simulators with the device plugged in.)
4. **Build and install.** From the repository root:

   ```bash
   port/scripts/ios_install.sh
   ```

   The script takes the team of the Apple ID in Xcode (or `--team TEAM_ID`) and the connected device (or
   `--device NAME`), configures the Xcode project, builds a signed Release app and installs it. By hand, the same is:

   ```bash
   cmake -S port -B build/ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 -DTMC_BAKE_DATA=OFF -DTMC_IOS_TEAM=TEAM_ID
   xcodebuild -project build/ios/tmc_native.xcodeproj -scheme tmc_ios -configuration Release -destination 'id=DEVICE_UDID' -allowProvisioningUpdates -allowProvisioningDeviceRegistration build
   xcrun devicectl device install app --device DEVICE_UDID build/ios/Release-iphoneos/TheMinishCap.app
   ```

   `TEAM_ID` is the 10-character ID of your team (`defaults read com.apple.dt.Xcode IDEProvisioningTeamByIdentifier`
   lists it as `teamID`); `DEVICE_UDID` comes from `xcrun devicectl list devices`.
5. **Trust the developer** the first time: the app will not open until you go to Settings > General > VPN & Device
   Management, tap your Apple ID under Developer App and tap **Trust**.
6. **Give it the ROM.** Put your `.gba` in the Files app (AirDrop, iCloud Drive, or Finder with the device
   connected), open The Minish Cap and tap **Choose ROM…**. The data is extracted once; Settings > Game > **Change
   ROM…** (or **Re-extract Game Data**) does it again later.
7. **Connect a controller** (Settings > Bluetooth on the device) or a keyboard, or turn on the touch controls. Press
   Home or hold ☰ on the controller, press Escape, or tap the screen for the settings. A short press of ☰ is Start,
   given when it is let go, so holding it for the settings never pauses the game too. On iOS 27, if Home still opens
   the system's Game overlay, Settings > Controls > **Home Button…** lets apps handle it.

#### Touch controls

Settings > Controls > **Touch controls** puts the GBA's buttons on the screen (off by default). With them on the game
needs no controller, and the **☰** button opens the settings instead of a tap (Home, holding ☰ on a controller and
Escape still work).

- **Landscape** is laid out like the original GBA: the D-pad with Start and Select under it on the left, A and B on the
  right, L and R in the top corners, ⏩ and ☰ bottom right, and the picture in the middle (5x on an iPhone 15).
- **Portrait** is like the GBA SP: the screen in halves, the picture in the top one (4x on an iPhone 15) and the
  controls in the bottom one: L and R as bars at its top, then the D-pad and A and B, with Start, Select and ☰ in the
  middle at the bottom and ⏩ in the bottom left corner.
- The buttons are the same size in points on iPhone and iPad, and the picture never sits under them: with Integer
  scaling it gets the largest whole-number scale that leaves them room, without it all the room they leave.
- **Button style**: Plain (flat translucent shapes, the default), Indigo (dark caps in purple recesses) or Outline
  (a line drawing). The art is from `port/design/skins/`; only the buttons are drawn, not a shell around them.
- **⏩** switches fast-forward with a tap (as Game > Fast-forward) and holds it while held.
- **Touch Layout** (per orientation, for the one shown): **Opacity** and **Size** sliders, **Move Controls…** (the game
  pauses; drag the D-pad, A and B, L, R, Start and Select, ☰ and ⏩ anywhere on the screen; Reset puts them back,
  Done ends) and **Reset Touch Layout**.
- Every finger counts on its own. The D-pad goes 8 ways by the angle of your thumb, and slides from one direction to
  the next without lifting. Slide between A and B to move the press; the gap between them presses both.
- **Haptics** (on by default): a light tap on each press and a softer tick when the D-pad changes direction. Devices
  without a Taptic Engine, like the iPad mini, have none.
- The buttons hide while a gamepad is connected, and the picture has the whole screen again; they come back when it
  disconnects.

#### With a free Apple ID

- **The app stops opening after 7 days**, when its provisioning profile expires. Plug the device in and run
  `port/scripts/ios_install.sh` again; installing over the old app keeps the saves.
- **At most 3 apps** installed this way can be on a device at once (and Xcode makes at most 10 new app IDs a week). If
  Xcode reports the limit, delete another app you installed from Xcode. If it says the bundle ID is taken, pick your
  own: `TMC_IOS_BUNDLE_ID=io.github.yourname.tmc-ios port/scripts/ios_install.sh`.
- TestFlight and the App Store need the paid Apple Developer Program.

The game's save is `tmc.sav` in Files > On My iPad (or iPhone) > The Minish Cap, the same 8 KB file as the Mac app's
`~/Library/Application Support/tmc_macos/tmc.sav`: copy it either way to carry a game between Mac and iPad. Quick
save states are next to it in `states`.

#### How the iOS app is built

The iOS/iPadOS app (`port/src/ios`) shares the game thread, renderer, shaders,
sound, input and settings storage with the Mac app (`port/src/apple`); only the
UIKit shell is its own. It is an Xcode project made by
CMake, always without the game's data (`TMC_BAKE_DATA=OFF`):

```bash
cmake -S port -B build/ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 -DTMC_BAKE_DATA=OFF
xcodebuild -project build/ios/tmc_native.xcodeproj -scheme tmc_ios -destination 'generic/platform=iOS' -jobs 4 CODE_SIGNING_ALLOWED=NO build
```

The second line checks that it compiles and links for devices without signing;
`-DTMC_IOS_TEAM=...` (automatic signing) and `port/scripts/ios_install.sh` make
an installable one. `TMC_IOS_BUNDLE_ID` changes the bundle ID
(`io.github.awkitsune.tmc-ios`).

- **Screen.** Full screen, the status bar and home indicator hidden, in any
  orientation, scaled by a whole number by default inside the safe area.
- **Input.** Gamepads as on the Mac, and a hardware keyboard through
  GCKeyboard with the Mac's keys, and the optional
  [touch controls](#touch-controls). With those off and no controller, the
  game pauses under a "Connect a controller" overlay, also when one
  disconnects.
- **Settings.** A sheet (medium and large on iPhone, a card on iPad) with the
  Mac's settings in pages: Display (filter, integer scaling, LCD response,
  the knobs), Controls, Timing (frame pacing), Game (quick save slots, reset,
  fast-forward, sound, Change ROM…) and About; no Debug tools. It opens from the
  controller's Home button (claimed from the system with
  `preferredSystemGestureState`), by holding ☰ (a press is Start, sent on
  release), with Escape, or with a tap anywhere. With a controller it is driven
  by its own polling, not UIKit focus: the D-pad moves, L and R jump between
  sections, left and right change a value (a slider by 1/100 of its range,
  faster while held), A selects, B goes back and closes. The game pauses while
  it is open.
- **Life cycle.** The game pauses when the app resigns active (and the save
  file is synced to disk) and in the background, and goes on when it returns.
  Sound is an `AVAudioSession` (playback) with RemoteIO, stopped during
  interruptions and remade at the new rate after a route change. Display sync
  uses a `CADisplayLink` at 60 Hz on its own thread.
- **Files.** `tmc.sav` and `states/` are in the app's Documents, shown in the
  Files app; the extracted game data is in `Library/Application Support/tmc_macos`
  inside the app's container (`port/src/romdata.c`), imported on first launch
  from a ROM chosen in a document picker and checked as on the Mac.
- **Self-test.** `TMC_SELFTEST=1` runs the app's checks on the device and quits
  with "N failed checks" on standard output: the renderer and LCD response
  kernel, the audio session, the ROM import path, the files, the keyboard and
  gamepad mappings, a virtual controller attaching and detaching, the frame
  rate, a save state round trip, the settings sheet driven by keys, and pausing
  on resign active. `port/scripts/ios_install.sh --selftest` installs and runs it.

### Running headless

Environment variables control the headless runner:

| Variable | Effect |
|---|---|
| `TMC_FRAMES=n` | Exit after n frames |
| `TMC_INPUT="frame:keys:duration,..."` | Hold buttons, e.g. `600:a:3,900:start+a:5` (`a b select start right left up down r l`) |
| `TMC_TRACE_TASK=1` | Print each change of the game's task/state/substate |
| `TMC_TRACE_DMA=1` | Log every DMA transfer |
| `TMC_WATCHDOG=n` | After n seconds, print a backtrace and exit (finds hangs) |
| `TMC_SAVE=path` | Back the emulated EEPROM with this file |
| `TMC_SCREENSHOT="frame,frame,..."` or `"every:n"` | Save PNG screenshots, rendered by the software renderer |
| `TMC_SCREENSHOT_DIR=path` | Where screenshots go (default `screenshots`) |
| `TMC_DUMP_IO=1` | Print display and interrupt registers with each screenshot |
| `TMC_FRAME_HASH=path` | Write `frame crc32` of every frame's picture (draws every frame) |
| `TMC_LINE_LOG="frame,frame,..."` | For those frames, print line 0's display registers and, for each later line, the ones that differ from line 0 |
| `TMC_LINE_STATS=1` | For every frame whose display registers change between lines, print `register/lines changed` |
| `TMC_PPU_COMPARE=1` | Draw every frame both line by line and as a whole, and print the frames and lines where they differ |
| `TMC_PPU_LEGACY=1` | Draw each frame as a whole before any mid-frame change (the renderer before scanline timing) |
| `TMC_WAV=path` | Record the run's sound to a WAV file (15768 Hz, or 48 kHz with `TMC_SOUND_QUALITY=hq`) |
| `TMC_ROM=path` | Distributable build: extract the game data from this ROM if needed. Development build: check the built-in data against it |
| `TMC_CACHE_DIR=dir` | Where the extracted game data lives (default `~/Library/Application Support/tmc_macos`) |
| `TMC_ROMDATA_VERBOSE=1` | Print how the game data was loaded and how long it took |
| `TMC_FRAMES=+n` | Exit n frames after the start or the loaded state |
| `TMC_LOAD_STATE=path` | Start from a save state |
| `TMC_SAVE_STATES="frame,..."` or `"every:n"` | Save states as `frame_NNNNNN.tmcs` in `TMC_STATE_DIR` (default `states`) |
| `TMC_STATE_DIR=dir` | Where states go; also enables the input scripts' `save:NAME` steps |
| `TMC_INPUT_FROM=NAME` | Start the input script after its `save:NAME` step |
| `TMC_HASH="frame,..."` or `"every:n"` | Print `hash F state S video V`: checksums of the state and of the picture |
| `TMC_RECORD_INPUT=path` | Write the keys held each frame as a `TMC_INPUT` script, to replay a run exactly |
| `TMC_ROM_WRITES=1` | Log stores into the game's read-only data and skip them instead of crashing (arm64 only: it decodes the faulting instruction) |
| `TMC_WARP=AA:RR[:X:Y]` | Once the player has control, go to that room (hex area and room) through the game's own exit transition; `TMC_WARP_ITEMS=A,B` also equips two items. Used by `port/scripts/room_smoke.sh`, see `port/notes/room-smoke.md` |
| `TMC_DUMP_OAM="frame,..."` or `"every:n"` | Print the decoded sprite table as that frame was drawn (`TMC_DUMP_OAM_ALL=1`: hidden entries too) |
| `TMC_DUMP_BG="frame,..."` | Print each layer's settings and a summary of its map (tiles, palettes, flips, per-line changes) |
| `TMC_DUMP_PAL="frame,..."` | Print the 512 palette colours (BGR555) with hashes |
| `TMC_DUMP_REGS="frame,..."` | Print the decoded display registers (DISPCNT, BGs, windows, blending, mosaic, DMA) and the registers each line changed |
| `TMC_DUMP_FILE=path` | Write the `TMC_DUMP_*` text there instead of stdout |
| `TMC_DUMP_PNG=dir` | With `TMC_DUMP_BG`: also each layer's whole map and all VRAM tiles (greyscale) as PNGs |
| `TMC_LAYER_MASK=hex` | Draw only these layers (bits 0-3 BG0-BG3, bit 4 OBJ; `1f` = all, the default) |
| `TMC_CHECK_ENTITY_LISTS=path` | Before starting, write every room's entity lists as the native loader reads them (entry counts, invalid kinds); `port/scripts/entity_list_check.py` compares them with tmc.gba |

To see the logos, title screen and file select (Start, then A to leave the title
screen):

```bash
TMC_FRAMES=900 TMC_INPUT="400:start:5,600:a:5" TMC_SCREENSHOT="every:30" build/native/tmc
```

Crashes print a backtrace with the load address. Without debug info, macOS crash
reports in `~/Library/Logs/DiagnosticReports/tmc-*.ips` name the functions too.
For memory errors, configure a separate build with `-DTMC_SANITIZE=ON` and run with
`ASAN_OPTIONS=symbolize=0`, then resolve the printed offsets offline:

```bash
atos -o build/native-asan/tmc -arch arm64 0x100026464
```

(Attaching lldb or letting ASan symbolize may wait on a macOS developer-tools
permission prompt.)

### Save states and checkpoints

A save state is everything the game can change: its RAM variables, the
emulated I/O registers, VRAM, OAM and palette, the EEPROM, the sound engine and
the frame counter. Loading one carries on exactly as the game would have from
that frame: the same input gives the same state and picture checksums
(`TMC_HASH`) as a run that never stopped. The EEPROM comes back with the state
but is only written to the save file the next time the game saves.

The checkpoints along the scripted playthrough are written by

```bash
port/scripts/checkpoints.sh          # a few seconds; rerun after changing the game
```

into `port/states` (not in git): `bedroom` (frame 8387, after the intro),
`outside_house` (13041), `hyrule_town` (15217), `town_shield` (22082) and
`hyrule_field_north` (23041), one per `save:NAME` step of
`port/scripts/play/01_new_game.txt`. To play on from one:

```bash
port/scripts/play.sh -s hyrule_town port/scripts/play/01_new_game.txt 3000 /tmp/run 300
TMC_LOAD_STATE=port/states/hyrule_town.tmcs TMC_FRAMES=+600 TMC_SCREENSHOT=every:60 build/native/tmc
```

`play.sh -s` takes a checkpoint name or a `.tmcs` path; FRAMES counts from the
state, and a script with that `save:` step carries on after it. Any script can
save its own checkpoints with `save:NAME` steps (written when `TMC_STATE_DIR` is
set). To check that a change keeps behaviour, compare `TMC_HASH=every:100`
output, or diff two states variable by variable:

```bash
port/scripts/state_diff.py a.tmcs b.tmcs
```

How it works (`port/src/state.c`): everything mutable is compiled into one
segment, `__TMCSTATE` (a forced `#pragma clang section` for the game's sources
and the port's hardware, EEPROM and sound files; `EWRAM_DATA`/`IWRAM_DATA`;
datagen's RAM symbols). States are taken at the main loop's VBlank wait, where
nothing but globals is live. The segment is full of native pointers (into the
game's data, to functions, into the segment), and each is saved as "symbol +
offset". The programs run with address randomization on like any other.

Which words are pointers comes from the pointer map, which the build makes from
the C types (`port/tools/datagen/statemap.py`, into `build/native/state_map.c`,
with its decisions in `state_map.txt`):

- pointer fields, and the integer types that hold one natively (`PtrU32`,
  `uintptr_t`), in every struct, union and array of every state variable. A
  pointer-capable word is a pointer if its value is 2^32 or more (the game's
  integers are 32-bit, the program lives above 4 GB), so a field that holds
  either (a script word, the position the lilypad keeps in `child`) is read
  exactly;
- entity slots (`gEntities`, `gAuxPlayerEntities`, the manager slots), slot by
  slot: kind and id pick the subtype struct through the entity function tables
  and the types those functions take. `statemap.txt` settles the two ids whose
  layout also depends on `type`. Any entity can hold a script context at
  `nativeScriptContext` (`ENTITY_SCRIPT_CONTEXT`); for the few subtypes that keep
  data there it counts only while the entity is scripted;
- heap blocks: each `zMalloc` call records the type it allocates
  (`include/common.h`, `port/src/heap.c`), which the map reads from the call's
  `sizeof(T)` or `(T*)` cast;
- the state variables with no C type (datagen RAM blocks the native code does
  not use, assembly symbols) are listed in `statemap.txt`. The build checks that
  the map covers every variable of the segment (`TMC_STATE_MAP_CHECK`), and a
  state never saves without it.

So a state loads exactly at any address (states of the same program at another
address), and into another build (a rebuild, the app from a headless
checkpoint, the other architecture) by symbol: variables by name, each pointer
re-pointed at its target in that build. A target that build does not have, a
pointer past its target's end, or a word of the old program's addresses the
list does not name makes the load fail with a message instead of leaving a wild
pointer. State checksums (`TMC_HASH`, the files' headers) count each pointer as
its target's name plus offset, so they are the same at every address.

`port/scripts/state_diff.py --check-pointers a.tmcs b.tmcs` tests the map: two
states of the same frame from runs at different addresses must differ exactly
in their listed pointers. The playthrough's states every 1000 frames pass.

States from before (version 1) listed every word whose value looked like a
pointer. They still load, through the map: only the listed words that can be
pointers are moved, so data that looked like one (entity kinds and ids, often)
stays as it was. A heap block in one is given the type of the only allocation of
its size; if none or several have that size, the load fails.

Determinism: runs used to differ between attempts around frames 9000 and
15000. The cause was the player's script context pointer overlapping
`PlayerEntity.unk_80` (its bits decided Link's facing in the Smith cutscene),
together with `gUnk_08016984` being zero instead of its ROM value, plus
`EncodeBCD` reading an uninitialized "r1" and a cutscene script pointer's low
bits copied as entity data. With those fixed, runs at different load addresses
give the same state and picture checksums.

### Exporting the music

`tmc-songs` renders the game's songs to WAV files with the same engine, without
running the game. `list` shows every song with its number, constant (from
`include/sound.h`), data label and music player:

```bash
build/native/tmc-songs list
```

`render` takes `all`, `bgm`, `sfx`, song numbers, ranges (`1-20`), constants
(`BGM_HYRULE_FIELD`) or labels (`bgmHyruleField`):

```bash
build/native/tmc-songs render bgm
```

```bash
build/native/tmc-songs render --hq BGM_HYRULE_FIELD 23 0x10A
```

Each song plays on its own music player until it ends. A song that loops plays
its loop twice (`--loops N`) and then fades out over 8 seconds (`--fade
SECONDS`), the way the game fades music. A song's loop is counted per track (a
backward GOTO), and the song has looped when its least-looped running track has.
Songs that neither end nor loop stop at `--max` seconds (600; SFX at 60).

Files land in `songs/authentic/` or `songs/hq/` under the current directory
(`--out DIR` to change), named by number and constant, e.g.
`025-BGM_HYRULE_FIELD.wav`, with title, album, track number and a comment in a
LIST/INFO chunk. Silent entries (the unused BGM slots) are skipped; `render all`
writes about 500 files (240 MB authentic) in about ten seconds.

| Mode | Output |
|---|---|
| default (authentic) | What the GBA plays: the engine's 8-bit mono DirectSound mix at 15768 Hz with its linear interpolation and 8-bit wraparound, plus the CGB channels, as 16-bit stereo at 15768 Hz |
| `--hq` | The same engine, envelopes and reverb, mixed in floating point at 48 kHz (`--rate HZ`) with cubic interpolation and per-frame volume ramps |

## Layout

| Path | Purpose |
|---|---|
| `port/include/port/hw.h` | Host buffers for I/O registers, palette, VRAM, OAM; DMA and BIOS entry points |
| `port/include/port/platform.h` | Frame boundary and soft reset, implemented by the platform layer |
| `port/src/hw.c` | Hardware buffers and DMA (immediate DMA works; timed DMA is latched for the renderer) |
| `port/src/bios.c` | BIOS calls in C (copies, decompression, division, affine, VBlankIntrWait) |
| `port/src/interrupts.c` | `IntrMain` replacement dispatching through the game's handler table |
| `port/src/headless.c` | Headless platform and process entry; `TMC_FRAMES=n` stops after n frames |
| `port/src/state.c`, `port/include/port/state.h` | Save states: the `__TMCSTATE` segment to a file and back |
| `port/tools/datagen/statemap.py`, `statemap.txt`, `port/include/port/state_map.h` | The save states' pointer map, from the C types |
| `port/src/heap.c` | `zMalloc` call sites, so save states know each heap block's type |
| `port/scripts/checkpoints.sh`, `state_diff.py` | Checkpoint states along the playthrough; comparing two states |
| `port/src/frame.c` | Frame boundary shared by both platforms: latch keys, raise VCOUNT and VBLANK |
| `port/src/apple/game.c` | App platform layer (macOS and iOS): game thread, 59.73 Hz pacing, frame hand-off |
| `port/src/macos/app.m` | Window, menus, toolbar, subtitle, full screen, save location |
| `port/scripts/room_names.py`, `port/src/apple/roomname.c` | Room names for the window's subtitle and the iOS settings |
| `port/src/apple/renderer.m`, `shaders.metal` | Metal presentation and filters (shaders compiled at launch) |
| `port/src/apple/input.m` | Keyboard and GameController input (GCKeyboard on iOS) |
| `port/src/apple/settingsstore.m` | The settings' keys and the saved filter knobs |
| `port/src/macos/settings.m` | Settings window (tabs that scroll, fitted to the screen) |
| `port/src/apple/displaysynccore.m`, `port/src/macos/displaysync.m` | Display refresh rate and present times, for display-sync pacing (CVDisplayLink) |
| `port/src/apple/latency.c` | Opt-in input latency log (`TMC_LATENCY_LOG`) |
| `port/src/macos/selftest.m` | `TMC_SELFTEST` scripted run with PNG captures |
| `port/src/macos/romsetup.m` | First launch: ROM open panel, progress window, errors, Change ROM… |
| `port/src/ios/` | The iOS/iPadOS app: game screen, settings sheet, ROM import, audio session, CADisplayLink, self-test |
| `port/scripts/ios_install.sh` | Builds the iOS app signed with your team and installs it on a connected device |
| `port/src/romdata.c` | ROM checks, extraction to `gamedata.bin`, filling the game data from it |
| `port/tools/datagen/provenance.py` | Tells asset bytes (from `baserom.gba`) from source-written ones |
| `port/scripts/asset_overlap.py` | Lists what a program shares with the extracted asset files |
| `port/src/data_glue.c` | Code embedded in data, demo-only symbols, the save header |
| `port/src/asm/` | C versions of every function in `asm/src/*.s` |
| `port/src/eeprom.c` | Emulated save EEPROM (replaces `src/eeprom.c`) |
| `port/src/sound/` | Native sound: `asm/lib/m4a_asm.s` in C, the mixer, CGB channels, WAV output (see [Sound](#sound)) |
| `port/src/apple/audio.c` | Core Audio output with drift control (RemoteIO on iOS) |
| `port/src/tools/tmc_songs.c` | The song exporter |
| `port/src/trace.c` | Game-state tracing for the headless runner |
| `port/src/gfxdebug.c` | Graphics debugging: hardware snapshots per frame, sprite/layer/register decoders, `TMC_DUMP_*` text (see [Graphics debugging](#graphics-debugging)) |
| `port/src/macos/debugwindows.m` | The Debug menu's Tiles, Sprites, Layers, Palettes, IO Registers and Shader Buffers windows |
| `port/src/ppu.c` | Software GBA renderer (BG modes 0-2, sprites, windows, blending, mosaic), drawn a scanline at a time, and PNG output |
| `port/src/frame.c` | One frame of display timing: per-line VCOUNT/HBlank interrupts and HBlank DMA, then VBlank (see [Mid-frame effects](#mid-frame-effects)) |
| `port/src/tools/scanline_test.c` | Checks of per-line scroll, affine and window effects without the game |
| `port/src/tools/ppu_test.c` | Checks of BG and OBJ mosaic (with windows, blending, the OBJ window) and tile fetches without the game |
| `port/tools/datagen/` | GBA data objects → native arm64 assembly (see below) |
| `port/scripts/link_report.py` | Classifies unresolved symbols into the remaining worklist |

Header hooks (all `PLATFORM_NATIVE`-only): `gba/defines.h` and `gba/io_reg.h`
point `PLTT`/`VRAM`/`OAM`/`REG_BASE` at the host buffers; `gba/macro.h` routes
`DmaSet`/`DmaStop` to the port; `gba/syscall.h` routes `SystemCall`;
`global.h` disables GBA struct-size asserts; `gba/types.h` adds `PtrU32`, an
integer field that holds a pointer (`u32` on GBA, `uintptr_t` natively);
`gba/defines.h` adds `GBA_PTR(addr)` for fixed GBA addresses in C code, mapped
natively to the host buffer or variable at that address.

## Phase 0 results

These are historical results from the initial port investigation, not the current build status.

**Compile:** 608 of 619 C files build for arm64. The other 11 (`cutscene.c` and
ten `worldEvent*.c`) are listed in `TMC_NATIVE_PENDING`: their static
`EntityData` tables store pointers in the `u32 spritePtr` field, which cannot be
a constant initializer with 64-bit pointers.

Source changes needed to get there, all verified to keep `tmc.gba` matching:
five GCC lvalue casts rewritten in standard C, five `static` definitions that
conflicted with `extern` declarations, and the m4a `swi 0x2A` routed through
`SystemCall`.

**Link:** 1348 unresolved symbols, all accounted for:

| Count | Defined in | Native plan |
|---:|---|---|
| 539 | `data/*.s` tables | Main Phase 1 decision, see below |
| 507 | songs generated from assets | Phase 4 (audio) |
| 130 | `linker.ld` fixed EWRAM/IWRAM addresses | 121 become ordinary globals; 9 `ram_*` IWRAM copies alias their ROM functions |
| 117 | `asm/src/*.s` | Rewrite in C; 23 are interworking veneers that become aliases |
| 29 | `asm/lib/m4a_asm.s` | Phase 4, C mixer |
| 12 | `asm/lib/libagbsyscall.s` (BIOS) | Phase 1: `CpuSet`, `Div`/`Mod`, `LZ77UnComp*`, affine sets, etc. in C |
| 14 | the 11 pending C files | Unblocked by the data decision |

Remaining asm functions by file: `code_08003FC4.s` 25, `veneer.s` 23,
`code_080043E8.s` 16, `code_08001A7C.s` 13, `player.s` 12, `script.s` 10,
`enemy.s` 9, others 10.

**64-bit hazards** flagged by the compiler (the Phase 1 worklist, visible in the
normal build output): 68 pointer→int casts, 46 int→pointer casts, 17 more via
`void*`, plus 24 raw GBA address literals such as `(SaveHeader*)0x2000000`.

## Game data (phase 1)

`port/tools/datagen` converts the assembled GBA data objects
(`build/USA/data/**/*.o`, about 13.7 MB with 14k pointers) into arm64 assembly
in `build/native/data`:

- **Types** come from the game sources through libclang. GBA layouts follow
  agbcc's rules (4-byte pointers, structs rounded up to 4 bytes) and match all
  of the decomp's struct-size asserts.
- **Pointer-free data** (98% of the bytes) is copied byte for byte: each section
  is reproduced in one piece so labels keep their exact relative offsets.
- **Data with pointers** is re-laid out in a separate section: pointer tables
  are widened to 8 bytes, typed tables are converted field by field (bitfields
  and unions included), and pointers into the middle of tables are remapped.
- **Script bytecode** keeps its bytes; each pointer slot holds a tagged index
  into a per-object table of native pointers (`script_tag.h` has the tag).
- **RAM variables** placed by `linker.ld` become native storage (`ram.s`),
  with shared addresses kept as aliases.
- **Songs** stay raw: their track data holds absolute ROM addresses without
  relocations. `sound_map.s` (`gSoundRomMap`) maps every GBA address in the
  sound data to its native counterpart, linearly for byte-for-byte data and by
  element for the re-laid-out voicegroups, and the sound engine translates
  addresses through it.

In a distributable build (`--no-bake`) every asset byte keeps its place but is
left zero. The run tables in `__DATA,__tmcruns` say
where it goes. `rom_manifest.s` holds the ROM extents the data file stores,
and `rom_manifest.txt` lists every run.

About 1,100 tables have no C declaration; `hints.txt` types them, mostly by the
data macro they start with. `build/native/data/notes.txt` lists C declarations
that need a pointer type natively (e.g. pointer tables declared as `u32[]`).

## Status

Version 1.0 (`v1.0`; release notes in [port/dist/release-notes-1.0.md](port/dist/release-notes-1.0.md)). The game is playable on the Mac and on iPhone and iPad, and has been played
by hand from a new game to the end credits on the Mac. Every function in `asm/src` has a C version, the BIOS calls
are in C, the save EEPROM is emulated, and sound is a C version of the m4a
engine. Rendering and sound follow the hardware (`port/notes/ppu-audit.md`,
`port/notes/audio-audit.md`), with one known difference: every sprite on a
line is drawn, where the GBA drops sprites on very crowded lines (the
flicker in the Mazaal and Vaati fights).

Crashes that playtesting finds are, nearly always, places where the decompiled
code reads or writes memory in a way the GBA tolerates and a 64-bit host does
not: NULL pointers that read BIOS bytes, pointers stored in 32-bit fields,
fields reached by GBA byte offset, division by zero. Each fix is native-only
and keeps `tmc.gba` matching. `port/notes/native-hazards.md` lists the classes,
every fix and the cases still open; `port/notes/room-smoke.md` covers the
every-room warp test (`port/scripts/room_smoke.sh`).

Checks after any change: `make -j4` prints `tmc.gba: OK`, the headless
new-game playthrough (`port/scripts/play/01_new_game.txt`, 24000 frames) ends
on the same frame hash, and the room smoke test reaches all 576 valid rooms.
The smoke test does not cover every story state or gameplay interaction; see
[its coverage limits](port/notes/room-smoke.md#what-the-smoke-test-cannot-see).

Not done yet: the shared app is ad-hoc signed. See [improvements](port/notes/improvements.md)
for this work.

## Mid-frame effects

The game makes per-line effects with one mechanism: a table of register values
for each line, built by the game logic in the double-buffered `gUnk_02017AA0`
(swapped by `gUnk_03003DE4[0]`) and handed to `SetVBlankDMA`. The VBlank
handler (`PerformVBlankDMA` in `src/interrupts.c`) copies line 0's entry and
arms DMA 0 to copy the next entry at every HBlank. No HBlank interrupt is used;
the VCOUNT interrupt (line 80) only runs the sound mixer.

| Effect | Register | Code | Where to look |
|---|---|---|---|
| Iris / circle wipe | WIN0H | `sub_0801E1EC` from `fade.c` (`FADE_IRIS`, script `SetFadeIris`) | Cutscene transitions, e.g. the intro before Link wakes up (headless playthrough frames 5468-5577) |
| Dark-room spotlight | WIN0H | `sub_0801E160` from `manager/lightManager.c` | Dark rooms (circle of light around Link) |
| Minish portal close-up | WIN0H | `object/minishPortalCloseup.c` | Shrinking at a Minish portal |
| White triangle | WIN0H | `sub_0801E49C` from `object/whiteTriangleEffect.c` | Scenes that spawn `WhiteTriangleEffect` |
| Heat haze | BG3HOFS | `manager/steamOverlayManager.c` | Cave of Flames |
| Light rays | BG3HOFS | `manager/lightRayManager.c` | West Minish Woods, the barrel Minish house |
| Vaati appearing | BG3HOFS (affine table) | `manager/vaatiAppearingManager.c` | Vaati appearing / disappearing |
| Rolling barrel | BG2PA..BG2Y | `manager/rollingBarrelManager.c` | Deepwood Shrine, inside the barrel |
| BG3 band | BG3CNT | `menu/pauseMenuScreen6.c` | Pause menu screen 6 (BG3 priority changes for a band of lines) |

`HwRunFrame` (`port/src/frame.c`) plays out a frame like the hardware: for each
visible line it sets VCOUNT and raises the VCOUNT interrupt on its line, draws
the line from the registers as they are, then raises HBlank and runs HBlank DMA
(`HwDmaHBlank` in `hw.c`); after line 159 it runs VBlank DMA and the VBlank
interrupt. Line 0 is drawn with the copy made at VBlank and line n with the
entry the DMA wrote after line n-1, as on hardware. The affine reference points
are latched at the start of the frame, step by PB/PD every line and reload when
DMA writes BG2X/BG2Y (BG3X/BG3Y). Drawing line by line costs the same as a whole
frame: each line reads its registers once.

Checks, with no screenshots:

```bash
cmake --build build/native --target scanline_test && build/native/scanline_test
```

```bash
cmake --build build/native --target ppu_test && build/native/ppu_test
```

```bash
TMC_FRAMES=24000 TMC_INPUT=@port/scripts/play/01_new_game.txt TMC_PPU_COMPARE=1 TMC_LINE_STATS=1 build/native/tmc
```

`scanline_test` checks every pixel of per-line scroll, affine and window tables
(it fails with `TMC_PPU_LEGACY=1`). `ppu_test` draws scenes with and without
mosaic and checks every pixel against the block pattern (see
`port/notes/ppu-audit.md` for what the renderer does and does not cover). In the playthrough, `TMC_PPU_COMPARE` reports
exactly the 74 frames where `TMC_LINE_STATS` sees WIN0H change between lines (the
intro's iris wipes) and no others. Compare checksums of different builds with
care: an NPC in Smith's house (area 0x22, around frame 8900) moves differently
depending on leftover stack contents, so its frames change between builds
whatever the renderer does; `TMC_PPU_COMPARE` compares within one run.

### Performance window

**Debug → Performance** shows the frame number, game FPS, rolling game-thread
work and its PPU breakdown, plus separate CPU and GPU presentation timings.
The stacked bar is scaled to the current frame interval (including fast-forward),
or to total work if it exceeds that interval. Game-thread times are elapsed wall
time, not CPU usage: scheduling and instrumentation can affect them. The PPU
breakdown includes the cost of collecting the samples. Presentation runs
asynchronously and can overlap game-thread work, so its times are not added to
the bar. GPU timing is shown only when Metal supplies it. Reset clears the
samples; collection stops when the window closes.

**Settings → Rendering → Draw the game on the GPU** (on by default) has the
Metal PPU draw the game picture; turn it off to draw it in software. The CPU
still runs the game and the display timing, and records each scanline's
hardware state instead of drawing it; the GPU then draws the frame from that
record. In the Performance window, the recording appears under Other PPU and
GPU execution includes the PPU and display filters. Switching while paused
takes effect at the next game frame; switching back to software redraws the
paused picture. How it works and the offline pixel comparison tool are in
[GPU PPU](port/notes/gpu-ppu.md).

## Graphics debugging

The app's Debug menu opens emulator-style viewers, each a resizable window that
remembers its frame and options and refreshes about 12 times a second:

| Window | Shortcut | Shows |
|---|---|---|
| Tiles | ⌃⌘1 | VRAM as tiles, 32 per row: BG char blocks 0-3, one block, OBJ tiles or all of it, 4bpp or 8bpp, in greyscale or any of the 16 BG/OBJ palettes, zoomed. Hover: tile number, address, char block, pixel value |
| Sprites | ⌃⌘2 | All 128 OAM entries: position, shape/size, tile, palette, priority, mode, affine/double-size, matrix, mosaic, depth, flips, hidden. The selected one is previewed (unrotated); "Highlight in game window" outlines its box in the game |
| Layers | ⌃⌘3 | BG0-BG3 as whole maps (text or affine) with the visible area outlined at the current scroll (per line when the scroll changes between lines; the screen's corners for affine layers) and the layer's settings. Hover: map entry. The Draw checkboxes hide BG0-BG3/OBJ in the game while the window is open |
| Palettes | ⌃⌘4 | The 256 BG and 256 OBJ colours. Hover: index, address, BGR555 and RGB |
| IO Registers | ⌃⌘5 | DISPCNT, DISPSTAT, BG control/scroll/affine, windows, BLDCNT/BLDALPHA/BLDY, mosaic, interrupts and the latched DMA channels, decoded; then the registers that changed on each line |
| Shader Buffers | ⌃⌘6 | Any texture of the Metal pipeline, live: the uploaded frame, the LCD response history (linear) and output, the filter pass's input, its output before colour correction and gamma, the CRT scanline beams, the LCD grid/subpixel or CRT grille mask, the bloom terms (spot brightness, beam width or LCD gap, scanline strength), the colour-corrected output before gamma, and the final drawable. Zoom, channel isolation (R/G/B/A/luma), exposure and false colour; hover shows the stored value (float16 or 8-bit) |

How they get their data, at no cost while none is open:

- **Hardware viewers.** While one is open, the game thread copies VRAM, OAM,
  palette, I/O and the latched DMA channels into a snapshot just before it draws
  each frame, and `HwRunFrame` copies the display registers of every line into it
  (`gHwLineCapture`, `port/include/port/hw.h`); then it publishes the snapshot
  under a lock (`FrontendCopyGfxSnapshot`, `game.c`). The windows decode it on
  the main thread with `port/src/gfxdebug.c`, the same code as the headless
  dumps. While paused, opening a viewer takes one snapshot of the hardware as it
  stands (without per-line registers).
- **Layer mask.** `PpuSetLayerMask` (`ppu.c`) leaves hidden layers out of the
  picture (the OBJ window still applies); with all layers on, the picture is the
  same as without it.
- **Shader Buffers.** The renderer blits the chosen texture into a shared
  readback texture every fourth display refresh, one copy at a time, and only
  while the window is open. The filter pass is a single fragment shader, so its
  stages are captured by drawing it a second time into a float16 texture of the
  viewport's size with a debug stage selected (`debugStage` in `shaders.metal`);
  the picture is drawn exactly as before. There are no separate blur or bloom
  textures: CRT bloom is the beam width worked out per pixel, shown as a buffer.

Headless, the same decoders print text for chosen frames, as the hardware was
when that frame was drawn. For example, in Hyrule Town on the playthrough:

```bash
TMC_FRAMES=16001 TMC_INPUT=@port/scripts/play/01_new_game.txt TMC_DUMP_OAM=16000 TMC_DUMP_BG=16000 TMC_DUMP_REGS=16000 TMC_DUMP_PAL=16000 build/native/tmc
```

```
frame 16000 oam: 1D mapping, 53 of 128 entries on screen (hash c36dda51d690a4a7)
  obj attr0 attr1 attr2     x    y  size  shape   tile pal pri mode    flags  affine
    0  4004  80ad  110e   173    4 32x16 wide   0x10e   1   0 normal  ......  -
```

`TMC_DUMP_PNG=dir` adds each layer's map as a PNG; with `TMC_LAYER_MASK=04`
(BG2 only) the frame's screenshot is exactly that map cropped at the scroll.

## Sound

The game's m4a engine runs natively: `src/gba/m4a.c` as it is (with
`PLATFORM_NATIVE` fixes for 64-bit pointers, hardware setup and GBA addresses in
song headers) and `asm/lib/m4a_asm.s` rewritten in C in `port/src/sound`:

| File | Contents |
|---|---|
| `m4a_engine.c` | The sequencer (`MPlayMain`), song commands (`ply_*`), note allocation, channel chains |
| `m4a_mixer.c` | `SoundMain` and the DirectSound mixer (`SoundMainRAM`), authentic and HQ |
| `psg.c` | The CGB channels (two squares, wave, noise) synthesised from the sound registers `CgbSound` writes |
| `m4a_addr.c` | `M4aGbaPtr`: GBA addresses in song data to native pointers through `gSoundRomMap` |
| `wav.c` | WAV writer and `TMC_WAV` |

Once per frame (the mid-frame VCOUNT interrupt, as on the GBA) `SoundMain` runs
the music players and `CgbSound`, mixes the DirectSound channels, synthesises the
CGB channels for the same frame and hands 264 stereo samples (at the engine's
15768 Hz) to the platform: the app's Core Audio output, `TMC_WAV`, or the song
exporter. TMC's engine mixes DirectSound in mono; only the CGB channels are
panned.

In the app the samples go through a lock-free ring to an output unit whose
render callback resamples them to the device rate. The game keeps its own
59.73 Hz timer (in display sync it runs at the display's rate instead, and the
resampler is told so); the resampling ratio follows the ring's fill level (about
50 ms) by at most 0.5%, so the two clocks never drift apart. Fast-forward drops audio
once a quarter second is buffered; pausing fades out. `TMC_AUDIO_WAV=path`
records what the app actually sent to the device and prints underrun, drop and
drift statistics on quit.

Known differences from hardware:

- The CGB channels are synthesised from the registers' state once per frame
  (as `CgbSound` leaves it), not from each write; m4a only writes them once per
  frame, so this matters only for the order of writes within a frame.
- CGB output is box-filtered (oversampled to ~96 kHz) rather than produced by
  the GBA's PWM DAC; the authentic mode does not model the DAC's 9-bit
  resampling at 65.5 kHz or the speaker's response.
- The final mix is DC-blocked (5 Hz high-pass), like the GBA's output coupling.
- Key-split voices (`voice_keysplit`) pick the note's key directly; TMC has none.
