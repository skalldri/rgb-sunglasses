#!/usr/bin/env bash
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
#     cmake/                   rgbx-sdk-config.cmake + toolchain files
#     scripts/                 pinned toolchain installers

set -euo pipefail

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

abi_version="$(grep -o '#define RGBX_ABI_VERSION [0-9]*' "$REPO_ROOT/fw/include/rgbx/rgbx_api.h" | grep -o '[0-9]*$')"
arm_toolchain="$(grep -o 'ARM_TOOLCHAIN_VERSION="[^"]*"' "$SDK_DIR/scripts/install-arm-toolchain.sh" | cut -d'"' -f2)"
wasi_sdk="$(grep -o 'WASI_SDK_VERSION="[^"]*"' "$REPO_ROOT/fw/sim/scripts/install-toolchain.sh" | cut -d'"' -f2)"
if [ -z "$abi_version" ] || [ -z "$arm_toolchain" ] || [ -z "$wasi_sdk" ]; then
    echo "error: failed to extract abi/toolchain pins from source files" >&2
    exit 1
fi

mkdir -p "$OUTPUT"
OUTPUT="$(cd "$OUTPUT" && pwd)"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
root="$tmp/rgbx-sdk-$VERSION"

mkdir -p "$root/include/rgbx" "$root/arm" "$root/wasm/shim" "$root/cmake" "$root/scripts"

# ABI headers — the contract, verbatim.
cp "$REPO_ROOT/fw/include/rgbx/"*.h "$root/include/rgbx/"

# ARM side: the .exported_sym-emitting shims + gates.
cp -r "$SDK_DIR/arm/shim" "$root/arm/shim"
cp "$SDK_DIR/arm/allowed-symbols.txt" "$SDK_DIR/arm/check-llext.sh" "$root/arm/"

# WASM side: the simulator shims (NOT audio_dsp_wasm.cpp — the audio DSP
# module is simulator infrastructure, not part of an extension build).
cp -r "$REPO_ROOT/fw/sim/shim/include" "$root/wasm/shim/include"
cp "$REPO_ROOT/fw/sim/shim/sim_shim.c" "$REPO_ROOT/fw/sim/shim/abi_offsets.c" "$root/wasm/shim/"
cp "$REPO_ROOT/fw/sim/scripts/check-wasm.mjs" "$root/wasm/"

# CMake package + toolchain installers.
cp "$SDK_DIR/cmake/rgbx-sdk-config.cmake" "$root/cmake/"
mkdir -p "$root/cmake/toolchains"
cp "$SDK_DIR/cmake/toolchains/"*.cmake "$root/cmake/toolchains/"
cp "$SDK_DIR/scripts/install-arm-toolchain.sh" "$root/scripts/"
cp "$REPO_ROOT/fw/sim/scripts/install-toolchain.sh" "$root/scripts/install-wasi-sdk.sh"
chmod +x "$root/scripts/"*.sh "$root/arm/check-llext.sh"

cat > "$root/sdk-manifest.json" <<EOF
{
  "sdkVersion": "$VERSION",
  "fwRelease": "fw-v$VERSION",
  "abiVersion": $abi_version,
  "armToolchain": "arm-gnu-$arm_toolchain",
  "wasiSdk": "$wasi_sdk"
}
EOF

tarball="$OUTPUT/rgbx-sdk-$VERSION.tar.gz"
tar -czf "$tarball" -C "$tmp" "rgbx-sdk-$VERSION"

sha256="$(sha256sum "$tarball" | cut -d' ' -f1)"
echo "built $tarball"
echo "sha256: $sha256"
