#!/usr/bin/env bash
#
# macos-setup.sh — bootstrap a Mac (Apple Silicon, e.g. Mac Mini M1) to build and
# run the RGB Sunglasses iOS app.
#
# iOS native builds require macOS + Xcode and therefore cannot run inside the
# Linux devcontainer used for firmware and the Android app. This script is the
# self-setup equivalent for the iOS toolchain: it verifies/installs the host
# tooling, installs the JS dependencies, and generates the native iOS project.
#
# It is idempotent — every step checks for what it needs before doing anything,
# so it is safe to re-run.
#
# Usage:
#     ./scripts/macos-setup.sh
#
# After it finishes, run the app on the iOS Simulator with:
#     npm run ios            # (or: npx expo run:ios)
#
set -euo pipefail

# Resolve repo paths relative to this script so it works from any CWD.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

info()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn()  { printf '\033[1;33mwarn:\033[0m %s\n' "$*" >&2; }
fail()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

# --- 0. Sanity: this is a Mac --------------------------------------------------
[[ "$(uname -s)" == "Darwin" ]] || fail "this script must run on macOS."

# --- 1. Xcode + command line tools --------------------------------------------
if ! xcode-select -p >/dev/null 2>&1; then
    warn "Xcode command line tools not found. Launching the installer..."
    xcode-select --install || true
    fail "install the Xcode command line tools (and the full Xcode.app from the App Store), then re-run."
fi
if ! command -v xcodebuild >/dev/null 2>&1; then
    fail "xcodebuild not found. Install the full Xcode.app from the App Store and run 'sudo xcode-select -s /Applications/Xcode.app', then re-run."
fi
info "Xcode: $(xcodebuild -version | head -1)"

# --- 2. Homebrew ---------------------------------------------------------------
if ! command -v brew >/dev/null 2>&1; then
    fail "Homebrew not found. Install it from https://brew.sh and re-run."
fi
info "Homebrew: $(brew --version | head -1)"

# brew_ensure <formula> <command-to-probe>
brew_ensure() {
    local formula="$1" probe="$2"
    if command -v "${probe}" >/dev/null 2>&1; then
        info "${probe} already present ($("${probe}" --version 2>/dev/null | head -1))"
    else
        info "Installing ${formula} via Homebrew..."
        brew install "${formula}"
    fi
}

# --- 3. Host tooling -----------------------------------------------------------
# node: required to run Expo/Metro. We don't force a version manager here; if a
# node is already on PATH we use it, otherwise install the LTS via brew.
brew_ensure node node
# watchman: recommended by React Native for fast file watching.
brew_ensure watchman watchman
# cocoapods: installs the iOS native pods. Prefer an existing install (system
# gem or brew) over reinstalling.
if command -v pod >/dev/null 2>&1; then
    info "CocoaPods already present ($(pod --version))"
else
    brew_ensure cocoapods pod
fi
# applesimutils: optional, lets Expo pick simulators by name. Non-fatal.
if ! command -v applesimutils >/dev/null 2>&1; then
    info "Installing applesimutils (optional) via Homebrew..."
    brew install applesimutils || warn "applesimutils install failed (optional) — continuing."
fi

# --- 4. JS dependencies --------------------------------------------------------
info "Installing JS dependencies (npm ci) in ${APP_DIR}..."
cd "${APP_DIR}"
if [[ -f package-lock.json ]]; then
    npm ci
else
    npm install
fi

# --- 5. Generate the native iOS project ----------------------------------------
# `expo prebuild` regenerates ios/ from app.json + config plugins and runs
# `pod install`. The ios/ directory is gitignored (managed workflow).
info "Generating the native iOS project (expo prebuild)..."
npx expo prebuild --platform ios --clean

info "Done."
echo
echo "Next steps:"
echo "  cd ${APP_DIR}"
echo "  npm run ios            # build + launch on the iOS Simulator"
echo
echo "Note: BLE requires a physical iPhone — the iOS Simulator has no Bluetooth"
echo "radio, so device scanning will find nothing there."
