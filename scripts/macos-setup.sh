#!/usr/bin/env bash
#
# macos-setup.sh — bootstrap a Mac (Apple Silicon, e.g. the Mac Mini) for
# FIRMWARE development + the shared agent tooling in this repo.
#
# The Linux devcontainer is the primary firmware environment; this script is
# its macOS equivalent (the role the .devcontainer/Dockerfile plays there) for
# when the dev board is physically attached to a Mac. It installs:
#
#   - Homebrew bash >= 4 ....... scripts/hw-lock.sh needs it (macOS ships 3.2)
#   - Go + mcumgr CLI .......... firmware OTA flashing over the MCUmgr serial
#                                port (no J-Link required; fw/scripts/mcumgr-flash.sh)
#   - nrfutil + NCS v3.1.1 ..... the nRF Connect SDK toolchain + source tree at
#                                ~/ncs/v3.1.1 (mirrors the devcontainer's
#                                /root/ncs/v3.1.1) — a very large download
#   - serial-mcp-server ........ the `serial_mcp` MCP server .mcp.json expects
#   - GLIM asset tooling ....... ffmpeg + a python venv (Pillow/numpy/lz4/yt-dlp)
#                                that the fw/tools/convert_*.py generators need
#
# The iOS app toolchain is separate: app/scripts/macos-setup.sh.
#
# It is idempotent — every step probes before installing, so it is safe to
# re-run (e.g. after a partial NCS download).
#
# Usage:
#     ./scripts/macos-setup.sh
#
set -euo pipefail

NCS_VERSION="v3.1.1"
NCS_DIR="${HOME}/ncs"
NCS_SDK_DIR="${NCS_DIR}/${NCS_VERSION}"
NCS_VENV="${NCS_DIR}/venv-${NCS_VERSION}"
NCS_ENV_FILE="${NCS_DIR}/env-${NCS_VERSION}.sh"
NRFUTIL_URL="https://developer.nordicsemi.com/.pc-tools/nrfutil/universal-osx/nrfutil"

# The Homebrew python formula every venv here is built on. Deliberately pinned
# one minor behind Homebrew's rolling `python3`: a venv built on that one dangles
# the day the formula behind it is replaced/autoremoved, and the existence probes
# below would still pass, so re-running this script would NOT repair it. Pinning
# also buys the binary-wheel availability window (checked 2026-08: lz4 ships
# cp314 but not yet cp315, while numpy/Pillow already ship both). Bumping this
# one line and re-running is the whole upgrade procedure.
PYTHON_PIN="3.12"
TOOLS_VENV_DIR="${HOME}/.cache/rgb-sunglasses"
TOOLS_VENV="${TOOLS_VENV_DIR}/tools-venv-${PYTHON_PIN}"
TOOLS_VENV_CURRENT="${TOOLS_VENV_DIR}/tools-venv-current"

info()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn()  { printf '\033[1;33mwarn:\033[0m %s\n' "$*" >&2; }
fail()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

# --- 0. Sanity: this is a Mac --------------------------------------------------
[[ "$(uname -s)" == "Darwin" ]] || fail "this script must run on macOS."

# --- 1. Homebrew ---------------------------------------------------------------
if ! command -v brew >/dev/null 2>&1; then
    fail "Homebrew not found. Install it from https://brew.sh and re-run."
fi
BREW_BIN="$(brew --prefix)/bin"
info "Homebrew: $(brew --version | head -1) (${BREW_BIN})"

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

# --- 2. bash >= 4 (for scripts/hw-lock.sh) -------------------------------------
# macOS ships bash 3.2; hw-lock.sh uses associative arrays and re-execs itself
# into this Homebrew bash when invoked under the stock one.
if [[ -x "${BREW_BIN}/bash" ]]; then
    info "Homebrew bash already present ($("${BREW_BIN}/bash" --version | head -1))"
else
    info "Installing bash via Homebrew (needed by scripts/hw-lock.sh)..."
    brew install bash
fi

# --- 3. Go + mcumgr CLI --------------------------------------------------------
# Mirrors the devcontainer Dockerfile's mcumgr build. GOBIN is pointed at the
# brew bin dir so `mcumgr` is on PATH even in non-login shells (agent Bash
# calls); on Apple Silicon that dir is user-owned, no sudo needed.
brew_ensure go go
if command -v mcumgr >/dev/null 2>&1; then
    info "mcumgr already present ($(command -v mcumgr))"
else
    info "Building mcumgr CLI from source (go install)..."
    GOBIN="${BREW_BIN}" go install github.com/apache/mynewt-mcumgr-cli/mcumgr@latest
    info "mcumgr installed to ${BREW_BIN}/mcumgr"
fi

# --- 4. serial-mcp-server (the `serial_mcp` command in .mcp.json) --------------
brew_ensure pipx pipx
if command -v serial_mcp >/dev/null 2>&1; then
    info "serial_mcp already present ($(command -v serial_mcp))"
else
    info "Installing serial-mcp-server via pipx..."
    pipx install serial-mcp-server
    # .mcp.json spawns the bare command `serial_mcp`; the MCP server process may
    # not inherit a login-shell PATH that includes ~/.local/bin, so link it into
    # the brew bin dir. -sf so a stale/broken leftover link can't abort the
    # script under set -e (`! -e` is false for a broken symlink).
    if [[ -x "${HOME}/.local/bin/serial_mcp" ]]; then
        ln -sf "${HOME}/.local/bin/serial_mcp" "${BREW_BIN}/serial_mcp"
        info "Linked serial_mcp into ${BREW_BIN}"
    fi
fi

# --- 5. nrfutil ----------------------------------------------------------------
# Not needed for the OTA dev loop, but `nrfutil device` becomes useful the day
# a J-Link is attached to this Mac. NOTE: `nrfutil sdk-manager` is deliberately
# NOT used for the NCS install below — on macOS it hard-refuses any install dir
# but /opt/nordic/ncs, which requires sudo to create.
if command -v nrfutil >/dev/null 2>&1; then
    info "nrfutil already present ($(command -v nrfutil))"
else
    info "Downloading nrfutil (Nordic universal macOS binary)..."
    curl -fSL "${NRFUTIL_URL}" -o "${BREW_BIN}/nrfutil"
    chmod +x "${BREW_BIN}/nrfutil"
    info "nrfutil installed to ${BREW_BIN}/nrfutil"
fi

# --- 6. NCS ${NCS_VERSION} (west workspace + Zephyr SDK toolchain) -------------
# Manual west install, mirroring what the devcontainer's base image is at
# /root/ncs/<version>: an sdk-nrf west workspace pinned to the same version,
# here at ~/ncs/<version>, with west + all Zephyr python deps isolated in a
# dedicated venv. This is the big one (several GB of git clones on a fresh
# install; --narrow/--depth=1 keeps it tractable).

# Zephyr host build deps (the devcontainer base image ships these too).
brew_ensure cmake cmake
brew_ensure ninja ninja
brew_ensure dtc dtc
brew_ensure gperf gperf
brew_ensure ccache ccache
brew_ensure wget wget   # the Zephyr SDK's setup.sh fetches toolchains with wget

# GNU coreutils for `timeout` — used by the documented hw-lock poll incantation
# (macOS ships no timeout at all; coreutils installs it as gtimeout, so add an
# unprefixed symlink — nothing else on macOS claims that name). -sf so a
# stale/broken leftover link can't abort the script under set -e.
brew_ensure coreutils gtimeout
if ! command -v timeout >/dev/null 2>&1; then
    ln -sf "$(command -v gtimeout)" "${BREW_BIN}/timeout"
    info "Linked timeout -> gtimeout in ${BREW_BIN}"
fi

# A pinned modern python for the venvs (the OS python can be too old); see
# PYTHON_PIN at the top of this file for why it is pinned rather than `python3`.
if ! command -v "python${PYTHON_PIN}" >/dev/null 2>&1; then
    info "Installing python@${PYTHON_PIN} via Homebrew..."
    brew install "python@${PYTHON_PIN}"
fi

if [[ -x "${NCS_VENV}/bin/west" ]]; then
    info "west venv already present (${NCS_VENV})"
else
    info "Creating the NCS python venv at ${NCS_VENV}..."
    "python${PYTHON_PIN}" -m venv "${NCS_VENV}"
    "${NCS_VENV}/bin/pip" install --quiet --upgrade pip west
fi

if [[ -d "${NCS_SDK_DIR}/.west" ]]; then
    info "NCS workspace already initialized at ${NCS_SDK_DIR}"
else
    info "Initializing the NCS ${NCS_VERSION} west workspace at ${NCS_SDK_DIR}..."
    "${NCS_VENV}/bin/west" init -m https://github.com/nrfconnect/sdk-nrf --mr "${NCS_VERSION}" "${NCS_SDK_DIR}"
fi

# Idempotent: re-running west update after an interrupted download resumes it.
info "Fetching/updating all NCS modules (west update — very large on first run)..."
(cd "${NCS_SDK_DIR}" && "${NCS_VENV}/bin/west" update --narrow -o=--depth=1)

info "Installing Zephyr/NCS python dependencies into the venv..."
(cd "${NCS_SDK_DIR}" && "${NCS_VENV}/bin/west" packages pip --install)

info "Installing the Zephyr SDK ARM toolchain (west sdk install)..."
(cd "${NCS_SDK_DIR}" && "${NCS_VENV}/bin/west" sdk install --toolchains arm-zephyr-eabi)

# --- 7. Write the build environment file ---------------------------------------
# scripts/fw-env.sh sources this to put west + the toolchain env on PATH. The
# version-stable `env-current.sh` symlink is the ONLY name other scripts
# (fw-env.sh, check-software.sh) reference, so bumping NCS_VERSION at the top of
# this file and re-running is the complete version-bump procedure — nothing else
# hardcodes the version.
info "Writing the build environment file to ${NCS_ENV_FILE}..."
cat > "${NCS_ENV_FILE}" <<EOF
# Generated by scripts/macos-setup.sh — source this to build firmware on macOS
# (scripts/fw-env.sh does it for you). Activates the NCS venv (west + Zephyr
# python deps) and points west/cmake at the ${NCS_VERSION} workspace; ZEPHYR_BASE
# lets 'west build' run from the repo root (outside the workspace), same as the
# devcontainer.
source "${NCS_VENV}/bin/activate"
export ZEPHYR_BASE="${NCS_SDK_DIR}/zephyr"
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
EOF
ln -sf "$(basename "${NCS_ENV_FILE}")" "${NCS_DIR}/env-current.sh"

# --- 8. Extension-simulator wasm toolchain -------------------------------------
# The pinned wasi-sdk fw/sim/ compiles rgbx extensions with (the macOS
# equivalent of the .devcontainer/Dockerfile layer). install-toolchain.sh is
# idempotent: it resolves an existing install or downloads the macOS tarball
# into ~/.cache/rgb-sunglasses once.
info "Installing the extension-simulator wasm toolchain (wasi-sdk)..."
"$(cd "$(dirname "$0")/.." && pwd)/fw/sim/scripts/install-toolchain.sh" >/dev/null

# --- 9. Doxygen (extension API docs) -------------------------------------------
# fw/extensions/build-docs.sh is a documented pre-PR step for anything touching
# fw/include/rgbx/, and sdk-ci.yml gates it with WARN_AS_ERROR — so the version
# has to match the one CI uses or a clean local run can still go red. The pin
# lives in build-docs.sh (which also asserts it at run time); Homebrew's
# `doxygen` formula tracks latest, so install the pinned release directly, the
# same way the devcontainer Dockerfile does.
DOXYGEN_VERSION="$(grep -oE '^DOXYGEN_VERSION="[^"]+"' \
    "$(cd "$(dirname "$0")/.." && pwd)/fw/extensions/build-docs.sh" | cut -d'"' -f2)"
DOXYGEN_PREFIX="${HOME}/.cache/rgb-sunglasses/doxygen-${DOXYGEN_VERSION}"
if [ -x "${DOXYGEN_PREFIX}/bin/doxygen" ]; then
    info "Doxygen ${DOXYGEN_VERSION} already installed (${DOXYGEN_PREFIX})"
else
    info "Installing Doxygen ${DOXYGEN_VERSION} (extension API docs)..."
    mkdir -p "${DOXYGEN_PREFIX}"
    # Upstream ships a universal .dmg for macOS; the tarball is Linux-only, so
    # mount the dmg and copy the binary out of the app bundle.
    _dmg="$(mktemp -d)/doxygen.dmg"
    curl -fsSL --retry 5 --retry-delay 5 \
        "https://github.com/doxygen/doxygen/releases/download/Release_${DOXYGEN_VERSION//./_}/Doxygen-${DOXYGEN_VERSION}.dmg" \
        -o "${_dmg}"
    _mnt="$(hdiutil attach -nobrowse -readonly -mountrandom /tmp "${_dmg}" | awk '/\/tmp\//{print $NF}' | tail -1)"
    mkdir -p "${DOXYGEN_PREFIX}/bin"
    cp "${_mnt}/Doxygen.app/Contents/Resources/doxygen" "${DOXYGEN_PREFIX}/bin/doxygen"
    hdiutil detach -quiet "${_mnt}"
    rm -f "${_dmg}"
fi
# Symlink onto PATH so `doxygen` resolves in non-login shells (agent Bash calls).
ln -sf "${DOXYGEN_PREFIX}/bin/doxygen" "${BREW_BIN}/doxygen"

# --- 10. GLIM asset tooling (the fw/tools converters) --------------------------
# Every .glim asset is generated from source by fw/tools/convert_*.py — nothing is
# checked into the repo as a binary (fw/CLAUDE.md, "Setting up GLIM files on a new
# board"). Those scripts need four things this Mac ships none of: ffmpeg and
# yt-dlp as subprocesses, plus Pillow/numpy/lz4 as importable modules.
#
# Homebrew's python3 is PEP 668 externally-managed, so a plain `pip3 install` is
# refused outright; the packages go in a dedicated venv that scripts/tools-env.sh
# activates — the same split as the NCS venv + scripts/fw-env.sh above.
#
# ONE venv rather than pipx-for-yt-dlp plus a venv for the libraries, even though
# section 4 uses pipx for serial-mcp-server: pipx installs *applications*, and
# Pillow/numpy/lz4 are importable libraries with no console scripts, so they need
# this venv regardless. Co-locating yt-dlp in it is also what makes the deno JS
# runtime work with no PATH surgery at all — yt-dlp needs deno to solve YouTube's
# nsig challenge (issue #358; the `deno` pip extra ships the binary as a wheel),
# and it looks for that binary in its OWN interpreter's scripts dir before
# falling back to PATH (_find_exe() in yt_dlp/utils/_jsruntime.py).
#
# Deliberately NOT installed: yaspin and tqdm (both in the devcontainer's pip
# line) — nothing in this repo imports either one. numpy IS installed explicitly
# because convert_bad_apple.py imports it directly.

# Hand-rolled rather than brew_ensure: ffmpeg spells its version flag `-version`,
# so the helper's `--version` probe prints an empty version string. Same reason
# the bash and python@N steps above bypass it.
if command -v ffmpeg >/dev/null 2>&1; then
    info "ffmpeg already present ($(ffmpeg -version 2>/dev/null | head -1))"
else
    info "Installing ffmpeg via Homebrew (large dependency closure — several minutes)..."
    brew install ffmpeg
fi

if [[ -x "${TOOLS_VENV}/bin/python" ]]; then
    info "GLIM tools venv already present (${TOOLS_VENV})"
else
    info "Creating the GLIM tools python venv at ${TOOLS_VENV}..."
    "python${PYTHON_PIN}" -m venv "${TOOLS_VENV}"
    "${TOOLS_VENV}/bin/pip" install --quiet --upgrade pip
fi

# Unconditional, and cheap: once every requirement is satisfied pip touches no
# network and prints nothing, so re-running is also what makes a newly-added
# package land in an existing venv.
info "Installing the GLIM converter python dependencies..."
"${TOOLS_VENV}/bin/pip" install --quiet Pillow numpy lz4

# yt-dlp is the one package here that goes stale on its own — YouTube breaks
# extraction on a weeks timescale, and unlike the devcontainer (whose pip line
# re-runs on every rebuild) this venv would otherwise keep its first version
# forever. --upgrade makes "re-run scripts/macos-setup.sh" the documented fix for
# a GLIM download that suddenly starts failing.
info "Installing/updating yt-dlp + the deno JS runtime..."
"${TOOLS_VENV}/bin/pip" install --quiet --upgrade 'yt-dlp[default,deno]'

# Version-stable symlink, same idea as ~/ncs/env-current.sh: scripts/tools-env.sh
# and check-software.sh reference only this name, so bumping PYTHON_PIN at the top
# of this file and re-running is the complete procedure (the old venv is left
# behind rather than silently reused). -sf so a stale/broken leftover link can't
# abort the script under set -e (`! -e` is false for a broken symlink).
ln -sf "$(basename "${TOOLS_VENV}")" "${TOOLS_VENV_CURRENT}"

# --- 11. Headless-CI policies (board disk access, issue #367) ------------------
# loginwindow blocks disk mounts and EJECTS newly-appearing disks whenever its
# lock shield is up — and the shield engages on DISPLAY DIM
# (kLWLockFromDisplayDim), not just an explicit lock. Two measured facts govern
# what helps here (2026-08-13, on this Mac Mini):
#   - AutomountDisksWithoutUserLogin does NOT stop the lock-shield eject (tested:
#     identical dissent 0xF8DA0008 + eject with it set). It is still set below
#     because it covers the OTHER headless case — mounting disks when nobody has
#     logged in at all, e.g. after a power cycle. (SIP blocks kickstarting
#     diskarbitrationd, so it fully applies from the next macOS reboot.)
#   - `pmset -a displaysleep 0` is what actually keeps the disk reachable: the
#     display never dims, so the shield (and its DA blockers) never re-arm.
# Sudo steps; each is idempotent and non-fatal if declined.
if [ "$(defaults read /Library/Preferences/SystemConfiguration/autodiskmount AutomountDisksWithoutUserLogin 2>/dev/null)" = "1" ]; then
    info "AutomountDisksWithoutUserLogin already enabled"
else
    info "Enabling AutomountDisksWithoutUserLogin (mount with no user logged in — needs sudo)..."
    sudo defaults write /Library/Preferences/SystemConfiguration/autodiskmount AutomountDisksWithoutUserLogin -bool true \
        || warn "sudo declined — disks won't mount when no user is logged in."
fi

if pmset -g 2>/dev/null | grep -qE '^[[:space:]]*displaysleep[[:space:]]+0([[:space:]]|$)'; then
    info "Display sleep already disabled"
else
    info "Disabling display sleep (display dim re-arms the disk-eject shield — needs sudo)..."
    sudo pmset -a displaysleep 0 \
        || warn "sudo declined — the board's disk will vanish whenever the display dims (#367)."
fi

# --- 12. Summary ---------------------------------------------------------------
info "Done."
echo
echo "Firmware dev loop on this Mac:"
echo "  . scripts/fw-env.sh              # put west + the NCS toolchain on PATH"
echo "  /build-proto0                    # (or the west build command it documents)"
echo "  fw/scripts/mcumgr-flash.sh       # OTA-flash the app image over serial (no J-Link)"
echo
echo "GLIM asset generation on this Mac:"
echo "  . scripts/tools-env.sh           # Pillow/numpy/lz4 + yt-dlp/ffmpeg for the converters"
echo "  python3 fw/tools/generate_nyan_cat_glim.py --output nyan_cat.glim"
echo
echo "Notes:"
echo "  - The first proto0 build configures from scratch and is very slow (tens of minutes)."
echo "  - Twister tests (native_sim) do NOT run on macOS — use CI or the devcontainer."
echo "  - fw-env.sh and tools-env.sh activate DIFFERENT venvs — use separate shells."
echo "  - yt-dlp goes stale as YouTube changes; re-run this script to refresh it."
echo "  - Board disk access needs the screen UNLOCKED — loginwindow ejects it when locked (#367)."
echo "  - iOS app toolchain is separate: app/scripts/macos-setup.sh"
