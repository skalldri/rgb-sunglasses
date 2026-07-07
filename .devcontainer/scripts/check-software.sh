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

echo ""
echo "======================================"
exit 0
