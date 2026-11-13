# Adrift - Custom Firmware for M-VAVE FM-1

**Adrift** is a standalone 6-voice polyphonic bare-metal synthesizer firmware for the **M-VAVE FM-1** pocket synthesizer (`JieLi AC791N` dual-core `pi32v2` SoC), built on [`hugelton/felucca-bsp`](external/felucca-bsp) and verifiable without physical hardware via both a `<1s` native macOS `SDL2`+`CoreMIDI` simulator and [`joakimeriksson/fm1-emu`](external/fm1-emu).

Adrift offers 16 voices, assignable across 6 multi-timbral voice groups. Each voice is a virtual
analog morphing oscillator and a 1024-sgage Bucket Brigade Device resonator, with 2 envelopes
(one post VCO, and one post resonator).

---

## Quick Start

```sh
# 1. One-time environment verification / setup (macOS Apple Silicon)
./tools/setup_env.sh

# 2. Run automated verification suite
./build.sh test

# 3. Build & launch fast-iteration native macOS SDL2 + CoreMIDI simulator (<1s build)
./build.sh sdl
./build/sdl/adrift

# 4. Build flashable M-VAVE FM-1 hardware firmware package (build/fw/adrift.fwsc)
./build.sh fw

# 5. Boot actual pi32v2 .fwsc firmware in cycle-accurate AC791N hardware emulator (live audio + MIDI)
./build.sh emu

# 6. Run strict cycle-accurate 16-voice multi-group pi32v2 CPU & audio verification benchmark
./build.sh bench

# 7. Build static website, WebAssembly emulator & browser flasher bundle (build/web/)
./build.sh web

# 8. Build and publish the website to the gh-pages branch
./tools/deploy_gh_pages.sh
```

---

## Per-Voice Signal Flow (`×16` Hardware Voices Across `G1..G6` Voice Groups)

```text
[Micro-ACB Morphing VCO] ──┐
(Sine->Tri->Ramp->PWM)     ├──> [Env 1 VCA] ──> [12 dB/oct Input LPF] ──┬── (Dry) ───────────────────────────┐
[White / S&H Noise] ───────┘         ▲          (32.7 Hz - 21 kHz)      │                                    ▼
                                 [Env 1]                                └──> (+) ──> [1024-Stage BBD] ──> [Dry/Wet] ──> [Env 2 VCA] ──> Σ (16 Voices)
                          (0-2s Delay + DADSR                                 ▲       (Key-Tracked)                      ▲                  │
                           + Gate A-D Loop)                                   │             │                        [Env 2]                ▼
                                                                              └── [Invert] ◄─ [FB VCA] ◄─ [6 dB FB LPF] (Post-BBD ADSR  [Master Stereo FX]
                                                                                  (+/-1)     (0..115%)                  + Drone Hold)   (Chorus -> Delay ->
                                                                                                                                         Shimmer Reverb ->
                                                                                                                                         Bus Compressor)
                                                                                                                                            │
                                                                                                                                            ▼
                                                                                                                                        [Master Soft-Knee
                                                                                                                                         Limiter -> Out]
```

- **6 Multi-Timbral Voice Groups (`G1..G6`) Sharing 6 Hardware Voices**:
  - Default preset allocation: **Group 1 (`G1`) = `3` voices** on **`MIDI Ch 1`** (`Round-Robin`), **Groups 2–6 (`G2..G6`) = `0` voices** (disabled, pre-assigned to `MIDI Ch 2..6`).
  - Sound settings are stored **per group** so every voice inside a group shares a cohesive sound while multiple enabled groups play independent patches simultaneously on separate MIDI channels.
  - Pressing **`SEQ`** toggles the **Group Settings sub-menu** (`GRP SETTINGS` + 6-column `G1..G6` allocation matrix), while pressing **`SAVE`** opens the **Save User Preset** modal (`U1..U24`, persistent across power cycles in NOR flash on FM-1 and `user_presets.bin` on SDL2):
    - **`1:VOICES`**: `0` (`OFF`) to `16`, capped strictly to the group's current allocation plus remaining free voices (`USED:X/16 FREE:Y`) so voices are never stolen from other active groups.
    - **`2:ASSIGN`**: `POLY` (polyphonic round-robin + oldest-voice stealing within the group), `UNISON` (all group voices stacked on each note, with detuning and spread, and one BBD stage per voice), or `SUPER` (like `UNISON`, but with a single BBD stage after stacking instead of one per voice).
    - **`3:MIDI`**: `CH 1` .. `CH 16` per group (the built-in 27-key panel and SDL QWERTY keyboard automatically transmit on the selected group's MIDI channel).
    - **`4:DETUNE`**: `0C` .. `50C` zero-mean symmetrical Unison pitch spread across both the Micro-ACB VCO and 1024-stage BBD resonator.
    - **`5:MIDI`** (`CH 1..CH 16`)
    - **`6:PAN`** (`L100..C..R100` per-group stereo pan)
    - **`7:LEGATO`** (`RETRIG` / `LEGATO`)
    - **`8:GRP LVL`** (`0..100%`).
- **Dynamic Per-Group UI Color Themes (`HOME` Button)**: Cycles `G1 -> G2 -> G3 -> G4 -> G5 -> G6 -> G1` (**`G1` Electric Cyan**, **`G2` Lush Emerald**, **`G3` Warm Gold**, **`G4` Sunset Coral**, **`G5` Synthwave Magenta**, **`G6` Luminous Violet**), coloring header badges, tabs, oscilloscope/envelope curves, cards, and the 6 hardware voice-ownership pips.

---

## Hardware Knobs, Buttons, Pages & SDL Controls

Each parameter screen displays **4 parameters at a time** in a 1×4 row mapped directly to **`KNOB1`**, **`KNOB2`**, **`KNOB3`**, and **`KNOB4`**. Pressing a screen's button a second time cycles between **Page `1/2`** (parameters 1–4) and **Page `2/2`** (parameters 5–7, with the 4th cell left empty).

| FM-1 Control | SDL Key / Action | Function |
| :--- | :--- | :--- |
| **`KNOB1`..`KNOB4`** | Top 4 Knobs | Edit the 4 parameters displayed on the current screen's active page |
| **`SELECT` Knob** | `Tab` / `7` / Knob 5 / Click Badge | **Select Voice Group** (`G1`..`G6`) |
| **`PRESETS` Knob** | `-` / `=` / Knob 6 / Click Preset Badge | **Presets Browser** — First turn snapshots current user settings and opens the Presets screen at the active preset; subsequent turns scroll & wrap through `INIT SYNTH`, `INIT GROUP`, Factory `P1`..`P24`, and User `U1`..`U24` with live audio preview. **`KNOB1` (`1:BK`)** filters `ALL` / `USER` / `FACT`, **`KNOB2` (`2:MD`)** selects `FULL` preset load or `GRP` single-group patch import (with **`KNOB3` (`3:SR`)** choosing source `G1`..`G6` into the active group), and **`KNOB4` (`4:CL`)** arms two-step clear on occupied User slots |
| **`ALGORITHM` Knob** | `;` / `'` / Knob 7 | **Scale Browser** — First turn opens the Scales screen highlighting the active scale (default `CHROMATIC`); subsequent turns scroll & wrap through 16 musical scales (`CHROMATIC`, `MAJOR`, `NAT MINOR`, `MIN PENTA`, `MAJ PENTA`, `BLUES`, `DORIAN`, `PHRYGIAN`, `LYDIAN`, `MIXOLYDIAN`, `LOCRIAN`, `HARM MINOR`, `MEL MINOR`, `PHRYG DOM`, `HIRAJOSHI`, `WHOLE TONE`) with live white-key mapping preview, while **`KNOB1`** selects the root note (`C`..`B`, default `C`). In non-chromatic scales, white keys step sequentially through scale degrees and black keys are muted |
| **`SEL` (`SCL`)** | `1` | **`EXC` Screen (3 Subpages)** — Page 1/3: Morph, PWM, VCO Level (`0..100%` overall exciter wave+noise level), Semi; Page 2/3: Fine, Mix (`0..100%` Wave/Noise mix: `0%` = wave only, `100%` = noise only), S&H Clock, Pluck (`0..100%` Note-On Phase Lock + Pick/Mallet Impulse + `-1 Oct` Sub-Osc); Page 3/3: ACB Drift (`0..100%`), VCO Keytrack (`TRACK` / `FIXED`) |
| **`ENV`** | `2` | **`ENV1` Screen** — Interactive DADSR / Loop graph + Page 1/2: Attack, Decay, Sustain, Release; Page 2/2: Delay, Vel Sens, Loop, Range (`SHORT` `0–250 ms`, `NORM` `5 s / 10 s`, `LONG` `up to 25 s` per stage) |
| **`LFO`** | `3` | **`BBD` Screen** — Interactive Resonator graph + Page 1/2: Input LPF, Input Res, Feedback, FB LPF; Page 2/2: Invert, Glide, Dry/Wet, Dispersion (`0..100%` Loop All-Pass Stiffness / Inharmonicity) |
| **`EDIT`** | `4` | **`ENV2` Screen** — Interactive DADSR / Drone graph + Page 1/2: Attack, Decay, Sustain, Release; Page 2/2: Delay, Vel Sens, Drone |
| **`GLO`** | `5` | **`MOD` Screen (4 Subpages)** — Interactive Multi-LFO 1..3 overlay graph + Page 1/4 (`LFO 1`): Wave, Rate, Depth, Dest; Page 2/4 (`LFO 2`): Wave, Rate, Depth, Dest; Page 3/4 (`LFO 3`): Wave, Rate, Depth, Dest (`VCO PT`, `MORPH`, `PWM`, `IN LPF`, `BBD PT`, `FB LPF`, `FB AMT`, `ALL PT`, `PAN`, `SPREAD`, `DRYWET`, `VCA`, `EX MIX`, `EX LVL`, `SH CLK`, `IN RES`, `DISPR`, `E1 ATK`, `E1 DEC`, `E1 SUS`, `E1 REL`, `E1 DLY`, plus `L1 RT` on `LFO 2` & `LFO 3`); Page 4/4 (`MW`): MW Depth |
| **`SAVE`** | `8` / `\` / `Cmd+S` / `Ctrl+S` / Right-Click Preset Badge | **`SAVE PRESET` Modal (`U1`..`U24`)** — Saves all 6 Voice Groups + Master Stereo FX + 12-char name to persistent NOR flash (`0x0C0000..0x0D9000` with atomic backup sector) / `user_presets.bin`. **`KNOB1`** selects slot `U1..U24`, **`KNOB2`** moves the 12-cell cursor, **`KNOB3`** edits the active character, **`KNOB4`** clears/resets (or type directly on QWERTY in SDL2); **`OCT+` (`Enter`)** commits, **`OCT-` (`Esc`)** cancels |
| **`SEQ`** | `0` / `` ` `` / `Enter` / Right-Click Group Badge | **`GRP` (Group) Screen** — 6-column `G1..G6` matrix + Page 1/2: Voices, Assign, Spread, Detune; Page 2/2: MIDI Ch, Pan (`L100..C..R100`), Transpose, Group Level |
| **`ARP`** | `9` / `A` | **`ARP` (Arpeggiator) Screen** — 8-step Arpeggio visualizer + Page 1/2: Algo, Range (`1..4 OCT`), Rate (`40..240 BPM` internal, or auto-locked `EXT` + BPM under external `24 PPQN` MIDI Clock), Gate (`5..100%`); Page 2/2: Velocity (`KEY` / `1..127`), Latch (`OFF` / `ON`), Swing (`0..100%`), Ratchet (`1X`, `2X`, `3X`, `4X`, `1-2X`, `1-3X`, `1-4X`, `RND`) |
| **`FX`** | `F` / `/` | **`FX` (Master Stereo FX) Screen (3 Subpages)** — Stereo Chorus/Delay/Reverb + Soft-Knee Bus Compressor & GR visualizer — Page 1/3: Chorus Mix (`0..100%`), Delay Time (`20..360 ms` internal, or tempo-synced `1/32`, `1/16T`, `1/16`, `1/8T`, `1/16D`, `1/8`, `1/8D`, `1/4` under external MIDI Clock), Delay Feedback (`0..92%`), Delay Mix (`0..100%`); Page 2/3: Reverb Size (`0..100%`), Reverb Damping (`0..100%`), Octave Shimmer (`0..100%`), Reverb Mix (`0..100%`); Page 3/3: Bus Comp Mix (`0..100%` parallel dry/wet glue), Threshold (`-24..-2 dB`), Attack (`1..50 ms` with `~110 Hz` sidechain HPF & program-dependent release), Makeup (`+0..+12 dB`) |
| **`HOME`** | `6` / Click Badge | **`HOME` Visualizers (6 Pages)** — Cycles `1/6 SCOPE` (Oscilloscope: `MONO`/`DUAL`/`FILL`), `2/6 VECT` (Stereo Vectorscope / Lissajous Goniometer + Correlation & L/R Meters: `GONI`/`X-Y`/`DOTS`), `3/6 SPEC` (64-pt FFT Spectrum Analyzer + 24-Row Waterfall: `SPLT`/`BARS`/`WTRF`), `4/6 STAGE` (2D Stereo Soundstage & 16-Voice Pitch/Pan/BBD Constellation: `FLD`/`VEC`/`HRM`), `5/6 POLY` (16-Voice Telemetry Bridge: `TRI`/`BBD`/`ENV`), and `6/6 PERF` (6-Group Performance & Arp Dashboard + Master FX Meters: `DASH`/`ARP`/`MIX`). On all `HOME` pages, **`KNOB1`..`KNOB4`** control **`1:ZOOM`** (`1X..8X`), **`2:GAIN`** (`25%..400%`), **`3:FREEZE`** (`RUN`/`HOLD`), and **`4:MODE`** |
| **`PLAY`** | `Space` / `Backspace` | **Panic** — All Notes Off + immediate BBD resonator & Master FX tail flush |
| **`OCT-` / `OCT+`** | `[` / `]` | Keyboard Octave Down / Up (or **Cancel & Restore** / **Confirm Selection** when in the Presets Browser, Save Preset Modal, or Scale Browser screen) |
| **`REC`** | — | Unused (inactive, LED off) |

---

## MIDI Implementation

Reacts to both **USB-MIDI** and **3.5mm TRS MIDI IN** (`31,250` baud), routed per MIDI channel (`CH 1..16`) to matching Voice Groups (`G1..G6`):
- **Note On / Note Off** with Velocity (Round-Robin or Unison within each matching group), **14-bit Pitch Bend**, **Channel / Poly Pressure**, **Bank Select MSB (`CC0`: `0` = Factory `P1..P24`, `1` = User `U1..U24`)**, **Program Change (`0..23` Factory / `24..47` User in Bank 0, `0..23` User in Bank 1; empty User slots ignored cleanly)**, **Mod Wheel (`CC1`)**, **Portamento (`CC5`)**, **Group Volume (`CC7`)**, **Sustain Pedal (`CC64`)**, **Legato (`CC68`)**, **Drone Hold (`CC69`)**, **Unison Detune (`CC94`)**, **All Sound Off (`CC120`)**, **Reset All Controllers (`CC121`)**, **All Notes Off (`CC123`)**.
- **Per-Group Sound CCs**:
  - Exciter: `CC14`/`CC70` Morph, `CC15` PWM, `CC16` VCO Level (overall exciter level), `CC17` Semi, `CC18` Fine, `CC19` Wave/Noise Mix, `CC20` S&H Rate, `CC21` ACB Drift, `CC87` VCO Keytrack, `CC88` Pluck / Sub-Osc.
  - Env 1: `CC22` Onset Delay, `CC23` Attack, `CC24` Decay, `CC25` Sustain, `CC26` Release, `CC27` Vel Sens, `CC28` Loop Mode, `CC39` Env 1 Range (`SHORT` / `NORM` / `LONG`).
  - Filters & BBD: `CC74` Input LPF Cutoff, `CC71` Input Resonance, `CC75` BBD Feedback (`0-115%`), `CC76` 6 dB FB LPF Cutoff, `CC77` Polarity Invert, `CC78` Invert Pitch Comp, `CC79` Dry/Wet Mix, `CC89` Loop All-Pass Dispersion.
  - Post-BBD Env 2 & LFOs 1..3: `CC73` Env 2 Attack, `CC80` Env 2 Decay, `CC81` Env 2 Sustain, `CC72` Env 2 Release, `CC82` Env 2 Vel Sens, `CC69` Drone Hold, `CC83` LFO 1 Wave, `CC84` LFO 1 Rate, `CC85` LFO 1 Depth, `CC86` LFO 1 Dest, `CC31` LFO 2 Wave, `CC32` LFO 2 Rate, `CC33` LFO 2 Depth, `CC34` LFO 2 Dest, `CC35` LFO 3 Wave, `CC36` LFO 3 Rate, `CC37` LFO 3 Depth, `CC38` LFO 3 Dest.
  - Arpeggiator: `CC102` Algorithm (`0..10`), `CC103` Range (`1..4` octaves), `CC104` Rate (`40..240` BPM), `CC105` Gate (`5..100%`), `CC106` Velocity (`0` = Key, `1..127` = fixed), `CC107` Latch (`0..63` Off, `64..127` On), `CC108` Swing (`0..100%`), `CC109` Ratchet (`0..7`).
  - Master Stereo FX: `CC95`/`CC110` Chorus Mix, `CC111` Delay Time, `CC112` Delay Feedback, `CC113` Delay Mix, `CC114` Reverb Size, `CC115` Reverb Damping, `CC92`/`CC116` Reverb Shimmer, `CC91`/`CC117` Reverb Mix, `CC118` Bus Comp Mix, `CC119` Bus Comp Threshold, `CC29` Bus Comp Attack, `CC30` Bus Comp Makeup.
- **External MIDI Clock Sync (`24 PPQN` System Real-Time)**:
  - Automatically locks to incoming `0xF8` Timing Clock (`30..300 BPM`) across USB-MIDI, 3.5mm TRS MIDI IN, and CoreMIDI without disturbing running status, with a 2-second watchdog fallback to internal tempo when clock stops.
  - Handles `0xFA` (`Start`, resets all active Arpeggiators to step 1 on the downbeat), `0xFB` (`Continue`), and `0xFC` (`Stop`).
  - Phase-locks all 6 per-group Arpeggiators to `1/16`th steps (`6` pulses per step) while preserving sub-step Swing and Ratchet (`2X`/`3X`/`4X`) timing, and tempo-syncs Master Stereo Ping-Pong Delay (`2:DLY TM`) to 8 musical subdivisions (`1/32`, `1/16T`, `1/16`, `1/8T`, `1/16D`, `1/8`, `1/8D`, `1/4`).
