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
    # the brew bin dir.
    if [[ -x "${HOME}/.local/bin/serial_mcp" && ! -e "${BREW_BIN}/serial_mcp" ]]; then
        ln -s "${HOME}/.local/bin/serial_mcp" "${BREW_BIN}/serial_mcp"
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
# unprefixed symlink — nothing else on macOS claims that name).
brew_ensure coreutils gtimeout
if ! command -v timeout >/dev/null 2>&1; then
    ln -s "$(command -v gtimeout)" "${BREW_BIN}/timeout"
    info "Linked timeout -> gtimeout in ${BREW_BIN}"
fi

# A pinned modern python for the venv (the OS python can be too old).
if ! command -v python3.12 >/dev/null 2>&1; then
    info "Installing python@3.12 via Homebrew..."
    brew install python@3.12
fi

if [[ -x "${NCS_VENV}/bin/west" ]]; then
    info "west venv already present (${NCS_VENV})"
else
    info "Creating the NCS python venv at ${NCS_VENV}..."
    python3.12 -m venv "${NCS_VENV}"
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
# scripts/fw-env.sh sources this to put west + the toolchain env on PATH.
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

# --- 8. Summary ----------------------------------------------------------------
info "Done."
echo
echo "Firmware dev loop on this Mac:"
echo "  . scripts/fw-env.sh              # put west + the NCS toolchain on PATH"
echo "  /build-proto0                    # (or the west build command it documents)"
echo "  fw/scripts/mcumgr-flash.sh       # OTA-flash the app image over serial (no J-Link)"
echo
echo "Notes:"
echo "  - The first proto0 build configures from scratch and is very slow (tens of minutes)."
echo "  - Twister tests (native_sim) do NOT run on macOS — use CI or the devcontainer."
echo "  - iOS app toolchain is separate: app/scripts/macos-setup.sh"
