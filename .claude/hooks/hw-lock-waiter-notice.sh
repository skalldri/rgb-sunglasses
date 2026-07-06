#!/usr/bin/env bash
# UserPromptSubmit hook: if this session currently holds the 'board' and/or
# 'app' hw-lock and another agent is queued (via --wait) waiting on it, remind
# the holder every turn -- not just the next time it happens to touch a
# hardware tool -- so a held-but-idle lock doesn't block someone else
# indefinitely with no visibility into the fact that anyone is waiting.
#
# Always advisory-only: never blocks the user's prompt (never exits 2).
set -euo pipefail

PAYLOAD="$(cat)"
CWD="$(python3 -c 'import json,sys; print(json.loads(sys.stdin.read()).get("cwd") or ".")' <<<"$PAYLOAD" 2>/dev/null || echo .)"

HW_LOCK="$CWD/scripts/hw-lock.sh"
[ -x "$HW_LOCK" ] || exit 0

MESSAGES=()
for resource in board app; do
    if "$HW_LOCK" check "$resource" >/dev/null 2>&1; then
        waiters="$("$HW_LOCK" waiters "$resource" 2>/dev/null || echo 0)"
        case "$waiters" in
            ''|*[!0-9]*) continue ;;
        esac
        if [ "$waiters" -gt 0 ]; then
            MESSAGES+=("You are holding the '$resource' hardware lock and $waiters other agent(s) are queued waiting for it -- please wrap up and run \`scripts/hw-lock.sh release $resource\` (or \`release --all\`) as soon as you reasonably can so they aren't blocked.")
        fi
    fi
done

[ ${#MESSAGES[@]} -gt 0 ] || exit 0

python3 -c '
import json, sys
msg = " ".join(sys.argv[1:])
print(json.dumps({"hookSpecificOutput": {"hookEventName": "UserPromptSubmit", "additionalContext": msg}}))
' "${MESSAGES[@]}"
exit 0
