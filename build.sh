#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Unified build, test & emulation entry point for Adrift (M-VAVE FM-1 custom firmware).
#
# Subcommands:
#   ./build.sh test              Compile & run automated DSP/MIDI unit & WAV verification suite
#   ./build.sh sdl               Compile native macOS SDL2 + CoreMIDI fast-iteration simulator (<1s)
#   ./build.sh screenshots       Render PNG screenshots of all UI screens into screenshots/
#   ./build.sh fw                Compile flashable hardware firmware package (build/fw/adrift.fwsc)
#   ./build.sh release [ver]     Compile release firmware directly via $JIELI_TOOLCHAIN & $AC79_SDK (no Docker)
#   ./build.sh web               Build static website, WASM synth emulator & firmware installer (build/web/)
#   ./build.sh emu               Launch fm1-live running build/fw/adrift.fwsc with live audio & MIDI
#   ./build.sh bench             Run strict 240 MHz & 360 MHz pi32v2 cycle-accurate CPU benchmark
#   ./build.sh all               Build test + sdl + fw

set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
BSP="$ROOT/external/felucca-bsp"
EMU_BIN="$ROOT/external/fm1-emu/target/release/fm1-live"
JDIR="${JIELI_DIR:-$ROOT/.jieli}"
TC_HOST="$JDIR/toolchain"
SDK_HOST="${AC79_SDK:-$JDIR/ac79-sdk}"

CORE_MODULES="
dsp/dsp_tables
dsp/exciter
dsp/envelope
dsp/filters
dsp/bbd
dsp/fx
synth/synth
synth/presets
synth/preset_store
synth/voice
synth/arp
synth/midi
ui/ui
ui/ui_draw
ui/ui_params
ui/ui_panes
app
"

TEST_MODULES="
adrift_tests
test_dsp
test_groups_ui
test_midi_arp_fx
"

HOST_WARN="-std=c99 -Wall -Wextra -Werror -Wno-unused-parameter"

need_host_build() {
    [ ! -f "$2" ] || [ "$1" -nt "$2" ] || [ "$NEWEST_HDR" -nt "$2" ]
}

build_test() {
    echo "==> Building & running DSP/MIDI verification suite..."
    OUT="$ROOT/build/test"
    OBJ_DIR="$OUT/obj"
    mkdir -p "$OBJ_DIR"

    if [ -n "${HOST_CC:-}" ]; then
        HCC="$HOST_CC"
    elif command -v clang >/dev/null 2>&1; then
        HCC="clang"
    else
        HCC="cc"
    fi

    if [ "${ADRIFT_SAN_FLAGS+set}" = "set" ]; then
        SAN_FLAGS="$ADRIFT_SAN_FLAGS"
    else
        printf 'int main(void) { return 0; }\n' > "$OBJ_DIR/.san_probe.c"
        if "$HCC" -fsanitize=address,undefined "$OBJ_DIR/.san_probe.c" -o "$OBJ_DIR/.san_probe" >/dev/null 2>&1 && \
           ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$OBJ_DIR/.san_probe" >/dev/null 2>&1; then
            SAN_FLAGS="-fsanitize=address,undefined"
        else
            SAN_FLAGS=""
        fi
        rm -f "$OBJ_DIR/.san_probe.c" "$OBJ_DIR/.san_probe"
    fi

    TEST_CFG="$HCC|$SAN_FLAGS|$(uname -s)-$(uname -m)"
    if [ ! -f "$OBJ_DIR/.build_cfg" ] || [ "$(cat "$OBJ_DIR/.build_cfg" 2>/dev/null)" != "$TEST_CFG" ]; then
        rm -f "$OBJ_DIR"/*.o
        printf '%s\n' "$TEST_CFG" > "$OBJ_DIR/.build_cfg"
    fi

    NEWEST_HDR="$(ls -t "$ROOT"/src/*.h "$ROOT"/src/*/*.h "$ROOT"/tests/*.h 2>/dev/null | head -n 1)"
    OBJS=""
    PIDS=""

    for m in $CORE_MODULES platform/platform_headless; do
        src="$ROOT/src/${m}.c"
        obj="$OBJ_DIR/$(echo "$m" | tr '/' '_').o"
        OBJS="$OBJS $obj"
        if need_host_build "$src" "$obj"; then
            "$HCC" $HOST_WARN -O2 -g $SAN_FLAGS -I"$ROOT/src" -c "$src" -o "$obj" &
            PIDS="$PIDS $!"
        fi
    done

    for t in $TEST_MODULES; do
        src="$ROOT/tests/${t}.c"
        obj="$OBJ_DIR/test_${t}.o"
        OBJS="$OBJS $obj"
        if need_host_build "$src" "$obj"; then
            "$HCC" $HOST_WARN -O2 -g $SAN_FLAGS -I"$ROOT/src" -c "$src" -o "$obj" &
            PIDS="$PIDS $!"
        fi
    done

    for pid in $PIDS; do
        wait "$pid"
    done

    "$HCC" $HOST_WARN -O2 -g $SAN_FLAGS $OBJS -lm -o "$OUT/adrift_tests"
    ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$OUT/adrift_tests" "$OUT"
}

build_sdl() {
    echo "==> Building native macOS SDL2 + CoreMIDI simulator..."
    OUT="$ROOT/build/sdl"
    OBJ_DIR="$OUT/obj"
    mkdir -p "$OBJ_DIR"
    SDL_CFLAGS="$(sdl2-config --cflags)"
    SDL_LIBS="$(sdl2-config --libs)"
    NEWEST_HDR="$(ls -t "$ROOT"/src/*.h "$ROOT"/src/*/*.h 2>/dev/null | head -n 1)"
    OBJS=""
    PIDS=""

    for m in $CORE_MODULES; do
        src="$ROOT/src/${m}.c"
        obj="$OBJ_DIR/$(echo "$m" | tr '/' '_').o"
        OBJS="$OBJS $obj"
        if need_host_build "$src" "$obj"; then
            clang $HOST_WARN -O2 -g -I"$ROOT/src" -c "$src" -o "$obj" &
            PIDS="$PIDS $!"
        fi
    done

    sdl_src="$ROOT/src/platform/platform_sdl.c"
    sdl_obj="$OBJ_DIR/platform_platform_sdl.o"
    OBJS="$OBJS $sdl_obj"
    if need_host_build "$sdl_src" "$sdl_obj"; then
        clang $HOST_WARN -O2 -g $SDL_CFLAGS -I"$ROOT/src" -c "$sdl_src" -o "$sdl_obj" &
        PIDS="$PIDS $!"
    fi

    for pid in $PIDS; do
        wait "$pid"
    done

    clang $HOST_WARN -O2 -g \
        $OBJS \
        $SDL_LIBS \
        -framework CoreMIDI -framework CoreFoundation \
        -lm -o "$OUT/adrift"
    echo "  [sdl]    Built $OUT/adrift"
}

build_screenshots() {
    echo "==> Building & running headless UI screenshot generator..."
    OUT="$ROOT/build/screenshots"
    OBJ_DIR="$OUT/obj"
    mkdir -p "$OBJ_DIR"
    NEWEST_HDR="$(ls -t "$ROOT"/src/*.h "$ROOT"/src/*/*.h 2>/dev/null | head -n 1)"
    OBJS=""
    PIDS=""

    for m in $CORE_MODULES platform/platform_headless; do
        src="$ROOT/src/${m}.c"
        obj="$OBJ_DIR/$(echo "$m" | tr '/' '_').o"
        OBJS="$OBJS $obj"
        if need_host_build "$src" "$obj"; then
            clang $HOST_WARN -O2 -g -I"$ROOT/src" -c "$src" -o "$obj" &
            PIDS="$PIDS $!"
        fi
    done

    gen_src="$ROOT/tools/gen_screenshots.c"
    gen_obj="$OBJ_DIR/tools_gen_screenshots.o"
    OBJS="$OBJS $gen_obj"
    if need_host_build "$gen_src" "$gen_obj"; then
        clang $HOST_WARN -O2 -g -I"$ROOT/src" -c "$gen_src" -o "$gen_obj" &
        PIDS="$PIDS $!"
    fi

    for pid in $PIDS; do
        wait "$pid"
    done

    clang $HOST_WARN -O2 -g $OBJS -lz -lm -o "$OUT/gen_screenshots"
    if [ "$#" -eq 0 ]; then
        "$OUT/gen_screenshots" "$ROOT/screenshots"
    else
        "$OUT/gen_screenshots" "$@"
    fi
}

ensure_fw_container() {
    if [ ! -f "$TC_HOST/pi32v2/bin/clang" ] || [ ! -f "$SDK_HOST/cpu/wl82/tools/uboot.boot" ]; then
        "$ROOT/tools/fetch_jieli.sh" "$JDIR"
    fi
    CNAME="adrift-pi32v2-builder"
    RUNNING="$(docker inspect -f '{{.State.Running}}' "$CNAME" 2>/dev/null || true)"
    if [ "$RUNNING" != "true" ]; then
        docker rm -f "$CNAME" >/dev/null 2>&1 || true
        IMAGE="${JIELI_DOCKER_IMAGE:-debian:bookworm-slim}"
        docker run -d --rm --name "$CNAME" --platform linux/amd64 \
            -v "$ROOT:/work" \
            -v "$TC_HOST:/opt/jieli:ro" \
            -w /work \
            "$IMAGE" sh -c "sleep 3600" >/dev/null
    fi
}

build_fw() {
    echo "==> Building M-VAVE FM-1 hardware firmware (pi32v2 -> adrift.fwsc)..."
    OUT_REL="build/fw"
    OUT="$ROOT/$OUT_REL"
    mkdir -p "$OUT/ldr" "$OUT/obj"

    ensure_fw_container
    CNAME="adrift-pi32v2-builder"
    CC="/opt/jieli/pi32v2/bin/clang -target pi32v2"
    LD="/opt/jieli/pi32v2/bin/ld"
    OBJCOPY="/opt/jieli/common/bin/objcopy"
    OBJDUMP="/opt/jieli/common/bin/objdump"

    # 1. Compile & link felucca-bsp RAM update loader -> ota.bin
    if [ ! -f "$OUT/ota.bin" ] || [ "$BSP/loader/loader.c" -nt "$OUT/ota.bin" ]; then
        echo "==> Building OTA loader..."
        docker exec -w /work "$CNAME" sh -e -c "
            rx() { for t in 1 2 3; do \"\$@\" && return 0; sleep 1; done; return 1; }
            rx $CC -c external/felucca-bsp/loader/crt0_ldr.S -o $OUT_REL/ldr/crt0_ldr.o
            rx $CC -Os -ffunction-sections -fno-builtin -Wall -Wno-unused-function \
                -Iexternal/felucca-bsp/chip -Iexternal/felucca-bsp/boards/fm1 \
                -c external/felucca-bsp/loader/loader.c -o $OUT_REL/ldr/loader.o
            rx $LD --gc-sections -e _start -T external/felucca-bsp/loader/loader.ld \
                $OUT_REL/ldr/crt0_ldr.o $OUT_REL/ldr/loader.o -o $OUT_REL/ldr/loader.elf
            rx $OBJCOPY -O binary -j .text $OUT_REL/ldr/loader.elf $OUT_REL/ldr/loader.bin
        "
        python3 "$ROOT/tools/fw_pack.py" wrap-loader "$OUT/ldr/loader.bin" "$OUT/ota.bin"
    fi

    # 2. Compile & link Adrift firmware (DSP & UI renderers at -O2 -mcpu=r3, cold tables/platform at -Os -mcpu=r3)
    FW_WARN="-std=c99 -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-parameter"
    FW_COMMON="-mcpu=r3 -ffunction-sections -fdata-sections -fno-builtin $FW_WARN -Isrc"
    docker exec -w /work "$CNAME" sh -e -c "
        rx() { for t in 1 2 3; do \"\$@\" && return 0; sleep 1; done; return 1; }
        newest_hdr=\$(ls -t src/*.h src/*/*.h 2>/dev/null | head -n 1)
        need_build() {
            [ ! -f \"\$2\" ] || [ \"\$1\" -nt \"\$2\" ] || [ \"\$newest_hdr\" -nt \"\$2\" ]
        }

        need_build external/felucca-bsp/chip/crt0.S $OUT_REL/obj/crt0.o && \
            rx $CC -c external/felucca-bsp/chip/crt0.S -o $OUT_REL/obj/crt0.o
        need_build external/felucca-bsp/chip/fm1_vec.S $OUT_REL/obj/fm1_vec.o && \
            rx $CC -c external/felucca-bsp/chip/fm1_vec.S -o $OUT_REL/obj/fm1_vec.o
        need_build external/felucca-bsp/chip/fm1_isr.S $OUT_REL/obj/fm1_isr.o && \
            rx $CC -c external/felucca-bsp/chip/fm1_isr.S -o $OUT_REL/obj/fm1_isr.o
        need_build external/felucca-bsp/chip/fm1_cpu1.S $OUT_REL/obj/fm1_cpu1.o && \
            rx $CC -c external/felucca-bsp/chip/fm1_cpu1.S -o $OUT_REL/obj/fm1_cpu1.o

        for f in dsp/dsp_tables dsp/exciter dsp/envelope dsp/filters dsp/bbd dsp/fx synth/synth synth/voice synth/arp synth/midi ui/ui ui/ui_draw ui/ui_panes; do
            o=\$(echo \"\$f\" | tr '/' '_')
            if need_build \"src/\${f}.c\" \"$OUT_REL/obj/\${o}.o\"; then
                rx $CC -O2 $FW_COMMON -c \"src/\${f}.c\" -o \"$OUT_REL/obj/\${o}.o\"
            fi
        done

        for f in synth/presets synth/preset_store ui/ui_params app; do
            o=\$(echo \"\$f\" | tr '/' '_')
            if need_build \"src/\${f}.c\" \"$OUT_REL/obj/\${o}.o\"; then
                rx $CC -Os $FW_COMMON -c \"src/\${f}.c\" -o \"$OUT_REL/obj/\${o}.o\"
            fi
        done

        if need_build src/platform/platform_fm1.c $OUT_REL/obj/platform_fm1.o; then
            rx $CC -Os $FW_COMMON \
                -Iexternal/felucca-bsp/chip -Iexternal/felucca-bsp/boards/fm1 \
                -c src/platform/platform_fm1.c -o $OUT_REL/obj/platform_fm1.o
        fi

        rx $LD --gc-sections -e _start -T src/platform/app.ld \
            $OUT_REL/obj/crt0.o $OUT_REL/obj/fm1_vec.o $OUT_REL/obj/fm1_isr.o $OUT_REL/obj/fm1_cpu1.o \
            $OUT_REL/obj/dsp_dsp_tables.o $OUT_REL/obj/dsp_exciter.o \
            $OUT_REL/obj/dsp_envelope.o $OUT_REL/obj/dsp_filters.o \
            $OUT_REL/obj/dsp_bbd.o $OUT_REL/obj/dsp_fx.o \
            $OUT_REL/obj/synth_synth.o $OUT_REL/obj/synth_presets.o \
            $OUT_REL/obj/synth_preset_store.o \
            $OUT_REL/obj/synth_voice.o $OUT_REL/obj/synth_arp.o \
            $OUT_REL/obj/synth_midi.o \
            $OUT_REL/obj/ui_ui.o $OUT_REL/obj/ui_ui_draw.o \
            $OUT_REL/obj/ui_ui_params.o $OUT_REL/obj/ui_ui_panes.o \
            $OUT_REL/obj/app.o $OUT_REL/obj/platform_fm1.o \
            /opt/jieli/pi32v2/lib/r3/libcompiler_rt.a \
            -o $OUT_REL/adrift.elf

        rx $OBJCOPY -O binary -j .text $OUT_REL/adrift.elf $OUT_REL/text.bin
        rx $OBJCOPY -O binary -j .ram_text $OUT_REL/adrift.elf $OUT_REL/ramtext.bin
        rx $OBJCOPY -O binary -j .data $OUT_REL/adrift.elf $OUT_REL/data.bin
        rx $OBJDUMP -t $OUT_REL/adrift.elf > $OUT_REL/syms.txt
        rx $OBJDUMP -d -mcpu=r3 $OUT_REL/adrift.elf > $OUT_REL/dis.txt
        rx $OBJDUMP -d -mcpu=r3 -j .ram_text $OUT_REL/adrift.elf > $OUT_REL/rt_dis.txt
    "

    # 3. Verify pi32v2 hardware invariants & pack app.bin + adrift.fwsc
    python3 "$ROOT/tools/fw_pack.py" pack-app \
        "$OUT/text.bin" "$OUT/ramtext.bin" "$OUT/data.bin" \
        "$OUT/syms.txt" "$OUT/dis.txt" "$OUT/rt_dis.txt" \
        "$OUT/adrift.bin"

    AC79_SDK="$SDK_HOST" python3 "$BSP/tools/fm1pkg.py" \
        "$OUT/adrift.bin" "$OUT/ota.bin" "$OUT/adrift.fwsc" --product FM-1_462000200
    echo "  [fwsc]   Ready: $OUT/adrift.fwsc"
}

ensure_bsp_submodule() {
    if [ ! -f "$BSP/tools/fm1pkg.py" ]; then
        echo "==> Initializing external/felucca-bsp submodule..."
        git -C "$ROOT" submodule update --init external/felucca-bsp
    fi
}

verify_sdk_blobs() {
    SDK_CHECK_DIR="$1"
    python3 - "$SDK_CHECK_DIR" <<'PY'
import hashlib, pathlib, sys

sdk = pathlib.Path(sys.argv[1])
expected = {
    "cpu/wl82/tools/uboot.boot": "4e3b4c220dc96641cb5a723f41e68ce41d5261ae9434bb33fbd7f2c59976ded4",
    "cpu/wl82/tools/cfg_tool.bin": "276579954f076886a6a7694f65dc71c034a63a2c204b76749065c0ac7b010d1b",
    "cpu/wl82/tools/cfg/eq_cfg_hw.bin": "41167491bffed4651750719c973d2758adeb9021a5670d02d6a53c85ed80ea7d",
}
for rel, sha in expected.items():
    p = sdk / rel
    if not p.is_file():
        raise SystemExit(f"ERROR: Missing AC79 SDK file: {p}")
    actual = hashlib.sha256(p.read_bytes()).hexdigest()
    if actual != sha:
        raise SystemExit(f"ERROR: SHA-256 mismatch for {p} ({actual} != {sha})")
PY
}

build_release() {
    REL_VER="${1:-}"
    TC="${JIELI_TOOLCHAIN:-$TC_HOST}"
    SDK="${AC79_SDK:-$SDK_HOST}"

    if [ ! -x "$TC/pi32v2/bin/clang" ] || [ ! -x "$TC/pi32v2/bin/ld" ]; then
        echo "ERROR: JIELI_TOOLCHAIN ('$TC') does not contain pi32v2/bin/{clang,ld}." >&2
        echo "       Set JIELI_TOOLCHAIN to the JieLi Linux toolchain directory." >&2
        exit 1
    fi
    if [ ! -d "$SDK" ]; then
        echo "ERROR: AC79_SDK ('$SDK') not found. Set AC79_SDK to the JieLi AC79 SDK directory." >&2
        exit 1
    fi

    ensure_bsp_submodule
    verify_sdk_blobs "$SDK"

    OUT="$ROOT/build/release"
    rm -rf "$OUT"
    mkdir -p "$OUT/ldr" "$OUT/obj"

    if [ -n "$REL_VER" ]; then
        VER_SAFE="$(printf '%s' "$REL_VER" | tr '/ ' '--')"
        PKG_OUT="$OUT/adrift-${VER_SAFE}.fwsc"
    else
        PKG_OUT="$OUT/adrift.fwsc"
    fi

    echo "==> Building release firmware directly via JIELI_TOOLCHAIN & AC79_SDK -> $(basename "$PKG_OUT")..."

    CC_BIN="$TC/pi32v2/bin/clang"
    LD_BIN="$TC/pi32v2/bin/ld"
    OBJCOPY_BIN="$TC/common/bin/objcopy"
    OBJDUMP_BIN="$TC/common/bin/objdump"
    LIBCRT="$TC/pi32v2/lib/r3/libcompiler_rt.a"

    rx() {
        for _try in 1 2 3; do
            "$@" && return 0
            sleep 1
        done
        return 1
    }

    # 1. Compile & wrap felucca-bsp RAM update loader -> ota.bin
    echo "==> Building OTA loader..."
    rx "$CC_BIN" -target pi32v2 -c "$BSP/loader/crt0_ldr.S" -o "$OUT/ldr/crt0_ldr.o"
    rx "$CC_BIN" -target pi32v2 -Os -ffunction-sections -fno-builtin -Wall -Wno-unused-function \
        -I"$BSP/chip" -I"$BSP/boards/fm1" \
        -c "$BSP/loader/loader.c" -o "$OUT/ldr/loader.o"
    rx "$LD_BIN" --gc-sections -e _start -T "$BSP/loader/loader.ld" \
        "$OUT/ldr/crt0_ldr.o" "$OUT/ldr/loader.o" -o "$OUT/ldr/loader.elf"
    rx "$OBJCOPY_BIN" -O binary -j .text "$OUT/ldr/loader.elf" "$OUT/ldr/loader.bin"
    python3 "$ROOT/tools/fw_pack.py" wrap-loader "$OUT/ldr/loader.bin" "$OUT/ota.bin"

    # 2. Compile & link Adrift firmware
    FW_WARN="-std=c99 -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-parameter"
    FW_COMMON="-mcpu=r3 -ffunction-sections -fdata-sections -fno-builtin $FW_WARN -I$ROOT/src"

    rx "$CC_BIN" -target pi32v2 -c "$BSP/chip/crt0.S" -o "$OUT/obj/crt0.o"
    rx "$CC_BIN" -target pi32v2 -c "$BSP/chip/fm1_vec.S" -o "$OUT/obj/fm1_vec.o"
    rx "$CC_BIN" -target pi32v2 -c "$BSP/chip/fm1_isr.S" -o "$OUT/obj/fm1_isr.o"
    rx "$CC_BIN" -target pi32v2 -c "$BSP/chip/fm1_cpu1.S" -o "$OUT/obj/fm1_cpu1.o"

    for f in dsp/dsp_tables dsp/exciter dsp/envelope dsp/filters dsp/bbd dsp/fx synth/synth synth/voice synth/arp synth/midi ui/ui ui/ui_draw ui/ui_panes; do
        o="$(echo "$f" | tr '/' '_')"
        rx "$CC_BIN" -target pi32v2 -O2 $FW_COMMON -c "$ROOT/src/${f}.c" -o "$OUT/obj/${o}.o"
    done

    for f in synth/presets synth/preset_store ui/ui_params app; do
        o="$(echo "$f" | tr '/' '_')"
        rx "$CC_BIN" -target pi32v2 -Os $FW_COMMON -c "$ROOT/src/${f}.c" -o "$OUT/obj/${o}.o"
    done

    rx "$CC_BIN" -target pi32v2 -Os $FW_COMMON \
        -I"$BSP/chip" -I"$BSP/boards/fm1" \
        -c "$ROOT/src/platform/platform_fm1.c" -o "$OUT/obj/platform_fm1.o"

    rx "$LD_BIN" --gc-sections -e _start -T "$ROOT/src/platform/app.ld" \
        "$OUT/obj/crt0.o" "$OUT/obj/fm1_vec.o" "$OUT/obj/fm1_isr.o" "$OUT/obj/fm1_cpu1.o" \
        "$OUT/obj/dsp_dsp_tables.o" "$OUT/obj/dsp_exciter.o" \
        "$OUT/obj/dsp_envelope.o" "$OUT/obj/dsp_filters.o" \
        "$OUT/obj/dsp_bbd.o" "$OUT/obj/dsp_fx.o" \
        "$OUT/obj/synth_synth.o" "$OUT/obj/synth_presets.o" \
        "$OUT/obj/synth_preset_store.o" \
        "$OUT/obj/synth_voice.o" "$OUT/obj/synth_arp.o" \
        "$OUT/obj/synth_midi.o" \
        "$OUT/obj/ui_ui.o" "$OUT/obj/ui_ui_draw.o" \
        "$OUT/obj/ui_ui_params.o" "$OUT/obj/ui_ui_panes.o" \
        "$OUT/obj/app.o" "$OUT/obj/platform_fm1.o" \
        "$LIBCRT" \
        -o "$OUT/adrift.elf"

    rx "$OBJCOPY_BIN" -O binary -j .text "$OUT/adrift.elf" "$OUT/text.bin"
    rx "$OBJCOPY_BIN" -O binary -j .ram_text "$OUT/adrift.elf" "$OUT/ramtext.bin"
    rx "$OBJCOPY_BIN" -O binary -j .data "$OUT/adrift.elf" "$OUT/data.bin"
    rx "$OBJDUMP_BIN" -t "$OUT/adrift.elf" > "$OUT/syms.txt"
    rx "$OBJDUMP_BIN" -d -mcpu=r3 "$OUT/adrift.elf" > "$OUT/dis.txt"
    rx "$OBJDUMP_BIN" -d -mcpu=r3 -j .ram_text "$OUT/adrift.elf" > "$OUT/rt_dis.txt"

    # 3. Verify pi32v2 hardware invariants & assemble release .fwsc
    python3 "$ROOT/tools/fw_pack.py" pack-app \
        "$OUT/text.bin" "$OUT/ramtext.bin" "$OUT/data.bin" \
        "$OUT/syms.txt" "$OUT/dis.txt" "$OUT/rt_dis.txt" \
        "$OUT/adrift.bin"

    AC79_SDK="$SDK" python3 "$BSP/tools/fm1pkg.py" \
        "$OUT/adrift.bin" "$OUT/ota.bin" "$PKG_OUT" --product FM-1_462000200

    if [ -f "$BSP/LICENSES/Apache-2.0.txt" ]; then
        cp "$BSP/LICENSES/Apache-2.0.txt" "$OUT/Apache-2.0.txt"
    fi

    # Clean intermediate build objects so build/release/ holds only release outputs
    rm -rf "$OUT/ldr" "$OUT/obj" "$OUT/adrift.elf" "$OUT/text.bin" "$OUT/ramtext.bin" \
        "$OUT/data.bin" "$OUT/syms.txt" "$OUT/dis.txt" "$OUT/rt_dis.txt" "$OUT/adrift.bin" "$OUT/ota.bin"

    echo "  [release] Ready: $PKG_OUT"
}

build_web() {
    REBUILD_FW=0
    for arg in "$@"; do
        case "$arg" in
            --rebuild-fw) REBUILD_FW=1 ;;
        esac
    done

    echo "==> Building Adrift website & WebAssembly emulator (build/web/)..."

    # Resolve wasm32-capable clang and wasm-ld
    if [ -n "${WASM_CC:-}" ]; then
        WCC="$WASM_CC"
    elif [ -x "/opt/homebrew/opt/llvm/bin/clang" ]; then
        WCC="/opt/homebrew/opt/llvm/bin/clang"
    elif [ -x "/usr/local/opt/llvm/bin/clang" ]; then
        WCC="/usr/local/opt/llvm/bin/clang"
    else
        WCC="clang"
    fi

    if [ -n "${WASM_LD:-}" ]; then
        WLD="$WASM_LD"
    elif [ -x "/opt/homebrew/opt/lld/bin/wasm-ld" ]; then
        WLD="/opt/homebrew/opt/lld/bin/wasm-ld"
    elif [ -x "/usr/local/opt/lld/bin/wasm-ld" ]; then
        WLD="/usr/local/opt/lld/bin/wasm-ld"
    elif command -v wasm-ld >/dev/null 2>&1; then
        WLD="$(command -v wasm-ld)"
    else
        echo "ERROR: wasm-ld not found. Install via 'brew install llvm lld' or run ./tools/setup_env.sh." >&2
        exit 1
    fi

    OUT="$ROOT/build/web"
    OBJ_DIR="$ROOT/build/web_obj"
    rm -rf "$OUT"
    mkdir -p "$OUT/assets/screens" "$OBJ_DIR"

    NEWEST_HDR="$(ls -t "$ROOT"/src/*.h "$ROOT"/src/*/*.h 2>/dev/null | head -n 1)"
    WASM_FLAGS="--target=wasm32 -std=c99 -O2 -nostdlib -fno-builtin -fvisibility=hidden -Wall -Wextra -Werror -Wno-unused-parameter -I$ROOT/src"
    OBJS=""
    PIDS=""

    for m in $CORE_MODULES platform/platform_web; do
        src="$ROOT/src/${m}.c"
        obj="$OBJ_DIR/$(echo "$m" | tr '/' '_').o"
        OBJS="$OBJS $obj"
        if need_host_build "$src" "$obj"; then
            "$WCC" $WASM_FLAGS -c "$src" -o "$obj" &
            PIDS="$PIDS $!"
        fi
    done

    for pid in $PIDS; do
        wait "$pid"
    done

    "$WLD" --no-entry --export-dynamic --strip-all \
        --initial-memory=2097152 -z stack-size=131072 \
        $OBJS -o "$OUT/adrift.wasm"
    echo "  [wasm]   Built $OUT/adrift.wasm"

    # Ensure UI screenshots are generated and copy into web assets
    if [ "$REBUILD_FW" -eq 1 ] || [ ! -f "$ROOT/screenshots/01_home_scope.png" ] || [ ! -f "$ROOT/screenshots/29_preset_save.png" ]; then
        build_screenshots
    fi
    cp "$ROOT"/screenshots/*.png "$OUT/assets/screens/"

    # Ensure hardware firmware package exists and copy into web root for WebMIDI flashing & download
    if [ "$REBUILD_FW" -eq 1 ] || [ ! -f "$ROOT/build/fw/adrift.fwsc" ]; then
        build_fw
    fi
    cp "$ROOT/build/fw/adrift.fwsc" "$OUT/adrift.fwsc"

    # Copy static website files
    if [ -d "$ROOT/web" ]; then
        cp -R "$ROOT/web/." "$OUT/"
    fi
    : > "$OUT/.nojekyll"

    # Generate version & integrity metadata for UI display and flasher verification
    python3 - "$ROOT" "$OUT/adrift.fwsc" "$OUT/version.json" <<'PY'
import datetime, hashlib, json, os, subprocess, sys

root, fw_path, out_json = sys.argv[1], sys.argv[2], sys.argv[3]
data = open(fw_path, "rb").read()
sha256 = hashlib.sha256(data).hexdigest()

try:
    commit = subprocess.check_output(
        ["git", "-C", root, "rev-parse", "--short", "HEAD"],
        stderr=subprocess.DEVNULL, text=True
    ).strip()
except Exception:
    commit = "unknown"

meta = {
    "name": "Adrift",
    "product": "FM-1_462000200",
    "commit": commit,
    "built_at": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    "fwsc_file": "adrift.fwsc",
    "fwsc_bytes": len(data),
    "fwsc_sha256": sha256,
}
with open(out_json, "w", encoding="utf-8") as f:
    json.dump(meta, f, indent=2)
    f.write("\n")
PY
    echo "  [web]    Ready: $OUT/"
}

run_emu() {
    [ -f "$ROOT/build/fw/adrift.fwsc" ] || build_fw
    exec "$EMU_BIN" --fwsc "$ROOT/build/fw/adrift.fwsc" "$@"
}

run_bench() {
    [ -f "$ROOT/build/fw/adrift.fwsc" ] || build_fw
    echo "==> Running cycle-accurate 16-voice dual-core (CPU0+CPU1) pi32v2 benchmark @ 360 MHz (FM1_STRICT=1)..."
    FM1_STRICT=1 \
    FM1_BENCH_KEYS="450:e6:1,500:e6:1,550:1,590:1^,650:m903070,700:m903470,750:m903772,800:m903B72,850:m903C74,900:m904074,950:m904376,1000:m904776" \
    FM1_BENCH_WAV="$ROOT/build/fw/bench_audio.raw" \
    FM1_BENCH_PANEL="$ROOT/build/fw/bench_panel.ppm" \
    "$EMU_BIN" --fwsc "$ROOT/build/fw/adrift.fwsc" --midi none --bench 3000 --mhz 360
}

CMD="${1:-all}"
shift || true

case "$CMD" in
    test)               build_test ;;
    sdl)                build_sdl ;;
    screenshots)        build_screenshots "$@" ;;
    fw)                 ensure_bsp_submodule; build_fw ;;
    release|--release)  build_release "$@" ;;
    web)                build_web "$@" ;;
    emu)                run_emu "$@" ;;
    bench)              run_bench ;;
    all)
        build_test
        build_sdl
        ensure_bsp_submodule
        build_fw
        ;;
    *)
        echo "Usage: $0 {test|sdl|screenshots|fw|release|web|emu|bench|all}" >&2
        exit 1
        ;;
esac
