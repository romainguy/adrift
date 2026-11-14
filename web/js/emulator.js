// SPDX-License-Identifier: MPL-2.0
// Browser emulator coordinator: AudioContext + AudioWorklet, WebMIDI input, QWERTY piano & User Preset persistence.

import { Fm1Panel } from './panel.js';

const STORAGE_KEY = 'adrift_user_flash_v1';
const FLASH_BYTES = 25 * 1024;

// Text editing commands matching platform.h ADRIFT_TEXT_CMD_*
const TEXT_CMD = {
  NONE: 0,
  BACKSPACE: 1,
  DELETE: 2,
  LEFT: 3,
  RIGHT: 4,
  CLEAR: 5,
};

// QWERTY piano mapping -> [slotId, semitonesFromC4]
const PIANO_CODE_MAP = {
  KeyZ: [0, 0],
  KeyS: [1, 1],
  KeyX: [2, 2],
  KeyD: [3, 3],
  KeyC: [4, 4],
  KeyV: [5, 5],
  KeyG: [6, 6],
  KeyB: [7, 7],
  KeyH: [8, 8],
  KeyN: [9, 9],
  KeyJ: [10, 10],
  KeyM: [11, 11],
  Comma: [12, 12],
  KeyQ: [13, 12],
  KeyW: [14, 14],
  KeyE: [15, 16],
  KeyR: [16, 17],
  KeyT: [17, 19],
  KeyY: [18, 21],
  KeyU: [19, 23],
  KeyI: [20, 24],
};

function bytesToBase64(bytes) {
  let bin = '';
  const chunk = 0x8000;
  for (let i = 0; i < bytes.length; i += chunk) {
    bin += String.fromCharCode.apply(null, bytes.subarray(i, i + chunk));
  }
  return btoa(bin);
}

function base64ToBytes(b64) {
  const bin = atob(b64);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) {
    out[i] = bin.charCodeAt(i);
  }
  return out;
}

export class AdriftEmulator {
  constructor(canvas, callbacks = {}) {
    this.canvas = canvas;
    this.cb = callbacks;

    this.audioCtx = null;
    this.workletNode = null;
    this.wasmBytesPromise = fetch('./adrift.wasm').then((r) => {
      if (!r.ok) throw new Error(`Failed to fetch adrift.wasm (${r.status})`);
      return r.arrayBuffer();
    });

    this.startingPromise = null;
    this.ready = false;
    this.muted = false;
    this.inSaveMenu = false;
    this.activePreset = 0;
    this.activeGroup = 0;
    this.pendingWorkletMsgs = [];
    this.heldPianoCodes = new Set();
    this.previewRaf = 0;
    this.previewBootFrame = 0;
    this.previewCompleted = false;

    this.midiAccess = null;
    this.selectedMidiId = 'all';
    this.midiPaused = false;

    this.flashBytes = this._loadLocalFlash();

    this.panel = new Fm1Panel(canvas, {
      onInteraction: () => {
        this.ensureStarted();
      },
      onPanelKey: (key, down, velocity) => {
        this._post({ type: 'panelKey', key, down, velocity });
      },
      onEncoder: (index, delta) => {
        this._post({ type: 'enc', index, delta });
      },
      onHwButton: (index, down) => {
        this._post({ type: 'hwBtn', index, down });
      },
      onMasterPot: (value) => {
        this._post({ type: 'masterPot', value });
      },
      onLcdClick: (x, y, right) => {
        this._post({ type: 'lcdClick', x, y, right });
      },
    });

    this._bindKeyboard();
    this._initOptionalWebMidi();
    this._startBootPreview();
  }

  _stopBootPreview() {
    if (this.previewRaf) {
      cancelAnimationFrame(this.previewRaf);
      this.previewRaf = 0;
    }
  }

  _startBootPreview() {
    this.wasmBytesPromise
      .then((wasmBytes) => WebAssembly.instantiate(wasmBytes, {}))
      .then((res) => {
        if (this.startingPromise || this.workletNode) return;
        const inst = res.instance || res;
        const ex = inst.exports;
        const mem = ex.memory;

        ex.adrift_web_storage_reset_ram();
        if (this.flashBytes && this.flashBytes.byteLength) {
          const ptr = ex.adrift_web_storage_ptr();
          const cap = ex.adrift_web_storage_len();
          new Uint8Array(mem.buffer, ptr, cap).set(
            this.flashBytes.subarray(0, Math.min(this.flashBytes.length, cap))
          );
        }
        ex.adrift_web_init();

        let settleTicks = 15;
        let lastMs = 0;
        const step = (now) => {
          this.previewRaf = 0;
          if (this.startingPromise || this.workletNode) return;
          if (lastMs === 0 || now - lastMs >= 14.5) {
            lastMs = now;
            ex.adrift_web_tick_ui(8);
            const bf = ex.adrift_web_boot_frame ? (ex.adrift_web_boot_frame() >>> 0) : 0;
            this.previewBootFrame = bf > 0 ? bf : 68;

            const rgbaPtr = ex.adrift_web_rgba_ptr();
            const rgbaCopy = new Uint8ClampedArray(mem.buffer, rgbaPtr, 240 * 240 * 4).slice();
            this.panel.updateFromWorklet({
              rgba: rgbaCopy.buffer,
              btnLeds: ex.adrift_web_leds_buttons() >>> 0,
              keyLeds: 0,
              activeGroup: ex.adrift_web_active_group() >>> 0,
              activePreset: ex.adrift_web_active_preset() >>> 0,
              inSaveMenu: false,
              bootFrame: bf,
            });

            if (bf === 0) {
              this.previewCompleted = true;
              if (settleTicks > 0) {
                settleTicks--;
              } else {
                return;
              }
            }
          }
          this.previewRaf = requestAnimationFrame(step);
        };
        this.previewRaf = requestAnimationFrame(step);
      })
      .catch(() => {
        // Ignore preview errors; ensureStarted() reports any real load failure.
      });
  }

  _loadLocalFlash() {
    try {
      const raw = localStorage.getItem(STORAGE_KEY);
      if (raw) {
        const bytes = base64ToBytes(raw);
        if (bytes.length === FLASH_BYTES) {
          return bytes;
        }
      }
    } catch (_) {
      // Ignore storage quota/privacy restrictions
    }
    const blank = new Uint8Array(FLASH_BYTES);
    blank.fill(0xff);
    return blank;
  }

  _saveLocalFlash(bytes) {
    this.flashBytes = new Uint8Array(bytes);
    try {
      localStorage.setItem(STORAGE_KEY, bytesToBase64(this.flashBytes));
    } catch (_) {
      // Ignore quota errors
    }
    if (this.cb.onStorageSaved) {
      this.cb.onStorageSaved(this.countOccupiedUserPresets());
    }
  }

  countOccupiedUserPresets() {
    let count = 0;
    for (let slot = 0; slot < 24; slot++) {
      const off = slot * 1024;
      if (
        this.flashBytes[off] === 0x41 && // 'A'
        this.flashBytes[off + 1] === 0x44 && // 'D'
        this.flashBytes[off + 2] === 0x52 && // 'R'
        this.flashBytes[off + 3] === 0x50 && // 'P'
        (this.flashBytes[off + 7] & 0x01) !== 0
      ) {
        count++;
      }
    }
    return count;
  }

  async ensureStarted() {
    if (this.audioCtx && this.ready) {
      if (this.audioCtx.state === 'suspended') {
        await this.audioCtx.resume();
      }
      this.panel.setAudioRunning(true);
      if (this.cb.onAudioState) this.cb.onAudioState(true, this.muted);
      return;
    }
    if (this.startingPromise) {
      return this.startingPromise;
    }

    const resumeBootFrame =
      this.previewCompleted || this.pendingWorkletMsgs.length > 0 ? 68 : this.previewBootFrame;
    this._stopBootPreview();

    this.startingPromise = (async () => {
      const AudioCtx = window.AudioContext || window.webkitAudioContext;
      if (!this.audioCtx) {
        this.audioCtx = new AudioCtx({ latencyHint: 'interactive' });
      }
      if (this.audioCtx.state === 'suspended') {
        await this.audioCtx.resume();
      }

      const wasmBytes = await this.wasmBytesPromise;
      await this.audioCtx.audioWorklet.addModule('./js/worklet.js');

      const initialFlashCopy = this.flashBytes.slice().buffer;
      const initialBootFrame = this.pendingWorkletMsgs.length > 0 ? 68 : resumeBootFrame;
      const node = new AudioWorkletNode(this.audioCtx, 'adrift-processor', {
        numberOfInputs: 0,
        numberOfOutputs: 1,
        outputChannelCount: [2],
        processorOptions: {
          wasmBytes,
          initialFlash: initialFlashCopy,
          initialBootFrame,
        },
      });

      node.port.onmessage = (ev) => {
        const msg = ev.data;
        if (!msg) return;
        if (msg.type === 'frame') {
          this.inSaveMenu = msg.inSaveMenu;
          const prevPreset = this.activePreset;
          const prevGroup = this.activeGroup;
          this.activePreset = msg.activePreset;
          this.activeGroup = msg.activeGroup;
          this.panel.updateFromWorklet(msg);
          if ((prevPreset !== this.activePreset || prevGroup !== this.activeGroup) && this.cb.onStateChange) {
            this.cb.onStateChange({
              activePreset: this.activePreset,
              activeGroup: this.activeGroup,
            });
          }
        } else if (msg.type === 'storage') {
          this._saveLocalFlash(new Uint8Array(msg.flash));
        } else if (msg.type === 'ready') {
          this.ready = true;
          this.panel.setAudioRunning(true);
          if (this.cb.onAudioState) this.cb.onAudioState(true, this.muted);
        } else if (msg.type === 'error' && this.cb.onError) {
          this.cb.onError(msg.message);
        }
      };

      node.connect(this.audioCtx.destination);
      this.workletNode = node;

      for (const queued of this.pendingWorkletMsgs) {
        node.port.postMessage(queued);
      }
      this.pendingWorkletMsgs.length = 0;

      this.panel.setAudioRunning(true);
      if (this.cb.onAudioState) this.cb.onAudioState(true, this.muted);
    })();

    try {
      await this.startingPromise;
    } finally {
      this.startingPromise = null;
    }
  }

  _post(msg, transfer) {
    if (this.workletNode) {
      if (transfer) {
        this.workletNode.port.postMessage(msg, transfer);
      } else {
        this.workletNode.port.postMessage(msg);
      }
    } else {
      this.pendingWorkletMsgs.push(msg);
    }
  }

  async loadPreset(presetIdx) {
    await this.ensureStarted();
    this._post({ type: 'loadPreset', index: presetIdx & 0xff });
  }

  panic() {
    this.heldPianoCodes.clear();
    this._post({ type: 'panic' });
  }

  toggleMute() {
    this.muted = !this.muted;
    this._post({ type: 'mute', muted: this.muted });
    if (this.cb.onAudioState) {
      this.cb.onAudioState(Boolean(this.audioCtx && this.audioCtx.state === 'running'), this.muted);
    }
    return this.muted;
  }

  setQwertyHints(enabled) {
    this.panel.setQwertyHints(enabled);
  }

  exportUserPresetsBlob() {
    return new Blob([this.flashBytes], { type: 'application/octet-stream' });
  }

  async importUserPresetsBytes(uint8) {
    if (!uint8 || uint8.byteLength < 1024) {
      throw new Error('Backup file is too small to contain Adrift user presets.');
    }
    const next = new Uint8Array(FLASH_BYTES);
    next.fill(0xff);
    next.set(uint8.subarray(0, Math.min(uint8.length, FLASH_BYTES)));
    this._saveLocalFlash(next);
    if (this.workletNode) {
      const copy = next.slice().buffer;
      this._post({ type: 'setFlash', flash: copy }, [copy]);
    }
  }

  resetUserPresets() {
    const blank = new Uint8Array(FLASH_BYTES);
    blank.fill(0xff);
    this._saveLocalFlash(blank);
    if (this.workletNode) {
      this._post({ type: 'resetFlash' });
    }
  }

  setMidiPaused(paused) {
    this.midiPaused = Boolean(paused);
    if (paused) {
      this.panic();
    }
  }

  setMidiInputId(id) {
    this.selectedMidiId = id || 'all';
  }

  async _initOptionalWebMidi() {
    if (!navigator.requestMIDIAccess) {
      if (this.cb.onMidiInputs) this.cb.onMidiInputs([], false);
      return;
    }
    try {
      const access = await navigator.requestMIDIAccess({ sysex: false });
      this._attachMidiAccess(access);
    } catch (_) {
      if (this.cb.onMidiInputs) this.cb.onMidiInputs([], true);
    }
  }

  async requestMidiControllerAccess() {
    if (!navigator.requestMIDIAccess) return false;
    try {
      const access = await navigator.requestMIDIAccess({ sysex: false });
      this._attachMidiAccess(access);
      return true;
    } catch (_) {
      return false;
    }
  }

  _attachMidiAccess(access) {
    this.midiAccess = access;
    const refreshPorts = () => {
      const list = [];
      for (const input of access.inputs.values()) {
        if (input.state !== 'connected') continue;
        list.push({ id: input.id, name: input.name || 'MIDI Input' });
        input.onmidimessage = (ev) => this._onExternalMidiMessage(input.id, ev.data);
      }
      if (this.cb.onMidiInputs) {
        this.cb.onMidiInputs(list, true);
      }
    };
    access.onstatechange = refreshPorts;
    refreshPorts();
  }

  _onExternalMidiMessage(portId, data) {
    if (this.midiPaused || !data || data.length === 0) return;
    if (this.selectedMidiId !== 'all' && this.selectedMidiId !== portId) return;

    const status = data[0];
    // Ignore SysEx messages in synth input path
    if (status === 0xf0 || status === 0xf7) return;

    // Auto-start AudioContext if already allowed, else post once started
    if (!this.ready && status >= 0x80 && status < 0xf0) {
      this.ensureStarted().catch(() => {});
    }

    if (status >= 0xf8) {
      if (status === 0xf8 || status === 0xfa || status === 0xfb || status === 0xfc) {
        this._post({ type: 'midi', status, d1: 0, d2: 0 });
      }
      return;
    }

    if (status >= 0x80 && status < 0xf0) {
      const d1 = data.length > 1 ? data[1] & 0x7f : 0;
      const d2 = data.length > 2 ? data[2] & 0x7f : 0;
      this._post({ type: 'midi', status, d1, d2 });
    }
  }

  _shouldIgnoreKeyEvent(e) {
    const el = e.target;
    if (!el) return false;
    const tag = el.tagName;
    if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT') return true;
    if (el.isContentEditable) return true;
    if (typeof el.closest === 'function' && el.closest('dialog[open]')) return true;
    return false;
  }

  _bindKeyboard() {
    window.addEventListener('keydown', (e) => {
      if (this._shouldIgnoreKeyEvent(e)) return;

      const cmdCtrl = e.metaKey || e.ctrlKey;
      if (cmdCtrl) {
        if (e.code === 'KeyS' && this.ready) {
          e.preventDefault();
          this._post({ type: 'hwBtnPulse', index: 9 }); // SAVE
        }
        return;
      }
      if (e.altKey) return;

      // Direct QWERTY preset-name typing while inside SAVE modal
      if (this.inSaveMenu && this.ready) {
        if (e.code === 'Enter' || e.code === 'NumpadEnter') {
          e.preventDefault();
          if (!e.repeat) this._post({ type: 'hwBtnPulse', index: 1 }); // OCT+ Confirm
          return;
        }
        if (e.code === 'Escape') {
          e.preventDefault();
          if (!e.repeat) this._post({ type: 'hwBtnPulse', index: 0 }); // OCT- Cancel
          return;
        }
        if (e.code === 'Backspace') {
          e.preventDefault();
          this._post({ type: 'text', ascii: 0, cmd: TEXT_CMD.BACKSPACE });
          return;
        }
        if (e.code === 'Delete') {
          e.preventDefault();
          this._post({ type: 'text', ascii: 0, cmd: TEXT_CMD.DELETE });
          return;
        }
        if (e.code === 'ArrowLeft') {
          e.preventDefault();
          this._post({ type: 'text', ascii: 0, cmd: TEXT_CMD.LEFT });
          return;
        }
        if (e.code === 'ArrowRight') {
          e.preventDefault();
          this._post({ type: 'text', ascii: 0, cmd: TEXT_CMD.RIGHT });
          return;
        }
        if (e.code === 'ArrowUp') {
          e.preventDefault();
          this.panel.nudgeEncoderAngle(5, -1);
          this._post({ type: 'enc', index: 5, delta: -1 });
          return;
        }
        if (e.code === 'ArrowDown') {
          e.preventDefault();
          this.panel.nudgeEncoderAngle(5, 1);
          this._post({ type: 'enc', index: 5, delta: 1 });
          return;
        }
        if (e.key && e.key.length === 1) {
          const ch = e.key.toUpperCase().charCodeAt(0);
          if (ch >= 0x20 && ch <= 0x5f) {
            e.preventDefault();
            this._post({ type: 'text', ascii: ch, cmd: TEXT_CMD.NONE });
            return;
          }
        }
        return;
      }

      // Piano keys (auto-starts audio on first key press!)
      const piano = PIANO_CODE_MAP[e.code];
      if (piano) {
        e.preventDefault();
        if (e.repeat || this.heldPianoCodes.has(e.code)) return;
        this.heldPianoCodes.add(e.code);
        this.ensureStarted();
        this._post({
          type: 'qwertyNote',
          slot: piano[0],
          semi: piano[1],
          down: true,
          velocity: 104,
        });
        return;
      }

      // Panel navigation shortcuts (only intercept when emulator audio is active or canvas focused)
      if (!this.ready && document.activeElement !== this.canvas) return;
      if (e.repeat) return;

      switch (e.code) {
        case 'Digit1':
          e.preventDefault();
          this.ensureStarted();
          this._post({ type: 'hwBtnPulse', index: 3 }); // EXC
          break;
        case 'Digit2':
          e.preventDefault();
          this.ensureStarted();
          this._post({ type: 'hwBtnPulse', index: 4 }); // ENV1
          break;
        case 'Digit3':
          e.preventDefault();
          this.ensureStarted();
          this._post({ type: 'hwBtnPulse', index: 5 }); // BBD
          break;
        case 'Digit4':
          e.preventDefault();
          this.ensureStarted();
          this._post({ type: 'hwBtnPulse', index: 6 }); // ENV2
          break;
        case 'Digit5':
          e.preventDefault();
          this.ensureStarted();
          this._post({ type: 'hwBtnPulse', index: 7 }); // MOD
          break;
        case 'Digit6':
          e.preventDefault();
          this.ensureStarted();
          this._post({ type: 'hwBtnPulse', index: 8 }); // HOME
          break;
        case 'Digit7':
          e.preventDefault();
          this.ensureStarted();
          this.panel.nudgeEncoderAngle(4, e.shiftKey ? -1 : 1);
          this._post({ type: 'enc', index: 4, delta: e.shiftKey ? -1 : 1 }); // SELECT Group
          break;
        case 'Digit8':
        case 'Backslash':
          e.preventDefault();
          this.ensureStarted();
          this._post({ type: 'hwBtnPulse', index: 9 }); // SAVE
          break;
        case 'Digit9':
        case 'KeyA':
          e.preventDefault();
          this.ensureStarted();
          this._post({ type: 'hwBtnPulse', index: 10 }); // ARP
          break;
        case 'Digit0':
        case 'Backquote':
          e.preventDefault();
          this.ensureStarted();
          this._post({ type: 'hwBtnPulse', index: 11 }); // SEQ / GRP
          break;
        case 'KeyF':
        case 'Slash':
          e.preventDefault();
          this.ensureStarted();
          this._post({ type: 'hwBtnPulse', index: 2 }); // FX
          break;
        case 'Minus':
          e.preventDefault();
          this.ensureStarted();
          this.panel.nudgeEncoderAngle(5, -1);
          this._post({ type: 'enc', index: 5, delta: -1 }); // PRESETS -
          break;
        case 'Equal':
          e.preventDefault();
          this.ensureStarted();
          this.panel.nudgeEncoderAngle(5, 1);
          this._post({ type: 'enc', index: 5, delta: 1 }); // PRESETS +
          break;
        case 'Semicolon':
          e.preventDefault();
          this.ensureStarted();
          this.panel.nudgeEncoderAngle(6, -1);
          this._post({ type: 'enc', index: 6, delta: -1 }); // SCALE -
          break;
        case 'Quote':
          e.preventDefault();
          this.ensureStarted();
          this.panel.nudgeEncoderAngle(6, 1);
          this._post({ type: 'enc', index: 6, delta: 1 }); // SCALE +
          break;
        case 'BracketLeft':
          e.preventDefault();
          this.ensureStarted();
          this._post({ type: 'hwBtnPulse', index: 0 }); // OCT -
          break;
        case 'BracketRight':
          e.preventDefault();
          this.ensureStarted();
          this._post({ type: 'hwBtnPulse', index: 1 }); // OCT +
          break;
        case 'Backspace':
          e.preventDefault();
          this.panic();
          break;
        case 'Space':
          if (document.activeElement === this.canvas) {
            e.preventDefault();
            this.panic();
          }
          break;
        default:
          break;
      }
    });

    window.addEventListener('keyup', (e) => {
      const piano = PIANO_CODE_MAP[e.code];
      if (piano && this.heldPianoCodes.has(e.code)) {
        this.heldPianoCodes.delete(e.code);
        this._post({
          type: 'qwertyNote',
          slot: piano[0],
          semi: piano[1],
          down: false,
          velocity: 0,
        });
      }
    });

    window.addEventListener('blur', () => {
      if (this.heldPianoCodes.size > 0) {
        for (const code of this.heldPianoCodes) {
          const piano = PIANO_CODE_MAP[code];
          if (piano) {
            this._post({
              type: 'qwertyNote',
              slot: piano[0],
              semi: piano[1],
              down: false,
              velocity: 0,
            });
          }
        }
        this.heldPianoCodes.clear();
      }
    });
  }
}
