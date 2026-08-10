#!/usr/bin/env bash
# Build the rgbx extension API docs (Doxygen) into fw/build/doxygen/html.
#
#   fw/extensions/build-docs.sh            # version stamped from `git describe`
#   RGBX_DOC_VERSION=local fw/extensions/build-docs.sh
#
# Single source of truth for the invocation: used by developers, by sdk-ci.yml's
# `docs` gate, and by pages.yml's deploy, so all three build the docs the same
# way. Two things it exists to get right, both of which were previously left to
# whoever typed the command:
#
#  1. Doxygen resolves the Doxyfile's relative paths against the CURRENT WORKING
#     DIRECTORY, not the config file's location, and fw/build must already
#     exist. Running it from the wrong place either fails or writes the output
#     somewhere unexpected.
#
#  2. The Doxygen version is asserted, not assumed. The build runs with
#     WARN_AS_ERROR, and Doxygen's warning set shifts between releases, so a
#     mismatched local binary can pass while CI fails. Worse, it can also pass
#     while producing a subtly WRONG page: header.html is generated from the
#     1.12.0 template, and an older Doxygen leaves its `$langISO` /
#     `$projecticon` / `$darkmode` placeholders unsubstituted in the output
#     while still exiting 0. That is invisible unless you diff the HTML.
#
# DOXYGEN_VERSION below is the authoritative pin. `.github/actions/doxygen`
# greps it from this file (same pattern as fw/sim/scripts/install-toolchain.sh
# and the sim-wasm-toolchain action); .devcontainer/Dockerfile and
# scripts/macos-setup.sh install it.
set -euo pipefail

DOXYGEN_VERSION="1.12.0"

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

DOXYGEN="${DOXYGEN:-doxygen}"
if ! command -v "$DOXYGEN" >/dev/null 2>&1; then
    cat >&2 <<EOF
error: doxygen not found on PATH.

  devcontainer: rebuild the container (.devcontainer/Dockerfile installs
                Doxygen $DOXYGEN_VERSION)
  macOS:        run scripts/macos-setup.sh
EOF
    exit 1
fi

actual="$("$DOXYGEN" --version 2>/dev/null | cut -d' ' -f1)"
if [ "$actual" != "$DOXYGEN_VERSION" ]; then
    cat >&2 <<EOF
error: Doxygen $DOXYGEN_VERSION is required, found $actual.

The docs build treats warnings as errors and the HTML header template is
version-specific, so a different Doxygen can fail in CI after passing here — or
silently emit a page with unsubstituted template placeholders.

  devcontainer: rebuild the container
  macOS:        run scripts/macos-setup.sh
  override:     DOXYGEN=/path/to/doxygen-$DOXYGEN_VERSION/bin/doxygen $0
EOF
    exit 1
fi

# Stamped into the page header so published docs say which firmware release
# they correspond to. `--always` keeps this working in a shallow clone or a
# fork with no fw-v* tags.
if [ -z "${RGBX_DOC_VERSION:-}" ]; then
    RGBX_DOC_VERSION="$(git -C "$REPO_ROOT" describe --tags --match 'fw-v*' --always 2>/dev/null || echo local)"
fi
export RGBX_DOC_VERSION

mkdir -p "$REPO_ROOT/fw/build"

cd "$REPO_ROOT/fw/extensions"

# Derive the output tree from the Doxyfile rather than repeating it here. The
# rm -rf below is destructive and the two must agree: if OUTPUT_DIRECTORY (or
# HTML_OUTPUT) is ever changed and this copy isn't, the clean silently stops
# pruning the tree Doxygen actually writes.
doxy_setting() {
    sed -n "s/^$1[[:space:]]*=[[:space:]]*//p" Doxyfile | tail -1
}
OUT_DIR="$(doxy_setting OUTPUT_DIRECTORY)"
HTML_SUBDIR="$(doxy_setting HTML_OUTPUT)"
: "${HTML_SUBDIR:=html}"   # Doxygen's default when the setting is absent
if [ -z "$OUT_DIR" ]; then
    echo "error: could not read OUTPUT_DIRECTORY from $PWD/Doxyfile" >&2
    exit 1
fi

# Doxygen never removes files it no longer generates, so a page whose source was
# renamed or dropped lingers in the output — and pages.yml copies the whole tree
# to site/api, which would publish the orphan next to its replacement. Start clean.
rm -rf "$OUT_DIR"

"$DOXYGEN" Doxyfile

HTML_DIR="$(cd "$OUT_DIR/$HTML_SUBDIR" && pwd)"

echo "docs written to $HTML_DIR (version: $RGBX_DOC_VERSION)"
