#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Unified build, test & emulation entry point for Adrift (M-VAVE FM-1 custom firmware).
#
# Subcommands:
#   ./build.sh test        Compile & run automated DSP/MIDI unit & WAV verification suite
#   ./build.sh sdl         Compile native macOS SDL2 + CoreMIDI fast-iteration simulator (<1s)
#   ./build.sh screenshots Render PNG screenshots of all UI screens into screenshots/
#   ./build.sh fw          Compile flashable hardware firmware package (build/fw/adrift.fwsc)
#   ./build.sh emu         Launch fm1-live running build/fw/adrift.fwsc with live audio & MIDI
#   ./build.sh bench       Run strict 240 MHz & 360 MHz pi32v2 cycle-accurate CPU benchmark
#   ./build.sh all         Build test + sdl + fw

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
    SAN_FLAGS="${ADRIFT_SAN_FLAGS:--fsanitize=address,undefined}"
    NEWEST_HDR="$(ls -t "$ROOT"/src/*.h "$ROOT"/src/*/*.h "$ROOT"/tests/*.h 2>/dev/null | head -n 1)"
    OBJS=""
    PIDS=""

    for m in $CORE_MODULES platform/platform_headless; do
        src="$ROOT/src/${m}.c"
        obj="$OBJ_DIR/$(echo "$m" | tr '/' '_').o"
        OBJS="$OBJS $obj"
        if need_host_build "$src" "$obj"; then
            clang $HOST_WARN -O2 -g $SAN_FLAGS -I"$ROOT/src" -c "$src" -o "$obj" &
            PIDS="$PIDS $!"
        fi
    done

    for t in $TEST_MODULES; do
        src="$ROOT/tests/${t}.c"
        obj="$OBJ_DIR/test_${t}.o"
        OBJS="$OBJS $obj"
        if need_host_build "$src" "$obj"; then
            clang $HOST_WARN -O2 -g $SAN_FLAGS -I"$ROOT/src" -c "$src" -o "$obj" &
            PIDS="$PIDS $!"
        fi
    done

    for pid in $PIDS; do
        wait "$pid"
    done

    clang $HOST_WARN -O2 -g $SAN_FLAGS $OBJS -lm -o "$OUT/adrift_tests"
    "$OUT/adrift_tests" "$OUT"
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
        "$OUT/adrift.bin" "$OUT/ota.bin" "$OUT/adrift.fwsc" --product FM-1_462
    echo "  [fwsc]   Ready: $OUT/adrift.fwsc"
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
    test)        build_test ;;
    sdl)         build_sdl ;;
    screenshots) build_screenshots "$@" ;;
    fw)          build_fw ;;
    emu)         run_emu "$@" ;;
    bench)       run_bench ;;
    all)
        build_test
        build_sdl
        build_fw
        ;;
    *)
        echo "Usage: $0 {test|sdl|screenshots|fw|emu|bench|all}" >&2
        exit 1
        ;;
esac
