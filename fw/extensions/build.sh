#!/usr/bin/env bash
# Build every animation extension under fw/extensions/*/ against the LLEXT
# EDK generated from the current proto0 firmware build.
#
#   fw/extensions/build.sh [build-dir]      # default: fw/build
#
# Outputs <name>.llext files into <build-dir>/extensions/, ready to copy to
# the board's /NAND:/ext/ over USB mass storage (mount, cp, sync, umount,
# then reboot the board so the firmware re-mounts FAT and re-discovers).
#
# Third-party extension developers don't use this script — they get the
# llext-edk.tar.xz archive and compile the same way against its cmake.cflags
# or Makefile.cflags (see fw/extensions/README.md).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${1:-$REPO_ROOT/fw/build}"
EDK_TARBALL="$BUILD_DIR/fw/zephyr/llext-edk.tar.xz"
EDK_DIR="$BUILD_DIR/extensions/edk"
OUT_DIR="$BUILD_DIR/extensions"
EXT_SRC_DIR="$REPO_ROOT/fw/extensions"

# Locate the Zephyr SDK cross toolchain the same way the firmware build does.
TOOLCHAIN_BIN=$(ls -d /root/ncs/toolchains/*/opt/zephyr-sdk/arm-zephyr-eabi/bin 2>/dev/null | head -1)
CC="$TOOLCHAIN_BIN/arm-zephyr-eabi-gcc"
CXX="$TOOLCHAIN_BIN/arm-zephyr-eabi-g++"
LD="$TOOLCHAIN_BIN/arm-zephyr-eabi-ld"

# 1. (Re)generate the EDK. The llext-edk target does NOT notice new/changed
#    headers on its own, so force it by deleting the stale tarball first.
rm -f "$EDK_TARBALL"
west build --build-dir "$BUILD_DIR" --domain fw -t llext-edk >/dev/null

# 2. Extract it.
rm -rf "$EDK_DIR"
mkdir -p "$EDK_DIR"
tar -xJf "$EDK_TARBALL" -C "$EDK_DIR"
EDK_INSTALL="$EDK_DIR/llext-edk"

# 3. Expand the EDK's make-format cflags (they reference LLEXT_EDK_INSTALL_DIR).
expand_cflags() {
    make -s -f - LLEXT_EDK_INSTALL_DIR="$EDK_INSTALL" <<EOF
include $EDK_INSTALL/Makefile.cflags
all:
	@echo \$(LLEXT_CFLAGS)
EOF
}
CFLAGS="$(expand_cflags)"
# The EDK flags are C-centric; strip the C-only ones when driving g++.
CXXFLAGS="$(echo "$CFLAGS" | tr ' ' '\n' \
    | grep -v -e '^-std=c99$' -e '^-Wno-pointer-sign$' -e '^-Werror=implicit-int$' \
    | tr '\n' ' ') -std=c++23 -fno-exceptions -fno-rtti"

# 4. Build each extension directory (single translation unit -> single
#    relocatable object, which IS the .llext under CONFIG_LLEXT_TYPE_ELF_OBJECT).
mkdir -p "$OUT_DIR"
built=0
for dir in "$EXT_SRC_DIR"/*/; do
    name="$(basename "$dir")"
    obj="$OUT_DIR/$name.o"
    if compgen -G "$dir/*.cpp" >/dev/null; then
        src="$(compgen -G "$dir/*.cpp" | head -1)"
        # shellcheck disable=SC2086
        "$CXX" $CXXFLAGS -c "$src" -o "$obj"
    elif compgen -G "$dir/*.c" >/dev/null; then
        src="$(compgen -G "$dir/*.c" | head -1)"
        # shellcheck disable=SC2086
        "$CC" $CFLAGS -c "$src" -o "$obj"
    else
        continue
    fi
    # Normalize section layout with a partial link: C++ objects carry COMDAT
    # group sections (.text._Z...) interleaved between .data/.bss in file
    # offsets, which trips the llext loader's region-overlap check
    # ("Region 0 ELF file range ... overlaps with 1"). A plain `ld -r` packs
    # all text sections contiguously ahead of rodata/data/bss. Harmless for
    # plain-C extensions.
    "$LD" -r "$obj" -o "$OUT_DIR/$name.llext"
    rm -f "$obj"
    echo "built $OUT_DIR/$name.llext ($(stat -c%s "$OUT_DIR/$name.llext") bytes)"
    built=$((built + 1))
done

if [ "$built" -eq 0 ]; then
    echo "no extensions found under $EXT_SRC_DIR" >&2
    exit 1
fi
