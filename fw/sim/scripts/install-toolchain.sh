#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Stuart Alldritt
# Resolve (installing if necessary) the pinned wasi-sdk toolchain used to
# compile rgbx extensions to WebAssembly for the simulator.
#
#   fw/sim/scripts/install-toolchain.sh          # prints the wasi-sdk root
#   fw/sim/scripts/install-toolchain.sh --check  # exit 0/1, prints nothing
#   fw/sim/scripts/install-toolchain.sh --verify-archive <file> <sha256>
#
# Trust model (same as fw/sdk/scripts/install-arm-toolchain.sh). The only
# authenticated datum is the per-host archive SHA-256 pinned below. Integrity
# is re-derived from the actual compiler bytes on every run, never from a note
# written next to them:
#
#   * The cache holds the verified distribution ARCHIVE. Every resolve re-hashes
#     that archive against the pin; a mismatch or an absent archive is not
#     trusted.
#   * The extracted tree is a disposable derivative. The path-producing resolve
#     always re-extracts it from the just-verified archive, so a cache writer's
#     tampered tree is overwritten by verified bytes before clang is handed out.
#     No provenance string sits beside the binary for an attacker to forge.
#
# $WASI_SDK_PATH is the one documented exemption: it names a toolchain the
# operator already trusts and is used as-is. The devcontainer image ships the
# verified archive at /opt (see .devcontainer/Dockerfile) so the first in-
# container build extracts from pin-verified bytes without a network fetch.
#
# The pinned version below must stay in sync with the wasi-sdk layer in
# .devcontainer/Dockerfile.

set -euo pipefail

WASI_SDK_VERSION="33.0"
WASI_SDK_TAG="wasi-sdk-33"
# SHA-256 of each wasi-sdk archive, from the SHA256SUMS asset published beside
# the release at github.com/WebAssembly/wasi-sdk/releases/tag/wasi-sdk-33 .
WASI_SDK_SHA256_X86_64_LINUX="0ba8b5bfaeb2adf3f29bab5841d76cf5318ab8e1642ea195f88baba1abd47bce"
WASI_SDK_SHA256_ARM64_LINUX="4f98ee738c7abb45c81a94d1461fc53cc569d1cd01498951c8184d841a027844"
WASI_SDK_SHA256_X86_64_MACOS="18f3f201ba9734e6a4455b0b6410690395a55e9ffa9f6f5066f66083a94b93b3"
WASI_SDK_SHA256_ARM64_MACOS="85c997a2665ead91673b5bb88b7d0df3fc8900df3bfa244f720d478187bbdc78"
CACHE_ROOT="${XDG_CACHE_HOME:-$HOME/.cache}/rgb-sunglasses"

sha256_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    else
        shasum -a 256 "$1" | cut -d' ' -f1
    fi
}

verify_archive() {
    local archive="$1"
    local expected="$2"
    local actual
    actual="$(sha256_file "$archive")"
    if [ "$actual" != "$expected" ]; then
        echo "error: wasi-sdk archive SHA-256 mismatch: expected $expected, got $actual" >&2
        return 1
    fi
}

if [ "${1:-}" = "--verify-archive" ]; then
    if [ "$#" -ne 3 ]; then
        echo "usage: $0 --verify-archive <archive> <sha256>" >&2
        exit 2
    fi
    verify_archive "$2" "$3"
    exit $?
fi

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
case "$asset_arch-$asset_os" in
    x86_64-linux) asset_sha256="$WASI_SDK_SHA256_X86_64_LINUX" ;;
    arm64-linux) asset_sha256="$WASI_SDK_SHA256_ARM64_LINUX" ;;
    x86_64-macos) asset_sha256="$WASI_SDK_SHA256_X86_64_MACOS" ;;
    arm64-macos) asset_sha256="$WASI_SDK_SHA256_ARM64_MACOS" ;;
    *)
        echo "error: no wasi-sdk digest for '$asset_arch-$asset_os'" >&2
        exit 1
        ;;
esac

asset="wasi-sdk-$WASI_SDK_VERSION-$asset_arch-$asset_os.tar.gz"
url="https://github.com/WebAssembly/wasi-sdk/releases/download/$WASI_SDK_TAG/$asset"
tree="$CACHE_ROOT/wasi-sdk-$WASI_SDK_VERSION"
# Verified-archive candidates, in preference order: the image layer, then the
# writable cache. Both must hash to the pin to be trusted.
archive_candidates=(
    "/opt/$asset"
    "$CACHE_ROOT/$asset"
)

# An operator-supplied override root holds a usable clang.
override_clang() {
    local root="${WASI_SDK_PATH:-}"
    [ -n "$root" ] && [ -x "$root/bin/clang" ]
}

# Print the first candidate archive whose bytes hash to the pin, else fail.
verified_archive() {
    local candidate
    for candidate in "${archive_candidates[@]}"; do
        if [ -f "$candidate" ] && [ "$(sha256_file "$candidate")" = "$asset_sha256" ]; then
            echo "$candidate"
            return 0
        fi
    done
    return 1
}

# Ensure a pin-verified archive exists in the writable cache, downloading it if
# no candidate verifies. Prints the archive path.
ensure_verified_archive() {
    local found
    if found="$(verified_archive)"; then
        echo "$found"
        return 0
    fi
    echo "wasi-sdk $WASI_SDK_VERSION archive not cached or unverified; downloading ..." >&2
    mkdir -p "$CACHE_ROOT"
    local tmp
    tmp="$(mktemp -d "$CACHE_ROOT/wasi-sdk-dl.XXXXXX")"
    trap 'rm -rf "$tmp"' RETURN
    curl -fsSL -o "$tmp/$asset" "$url"
    verify_archive "$tmp/$asset" "$asset_sha256"
    mv -f "$tmp/$asset" "$CACHE_ROOT/$asset"
    echo "$CACHE_ROOT/$asset"
}

# Re-extract the wasi-sdk tree from the verified archive, replacing whatever was
# there, so the clang handed out is always freshly unpacked from verified bytes.
extract_tree() {
    local archive="$1"
    mkdir -p "$CACHE_ROOT"
    local tmp
    tmp="$(mktemp -d "$CACHE_ROOT/wasi-sdk-x.XXXXXX")"
    trap 'rm -rf "$tmp"' RETURN
    tar -xzf "$archive" -C "$tmp"
    # The archive unpacks to wasi-sdk-<version>-<arch>-<os>/; normalize the name.
    local extracted="$tmp/wasi-sdk-$WASI_SDK_VERSION-$asset_arch-$asset_os"
    if [ ! -x "$extracted/bin/clang" ]; then
        echo "error: unexpected archive layout in $asset" >&2
        return 1
    fi
    rm -rf "$tree"
    mv "$extracted" "$tree"
}

if [ "${1:-}" = "--check" ]; then
    # A cheap probe: an operator override, or a verified archive available. It
    # never re-extracts and never downloads.
    if override_clang; then
        exit 0
    fi
    verified_archive >/dev/null 2>&1
    exit $?
fi

if override_clang; then
    echo "$WASI_SDK_PATH"
    exit 0
fi

archive="$(ensure_verified_archive)"
extract_tree "$archive"
echo "$tree"
