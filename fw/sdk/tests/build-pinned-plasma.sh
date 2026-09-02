#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Stuart Alldritt
#
# Rebuild the canonical Plasma RGBX v2 module from the external repository this
# firmware pins, and check it against the pinned digests.
#
#   fw/sdk/tests/build-pinned-plasma.sh <extracted-sdk-dir> <output-dir>
#
# This repository does not carry a copy of the Plasma source. The canonical
# source lives in its own repository; fw/sdk/tests/plasma-v2-pin.json records
# the repository, the revision, and the SHA-256 of the module and the package
# that revision builds. The module is what
# fw/tests/extensions/wasm_mvp_runtime/src/plasma_v2_sdk_module.h commits as
# the RGBX v2 conformance fixture, so rebuilding it here and comparing digests
# is what keeps the fixture honest: a change in the pinned source, the
# compiler, the post-link gate, or the package builder shows up as a mismatch
# instead of a fixture nobody can re-derive.
#
# The build uses the SDK passed in, which CI packages from THIS checkout rather
# than the release the extension pins for its own development. Same reason
# community-extensions.yml does it: an artifact this firmware embeds has to
# match the ABI this firmware ships.
#
# A pin marked provisional names a revision that is not merged upstream yet.
# That is a hard gate, not a note: the build fails unless the caller sets
# RGBX_PLASMA_ALLOW_PROVISIONAL=1, and CI sets it on pull requests only. Be
# precise about what that buys. It does not stop a provisional pin from being
# merged: the pull request that carries one is green by design, so the change
# can be reviewed. What it stops is main staying green with one. The
# push-to-main run of sdk-ci gets no allowance and fails, and keeps failing
# until the pin is repointed. release.yaml never runs sdk-ci, so a release is
# not protected by a gate it runs itself; it is protected transitively, by main
# being green before a release is cut.
#
# Containment matches the community extension build. A full clone over a real
# transport, so the revision has to be reachable from the repository's own
# advertised refs: fetching a SHA directly, or cloning a local path (which
# hardlinks the object store and resolves objects no ref advertises), would
# also serve commits that exist only in a fork network, and a fork SHA paired
# with a trusted repository URL must not pass. Pin values are validated against
# tight patterns before any of them reaches a command, and they are passed as
# arguments, never spliced into script text. The repo and rev patterns below
# deliberately mirror the ones in extensions/validate-registry.mjs; keep the
# two in step.
#
# RGBX_PLASMA_GIT_SOURCE overrides where the clone comes from, for local
# verification against a checkout that is already on disk. It must be an https
# or file URL, which is also what keeps the reachability check meaningful. The
# pinned digests are the binding gate either way: no other source produces
# them.

set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "usage: build-pinned-plasma.sh <extracted-sdk-dir> <output-dir>" >&2
    exit 2
fi

sdk_dir="$1"
out_dir="$2"
tests_dir="$(cd "$(dirname "$0")" && pwd)"
pin="$tests_dir/plasma-v2-pin.json"

if [ ! -f "$sdk_dir/cmake/rgbx-sdk-config.cmake" ]; then
    echo "error: '$sdk_dir' does not look like an extracted rgbx-sdk tree" >&2
    exit 2
fi

# Read one string field out of the pin. Anything missing, non-string, or empty
# is a malformed pin rather than a default to fall back on.
pin_field() {
    local value
    if ! value="$(node -e '
const { readFileSync } = require("node:fs");
const pin = JSON.parse(readFileSync(process.argv[1], "utf8"));
const value = pin[process.argv[2]];
if (typeof value !== "string" || value.length === 0) {
    process.exit(1);
}
process.stdout.write(value);
' "$pin" "$1")"; then
        echo "error: $pin has no usable '$1' field" >&2
        exit 1
    fi
    printf '%s' "$value"
}

# Anchored whole-value validation. A line-oriented matcher is not enough here:
# a value carrying an embedded newline satisfies a ^...$ pattern one line at a
# time while still reaching a command as one argument holding two lines, so
# line terminators are rejected outright before the pattern is applied.
require_match() {
    local label="$1" value="$2" pattern="$3" kind="${4:-field}"
    case "$value" in
        *$'\n'* | *$'\r'*)
            echo "error: $kind '$label' contains a line terminator" >&2
            exit 1
            ;;
    esac
    if [[ ! $value =~ $pattern ]]; then
        echo "error: $kind '$label' does not match $pattern" >&2
        exit 1
    fi
}

repo="$(pin_field repo)"
rev="$(pin_field rev)"
artifact="$(pin_field artifact)"
module_sha256="$(pin_field moduleSha256)"
package_sha256="$(pin_field packageSha256)"

# Charset-tight on purpose: these values steer a clone and a build, so they are
# an injection surface and not just a format check. Owner names are
# alphanumeric plus hyphen; repository names add . and _.
require_match repo "$repo" '^https://github\.com/[A-Za-z0-9-]+/[A-Za-z0-9._-]+$'
# A full commit SHA, never a branch or a tag: tampering with the source
# repository after review has to be inert.
require_match rev "$rev" '^[0-9a-f]{40}$'
require_match artifact "$artifact" '^[a-z0-9_]{1,25}$'
require_match moduleSha256 "$module_sha256" '^[0-9a-f]{64}$'
require_match packageSha256 "$package_sha256" '^[0-9a-f]{64}$'

# provisional is required, and it has to be a JSON boolean. Reading it as a
# strict comparison against true made every other shape mean "not provisional":
# an absent field, or a string "true" from a hand edit, disarmed the gate in
# silence. Absence and the wrong type are malformed-pin errors, exactly like
# every string field above.
#   0 = provisional, 1 = not provisional, 2 = missing or not a boolean
provisional_state() {
    node -e '
const { readFileSync } = require("node:fs");
const pin = JSON.parse(readFileSync(process.argv[1], "utf8"));
if (!Object.prototype.hasOwnProperty.call(pin, "provisional") ||
    typeof pin.provisional !== "boolean") {
    process.exit(2);
}
process.exit(pin.provisional ? 0 : 1);
' "$pin"
}

provisional_rc=0
provisional_state || provisional_rc=$?
case "$provisional_rc" in
    0)
        if [ "${RGBX_PLASMA_ALLOW_PROVISIONAL:-}" != "1" ]; then
            echo "error: $pin is marked provisional: it names a revision that is not merged upstream." >&2
            echo "       Set RGBX_PLASMA_ALLOW_PROVISIONAL=1 to build against it anyway, which is" >&2
            echo "       what CI does on pull requests. The push-to-main run gets no allowance and" >&2
            echo "       stays red until the pin names a merged revision." >&2
            exit 1
        fi
        echo "warning: building against a provisional pin ($pin); it must name a merged upstream revision before release" >&2
        ;;
    1) ;;
    *)
        echo "error: $pin must carry a 'provisional' field holding a JSON boolean" >&2
        exit 1
        ;;
esac

sha256_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    else
        shasum -a 256 "$1" | cut -d' ' -f1
    fi
}

work="$(mktemp -d "${TMPDIR:-/tmp}/rgbx-plasma-pin.XXXXXX")"
trap 'rm -rf "$work"' EXIT

source_url="${RGBX_PLASMA_GIT_SOURCE:-$repo}"
if [ "$source_url" != "$repo" ]; then
    # A real transport URL or nothing. A bare path would let git use the local
    # optimization, which hardlinks the object store and resolves commits no
    # ref advertises, and a value starting with a dash would be read as an
    # option such as --upload-pack.
    require_match RGBX_PLASMA_GIT_SOURCE "$source_url" \
        '^(https|file)://[^[:space:]]+$' "environment variable"
    echo "note: cloning from the RGBX_PLASMA_GIT_SOURCE override instead of $repo" >&2
fi
git clone --quiet --no-checkout --no-local -- "$source_url" "$work/src"
if ! git -C "$work/src" cat-file -e "$rev^{commit}" 2>/dev/null; then
    echo "error: pinned rev $rev is not reachable from $source_url's advertised refs (fork-network SHA?)" >&2
    exit 1
fi
git -C "$work/src" checkout --quiet "$rev"

cmake -S "$work/src" -B "$work/build" \
    -DRGBX_TARGET=rgbx-v2 \
    -DRGBX_SDK_SOURCE_DIR="$sdk_dir" \
    -DRGBX_STRICT_TOOLCHAIN=ON
cmake --build "$work/build"

module="$work/build/$artifact.wasm"
package="$work/build/$artifact.rgbx"
if [ ! -f "$module" ] || [ ! -f "$package" ]; then
    echo "error: the pinned revision's rgbx-v2 target did not produce $artifact.wasm and $artifact.rgbx" >&2
    ls -1 "$work/build" >&2
    exit 1
fi

check_digest() {
    local label="$1" path="$2" expected="$3" actual
    actual="$(sha256_file "$path")"
    if [ "$actual" != "$expected" ]; then
        echo "error: $label built from $rev hashes to $actual, but the pin expects $expected" >&2
        echo "       Either the pin is stale, or the toolchain stopped reproducing it." >&2
        exit 1
    fi
}

check_digest "$artifact.wasm" "$module" "$module_sha256"
check_digest "$artifact.rgbx" "$package" "$package_sha256"

mkdir -p "$out_dir"
cp "$module" "$package" "$out_dir/"
echo "$artifact from $repo@$rev matches the pinned module digest $module_sha256"
echo "$artifact package matches the pinned package digest $package_sha256"
