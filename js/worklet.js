// SPDX-License-Identifier: MPL-2.0
// AudioWorkletProcessor hosting Adrift's freestanding C99 DSP & UI engine (adrift.wasm).

const SRC_RATE = 24000000 / 544; // 44,117.6470588 Hz hardware I2S clock
const RING_SIZE = 2048;
const RING_MASK = RING_SIZE - 1;
const UI_TICK_SAMPLES = 1470; // ~30.01 Hz + LED refresh

class AdriftProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    this.ready = false;
    this.exports = null;
    this.mem = null;

    this.ringL = new Float32Array(RING_SIZE);
    this.ringR = new Float32Array(RING_SIZE);
    this.srcWritten = 0;
    this.srcPos = 0.0;
    this.step = SRC_RATE / sampleRate;

    this.uiSampleAccum = 0;
    this.dspWallAccumMs = 0;
    this.lastUiWallMs = Date.now();
    this.cpuPct = 8;

    this.muted = false;
    this.muteGain = 1.0;

    this.pendingMsgs = [];
    this.port.onmessage = (ev) => {
      if (!this.ready) {
        this.pendingMsgs.push(ev.data);
        return;
      }
      this._handleMsg(ev.data);
    };

    const opts = (options && options.processorOptions) || {};
    this._initWasm(opts.wasmBytes, opts.initialFlash);
  }

  async _initWasm(wasmBytes, initialFlash) {
    try {
      const res = await WebAssembly.instantiate(wasmBytes, {});
      const inst = res.instance || res;
      this.exports = inst.exports;
      this.mem = this.exports.memory;

      this.exports.adrift_web_storage_reset_ram();
      if (initialFlash && initialFlash.byteLength) {
        const ptr = this.exports.adrift_web_storage_ptr();
        const cap = this.exports.adrift_web_storage_len();
        const src = new Uint8Array(initialFlash);
        const dst = new Uint8Array(this.mem.buffer, ptr, cap);
        dst.set(src.subarray(0, Math.min(src.length, cap)));
      }

      this.exports.adrift_web_init();

      // Prime initial interpolation history so Hermite cubic starts cleanly at sample 0
      this._produceSourceFrames(64);
      this.srcPos = 2.0;
      this.ready = true;

      for (let i = 0; i < this.pendingMsgs.length; i++) {
        this._handleMsg(this.pendingMsgs[i]);
      }
      this.pendingMsgs.length = 0;

      this._emitUiFrame();
      this.port.postMessage({ type: 'ready' });
    } catch (err) {
      this.port.postMessage({
        type: 'error',
        message: err && err.message ? err.message : String(err),
      });
    }
  }

  _handleMsg(msg) {
    if (!msg || !this.exports) return;
    switch (msg.type) {
      case 'midi':
        this.exports.adrift_web_push_midi(msg.status & 0xff, msg.d1 & 0x7f, msg.d2 & 0x7f);
        break;
      case 'panelKey':
        this.exports.adrift_web_panel_key(msg.key & 0xff, msg.down ? 1 : 0, msg.velocity & 0x7f);
        break;
      case 'qwertyNote':
        this.exports.adrift_web_qwerty_note(
          msg.slot & 0xff,
          msg.semi | 0,
          msg.down ? 1 : 0,
          msg.velocity & 0x7f
        );
        break;
      case 'enc':
        this.exports.adrift_web_enc_delta(msg.index & 0xff, msg.delta | 0);
        break;
      case 'hwBtn':
        this.exports.adrift_web_hw_button(msg.index & 0xff, msg.down ? 1 : 0);
        break;
      case 'hwBtnPulse':
        this.exports.adrift_web_hw_button_pulse(msg.index & 0xff);
        break;
      case 'masterPot':
        this.exports.adrift_web_set_master_pot(msg.value | 0);
        break;
      case 'text':
        this.exports.adrift_web_text_input(msg.ascii & 0xff, msg.cmd & 0xff);
        break;
      case 'lcdClick':
        this.exports.adrift_web_lcd_click(msg.x | 0, msg.y | 0, msg.right ? 1 : 0);
        break;
      case 'loadPreset':
        this.exports.adrift_web_load_preset(msg.index & 0xff);
        this._emitUiFrame();
        break;
      case 'panic':
        this.exports.adrift_web_panic();
        break;
      case 'mute':
        this.muted = Boolean(msg.muted);
        break;
      case 'setFlash': {
        const ptr = this.exports.adrift_web_storage_ptr();
        const cap = this.exports.adrift_web_storage_len();
        const dst = new Uint8Array(this.mem.buffer, ptr, cap);
        dst.fill(0xff);
        if (msg.flash && msg.flash.byteLength) {
          const src = new Uint8Array(msg.flash);
          dst.set(src.subarray(0, Math.min(src.length, cap)));
        }
        this.exports.adrift_web_storage_reload();
        this._emitStorage();
        this._emitUiFrame();
        break;
      }
      case 'resetFlash':
        this.exports.adrift_web_storage_reset_ram();
        this.exports.adrift_web_storage_reload();
        this._emitStorage();
        this._emitUiFrame();
        break;
      default:
        break;
    }
  }

  _produceSourceFrames(frames) {
    const t0 = Date.now();
    this.exports.adrift_web_render_audio(frames);
    const lPtr = this.exports.adrift_web_audio_l_ptr();
    const rPtr = this.exports.adrift_web_audio_r_ptr();
    const bufL = new Float32Array(this.mem.buffer, lPtr, frames);
    const bufR = new Float32Array(this.mem.buffer, rPtr, frames);

    let w = this.srcWritten;
    for (let i = 0; i < frames; i++) {
      const idx = w & RING_MASK;
      this.ringL[idx] = bufL[i];
      this.ringR[idx] = bufR[i];
      w++;
    }
    this.srcWritten = w;
    this.dspWallAccumMs += Math.max(0, Date.now() - t0);
  }

  _emitStorage() {
    const ptr = this.exports.adrift_web_storage_ptr();
    const len = this.exports.adrift_web_storage_len();
    const copy = new Uint8Array(this.mem.buffer, ptr, len).slice();
    this.port.postMessage({ type: 'storage', flash: copy.buffer }, [copy.buffer]);
  }

  _emitUiFrame() {
    const now = Date.now();
    const wallSpan = Math.max(16, now - this.lastUiWallMs);
    this.cpuPct = Math.min(99, Math.max(2, Math.round((this.dspWallAccumMs / wallSpan) * 100)));
    this.dspWallAccumMs = 0;
    this.lastUiWallMs = now;

    this.exports.adrift_web_tick_ui(this.cpuPct);

    if (this.exports.adrift_web_take_storage_dirty() !== 0) {
      this._emitStorage();
    }

    const rgbaPtr = this.exports.adrift_web_rgba_ptr();
    const rgbaCopy = new Uint8ClampedArray(this.mem.buffer, rgbaPtr, 240 * 240 * 4).slice();
    this.port.postMessage(
      {
        type: 'frame',
        rgba: rgbaCopy.buffer,
        btnLeds: this.exports.adrift_web_leds_buttons() >>> 0,
        keyLeds: this.exports.adrift_web_leds_keys() >>> 0,
        activeGroup: this.exports.adrift_web_active_group() >>> 0,
        activePreset: this.exports.adrift_web_active_preset() >>> 0,
        inSaveMenu: this.exports.adrift_web_in_save_menu() !== 0,
      },
      [rgbaCopy.buffer]
    );
  }

  process(_inputs, outputs) {
    const out = outputs[0];
    if (!out || out.length === 0) return true;
    const outL = out[0];
    const outR = out.length > 1 ? out[1] : null;
    const n = outL.length;

    if (!this.ready) {
      outL.fill(0);
      if (outR) outR.fill(0);
      return true;
    }

    const maxNeededIdx = Math.floor(this.srcPos + (n - 1) * this.step) + 3;
    while (this.srcWritten <= maxNeededIdx) {
      this._produceSourceFrames(64);
      this.uiSampleAccum += 64;
    }

    const targetGain = this.muted ? 0.0 : 1.0;
    let gain = this.muteGain;
    let pos = this.srcPos;
    const step = this.step;
    const rL = this.ringL;
    const rR = this.ringR;

    for (let i = 0; i < n; i++) {
      gain += (targetGain - gain) * 0.015;
      const base = Math.floor(pos);
      const t = pos - base;

      const i0 = (base - 1) & RING_MASK;
      const i1 = base & RING_MASK;
      const i2 = (base + 1) & RING_MASK;
      const i3 = (base + 2) & RING_MASK;

      // 4-point 3rd-order Hermite interpolation
      const l0 = rL[i0], l1 = rL[i1], l2 = rL[i2], l3 = rL[i3];
      const lc1 = 0.5 * (l2 - l0);
      const lc2 = l0 - 2.5 * l1 + 2.0 * l2 - 0.5 * l3;
      const lc3 = 0.5 * (l3 - l0) + 1.5 * (l1 - l2);
      const sL = (((lc3 * t + lc2) * t + lc1) * t + l1) * gain;

      const q0 = rR[i0], q1 = rR[i1], q2 = rR[i2], q3 = rR[i3];
      const qc1 = 0.5 * (q2 - q0);
      const qc2 = q0 - 2.5 * q1 + 2.0 * q2 - 0.5 * q3;
      const qc3 = 0.5 * (q3 - q0) + 1.5 * (q1 - q2);
      const sR = (((qc3 * t + qc2) * t + qc1) * t + q1) * gain;

      outL[i] = sL;
      if (outR) outR[i] = sR;
      pos += step;
    }

    this.srcPos = pos;
    this.muteGain = gain;

    if (this.uiSampleAccum >= UI_TICK_SAMPLES) {
      this.uiSampleAccum %= UI_TICK_SAMPLES;
      this._emitUiFrame();
    }

    return true;
  }
}

registerProcessor('adrift-processor', AdriftProcessor);
