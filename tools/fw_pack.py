#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Adrift firmware binary packer & pi32v2 hardware safety verifier.

Subcommands:
  wrap-loader <loader.bin> <ota.bin>
      Compresses loader.bin in 4096-byte LZ4 blocks and wraps it with the two
      JLFS headers expected by the AC791N SPL (`usb_hid_ota.bin` @ 0x01C0A800).

  pack-app <text.bin> <ramtext.bin> <data.bin> <syms.txt> <dis.txt> <rt_dis.txt> <app.bin>
      Assembles the XIP app image at 0x02000120, audits .ram_text for flash calls,
      verifies zero soft-double symbols, checks for the AC791N pi32v2 64-bit
      multiply-near-load silicon bug (felucca-bsp docs/CPU.md), and reports
      Flash / SRAM utilization.
"""

import re
import struct
import sys
from pathlib import Path

APP_XIP = 0x02000120
LOADER_LOAD = 0x01C0A800
LOADER_NAME = b"usb_hid_ota.bin"
MAX_FN_STACK_BYTES = 3584
DIS_LINE = re.compile(r"^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*\t(.*)$")


def _len_bytes(n: int) -> bytearray:
    out = bytearray()
    while n >= 255:
        out.append(255)
        n -= 255
    out.append(n)
    return out


def lz4_compress_block(src: bytes) -> bytes:
    """Minimal LZ4 raw block compressor (no frame header)."""
    n, out, table = len(src), bytearray(), {}
    anchor, i = 0, 0
    limit = n - 12
    while i < limit:
        key = src[i : i + 4]
        cand = table.get(key)
        table[key] = i
        if cand is None or i - cand > 65535:
            i += 1
            continue
        mlen = 4
        while i + mlen < n - 5 and src[cand + mlen] == src[i + mlen]:
            mlen += 1
        lit = src[anchor:i]
        tok = (min(len(lit), 15) << 4) | min(mlen - 4, 15)
        out.append(tok)
        if len(lit) >= 15:
            out += _len_bytes(len(lit) - 15)
        out += lit
        off = i - cand
        out += bytes((off & 0xFF, off >> 8))
        if mlen - 4 >= 15:
            out += _len_bytes(mlen - 4 - 15)
        i += mlen
        anchor = i
    lit = src[anchor:]
    out.append(min(len(lit), 15) << 4)
    if len(lit) >= 15:
        out += _len_bytes(len(lit) - 15)
    out += lit
    return bytes(out)


def lz4_decompress_block(blk: bytes) -> bytes:
    out, i = bytearray(), 0
    while i < len(blk):
        tok = blk[i]
        i += 1
        ll = tok >> 4
        if ll == 15:
            while True:
                b = blk[i]
                i += 1
                ll += b
                if b != 255:
                    break
        out += blk[i : i + ll]
        i += ll
        if i >= len(blk):
            break
        off = blk[i] | (blk[i + 1] << 8)
        i += 2
        ml = tok & 15
        if ml == 15:
            while True:
                b = blk[i]
                i += 1
                ml += b
                if b != 255:
                    break
        ml += 4
        for _ in range(ml):
            out.append(out[-off])
    return bytes(out)


def crc16_xmodem(data: bytes, crc: int = 0) -> int:
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if (crc & 0x8000) else (crc << 1)
        crc &= 0xFFFF
    return crc


def jlfs_ldr_head(dcrc: int, a: int, b: int, attr: int, name: bytes) -> bytes:
    body = struct.pack("<HIIBBH16s", dcrc, a, b, attr, 0, 0, name)
    return struct.pack("<H", crc16_xmodem(body)) + body


def cmd_wrap_loader(loader_bin: Path, ota_out: Path) -> None:
    image = loader_bin.read_bytes()
    if not image or len(image) > 0x14000:
        raise SystemExit(f"wrap-loader: invalid loader size ({len(image)} B)")
    blocks = bytearray()
    for i in range(0, len(image), 4096):
        raw = image[i : i + 4096]
        comp = lz4_compress_block(raw)
        if len(comp) > 4096 or lz4_decompress_block(comp) != raw:
            raise SystemExit(f"wrap-loader: LZ4 self-check failed on block {i // 4096}")
        blocks += struct.pack("<II", len(comp), len(raw)) + comp
    inner = jlfs_ldr_head(crc16_xmodem(image), len(image), LOADER_LOAD, 0, LOADER_NAME)
    body = inner + bytes(blocks)
    ota = jlfs_ldr_head(crc16_xmodem(body), 0x20, len(body), 0x41, LOADER_NAME) + body
    ota_out.write_bytes(ota)
    print(f"  [loader] raw={len(image)} B @ {LOADER_LOAD:#x} -> ota.bin={len(ota)} B")


def find_mul64_near_loads(dis: str, window: int = 4) -> list[str]:
    """Detect inlined 32x32->64 multiplies beside memory loads on pi32v2."""
    found: list[str] = []
    fn = ""
    body: list[str] = []

    def scan() -> bool:
        for i, ins in enumerate(body):
            m = re.match(r"(r\d+)_(r\d+) [+-]?= r\d+ \* r\d+", ins)
            if not m:
                continue
            dst = {m.group(1), m.group(2)}
            before = [b for b in body[max(0, i - 2) : i] if not re.search(r"\bcall\b", b)]
            if any(re.search(r"= [hbd]?\[", b) for b in before):
                return True
            for b in body[i + 1 : i + 1 + window]:
                if any(re.search(r"\b" + r + r"\b", b) for r in dst):
                    break
                if re.search(r"= [hbd]?\[", b):
                    return True
        return False

    for ln in dis.splitlines() + ["end:"]:
        mm = DIS_LINE.match(ln)
        if not mm:
            m2 = re.match(r"^([A-Za-z_.$][\w.$]*):$", ln.strip())
            if m2 and not m2.group(1).startswith(".GJT"):
                if fn and not fn.startswith("__") and scan() and fn not in found:
                    found.append(fn)
                fn, body = m2.group(1), []
            continue
        body.append(mm.group(3))
    return found


def find_stack_frames(dis: str) -> tuple[list[tuple[int, str]], list[str]]:
    """Measure per-function static stack frames and detect dynamic sp adjustments."""
    frames: list[tuple[int, str]] = []
    dyn_alloca: list[str] = []
    fn = ""
    cur_bb = 0
    best_bb = 0
    for ln in dis.splitlines() + ["end:"]:
        m_fn = re.match(r"^([A-Za-z_.$][\w.$]*):$", ln.strip())
        if m_fn and not m_fn.group(1).startswith(".GJT"):
            if fn and not fn.startswith("__"):
                frames.append((best_bb, fn))
            fn = m_fn.group(1)
            cur_bb = 0
            best_bb = 0
            continue
        mm = DIS_LINE.match(ln)
        if not mm:
            continue
        ins = mm.group(3)
        if re.search(r"\bsp\s*(?:[+-]=|=)\s*r\d+\b", ins):
            if fn and fn not in dyn_alloca:
                dyn_alloca.append(fn)
        m_sp = re.search(r"\bsp \+= -(\d+)\b", ins)
        if m_sp:
            cur_bb += int(m_sp.group(1))
            if cur_bb > best_bb:
                best_bb = cur_bb
        elif re.search(r"\b(?:call|goto|if|rts|reti)\b|\{pc", ins):
            cur_bb = 0
    frames.sort(reverse=True)
    return frames, dyn_alloca


def sym_addr(syms: str, name: str) -> int:
    m = re.search(r"^([0-9a-f]+) .*\s" + re.escape(name) + r"$", syms, re.M)
    if not m:
        raise SystemExit(f"pack-app: missing required linker symbol {name}")
    return int(m.group(1), 16)


def cmd_pack_app(
    text_bin: Path,
    rt_bin: Path,
    data_bin: Path,
    syms_path: Path,
    dis_path: Path,
    rt_dis_path: Path,
    out_bin: Path,
) -> None:
    syms = syms_path.read_text()
    dis = dis_path.read_text()
    rt_dis = rt_dis_path.read_text()

    start = sym_addr(syms, "_start")
    if start != APP_XIP:
        raise SystemExit(f"pack-app: _start is {start:#x}, expected {APP_XIP:#x}")

    img = bytearray(text_bin.read_bytes())
    if img[:4] != bytes.fromhex("04818000"):
        raise SystemExit(f"pack-app: text.bin starts with {img[:4].hex()}, expected crt0 stub 04818000")

    # Verify .ram_text makes no direct calls into XIP flash (indirect `call r1` in fm1_cpu1_entry is allowed)
    rt_calls = [
        ln.strip()
        for ln in rt_dis.splitlines()
        if DIS_LINE.match(ln) and re.search(r"\bcall\b(?!\s+r\d+\b)", ln)
    ]
    if rt_calls:
        raise SystemExit(f"pack-app: .ram_text contains forbidden XIP call(s): {rt_calls[:3]}")

    # Verify zero soft-double compiler runtime routines
    soft_double = re.findall(r"\s(__[a-z0-9_]*df[23]?)$", syms, re.M)
    if soft_double:
        raise SystemExit(f"pack-app: forbidden soft-double routines linked: {sorted(set(soft_double))}")

    # Verify pi32v2 64-bit multiply-near-load silicon hazard
    hazards = find_mul64_near_loads(dis)
    if hazards:
        raise SystemExit(
            "pack-app: pi32v2 silicon hazard! 64-bit multiply inlined beside load in: "
            + ", ".join(hazards)
            + " (move int64_t multiplies into noinline helpers)"
        )

    # Verify bounded per-function stack frames (7.75 KB ISR SSP + 256 B guard bands)
    frames, dyn_alloca = find_stack_frames(dis + "\n" + rt_dis)
    if dyn_alloca:
        raise SystemExit(
            "pack-app: forbidden dynamic stack allocation (VLA/alloca) in: "
            + ", ".join(dyn_alloca)
        )
    oversized = [(sz, fn) for sz, fn in frames if sz > MAX_FN_STACK_BYTES]
    if oversized:
        desc = ", ".join(f"{fn} ({sz} B)" for sz, fn in oversized)
        raise SystemExit(
            f"pack-app: stack frame exceeds {MAX_FN_STACK_BYTES} B limit: {desc}"
        )
    max_sp, max_sp_fn = frames[0] if frames else (0, "none")

    for bin_path, load_sym in ((rt_bin, "_rt_load"), (data_bin, "_data_load")):
        load = sym_addr(syms, load_sym)
        if load % 4 != 0:
            raise SystemExit(f"pack-app: {load_sym}={load:#x} is not 4-byte aligned")
        blob = bin_path.read_bytes() if bin_path.exists() else b""
        if blob:
            pad = load - APP_XIP - len(img)
            if pad < 0:
                raise SystemExit(f"pack-app: {load_sym}={load:#x} overlaps preceding section")
            img += b"\xff" * pad
            img += blob

    img += b"\xff" * (-len(img) % 4)
    out_bin.write_bytes(img)

    data_s = sym_addr(syms, "_data_start")
    data_e = sym_addr(syms, "_data_end")
    bss_s = sym_addr(syms, "_bss_start")
    bss_e = sym_addr(syms, "_bss_end")
    ram_used = (data_e - data_s) + (bss_e - bss_s)
    ram_budget = 432 * 1024
    xip_budget = 0x8DFBC
    print(
        f"  [app]    XIP={len(img)} / {xip_budget} B ({len(img) * 100.0 / xip_budget:.1f}%), "
        f"SRAM={ram_used} / {ram_budget} B ({ram_used * 100.0 / ram_budget:.1f}%), "
        f"max-stack={max_sp} B ({max_sp_fn}), "
        f"pi32v2-mul64-check=CLEAN, soft-double=NONE"
    )


def main() -> None:
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    sub = sys.argv[1]
    if sub == "wrap-loader" and len(sys.argv) == 4:
        cmd_wrap_loader(Path(sys.argv[2]), Path(sys.argv[3]))
    elif sub == "pack-app" and len(sys.argv) == 9:
        cmd_pack_app(*(Path(p) for p in sys.argv[2:9]))
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
