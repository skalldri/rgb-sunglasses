---
description: Check software authentication and tooling readiness (gh, etc.)
allowed-tools: Bash(.devcontainer/scripts/check-software.sh)
---

Run `.devcontainer/scripts/check-software.sh` and show the output verbatim.

If GitHub CLI shows NOT AUTHENTICATED, prompt the user to run `gh auth login` in the terminal.
