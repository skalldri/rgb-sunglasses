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

WASI_SDK="$("$SIM_DIR/scripts/install-toolchain.sh")"
CC="$WASI_SDK/bin/clang"
CXX="$WASI_SDK/bin/clang++"

# Shared flags. -isystem puts the Zephyr shim headers (EXPORT_SYMBOL no-op,
# printk decl) ahead of nothing in particular — the wasi sysroot has no
# zephyr/ headers — but keeps them out of warning scope.
COMMON_FLAGS=(
    -O2 -g
    -mexec-model=reactor
    -I "$REPO_ROOT/fw/include"
    -isystem "$SIM_DIR/shim/include"
    -Wall -Wextra
    -Wl,--export-if-defined=rgbx_manifest
    -Wl,--export-if-defined=rgbx_inputs
    -Wl,--export-if-defined=rgbx_framebuffer
    -Wl,--export-if-defined=rgbx_init
    -Wl,--export-if-defined=rgbx_tick
    -Wl,--export-if-defined=rgbx_good_moment
    -Wl,--export-if-defined=rgbx_sim_log_buf
    -Wl,--export-if-defined=rgbx_sim_log_len
)
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
    echo "built $out ($(stat -c%s "$out") bytes)"
    built=$((built + 1))
done

if [ "$built" -eq 0 ]; then
    echo "no extensions found under $EXT_SRC_DIR" >&2
    exit 1
fi
