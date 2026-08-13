#!/usr/bin/env bash
# Build animation extensions to WebAssembly for the hardware-free simulator.
#
#   fw/sim/build-extensions.sh [name ...]   # default: every fw/extensions/*/
#
# Outputs <name>.wasm files into fw/sim/out/wasm/. Mirrors the on-device
# build (fw/extensions/build.sh): one translation unit per directory, prefer
# the first .cpp (clang++) else the first .c (clang). The same source that
# builds here also builds to a .llext with the ARM EDK — the sim build proves
# LOGIC, the ARM build proves the extension links against the device's real
# (much smaller) symbol table. Both must pass before an extension is done.
#
# Each module is linked with -mexec-model=reactor and must end up with ZERO
# wasm imports (the rgbx ABI has no function imports by design); the gate in
# scripts/check-wasm.mjs enforces that plus the presence of the required
# rgbx exports.

set -euo pipefail

SIM_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SIM_DIR/../.." && pwd)"
EXT_SRC_DIR="$REPO_ROOT/fw/extensions"
OUT_DIR="$SIM_DIR/out/wasm"

# The CMSIS-DSP revision golden digests and the DSP parity gate were
# generated against — the NCS v3.1.1 module pin. Single source of truth:
# sim-ci.yml greps this line for its pinned checkout, and the build below
# warns when the local SDK tree has drifted from it (an NCS upgrade would
# otherwise silently change DSP float output vs CI).
CMSIS_DSP_PIN="d80a49b2bb186317dc1db4ac88da49c0ab77e6e7"

# Portable file size (stat -c is GNU-only; the Mac Mini host has BSD stat).
file_size() {
    wc -c < "$1" | tr -d ' '
}

WASI_SDK="$("$SIM_DIR/scripts/install-toolchain.sh")"
CC="$WASI_SDK/bin/clang"
CXX="$WASI_SDK/bin/clang++"

# Shared flags. -isystem puts the Zephyr shim headers (EXPORT_SYMBOL no-op,
# and a <zephyr/kernel.h> that forwards to <rgbx/rgbx_sys.h>) ahead of nothing
# in particular — the wasi sysroot has no zephyr/ headers — but keeps them out
# of warning scope. That forward is why -I fw/include must accompany the
# -isystem everywhere the shim tree is used, including the DSP build below.
#
# -Wl,--fatal-warnings promotes wasm-ld's "function signature mismatch"
# warning to a link error. wasm calls are typed by full signature, so a
# mismatch is not a discarded return value — wasm-ld emits a stub that traps
# with `RuntimeError: unreachable` on first call, having only warned (exit 0)
# at build time. That is the issue #351 failure mode, and it covers every
# sanctioned symbol, not just printk: declare `float sinf(double)` by hand and
# this catches it against wasi-libc's definition. The ARM side has no
# equivalent linker check (ELF resolution is by name alone), which is why
# <rgbx/rgbx_sys.h> shipping the declarations is the primary fix and this is
# the backstop. NOTE: COMMON_FLAGS is not universal in this script — the
# audio_dsp link below builds its own flag list and repeats the flag itself.
COMMON_FLAGS=(
    -O2 -g
    -mexec-model=reactor
    -I "$REPO_ROOT/fw/include"
    -isystem "$SIM_DIR/shim/include"
    -Wall -Wextra
    "-Wl,--fatal-warnings"
)
# The export surface is single-sourced in shim/rgbx-exports.txt (also read
# by the rgbx-sdk's cmake/rgbx-sdk-config.cmake — keep them from drifting).
while read -r export_sym; do
    case "$export_sym" in ''|'#'*) continue ;; esac
    COMMON_FLAGS+=("-Wl,--export-if-defined=$export_sym")
done < "$SIM_DIR/shim/rgbx-exports.txt"
# Match the device C++ dialect (fw/extensions/build.sh): C++23, no
# exceptions, no RTTI. -Wno-null-conversion: clang (unlike the device's GCC)
# warns on rgbx_animation.h's `NULL ? NULL : params` macro expansion; the
# header ships in the EDK and stays as-is.
CXX_FLAGS=(-std=c++23 -fno-exceptions -fno-rtti -Wno-null-conversion)

mkdir -p "$OUT_DIR"

# The C support shims are shared by every module; compile them once as C.
SHIM_OBJ_DIR="$SIM_DIR/out/shim-obj"
mkdir -p "$SHIM_OBJ_DIR"
"$CC" -O2 -g -I "$REPO_ROOT/fw/include" -isystem "$SIM_DIR/shim/include" -Wall -Wextra \
    -c "$SIM_DIR/shim/sim_shim.c" -o "$SHIM_OBJ_DIR/sim_shim.o"
"$CC" -O2 -g -I "$REPO_ROOT/fw/include" -isystem "$SIM_DIR/shim/include" -Wall -Wextra \
    -c "$SIM_DIR/shim/abi_offsets.c" -o "$SHIM_OBJ_DIR/abi_offsets.o"
SHIM_OBJS=("$SHIM_OBJ_DIR/sim_shim.o" "$SHIM_OBJ_DIR/abi_offsets.o")

if [ "$#" -gt 0 ]; then
    dirs=()
    for name in "$@"; do
        if [ -d "$EXT_SRC_DIR/$name" ]; then
            dirs+=("$EXT_SRC_DIR/$name/")
        elif [ -d "$name" ]; then
            dirs+=("${name%/}/")
        else
            echo "error: no extension directory '$name' (looked in $EXT_SRC_DIR and as a path)" >&2
            exit 1
        fi
    done
else
    dirs=("$EXT_SRC_DIR"/*/)
fi

built=0
for dir in "${dirs[@]}"; do
    name="$(basename "$dir")"
    out="$OUT_DIR/$name.wasm"
    if compgen -G "$dir/*.cpp" >/dev/null; then
        src="$(compgen -G "$dir/*.cpp" | head -1)"
        "$CXX" "${COMMON_FLAGS[@]}" "${CXX_FLAGS[@]}" "${SHIM_OBJS[@]}" "$src" -o "$out"
    elif compgen -G "$dir/*.c" >/dev/null; then
        src="$(compgen -G "$dir/*.c" | head -1)"
        "$CC" "${COMMON_FLAGS[@]}" "${SHIM_OBJS[@]}" "$src" -o "$out"
    else
        continue
    fi
    node "$SIM_DIR/scripts/check-wasm.mjs" "$out"
    echo "built $out ($(file_size "$out") bytes)"
    built=$((built + 1))
done

if [ "$built" -eq 0 ]; then
    echo "no extensions found under $EXT_SRC_DIR" >&2
    exit 1
fi

# --- audio_dsp.wasm: the REAL firmware DSP for genuine audio features ------
# Compiles fw/src/sound/audio_dsp.cpp + the needed SDK CMSIS-DSP groups
# (generic C paths — the native_sim replay harness proves this TU runs
# correctly off-target). Skipped when specific extension names were
# requested, rebuilt on every full run (it's a few seconds).
if [ "$#" -eq 0 ]; then
    CMSIS_DSP="${CMSIS_DSP_PATH:-/root/ncs/v3.1.1/modules/lib/cmsis-dsp}"
    if [ ! -d "$CMSIS_DSP/Include" ]; then
        # macOS host keeps NCS in $HOME (see fw/CLAUDE.md).
        CMSIS_DSP="$HOME/ncs/v3.1.1/modules/lib/cmsis-dsp"
    fi
    if [ ! -d "$CMSIS_DSP/Include" ]; then
        echo "warning: CMSIS-DSP not found (set CMSIS_DSP_PATH); skipping audio_dsp.wasm" >&2
    else
        # Warn (don't fail) when the tree isn't at the pinned revision —
        # golden digests and the CI parity gate were generated against it.
        if command -v git >/dev/null 2>&1 && [ -e "$CMSIS_DSP/.git" ]; then
            actual_rev="$(git -C "$CMSIS_DSP" rev-parse HEAD 2>/dev/null || echo unknown)"
            if [ "$actual_rev" != "$CMSIS_DSP_PIN" ]; then
                echo "warning: CMSIS-DSP at $CMSIS_DSP is $actual_rev, not the pinned $CMSIS_DSP_PIN — DSP output may differ from CI/goldens" >&2
            fi
        fi
        dsp_out="$OUT_DIR/audio_dsp.wasm"
        dsp_obj="$SIM_DIR/out/dsp-obj"
        mkdir -p "$dsp_obj"
        # CMSIS-DSP group sources are C — q31 constant tables don't survive
        # C++ narrowing rules, so they must go through clang, not clang++.
        DSP_DEFS=(-DDISABLEFLOAT16 -D__GNUC_PYTHON__)
        DSP_INC=(-isystem "$CMSIS_DSP/Include" -isystem "$CMSIS_DSP/PrivateInclude")
        for group in TransformFunctions CommonTables ComplexMathFunctions \
            StatisticsFunctions BasicMathFunctions FastMathFunctions WindowFunctions; do
            "$CC" -O2 -g "${DSP_DEFS[@]}" "${DSP_INC[@]}" \
                -c "$CMSIS_DSP/Source/$group/$group.c" -o "$dsp_obj/$group.o"
        done
        # This link spells its own flags rather than reusing COMMON_FLAGS, so
        # --fatal-warnings has to be repeated here — and this is the module
        # that needs it most: it is the only one linking firmware C++
        # (audio_dsp.cpp, clang++) against third-party C (the CMSIS-DSP groups,
        # clang). An NCS bump that changes an arm_* signature would otherwise
        # link green and trap with RuntimeError: unreachable inside the parity
        # run, instead of failing the build with the symbol named.
        "$CXX" -O2 -g -mexec-model=reactor -Wl,--fatal-warnings \
            -std=c++23 -fno-exceptions -fno-rtti \
            -I "$REPO_ROOT/fw/src" -I "$REPO_ROOT/fw/src/sound" \
            -I "$REPO_ROOT/fw/include" \
            -isystem "$SIM_DIR/shim/include" \
            "${DSP_DEFS[@]}" "${DSP_INC[@]}" \
            -Wl,--export=sim_pcm -Wl,--export=sim_band_energy \
            -Wl,--export=sim_band_flux -Wl,--export=sim_band_mean \
            -Wl,--export=sim_band_sigma -Wl,--export=sim_beat \
            -Wl,--export=sim_display_bucket \
            -Wl,--export=sim_init -Wl,--export=sim_process \
            -Wl,--export=sim_reset_history \
            "$SIM_DIR/shim/audio_dsp_wasm.cpp" \
            "$REPO_ROOT/fw/src/sound/audio_dsp.cpp" \
            "$dsp_obj"/*.o \
            -o "$dsp_out"
        node "$SIM_DIR/scripts/check-wasm.mjs" "$dsp_out" --dsp
        echo "built $dsp_out ($(file_size "$dsp_out") bytes)"
    fi
fi
