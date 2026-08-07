#!/usr/bin/env bash
# Resolve (installing if necessary) the pinned wasi-sdk toolchain used to
# compile rgbx extensions to WebAssembly for the simulator.
#
#   fw/sim/scripts/install-toolchain.sh          # prints the wasi-sdk root
#   fw/sim/scripts/install-toolchain.sh --check  # exit 0/1, prints nothing
#
# Resolution order:
#   1. $WASI_SDK_PATH (explicit override)
#   2. /opt/wasi-sdk-<version> (devcontainer Dockerfile layer)
#   3. ~/.cache/rgb-sunglasses/wasi-sdk-<version> (downloaded on demand —
#      lets the current container and CI work without an image rebuild)
#
# The pinned version below must stay in sync with the wasi-sdk layer in
# .devcontainer/Dockerfile.

set -euo pipefail

WASI_SDK_VERSION="33.0"
WASI_SDK_TAG="wasi-sdk-33"
CACHE_ROOT="${XDG_CACHE_HOME:-$HOME/.cache}/rgb-sunglasses"

candidates=(
    "${WASI_SDK_PATH:-}"
    "/opt/wasi-sdk-$WASI_SDK_VERSION"
    "$CACHE_ROOT/wasi-sdk-$WASI_SDK_VERSION"
)

resolve() {
    local dir
    for dir in "${candidates[@]}"; do
        if [ -n "$dir" ] && [ -x "$dir/bin/clang" ]; then
            echo "$dir"
            return 0
        fi
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
arch="$(uname -m)"
case "$arch" in
    x86_64) asset_arch="x86_64" ;;
    arm64 | aarch64) asset_arch="arm64" ;;
    *)
        echo "error: unsupported architecture '$arch' for wasi-sdk" >&2
        exit 1
        ;;
esac
os="$(uname -s)"
case "$os" in
    Linux) asset_os="linux" ;;
    Darwin) asset_os="macos" ;;
    *)
        echo "error: unsupported OS '$os' for wasi-sdk" >&2
        exit 1
        ;;
esac

asset="wasi-sdk-$WASI_SDK_VERSION-$asset_arch-$asset_os.tar.gz"
url="https://github.com/WebAssembly/wasi-sdk/releases/download/$WASI_SDK_TAG/$asset"
dest="$CACHE_ROOT/wasi-sdk-$WASI_SDK_VERSION"

echo "wasi-sdk $WASI_SDK_VERSION not found; downloading to $dest ..." >&2
mkdir -p "$CACHE_ROOT"
tmp="$(mktemp -d "$CACHE_ROOT/wasi-sdk-dl.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT
curl -fsSL -o "$tmp/$asset" "$url"
tar -xzf "$tmp/$asset" -C "$tmp"
# The tarball unpacks to wasi-sdk-<version>-<arch>-<os>/; normalize the name.
mv "$tmp/wasi-sdk-$WASI_SDK_VERSION-$asset_arch-$asset_os" "$dest"

echo "$dest"
