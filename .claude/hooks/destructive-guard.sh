#!/usr/bin/env bash
# PreToolUse hook: denies destructive Bash commands that CLAUDE.md forbids but
# that nothing previously enforced mechanically:
#
#   1. pkill/killall        -- kill processes container-wide, incl. the
#                              devcontainer init / MCP server / VS Code server
#                              (root CLAUDE.md "Process management").
#   2. mkfs / mkfs.<type>   -- host-formatting the board NAND is owned by the
#                              firmware `fatfs reformat` shell command
#                              (fw/CLAUDE.md); genuinely new disks are the
#                              user's call.
#   3. reset-project(.js)   -- app/scripts/reset-project.js is Expo template
#                              scaffolding reset; it deletes the app source
#                              tree.
#   4. git commit on main   -- root CLAUDE.md "Git workflow": always branch
#                              first. Denied ONLY when the branch resolved via
#                              `git -C "$cwd" rev-parse --abbrev-ref HEAD` is
#                              exactly "main"; detached HEAD, other branches,
#                              and any git error (non-repo shell) are allowed
#                              (fail-open).
#
# Everything hardware-lock-related stays in hw-lock-guard.sh -- this hook
# deliberately duplicates none of its patterns. Like hw-lock-guard.sh, it
# anchors on the `cwd` field from the hook's own stdin JSON payload (not
# ${CLAUDE_PROJECT_DIR}) so per-worktree sessions resolve their own branch.
# JSON in/out contract mirrors hw-lock-guard.sh: hookSpecificOutput with
# permissionDecision allow/deny (+ permissionDecisionReason on deny); the
# default outcome is allow, and any parse error allows.
#
# Known limitations (accepted by design -- verified 2026-07, re-verify before
# "fixing" any of these; the guard errs toward interrupting a read-only
# command rather than missing a destructive one):
#
#   * pkill/killall/mkfs are matched at INVOCATION position only: start of the
#     command or of any line (heredoc bodies included), or after ;, &, |, (,
#     $(, backtick, sudo/exec/env/xargs/time. Prose mentions mid-line (a
#     commit message that *talks about* pkill, `grep mkfs ...`) are allowed;
#     a line of a multi-line command that *starts with* pkill is still denied
#     even if it is only heredoc text (accepted cost -- prose lines rarely
#     lead with a bare command name). reset-project is denied only when
#     executed via an interpreter/runner (node/bash/sh/npm/npx/yarn); `ls`
#     or `grep` of that path is allowed.
#
#   * Case-sensitive on purpose: `PKILL node` or `MKFS.VFAT` pass. Real
#     Linux binaries are lowercase, so accidental destructive calls are
#     still caught; the threat model is accident, not evasion, and adding
#     re.IGNORECASE would only widen the false-positive surface above.
#
#   * git-commit-on-main resolves the branch from the payload `cwd`, not
#     from any `cd` inside the command -- `cd /other/repo && git commit` is
#     judged against the session cwd's branch. Compound commands that change
#     directory before committing bypass (or falsely trip) this rule.
set -euo pipefail

PAYLOAD="$(cat)"

python3 -c '
import json, re, subprocess, sys

ALLOW = {"hookSpecificOutput": {"hookEventName": "PreToolUse",
                                "permissionDecision": "allow"}}

def emit(obj):
    print(json.dumps(obj))
    sys.exit(0)

def deny(reason):
    emit({"hookSpecificOutput": {"hookEventName": "PreToolUse",
                                 "permissionDecision": "deny",
                                 "permissionDecisionReason": reason}})

try:
    payload = json.loads(sys.stdin.read())
except Exception:
    emit(ALLOW)  # unparsable payload: fail open

if (payload.get("tool_name") or "") != "Bash":
    emit(ALLOW)

tool_input = payload.get("tool_input") or {}
command = tool_input.get("command") or ""
cwd = payload.get("cwd") or "."

# Invocation position: start of command/line (heredoc bodies are lines of the
# same command string), or right after a shell operator or a common wrapper.
# Prose that merely MENTIONS a tool mid-line does not match.
INVOKE = (r"(?:^|[;&|(]\s*|\$\(\s*|`\s*"
          r"|\bsudo\s+|\bexec\s+|\btime\s+"
          r"|\benv\s+(?:\S+=\S+\s+)*"
          r"|\bxargs\s+(?:-\S+\s+)*)")

if re.search(INVOKE + r"(pkill|killall)\b", command, re.MULTILINE):
    deny("Refusing: pkill/killall kill processes container-wide (devcontainer "
         "init, MCP server, VS Code server) and crash the session -- use "
         "pgrep to find the PID, then a targeted kill <pid> (root CLAUDE.md, "
         "Process management).")

if re.search(INVOKE + r"mkfs(\.[a-z0-9]+)?\b", command, re.MULTILINE):
    deny("Refusing: never host-format the board NAND -- the firmware shell "
         "command `fatfs reformat` owns that partition (fw/CLAUDE.md); for a "
         "genuinely new/unformatted disk, ask the user to run mkfs "
         "themselves.")

if re.search(r"\b(node|bash|sh|zsh|npm|npx|yarn)\b[^;&|\n]*\breset-project(\.js)?\b",
             command):
    deny("Refusing: app/scripts/reset-project.js is the Expo template "
         "scaffolding reset and deletes the app source tree -- if a "
         "scaffolding reset is really intended, ask the user first.")

if re.search(r"\bgit\b.*\bcommit\b", command):
    branch = None
    try:
        r = subprocess.run(
            ["git", "-C", cwd, "rev-parse", "--abbrev-ref", "HEAD"],
            capture_output=True, text=True, timeout=5)
        if r.returncode == 0:
            branch = r.stdout.strip()
    except Exception:
        branch = None  # git missing/hung: fail open
    if branch == "main":
        deny("Refusing: never commit directly to main -- create a feature "
             "branch first with git checkout -b <branch>, then commit (root "
             "CLAUDE.md, Git workflow).")

emit(ALLOW)
' <<<"$PAYLOAD"
