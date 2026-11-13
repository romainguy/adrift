// SPDX-License-Identifier: MPL-2.0
// Browser USB-MIDI SysEx OTA Firmware Installer for M-VAVE FM-1.
// Faithful ES module port of external/felucca-bsp/tools/fm1_install.py.

export const HS_QUERY = new Uint8Array([0xf0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7f, 0xf7]);
export const HS_FULL = new Uint8Array([0xf0, 0x00, 0x32, 0x45, 0x08, 0x00, 0x00, 0x40, 0x00, 0x7e, 0x01, 0xf7]);
export const UPGRADE = new Uint8Array([0xf0, 0x22, 0x24, 0x35, 0x7f, 0xf7]);

export const FINISH_CHECK = 0xe0000000 >>> 0;
export const FINISH_WRITE = 0xf0000000 >>> 0;
export const MAXDATA = 512;

export const FAST_CMD = 0x31;
export const CHECK_CMD = 0x32;
export const FAST_CHUNK = 4096;
export const TABLE = 0xd0000000 >>> 0;
export const TABLE_END = 0xe0000000 >>> 0;

export const BLOCKS = 20;
export const BLK = 0x30;
export const KEEP = 0x2f;

export const LOADER_MARK = 'FELUCCA-LOADER-1';
export const STOCK_V15_SHA256 = 'db1642b2b6fa5c2cccb11ffd13878068bb28601678d3644049f99dc40e7edb8a';
export const PORT_RE = /fm-1|felucca|adrift|mighty|ota|composite|sinco|usb-midi/i;

export const DEFAULT_DELAYS = {
  open: 300,
  start: 2000,
  startFast: 50,
  fastHello: 500,
  reply: 10,
  loader: 3000,
  reboot: 3000,
  retry: 1000,
  poll: 1000,
  hs: 1000,
  hsFull: 600,
  idleCheck: 8000,
  idleWrite: 180000,
  waitLoader: 30000,
  waitReboot: 40000,
};

export class InstallError extends Error {
  constructor(code, message) {
    super(message);
    this.name = 'InstallError';
    this.code = code;
  }
}

const sleep = (ms) => (ms > 0 ? new Promise((resolve) => setTimeout(resolve, ms)) : Promise.resolve());

// ---- 7-bit LSB-first SysEx framing ----

export function pack7(data) {
  const out = [];
  let acc = 0;
  let nb = 0;
  for (let i = 0; i < data.length; i++) {
    acc |= (data[i] & 0xff) << nb;
    nb += 8;
    while (nb >= 7) {
      out.push(acc & 0x7f);
      acc >>>= 7;
      nb -= 7;
    }
  }
  if (nb > 0) {
    out.push(acc & 0x7f);
  }
  return new Uint8Array(out);
}

export function unpack7(s) {
  const out = [];
  let acc = 0;
  let nb = 0;
  for (let i = 0; i < s.length; i++) {
    acc |= (s[i] & 0x7f) << nb;
    nb += 7;
    while (nb >= 8) {
      out.push(acc & 0xff);
      acc >>>= 8;
      nb -= 8;
    }
  }
  return new Uint8Array(out);
}

export function response(addr, data, fl = 0) {
  const n = data.length;
  const uAddr = addr >>> 0;
  const body = new Uint8Array(15 + n);
  body[0] = 0x00;
  body[1] = 0x59;
  body[2] = 0x30;
  body[3] = (n + 8) & 0xff;
  body[4] = ((n + 8) >>> 8) & 0xff;
  body[5] = 0x00;
  body[6] = fl & 0xff;
  body[7] = uAddr & 0xff;
  body[8] = (uAddr >>> 8) & 0xff;
  body[9] = (uAddr >>> 16) & 0xff;
  body[10] = (uAddr >>> 24) & 0xff;
  body[11] = n & 0xff;
  body[12] = (n >>> 8) & 0xff;
  body[13] = 0x00;
  body.set(data, 14);

  let sum = 0;
  for (let i = 6; i < 14 + n; i++) {
    sum = (sum + body[i]) & 0xff;
  }
  body[14 + n] = (~sum) & 0xff;

  const packed = pack7(body);
  const pkt = new Uint8Array(packed.length + 2);
  pkt[0] = 0xf0;
  pkt.set(packed, 1);
  pkt[pkt.length - 1] = 0xf7;
  return pkt;
}

export function frame(cmd, body) {
  const n = body.length;
  const u = new Uint8Array(7 + n);
  u[0] = 0x00;
  u[1] = 0x59;
  u[2] = cmd & 0xff;
  u[3] = n & 0xff;
  u[4] = (n >>> 8) & 0xff;
  u[5] = (n >>> 16) & 0xff;
  u.set(body, 6);

  let sum = 0;
  for (let i = 0; i < n; i++) {
    sum = (sum + body[i]) & 0xff;
  }
  u[6 + n] = (~sum) & 0xff;

  const packed = pack7(u);
  const pkt = new Uint8Array(packed.length + 2);
  pkt[0] = 0xf0;
  pkt.set(packed, 1);
  pkt[pkt.length - 1] = 0xf7;
  return pkt;
}

export const FAST_HELLO = frame(FAST_CMD, new Uint8Array([1, FAST_CHUNK & 0xff, (FAST_CHUNK >>> 8) & 0xff]));

export function parseFrame(pkt) {
  if (!pkt || pkt.length < 2 || pkt[0] !== 0xf0 || pkt[pkt.length - 1] !== 0xf7) {
    return null;
  }
  const u = unpack7(pkt.subarray(1, pkt.length - 1));
  if (u.length < 7 || u[0] !== 0x00 || u[1] !== 0x59) {
    return null;
  }
  const n = (u[3] | (u[4] << 8) | (u[5] << 16)) >>> 0;
  if (u.length !== 7 + n) {
    return null;
  }
  let sum = 0;
  for (let i = 6; i < 6 + n; i++) {
    sum = (sum + u[i]) & 0xff;
  }
  if (((~sum) & 0xff) !== u[6 + n]) {
    return null;
  }
  return { cmd: u[2], body: u.subarray(6, 6 + n) };
}

export function parseRequest(pkt) {
  if (!pkt || pkt.length < 2 || pkt[0] !== 0xf0 || pkt[pkt.length - 1] !== 0xf7) {
    return null;
  }
  const u = unpack7(pkt.subarray(1, pkt.length - 1));
  if (u.length !== 15 || u[0] !== 0x00 || u[1] !== 0x59 || u[2] !== 0x30) {
    return null;
  }
  let sum = 0;
  for (let i = 6; i < 14; i++) {
    sum = (sum + u[i]) & 0xff;
  }
  if (((~sum) & 0xff) !== u[14]) {
    return null;
  }
  const flags = u[6];
  const addr = (u[7] | (u[8] << 8) | (u[9] << 16) | (u[10] << 24)) >>> 0;
  const len = (u[11] | (u[12] << 8) | (u[13] << 16)) >>> 0;
  return { flags, addr, len };
}

export function parseIdentity(pkt) {
  if (!pkt || pkt.length < 2 || pkt[0] !== 0xf0 || pkt[pkt.length - 1] !== 0xf7) {
    return null;
  }
  const d = unpack7(pkt.subarray(1, pkt.length - 1));
  if (d.length !== 34 || d[0] !== 0x00 || d[1] !== 0x59 || d[2] !== 0x11) {
    return null;
  }
  let txt = '';
  for (let i = 6; i < 33; i++) {
    if (d[i] === 0) break;
    txt += String.fromCharCode(d[i]);
  }
  const m = /^([^_]+)_(\d+)$/.exec(txt);
  if (!m) return null;
  return {
    text: txt,
    model: m[1],
    version: parseInt(m[2], 10),
    loader: m[1].toLowerCase().startsWith('ota-'),
  };
}

// ---- CRC16-XMODEM & CRC32-IEEE for Fast 4 KiB Sector Diffing ----

export function crc16Xmodem(data, init = 0) {
  let crc = init & 0xffff;
  for (let i = 0; i < data.length; i++) {
    crc ^= (data[i] & 0xff) << 8;
    for (let b = 0; b < 8; b++) {
      if (crc & 0x8000) {
        crc = ((crc << 1) ^ 0x1021) & 0xffff;
      } else {
        crc = (crc << 1) & 0xffff;
      }
    }
  }
  return crc;
}

const CRC32_TABLE = (() => {
  const tbl = new Uint32Array(256);
  for (let i = 0; i < 256; i++) {
    let c = i;
    for (let k = 0; k < 8; k++) {
      c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    }
    tbl[i] = c >>> 0;
  }
  return tbl;
})();

export function crc32Ieee(data) {
  let crc = 0xffffffff;
  for (let i = 0; i < data.length; i++) {
    crc = CRC32_TABLE[(crc ^ data[i]) & 0xff] ^ (crc >>> 8);
  }
  return (~crc) >>> 0;
}

export function sectorTable(image, off, count) {
  const out = new Uint8Array(count * 6);
  for (let k = 0; k < count; k++) {
    const start = off + k * FAST_CHUNK;
    const sec = image.subarray(start, start + FAST_CHUNK);
    const c16 = crc16Xmodem(sec);
    const c32 = crc32Ieee(sec);
    const p = k * 6;
    out[p + 0] = c16 & 0xff;
    out[p + 1] = (c16 >>> 8) & 0xff;
    out[p + 2] = c32 & 0xff;
    out[p + 3] = (c32 >>> 8) & 0xff;
    out[p + 4] = (c32 >>> 16) & 0xff;
    out[p + 5] = (c32 >>> 24) & 0xff;
  }
  return out;
}

// ---- Firmware Package (.fwsc) Extraction & Validation ----

export function productOf(raw) {
  if (!raw || raw.length < BLOCKS * BLK) {
    throw new InstallError('badpkg', 'Not a valid FM-1 firmware package (file is too short).');
  }
  let out = '';
  for (let i = 0; i < BLOCKS; i++) {
    const m = raw[i * BLK + KEEP];
    if (m !== 0x7d) {
      out += String.fromCharCode((m - i - 1) & 0xff);
    }
  }
  return out;
}

export function logicalImage(raw) {
  const out = new Uint8Array(raw.length - BLOCKS);
  let dst = 0;
  for (let i = 0; i < BLOCKS; i++) {
    out.set(raw.subarray(i * BLK, i * BLK + KEEP), dst);
    dst += KEEP;
  }
  out.set(raw.subarray(BLOCKS * BLK), dst);
  return out;
}

export function modelOf(text) {
  const head = String(text).split('_')[0];
  return head.replace(/^ota-/i, '');
}

export function containsBytes(haystack, asciiNeedle) {
  const nLen = asciiNeedle.length;
  if (haystack.length < nLen) return false;
  const c0 = asciiNeedle.charCodeAt(0);
  outer: for (let i = 0; i <= haystack.length - nLen; i++) {
    if (haystack[i] !== c0) continue;
    for (let j = 1; j < nLen; j++) {
      if (haystack[i + j] !== asciiNeedle.charCodeAt(j)) continue outer;
    }
    return true;
  }
  return false;
}

export async function sha256Hex(bytes) {
  const buf = bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
  const digest = await crypto.subtle.digest('SHA-256', buf);
  const arr = new Uint8Array(digest);
  let hex = '';
  for (let i = 0; i < arr.length; i++) {
    hex += arr[i].toString(16).padStart(2, '0');
  }
  return hex;
}

export async function loadPackageBytes(rawBytes, { force = false, label = 'adrift.fwsc' } = {}) {
  const raw = rawBytes instanceof Uint8Array ? rawBytes : new Uint8Array(rawBytes);
  const product = productOf(raw);
  if (!/^[^_]+_\d+$/.test(product)) {
    throw new InstallError('badpkg', `${label}: unrecognized firmware header (${product || 'empty'}).`);
  }
  const hasLoaderMark = containsBytes(raw, LOADER_MARK);
  const sha = await sha256Hex(raw);
  const isOfficialStock = sha === STOCK_V15_SHA256;

  if (!hasLoaderMark && !isOfficialStock && !force) {
    throw new InstallError(
      'badpkg',
      `${label}: only Adrift/Felucca packages and the unmodified official M-VAVE V15 firmware (FM-1.fwsc) are accepted for safe flashing.`
    );
  }

  const image = logicalImage(raw);
  const hasImageLoaderMark = containsBytes(image, LOADER_MARK);
  return {
    product,
    image,
    sha256: sha,
    isOfficialStock,
    useClassic: !hasImageLoaderMark,
  };
}

// ---- Pairing & Handshake ----

export function pairOutput(inName, outNames) {
  if (outNames.includes(inName)) return inName;
  const stripNum = (s) => s.replace(/\s+\d+$/, '');
  const base = stripNum(inName);
  const same = outNames.filter((o) => stripNum(o) === base);
  if (same.length === 1) return same[0];
  return outNames.length === 1 ? outNames[0] : null;
}

async function askIdentity(link, queryPkt, timeoutMs) {
  if (!link.send(queryPkt)) return false;
  const end = performance.now() + timeoutMs;
  while (true) {
    const left = end - performance.now();
    if (left <= 0) break;
    const pkt = await link.read(left);
    if (!pkt) break;
    const ident = parseIdentity(pkt);
    if (ident) return ident;
  }
  return null;
}

export async function handshake(link, delays = DEFAULT_DELAYS, tries = 3) {
  link.drain();
  for (let t = 0; t < tries; t++) {
    const ident = await askIdentity(link, HS_QUERY, delays.hs);
    if (ident === false) return null;
    if (ident === null) continue;
    if (ident.loader || !/^[^_]+_\d{3}$/.test(ident.text)) {
      return ident;
    }
    const full = await askIdentity(link, HS_FULL, delays.hsFull);
    if (full && full.model === ident.model && full.text.startsWith(ident.text)) {
      return full;
    }
    return ident;
  }
  return null;
}

// ---- Core Two-Stage OTA Updater ----

export class Fm1Updater {
  constructor(backend, { portFilter = '', delays = DEFAULT_DELAYS } = {}) {
    this.backend = backend;
    this.portFilter = portFilter.trim();
    this.delays = { ...DEFAULT_DELAYS, ...delays };
  }

  candidates(onlyPort) {
    const names = this.backend.inputNames();
    if (this.portFilter) {
      const lower = this.portFilter.toLowerCase();
      const mine = names.filter((n) => n.toLowerCase().includes(lower));
      if (onlyPort) return mine;
      return [...mine, ...names.filter((n) => !mine.includes(n) && PORT_RE.test(n))];
    }
    return names.filter((n) => PORT_RE.test(n));
  }

  async find(want = null, onlyPort = true) {
    const outs = this.backend.outputNames();
    for (const name of this.candidates(onlyPort)) {
      const outName = pairOutput(name, outs);
      if (!outName) continue;
      let link = null;
      try {
        link = await this.backend.open(name, outName, this.delays.open);
      } catch (_) {
        continue;
      }
      const ident = await handshake(link, this.delays, 2);
      if (ident && (!want || want(ident))) {
        return { link, id: ident, name };
      }
      link.close();
    }
    return null;
  }

  async waitFor(want, waitMs) {
    const end = performance.now() + waitMs;
    while (performance.now() < end) {
      const dev = await this.find(want, false);
      if (dev) return dev;
      await sleep(this.delays.retry);
    }
    return null;
  }

  async serve(link, image, finishAddr, idleMs, onProgress, { loader = false, fast = false, onChecked = null } = {}) {
    let served = 0;
    let last = performance.now();

    while (true) {
      const pkt = await link.read(this.delays.poll);
      if (!pkt) {
        if (link.lost) return { served, finished: false, lost: true };
        if (performance.now() - last > idleMs) return { served, finished: false, lost: false };
        continue;
      }

      const req = parseRequest(pkt);
      if (!req) {
        if (loader && onChecked) {
          const f = parseFrame(pkt);
          if (f && f.cmd === CHECK_CMD && f.body.length === 4) {
            last = performance.now();
            const k = f.body[0] | (f.body[1] << 8);
            const total = f.body[2] | (f.body[3] << 8);
            onChecked(k, total);
          }
        }
        continue;
      }

      const { flags, addr, len } = req;
      last = performance.now();

      if (addr === FINISH_CHECK || addr === FINISH_WRITE) {
        link.send(response(addr, new Uint8Array([0x73, 0x75, 0x63, 0x63, 0x65, 0x73, 0x73, 0x00])));
        if (addr === finishAddr) {
          return { served, finished: true, lost: false };
        }
        continue;
      }

      let data;
      if (loader && addr >= TABLE && addr < TABLE_END) {
        const off = (addr - TABLE) >>> 0;
        const k = Math.floor(len / 6);
        if (len === 0 || len % 6 !== 0 || len > FAST_CHUNK || off + k * FAST_CHUNK > image.length) {
          throw new InstallError('badreq', `Device requested an out-of-range sector table (0x${off.toString(16)}, ${len} B).`);
        }
        data = sectorTable(image, off, k);
      } else {
        const maxChunk = loader ? FAST_CHUNK : MAXDATA;
        if (len > maxChunk || addr + len > image.length) {
          throw new InstallError('badreq', `Device requested bytes outside the firmware image (0x${addr.toString(16)}+${len}).`);
        }
        data = image.subarray(addr, addr + len);
      }

      if (!fast) {
        await sleep(this.delays.reply);
      }
      if (!link.send(response(addr, data, flags))) {
        return { served, finished: false, lost: true };
      }
      served++;
      if (onProgress) onProgress(served, addr, len);
    }
  }

  async check(dev, image, onStep) {
    onStep('start', dev.id.text);
    dev.link.send(UPGRADE);
    await sleep(this.delays.start);

    const res = await this.serve(dev.link, image, FINISH_CHECK, this.delays.idleCheck, (k) => {
      onStep('check', k);
    });
    dev.link.close();

    if (!res.finished) {
      throw new InstallError(
        res.lost ? 'lost' : 'stopped',
        `The FM-1 ${res.lost ? 'was unplugged' : 'stopped responding'} during package check (${res.served} reads). Nothing was modified on your synth.`
      );
    }

    onStep('loader');
    await sleep(this.delays.loader);
    const ota = await this.waitFor((i) => i.loader, this.delays.waitLoader);
    if (!ota) {
      throw new InstallError(
        'noloader',
        'The FM-1 entered update mode, but the browser did not see it reconnect yet. Unplug and reconnect the USB cable, then click Flash again to finish.'
      );
    }
    return ota;
  }

  async fastHello(link) {
    link.drain();
    if (!link.send(FAST_HELLO)) return false;
    const end = performance.now() + this.delays.fastHello;
    while (true) {
      const left = end - performance.now();
      if (left <= 0) break;
      const pkt = await link.read(left);
      if (!pkt) break;
      const f = parseFrame(pkt);
      if (f && f.cmd === FAST_CMD && f.body.length === 3 && f.body[0] === 1) {
        const chunkSz = f.body[1] | (f.body[2] << 8);
        if (chunkSz >= FAST_CHUNK) return true;
      }
    }
    return false;
  }

  async write(ota, image, onStep, classic = false) {
    onStep('write', 0);
    const fast = !classic && (await this.fastHello(ota.link));
    onStep('mode', fast ? 'fast' : 'classic');

    let bestPct = 0;
    let tableOff = null;
    let tableSectors = 0;

    const showPct = (pct) => {
      const clamped = Math.max(0, Math.min(99, Math.floor(pct)));
      if (clamped > bestPct) {
        bestPct = clamped;
        onStep('write', bestPct);
      }
    };

    const onRead = (_k, addr, len) => {
      if (addr >= TABLE && addr < TABLE_END) {
        tableOff = (addr - TABLE) >>> 0;
        tableSectors = Math.floor(len / 6);
      } else if (tableOff !== null) {
        if (tableSectors > 0 && addr >= tableOff) {
          showPct((Math.floor((addr - tableOff) / FAST_CHUNK) + 1) * (100 / tableSectors));
        }
      } else {
        const pct = Math.min(99, Math.floor(((addr + len) * 100) / image.length));
        onStep('write', pct);
      }
    };

    const onChecked = (k, total) => {
      if (total > 0) {
        showPct((k * 100) / total);
      }
    };

    ota.link.send(UPGRADE);
    await sleep(fast ? this.delays.startFast : this.delays.start);

    const res = await this.serve(ota.link, image, FINISH_WRITE, this.delays.idleWrite, onRead, {
      loader: true,
      fast,
      onChecked,
    });
    ota.link.close();

    if (!res.finished) {
      throw new InstallError(
        res.lost ? 'lost' : 'stopped',
        `The update paused after ${res.served} blocks. Reconnect the USB cable and click Flash again — the FM-1 stays safely in update mode and will resume right where it left off.`
      );
    }
    onStep('write', 100);
  }

  async verify(product, onStep) {
    onStep('reboot');
    await sleep(this.delays.reboot);
    const back = await this.waitFor((i) => !i.loader, this.delays.waitReboot);
    if (!back) {
      throw new InstallError(
        'noreturn',
        'Firmware finished writing! If your FM-1 did not restart automatically, switch its power off and back on.'
      );
    }
    back.link.close();
    if (back.id.text !== product) {
      throw new InstallError(
        'mismatch',
        `Write finished, but the synth reported ${back.id.text} instead of ${product}.`
      );
    }
    onStep('done', back.id.text);
    return back.id.text;
  }

  async install(dev, image, product, onStep = () => {}, classic = false) {
    if (modelOf(dev.id.text) !== modelOf(product)) {
      dev.link.close();
      throw new InstallError('model', `Connected device is ${dev.id.text}, which does not match ${product}.`);
    }
    const ota = dev.id.loader ? dev : await this.check(dev, image, onStep);
    await this.write(ota, image, onStep, classic);
    return this.verify(product, onStep);
  }
}

// ---- Browser WebMIDI SysEx Transport ----

class WebMidiLink {
  constructor(access, input, output) {
    this.access = access;
    this.input = input;
    this.output = output;
    this.name = input.name || 'FM-1';
    this.queue = [];
    this.waiter = null;
    this.closed = false;

    this._handler = (ev) => {
      if (this.closed || !ev.data || ev.data[0] !== 0xf0) return;
      const copy = new Uint8Array(ev.data);
      if (this.waiter) {
        const resolve = this.waiter;
        this.waiter = null;
        resolve(copy);
      } else {
        this.queue.push(copy);
      }
    };
    this.input.addEventListener('midimessage', this._handler);
  }

  get lost() {
    if (this.closed) return true;
    return this.input.state !== 'connected' || this.output.state !== 'connected';
  }

  send(pkt) {
    if (this.closed || this.lost) return false;
    try {
      this.output.send(pkt);
      return true;
    } catch (_) {
      return false;
    }
  }

  read(timeoutMs) {
    if (this.queue.length > 0) {
      return Promise.resolve(this.queue.shift());
    }
    if (timeoutMs <= 0 || this.lost) {
      return Promise.resolve(null);
    }
    return new Promise((resolve) => {
      const timer = setTimeout(() => {
        if (this.waiter === done) {
          this.waiter = null;
        }
        resolve(null);
      }, timeoutMs);

      const done = (pkt) => {
        clearTimeout(timer);
        resolve(pkt);
      };
      this.waiter = done;
    });
  }

  drain() {
    this.queue.length = 0;
  }

  close() {
    if (this.closed) return;
    this.closed = true;
    try {
      this.input.removeEventListener('midimessage', this._handler);
    } catch (_) {
      // Ignore detach errors
    }
    if (this.waiter) {
      const resolve = this.waiter;
      this.waiter = null;
      resolve(null);
    }
  }
}

export class WebMidiBackend {
  constructor(access) {
    this.access = access;
  }

  static isSupported() {
    return typeof navigator !== 'undefined' && typeof navigator.requestMIDIAccess === 'function';
  }

  static async requestAccess() {
    if (!WebMidiBackend.isSupported()) {
      throw new InstallError(
        'unsupported',
        'Direct USB flashing requires a browser with WebMIDI SysEx support (Google Chrome, Microsoft Edge, Brave, or Opera on Desktop/Android).'
      );
    }
    try {
      const access = await navigator.requestMIDIAccess({ sysex: true });
      if (!access.sysexEnabled) {
        throw new InstallError(
          'permission',
          'System Exclusive (SysEx) permission was denied. Please allow MIDI SysEx access in your browser address bar and try again.'
        );
      }
      return new WebMidiBackend(access);
    } catch (err) {
      if (err instanceof InstallError) throw err;
      throw new InstallError(
        'permission',
        'Browser MIDI SysEx permission was declined or blocked. Allow MIDI SysEx access in the address bar and try again.'
      );
    }
  }

  inputNames() {
    const seen = new Set();
    for (const inp of this.access.inputs.values()) {
      if (inp.state === 'connected' && inp.name) {
        seen.add(inp.name);
      }
    }
    return Array.from(seen);
  }

  outputNames() {
    const seen = new Set();
    for (const out of this.access.outputs.values()) {
      if (out.state === 'connected' && out.name) {
        seen.add(out.name);
      }
    }
    return Array.from(seen);
  }

  async open(inName, outName, openDelayMs = 300) {
    let inp = null;
    let out = null;
    for (const p of this.access.inputs.values()) {
      if (p.state === 'connected' && p.name === inName) {
        inp = p;
        break;
      }
    }
    for (const p of this.access.outputs.values()) {
      if (p.state === 'connected' && p.name === outName) {
        out = p;
        break;
      }
    }
    if (!inp || !out) {
      throw new Error(`MIDI port ${inName} is no longer connected.`);
    }
    await inp.open();
    await out.open();
    const link = new WebMidiLink(this.access, inp, out);
    await sleep(openDelayMs);
    return link;
  }
}
