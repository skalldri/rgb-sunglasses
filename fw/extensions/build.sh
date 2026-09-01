#!/usr/bin/env bash
# Build every animation extension under fw/extensions/*/ against the LLEXT
# EDK generated from the current proto0 firmware build.
#
#   fw/extensions/build.sh [build-dir]      # default: fw/build
#
# Outputs <name>.llext files into <build-dir>/extensions/, ready to copy to
# the board's /NAND:/ext/ over USB mass storage (mount, cp, sync, umount,
# then reboot the board so the firmware re-mounts FAT and re-discovers).
# Each comes with a <name>.llext.debug sidecar (the same object with its
# DWARF still attached) for resolving a fault PC offset — never copy those
# to the board; provision-device.sh's *.llext glob deliberately skips them.
#
# That USB copy is the loop for LOCAL builds. Extensions that ship on a GitHub
# release reach end users a different way: the companion app's firmware-update
# modal hashes each on-device .llext and re-uploads the ones that don't match
# the release's asset digest (see fw/CLAUDE.md, "File management (group 8)").
#
# Third-party extension developers don't use this script — see "Building" in
# fw/extensions/README.md for the supported standalone flow.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${1:-$REPO_ROOT/fw/build}"
EDK_TARBALL="$BUILD_DIR/fw/zephyr/llext-edk.tar.xz"
EDK_DIR="$BUILD_DIR/extensions/edk"
OUT_DIR="$BUILD_DIR/extensions"
EXT_SRC_DIR="$REPO_ROOT/fw/extensions"

# Locate the Zephyr SDK cross toolchain. Two lookups, because the SDK lands in a
# different place on each supported host:
#   - devcontainer: bundled under the NCS toolchain dir (/root/ncs/toolchains/...)
#   - macOS host:   installed by `west sdk install` into ~/zephyr-sdk-<version>
# The second lookup reads the CMake package registry the SDK writes on install,
# which is how Zephyr's own build finds it — so it works on any host rather than
# hardcoding another absolute path.
#
# Deliberately NOT `ls ... | head -1`: with `set -o pipefail` a non-matching ls
# makes the whole pipeline fail, and `set -e` then kills this script BEFORE the
# error message below can print — a silent exit 1 (hit for real on macOS).
find_toolchain_bin() {
    local dir sdk_cmake sdk_root
    for dir in /root/ncs/toolchains/*/opt/zephyr-sdk/arm-zephyr-eabi/bin; do
        if [ -d "$dir" ]; then
            echo "$dir"
            return 0
        fi
    done
    for sdk_cmake in "$HOME"/.cmake/packages/Zephyr-sdk/*; do
        [ -f "$sdk_cmake" ] || continue
        # each registry file holds the SDK's cmake dir: <sdk-root>/cmake
        sdk_root="$(dirname "$(cat "$sdk_cmake")")"
        if [ -d "$sdk_root/arm-zephyr-eabi/bin" ]; then
            echo "$sdk_root/arm-zephyr-eabi/bin"
            return 0
        fi
    done
    return 1
}

TOOLCHAIN_BIN="$(find_toolchain_bin || true)"
if [ -z "$TOOLCHAIN_BIN" ]; then
    echo "error: Zephyr SDK arm-zephyr-eabi toolchain not found." >&2
    echo "       Looked in /root/ncs/toolchains/*/opt/zephyr-sdk (devcontainer) and" >&2
    echo "       the CMake package registry ~/.cmake/packages/Zephyr-sdk (macOS)." >&2
    echo "       On macOS run scripts/macos-setup.sh to install it." >&2
    exit 1
fi
CC="$TOOLCHAIN_BIN/arm-zephyr-eabi-gcc"
CXX="$TOOLCHAIN_BIN/arm-zephyr-eabi-g++"
LD="$TOOLCHAIN_BIN/arm-zephyr-eabi-ld"
OBJCOPY="$TOOLCHAIN_BIN/arm-zephyr-eabi-objcopy"

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
    # Same split as the SDK's rgbx_add_extension: the partial link keeps its
    # DWARF in <name>.llext.debug, and the file that ships to the device is
    # the debug-stripped copy (the loader only reads SHF_ALLOC sections, so
    # DWARF is pure upload/NAND cost). --strip-debug keeps .symtab/.strtab,
    # which the loader relocates through.
    "$LD" -r "$obj" -o "$OUT_DIR/$name.llext.debug"
    "$OBJCOPY" --strip-debug "$OUT_DIR/$name.llext.debug" "$OUT_DIR/$name.llext"
    rm -f "$obj"
    # `wc -c` rather than stat: -c%s is GNU-only and macOS stat wants -f%z, so
    # the size printed empty there ("built ... ( bytes)").
    echo "built $OUT_DIR/$name.llext ($(wc -c < "$OUT_DIR/$name.llext" | tr -d ' ') bytes)"
    built=$((built + 1))
done

if [ "$built" -eq 0 ]; then
    echo "no extensions found under $EXT_SRC_DIR" >&2
    exit 1
fi
