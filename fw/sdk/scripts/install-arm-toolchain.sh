#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Stuart Alldritt
#
# Resolve (installing if necessary) the pinned Arm GNU Toolchain used to
# compile rgbx extensions to ARM .llext files without the Zephyr SDK.
#
#   fw/sdk/scripts/install-arm-toolchain.sh          # prints the toolchain root
#   fw/sdk/scripts/install-arm-toolchain.sh --check  # exit 0/1, prints nothing
#   fw/sdk/scripts/install-arm-toolchain.sh --verify-archive <file> <sha256>
#
# Trust model. The only authenticated datum is the per-host archive SHA-256
# pinned in this script. Integrity is re-derived from the actual compiler bytes
# on every run, never from a note written next to them:
#
#   * The cache holds the verified distribution ARCHIVE, not just an extracted
#     tree. Every resolve re-hashes that archive and compares it to the pin; a
#     mismatch or an absent archive is not trusted.
#   * The extracted toolchain tree is a disposable derivative. The
#     path-producing resolve always re-extracts it from the just-verified
#     archive, so a tree a cache writer tampered with is overwritten by
#     verified bytes before the compiler is ever handed out. No provenance
#     string sits beside the binary for an attacker to forge.
#
# $RGBX_ARM_TOOLCHAIN_PATH is the one documented exemption: it names a
# toolchain the operator already trusts (a Zephyr SDK tree, a distro package),
# is used as-is, and can never satisfy or short-circuit a download.
#
# The version pin is greppable on one line: CI's arm-gnu-toolchain composite
# action keys its cache off it, and the SDK manifest records it with the
# per-host digests. Each digest cites its vendor-published source below.

set -euo pipefail

ARM_TOOLCHAIN_VERSION="13.2.Rel1"
# SHA-256 of each arm-none-eabi archive, from the .sha256asc file Arm publishes
# beside the asset under
# developer.arm.com/-/media/Files/downloads/gnu/13.2.rel1/binrel/ .
ARM_TOOLCHAIN_SHA256_X86_64_LINUX="6cd1bbc1d9ae57312bcd169ae283153a9572bd6a8e4eeae2fedfbc33b115fdbb"
ARM_TOOLCHAIN_SHA256_AARCH64_LINUX="8fd8b4a0a8d44ab2e195ccfbeef42223dfb3ede29d80f14dcf2183c34b8d199a"
ARM_TOOLCHAIN_SHA256_DARWIN_X86_64="075faa4f3e8eb45e59144858202351a28706f54a6ec17eedd88c9fb9412372cc"
ARM_TOOLCHAIN_SHA256_DARWIN_ARM64="39c44f8af42695b7b871df42e346c09fee670ea8dfc11f17083e296ea2b0d279"
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
        echo "error: Arm GNU Toolchain archive SHA-256 mismatch: expected $expected, got $actual" >&2
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

# URL path uses the lowercase form of the version.
lower_version="$(echo "$ARM_TOOLCHAIN_VERSION" | tr '[:upper:]' '[:lower:]')"
arch="$(uname -m)"
os="$(uname -s)"
case "$os" in
    Linux)
        case "$arch" in
            x86_64)
                asset_host="x86_64"
                asset_sha256="$ARM_TOOLCHAIN_SHA256_X86_64_LINUX"
                ;;
            arm64 | aarch64)
                asset_host="aarch64"
                asset_sha256="$ARM_TOOLCHAIN_SHA256_AARCH64_LINUX"
                ;;
            *)
                echo "error: unsupported architecture '$arch' for the Arm GNU Toolchain" >&2
                exit 1
                ;;
        esac
        ;;
    Darwin)
        case "$arch" in
            x86_64)
                asset_host="darwin-x86_64"
                asset_sha256="$ARM_TOOLCHAIN_SHA256_DARWIN_X86_64"
                ;;
            arm64)
                asset_host="darwin-arm64"
                asset_sha256="$ARM_TOOLCHAIN_SHA256_DARWIN_ARM64"
                ;;
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
cached_archive="$CACHE_ROOT/$asset"
tree="$CACHE_ROOT/arm-gnu-toolchain-$ARM_TOOLCHAIN_VERSION"

# An operator-supplied override root holds a gcc for one of the two triples.
override_gcc() {
    local root="${RGBX_ARM_TOOLCHAIN_PATH:-}"
    [ -n "$root" ] || return 1
    [ -x "$root/bin/arm-none-eabi-gcc" ] || [ -x "$root/bin/arm-zephyr-eabi-gcc" ]
}

# A cached archive is trusted only while its bytes still hash to the pin.
cached_archive_ok() {
    [ -f "$cached_archive" ] && [ "$(sha256_file "$cached_archive")" = "$asset_sha256" ]
}

# Ensure a pin-verified archive is present in the cache, downloading it when the
# cache is empty or its archive fails verification. Leaves $cached_archive
# holding bytes that match the pin, or exits nonzero.
ensure_verified_archive() {
    if cached_archive_ok; then
        return 0
    fi
    echo "Arm GNU Toolchain $ARM_TOOLCHAIN_VERSION archive not cached or unverified; downloading ..." >&2
    mkdir -p "$CACHE_ROOT"
    local tmp
    tmp="$(mktemp -d "$CACHE_ROOT/arm-gnu-dl.XXXXXX")"
    trap 'rm -rf "$tmp"' RETURN
    curl -fsSL -o "$tmp/$asset" "$url"
    verify_archive "$tmp/$asset" "$asset_sha256"
    mv -f "$tmp/$asset" "$cached_archive"
}

# Re-extract the toolchain tree from the verified archive, replacing whatever
# was there. This is what makes a tampered cached tree harmless: the compiler
# handed out is always freshly unpacked from pin-verified bytes.
extract_tree() {
    local tmp
    tmp="$(mktemp -d "$CACHE_ROOT/arm-gnu-x.XXXXXX")"
    trap 'rm -rf "$tmp"' RETURN
    tar -xJf "$cached_archive" -C "$tmp"
    # The archive unpacks to arm-gnu-toolchain-<Version>-<host>-arm-none-eabi/
    # (version casing differs from the URL); normalize via glob.
    local extracted
    extracted="$(compgen -G "$tmp/arm-gnu-toolchain-*-arm-none-eabi" | head -1)"
    if [ -z "$extracted" ]; then
        echo "error: unexpected archive layout in $asset" >&2
        return 1
    fi
    rm -rf "$tree"
    mv "$extracted" "$tree"
}

if [ "${1:-}" = "--check" ]; then
    # A cheap probe: an operator override, or a cache holding a pin-verified
    # archive. It never re-extracts and never downloads; the path-producing
    # resolve below is what re-derives the tree from verified bytes.
    if override_gcc; then
        exit 0
    fi
    cached_archive_ok >/dev/null 2>&1
    exit $?
fi

if override_gcc; then
    echo "$RGBX_ARM_TOOLCHAIN_PATH"
    exit 0
fi

ensure_verified_archive
extract_tree
echo "$tree"
