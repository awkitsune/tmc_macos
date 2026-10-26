# Audio audit: `port/src/sound` and the output against GBATEK

Code audited: `port/src/sound/psg.c` (the four CGB channels and the DAC),
`port/src/sound/m4a_mixer.c` (SoundMain and the DirectSound mixer),
`port/src/sound/m4a_engine.c` (the sequencer), and the output in
`port/src/apple/audio.c`. References: GBATEK "GBA Sound Controller", mGBA's
`src/gba/audio.c` where GBATEK says nothing (the sweep edge cases, the PSG
scale), and the m4a driver in this repo (`asm/lib/m4a_asm.s`, `src/gba/m4a.c`,
`include/gba/m4a_internal.h`).

## What the game uses

`TMC_SOUND_TRACE=path` (new, `psg.c`) writes, on exit, each frame's mixing
registers, the registers at every channel trigger, the sweep, length and
envelope events, the sub-samples the DAC clips, and which CGB channels were
audible in each frame. Two runs:

- the 24000-frame playthrough (`port/scripts/play/01_new_game.txt`);
- `tmc-songs render --loops 1 --fade 1 --max 120 all`: all 503 songs, 208167
  frames. This covers the music and SFX the playthrough never reaches.

The game writes the sound registers only from the m4a driver (`src/gba/m4a.c`:
SoundInit, m4aSoundMode, MPlayExtender, CgbSound, CgbOscOff). No other code in
`src/` touches them.

| | playthrough | all songs |
|---|---|---|
| NR50 (SOUNDCNT_L volumes) | 77h every frame | 77h every frame |
| SOUNDCNT_H | 0B0Eh every frame | 0B0Eh every frame |
| SOUNDCNT_X master enable | on every frame | on every frame |
| SOUNDBIAS | 4200h every frame | 4200h every frame |
| triggers ch1 / ch2 / ch3 / ch4 | 1275 / 1498 / 320 / 394 | 2859 / 1857 / 3718 / 7757 |
| frames with ch1 / ch2 / ch3 / ch4 audible | 1102 / 1437 / 419 / 183 | 4571 / 1143 / 39491 / 11111 |
| duty codes (ch1 and ch2) | 0, 1, 2 | 0, 1, 2 |
| ch1 sweep frequency writes / overflow cut-offs | 394 / 1 (in a step) | 6664 / 9 (in a step) |
| NRx2 envelope (ch1, 2, 4) | down, periods 1 and 2 | down, periods 1, 2, 5; up, period 1 |
| length counter enabled at a trigger | never | never |
| NR30 | 80h only | 80h only |
| NR32 codes in frames with ch3 on | 0, 50%, 25% | 0, 100%, 50%, 25%, forced 75% |
| distinct wave RAM contents | 2 | 4 |
| NR43 | 7- and 15-bit, r 0-7, s 0-5 | 7- and 15-bit, r 0-7, s 0-9 |
| DAC sub-samples clipped (after the fixes) | 70 of 88,704,000 | 0 |

SOUNDCNT_H 0B0Eh means: PSG at 100%, DirectSound A and B at 100%, A to the right
and left outputs, B to neither, and A's FIFO reset bit set (natively nothing
clears it). SOUNDBIAS 4200h is bias level 100h at 8-bit/65.536 kHz resolution
(m4aSoundMode `SOUND_MODE_DA_BIT_8`).

## Fixed

1. **PSG level: twice too quiet against DirectSound.** GBATEK: "Each of the two
   FIFOs can span the FULL output range (+/-200h). Each of the four PSGs can span
   one QUARTER of the output range (+/-80h)." DirectSound was right (an 8-bit
   sample is 4 DAC units at 100%). But a CGB channel came out at
   level × (NR50 volume + 1) × ratio, which is at most 15 × 8 = 120 units, an
   eighth of the range. It is now level × 2 × (volume + 1) at 100%, at most
   240 units. That is a quarter, and the same as mGBA's
   `(level << 3) * (1 + vol) >> (4 - volume)`.
2. **DAC clipping.** GBATEK: the six channels are summed, the bias is added, and
   the result is clipped to 0..3FFh. The port used to clip only at the very end,
   at ±512 units after the DC filter, so a PSG + DirectSound peak that clips on
   hardware went through. `PsgRender` now adds the PSG to the DirectSound output
   in each sub-sample and clips the sum to [-bias, 3FFh - bias]. The bias is
   read from SOUNDBIAS. The final ±1.0 clamp after the DC filter stays as a
   guard.
3. **SOUNDBIAS was 0 natively.** The BIOS leaves it at 0200h, and
   RegisterRamReset's sound reset sets it back to that. Natively gHwIo started
   zeroed, so the register read 4000h after SoundInit, a bias level of 0.
   `RegisterRamReset` (`port/src/bios.c`) now does the sound reset (bit 6):
   0x060-0x0A7 cleared, SOUNDBIAS = 0200h. `tmc-songs` uses it instead of its
   own memset. A save state from an older build still holds bias 0, which would
   now clip every negative sample. `StateLoad` therefore sets such a state's
   bias level to 100h.
4. **Noise LFSR in GBATEK's form.** The port ran the Game Boy (Fibonacci)
   register from 7FFFh and output its inverted bit 0. GBATEK's generator shifts
   right, outputs the bit shifted out, XORs 6000h/60h when it is 1, and starts
   at 4000h/40h. A simulation of both over three periods gives the same
   sequence, inverted and 15 (7-bit: 7) steps out of phase, with periods 32767
   and 127. Inverting a ±level square changes nothing after the AC coupling
   except a mean of 1/127 of a step. The port now uses GBATEK's form, so the
   samples match it exactly from the trigger on.
5. **DirectSound A from the registers.** The mixer copied the mono mix to both
   outputs unconditionally. It now takes SOUNDCNT_H bit 2 (50/100%) and bits 8
   and 9 (right/left), and silences everything when SOUNDCNT_X bit 7 is clear.
   With the game's 0B0Eh / 80h this is exactly the old behaviour: A at 100% to
   L and R. DirectSound B is never fed (m4a's mono mode writes FIFO A only) and
   is not routed, so it stays out. TMC stays mono.

## Checked and correct

PSG (`psg.c`):

- Square duty waveforms 12.5/25/50/75% (00000001, 10000001, 10000111,
  01111110), and the square frequency 131072/(2048-n) Hz. The duty position is
  not reset on a trigger, as on hardware.
- Wave channel: 32 four-bit samples per period at 65536/(2048-n) Hz
  (2097152/(2048-n) Hz per sample). Samples are read high nibble first. Volume
  codes 0/100/50/25% are shifts 4/0/1/2, and the forced 75% is
  `sample * 3 >> 2`, as mGBA does. Both 100% and 75% occur in the songs.
- Wave RAM banks. m4a writes a new wave with NR30 = 40h (playing bank 1, so the
  CPU writes bank 0), then sets NR30 = 0 and 80h (playing bank 0, 32-sample
  mode). The bank written is the bank played, so one 16-byte view of the
  registers gives what hardware gives. NR30 is never anything but 80h while
  channel 3 plays.
- Noise frequency 524288 / r / 2^(s+1) Hz with r = 0 counting as 0.5, and the
  7/15-bit width. s ≥ 14 is held (GBATEK: prohibited); the game never uses it.
- Length counter: 64 - n (wave: 256 - n) at 256 Hz, only with NRx4 bit 6. On a
  trigger an expired counter reloads to the maximum. The channel's NR52 bit
  clears when it runs out, which CgbSound reads. The game never enables a
  length (m4a sets `n4` bit 6 only for a nonzero `length`, and none of the 503
  songs has one), so these paths are unexercised.
- Envelope: initial volume, direction, period n/64 s, and period 0 = off, on
  frame-sequencer step 7. Retriggers reload volume and timer. m4a retriggers on
  every volume change and runs its own envelope in step with the hardware one
  (`c15`, twice every 15 frames).
- Ch1 sweep: period n/128 Hz on steps 2 and 6, and X ± X >> shift. On a trigger:
  shadow = frequency, the sweep is enabled if the period or the shift is
  nonzero, and an overflow check runs when the shift is nonzero. In a step:
  overflow (> 2047) turns the channel off. Otherwise, with a nonzero shift, the
  new frequency is written to NR13/NR14 and checked again. Period 0 reloads the
  timer with 8 and does nothing. The game uses it (the cut-offs in the table).
- DAC enable: NRx2 & F8h (ch3: NR30 bit 7). Off stops the channel.
- NR51 per-channel L/R enables, NR50 volumes 0-7 as ×(1-8), the
  25/50/100% PSG ratio, and the NR52 master enable.

Mixing and output (`m4a_mixer.c`, `audio.c`):

- Engine rate: m4aSoundMode `SOUND_MODE_FREQ_15768`, 264 samples per frame.
  Timer 0 reload -(280896/264) = -1064 gives 16777216 / 1064 = 15768.06 Hz on
  hardware. The port outputs `pcmFreqency` = 15768 Hz, 0.0004% slow.
- SoundMain order: MPlayMainHead, CgbSound, then the mix. The PCM ring slot is
  `pcmBuffer + samples * (pcmDmaPeriod - (pcmDmaCounter - 1))`, as SoundMain in
  the assembly computes it.
- Reverb (the mono SoundMainRAM path): `(out[i] + next[i]) * reverb >> 8`, plus
  1 when bit 7 is set, where `next` is the following ring slot, or the start of
  the ring when the counter is 2. This matches `sub_080AF3B0` instruction by
  instruction.
- The channel envelope (attack/decay/sustain/release, pseudo-echo) matches the
  assembly at `_080AF442`-`_080AF4D8`. That includes the 8-bit store of the
  final `(left + right) * env >> 9`.
- The sample loop: `(sample * vol) >> 8` added with 8-bit wraparound, through
  the byte-lane `mul`/`bic`/`ror` trick. Linear interpolation
  `s0 + (fw * delta >> 23)` with step `divFreq * frequency`. Fixed-rate samples
  one per output sample. The loop wrap arithmetic matches `_080AF5BC`.
- DirectSound output: an 8-bit mono sample × 4 DAC units = ±512 (GBATEK's full
  range).
- DC removal: a 5 Hz one-pole high-pass, standing in for the output coupling
  capacitor. Not in GBATEK. It removes the unipolar PSG offset and the 8-bit
  mixer's rounding offset. Then the sum is scaled by 1/512.
- `audio.c` is only transport: an SPSC ring, a Catmull-Rom resample to the
  device rate with ±0.5% drift correction, and fade-out on underrun. It changes
  no levels or channels.
- The HQ path (`TMC_SOUND_QUALITY=hq`) is a deliberate enhancement and not
  hardware. It now gets the same DAC clipping.
- `m4a_engine.c` (sequencer, ply_* commands, note allocation, LFO) touches no
  hardware. GBATEK doesn't cover it. Its sound registers are written only
  through CgbSound in `src/gba/m4a.c`, which compiles unchanged natively.

## Not implemented, not used by the game

- **SOUNDBIAS resolution (bits 14-15).** At the game's 8-bit setting the
  output keeps 10-bit values in steps of 4. DirectSound samples are multiples
  of 4 (× 4), and a CGB level is a multiple of 16 at NR50 = 77h, so 8-bit loses
  nothing on hardware. Applying it here was tried. It changed 52,114
  sub-samples, all of them noise levels that the box filter had averaged within
  a sub-sample. Rounding those is wrong, so the resolution is left out. The
  PWM rate itself (65.536 kHz) is below what the box filter's sub-samples
  resolve: 7 per output sample, about 110 kHz.
- **64-sample wave mode and playing bank 1** (NR30 bits 5 and 6 while ch3
  plays). NR30 is always 80h.
- **Duty 75%** is implemented, but no voice in the game uses it.
- **Length counter** (see above): implemented, never enabled.
- **Sweep "negate then clear negate" quirk** (Game Boy obscure behaviour: after
  a calculation in decrease mode, clearing NR10 bit 3 turns the channel off).
  m4a writes NR10 only right before a trigger.
- **NR52 bit 7 off clearing the PSG registers.** The game never turns it off.
- **Register writes between frames.** CgbSound writes everything once per
  frame and the PSG picks it up at the next render. Hardware would apply each
  write when it happens, but they all happen within one CgbSound call.
- **DirectSound B, FIFO timing and DMA.** m4a feeds FIFO A only. The mixed ring
  slot is handed over directly, without a FIFO, a timer or DMA1. That costs
  timing only, not samples.
- **SOUNDCNT_H ratio 3 (prohibited)** outputs no PSG.

### A CGB channel can be panned

`CgbPan` in `src/gba/m4a.c` hard-pans a CGB channel when one side's volume is at
least twice the other's: NR51 gets 0Fh or F0h masked to the channel. That is
the driver's own behaviour, and hardware does the same; it is not DirectSound
routing. Of the 503 songs, only `115 SFX_SECRET_BIG` uses it. Its channel 2
plays left only (NR51 21h/31h): RMS L 5266, R 3719. The playthrough never plays
it: L = R in every sample of its WAV. Silent CGB channels (both volumes 0) also
get 0Fh, harmlessly. Nothing was changed here.

## Checks

WAVs from `TMC_WAV` (16-bit, 15768 Hz) over the 24000-frame playthrough. Two
runs of the same binary are byte-identical, so every difference below comes from
the change. Each fix was measured on its own, cumulatively, with temporary
switches (not committed). Counts are per output channel (L and R are identical
in every case), out of 6,336,000 samples. Deltas are in 16-bit LSBs: 1 DAC unit
= 64 LSB.

| step | samples differing | max delta | where |
|---|---|---|---|
| restructure only (PSG summed per sub-sample) | 183 | 1 | float rounding, 165 frames |
| 1. PSG ×2 | 1,155,044 | 11,333 | 5552 frames: 2295 with a CGB channel audible, 3257 within 36 frames after one (the DC filter's tail) |
| 2+3. clipping with bias 100h | 3,183 | 2,366 | frames 320-339 and 18996-19008: 70-78 clipped sub-samples, plus the DC filter's decaying tail (1475 of the differences are 1 LSB, 1395 are 2-4) |
| 4. noise LFSR | 104,439 | 11,856 | 486 frames: 207 with a CGB channel audible, 279 within 26 frames after one |
| 5. DirectSound from SOUNDCNT_H/X | 0 | 0 | byte-identical |
| all, against the old build | 1,149,676 | 11,669 | as above; peak 28138 → 31488, no sample at full scale |

`tmc-songs render`, songs 1-12, old maths against new: the eight songs without
CGB channels are byte-identical. 005, 009, 011 and 012 (CGB parts) change, with
a max delta of 3584.

- `PATH=$PWD/.venv/bin:$PATH make -j4`: `tmc.gba: OK`.
- `cmake --build build/native -j4 --target tmc --target tmc_songs` builds
  (the state-map check passes).
- Playthrough: `23999 89eb5912`, and all 24000 frame CRCs identical to the old
  build.
- A frame-3000 save state written by the old build (bias 0), loaded into the
  new one (`TMC_LOAD_STATE`, 600 frames traced): SOUNDBIAS reads 4200h in every
  frame and nothing clips.
