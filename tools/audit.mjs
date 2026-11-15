#!/usr/bin/env node
// SPDX-License-Identifier: MPL-2.0
// Screen-reader utterance & words.tsv auditor enforcing FM-1 screen reading rules.

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(__dirname, "..");
const DEFAULT_DUMP = path.join(ROOT, "build/test/said.tsv");
const WORDS_TSV = path.join(ROOT, "src/speech/words.tsv");

// Explicit English dictionary of valid words (zero synth/UI abbreviations allowed).
const VALID_WORDS = new Set([
  // Single letters (checked contextually below for spelled initialisms, notes, or character readouts)
  "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m",
  "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z",

  // Numbers & musical divisions
  "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine",
  "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen",
  "seventeen", "eighteen", "nineteen", "twenty", "thirty", "forty", "fifty",
  "sixty", "seventy", "eighty", "ninety", "hundred", "thousand",
  "first", "second", "seconds", "third", "fourth", "fifth", "sixth", "seventh",
  "eighth", "sixteenth", "quarter", "half", "triplet", "dotted",

  // Function words, units & directions
  "and", "or", "of", "to", "in", "into", "on", "off", "is", "are", "be",
  "at", "by", "for", "from", "with", "the", "this", "no", "not", "yes",
  "all", "free", "full", "single", "empty", "new", "plus", "minus", "point",
  "times", "percent", "hertz", "kilohertz", "millisecond", "milliseconds",
  "decibel", "decibels", "cent", "cents", "beats", "per", "minute",
  "semitone", "semitones", "octave", "octaves", "left", "right", "center",
  "up", "down", "lower", "raise", "use", "used", "minimum", "maximum",
  "space", "dash", "slash", "dot", "underscore", "blank",

  // UI, navigation, actions & warnings
  "screen", "reader", "speech", "speed", "page", "subpage", "knob", "knobs",
  "button", "position", "character", "slot", "bank", "actions", "action",
  "factory", "user", "preset", "presets", "patch", "group", "groups",
  "voice", "voices", "save", "saved", "saves", "saving", "failed", "flash",
  "write", "error", "load", "loaded", "loads", "mode", "clear", "cleared",
  "clears", "end", "cancel", "canceled", "cancels", "overwrite", "reset",
  "name", "apply", "applied", "applies", "initialize", "initialized", "initial",
  "synth", "source", "target", "select", "selected", "active", "enabled",
  "disabled", "occupied", "unused", "unmapped", "confirm", "confirms",
  "note", "notes", "external", "locked", "stopped", "only", "available",
  "switch", "another", "turn", "press", "change", "cannot", "can", "here",
  "memory", "ready", "controls", "control", "other", "already", "limit",
  "reduce", "allocated", "before", "adding", "add", "more", "works", "when",
  "edit", "editing", "global", "home", "play", "stop", "backspace",
  "delete", "deleted", "restored", "typed", "item",

  // Synthesis, DSP & visualization words
  "exciter", "envelope", "envelopes", "modulation", "effects", "arpeggiator",
  "settings", "oscilloscope", "vectorscope", "spectrum", "soundstage",
  "polyphony", "performance", "wave", "morph", "pulse", "width", "oscillator",
  "level", "fine", "tune", "noise", "mix", "sample", "hold", "clock", "white",
  "pluck", "analog", "drift", "keytrack", "tracking", "fixed", "attack",
  "decay", "sustain", "release", "onset", "delay", "velocity", "sensitivity",
  "loop", "time", "range", "short", "normal", "long", "input", "low", "pass",
  "resonance", "feedback", "invert", "inverted", "pitch", "compensated",
  "glide", "dry", "wet", "dispersion", "drone", "gated", "rate", "depth",
  "destination", "sine", "triangle", "saw", "square", "mod", "wheel", "pan",
  "stereo", "spread", "output", "amount", "assign", "polyphonic", "unison",
  "super", "detune", "midi", "channel", "legato", "retrigger", "algorithm",
  "converge", "diverge", "random", "order", "chord", "tempo", "gate", "key",
  "latch", "swing", "ratchet", "chorus", "reverb", "size", "damping",
  "shimmer", "compressor", "threshold", "makeup", "gain", "zoom", "freeze",
  "running", "frozen", "display", "mono", "dual", "fill", "goniometer",
  "dots", "split", "bars", "waterfall", "field", "vectors", "harmonic",
  "triple", "dashboard", "mixer", "resonator", "bucket", "brigade", "amplifier",

  // Scales & musical terms
  "scale", "scales", "root", "sharp", "flat", "chromatic", "major", "minor",
  "natural", "pentatonic", "blues", "dorian", "phrygian", "lydian",
  "mixolydian", "locrian", "melodic", "dominant", "hirajoshi", "whole", "tone",

  // Factory patch & preset vocabulary
  "acoustic", "guitar", "bowed", "cello", "hollow", "pipe", "auto", "mandolin",
  "mystery", "bass", "infinite", "strum", "strummer", "slow", "pad",
  "melancholy", "halo", "glitter", "sub", "tube", "overdub", "swell", "grain",
  "fast", "granular", "void", "iron", "pulsing", "flute", "sitar", "jawari",
  "marimba", "bar", "wood", "glass", "bowl", "bowls", "clavinet", "funk",
  "monster", "organ", "drum", "kit", "kits", "modal", "kick", "snare", "lead",
  "leads", "hat", "hats", "techno",
  "warm", "bell", "bells", "string", "strings", "brass", "soft", "bright",
  "dark", "deep", "ambient", "classic", "vintage", "ensemble", "electric",
  "piano", "digital", "acid"
]);

// Forbidden symbols per Rule 4 (audit: symbols)
const FORBIDDEN_SYMBOLS_RE = /[\/|><*_=#\[\]{}~^@&$+\\-]/;
// Letters and digits glued together per Rule 5 (audit: letters-and-digits)
const LETTERS_AND_DIGITS_RE = /[A-Za-z]\d|\d[A-Za-z]/;
// Noise patterns per Rule 6 (audit: noise)
const NOISE_RE = /\b0x[0-9a-fA-F]+\b|<empty>|\bcpu\b|[.]{2,}|[-]{2,}/i;
// Multi-letter uppercase words per Rule 7
const MULTI_CAPS_RE = /\b[A-Z]{2,}\b/;

const violations = [];
function fail(rule, loc, detail, text) {
  violations.push({ rule, loc, detail, text });
}

// 1. Audit src/speech/words.tsv expansions first
if (!fs.existsSync(WORDS_TSV)) {
  fail("not-a-word", "src/speech/words.tsv", "Missing words.tsv file", "");
} else {
  const tsvLines = fs.readFileSync(WORDS_TSV, "utf8").split(/\r?\n/);
  tsvLines.forEach((raw, idx) => {
    const line = raw.trim();
    if (!line || line.startsWith("#")) return;
    const parts = line.split("\t");
    if (parts.length !== 2) {
      fail("punctuation", `words.tsv:${idx + 1}`, "Expected key<TAB>spoken_expansion", line);
      return;
    }
    const [key, spoken] = parts;
    if (FORBIDDEN_SYMBOLS_RE.test(spoken)) {
      fail("symbols", `words.tsv:${idx + 1} (${key})`, "Forbidden symbol in expansion", spoken);
    }
    if (LETTERS_AND_DIGITS_RE.test(spoken)) {
      fail("letters-and-digits", `words.tsv:${idx + 1} (${key})`, "Glued letter+digit in expansion", spoken);
    }
    const words = spoken.match(/[A-Za-z]+/g) || [];
    for (const w of words) {
      if (!VALID_WORDS.has(w.toLowerCase())) {
        fail("not-a-word", `words.tsv:${idx + 1} (${key})`, `Non-dictionary/abbreviated word '${w}'`, spoken);
      }
    }
  });
}

// 2. Audit build/test/said.tsv utterance dump
const dumpPath = process.argv[2] ? path.resolve(process.argv[2]) : DEFAULT_DUMP;
if (!fs.existsSync(dumpPath)) {
  console.error(`[audit] Missing speech dump file: ${dumpPath}`);
  process.exit(1);
}

const rawRows = fs.readFileSync(dumpPath, "utf8").split(/\r?\n/).filter((l) => l.length > 0);
if (rawRows.length < 50) {
  fail("noise", dumpPath, `Expected comprehensive utterance dump (>= 50 rows), got ${rawRows.length}`, "");
}

rawRows.forEach((row, idx) => {
  const loc = `${path.basename(dumpPath)}:${idx + 1}`;
  const cols = row.split("\t");
  if (cols.length !== 4) {
    fail("punctuation", loc, `Expected 4 TSV columns, got ${cols.length}`, row);
    return;
  }
  const [ctx, cat, prio, text] = cols;

  // Rule 11: too-long (<= 240 chars)
  if (text.length === 0) {
    fail("punctuation", `${loc} [${ctx}]`, "Empty SAY utterance", text);
    return;
  }
  if (text.length > 240) {
    fail("too-long", `${loc} [${ctx}]`, `Utterance is ${text.length} chars (> 240)`, text);
  }

  // Rule 4: symbols
  if (FORBIDDEN_SYMBOLS_RE.test(text)) {
    fail("symbols", `${loc} [${ctx}]`, "Contains forbidden symbol (/ | > < * _ = # [ ] { } ~ ^ @ & $ + -)", text);
  }

  // Rule 5: letters-and-digits
  if (LETTERS_AND_DIGITS_RE.test(text)) {
    fail("letters-and-digits", `${loc} [${ctx}]`, "Letters and digits run together", text);
  }

  // Rule 6: noise
  if (NOISE_RE.test(text)) {
    fail("noise", `${loc} [${ctx}]`, "Contains visual/debug noise (hex, dots bar, CPU, <empty>)", text);
  }

  // Rule 7: no multi-letter ALL-CAPS words
  if (MULTI_CAPS_RE.test(text)) {
    fail("not-a-word", `${loc} [${ctx}]`, "Contains multi-letter uppercase word (Rule 7)", text);
  }

  // Rule 10: punctuation
  if (text !== text.trim()) {
    fail("punctuation", `${loc} [${ctx}]`, "Leading or trailing whitespace", text);
  }
  if (/  +/.test(text)) {
    fail("punctuation", `${loc} [${ctx}]`, "Multiple consecutive spaces", text);
  }
  if (/\s+[.,?:;!]/.test(text)) {
    fail("punctuation", `${loc} [${ctx}]`, "Space before punctuation mark", text);
  }
  if (/[.,?:;!]{2,}/.test(text)) {
    fail("punctuation", `${loc} [${ctx}]`, "Multiple consecutive punctuation marks", text);
  }
  if (/[.,?:;!][A-Za-z0-9]/.test(text)) {
    fail("punctuation", `${loc} [${ctx}]`, "Missing space after punctuation mark", text);
  }
  if (!/[.?]$/.test(text)) {
    fail("punctuation", `${loc} [${ctx}]`, "Utterance must end with '.' or '?'", text);
  }

  // Tokenize words
  const tokens = text.match(/[A-Za-z0-9]+/g) || [];
  const alphaWords = text.match(/[A-Za-z]+/g) || [];

  // Rule 1: not-a-word
  for (const w of alphaWords) {
    if (!VALID_WORDS.has(w.toLowerCase())) {
      fail("not-a-word", `${loc} [${ctx}]`, `Unknown or abbreviated word '${w}'`, text);
    }
  }

  // Rule 9: repeated (no adjacent duplicate words, even across punctuation)
  for (let i = 1; i < tokens.length; i++) {
    if (tokens[i].toLowerCase() === tokens[i - 1].toLowerCase()) {
      fail("repeated", `${loc} [${ctx}]`, `Repeated adjacent word '${tokens[i]}'`, text);
    }
  }
  // Also catch "X. X <noun>" pattern from Rule 9 ("909. 909 kit")
  if (/\b([A-Za-z0-9]+)[.,]\s+\1\b/i.test(text)) {
    fail("repeated", `${loc} [${ctx}]`, "Repeated leading token across clause boundary", text);
  }

  // Rule 11: readout-short & screen-long (count spelled initialisms like "L F O" as 1 word per Rule 7)
  const wordCount = (text.replace(/\b(?:[A-Z]\s+){1,4}[A-Z]\b/g, "ACRONYM").match(/[A-Za-z0-9]+/g) || []).length;
  if (cat === "READOUT") {
    if (wordCount > 8) {
      fail("readout-short", `${loc} [${ctx}]`, `Knob readout has ${wordCount} words (> 8)`, text);
    }
    const sentences = text.split(/[.?!]/).filter((s) => s.trim().length > 0);
    if (sentences.length > 1) {
      fail("readout-short", `${loc} [${ctx}]`, "Knob readout must be a single name + value phrase", text);
    }
  }
  if (cat === "SCREEN") {
    if (wordCount > 45) {
      fail("screen-long", `${loc} [${ctx}]`, `Screen announcement has ${wordCount} words (> 45)`, text);
    }
  }

  // Rule 12: list-position & help-names-button
  if (cat === "LIST") {
    const m = text.match(/\b(\d+)\s+of\s+(\d+)\.$/i);
    if (!m) {
      fail("list-position", `${loc} [${ctx}]`, "List row must end with '<pos> of <total>.'", text);
    } else {
      const pos = Number(m[1]);
      const tot = Number(m[2]);
      if (!(pos >= 1 && tot >= 1 && pos <= tot)) {
        fail("list-position", `${loc} [${ctx}]`, `Invalid list position ${pos} of ${tot}`, text);
      }
    }
  }
  if (cat === "HELP") {
    if (!/^[a-z0-9 ]+\s+button\./i.test(text)) {
      fail("help-names-button", `${loc} [${ctx}]`, "Help card must start with '<name> button.'", text);
    }
  }

  // Rule 15: warning-says-what + ANN_KEEP
  if (cat === "WARNING") {
    if (prio !== "KEEP") {
      fail("warning-says-what", `${loc} [${ctx}]`, `Warning sent with prio=${prio} instead of KEEP (ANN_KEEP)`, text);
    }
    if (!/\b(cannot|failed|not used|unmapped|full|error)\b/i.test(text)) {
      fail("warning-says-what", `${loc} [${ctx}]`, "Warning must state what did not happen", text);
    }
  }
});

if (violations.length > 0) {
  console.error(`  [audit]  FAILED with ${violations.length} violation(s):`);
  for (const v of violations.slice(0, 40)) {
    console.error(`    - (${v.rule}) ${v.loc}: ${v.detail}\n        "${v.text}"`);
  }
  if (violations.length > 40) {
    console.error(`    ... and ${violations.length - 40} more`);
  }
  process.exit(1);
}

console.log(`  [audit]  PASSED 0 violations across ${rawRows.length} utterances + words.tsv`);
