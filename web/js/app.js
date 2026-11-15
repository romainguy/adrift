// SPDX-License-Identifier: MPL-2.0
// Main website UI controller: connects the live FM-1 emulator, WebMIDI firmware installer,
// interactive hardware screen explorer, 24 factory preset showcase, and searchable MIDI CC guide.

import { AdriftEmulator } from './emulator.js';
import { GROUP_COLORS } from './panel.js';
import {
  Fm1Updater,
  InstallError,
  WebMidiBackend,
  loadPackageBytes,
} from './flasher.js';

// ---- All 24 Factory Presets (matching src/synth/presets.c indices 0..23) ----

const PRESETS = [
  {
    idx: 0,
    code: 'P01',
    title: 'Acoustic Guitar',
    oled: 'ACOUST GUIT',
    cat: 'strings',
    voices: '3V · 1 Group',
    groups: [0],
    desc: 'Warm plucked nylon-and-steel acoustic string with velocity-sensitive body resonance and 13 free voices ready for layering.',
  },
  {
    idx: 1,
    code: 'P02',
    title: 'Bowed Cello Duo',
    oled: 'BOWED CELLO',
    cat: 'strings',
    voices: '14V · 2 Groups',
    groups: [0, 1],
    desc: 'Expressive rosined cello body on G1 paired with a delayed +1 octave wooden harmonic bloom on G2.',
  },
  {
    idx: 2,
    code: 'P03',
    title: 'Chord Strummer',
    oled: 'STRUMMER',
    cat: 'strings',
    voices: '16V · 4 Groups',
    groups: [0, 1, 2, 3],
    desc: 'Press a single note to strum a cascading 4-string acoustic chord (Root, 5th, Octave, and 12th) staggered in time.',
  },
  {
    idx: 3,
    code: 'P04',
    title: 'Auto Feedback',
    oled: 'AUTO FEEDBK',
    cat: 'pads',
    voices: '12V · 2 Groups',
    groups: [0, 1],
    desc: 'Pure BBD self-oscillation singing on the edge of runaway analog feedback, reinforced by a detuned sub-octave unison.',
  },
  {
    idx: 4,
    code: 'P05',
    title: 'Slowpad',
    oled: 'SLOWPAD',
    cat: 'pads',
    voices: '16V · 2 Groups',
    groups: [0, 1],
    desc: 'Wide pulse-width-modulated BBD string pad melting into stereo chorus, ping-pong echo, and octave shimmer.',
  },
  {
    idx: 5,
    code: 'P06',
    title: 'Melancholy Bath',
    oled: 'MELANCHOLY',
    cat: 'keys',
    voices: '14V · 2 Groups',
    groups: [0, 1],
    desc: 'Lo-fi tape-warbled felted keys shadowed by a delayed inverted-phase octave ghost halo.',
  },
  {
    idx: 6,
    code: 'P07',
    title: '7 · 12 Glitter',
    oled: '7 12 GLITTER',
    cat: 'pads',
    voices: '16V · 3 Groups',
    groups: [0, 1, 2],
    desc: 'Crisp harp pluck followed by looping fifth (+7 ST) and octave (+12 ST) envelope sparkles.',
  },
  {
    idx: 7,
    code: 'P08',
    title: 'Mystery Pluck',
    oled: 'MISTERY PLK',
    cat: 'strings',
    voices: '14V · 2 Groups',
    groups: [0, 1],
    desc: 'Stepped sample-and-hold modal bell pluck over a deep hollow inverted sub-octave tube.',
  },
  {
    idx: 8,
    code: 'P09',
    title: 'Random Pluck',
    oled: 'RANDOM PLK',
    cat: 'strings',
    voices: '16V · 2 Groups',
    groups: [0, 1],
    desc: 'Every note lands with a slightly different wave shape and resonant filter color via per-voice sample & hold.',
  },
  {
    idx: 9,
    code: 'P10',
    title: '7th Overdub',
    oled: '7TH OVERDUB',
    cat: 'keys',
    voices: '16V · 3 Groups',
    groups: [0, 1, 2],
    desc: 'Warm electric tine keys that swell into a lush minor-7th chord (+7 and +10 semitone overdub layers) as you hold.',
  },
  {
    idx: 10,
    code: 'P11',
    title: 'Granular Cloud',
    oled: 'GRANULAR',
    cat: 'pads',
    voices: '16V · 4 Groups',
    groups: [0, 1, 2, 3],
    desc: 'Four asynchronous looping-envelope grain streams weaving shimmering fifths and octaves over a warm foundation pad.',
  },
  {
    idx: 11,
    code: 'P12',
    title: 'Into The Void',
    oled: 'INTO VOID',
    cat: 'pads',
    voices: '16V · 3 Groups',
    groups: [0, 1, 2],
    desc: 'Cinematic groaning iron resonators and hollow metal pipes anchored by a 4-voice unison abyssal sub bass.',
  },
  {
    idx: 12,
    code: 'P13',
    title: 'Pulsing Poly-Gate',
    oled: 'PULSING',
    cat: 'pads',
    voices: '14V · 2 Groups',
    groups: [0, 1],
    desc: 'Hypnotic interlocking polyrhythms created by looping A-D and D-A-D exciter envelopes an octave apart.',
  },
  {
    idx: 13,
    code: 'P14',
    title: 'Hollow Flute',
    oled: 'HOLLOW FLUTE',
    cat: 'strings',
    voices: '12V · 2 Groups',
    groups: [0, 1],
    desc: 'Inverted-polarity cylindrical woodwind pipe paired with a breathy unison reed chiff on every attack.',
  },
  {
    idx: 14,
    code: 'P15',
    title: 'Sitar Jawari',
    oled: 'SITAR JAWARI',
    cat: 'strings',
    voices: '16V · 3 Groups',
    groups: [0, 1, 2],
    desc: 'Dispersive buzzing Jawari bridge with delayed sympathetic fifth (+7 ST) and high Chikari octave (+12 ST) strings.',
  },
  {
    idx: 15,
    code: 'P16',
    title: 'Marimba Wood',
    oled: 'MARIMBA WOOD',
    cat: 'keys',
    voices: '14V · 2 Groups',
    groups: [0, 1],
    desc: 'Woody hollow rosewood marimba bars kissed with a bright kalimba thumb-tine harmonic.',
  },
  {
    idx: 16,
    code: 'P17',
    title: 'Glass Bowls',
    oled: 'GLASS BOWLS',
    cat: 'keys',
    voices: '16V · 2 Groups',
    groups: [0, 1],
    desc: 'Singing Tibetan glass bowls wrapped in a crystalline octave shimmer reverb tail.',
  },
  {
    idx: 17,
    code: 'P18',
    title: 'Clavinet Funk',
    oled: 'CLAVINET FN',
    cat: 'keys',
    voices: '12V · 2 Groups',
    groups: [0, 1],
    desc: 'Snappy narrow-pulse funk clavinet with an upper-bridge key click for percussive riffs.',
  },
  {
    idx: 18,
    code: 'P19',
    title: 'Super Bass',
    oled: 'SUPER BASS',
    cat: 'leads',
    voices: '12V · 2 Groups',
    groups: [0, 1],
    desc: 'Eight detuned oscillators slammed into one shared stereo BBD resonator, topped with a tight 4-voice plucked transient.',
  },
  {
    idx: 19,
    code: 'P20',
    title: '16V Monster',
    oled: '16V MONSTER',
    cat: 'leads',
    voices: '16V · 1 Group',
    groups: [0],
    desc: 'All 16 hardware voices stacked into a single massive stereo unison lead — pure wall-of-sound analog grit.',
  },
  {
    idx: 20,
    code: 'P21',
    title: 'BBD Chorus Poly',
    oled: 'BBD CHORUS',
    cat: 'keys',
    voices: '16V · 2 Groups',
    groups: [0, 1],
    desc: 'Lush 8-voice polysynth blending dry oscillators with warm BBD ensemble chorus and PWM movement.',
  },
  {
    idx: 21,
    code: 'P22',
    title: 'Cathedral Pipe Organ',
    oled: 'PIPE ORGAN',
    cat: 'keys',
    voices: '16V · 4 Groups',
    groups: [0, 1, 2, 3],
    desc: 'Four-rank pipe organ stacking 16-foot Bourdon sub, 8-foot Principal, Nazard fifth, and 4-foot Clarion pipes.',
  },
  {
    idx: 22,
    code: 'P23',
    title: 'Techno Kit',
    oled: 'TECHNO KIT',
    cat: 'leads',
    voices: '16V · 4 Groups',
    groups: [0, 1, 2, 3],
    desc: 'Four-channel live techno rig: punchy analog kick (Ch 1), crisp metallic hats (Ch 2), singing saw/PWM leads (Ch 3), and squelchy acid bass (Ch 4).',
  },
  {
    idx: 23,
    code: 'P24',
    title: 'Drone + Lead',
    oled: 'DRONE + LEAD',
    cat: 'leads',
    voices: '16V · 4 Groups',
    groups: [0, 1, 2, 3],
    desc: 'Soaring 8-voice bowed lead on Ch 1 over hands-free C3 & G3 unison drones (Ch 2–3) and a pluck companion (Ch 4).',
  },
];

// ---- Interactive FM-1 Hardware Control & Screen Guide (all 29 screenshots) ----

const CONTROL_GUIDES = [
  {
    id: 'home',
    hwLabel: 'HOME Button',
    adriftRole: '6 Live Visualizers',
    badgeColor: '#F8BE30',
    summary:
      'Press HOME once to enter full-screen performance visuals, then press HOME repeatedly to cycle through 6 real-time analyzers.',
    tip: 'On every visualizer page, Knobs 1–4 control Zoom (1X–8X), Gain, Freeze, and Display Style.',
    screens: [
      { file: '01_home_scope.png', tab: '1/6 Scope', caption: 'Dual-trace stereo waveform oscilloscope colored by active Voice Group.' },
      { file: '02_home_vect.png', tab: '2/6 Vector', caption: 'Lissajous stereo vectorscope & phase-width meter showing spatial spread.' },
      { file: '03_home_spec.png', tab: '3/6 Spectrum', caption: 'Real-time 32-band spectrum analyzer with scrolling 24-row harmonic waterfall.' },
      { file: '04_home_stage.png', tab: '4/6 Stage', caption: '2D stereo soundstage constellation plotting all 16 voices in pitch and pan.' },
      { file: '05_home_poly.png', tab: '5/6 16-Voice', caption: 'Per-voice telemetry bars for Env 1, BBD energy, and Env 2 across all 16 voices.' },
      { file: '06_home_perf.png', tab: '6/6 Perform', caption: 'Six-group performance mixer showing live levels, arp steps, and Master FX activity.' },
    ],
  },
  {
    id: 'exc',
    hwLabel: 'SCL Button',
    adriftRole: 'EXC · The Spark (1/3–3/3)',
    badgeColor: '#F8BE30',
    summary:
      'Shapes the energy fed into the resonator: morph smoothly from Sine to Triangle, Saw, and Pulse, blend vintage Sample & Hold noise, or dial in a crisp acoustic pluck.',
    tip: 'Press SCL again to step through pages 1/3, 2/3, and 3/3. Turn up PLUCK for guitars, harps, and marimbas.',
    screens: [
      { file: '07_exc_1_wave.png', tab: 'EXC 1/3 Wave', caption: 'Knobs 1–4: Continuous Wave Morph, Pulse Width, Oscillator Level, and Semitone Tune.' },
      { file: '08_exc_2_noise_pluck.png', tab: 'EXC 2/3 Pluck', caption: 'Knobs 1–4: Fine Tune, Noise Level, Sample & Hold Grit Rate, and Pluck Burst.' },
      { file: '09_exc_3_drift_keytrk.png', tab: 'EXC 3/3 Drift', caption: 'Knobs 1–3: Octave Shift, Organic Analog Pitch Drift, and Oscillator Keytracking.' },
    ],
  },
  {
    id: 'env1',
    hwLabel: 'ENV Button',
    adriftRole: 'ENV 1 · Exciter Contour (1/2–2/2)',
    badgeColor: '#40E88C',
    summary:
      'Controls how long and how hard the Exciter feeds the BBD body. Short decays create plucked strings; slow attacks bow the resonator; Looping modes create rhythmic pulses and granular clouds.',
    tip: 'Use DELAY on page 2/2 to stagger multiple Voice Groups into automatic strums or blooming chord overdubs!',
    screens: [
      { file: '10_env1_1_adsr.png', tab: 'ENV1 1/2 ADSR', caption: 'Knobs 1–4: Attack, Decay, Sustain, and Release feeding into the BBD resonator.' },
      { file: '11_env1_2_loop.png', tab: 'ENV1 2/2 Loop', caption: 'Knobs 1–4: Onset Delay, Velocity Sensitivity, Loop Mode (OFF / A-D / D-A-D), and Time Range.' },
    ],
  },
  {
    id: 'bbd',
    hwLabel: 'LFO Button',
    adriftRole: 'BBD · Resonator Body (1/2–2/2)',
    badgeColor: '#38D8F8',
    summary:
      'The heart of Adrift: a 1024-stage key-tracked Bucket-Brigade Delay resonator preceded by a resonant 12 dB/oct low-pass filter.',
    tip: 'Push FEEDBACK past 88% for singing self-oscillation, flip INVERT for hollow woodwind/square-pipe harmonics, and raise DISPERSION for metallic sitar & bell tones.',
    screens: [
      { file: '12_bbd_1_filter_fb.png', tab: 'BBD 1/2 Body', caption: 'Knobs 1–4: Input Filter Cutoff, Input Resonance, BBD Feedback, and Loop Damping LPF.' },
      { file: '13_bbd_2_invert_disp.png', tab: 'BBD 2/2 Pipe & Mix', caption: 'Knobs 1–4: Dry/Wet Balance, Pitch Glide, Polarity Invert (hollow -1 oct / pitch-comp), and Dispersion.' },
    ],
  },
  {
    id: 'env2',
    hwLabel: 'EDIT Button',
    adriftRole: 'ENV 2 · Output & Drone (1/2–2/2)',
    badgeColor: '#FC6C56',
    summary:
      'Shapes the final output volume of each voice after the resonator — and unlocks hands-free continuous DRONE HOLD.',
    tip: 'Turn DRONE to ON on page 2/2 so a group sustains endlessly without holding a key — ideal for playing leads over evolving root/fifth drones.',
    screens: [
      { file: '14_env2_1_adsr.png', tab: 'ENV2 1/2 VCA', caption: 'Knobs 1–4: Post-resonator Attack, Decay, Sustain, and Release.' },
      { file: '15_env2_2_drone.png', tab: 'ENV2 2/2 Drone', caption: 'Knobs 1–4: Group Output Volume, Velocity Sensitivity, Hands-Free Drone Hold, and Time Range.' },
    ],
  },
  {
    id: 'mod',
    hwLabel: 'GLO Button',
    adriftRole: 'MOD · 3 Per-Voice LFOs (1/4–4/4)',
    badgeColor: '#F464C6',
    summary:
      'Three independent per-voice LFOs (Sine, Triangle, Saw, Square, Sample & Hold) routable to 23 destinations including Pitch, Wave Morph, Filter, BBD Damping, Pan, and Envelope times.',
    tip: 'LFO 2 and LFO 3 can even cross-modulate LFO 1’s speed for organic, never-repeating tape drift.',
    screens: [
      { file: '16_mod_1_lfo1.png', tab: 'MOD 1/4 LFO 1', caption: 'Knobs 1–4: LFO 1 Waveform, Rate (0.05–30 Hz), Bipolar Depth, and Target Destination.' },
      { file: '17_mod_2_lfo2.png', tab: 'MOD 2/4 LFO 2', caption: 'Knobs 1–4: LFO 2 Waveform, Rate, Bipolar Depth, and Target Destination.' },
      { file: '18_mod_3_lfo3.png', tab: 'MOD 3/4 LFO 3', caption: 'Knobs 1–4: LFO 3 Waveform, Rate, Bipolar Depth, and Target Destination.' },
      { file: '19_mod_4_mw.png', tab: 'MOD 4/4 Control', caption: 'Knobs 1–3: Mod Wheel Depth, Pitch-Bend Range (0–24 ST), and Legato Mode.' },
    ],
  },
  {
    id: 'grp',
    hwLabel: 'SEQ Button + SELECT Knob',
    adriftRole: 'GRP · 6 Voice Groups (1/2–2/2)',
    badgeColor: '#A884FF',
    summary:
      'Turn SELECT anytime to switch between Groups G1–G6. Press SEQ to allocate the 16 voices across groups, switch between POLY, UNISON, and SUPER modes, pan groups in stereo, or set MIDI channels.',
    tip: 'Set two groups to MIDI Ch 1 with different Transpose offsets (+7 or +12) to build instant multi-layered instruments!',
    screens: [
      { file: '20_grp_1_matrix_assign.png', tab: 'GRP 1/2 Voices', caption: 'Knobs 1–4: Voice Count (0–16), Mode (POLY / UNI / SUPER), Unison Detune, and Stereo Spread.' },
      { file: '21_grp_2_midi_pan.png', tab: 'GRP 2/2 Routing', caption: 'Knobs 1–4: MIDI Channel (1–16), Semitone Transpose (-24..+24), Group Pan, and Legato.' },
    ],
  },
  {
    id: 'arp',
    hwLabel: 'ARP Button',
    adriftRole: 'ARP · Per-Group Arpeggiator (1/2–2/2)',
    badgeColor: '#40E88C',
    summary:
      'Every Voice Group has its own independent arpeggiator with 10 melodic patterns (Up, Down, Converge, Diverge, Random, Chord…), 1–4 octaves, Latch, Swing, and sub-step Ratcheting.',
    tip: 'Feed external MIDI Clock over USB or TRS MIDI and all active group arpeggiators lock tightly to your DAW tempo!',
    screens: [
      { file: '22_arp_1_pattern.png', tab: 'ARP 1/2 Pattern', caption: 'Knobs 1–4: Pattern Mode, Octave Range (1–4), Tempo BPM, and Gate Length.' },
      { file: '23_arp_2_groove_ratchet.png', tab: 'ARP 2/2 Groove', caption: 'Knobs 1–4: Key/Fixed Velocity, Hold Latch, Shuffle Swing, and Ratchet Sub-Repeats.' },
    ],
  },
  {
    id: 'fx',
    hwLabel: 'FX Button',
    adriftRole: 'FX · Master Stereo Studio Chain (1/3–3/3)',
    badgeColor: '#A884FF',
    summary:
      'A lush stereo finishing chain shared by all 6 groups: Ensemble Chorus → Tempo-Synced Ping-Pong Delay → Lush Octave-Shimmer Reverb → Soft-Knee Glue Compressor.',
    tip: 'Watch the live Gain Reduction (GR) meter on FX 3/3 while dialing in punchy parallel compression for drums or unison bass.',
    screens: [
      { file: '24_fx_1_chorus_delay.png', tab: 'FX 1/3 Echo', caption: 'Knobs 1–4: Stereo Chorus Mix, Delay Time (or Sync Division), Feedback, and Ping-Pong Mix.' },
      { file: '25_fx_2_shimmer_reverb.png', tab: 'FX 2/3 Shimmer', caption: 'Knobs 1–4: Hall Size, High-Freq Damping, +1 Octave Shimmer Pitch Mix, and Reverb Level.' },
      { file: '26_fx_3_bus_compressor.png', tab: 'FX 3/3 Glue Comp', caption: 'Knobs 1–4: Parallel Wet/Dry Mix, Threshold, Attack Speed, and Makeup Gain.' },
    ],
  },
  {
    id: 'presets',
    hwLabel: 'PRESETS Knob & SAVE Button',
    adriftRole: 'Browse P1–P24 & Save User U1–U24',
    badgeColor: '#F8BE30',
    summary:
      'Turn PRESETS to audition 24 Factory Presets (P1–P24), 24 persistent User Slots (U1–U24), or clean Init patches — and press OCT+ to commit or OCT- to cancel back to where you started.',
    tip: 'Power move: inside the Presets Browser, turn Knob 2 from FULL to GRP to import just ONE group layer from any preset into your current patch!',
    screens: [
      { file: '27_presets_browser.png', tab: 'Presets Browser', caption: 'Filter ALL / USER / FACT banks, audition live, or cherry-pick single group patches.' },
      { file: '29_preset_save.png', tab: 'Save User Preset', caption: 'Save up to 24 custom multi-group patches + FX directly to flash (U1–U24) with a 12-letter name.' },
    ],
  },
  {
    id: 'scales',
    hwLabel: 'ALGORITHM Knob',
    adriftRole: '16 Musical Keyboard Scales',
    badgeColor: '#38D8F8',
    summary:
      'Turn ALGORITHM to quantize the 27-key panel keyboard into 16 musical scales (Chromatic, Major, Minor, Pentatonic, Dorian, Phrygian Dominant, Hirajoshi, Blues, Whole Tone, and more) in any root key.',
    tip: 'In non-chromatic scales, every white pill key maps to an in-key scale degree across octaves so you can never hit a wrong note!',
    screens: [
      { file: '28_scales_browser.png', tab: 'Scale & Root', caption: 'Turn ALGORITHM to pick a scale, Knob 1 to set Root Note (C–B), and OCT+ to apply.' },
    ],
  },
  {
    id: 'screen-reader',
    hwLabel: 'Black Key 11 + Keys 4–7',
    adriftRole: 'Built-In Screen Reader (Speech)',
    badgeColor: '#40E88C',
    summary:
      'Hold Black Key 11 (F#5, the lone rightmost black key) and press Black Key 6 (F#4) to toggle speech on or off, Black Key 7 (G#4) to hear a full Where Am I? summary of the current screen and all 4 knobs, or Black Keys 4 / 5 (C#4 / D#4) to adjust speech speed (1–5).',
    tip: 'Speech is mixed strictly into the 3.5 mm headphone / line-out jack and never sent over USB audio out, so you can record clean synth audio over USB while navigating completely by ear.',
    screens: [
      { file: '07_exc_1_wave.png', tab: 'Pages & Knobs', caption: 'Speaks plain English words and units ("Exciter, page 1 of 3", "cutoff 1 point 2 kilohertz") once a knob settles.' },
      { file: '27_presets_browser.png', tab: 'Presets & Scales', caption: 'Reads preset titles, bank sections, list positions ("4 of 24"), and scale names as you browse.' },
      { file: '29_preset_save.png', tab: 'Accessible Save', caption: 'Reads target slot, character cursor ("position 1 of 12, capital A"), edits, and confirm/cancel keys.' },
    ],
  },
];

// ---- Complete Musician MIDI CC Reference (matching src/synth/midi.c) ----

const MIDI_CCS = [
  { cc: 0, section: 'Presets', name: 'Bank Select (0 = Factory P1–24, 1 = User U1–24)', range: '0 or 1 (+ Program Change)' },
  { cc: 1, section: 'Performance', name: 'Mod Wheel (scales LFO 1 depth)', range: '0–127 (per group)' },
  { cc: 5, section: 'BBD Body', name: 'Portamento / Pitch Glide Time', range: '0–127' },
  { cc: 7, section: 'Performance', name: 'Group Output Volume', range: '0–127' },
  { cc: 10, section: 'Voice Group', name: 'Group Stereo Spread Width', range: '0–127 (alias CC 93)' },
  { cc: 14, section: 'Exciter', name: 'Wave Morph (Sine → Tri → Saw → Pulse)', range: '0–127 (alias CC 70)' },
  { cc: 15, section: 'Exciter', name: 'Pulse Width', range: '0–127' },
  { cc: 16, section: 'Exciter', name: 'Oscillator Level', range: '0–127' },
  { cc: 17, section: 'Exciter', name: 'Oscillator Semitone Offset', range: '-12 to +12 semitones' },
  { cc: 18, section: 'Exciter', name: 'Oscillator Fine Tune', range: '-100 to +100 cents' },
  { cc: 19, section: 'Exciter', name: 'Noise / S&H Level', range: '0–127' },
  { cc: 20, section: 'Exciter', name: 'Sample & Hold Clock Rate', range: '0–127' },
  { cc: 21, section: 'Exciter', name: 'Analog Pitch Drift', range: '0–127' },
  { cc: 22, section: 'Env 1', name: 'Exciter Onset Delay', range: '0–127' },
  { cc: 23, section: 'Env 1', name: 'Exciter Attack', range: '0–127' },
  { cc: 24, section: 'Env 1', name: 'Exciter Decay', range: '0–127' },
  { cc: 25, section: 'Env 1', name: 'Exciter Sustain', range: '0–127' },
  { cc: 26, section: 'Env 1', name: 'Exciter Release', range: '0–127' },
  { cc: 27, section: 'Env 1', name: 'Exciter Velocity Sensitivity', range: '0–127' },
  { cc: 28, section: 'Env 1', name: 'Envelope Loop Mode', range: '0–41 Off · 42–84 A-D · 85–127 D-A-D' },
  { cc: 39, section: 'Env 1', name: 'Envelope Time Range', range: '0–41 Short · 42–84 Norm · 85–127 Long' },
  { cc: 31, section: 'Mod / LFO', name: 'LFO 2 Waveform', range: 'Sine, Tri, Saw, Square, S&H' },
  { cc: 32, section: 'Mod / LFO', name: 'LFO 2 Rate', range: '0–127 (0.05–30 Hz)' },
  { cc: 33, section: 'Mod / LFO', name: 'LFO 2 Bipolar Depth', range: '0–127 (64 = center 0)' },
  { cc: 34, section: 'Mod / LFO', name: 'LFO 2 Destination', range: '23 mod targets' },
  { cc: 35, section: 'Mod / LFO', name: 'LFO 3 Waveform', range: 'Sine, Tri, Saw, Square, S&H' },
  { cc: 36, section: 'Mod / LFO', name: 'LFO 3 Rate', range: '0–127 (0.05–30 Hz)' },
  { cc: 37, section: 'Mod / LFO', name: 'LFO 3 Bipolar Depth', range: '0–127 (64 = center 0)' },
  { cc: 38, section: 'Mod / LFO', name: 'LFO 3 Destination', range: '23 mod targets' },
  { cc: 64, section: 'Performance', name: 'Sustain Pedal', range: '0–63 Off · 64–127 On' },
  { cc: 68, section: 'Performance', name: 'Legato Envelope Mode', range: '0–63 Off · 64–127 On' },
  { cc: 69, section: 'Env 2', name: 'Continuous Drone Hold', range: '0–63 Off · 64–127 Sustain Drone' },
  { cc: 71, section: 'BBD Body', name: 'Input Filter Resonance (Q)', range: '0–127' },
  { cc: 72, section: 'Env 2', name: 'Output VCA Release', range: '0–127' },
  { cc: 73, section: 'Env 2', name: 'Output VCA Attack', range: '0–127' },
  { cc: 74, section: 'BBD Body', name: 'Input Low-Pass Filter Cutoff', range: '0–127 (Brightness)' },
  { cc: 75, section: 'BBD Body', name: 'BBD Resonator Feedback', range: '0–127 (>110 self-oscillates)' },
  { cc: 76, section: 'BBD Body', name: 'BBD Feedback Loop Damping LPF', range: '0–127' },
  { cc: 77, section: 'BBD Body', name: 'BBD Polarity Invert (Hollow Pipe)', range: '0–63 Normal · 64–127 Inverted' },
  { cc: 78, section: 'BBD Body', name: 'Invert Pitch Compensation (+1 Oct)', range: '0–63 Sub-Oct · 64–127 Pitch-Locked' },
  { cc: 79, section: 'BBD Body', name: 'Dry / BBD Wet Balance', range: '0 = Dry · 127 = Pure BBD' },
  { cc: 80, section: 'Env 2', name: 'Output VCA Decay', range: '0–127' },
  { cc: 81, section: 'Env 2', name: 'Output VCA Sustain', range: '0–127' },
  { cc: 82, section: 'Env 2', name: 'Output VCA Velocity Sensitivity', range: '0–127' },
  { cc: 83, section: 'Mod / LFO', name: 'LFO 1 Waveform', range: 'Sine, Tri, Saw, Square, S&H' },
  { cc: 84, section: 'Mod / LFO', name: 'LFO 1 Rate', range: '0–127 (0.05–30 Hz)' },
  { cc: 85, section: 'Mod / LFO', name: 'LFO 1 Bipolar Depth', range: '0–127 (64 = center 0)' },
  { cc: 86, section: 'Mod / LFO', name: 'LFO 1 Destination', range: '22 mod targets' },
  { cc: 87, section: 'Exciter', name: 'Oscillator Pitch Keytracking', range: '0–63 Fixed · 64–127 Keytracked' },
  { cc: 88, section: 'Exciter', name: 'Acoustic Pluck Transient Burst', range: '0–127' },
  { cc: 89, section: 'BBD Body', name: 'All-Pass Dispersion (Metallic Stiffness)', range: '0–127' },
  { cc: 94, section: 'Voice Group', name: 'Unison / Super Detune Spread', range: '0–127 (0–120 cents)' },
  { cc: 102, section: 'Arpeggiator', name: 'Arp Pattern Mode', range: 'Off, Up, Down, Up/Dn, Converge, Random, Chord…' },
  { cc: 103, section: 'Arpeggiator', name: 'Arp Octave Range', range: '1–4 octaves' },
  { cc: 104, section: 'Arpeggiator', name: 'Arp Internal Tempo', range: '40–240 BPM' },
  { cc: 105, section: 'Arpeggiator', name: 'Arp Gate Length', range: '5%–100%' },
  { cc: 106, section: 'Arpeggiator', name: 'Arp Velocity Mode', range: '0 = Played Key · 1–127 = Fixed' },
  { cc: 107, section: 'Arpeggiator', name: 'Arp Hold / Latch', range: '0–63 Off · 64–127 Latch' },
  { cc: 108, section: 'Arpeggiator', name: 'Arp Groove Swing', range: '0%–100%' },
  { cc: 109, section: 'Arpeggiator', name: 'Arp Ratchet Sub-Triggers', range: '1x, 2x, 3x, 4x, Alt, Random' },
  { cc: 110, section: 'Master FX', name: 'Stereo Ensemble Chorus Mix', range: '0–127 (alias CC 95)' },
  { cc: 111, section: 'Master FX', name: 'Ping-Pong Delay Time / Division', range: '0–127' },
  { cc: 112, section: 'Master FX', name: 'Ping-Pong Delay Feedback', range: '0–127' },
  { cc: 113, section: 'Master FX', name: 'Ping-Pong Delay Mix', range: '0–127' },
  { cc: 114, section: 'Master FX', name: 'Reverb Room / Hall Size', range: '0–127' },
  { cc: 115, section: 'Master FX', name: 'Reverb High-Frequency Damping', range: '0–127' },
  { cc: 116, section: 'Master FX', name: 'Reverb +1 Oct Shimmer Amount', range: '0–127 (alias CC 92)' },
  { cc: 117, section: 'Master FX', name: 'Reverb Wet Mix', range: '0–127 (alias CC 91)' },
  { cc: 118, section: 'Master FX', name: 'Bus Compressor Parallel Mix', range: '0–127' },
  { cc: 119, section: 'Master FX', name: 'Bus Compressor Threshold', range: '0–127' },
  { cc: 29, section: 'Master FX', name: 'Bus Compressor Attack', range: '0–127' },
  { cc: 30, section: 'Master FX', name: 'Bus Compressor Makeup Gain', range: '0–127' },
  { cc: 120, section: 'Performance', name: 'All Sound Off (clears BBD + FX)', range: 'Any value' },
  { cc: 123, section: 'Performance', name: 'All Notes Off', range: 'Any value' },
];

const HERO_QUICK_PRESETS = [0, 1, 2, 4, 6, 10, 14, 16, 19, 23];

// ---- Progressive-enhancement Dialog setup (Modern Web Guidance) ----

function setupDialog(dialog) {
  if (!dialog) return;
  dialog.addEventListener('close', () => {
    dialog.returnValue = '';
  });

  // Fallback for browsers without declarative closedby="any"
  if (!('closedBy' in dialog)) {
    let downOutside = false;
    dialog.addEventListener('pointerdown', (e) => {
      if (e.target !== dialog) {
        downOutside = false;
        return;
      }
      const r = dialog.getBoundingClientRect();
      downOutside = e.clientX < r.left || e.clientX > r.right || e.clientY < r.top || e.clientY > r.bottom;
    });
    dialog.addEventListener('click', (e) => {
      if (e.target !== dialog || !downOutside) return;
      const r = dialog.getBoundingClientRect();
      const upOutside = e.clientX < r.left || e.clientX > r.right || e.clientY < r.top || e.clientY > r.bottom;
      if (upOutside) dialog.close();
    });
  }
}

function formatBytes(bytes) {
  if (!bytes) return '';
  return `${Math.round(bytes / 1024)} KB`;
}

// ---- Boot Application ----

document.addEventListener('DOMContentLoaded', () => {
  const canvas = document.getElementById('fm1-canvas');
  const audioBtn = document.getElementById('btn-audio-toggle');
  const audioStatusBadge = document.getElementById('emu-live-readout');
  const panicBtn = document.getElementById('btn-panic');
  const srToggleBtn = document.getElementById('btn-sr-toggle');
  const srExtraControls = document.getElementById('sr-extra-controls');
  const srWhereBtn = document.getElementById('btn-sr-where');
  const srSlowerBtn = document.getElementById('btn-sr-slower');
  const srFasterBtn = document.getElementById('btn-sr-faster');
  const trySrBtn = document.getElementById('btn-try-sr');
  const qwertyCheck = document.getElementById('chk-qwerty');
  const midiSelect = document.getElementById('midi-input-select');
  const midiConnectBtn = document.getElementById('btn-midi-connect');
  const userPresetCountEl = document.getElementById('user-preset-count');
  const btnExportPresets = document.getElementById('btn-export-presets');
  const fileImportPresets = document.getElementById('file-import-presets');
  const btnResetPresets = document.getElementById('btn-reset-presets');
  const quickPresetBar = document.getElementById('hero-quick-presets');

  const shortcutsDialog = document.getElementById('shortcuts-dialog');
  const btnOpenShortcuts = document.getElementById('btn-open-shortcuts');
  const btnCloseShortcuts = document.getElementById('btn-close-shortcuts');
  setupDialog(shortcutsDialog);

  if (btnOpenShortcuts && shortcutsDialog) {
    btnOpenShortcuts.addEventListener('click', () => shortcutsDialog.showModal());
  }
  if (btnCloseShortcuts && shortcutsDialog) {
    btnCloseShortcuts.addEventListener('click', () => shortcutsDialog.close());
  }

  // Instantiate live WASM FM-1 emulator
  const emulator = new AdriftEmulator(canvas, {
    onAudioState: (running, muted) => {
      if (!audioBtn) return;
      if (!running) {
        audioBtn.textContent = 'Start Synth Audio';
        audioBtn.dataset.state = 'idle';
      } else if (muted) {
        audioBtn.textContent = 'Unmute Audio';
        audioBtn.dataset.state = 'muted';
      } else {
        audioBtn.textContent = 'Mute Audio';
        audioBtn.dataset.state = 'live';
      }
      updateStatusReadout(emulator.activePreset, emulator.activeGroup, running, muted);
    },
    onStateChange: ({ activePreset, activeGroup }) => {
      highlightActivePresetUI(activePreset);
      updateStatusReadout(
        activePreset,
        activeGroup,
        Boolean(emulator.audioCtx && emulator.audioCtx.state === 'running'),
        emulator.muted
      );
    },
    onScreenReaderState: (srEnabled) => {
      if (srToggleBtn) {
        srToggleBtn.setAttribute('aria-pressed', srEnabled ? 'true' : 'false');
      }
      if (srExtraControls) {
        if (!srEnabled && srExtraControls.contains(document.activeElement) && srToggleBtn) {
          srToggleBtn.focus();
        }
        srExtraControls.hidden = !srEnabled;
      }
      updateStatusReadout(
        emulator.activePreset,
        emulator.activeGroup,
        Boolean(emulator.audioCtx && emulator.audioCtx.state === 'running'),
        emulator.muted
      );
    },
    onStorageSaved: (count) => {
      if (userPresetCountEl) {
        userPresetCountEl.textContent = `${count}/24 saved in browser`;
      }
    },
    onMidiInputs: (inputs, supported) => {
      if (!midiSelect) return;
      if (!supported) {
        midiSelect.innerHTML = '<option value="none">WebMIDI unavailable in this browser</option>';
        midiSelect.disabled = true;
        if (midiConnectBtn) midiConnectBtn.hidden = true;
        return;
      }
      const prev = midiSelect.value || 'all';
      midiSelect.innerHTML = '';
      const allOpt = document.createElement('option');
      allOpt.value = 'all';
      allOpt.textContent = inputs.length > 0 ? `All MIDI Inputs (${inputs.length})` : 'No USB/MIDI controller detected';
      midiSelect.appendChild(allOpt);
      for (const inp of inputs) {
        const opt = document.createElement('option');
        opt.value = inp.id;
        opt.textContent = inp.name;
        midiSelect.appendChild(opt);
      }
      if (Array.from(midiSelect.options).some((o) => o.value === prev)) {
        midiSelect.value = prev;
      }
    },
    onError: (message) => {
      if (audioStatusBadge) {
        audioStatusBadge.textContent = `Audio error: ${message}`;
      }
    },
  });

  function updateStatusReadout(presetIdx, groupIdx, running, muted) {
    if (!audioStatusBadge) return;
    let pLabel = 'INIT';
    if (presetIdx < 24 && PRESETS[presetIdx]) {
      pLabel = `${PRESETS[presetIdx].code} ${PRESETS[presetIdx].title}`;
    } else if (presetIdx >= 24 && presetIdx < 48) {
      pLabel = `U${String(presetIdx - 23).padStart(2, '0')} User Preset`;
    }
    const gLabel = `G${(groupIdx % 6) + 1}`;
    const stateLabel = !running ? 'Standby — click any key to play' : muted ? 'Audio Muted' : 'Audio Live';
    const srSuffix = emulator.srEnabled ? ' · Speech On' : '';
    audioStatusBadge.textContent = `${pLabel} · Editing ${gLabel} · ${stateLabel}${srSuffix}`;
  }

  if (userPresetCountEl) {
    userPresetCountEl.textContent = `${emulator.countOccupiedUserPresets()}/24 saved in browser`;
  }

  // Build Hero Quick-Preset Pills
  if (quickPresetBar) {
    for (const pIdx of HERO_QUICK_PRESETS) {
      const p = PRESETS[pIdx];
      const btn = document.createElement('button');
      btn.type = 'button';
      btn.className = 'preset-pill';
      btn.dataset.presetIdx = String(p.idx);
      if (p.idx === 0) btn.setAttribute('aria-pressed', 'true');
      else btn.setAttribute('aria-pressed', 'false');
      btn.innerHTML = `<span class="pill-code">${p.code}</span> <span>${p.title}</span>`;
      btn.addEventListener('click', () => {
        emulator.loadPreset(p.idx);
        highlightActivePresetUI(p.idx);
      });
      quickPresetBar.appendChild(btn);
    }
  }

  function highlightActivePresetUI(presetIdx) {
    document.querySelectorAll('.preset-pill').forEach((el) => {
      const match = Number(el.dataset.presetIdx) === presetIdx;
      el.setAttribute('aria-pressed', match ? 'true' : 'false');
    });
    document.querySelectorAll('.preset-card').forEach((card) => {
      const match = Number(card.dataset.presetIdx) === presetIdx;
      card.classList.toggle('is-playing', match);
    });
  }

  // Toolbar controls
  if (audioBtn) {
    audioBtn.addEventListener('click', async () => {
      if (!emulator.ready) {
        await emulator.ensureStarted();
      } else {
        emulator.toggleMute();
      }
    });
  }

  if (panicBtn) {
    panicBtn.addEventListener('click', () => emulator.panic());
  }

  if (srToggleBtn) {
    srToggleBtn.addEventListener('click', () => emulator.triggerScreenReaderAction(0));
  }
  if (srWhereBtn) {
    srWhereBtn.addEventListener('click', () => emulator.triggerScreenReaderAction(1));
  }
  if (srSlowerBtn) {
    srSlowerBtn.addEventListener('click', () => emulator.triggerScreenReaderAction(2));
  }
  if (srFasterBtn) {
    srFasterBtn.addEventListener('click', () => emulator.triggerScreenReaderAction(3));
  }
  if (trySrBtn) {
    trySrBtn.addEventListener('click', async () => {
      const playSec = document.getElementById('play');
      if (playSec) {
        playSec.scrollIntoView({ behavior: 'auto', block: 'start' });
      }
      if (srToggleBtn) {
        srToggleBtn.focus();
      }
      if (!emulator.srEnabled) {
        await emulator.triggerScreenReaderAction(0);
      } else {
        await emulator.triggerScreenReaderAction(1);
      }
    });
  }

  if (qwertyCheck) {
    qwertyCheck.addEventListener('change', () => {
      emulator.setQwertyHints(qwertyCheck.checked);
    });
  }

  if (midiSelect) {
    midiSelect.addEventListener('change', () => {
      emulator.setMidiInputId(midiSelect.value);
    });
  }

  if (midiConnectBtn) {
    midiConnectBtn.addEventListener('click', async () => {
      await emulator.requestMidiControllerAccess();
    });
  }

  if (btnExportPresets) {
    btnExportPresets.addEventListener('click', () => {
      const blob = emulator.exportUserPresetsBlob();
      const url = URL.createObjectURL(blob);
      const a = document.createElement('a');
      a.href = url;
      a.download = 'adrift-user-presets.bin';
      document.body.appendChild(a);
      a.click();
      a.remove();
      setTimeout(() => URL.revokeObjectURL(url), 2000);
    });
  }

  if (fileImportPresets) {
    fileImportPresets.addEventListener('change', async () => {
      const file = fileImportPresets.files && fileImportPresets.files[0];
      if (!file) return;
      try {
        const buf = await file.arrayBuffer();
        await emulator.importUserPresetsBytes(new Uint8Array(buf));
      } catch (err) {
        alert(err.message || String(err));
      } finally {
        fileImportPresets.value = '';
      }
    });
  }

  if (btnResetPresets) {
    btnResetPresets.addEventListener('click', () => {
      if (window.confirm('Clear all saved User Presets (U1–U24) stored in this browser?')) {
        emulator.resetUserPresets();
      }
    });
  }

  // ---- Interactive Hardware Control & Screen Explorer ----
  initControlExplorer();

  // ---- 24 Built-In Presets Showcase ----
  initPresetsGallery(emulator, highlightActivePresetUI);

  // ---- Searchable MIDI CC Cheat Sheet ----
  initMidiTable();

  // ---- Direct Browser Firmware Flasher ----
  initFirmwareFlasher(emulator);
});

function initControlExplorer() {
  const navEl = document.getElementById('control-guide-list');
  const hwLabelEl = document.getElementById('cg-hw-label');
  const roleEl = document.getElementById('cg-adrift-role');
  const summaryEl = document.getElementById('cg-summary');
  const tipEl = document.getElementById('cg-tip');
  const subTabsEl = document.getElementById('cg-subpage-tabs');
  const screenImg = document.getElementById('cg-screen-img');
  const screenCap = document.getElementById('cg-screen-caption');

  if (!navEl || !screenImg) return;

  let activeGuideIdx = 0;

  function renderSubpage(guide, subIdx) {
    const scr = guide.screens[subIdx] || guide.screens[0];
    screenImg.src = `./assets/screens/${scr.file}`;
    screenImg.alt = `${guide.adriftRole} — ${scr.tab} hardware screen`;
    if (screenCap) screenCap.textContent = scr.caption;

    subTabsEl.querySelectorAll('button').forEach((b, idx) => {
      b.setAttribute('aria-pressed', idx === subIdx ? 'true' : 'false');
    });
  }

  function selectGuide(idx) {
    activeGuideIdx = idx;
    const g = CONTROL_GUIDES[idx];
    navEl.querySelectorAll('button').forEach((b, i) => {
      b.setAttribute('aria-pressed', i === idx ? 'true' : 'false');
    });

    if (hwLabelEl) hwLabelEl.textContent = g.hwLabel;
    if (roleEl) {
      roleEl.textContent = g.adriftRole;
      roleEl.style.color = g.badgeColor;
    }
    if (summaryEl) summaryEl.textContent = g.summary;
    if (tipEl) tipEl.textContent = g.tip;

    subTabsEl.innerHTML = '';
    g.screens.forEach((scr, sIdx) => {
      const tabBtn = document.createElement('button');
      tabBtn.type = 'button';
      tabBtn.className = 'subpage-tab';
      tabBtn.textContent = scr.tab;
      tabBtn.setAttribute('aria-pressed', sIdx === 0 ? 'true' : 'false');
      tabBtn.addEventListener('click', () => renderSubpage(g, sIdx));
      subTabsEl.appendChild(tabBtn);
    });

    renderSubpage(g, 0);
  }

  CONTROL_GUIDES.forEach((g, idx) => {
    const li = document.createElement('li');
    const btn = document.createElement('button');
    btn.type = 'button';
    btn.className = 'control-select-btn';
    btn.setAttribute('aria-pressed', idx === 0 ? 'true' : 'false');
    btn.innerHTML = `
      <span class="cs-dot" style="background:${g.badgeColor}" aria-hidden="true"></span>
      <span class="cs-text">
        <strong>${g.hwLabel}</strong>
        <small>${g.adriftRole}</small>
      </span>
    `;
    btn.addEventListener('click', () => selectGuide(idx));
    li.appendChild(btn);
    navEl.appendChild(li);
  });

  selectGuide(0);
}

function initPresetsGallery(emulator, highlightActivePresetUI) {
  const grid = document.getElementById('presets-grid');
  const filterBtns = document.querySelectorAll('[data-preset-cat]');
  const searchInput = document.getElementById('preset-search-input');
  if (!grid) return;

  let activeCat = 'all';
  let query = '';

  function renderCards() {
    grid.innerHTML = '';
    const q = query.trim().toLowerCase();
    const matches = PRESETS.filter((p) => {
      const catOk = activeCat === 'all' || p.cat === activeCat;
      const qOk =
        !q ||
        p.title.toLowerCase().includes(q) ||
        p.oled.toLowerCase().includes(q) ||
        p.code.toLowerCase().includes(q) ||
        p.desc.toLowerCase().includes(q);
      return catOk && qOk;
    });

    for (const p of matches) {
      const card = document.createElement('article');
      card.className = 'preset-card';
      if (emulator.activePreset === p.idx) card.classList.add('is-playing');
      card.dataset.presetIdx = String(p.idx);

      const groupBadges = p.groups
        .map(
          (g) =>
            `<span class="grp-mini-tag" style="--grp-col:${GROUP_COLORS[g]}"><span class="grp-dot" aria-hidden="true"></span>G${g + 1}</span>`
        )
        .join('');

      card.innerHTML = `
        <div class="preset-card-top">
          <div class="preset-card-title-row">
            <span class="preset-num">${p.code}</span>
            <h3>${p.title}</h3>
          </div>
          <div class="preset-meta-row">
            <span class="preset-voices-badge">${p.voices}</span>
            <div class="preset-grp-list" aria-label="Active voice groups">${groupBadges}</div>
          </div>
        </div>
        <p class="preset-card-desc">${p.desc}</p>
        <div class="preset-card-footer">
          <button type="button" class="btn btn-secondary btn-play-preset">
            Play in Emulator
          </button>
        </div>
      `;

      card.querySelector('.btn-play-preset').addEventListener('click', () => {
        emulator.loadPreset(p.idx);
        highlightActivePresetUI(p.idx);
        const playSec = document.getElementById('play');
        if (playSec) {
          playSec.scrollIntoView({ behavior: 'smooth', block: 'start' });
        }
      });

      grid.appendChild(card);
    }
  }

  filterBtns.forEach((btn) => {
    btn.addEventListener('click', () => {
      activeCat = btn.dataset.presetCat || 'all';
      filterBtns.forEach((b) => b.setAttribute('aria-pressed', b === btn ? 'true' : 'false'));
      renderCards();
    });
  });

  if (searchInput) {
    searchInput.addEventListener('input', () => {
      query = searchInput.value || '';
      renderCards();
    });
  }

  renderCards();
}

function initMidiTable() {
  const tbody = document.getElementById('midi-cc-tbody');
  const searchInput = document.getElementById('midi-cc-search');
  const countEl = document.getElementById('midi-cc-count');
  if (!tbody) return;

  function renderRows(filterText = '') {
    const q = filterText.trim().toLowerCase();
    tbody.innerHTML = '';
    const list = MIDI_CCS.filter(
      (row) =>
        !q ||
        `cc ${row.cc}`.includes(q) ||
        String(row.cc) === q ||
        row.section.toLowerCase().includes(q) ||
        row.name.toLowerCase().includes(q) ||
        row.range.toLowerCase().includes(q)
    );
    for (const row of list) {
      const tr = document.createElement('tr');
      tr.innerHTML = `
        <td><code>CC ${row.cc}</code></td>
        <td><span class="cc-sec-tag">${row.section}</span></td>
        <td>${row.name}</td>
        <td>${row.range}</td>
      `;
      tbody.appendChild(tr);
    }
    if (countEl) {
      countEl.textContent = `Showing ${list.length} of ${MIDI_CCS.length} MIDI controls`;
    }
  }

  if (searchInput) {
    searchInput.addEventListener('input', () => renderRows(searchInput.value));
  }
  renderRows();
}

function initFirmwareFlasher(emulator) {
  const supportPill = document.getElementById('flash-support-pill');
  const btnFlashAdrift = document.getElementById('btn-flash-adrift');
  const btnCheckDevice = document.getElementById('btn-check-device');
  const portSelect = document.getElementById('flash-port-select');
  const progressWrap = document.getElementById('flash-progress-wrap');
  const progressBar = document.getElementById('flash-progress-bar');
  const statusHeadline = document.getElementById('flash-status-headline');
  const statusLog = document.getElementById('flash-status-log');
  const versionMetaEl = document.getElementById('fw-version-meta');
  const customFileInput = document.getElementById('file-custom-fwsc');
  const btnFlashCustom = document.getElementById('btn-flash-custom');

  let busy = false;
  let cachedBackend = null;

  // Populate build metadata from version.json
  fetch('./version.json')
    .then((r) => (r.ok ? r.json() : null))
    .then((meta) => {
      if (meta && versionMetaEl) {
        versionMetaEl.textContent = `Build ${meta.commit} · ${meta.product} · ${formatBytes(meta.fwsc_bytes)}`;
      }
    })
    .catch(() => {});

  const supported = WebMidiBackend.isSupported();
  if (supportPill) {
    if (supported) {
      supportPill.textContent = 'WebMIDI Ready — Direct USB Flashing Supported';
      supportPill.dataset.status = 'ok';
    } else {
      supportPill.textContent = 'Use Chrome, Edge, Brave, or Opera for USB Flashing (or download .fwsc below)';
      supportPill.dataset.status = 'warn';
    }
  }

  function appendLog(line) {
    if (!statusLog) return;
    const li = document.createElement('li');
    li.textContent = line;
    statusLog.appendChild(li);
    statusLog.scrollTop = statusLog.scrollHeight;
  }

  function setStatus(state, title, pct = null) {
    if (statusHeadline) {
      statusHeadline.dataset.state = state;
      statusHeadline.textContent = title;
    }
    if (progressWrap && progressBar) {
      if (pct === null) {
        progressWrap.hidden = true;
      } else {
        progressWrap.hidden = false;
        progressBar.value = Math.max(0, Math.min(100, pct));
      }
    }
  }

  function setControlsDisabled(disabled) {
    busy = disabled;
    if (btnFlashAdrift) btnFlashAdrift.disabled = disabled || !supported;
    if (btnCheckDevice) btnCheckDevice.disabled = disabled || !supported;
    if (btnFlashCustom) btnFlashCustom.disabled = disabled || !supported || !(customFileInput && customFileInput.files && customFileInput.files.length);
  }

  if (!supported) {
    setControlsDisabled(false);
  }

  async function ensureBackend() {
    if (!cachedBackend) {
      cachedBackend = await WebMidiBackend.requestAccess();
    }
    refreshPortDropdown(cachedBackend);
    return cachedBackend;
  }

  function refreshPortDropdown(backend) {
    if (!portSelect || !backend) return;
    const prev = portSelect.value;
    const names = backend.inputNames();
    portSelect.innerHTML = '<option value="">Auto-detect M-VAVE FM-1 (Recommended)</option>';
    for (const n of names) {
      const opt = document.createElement('option');
      opt.value = n;
      opt.textContent = n;
      portSelect.appendChild(opt);
    }
    if (Array.from(portSelect.options).some((o) => o.value === prev)) {
      portSelect.value = prev;
    }
  }

  if (btnCheckDevice) {
    btnCheckDevice.addEventListener('click', async () => {
      if (busy) return;
      if (statusLog) statusLog.innerHTML = '';
      setControlsDisabled(true);
      emulator.setMidiPaused(true);

      try {
        setStatus('working', 'Checking USB-MIDI ports for an M-VAVE FM-1…', null);
        const backend = await ensureBackend();
        const updater = new Fm1Updater(backend, { portFilter: portSelect ? portSelect.value : '' });
        const dev = await updater.find();
        if (!dev) {
          const seen = backend.inputNames();
          throw new InstallError(
            'notfound',
            `No M-VAVE FM-1 responded. Make sure it is switched ON with a USB data cable.${seen.length ? ` (Seen MIDI ports: ${seen.join(', ')})` : ''}`
          );
        }
        dev.link.close();
        const modeDesc = dev.id.loader
          ? 'Currently in Update Mode (ready to finish writing)'
          : dev.id.text === 'FM-1_462000400'
            ? 'Running Adrift firmware'
            : 'Running firmware';
        setStatus('ok', `Connected: ${dev.id.text} on "${dev.name}"`, null);
        appendLog(`${modeDesc} — port "${dev.name}" (${dev.id.text}). Ready to flash!`);
      } catch (err) {
        setStatus('error', 'Could not reach FM-1', null);
        appendLog(err.message || String(err));
      } finally {
        emulator.setMidiPaused(false);
        setControlsDisabled(false);
      }
    });
  }

  async function runFlashPackage(rawBytes, label) {
    if (busy) return;
    if (statusLog) statusLog.innerHTML = '';
    setControlsDisabled(true);
    emulator.setMidiPaused(true);

    try {
      setStatus('working', `Validating ${label}…`, 2);
      const pkg = await loadPackageBytes(rawBytes, { label });
      appendLog(
        pkg.isOfficialStock
          ? `Verified official M-VAVE factory firmware (${pkg.product}).`
          : `Verified firmware package ${pkg.product} (${formatBytes(rawBytes.byteLength)}).`
      );

      setStatus('working', 'Connecting to M-VAVE FM-1 over USB-MIDI…', 5);
      const backend = await ensureBackend();
      const updater = new Fm1Updater(backend, { portFilter: portSelect ? portSelect.value : '' });
      const dev = await updater.find();

      if (!dev) {
        const seen = backend.inputNames();
        throw new InstallError(
          'notfound',
          `M-VAVE FM-1 not found. Check that your synth is powered ON, connected with a USB data cable, and not open in a DAW.${seen.length ? ` (Detected MIDI ports: ${seen.join(', ')})` : ''}`
        );
      }

      appendLog(
        dev.id.loader
          ? `Found ${dev.name} already in Update Mode — resuming installation…`
          : `Found ${dev.id.text} on "${dev.name}". Keep the USB cable plugged in until complete.`
      );

      let lastCheckLog = 0;
      await updater.install(
        dev,
        pkg.image,
        pkg.product,
        (step, arg) => {
          if (step === 'start') {
            setStatus('working', `Preparing ${arg} for update…`, 8);
            appendLog(`Step 1/3: Sending update request to ${arg}…`);
          } else if (step === 'check') {
            const pct = Math.min(20, 8 + Math.floor(arg / 3));
            setStatus('working', `Verifying package header with synth (${arg} blocks)…`, pct);
            if (arg === 1 || arg - lastCheckLog >= 12) {
              lastCheckLog = arg;
            }
          } else if (step === 'loader') {
            setStatus('working', 'Switching FM-1 into Update Mode (USB reconnects automatically)…', 22);
            appendLog('Step 2/3: Waiting for FM-1 update loader to appear…');
          } else if (step === 'mode') {
            appendLog(
              arg === 'fast'
                ? 'Fast sector-diff active: writing only changed 4 KB blocks.'
                : 'Full write mode active: transferring complete image.'
            );
          } else if (step === 'write') {
            const uiPct = 25 + Math.round((arg * 68) / 100);
            setStatus('working', `Writing firmware to FM-1… ${arg}% (do not unplug)`, uiPct);
          } else if (step === 'reboot') {
            setStatus('working', 'Restarting your FM-1 into the new firmware…', 96);
            appendLog('Step 3/3: Write complete! Waiting for FM-1 to restart…');
          } else if (step === 'done') {
            setStatus('ok', `Success! Your FM-1 is now running ${arg === 'FM-1_462000400' ? 'Adrift' : arg}.`, 100);
            appendLog(`All done — verified ${arg}. Unplug or start playing!`);
          }
        },
        pkg.useClassic
      );
    } catch (err) {
      setStatus('error', 'Update did not complete', null);
      appendLog(err.message || String(err));
    } finally {
      emulator.setMidiPaused(false);
      setControlsDisabled(false);
    }
  }

  if (btnFlashAdrift) {
    btnFlashAdrift.addEventListener('click', async () => {
      if (busy) return;
      try {
        setStatus('working', 'Downloading adrift.fwsc…', 1);
        const res = await fetch('./adrift.fwsc');
        if (!res.ok) {
          throw new Error(`Could not fetch adrift.fwsc (${res.status}).`);
        }
        const buf = await res.arrayBuffer();
        await runFlashPackage(new Uint8Array(buf), 'adrift.fwsc');
      } catch (err) {
        setStatus('error', 'Download failed', null);
        appendLog(err.message || String(err));
      }
    });
  }

  if (customFileInput && btnFlashCustom) {
    customFileInput.addEventListener('change', () => {
      btnFlashCustom.disabled = busy || !supported || !(customFileInput.files && customFileInput.files.length);
    });
    btnFlashCustom.addEventListener('click', async () => {
      const file = customFileInput.files && customFileInput.files[0];
      if (!file || busy) return;
      const buf = await file.arrayBuffer();
      await runFlashPackage(new Uint8Array(buf), file.name);
    });
  }
}
