#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# One-shot developer environment provisioning script for Adrift on macOS Apple Silicon.
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

echo "==> [1/4] Checking host prerequisites..."
for cmd in clang python3 cargo sdl2-config; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "ERROR: Missing required host tool '$cmd' (install via Homebrew / rustup)." >&2
        exit 1
    fi
done

if ! command -v wasm-ld >/dev/null 2>&1 && [ ! -x /opt/homebrew/opt/lld/bin/wasm-ld ]; then
    echo "==> Installing llvm and lld (wasm-ld) via Homebrew for WebAssembly builds..."
    brew install llvm lld
fi

if ! command -v docker >/dev/null 2>&1 || ! command -v colima >/dev/null 2>&1; then
    echo "==> Installing colima, docker CLI, and qemu via Homebrew..."
    brew install colima docker qemu
fi

echo "==> [2/4] Ensuring Colima container runtime is running..."
if ! docker info >/dev/null 2>&1; then
    colima start --cpus 4 --memory 4
fi

echo "==> [3/4] Fetching JieLi AC791N Apache-2.0 SDK blobs & pi32v2 Linux toolchain..."
"$HERE/fetch_jieli.sh" "$ROOT/.jieli"

echo "==> [4/4] Building joakimeriksson/fm1-emu (fm1-live & fm1-emu)..."
cargo build --release --manifest-path "$ROOT/external/fm1-emu/Cargo.toml" -p fm1-live -p fm1-emu

echo "==> Environment ready! Run './build.sh test', './build.sh sdl', './build.sh fw', './build.sh web', or './build.sh emu'."
