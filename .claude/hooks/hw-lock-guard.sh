#!/usr/bin/env bash
# PreToolUse hook: denies hardware-touching tool calls (Bash commands that
# invoke known board/app tooling, plus every mcp__serial__*/mcp__execbro__*
# call) unless this session holds the corresponding scripts/hw-lock.sh lock.
#
# Deliberately anchors on the `cwd` field from the hook's own stdin JSON
# payload rather than ${CLAUDE_PROJECT_DIR} or this script's own location --
# this repo has already hit two worktree bugs this session caused by trusting
# a path other than "wherever this session actually is" (see root CLAUDE.md's
# "Hardware locking" section), so the lock check below always resolves
# scripts/hw-lock.sh relative to the calling session's own cwd.
set -euo pipefail

PAYLOAD="$(cat)"

PARSED="$(python3 -c '
import json, re, sys

payload = json.loads(sys.stdin.read())
cwd = payload.get("cwd") or "."
tool_name = payload.get("tool_name") or ""
tool_input = payload.get("tool_input") or {}

SERIAL_ALLOWLIST = {
    "serial_list_ports", "serial_connections_list", "serial_connection_status",
    "serial_spec_list", "serial_spec_get", "serial_spec_search",
    "serial_plugin_list", "serial_trace_status", "serial_close",
}
EXECBRO_ALLOWLIST = {
    "get_usage_guide", "get_apps", "list_android_devices", "list_devices",
    "list_ios_simulators", "get_license_status", "activate_license",
    "send_feedback",
}
# Commands that touch BOTH the board (serial) and the phone (adb) — require both
# locks. re-pair.sh drives the shell UART and the phone in one run.
BASH_BOTH_PATTERNS = [r"re-pair\.(sh|py)"]
BASH_BOARD_PATTERNS = [
    r"jlink-flash\.sh", r"mcumgr-flash\.sh", r"provision-device\.sh", r"JLinkExe",
    r"nrfutil\s+device", r"\bmcumgr\b", r"west\s+flash",
]
BASH_APP_PATTERNS = [r"\badb\b", r"expo run:android", r"adb-connect\.sh"]

resource = ""

if tool_name.startswith("mcp__serial__"):
    short = tool_name[len("mcp__serial__"):]
    if short not in SERIAL_ALLOWLIST:
        resource = "board"
elif tool_name.startswith("mcp__execbro__"):
    short = tool_name[len("mcp__execbro__"):]
    if short not in EXECBRO_ALLOWLIST:
        resource = "app"
elif tool_name == "Bash":
    command = tool_input.get("command") or ""
    if any(re.search(p, command) for p in BASH_BOTH_PATTERNS):
        resource = "board,app"
    elif any(re.search(p, command) for p in BASH_BOARD_PATTERNS):
        resource = "board"
    elif any(re.search(p, command) for p in BASH_APP_PATTERNS):
        resource = "app"

# resource may be a comma-separated list; every named lock must be held.
print(cwd)
print(resource)
' <<<"$PAYLOAD")"

CWD="$(sed -n '1p' <<<"$PARSED")"
RESOURCE="$(sed -n '2p' <<<"$PARSED")"

allow() {
    echo '{"hookSpecificOutput":{"hookEventName":"PreToolUse","permissionDecision":"allow"}}'
    exit 0
}

# Same as allow(), but for the "you already hold this lock" path: if another
# session is queued waiting on $1, surface that via additionalContext so the
# holder sees it right on the tool call it's about to make, not just on its
# next UserPromptSubmit turn (see hw-lock-waiter-notice.sh for that side).
allow_with_waiter_notice() {
    local resource="$1" waiters
    waiters="$("$HW_LOCK" waiters "$resource" 2>/dev/null || echo 0)"
    case "$waiters" in
        ''|*[!0-9]*|0) allow ;;
    esac
    python3 -c '
import json, sys
resource, waiters = sys.argv[1], sys.argv[2]
msg = (
    f"Heads up: {waiters} other agent(s) are queued waiting for the \x27{resource}\x27 "
    f"hardware lock you are holding. Wrap up and run `scripts/hw-lock.sh release {resource}` "
    f"as soon as you reasonably can so they are not blocked."
)
print(json.dumps({"hookSpecificOutput": {"hookEventName": "PreToolUse", "permissionDecision": "allow", "additionalContext": msg}}))
' "$resource" "$waiters"
    exit 0
}

deny() {
    python3 -c '
import json, sys
print(json.dumps({"hookSpecificOutput": {"hookEventName": "PreToolUse", "permissionDecision": "deny", "permissionDecisionReason": sys.argv[1]}}))
' "$1"
    exit 0
}

[ -n "$RESOURCE" ] || allow

HW_LOCK="$CWD/scripts/hw-lock.sh"
if [ ! -x "$HW_LOCK" ]; then
    echo "[hw-lock-guard] scripts/hw-lock.sh not found at $CWD -- allowing without a lock check." >&2
    allow
fi

# $RESOURCE may be a comma-separated set (e.g. "board,app"); every named lock
# must be held, else deny naming the first missing one.
IFS=',' read -ra RESOURCES <<<"$RESOURCE"
HOLD_ARGS="${RESOURCES[*]}"   # space-joined for the `hold` hint (default IFS)
for r in "${RESOURCES[@]}"; do
    if ! "$HW_LOCK" check "$r" >/dev/null 2>&1; then
        STATUS="$("$HW_LOCK" status "$r" 2>&1 || true)"
        deny "Refusing: the '$r' hardware lock is not held by this session. Run Monitor(command: \"scripts/hw-lock.sh hold $HOLD_ARGS\", persistent: true) first (see the hw-lock skill). Current status: $STATUS"
    fi
done

# All required locks held. Preserve the single-resource waiter-notice behavior;
# for a multi-resource set just allow (the notice covers one resource at a time).
if [ "${#RESOURCES[@]}" -eq 1 ]; then
    allow_with_waiter_notice "${RESOURCES[0]}"
fi
allow
