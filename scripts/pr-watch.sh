#!/usr/bin/env bash
#
# pr-watch.sh — emit one line per newly-opened GitHub PR, and per already-seen
# PR whose HEAD COMMIT changes (i.e. someone pushed).
#
# Designed to be driven by Claude Code's `Monitor` tool: each stdout line
# becomes one notification. See `.claude/skills/pr-review-watch/SKILL.md` for
# the full workflow (arming it, and fanning reviews out to parallel agents).
#
#   Monitor(command: "scripts/pr-watch.sh", description: "new PRs + pushes",
#           persistent: true)
#
# Two design decisions that are load-bearing — do not "simplify" either away:
#
#   1. The trigger is `headRefOid`, NOT `updatedAt`. `updatedAt` bumps on every
#      comment, including the review comments an automated reviewer posts in
#      response to this very monitor — so an updatedAt-keyed watcher reviews a
#      PR, bumps its own timestamp, and loops forever. A head-SHA change only
#      happens when someone pushes code.
#
#   2. Pushes are debounced by one poll cycle: a changed SHA must survive one
#      full interval before it fires. Authors push fixes in bursts (and rebase,
#      then fixup), and a high-effort review costs real time and tokens; this
#      collapses a burst into one review of the settled state.
#
# Usage:
#   scripts/pr-watch.sh [--repo OWNER/NAME] [--poll SECONDS] [--state-dir DIR]
#   scripts/pr-watch.sh --self-test      # offline check of the state machine
#
# Events emitted on stdout:
#   PR-WATCH: armed on <repo> — ...
#   NEW PR #<n> by <author> [<branch>]<draft>: <title>
#   UPDATED PR #<n> by <author> [<branch>]: <title> (head <old> -> <new>)
#   PR-WATCH WARNING: 5 consecutive 'gh pr list' failures — ...
#   PR-WATCH ERROR: ... (monitor exits)
#
# Silence is never success here: a broken `gh` surfaces as a WARNING event
# rather than a monitor that has quietly stopped noticing PRs.
set -uo pipefail

POLL="${PR_WATCH_POLL:-60}"
STATE_DIR="${PR_WATCH_STATE_DIR:-}"
REPO=""
SELF_TEST=0

while [ $# -gt 0 ]; do
  case "$1" in
    --repo)       REPO="${2:?--repo needs OWNER/NAME}"; shift 2 ;;
    --poll)       POLL="${2:?--poll needs SECONDS}"; shift 2 ;;
    --state-dir)  STATE_DIR="${2:?--state-dir needs DIR}"; shift 2 ;;
    --self-test)  SELF_TEST=1; shift ;;
    -h|--help)    sed -n '2,40p' "$0"; exit 0 ;;
    *) echo "PR-WATCH ERROR: unknown argument '$1'" >&2; exit 2 ;;
  esac
done

# ---------------------------------------------------------------- state files
# "<pr number> <sha>" per line. STATE = last SHA reported for that PR.
# PEND = a changed SHA seen once, waiting one cycle to settle.

lookup() { # lookup <num> <file>
  grep -m1 "^$1 " "$2" 2>/dev/null | awk '{print $2}'
}
upsert() { # upsert <file> <num> <sha>
  { grep -v "^$2 " "$1" 2>/dev/null || true; echo "$2 $3"; } > "$1.tmp" && mv "$1.tmp" "$1"
}
drop() { # drop <num> <file>
  { grep -v "^$1 " "$2" 2>/dev/null || true; } > "$2.tmp" && mv "$2.tmp" "$2"
}

# Decide what a single observation of (num, sha) means, and update state.
# Echoes the event kind: NEW | UPDATED <oldsha> | (nothing, if silent).
classify() { # classify <num> <sha>
  local num="$1" sha="$2" known
  known=$(lookup "$num" "$STATE")
  if [ -z "$known" ]; then
    upsert "$STATE" "$num" "$sha"
    echo "NEW"
  elif [ "$known" != "$sha" ]; then
    if [ "$(lookup "$num" "$PEND")" = "$sha" ]; then
      upsert "$STATE" "$num" "$sha"
      drop "$num" "$PEND"
      echo "UPDATED $known"
    else
      upsert "$PEND" "$num" "$sha"   # settle one cycle before firing
    fi
  else
    drop "$num" "$PEND"              # pushed then reverted within a cycle
  fi
}

# ------------------------------------------------------------------ self-test
# Runs the state machine offline against the cases that have actually bitten:
# a burst of pushes inside one cycle, and a push-then-revert.
if [ "$SELF_TEST" = 1 ]; then
  STATE=$(mktemp); PEND=$(mktemp)
  trap 'rm -f "$STATE" "$PEND" "$STATE.tmp" "$PEND.tmp"' EXIT
  printf '10 aaaa\n' > "$STATE"; : > "$PEND"
  fail=0
  check() { # check <label> <expected> <actual>
    if [ "$2" = "$3" ]; then echo "  ok   $1"; else echo "  FAIL $1: expected '$2', got '$3'"; fail=1; fi
  }
  check "unchanged is silent"            ""             "$(classify 10 aaaa)"
  check "push does not fire immediately" ""             "$(classify 10 bbbb)"
  check "push fires once settled"        "UPDATED aaaa" "$(classify 10 bbbb)"
  check "settled push is then silent"    ""             "$(classify 10 bbbb)"
  check "burst: first sha silent"        ""             "$(classify 10 cccc)"
  check "burst: second sha silent"       ""             "$(classify 10 dddd)"
  check "burst: fires once, final sha"   "UPDATED bbbb" "$(classify 10 dddd)"
  check "push then revert is silent"     ""             "$(classify 10 eeee)"
  check "revert leaves no pending fire"  ""             "$(classify 10 dddd)"
  check "unseen PR is NEW"               "NEW"          "$(classify 11 ffff)"
  check "new PR then silent"             ""             "$(classify 11 ffff)"
  check "pending file drained"           ""             "$(cat "$PEND")"
  [ "$fail" = 0 ] && echo "self-test: PASS" || echo "self-test: FAIL"
  exit "$fail"
fi

# ----------------------------------------------------------------------- main
if [ -z "$REPO" ]; then
  REPO=$(gh repo view --json nameWithOwner -q .nameWithOwner 2>/dev/null)
fi
if [ -z "${REPO:-}" ]; then
  echo "PR-WATCH ERROR: cannot resolve repo (gh auth? wrong cwd?) — monitor exiting"
  exit 1
fi

if [ -z "$STATE_DIR" ]; then
  STATE_DIR="${TMPDIR:-/tmp}/pr-watch-$(echo "$REPO" | tr '/' '-')"
fi
mkdir -p "$STATE_DIR" || { echo "PR-WATCH ERROR: cannot create $STATE_DIR"; exit 1; }
STATE="$STATE_DIR/state"
PEND="$STATE_DIR/pending"

QUERY='.[] | "\(.number)\t\(.headRefOid)\t\(.author.login)\t\(.headRefName)\t\(.isDraft)\t\(.title)"'

# Baseline: every currently-open PR at its current head, so nothing already
# open fires until it is pushed to again. Re-arming re-baselines — a push that
# landed while the monitor was down is absorbed, not reported.
: > "$PEND"
if ! gh pr list --repo "$REPO" --state open --limit 100 --json number,headRefOid \
      -q '.[] | "\(.number) \(.headRefOid)"' > "$STATE" 2>/dev/null; then
  echo "PR-WATCH ERROR: baseline query failed (gh auth?) — monitor exiting"
  exit 1
fi
echo "PR-WATCH: armed on $REPO — new PRs + pushes to $(wc -l < "$STATE" | tr -d ' ') open PR(s), ${POLL}s poll, 1-cycle push debounce"

fails=0
while true; do
  if cur=$(gh pr list --repo "$REPO" --state open --limit 100 \
             --json number,headRefOid,author,headRefName,isDraft,title -q "$QUERY" 2>/dev/null); then
    [ "$fails" -ge 5 ] && echo "PR-WATCH: recovered, polling again"
    fails=0
    while IFS=$'\t' read -r num sha author branch draft title; do
      [ -z "$num" ] && continue
      verdict=$(classify "$num" "$sha")
      case "$verdict" in
        NEW)
          [ "$draft" = "true" ] && d=" (draft)" || d=""
          echo "NEW PR #$num by $author [$branch]$d: $title" ;;
        UPDATED*)
          old="${verdict#UPDATED }"
          echo "UPDATED PR #$num by $author [$branch]: $title (head ${old:0:8} -> ${sha:0:8})" ;;
      esac
    done <<< "$cur"
  else
    fails=$((fails + 1))
    [ "$fails" -eq 5 ] && echo "PR-WATCH WARNING: 5 consecutive 'gh pr list' failures — PR polling may be broken"
  fi
  sleep "$POLL"
done
