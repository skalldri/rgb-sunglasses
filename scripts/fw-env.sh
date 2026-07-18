#!/usr/bin/env bash
# fw-env.sh — source this before firmware `west build` commands so they work on
# every supported host. Never execute it; it only mutates the current shell.
#
#   . scripts/fw-env.sh
#
# - Devcontainer: west is already on PATH — this is a no-op.
# - macOS host:   activates the NCS venv (west + Zephyr python deps) and sets
#                 ZEPHYR_BASE via the env file scripts/macos-setup.sh generated,
#                 so `west build` can run from the repo root (outside the ~/ncs
#                 west workspace), same as the devcontainer.
#
# Fails (return 1) with instructions if no firmware toolchain is available.

if command -v west >/dev/null 2>&1; then
    return 0 2>/dev/null || exit 0
fi

_FW_ENV_FILE="$HOME/ncs/env-v3.1.1.sh"
if [ "$(uname -s)" = "Darwin" ] && [ -f "$_FW_ENV_FILE" ]; then
    . "$_FW_ENV_FILE"
    unset _FW_ENV_FILE
    return 0 2>/dev/null || exit 0
fi

echo "error: no firmware toolchain found (west is not on PATH and $_FW_ENV_FILE does not exist)." >&2
echo "       On macOS, run scripts/macos-setup.sh once to install NCS v3.1.1." >&2
unset _FW_ENV_FILE
return 1 2>/dev/null || exit 1
