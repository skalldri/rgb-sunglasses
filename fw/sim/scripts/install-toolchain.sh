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
# Concurrency is handled separately from trust, and each part does exactly one
# thing. Resolves that share a cache serialize on a lock directory in the cache
# root, so in the ordinary case only one of them is installing a tree at a time.
# The swap is a rename aside followed by a rename in, which is what lets a tree
# be replaced even when something is obstructing removal of the old one. And
# after the rename, the resolve checks that the tree at the destination carries
# the nonce it staged and holds the compiler, so a resolve that lost the race
# fails instead of printing a path.
#
# Be exact about what that check catches. It detects a writer that raced this
# resolve WITHOUT having read the staged nonce, which is every accidental case:
# an overlapping resolve, an indexer, a straggler, a run that was killed
# mid-swap. It is not proof of ownership and does not authenticate the tree. A
# process running as this user, with read and write access to the cache root,
# can read the staged nonce and reproduce it, and no resolver can defend
# against that: the same process could replace the compiler after the resolve
# has returned the path. The cache is a same-user working area, and that is the
# threat model. Staging happens under a directory only this user may enter,
# which keeps the nonce away from a DIFFERENT user; against the same user it
# does nothing, by design.
#
# The lock is not the correctness argument either: deleting it does not turn a
# wrong tree into a handed-out path, because the post-swap check is what decides
# whether the resolve succeeds. And none of this decides what is TRUSTED. The
# pinned archive digest does, and every resolve re-derives it from the bytes on
# disk before anything is extracted.
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
CACHE_LOCK="$CACHE_ROOT/wasi-sdk.lock"
# A lock older than this belonged to a run that was killed, and the next waiter
# breaks it.
LOCK_STALE_MINUTES=30
# A resolve holds the lock across its download and its extraction, so the wait
# has to tolerate a cold cache on a slow link. It also has to outlast the
# staleness threshold by a margin: a budget equal to it would have waiters give
# up at the very moment the lock became breakable.
LOCK_WAIT_SECONDS=$((LOCK_STALE_MINUTES * 60 + 300))
if [[ ${RGBX_TOOLCHAIN_LOCK_WAIT_SECONDS:-} =~ ^[1-9][0-9]{0,5}$ ]] &&
   [ "$RGBX_TOOLCHAIN_LOCK_WAIT_SECONDS" -lt "$LOCK_WAIT_SECONDS" ]; then
    # Test hook, and an escape hatch for a wedged cache. Clamped to the default
    # rather than taken as given, so it can only make this resolve give up
    # sooner: a longer wait would outlast the staleness threshold and park a
    # build on a lock nobody is going to release.
    LOCK_WAIT_SECONDS="$RGBX_TOOLCHAIN_LOCK_WAIT_SECONDS"
fi
# The relative path of the compiler the extracted tree must contain, checked
# after the swap so a tree this resolve did not install cannot be handed out.
TREE_COMPILER="bin/clang"
# Written into the staged tree just before the swap and read back afterwards.
RESOLVE_NONCE_FILE=".rgbx-resolve-nonce"
lock_held=0

# Serialize resolves that share this cache. The wasm and the arm configure of
# one build, or two builds, otherwise install into the same cache path at the
# same time. The lock is a directory because mkdir is atomic on every
# filesystem this cache lands on and, unlike flock, needs no extra binary on
# macOS. Correctness does not rest on it; see the header.
#
# mkdir also fails forever against a path that exists and is not a directory,
# which would otherwise burn the whole wait budget without saying why.
assert_lock_path_usable() {
    if [ -L "$CACHE_LOCK" ] || { [ -e "$CACHE_LOCK" ] && [ ! -d "$CACHE_LOCK" ]; }; then
        echo "error: $CACHE_LOCK exists and is not a directory; remove it and retry" >&2
        return 1
    fi
}

lock_is_stale() {
    local owner
    # Bounded read: the owner file is untrusted cache content. Only a plausible
    # pid is ever probed, and nothing else is passed to kill, which would
    # otherwise accept values such as -1 (signal every process this one may
    # signal, so the lock looks alive forever) or 0 (this process group).
    # Missing or malformed content means the owner is UNKNOWN, which is not the
    # same as dead: fall through to the age rule rather than break a live lock.
    owner="$(head -c 32 "$CACHE_LOCK/owner" 2>/dev/null || true)"
    if [[ $owner =~ ^[1-9][0-9]{0,9}$ ]] && ! kill -0 "$owner" 2>/dev/null; then
        return 0
    fi
    # find -mmin is the portable age comparison; stat's flags are not.
    [ -n "$(find "$CACHE_LOCK" -maxdepth 0 -mmin "+$LOCK_STALE_MINUTES" 2>/dev/null)" ]
}

acquire_cache_lock() {
    local deadline=$((SECONDS + LOCK_WAIT_SECONDS))
    mkdir -p "$CACHE_ROOT"
    assert_lock_path_usable || return 1
    while ! mkdir "$CACHE_LOCK" 2>/dev/null; do
        assert_lock_path_usable || return 1
        if lock_is_stale; then
            # Best effort: if another waiter breaks it first, the next mkdir
            # is what decides who owns it.
            rm -rf "$CACHE_LOCK" 2>/dev/null || true
        fi
        if [ "$SECONDS" -ge "$deadline" ]; then
            echo "error: timed out after ${LOCK_WAIT_SECONDS}s waiting for another toolchain resolve to release $CACHE_LOCK" >&2
            return 1
        fi
        sleep 0.2
    done
    lock_held=1
    printf '%s\n' "$$" > "$CACHE_LOCK/owner" 2>/dev/null || true
}

release_cache_lock() {
    [ "$lock_held" -eq 1 ] || return 0
    lock_held=0
    rm -rf "$CACHE_LOCK" 2>/dev/null || true
}
trap release_cache_lock EXIT

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
    # Staged under a directory only this user may enter. mktemp -d already
    # creates 0700, but say so rather than depend on it: the staged tree holds
    # the swap nonce, and a different user should not be able to read it.
    tmp="$(umask 077; mktemp -d "$CACHE_ROOT/wasi-sdk-x.XXXXXX")"
    chmod 700 "$tmp" 2>/dev/null || true
    trap 'rm -rf "$tmp"' RETURN
    tar -xzf "$archive" -C "$tmp"
    # The archive unpacks to wasi-sdk-<version>-<arch>-<os>/; normalize the name.
    local extracted="$tmp/wasi-sdk-$WASI_SDK_VERSION-$asset_arch-$asset_os"
    if [ ! -x "$extracted/bin/clang" ]; then
        echo "error: unexpected archive layout in $asset" >&2
        return 1
    fi
    swap_tree_into_place "$extracted"
}

# A value this resolve can recognize afterwards. Not a secret and not a
# credential: it only has to be unpredictable to a concurrent writer and never
# reused between resolves.
new_resolve_nonce() {
    local value=""
    if [ -r /dev/urandom ]; then
        value="$(od -An -N16 -tx1 < /dev/urandom 2>/dev/null | tr -d ' \n')"
    fi
    if [ -z "$value" ]; then
        # Predictable, and that is acceptable: per the threat model above the
        # nonce is not a credential, only a marker that distinguishes this
        # resolve's tree from one an unrelated writer put there.
        value="$$-$(date +%s)-${RANDOM:-0}"
    fi
    printf '%s' "$value"
}

# Remove a replaced tree. This is the one place where rm -rf failing is
# expected rather than surprising, because an aside exists precisely when
# something obstructed removal: a directory the current user cannot write, an
# immutable flag (chflags uchg, chattr +i), a network filesystem that renamed a
# still-open file into place under the tree, or another process creating
# entries under it while rm walks it. A permissions obstruction is liftable, so
# retry once after widening the modes; the rest are not, and a leftover is
# reported rather than silently accumulating.
remove_aside() {
    local aside="$1" why
    [ -e "$aside" ] || return 0
    rm -rf "$aside" 2>/dev/null && return 0
    chmod -R u+rwx "$aside" 2>/dev/null || true
    why="$(rm -rf "$aside" 2>&1)" && return 0
    echo "warning: could not remove the replaced wasi-sdk tree $aside: ${why:-unknown reason}" >&2
    return 1
}

# Put a freshly extracted tree at $tree using renames only, never a delete
# followed by a move. rm -rf can lose (remove_aside above lists what actually
# obstructs it), and then it leaves the directory behind and the following mv
# moves the new tree INSIDE the stale one instead of over it: that is what
# produced "Directory not empty" and trees nested one level deep. Renaming the
# old tree aside and the new tree in are both single renames within one
# filesystem, so each is atomic and neither cares what is obstructing removal
# of the old path.
#
# A rename in is still only a rename. If a directory reappears at $tree between
# the aside and the rename, the rename nests rather than failing, so the result
# is verified afterwards instead of assumed.
swap_tree_into_place() {
    local staged="$1"
    local aside="$tree.old.$$"
    local nonce
    nonce="$(new_resolve_nonce)"
    # Written with a restrictive umask, under a staging directory only this user
    # may enter. That keeps the marker away from a different user; it is not a
    # defence against this one (see the header).
    ( umask 077; printf '%s\n' "$nonce" > "$staged/$RESOLVE_NONCE_FILE" )

    remove_aside "$aside" || true
    if [ -e "$tree" ] && ! mv "$tree" "$aside"; then
        echo "error: could not move the previous wasi-sdk tree aside at $tree" >&2
        return 1
    fi
    if ! mv "$staged" "$tree"; then
        echo "error: could not move the freshly extracted wasi-sdk into place at $tree" >&2
        return 1
    fi
    if ! swapped_tree_is_ours "$nonce" "$staged"; then
        # The winner owns $tree now, so the tree this resolve moved aside is
        # nobody's: reap it here rather than leaving a full copy of the previous
        # toolchain in the cache until it ages out of the sweep.
        remove_aside "$aside" || true
        return 1
    fi
    sweep_aside_trees
}

# Check that the tree now at $tree is the one this resolve staged and that it
# holds the compiler. Two things make that false: the rename nested the staged
# tree inside a directory that reappeared underneath it, and another writer
# replaced the tree between the rename and this check. This catches a racer that
# has not read the staged nonce, which is every accidental race; a same-user
# process that reads it can reproduce it and defeat this, which is outside what
# a resolver can defend and is why the archive digest, not this, is the trust
# anchor. What this buys is that a lost race becomes a failure instead of a path
# to a tree this resolve did not stage.
swapped_tree_is_ours() {
    local nonce="$1" staged="$2" nested
    if [ ! -f "$tree/$RESOLVE_NONCE_FILE" ] ||
       [ "$(cat "$tree/$RESOLVE_NONCE_FILE" 2>/dev/null)" != "$nonce" ]; then
        # A rename into a directory that reappeared underneath moved the staged
        # tree INSIDE it. Take this resolve's copy back out so the loser of the
        # race leaves no debris in the winner's tree, and only ever remove a
        # tree carrying this resolve's own nonce.
        nested="$tree/$(basename "$staged")"
        if [ -f "$nested/$RESOLVE_NONCE_FILE" ] &&
           [ "$(cat "$nested/$RESOLVE_NONCE_FILE" 2>/dev/null)" = "$nonce" ]; then
            rm -rf "$nested" 2>/dev/null || true
        fi
        echo "error: another writer replaced $tree while this resolve was installing it" >&2
        return 1
    fi
    if [ ! -x "$tree/$TREE_COMPILER" ]; then
        echo "error: $tree/$TREE_COMPILER is missing or not executable after extraction" >&2
        return 1
    fi
}

# Drop aside directories: this run's, and any left by a run that was killed
# mid-swap. Another resolve's aside is left alone until it is older than the
# staleness threshold, so a concurrent swap is never disturbed. Nothing under
# the cache root is trusted, so this is housekeeping, but a leftover that cannot
# be removed is reported every time rather than passing in silence.
sweep_aside_trees() {
    local own="$tree.old.$$" stale
    for stale in "$tree".old.*; do
        [ -e "$stale" ] || continue
        if [ "$stale" != "$own" ] &&
           [ -z "$(find "$stale" -maxdepth 0 -mmin "+$LOCK_STALE_MINUTES" 2>/dev/null)" ]; then
            continue
        fi
        remove_aside "$stale" || true
    done
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

acquire_cache_lock
archive="$(ensure_verified_archive)"
extract_tree "$archive"
release_cache_lock
echo "$tree"
