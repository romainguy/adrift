// SPDX-License-Identifier: MPL-2.0
// Interactive HiDPI M-VAVE FM-1 front-panel renderer & multi-touch input controller.

export const PANEL_W = 1280;
export const PANEL_H = 800;

export const GROUP_COLORS = [
  '#F8BE30', // G1 Warm Gold
  '#40E88C', // G2 Lush Emerald
  '#38D8F8', // G3 Electric Cyan
  '#FC6C56', // G4 Sunset Coral
  '#F464C6', // G5 Synthwave Magenta
  '#A884FF', // G6 Luminous Violet
];

const LCD_RECT = { x: 388, y: 44, w: 480, h: 480 };
const BEZEL_RECT = { x: 368, y: 28, w: 520, h: 512, r: 16 };

const KNOBS = [
  // Left deck: Master pot + 3 navigation encoders
  { id: 'master', kind: 'pot', enc: -1, x: 112, y: 132, r: 42, label: 'MASTER', sub: 'VOLUME' },
  { id: 'select', kind: 'enc', enc: 4, x: 256, y: 132, r: 42, label: 'SELECT', sub: 'GROUP G1–6' },
  { id: 'presets', kind: 'enc', enc: 5, x: 112, y: 294, r: 42, label: 'PRESETS', sub: 'P1–24 · U1–24' },
  { id: 'algo', kind: 'enc', enc: 6, x: 256, y: 294, r: 42, label: 'ALGORITHM', sub: 'KEY SCALE' },
  // Right deck: 4 parameter encoders
  { id: 'knob1', kind: 'enc', enc: 0, x: 942, y: 124, r: 34, label: '1', sub: 'PARAM 1' },
  { id: 'knob2', kind: 'enc', enc: 1, x: 1030, y: 124, r: 34, label: '2', sub: 'PARAM 2' },
  { id: 'knob3', kind: 'enc', enc: 2, x: 1118, y: 124, r: 34, label: '3', sub: 'PARAM 3' },
  { id: 'knob4', kind: 'enc', enc: 3, x: 1206, y: 124, r: 34, label: '4', sub: 'PARAM 4' },
];

const BUTTONS = [
  { hw: 0, x: 58, y: 410, w: 118, h: 52, r: 10, label: 'OCT -', sub: 'CANCEL' },
  { hw: 1, x: 192, y: 410, w: 118, h: 52, r: 10, label: 'OCT +', sub: 'APPLY / OK' },

  // Top row of 2x6 matrix
  { hw: 2, x: 908, y: 236, w: 50, h: 58, r: 8, label: 'FX', sub: 'BUS FX' },
  { hw: 3, x: 966, y: 236, w: 50, h: 58, r: 8, label: 'SCL', sub: 'EXCITER' },
  { hw: 4, x: 1024, y: 236, w: 50, h: 58, r: 8, label: 'ENV', sub: 'ENV 1' },
  { hw: 5, x: 1082, y: 236, w: 50, h: 58, r: 8, label: 'LFO', sub: 'BBD RES' },
  { hw: 6, x: 1140, y: 236, w: 50, h: 58, r: 8, label: 'EDIT', sub: 'ENV 2' },
  { hw: 7, x: 1198, y: 236, w: 50, h: 58, r: 8, label: 'GLO', sub: 'MOD · LFO' },

  // Bottom row of 2x6 matrix
  { hw: 8, x: 908, y: 332, w: 50, h: 58, r: 8, label: 'HOME', sub: 'SCOPES' },
  { hw: 9, x: 966, y: 332, w: 50, h: 58, r: 8, label: 'SAVE', sub: 'SAVE PST' },
  { hw: 10, x: 1024, y: 332, w: 50, h: 58, r: 8, label: 'ARP', sub: 'ARP' },
  { hw: 11, x: 1082, y: 332, w: 50, h: 58, r: 8, label: 'SEQ', sub: 'GROUPS' },
  { hw: 12, x: 1140, y: 332, w: 50, h: 58, r: 8, label: 'PLAY', sub: 'PANIC' },
  { hw: 13, x: 1198, y: 332, w: 50, h: 58, r: 8, label: 'REC', sub: '' },
];

// 27 pill keys: F3 (k=0) .. G5 (k=26); C4 is k=7, C5 is k=19
const WHITE_INDICES = [0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26];
const BLACK_SPECS = [
  [1, 0], [3, 1], [5, 2],
  [8, 4], [10, 5],
  [13, 7], [15, 8], [17, 9],
  [20, 11], [22, 12],
  [25, 14],
];

const QWERTY_LABELS = {
  7: 'Z', 8: 'S', 9: 'X', 10: 'D', 11: 'C', 12: 'V',
  13: 'G', 14: 'B', 15: 'H', 16: 'N', 17: 'J', 18: 'M',
  19: 'Q', 21: 'W', 23: 'E', 24: 'R', 26: 'T',
};

const NOTE_LABELS = {
  0: 'F3',
  7: 'C4',
  19: 'C5',
  26: 'G5',
};

function buildKeys() {
  const keys = new Array(27);
  WHITE_INDICES.forEach((k, i) => {
    keys[k] = {
      k,
      black: false,
      x: 62 + i * 73,
      y: 664,
      w: 60,
      h: 104,
      r: 14,
    };
  });
  BLACK_SPECS.forEach(([k, after]) => {
    const cx = 62 + (after + 1) * 73 - 6.5;
    keys[k] = {
      k,
      black: true,
      x: cx - 27,
      y: 562,
      w: 54,
      h: 88,
      r: 13,
    };
  });
  return keys;
}

const KEYS = buildKeys();

function roundRectPath(ctx, x, y, w, h, r) {
  ctx.beginPath();
  if (typeof ctx.roundRect === 'function') {
    ctx.roundRect(x, y, w, h, r);
  } else {
    const rr = Math.min(r, w * 0.5, h * 0.5);
    ctx.moveTo(x + rr, y);
    ctx.arcTo(x + w, y, x + w, y + h, rr);
    ctx.arcTo(x + w, y + h, x, y + h, rr);
    ctx.arcTo(x, y + h, x, y, rr);
    ctx.arcTo(x, y, x + w, y, rr);
    ctx.closePath();
  }
}

export class Fm1Panel {
  constructor(canvas, callbacks) {
    this.canvas = canvas;
    this.cb = callbacks;
    this.ctx = canvas.getContext('2d', { alpha: false });

    // Offscreen 240x240 buffer for crisp pixel-nearest blitting
    this.lcdCanvas = document.createElement('canvas');
    this.lcdCanvas.width = 240;
    this.lcdCanvas.height = 240;
    this.lcdCtx = this.lcdCanvas.getContext('2d');
    this.lcdImageData = this.lcdCtx.createImageData(240, 240);
    this.hasLiveLcd = false;
    this.placeholderImg = null;

    this.btnLeds = 0;
    this.keyLeds = 0;
    this.bootFrame = 0;
    this.activeGroup = 0;
    this.showQwertyHints = true;
    this.showAdriftLabels = true;
    this.audioRunning = false;

    // Visual angles in degrees: master pot [-135..+135], encoders unbounded
    this.masterPot = 900; // 0..1023
    this.encAngles = [0, 0, 0, 0, 0, 0, 0];
    this.pressedButtons = new Set();
    this.pressedKeys = new Set();

    // Active pointer map: pointerId -> gesture descriptor
    this.pointers = new Map();
    this.rafPending = false;

    this._setupResize();
    this._bindEvents();
    this._loadBootPreview();
    this.requestDraw();
  }

  _loadBootPreview() {
    const img = new Image();
    img.onload = () => {
      if (!this.hasLiveLcd) {
        this.placeholderImg = img;
        this.requestDraw();
      }
    };
    img.src = './assets/screens/01_home_scope.png';
  }

  _setupResize() {
    const updateSize = () => {
      const dpr = Math.min(window.devicePixelRatio || 1, 2.5);
      const rect = this.canvas.getBoundingClientRect();
      const cssW = Math.max(320, Math.round(rect.width || PANEL_W));
      const cssH = Math.round((cssW * PANEL_H) / PANEL_W);
      const pxW = Math.round(cssW * dpr);
      const pxH = Math.round(cssH * dpr);
      if (this.canvas.width !== pxW || this.canvas.height !== pxH) {
        this.canvas.width = pxW;
        this.canvas.height = pxH;
      }
      this.requestDraw();
    };
    if (typeof ResizeObserver !== 'undefined') {
      this.ro = new ResizeObserver(updateSize);
      this.ro.observe(this.canvas);
    } else {
      window.addEventListener('resize', updateSize);
    }
    updateSize();
  }

  _toLogical(clientX, clientY) {
    const rect = this.canvas.getBoundingClientRect();
    const scaleX = PANEL_W / Math.max(1, rect.width);
    const scaleY = PANEL_H / Math.max(1, rect.height);
    return {
      x: (clientX - rect.left) * scaleX,
      y: (clientY - rect.top) * scaleY,
    };
  }

  _hitTest(lx, ly) {
    // 1. Knobs (generous touch radius)
    for (let i = 0; i < KNOBS.length; i++) {
      const k = KNOBS[i];
      const dx = lx - k.x;
      const dy = ly - k.y;
      const hitR = k.r + 8;
      if (dx * dx + dy * dy <= hitR * hitR) {
        return { type: 'knob', knob: k };
      }
    }

    // 2. Buttons
    for (let i = 0; i < BUTTONS.length; i++) {
      const b = BUTTONS[i];
      if (lx >= b.x - 3 && lx <= b.x + b.w + 3 && ly >= b.y - 3 && ly <= b.y + b.h + 3) {
        return { type: 'button', btn: b };
      }
    }

    // 3. Center display
    if (
      lx >= LCD_RECT.x &&
      lx < LCD_RECT.x + LCD_RECT.w &&
      ly >= LCD_RECT.y &&
      ly < LCD_RECT.y + LCD_RECT.h
    ) {
      const x240 = Math.floor(((lx - LCD_RECT.x) * 240) / LCD_RECT.w);
      const y240 = Math.floor(((ly - LCD_RECT.y) * 240) / LCD_RECT.h);
      return { type: 'lcd', x240, y240 };
    }

    // 4. Pill keys (black keys first, then white keys)
    const keyIdx = this._hitKey(lx, ly);
    if (keyIdx >= 0) {
      return { type: 'key', key: keyIdx, velocity: this._keyVelocity(keyIdx, ly) };
    }

    return null;
  }

  _hitKey(lx, ly) {
    for (let i = 0; i < BLACK_SPECS.length; i++) {
      const k = KEYS[BLACK_SPECS[i][0]];
      if (lx >= k.x - 2 && lx <= k.x + k.w + 2 && ly >= k.y - 2 && ly <= k.y + k.h + 2) {
        return k.k;
      }
    }
    for (let i = 0; i < WHITE_INDICES.length; i++) {
      const k = KEYS[WHITE_INDICES[i]];
      if (lx >= k.x - 3 && lx <= k.x + k.w + 3 && ly >= k.y - 2 && ly <= k.y + k.h + 4) {
        return k.k;
      }
    }
    return -1;
  }

  _keyVelocity(keyIdx, ly) {
    const k = KEYS[keyIdx];
    const rel = Math.max(0, Math.min(1, (ly - k.y) / k.h));
    return Math.round(72 + rel * 48); // 72..120 expressive vertical velocity
  }

  _recomputePressedKeys() {
    this.pressedKeys.clear();
    for (const p of this.pointers.values()) {
      if (p.type === 'key' && p.key >= 0) {
        this.pressedKeys.add(p.key);
      }
    }
  }

  _bindEvents() {
    this.canvas.addEventListener('contextmenu', (e) => e.preventDefault());

    this.canvas.addEventListener('pointerdown', (e) => {
      e.preventDefault();
      this.canvas.focus({ preventScroll: true });
      if (this.cb.onInteraction) this.cb.onInteraction();

      const pt = this._toLogical(e.clientX, e.clientY);
      const hit = this._hitTest(pt.x, pt.y);
      if (!hit) return;

      try {
        this.canvas.setPointerCapture(e.pointerId);
      } catch (_) {
        // Ignore synthetic pointer events
      }

      if (hit.type === 'knob') {
        this.pointers.set(e.pointerId, {
          type: 'knob',
          knob: hit.knob,
          lastY: pt.y,
          accum: 0,
          moved: false,
        });
        this.requestDraw();
      } else if (hit.type === 'button') {
        this.pointers.set(e.pointerId, { type: 'button', hw: hit.btn.hw });
        this.pressedButtons.add(hit.btn.hw);
        if (this.cb.onHwButton) this.cb.onHwButton(hit.btn.hw, true);
        this.requestDraw();
      } else if (hit.type === 'lcd') {
        const right = e.button === 2 || e.shiftKey || e.ctrlKey;
        if (this.cb.onLcdClick) this.cb.onLcdClick(hit.x240, hit.y240, right);
      } else if (hit.type === 'key') {
        this.pointers.set(e.pointerId, { type: 'key', key: hit.key });
        const wasDown = this.pressedKeys.has(hit.key);
        this._recomputePressedKeys();
        if (!wasDown && this.cb.onPanelKey) {
          this.cb.onPanelKey(hit.key, true, hit.velocity);
        }
        this.requestDraw();
      }
    });

    this.canvas.addEventListener('pointermove', (e) => {
      const state = this.pointers.get(e.pointerId);
      if (!state) return;
      const pt = this._toLogical(e.clientX, e.clientY);

      if (state.type === 'knob') {
        const dy = state.lastY - pt.y; // drag up = clockwise
        state.lastY = pt.y;
        const fine = e.shiftKey ? 0.35 : 1.0;

        if (state.knob.kind === 'pot') {
          this.masterPot = Math.max(0, Math.min(1023, Math.round(this.masterPot + dy * 4.2 * fine)));
          if (this.cb.onMasterPot) this.cb.onMasterPot(this.masterPot);
          this.requestDraw();
        } else {
          state.accum += dy * fine;
          const stepPx = 7.5;
          const steps = state.accum > 0 ? Math.floor(state.accum / stepPx) : Math.ceil(state.accum / stepPx);
          if (steps !== 0) {
            state.moved = true;
            state.accum -= steps * stepPx;
            this.nudgeEncoderAngle(state.knob.enc, steps);
            if (this.cb.onEncoder) this.cb.onEncoder(state.knob.enc, steps);
          }
        }
      } else if (state.type === 'key') {
        const overKey = this._hitKey(pt.x, pt.y);
        if (overKey !== state.key) {
          const oldKey = state.key;
          state.key = overKey;
          this._recomputePressedKeys();
          if (oldKey >= 0 && !this.pressedKeys.has(oldKey) && this.cb.onPanelKey) {
            this.cb.onPanelKey(oldKey, false, 0);
          }
          if (overKey >= 0 && this.cb.onPanelKey) {
            this.cb.onPanelKey(overKey, true, this._keyVelocity(overKey, pt.y));
          }
          this.requestDraw();
        }
      }
    });

    const releasePointer = (e) => {
      const state = this.pointers.get(e.pointerId);
      if (!state) return;
      this.pointers.delete(e.pointerId);

      if (state.type === 'button') {
        this.pressedButtons.delete(state.hw);
        if (this.cb.onHwButton) this.cb.onHwButton(state.hw, false);
      } else if (state.type === 'key') {
        const oldKey = state.key;
        this._recomputePressedKeys();
        if (oldKey >= 0 && !this.pressedKeys.has(oldKey) && this.cb.onPanelKey) {
          this.cb.onPanelKey(oldKey, false, 0);
        }
      } else if (state.type === 'knob' && !state.moved && state.knob.kind === 'enc') {
        // Quick tap on SELECT / PRESETS / ALGORITHM steps +1 for effortless browsing
        if (state.knob.enc >= 4) {
          const dir = e.shiftKey ? -1 : 1;
          this.nudgeEncoderAngle(state.knob.enc, dir);
          if (this.cb.onEncoder) this.cb.onEncoder(state.knob.enc, dir);
        }
      }
      this.requestDraw();
    };

    this.canvas.addEventListener('pointerup', releasePointer);
    this.canvas.addEventListener('pointercancel', releasePointer);

    // Mouse wheel over any encoder or pot turns it smoothly
    this.canvas.addEventListener(
      'wheel',
      (e) => {
        const pt = this._toLogical(e.clientX, e.clientY);
        const hit = this._hitTest(pt.x, pt.y);
        if (!hit || hit.type !== 'knob') return;
        e.preventDefault();
        if (this.cb.onInteraction) this.cb.onInteraction();

        const dir = e.deltaY < 0 ? 1 : -1;
        if (hit.knob.kind === 'pot') {
          this.masterPot = Math.max(0, Math.min(1023, this.masterPot + dir * 32));
          if (this.cb.onMasterPot) this.cb.onMasterPot(this.masterPot);
          this.requestDraw();
        } else {
          this.nudgeEncoderAngle(hit.knob.enc, dir);
          if (this.cb.onEncoder) this.cb.onEncoder(hit.knob.enc, dir);
        }
      },
      { passive: false }
    );
  }

  nudgeEncoderAngle(encIdx, steps) {
    if (encIdx >= 0 && encIdx < this.encAngles.length) {
      this.encAngles[encIdx] = (this.encAngles[encIdx] + steps * 14) % 360;
      this.requestDraw();
    }
  }

  setQwertyHints(enabled) {
    this.showQwertyHints = Boolean(enabled);
    this.requestDraw();
  }

  setAudioRunning(running) {
    this.audioRunning = Boolean(running);
    this.requestDraw();
  }

  updateFromWorklet(frame) {
    const src = new Uint8ClampedArray(frame.rgba);
    this.lcdImageData.data.set(src);
    this.lcdCtx.putImageData(this.lcdImageData, 0, 0);
    this.hasLiveLcd = true;
    this.btnLeds = frame.btnLeds >>> 0;
    this.keyLeds = frame.keyLeds >>> 0;
    this.bootFrame = (frame.bootFrame || 0) >>> 0;
    this.activeGroup = frame.activeGroup % 6;
    this.requestDraw();
  }

  _bootLedGlow(xNorm) {
    if (!this.bootFrame || this.bootFrame > 68) return 0;
    const u = (this.bootFrame - 1) / 67;
    let sweep = 0;
    if (u < 0.62) {
      const front = (u / 0.42) * 1.18 - 0.06;
      const dist = front - xNorm;
      if (dist >= -0.09 && dist <= 0.34) {
        sweep = dist < 0 ? 1 + dist / 0.09 : 1 - dist / 0.34;
        sweep *= sweep;
      }
    }
    let breath = 0;
    if (u > 0.32 && u < 0.78) {
      const bu = (u - 0.32) / 0.46;
      breath = Math.sin(bu * Math.PI) * 0.45;
    }
    return Math.min(1, Math.max(sweep, breath));
  }

  requestDraw() {
    if (this.rafPending) return;
    this.rafPending = true;
    requestAnimationFrame(() => {
      this.rafPending = false;
      this._draw();
    });
  }

  _draw() {
    const ctx = this.ctx;
    const w = this.canvas.width;
    const h = this.canvas.height;
    if (!w || !h) return;

    ctx.save();
    ctx.scale(w / PANEL_W, h / PANEL_H);

    // Background around chassis
    ctx.fillStyle = '#080C12';
    ctx.fillRect(0, 0, PANEL_W, PANEL_H);

    const accent = GROUP_COLORS[this.activeGroup] || GROUP_COLORS[0];

    // Anodized aluminum outer enclosure
    const bodyGrad = ctx.createLinearGradient(0, 10, 0, PANEL_H - 10);
    bodyGrad.addColorStop(0, '#1D222E');
    bodyGrad.addColorStop(0.52, '#141822');
    bodyGrad.addColorStop(1, '#0F131B');
    roundRectPath(ctx, 14, 10, PANEL_W - 28, PANEL_H - 20, 26);
    ctx.fillStyle = bodyGrad;
    ctx.fill();

    // Subtle top metallic rim highlight
    ctx.lineWidth = 2;
    ctx.strokeStyle = 'rgba(255, 255, 255, 0.09)';
    ctx.stroke();

    // Recessed keyboard well
    roundRectPath(ctx, 36, 544, PANEL_W - 72, 234, 18);
    ctx.fillStyle = '#0B0E15';
    ctx.fill();
    ctx.lineWidth = 1.5;
    ctx.strokeStyle = 'rgba(255, 255, 255, 0.05)';
    ctx.stroke();

    // Corner hex screws
    const screws = [
      [38, 34],
      [PANEL_W - 38, 34],
      [38, 520],
      [PANEL_W - 38, 520],
    ];
    for (const [sx, sy] of screws) {
      ctx.beginPath();
      ctx.arc(sx, sy, 6.5, 0, Math.PI * 2);
      ctx.fillStyle = '#0B0E14';
      ctx.fill();
      ctx.lineWidth = 1;
      ctx.strokeStyle = 'rgba(255,255,255,0.12)';
      ctx.stroke();
    }

    // Center glass bezel
    roundRectPath(ctx, BEZEL_RECT.x, BEZEL_RECT.y, BEZEL_RECT.w, BEZEL_RECT.h, BEZEL_RECT.r);
    ctx.fillStyle = '#06080D';
    ctx.fill();
    ctx.lineWidth = 2;
    ctx.strokeStyle = 'rgba(255, 255, 255, 0.11)';
    ctx.stroke();

    // Subtle group-colored ambient glow around frame
    ctx.save();
    roundRectPath(ctx, LCD_RECT.x - 2, LCD_RECT.y - 2, LCD_RECT.w + 4, LCD_RECT.h + 4, 4);
    ctx.strokeStyle = accent;
    ctx.globalAlpha = 0.28;
    ctx.lineWidth = 2;
    ctx.stroke();
    ctx.restore();

    // Render 240x240 screen (pixel-sharp)
    ctx.save();
    ctx.imageSmoothingEnabled = false;
    if (this.hasLiveLcd) {
      ctx.drawImage(this.lcdCanvas, LCD_RECT.x, LCD_RECT.y, LCD_RECT.w, LCD_RECT.h);
    } else if (this.placeholderImg) {
      ctx.drawImage(this.placeholderImg, LCD_RECT.x, LCD_RECT.y, LCD_RECT.w, LCD_RECT.h);
    } else {
      ctx.fillStyle = '#080C12';
      ctx.fillRect(LCD_RECT.x, LCD_RECT.y, LCD_RECT.w, LCD_RECT.h);
    }
    ctx.restore();

    // If audio hasn't started yet (and boot splash has finished), draw a subtle non-blocking prompt pill
    if (!this.audioRunning && !this.bootFrame) {
      roundRectPath(ctx, LCD_RECT.x + 114, LCD_RECT.y + LCD_RECT.h - 36, 252, 26, 13);
      ctx.fillStyle = 'rgba(8, 12, 18, 0.88)';
      ctx.fill();
      ctx.lineWidth = 1.25;
      ctx.strokeStyle = accent;
      ctx.stroke();
      ctx.fillStyle = '#EEF2F8';
      ctx.font = '600 12px ui-sans-serif, system-ui, -apple-system, sans-serif';
      ctx.textAlign = 'center';
      ctx.textBaseline = 'middle';
      ctx.fillText('CLICK ANY KEY OR KNOB FOR SOUND', LCD_RECT.x + LCD_RECT.w / 2, LCD_RECT.y + LCD_RECT.h - 23);
    }

    // Bezel silkscreen branding
    ctx.fillStyle = '#7D89A3';
    ctx.font = '700 11px ui-monospace, SFMono-Regular, Menlo, monospace';
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    ctx.fillText('M-VAVE FM-1  ·  ADRIFT BBD SYNTHESIZER', BEZEL_RECT.x + BEZEL_RECT.w / 2, BEZEL_RECT.y + BEZEL_RECT.h - 8);

    // Draw 8 knobs (1 pot + 7 encoders)
    for (const k of KNOBS) {
      this._drawKnob(ctx, k, accent);
    }

    // Left deck helper silkscreen under OCT buttons
    ctx.fillStyle = '#6C7892';
    ctx.font = '600 11px ui-sans-serif, system-ui, sans-serif';
    ctx.textAlign = 'center';
    ctx.fillText('TRANSPOSE  /  MODAL ACTION', 184, 492);

    // Right deck helper header
    ctx.fillStyle = '#6C7892';
    ctx.font = '600 11px ui-sans-serif, system-ui, sans-serif';
    ctx.textAlign = 'center';
    ctx.fillText('PRESS AGAIN FOR SUBPAGES (1/2 · 1/6)', 1078, 434);

    // Active Voice Group indicator strip on right deck
    this._drawGroupLegend(ctx);

    // Draw 14 hardware buttons
    for (const b of BUTTONS) {
      this._drawButton(ctx, b, accent);
    }

    // Draw 27 pill keys + 27 key LEDs
    for (const i of WHITE_INDICES) {
      this._drawKey(ctx, KEYS[i], accent);
    }
    for (const [kIdx] of BLACK_SPECS) {
      this._drawKey(ctx, KEYS[kIdx], accent);
    }

    ctx.restore();
  }

  _drawGroupLegend(ctx) {
    const startX = 910;
    const y = 472;
    const pillW = 52;
    const gap = 58;
    for (let g = 0; g < 6; g++) {
      const x = startX + g * gap;
      const isAct = g === this.activeGroup;
      roundRectPath(ctx, x, y, pillW, 24, 6);
      ctx.fillStyle = isAct ? GROUP_COLORS[g] : '#10141E';
      ctx.fill();
      ctx.lineWidth = 1.2;
      ctx.strokeStyle = GROUP_COLORS[g];
      ctx.globalAlpha = isAct ? 1 : 0.45;
      ctx.stroke();
      ctx.globalAlpha = 1;

      ctx.fillStyle = isAct ? '#080C12' : GROUP_COLORS[g];
      ctx.font = '700 11px ui-monospace, SFMono-Regular, Menlo, monospace';
      ctx.textAlign = 'center';
      ctx.textBaseline = 'middle';
      ctx.fillText(`G${g + 1}`, x + pillW / 2, y + 12.5);
    }
  }

  _drawKnob(ctx, k, accent) {
    const isBeingDragged = Array.from(this.pointers.values()).some(
      (p) => p.type === 'knob' && p.knob.id === k.id
    );

    // Outer collar shadow
    ctx.beginPath();
    ctx.arc(k.x, k.y + 3, k.r + 4, 0, Math.PI * 2);
    ctx.fillStyle = 'rgba(0, 0, 0, 0.48)';
    ctx.fill();

    // Potentiometer arc track for MASTER
    if (k.kind === 'pot') {
      const startRad = ((135) * Math.PI) / 180;
      const endRad = ((405) * Math.PI) / 180;
      const valRad = startRad + (this.masterPot / 1023) * (endRad - startRad);
      ctx.beginPath();
      ctx.arc(k.x, k.y, k.r + 7, startRad, endRad);
      ctx.strokeStyle = '#252C3D';
      ctx.lineWidth = 3;
      ctx.lineCap = 'round';
      ctx.stroke();

      ctx.beginPath();
      ctx.arc(k.x, k.y, k.r + 7, startRad, valRad);
      ctx.strokeStyle = accent;
      ctx.lineWidth = 3;
      ctx.stroke();
    } else {
      ctx.beginPath();
      ctx.arc(k.x, k.y, k.r + 6, 0, Math.PI * 2);
      ctx.strokeStyle = isBeingDragged ? accent : 'rgba(255, 255, 255, 0.08)';
      ctx.lineWidth = isBeingDragged ? 2.5 : 1.5;
      ctx.stroke();
    }

    // Knurled skirt
    ctx.beginPath();
    ctx.arc(k.x, k.y, k.r, 0, Math.PI * 2);
    const skirtGrad = ctx.createLinearGradient(k.x, k.y - k.r, k.x, k.y + k.r);
    skirtGrad.addColorStop(0, '#363E52');
    skirtGrad.addColorStop(1, '#1B202C');
    ctx.fillStyle = skirtGrad;
    ctx.fill();

    // Inner cap
    ctx.beginPath();
    ctx.arc(k.x, k.y, k.r * 0.76, 0, Math.PI * 2);
    const capGrad = ctx.createLinearGradient(k.x, k.y - k.r * 0.76, k.x, k.y + k.r * 0.76);
    capGrad.addColorStop(0, '#272E3F');
    capGrad.addColorStop(1, '#141822');
    ctx.fillStyle = capGrad;
    ctx.fill();
    ctx.lineWidth = 1.2;
    ctx.strokeStyle = isBeingDragged ? '#EEF2F8' : 'rgba(255,255,255,0.12)';
    ctx.stroke();

    // Indicator line
    let deg = 0;
    if (k.kind === 'pot') {
      deg = -135 + (this.masterPot / 1023) * 270;
    } else {
      deg = this.encAngles[k.enc] || 0;
    }
    const rad = ((deg - 90) * Math.PI) / 180;
    const r0 = k.r * 0.28;
    const r1 = k.r * 0.70;
    ctx.beginPath();
    ctx.moveTo(k.x + Math.cos(rad) * r0, k.y + Math.sin(rad) * r0);
    ctx.lineTo(k.x + Math.cos(rad) * r1, k.y + Math.sin(rad) * r1);
    ctx.strokeStyle = isBeingDragged ? '#FFFFFF' : accent;
    ctx.lineWidth = 3;
    ctx.lineCap = 'round';
    ctx.stroke();

    // Hardware label above knob
    ctx.fillStyle = '#DCE3F1';
    ctx.font = '700 12px ui-sans-serif, system-ui, -apple-system, sans-serif';
    ctx.textAlign = 'center';
    ctx.textBaseline = 'bottom';
    ctx.fillText(k.label, k.x, k.y - k.r - 11);

    // Adrift role sublabel below knob
    if (this.showAdriftLabels && k.sub) {
      ctx.fillStyle = k.enc === 4 ? accent : '#8A97B2';
      ctx.font = '600 10.5px ui-monospace, SFMono-Regular, Menlo, monospace';
      ctx.textBaseline = 'top';
      ctx.fillText(k.sub, k.x, k.y + k.r + 10);
    }
  }

  _drawButton(ctx, b, accent) {
    const bootGlow = this._bootLedGlow((b.x + b.w * 0.5) / PANEL_W);
    const isLit = ((this.btnLeds >>> b.hw) & 1) !== 0 || bootGlow > 0.55;
    const isDown = this.pressedButtons.has(b.hw);
    const yOff = isDown ? 1.5 : 0;

    // Button bezel shadow
    roundRectPath(ctx, b.x - 1, b.y + 2, b.w + 2, b.h + 2, b.r + 1);
    ctx.fillStyle = '#07090E';
    ctx.fill();

    // Translucent silicone cap
    roundRectPath(ctx, b.x, b.y + yOff, b.w, b.h, b.r);
    if (isLit) {
      const litGrad = ctx.createLinearGradient(b.x, b.y, b.x, b.y + b.h);
      litGrad.addColorStop(0, '#3C3220');
      litGrad.addColorStop(1, '#262018');
      ctx.fillStyle = litGrad;
    } else {
      const offGrad = ctx.createLinearGradient(b.x, b.y, b.x, b.y + b.h);
      offGrad.addColorStop(0, '#283042');
      offGrad.addColorStop(1, '#1B2130');
      ctx.fillStyle = offGrad;
    }
    ctx.fill();

    ctx.lineWidth = isLit || bootGlow > 0.15 ? 1.8 : 1.1;
    ctx.strokeStyle = isLit
      ? '#F8BE30'
      : bootGlow > 0.05
        ? `rgba(248, 190, 48, ${(0.15 + bootGlow * 0.85).toFixed(3)})`
        : 'rgba(255, 255, 255, 0.12)';
    ctx.stroke();

    // Top LED window inside button
    roundRectPath(ctx, b.x + b.w / 2 - 9, b.y + yOff + 7, 18, 5, 2.5);
    ctx.fillStyle = ((this.btnLeds >>> b.hw) & 1) !== 0
      ? '#FFD15C'
      : bootGlow > 0.03
        ? `rgba(255, 209, 92, ${bootGlow.toFixed(3)})`
        : '#111520';
    ctx.fill();

    // Primary hardware legend
    ctx.fillStyle = isLit ? '#FFF6DC' : '#DCE3F1';
    ctx.font = '700 12px ui-sans-serif, system-ui, -apple-system, sans-serif';
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    ctx.fillText(b.label, b.x + b.w / 2, b.y + yOff + b.h * 0.60);

    // Adrift page legend underneath button
    if (this.showAdriftLabels && b.sub) {
      ctx.fillStyle = isLit ? accent : '#8894AC';
      ctx.font = '600 10px ui-monospace, SFMono-Regular, Menlo, monospace';
      ctx.textBaseline = 'top';
      ctx.fillText(b.sub, b.x + b.w / 2, b.y + b.h + 6);
    }
  }

  _drawKey(ctx, k, accent) {
    const isDown = this.pressedKeys.has(k.k);
    const bootGlow = this._bootLedGlow(k.k / 26);
    const isSounding = ((this.keyLeds >>> k.k) & 1) !== 0 || isDown;
    const yOff = isDown ? 2.5 : 0;

    // LED dot above key (like hardware FM-1 key LEDs)
    const ledX = k.x + k.w / 2;
    const ledY = k.y - 8;
    ctx.beginPath();
    ctx.arc(ledX, ledY, 3.3, 0, Math.PI * 2);
    ctx.fillStyle = '#1A202E';
    ctx.fill();
    if (isSounding || bootGlow > 0.03) {
      ctx.save();
      ctx.globalAlpha = isSounding ? 1 : bootGlow;
      ctx.beginPath();
      ctx.arc(ledX, ledY, 3.3, 0, Math.PI * 2);
      ctx.fillStyle = accent;
      ctx.fill();
      ctx.restore();
    }

    // Key shadow
    roundRectPath(ctx, k.x, k.y + 3, k.w, k.h, k.r);
    ctx.fillStyle = 'rgba(0, 0, 0, 0.55)';
    ctx.fill();

    // Key cap
    roundRectPath(ctx, k.x, k.y + yOff, k.w, k.h, k.r);
    if (k.black) {
      const bGrad = ctx.createLinearGradient(k.x, k.y, k.x, k.y + k.h);
      bGrad.addColorStop(0, isSounding ? '#323B52' : '#262D3E');
      bGrad.addColorStop(1, isSounding ? '#202738' : '#161A25');
      ctx.fillStyle = bGrad;
    } else {
      const wGrad = ctx.createLinearGradient(k.x, k.y, k.x, k.y + k.h);
      wGrad.addColorStop(0, isSounding ? '#FFFFFF' : '#E7ECF6');
      wGrad.addColorStop(1, isSounding ? '#D9E4F7' : '#C6D0E2');
      ctx.fillStyle = wGrad;
    }
    ctx.fill();

    ctx.lineWidth = isSounding ? 2.2 : 1.0;
    ctx.strokeStyle = isSounding
      ? accent
      : k.black
        ? 'rgba(255, 255, 255, 0.14)'
        : 'rgba(12, 16, 24, 0.35)';
    ctx.stroke();

    // Note marker (F3 / C4 / C5 / G5)
    const noteText = NOTE_LABELS[k.k];
    if (noteText) {
      ctx.fillStyle = k.black ? '#8C99B5' : '#4E5A73';
      ctx.font = '700 10px ui-sans-serif, system-ui, sans-serif';
      ctx.textAlign = 'center';
      ctx.textBaseline = 'top';
      ctx.fillText(noteText, k.x + k.w / 2, k.y + yOff + 8);
    }

    // Optional QWERTY shortcut badge
    if (this.showQwertyHints && QWERTY_LABELS[k.k]) {
      ctx.fillStyle = k.black ? '#A8B5CF' : '#313B50';
      ctx.font = '700 11px ui-monospace, SFMono-Regular, Menlo, monospace';
      ctx.textAlign = 'center';
      ctx.textBaseline = 'bottom';
      ctx.fillText(QWERTY_LABELS[k.k], k.x + k.w / 2, k.y + yOff + k.h - 9);
    }
  }
}
