#!/usr/bin/env bash
# Checks software authentication and tooling readiness.
# Outputs a plain-text status report to stdout. Exit code is always 0.

echo "=== RGB Sunglasses Software Check ==="
echo ""

# ── GitHub CLI ─────────────────────────────────────────────────────────────
if ! command -v gh >/dev/null 2>&1; then
    echo "GitHub CLI (gh): NOT INSTALLED"
    echo "  [!] Rebuild the devcontainer to install gh."
else
    GH_STATUS=$(gh auth status 2>&1)
    if echo "$GH_STATUS" | grep -q "Logged in"; then
        # gh's wording changed across versions: older releases print
        # "Logged in to github.com as <user>", newer ones print
        # "Logged in to github.com account <user>". Accept both.
        GH_USER=$(echo "$GH_STATUS" | grep -oE '(as|account) [^ ]+' | head -1 | awk '{print $2}')
        echo "GitHub CLI (gh): AUTHENTICATED as $GH_USER"
    else
        echo "GitHub CLI (gh): NOT AUTHENTICATED"
        echo "  [!] Run:  gh auth login"
        echo "  [!] Then select: GitHub.com → HTTPS → Login with a web browser"
    fi
fi

# ── macOS host firmware tooling ────────────────────────────────────────────
# On the Mac these are installed by scripts/macos-setup.sh (the devcontainer
# ships them in its image). Surfacing them here means a half-bootstrapped Mac
# is visible in the session-start table instead of failing mid-task.
if [ "$(uname -s)" = "Darwin" ]; then
    echo ""

    if [ -x /opt/homebrew/bin/bash ] || [ -x /usr/local/bin/bash ]; then
        echo "Homebrew bash (hw-lock): OK"
    else
        echo "Homebrew bash (hw-lock): NOT INSTALLED  [hardware locks unusable — run scripts/macos-setup.sh]"
    fi

    if command -v mcumgr >/dev/null 2>&1; then
        echo "mcumgr CLI (OTA flash): OK"
    else
        echo "mcumgr CLI (OTA flash): NOT INSTALLED  [run scripts/macos-setup.sh]"
    fi

    if command -v serial_mcp >/dev/null 2>&1; then
        echo "serial_mcp (MCP server): OK"
    else
        echo "serial_mcp (MCP server): NOT INSTALLED  [run scripts/macos-setup.sh]"
    fi

    NCS_ENV_FILE="$HOME/ncs/env-v3.1.1.sh"
    if [ -f "$NCS_ENV_FILE" ] && [ -d "$HOME/ncs/v3.1.1/zephyr" ]; then
        echo "NCS v3.1.1 toolchain: OK  [. scripts/fw-env.sh to use]"
    else
        echo "NCS v3.1.1 toolchain: NOT READY  [no firmware builds — run scripts/macos-setup.sh]"
    fi

    echo "Twister tests: not supported on macOS (native_sim is Linux-only) — use CI or the devcontainer"
fi

echo ""
echo "======================================"
exit 0
