#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Assemble the rgbx-sdk distribution tarball — everything a standalone
# extension repo needs to build both a device .llext and a simulator .wasm,
# with no Zephyr/west/monorepo checkout. Attached to every fw-v* release by
# release.yaml (SDK version == firmware release version, always).
#
#   fw/sdk/package-sdk.sh [--version <ver>] [--output <dir>]
#
# The tarball is assembled purely by COPYING from the canonical in-repo
# locations (fw/include/rgbx/, fw/sim/shim/, fw/sim/scripts/, fw/sdk/) —
# nothing is duplicated in-tree, and this script never writes into fw/build
# (root-owned in release CI) or anywhere outside its tmpdir and --output.
#
# Layout (see fw/docs/standalone-extension-repos.md section 5):
#   rgbx-sdk-<ver>/
#     sdk-manifest.json
#     include/rgbx/            ABI headers, verbatim
#     arm/                     shims + allowed-symbols.txt + check-llext.sh
#     wasm/                    sim shims + check-wasm.mjs
#     wasm-v2/                 device Wasm post-link, ABI gate, package builder
#     cmake/                   rgbx-sdk-config.cmake + toolchain files
#     scripts/                 pinned toolchain installers

set -euo pipefail
export COPYFILE_DISABLE=1

SDK_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SDK_DIR/../.." && pwd)"

VERSION=""
OUTPUT="$PWD"
while [ "$#" -gt 0 ]; do
    case "$1" in
        --version) VERSION="$2"; shift 2 ;;
        --output) OUTPUT="$2"; shift 2 ;;
        *)
            echo "usage: package-sdk.sh [--version <ver>] [--output <dir>]" >&2
            exit 2
            ;;
    esac
done

if [ -z "$VERSION" ]; then
    # Default: nearest fw-v* tag; mark builds past the tag as -dev.
    tag="$(git -C "$REPO_ROOT" describe --tags --match 'fw-v*' --abbrev=0 2>/dev/null || true)"
    if [ -z "$tag" ]; then
        VERSION="0.0.0-dev"
    else
        VERSION="${tag#fw-v}"
        if [ "$(git -C "$REPO_ROOT" describe --tags --match 'fw-v*' 2>/dev/null)" != "$tag" ]; then
            VERSION="$VERSION-dev"
        fi
    fi
fi
if [[ ! "$VERSION" =~ ^[0-9A-Za-z][0-9A-Za-z._-]{0,63}$ ]]; then
    echo "error: version must be a 1..64 character path-safe token" >&2
    exit 2
fi

# Every pin the SDK records (ABI versions, toolchain versions and their
# distribution digests, the RGBX v2 admission profile, the llext heap limit)
# is extracted from the file that owns it by write-sdk-manifest.py, which runs
# once the tree is assembled so it can also record each shipped file's SHA-256.
# The llext heap limit reaches check-llext.sh as a plain file because that gate
# runs without a JSON parser.
heap_kb="$(grep -o '^CONFIG_LLEXT_HEAP_SIZE=[0-9]*' "$REPO_ROOT/fw/boards/rgb_sunglasses_proto0_nrf5340_cpuapp.conf" | grep -o '[0-9]*$')"
if [ -z "$heap_kb" ]; then
    echo "error: failed to extract CONFIG_LLEXT_HEAP_SIZE from the proto0 board configuration" >&2
    exit 1
fi
heap_limit=$((heap_kb * 1024))

mkdir -p "$OUTPUT"
OUTPUT="$(cd "$OUTPUT" && pwd)"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
root="$tmp/rgbx-sdk-$VERSION"

mkdir -p "$root/include/rgbx" "$root/arm" "$root/wasm/shim" "$root/wasm-v2" "$root/cmake" "$root/scripts"
cp "$SDK_DIR/LICENSE" "$root/LICENSE"
cp "$SDK_DIR/NOTICE" "$root/NOTICE"

# ABI headers — the contract, verbatim.
cp "$REPO_ROOT/fw/include/rgbx/"*.h "$root/include/rgbx/"

# ARM side: the .exported_sym-emitting shims + gates. heap-limit.txt is the
# stamped CONFIG_LLEXT_HEAP_SIZE in bytes (see above).
cp -r "$SDK_DIR/arm/shim" "$root/arm/shim"
cp "$SDK_DIR/arm/allowed-symbols.txt" "$SDK_DIR/arm/check-llext.sh" "$root/arm/"
echo "$heap_limit" > "$root/arm/heap-limit.txt"

# WASM side: the simulator shims (NOT audio_dsp_wasm.cpp — the audio DSP
# module is simulator infrastructure, not part of an extension build) plus
# the single-sourced export-surface list.
cp -r "$REPO_ROOT/fw/sim/shim/include" "$root/wasm/shim/include"
cp "$REPO_ROOT/fw/sim/shim/sim_shim.c" "$REPO_ROOT/fw/sim/shim/abi_offsets.c" \
   "$REPO_ROOT/fw/sim/shim/rgbx-exports.txt" "$root/wasm/shim/"
cp "$REPO_ROOT/fw/sim/scripts/check-wasm.mjs" "$root/wasm/"

# Device WebAssembly side: deterministic linker-output normalization, exact
# memoryless ABI gate, and canonical RGBX container construction.
cp "$SDK_DIR/wasm-v2/"*.mjs "$root/wasm-v2/"

# CMake package + toolchain installers.
cp "$SDK_DIR/cmake/rgbx-sdk-config.cmake" "$root/cmake/"
mkdir -p "$root/cmake/toolchains"
cp "$SDK_DIR/cmake/toolchains/"*.cmake "$root/cmake/toolchains/"
cp "$SDK_DIR/scripts/install-arm-toolchain.sh" "$root/scripts/"
cp "$REPO_ROOT/fw/sim/scripts/install-toolchain.sh" "$root/scripts/install-wasi-sdk.sh"
chmod +x "$root/scripts/"*.sh "$root/arm/check-llext.sh"

python3 "$SDK_DIR/write-sdk-manifest.py" "$REPO_ROOT" "$root" "$VERSION"

tarball="$OUTPUT/rgbx-sdk-$VERSION.tar.gz"
source_date_epoch="${SOURCE_DATE_EPOCH:-0}"
python3 "$SDK_DIR/create-reproducible-archive.py" "$root" "$tarball" "$source_date_epoch"

# shasum is the macOS spelling; the SDK gate has to run on either host.
if command -v sha256sum >/dev/null 2>&1; then
    sha256="$(sha256sum "$tarball" | cut -d' ' -f1)"
else
    sha256="$(shasum -a 256 "$tarball" | cut -d' ' -f1)"
fi
echo "built $tarball"
echo "sha256: $sha256"
