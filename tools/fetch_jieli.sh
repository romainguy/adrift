#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Downloads and SHA-256-verifies the Apache-2.0 AC791N SDK blobs (uboot.boot,
# cfg_tool.bin, eq_cfg_hw.bin) and the JieLi Linux pi32v2 Clang toolchain into .jieli/
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
JDIR="${1:-$ROOT/.jieli}"
SDK_DIR="$JDIR/ac79-sdk"

mkdir -p "$JDIR"

# 1. Fetch & verify AC79 SDK blobs if not present
if [ ! -f "$SDK_DIR/cpu/wl82/tools/uboot.boot" ]; then
    echo "==> Fetching JieLi AC79 SDK (Apache-2.0 sparse checkout)..."
    mkdir -p "$SDK_DIR"
    (
        cd "$SDK_DIR"
        [ -d .git ] || git init -q .
        REV="d179b4484759423312073f5fbb232501aa491047"
        if ! git fetch -q --depth 1 --filter=blob:none "https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK" "$REV" 2>/dev/null; then
            echo "    Gitee unreachable or lacks commit; trying GitHub mirror..."
            git fetch -q --depth 1 --filter=blob:none "https://github.com/sqtech-ai/fw-AC79_AIoT_SDK" "$REV" 2>/dev/null || \
            git fetch -q --depth 1 --filter=blob:none "https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK" HEAD
        fi
        git sparse-checkout init --no-cone
        git sparse-checkout set /LICENSE /README.md /cpu/wl82/tools/
        git checkout -q FETCH_HEAD
    )
fi

check_sha() {
    FILE="$SDK_DIR/$1"
    EXPECTED="$2"
    if command -v sha256sum >/dev/null 2>&1; then
        ACTUAL="$(sha256sum "$FILE" | awk '{print $1}')"
    else
        ACTUAL="$(shasum -a 256 "$FILE" | awk '{print $1}')"
    fi
    if [ "$ACTUAL" != "$EXPECTED" ]; then
        echo "ERROR: SHA-256 mismatch for $1 ($ACTUAL != $EXPECTED)" >&2
        exit 1
    fi
}

check_sha "cpu/wl82/tools/uboot.boot"        "4e3b4c220dc96641cb5a723f41e68ce41d5261ae9434bb33fbd7f2c59976ded4"
check_sha "cpu/wl82/tools/cfg_tool.bin"      "276579954f076886a6a7694f65dc71c034a63a2c204b76749065c0ac7b010d1b"
check_sha "cpu/wl82/tools/cfg/eq_cfg_hw.bin" "41167491bffed4651750719c973d2758adeb9021a5670d02d6a53c85ed80ea7d"
echo "==> AC79 SDK blobs verified at $SDK_DIR"

# 2. Fetch JieLi Linux pi32v2 toolchain if not present
if [ ! -f "$JDIR/toolchain/pi32v2/bin/clang" ]; then
    echo "==> Downloading JieLi pi32v2 Linux toolchain..."
    curl -fL --retry 3 https://pkgman.jieliapp.com/s/linux-toolchain | tar -xJ -C "$JDIR"
    TC="$(ls -d "$JDIR"/jieli-linux-toolchains-* | sort | tail -1)"
    ln -sfn "$(basename "$TC")" "$JDIR/toolchain"
fi
echo "==> JieLi toolchain ready at $JDIR/toolchain"
