#!/usr/bin/env bash
# tools-env.sh — source this before running the fw/tools/ GLIM converters so they
# work on every supported host. Never execute it; it only mutates the current shell.
#
#   . scripts/tools-env.sh
#
# - Devcontainer: Pillow/numpy/lz4 + yt-dlp come from devcontainer.json's pip line
#                 and ffmpeg from the Dockerfile — this is a no-op.
# - macOS host:   activates the GLIM tools venv scripts/macos-setup.sh created,
#                 putting Pillow/numpy/lz4 on sys.path and yt-dlp (plus the deno
#                 JS runtime it needs for YouTube) on PATH.
#
# Fails (return 1) with instructions if no converter tooling is available.
#
# NOTE: this and scripts/fw-env.sh activate DIFFERENT python venvs, so in a shell
# that sources both, the last one wins for `python3` — and a `west build` run
# after this one would configure Zephyr against the tools venv and fail on the
# missing Zephyr python deps. They are not meant to be combined: build firmware in
# one shell, generate GLIM assets in another.

# Probe what the converters actually need rather than the platform, so this is a
# true no-op wherever the deps are already global (the devcontainer) and is
# self-idempotent — sourcing it twice can't stack venv activations.
if python3 -c 'import PIL, numpy, lz4.block' >/dev/null 2>&1 \
   && command -v yt-dlp >/dev/null 2>&1 \
   && command -v ffmpeg >/dev/null 2>&1; then
    return 0 2>/dev/null || exit 0
fi

# tools-venv-current is a version-stable symlink maintained by macos-setup.sh (the
# single place the python version is pinned) to the venv it created.
_TOOLS_VENV="$HOME/.cache/rgb-sunglasses/tools-venv-current"
if [ "$(uname -s)" = "Darwin" ] && [ -f "$_TOOLS_VENV/bin/activate" ]; then
    . "$_TOOLS_VENV/bin/activate"
    unset _TOOLS_VENV
    return 0 2>/dev/null || exit 0
fi

echo "error: no GLIM converter tooling found (Pillow/numpy/lz4/yt-dlp/ffmpeg are" >&2
echo "       not all available, and $_TOOLS_VENV does not exist)." >&2
echo "       On macOS, run scripts/macos-setup.sh once to install them." >&2
echo "       In the devcontainer, re-run the 'python' postCreateCommand from" >&2
echo "       .devcontainer/devcontainer.json (ffmpeg needs an image rebuild)." >&2
unset _TOOLS_VENV
return 1 2>/dev/null || exit 1
