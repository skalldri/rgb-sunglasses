#!/usr/bin/env bash
# Resolve (installing if necessary) the pinned Arm GNU Toolchain used to
# compile rgbx extensions to ARM .llext files without the Zephyr SDK.
#
#   fw/sdk/scripts/install-arm-toolchain.sh          # prints the toolchain root
#   fw/sdk/scripts/install-arm-toolchain.sh --check  # exit 0/1, prints nothing
#
# Resolution order:
#   1. $RGBX_ARM_TOOLCHAIN_PATH (explicit override — any prefix-compatible
#      toolchain root whose bin/ holds <triple>-gcc, e.g. a Zephyr SDK
#      arm-zephyr-eabi tree; the SDK cmake toolchain probes both triples)
#   2. ~/.cache/rgb-sunglasses/arm-gnu-toolchain-<version> (downloaded on
#      demand from developer.arm.com)
#
# The pin below is greppable on one line — CI's arm-gnu-toolchain composite
# action keys its cache off it; keep it in sync with sdk-manifest.json
# (package-sdk.sh greps it from here).

set -euo pipefail

ARM_TOOLCHAIN_VERSION="13.2.Rel1"
CACHE_ROOT="${XDG_CACHE_HOME:-$HOME/.cache}/rgb-sunglasses"

candidates=(
    "${RGBX_ARM_TOOLCHAIN_PATH:-}"
    "$CACHE_ROOT/arm-gnu-toolchain-$ARM_TOOLCHAIN_VERSION"
)

resolve() {
    local dir gcc
    for dir in "${candidates[@]}"; do
        [ -n "$dir" ] || continue
        for gcc in "$dir/bin/arm-none-eabi-gcc" "$dir/bin/arm-zephyr-eabi-gcc"; do
            if [ -x "$gcc" ]; then
                echo "$dir"
                return 0
            fi
        done
    done
    return 1
}

if [ "${1:-}" = "--check" ]; then
    resolve >/dev/null
    exit $?
fi

if dir=$(resolve); then
    echo "$dir"
    exit 0
fi

# Not installed anywhere — fetch the pinned release into the cache.
# URL path uses the lowercase form of the version.
lower_version="$(echo "$ARM_TOOLCHAIN_VERSION" | tr '[:upper:]' '[:lower:]')"
arch="$(uname -m)"
os="$(uname -s)"
case "$os" in
    Linux)
        case "$arch" in
            x86_64) asset_host="x86_64" ;;
            arm64 | aarch64) asset_host="aarch64" ;;
            *)
                echo "error: unsupported architecture '$arch' for the Arm GNU Toolchain" >&2
                exit 1
                ;;
        esac
        ;;
    Darwin)
        case "$arch" in
            x86_64) asset_host="darwin-x86_64" ;;
            arm64) asset_host="darwin-arm64" ;;
            *)
                echo "error: unsupported architecture '$arch' for the Arm GNU Toolchain" >&2
                exit 1
                ;;
        esac
        ;;
    *)
        echo "error: unsupported OS '$os' for the Arm GNU Toolchain" >&2
        exit 1
        ;;
esac

asset="arm-gnu-toolchain-$lower_version-$asset_host-arm-none-eabi.tar.xz"
url="https://developer.arm.com/-/media/Files/downloads/gnu/$lower_version/binrel/$asset"
dest="$CACHE_ROOT/arm-gnu-toolchain-$ARM_TOOLCHAIN_VERSION"

echo "Arm GNU Toolchain $ARM_TOOLCHAIN_VERSION not found; downloading to $dest ..." >&2
mkdir -p "$CACHE_ROOT"
tmp="$(mktemp -d "$CACHE_ROOT/arm-gnu-dl.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT
curl -fsSL -o "$tmp/$asset" "$url"
tar -xJf "$tmp/$asset" -C "$tmp"
# The tarball unpacks to arm-gnu-toolchain-<Version>-<host>-arm-none-eabi/
# (version casing differs from the URL); normalize via glob.
extracted="$(compgen -G "$tmp/arm-gnu-toolchain-*-arm-none-eabi" | head -1)"
if [ -z "$extracted" ]; then
    echo "error: unexpected archive layout in $asset" >&2
    exit 1
fi
# $dest may exist as a broken/partial tree (interrupted mv, corrupted CI
# cache restore) — resolve() already failed on it, and a bare mv would nest
# the toolchain INSIDE it and report success. Replace it.
rm -rf "$dest"
mv "$extracted" "$dest"

echo "$dest"
