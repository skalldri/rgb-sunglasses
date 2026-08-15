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
#      happens when the branch moves.
#
#      Known limits of that choice, both accepted: a base-branch retarget or an
#      "Update branch"/merge-queue sync moves (or fails to move) the head
#      without matching author intent, so this can review a merge commit nobody
#      wrote, and will miss a retarget that changes the whole diff. Neither is
#      worth reintroducing the self-trigger loop to catch.
#
#   2. Pushes are debounced by one poll cycle: a changed SHA must survive one
#      full interval before it fires. Authors push fixes in bursts (and rebase,
#      then fixup), and a high-effort review costs real time and tokens; this
#      collapses a burst into one review of the settled state. Newly-seen PRs
#      settle the same way, so open-then-immediately-fixup is one review too.
#
# Usage:
#   scripts/pr-watch.sh [--repo OWNER/NAME] [--poll SECONDS] [--state-dir DIR]
#                       [--limit N] [--skip-drafts]
#   scripts/pr-watch.sh --self-test      # offline check of the state machine
set -uo pipefail

POLL="${PR_WATCH_POLL:-60}"
STATE_DIR="${PR_WATCH_STATE_DIR:-}"
LIMIT="${PR_WATCH_LIMIT:-100}"
SKIP_DRAFTS=0
REPO=""
SELF_TEST=0

while [ $# -gt 0 ]; do
  case "$1" in
    --repo)        REPO="${2:?--repo needs OWNER/NAME}"; shift 2 ;;
    --poll)        POLL="${2:?--poll needs SECONDS}"; shift 2 ;;
    --state-dir)   STATE_DIR="${2:?--state-dir needs DIR}"; shift 2 ;;
    --limit)       LIMIT="${2:?--limit needs N}"; shift 2 ;;
    --skip-drafts) SKIP_DRAFTS=1; shift ;;
    --self-test)   SELF_TEST=1; shift ;;
    -h|--help)     sed -n '2,45p' "$0"; exit 0 ;;
    *) echo "PR-WATCH ERROR: unknown argument '$1'" >&2; exit 2 ;;
  esac
done

# A non-numeric or zero poll turns the loop into a `gh` busy-spin, which reads
# as "the watcher is working" right up until the rate limit.
case "$POLL" in
  ''|*[!0-9]*) echo "PR-WATCH ERROR: --poll must be a whole number of seconds, got '$POLL'"; exit 2 ;;
esac
[ "$POLL" -lt 1 ] && { echo "PR-WATCH ERROR: --poll must be >= 1, got '$POLL'"; exit 2; }
case "$LIMIT" in
  ''|*[!0-9]*) echo "PR-WATCH ERROR: --limit must be a whole number, got '$LIMIT'"; exit 2 ;;
esac
[ "$LIMIT" -lt 1 ] && { echo "PR-WATCH ERROR: --limit must be >= 1, got '$LIMIT'"; exit 2; }

# ---------------------------------------------------------------- state files
# STATE: "<num> <sha> <draft>" — last state reported for that PR.
# PEND:  "<num> <sha>"         — a change seen once, waiting a cycle to settle.
#                                A PEND entry with no STATE entry is a new PR.

field() { # field <num> <file> <n>
  grep -m1 "^$1 " "$2" 2>/dev/null | awk -v n="$3" '{print $n}'
}
lookup()       { field "$1" "$2" 2; }
lookup_draft() { field "$1" "$2" 3; }
upsert() { # upsert <file> <num> <rest...>
  local f="$1" num="$2"; shift 2
  { grep -v "^$num " "$f" 2>/dev/null || true; echo "$num $*"; } > "$f.tmp" && mv "$f.tmp" "$f"
}
drop() { # drop <num> <file>
  { grep -v "^$1 " "$2" 2>/dev/null || true; } > "$2.tmp" && mv "$2.tmp" "$2"
}

# Decide what one observation of (num, sha, draft) means, and update state.
# Echoes: NEW | UPDATED <oldsha> | READY | (nothing, if silent).
classify() { # classify <num> <sha> <draft>
  local num="$1" sha="$2" draft="$3" known known_draft
  known=$(lookup "$num" "$STATE")

  if [ -z "$known" ]; then
    # Unseen PR. Settle one cycle, so an open-then-fixup is a single review.
    if [ "$(lookup "$num" "$PEND")" != "$sha" ]; then
      upsert "$PEND" "$num" "$sha"
      return
    fi
    drop "$num" "$PEND"
    if [ "$SKIP_DRAFTS" = 1 ] && [ "$draft" = "true" ]; then
      upsert "$STATE" "$num" "$sha" "$draft"   # tracked, deliberately unreported
      return
    fi
    upsert "$STATE" "$num" "$sha" "$draft"
    echo "NEW"
    return
  fi

  known_draft=$(lookup_draft "$num" "$STATE")

  # A draft marked ready-for-review changes no SHA, so it is invisible to the
  # head-SHA trigger. Only meaningful when we skipped it on open.
  #
  # The stored draft flag is deliberately one-way: once a PR has been reported,
  # state records draft=false forever. Converting back to draft and out again
  # would otherwise re-fire READY at a byte-identical head on every toggle —
  # the same no-diff retrigger this script rejects `updatedAt` for.
  if [ "$SKIP_DRAFTS" = 1 ] && [ "$known_draft" = "true" ] && [ "$draft" != "true" ]; then
    # Settle like every other event. Under --skip-drafts, READY *is* the open
    # event, and mark-ready-then-fixup is as common as open-then-fixup — firing
    # immediately reviews a SHA the author is about to replace.
    if [ "$(lookup "$num" "$PEND")" != "$sha" ]; then
      upsert "$PEND" "$num" "$sha"
      return
    fi
    upsert "$STATE" "$num" "$sha" "false"
    drop "$num" "$PEND"
    echo "READY"
    return
  fi

  if [ "$known" != "$sha" ]; then
    # A push to a PR we are deliberately deferring is still deferred: track the
    # new head silently so READY later fires at the right SHA.
    if [ "$SKIP_DRAFTS" = 1 ] && [ "$draft" = "true" ] && [ "$known_draft" = "true" ]; then
      upsert "$STATE" "$num" "$sha" "true"
      drop "$num" "$PEND"
      return
    fi
    if [ "$(lookup "$num" "$PEND")" = "$sha" ]; then
      upsert "$STATE" "$num" "$sha" "$known_draft"
      drop "$num" "$PEND"
      echo "UPDATED $known"
    else
      upsert "$PEND" "$num" "$sha"
    fi
  else
    drop "$num" "$PEND"   # pushed then reverted inside one cycle
  fi
}

# An exit-0 response listing zero PRs is ambiguous: either the repo really has
# none open, or the API hiccuped. Acting on the second reading wipes state, and
# then EVERY open PR re-fires as NEW — a full review storm from one blip. A
# genuinely empty repo stays empty, so require the reading to repeat.
#
# This is a function, not an inline condition, because the self-test MUST drive
# the same code the loop runs. An earlier revision re-implemented the rule in
# the test; mutation testing showed 5 of 5 mutants — including the one that
# restores the storm — passing the whole suite.
EMPTY_POLLS=0
EMPTY_STREAK_LIMIT=3
should_trust_empty() { # should_trust_empty <count>  -> 0 = act on it
  if [ "$1" = 0 ]; then
    EMPTY_POLLS=$((EMPTY_POLLS + 1))
  else
    EMPTY_POLLS=0
  fi
  [ "$1" -gt 0 ] || [ "$EMPTY_POLLS" -ge "$EMPTY_STREAK_LIMIT" ]
}

# One poll's worth of work: classify every record, emit the events, then decide
# about pruning. Takes the tab-separated `gh pr list` output as a string so the
# self-test can drive it with no network.
#
# The loop body lives here, not inline, for the same reason should_trust_empty()
# does — and for a reason found the hard way. Extracting only the PREDICATE left
# the DECISION untested: mutating the call site (`if should_trust_empty` ->
# `if ! should_trust_empty`, or prune's argument -> "") passed all 49 assertions
# while still emitting the full review storm. 12 of 13 call-site mutants
# survived. Testing a rule is not testing the code that consults it.
#
# Call it directly, never through $( ): it updates EMPTY_POLLS, and a command
# substitution would run it in a subshell where that counter never carries.
# Redirect its stdout to a file if you need to inspect the events.
poll_once() { # poll_once <tab-separated gh records>
  local cur="$1" open_nums="" num sha draft author branch title verdict old d count
  while IFS=$'\t' read -r num sha draft author branch title; do
    # A newline inside a title splits one record in two. Skip the orphan
    # rather than writing a key-less line into state that nothing can remove.
    case "${num:-}" in ''|*[!0-9]*) continue ;; esac
    open_nums="$open_nums $num"
    verdict=$(classify "$num" "$sha" "$draft")
    case "$verdict" in
      NEW)
        [ "$draft" = "true" ] && d=" (draft)" || d=""
        echo "NEW PR #$num by $author [$branch]$d: $title" ;;
      READY)
        echo "READY PR #$num by $author [$branch]: $title (draft marked ready)" ;;
      UPDATED*)
        old="${verdict#UPDATED }"
        echo "UPDATED PR #$num by $author [$branch]: $title (head ${old:0:8} -> ${sha:0:8})" ;;
    esac
  done <<< "$cur"

  count=$(printf '%s\n' $open_nums | grep -c . || true)

  if should_trust_empty "$count"; then
    prune "${open_nums# }"
  elif [ "$EMPTY_POLLS" = 1 ] && [ -s "$STATE" ]; then
    echo "PR-WATCH: poll returned zero open PRs but state is non-empty — not pruning until it repeats"
  fi

  # A silent cap reads as "covered everything" when it isn't. Re-arm the notice
  # when the count drops back under, so a repo that crosses the boundary
  # repeatedly is not warned about exactly once, forever.
  if [ "$count" -ge "$LIMIT" ]; then
    if [ "$truncated_warned" = 0 ]; then
      truncated_warned=1
      echo "PR-WATCH WARNING: $count open PRs reached the --limit $LIMIT window — PRs outside it are not watched"
    fi
  else
    truncated_warned=0
  fi
}

# Forget PRs that are no longer open. Without this, a closed-then-reopened PR
# whose head never moved matches its own stale entry and never fires again —
# and state grows without bound.
prune() { # prune <space-separated list of currently-open numbers>
  local seen=" $1 " f
  for f in "$STATE" "$PEND"; do
    while read -r num rest; do
      [ -z "${num:-}" ] && continue
      case "$seen" in *" $num "*) echo "$num $rest" ;; esac
    done < "$f" > "$f.tmp"
    mv "$f.tmp" "$f"
  done
}

# ------------------------------------------------------------------ self-test
if [ "$SELF_TEST" = 1 ]; then
  STATE=$(mktemp); PEND=$(mktemp)
  trap 'rm -f "$STATE" "$PEND" "$STATE.tmp" "$PEND.tmp"' EXIT
  fail=0
  check() { # check <label> <expected> <actual>
    if [ "$2" = "$3" ]; then echo "  ok   $1"; else echo "  FAIL $1: expected '$2', got '$3'"; fail=1; fi
  }

  printf '10 aaaa false\n' > "$STATE"; : > "$PEND"
  check "unchanged is silent"            ""             "$(classify 10 aaaa false)"
  check "push does not fire immediately" ""             "$(classify 10 bbbb false)"
  check "push fires once settled"        "UPDATED aaaa" "$(classify 10 bbbb false)"
  check "settled push is then silent"    ""             "$(classify 10 bbbb false)"
  check "burst: first sha silent"        ""             "$(classify 10 cccc false)"
  check "burst: second sha silent"       ""             "$(classify 10 dddd false)"
  check "burst: fires once, final sha"   "UPDATED bbbb" "$(classify 10 dddd false)"
  check "push then revert is silent"     ""             "$(classify 10 eeee false)"
  check "revert leaves no pending fire"  ""             "$(classify 10 dddd false)"

  check "unseen PR settles first"        ""             "$(classify 11 ffff false)"
  check "unseen PR fires once settled"   "NEW"          "$(classify 11 ffff false)"
  check "new PR then silent"             ""             "$(classify 11 ffff false)"
  check "open-then-fixup is one NEW"     ""             "$(classify 12 aa11 false)"
  check "  (fixup lands mid-settle)"     ""             "$(classify 12 bb22 false)"
  check "  (fires once, at final sha)"   "NEW"          "$(classify 12 bb22 false)"

  SKIP_DRAFTS=1
  check "draft settles"                  ""             "$(classify 20 dr01 true)"
  check "draft is not reported"          ""             "$(classify 20 dr01 true)"
  check "draft->ready settles first"     ""             "$(classify 20 dr01 false)"
  check "draft->ready fires READY"       "READY"        "$(classify 20 dr01 false)"
  check "ready then silent"              ""             "$(classify 20 dr01 false)"
  # A ready PR converted back to draft and out again moves no SHA. Re-firing
  # READY on each toggle is the no-diff retrigger we reject updatedAt for.
  check "ready->draft is silent"         ""             "$(classify 20 dr01 true)"
  check "draft->ready does NOT re-fire"  ""             "$(classify 20 dr01 false)"
  # A push to a still-draft PR stays deferred, but must track the new head so
  # READY later fires at the right SHA rather than a stale one.
  check "push while draft settles"       ""             "$(classify 21 dr10 true)"
  check "push while draft is deferred"   ""             "$(classify 21 dr10 true)"
  check "  (pushed again, still draft)"  ""             "$(classify 21 dr11 true)"
  check "  (still silent)"               ""             "$(classify 21 dr11 true)"
  check "  (un-drafted, settles)"        ""             "$(classify 21 dr11 false)"
  check "  (READY at the NEW head)"      "READY"        "$(classify 21 dr11 false)"
  check "  (state holds the new head)"   "dr11"         "$(lookup 21 "$STATE")"
  # mark-ready then immediately push: one READY, at the settled SHA.
  check "ready+fixup: settles"           ""             "$(classify 22 aa00 true)"
  check "  (deferred as draft)"          ""             "$(classify 22 aa00 true)"
  check "  (un-drafted, settling)"       ""             "$(classify 22 aa00 false)"
  check "  (fixup lands mid-settle)"     ""             "$(classify 22 bb00 false)"
  check "  (one READY, final sha)"       "READY"        "$(classify 22 bb00 false)"
  SKIP_DRAFTS=0

  # Closed-then-reopened at an unchanged head must still fire.
  printf '30 cafe false\n' > "$STATE"; : > "$PEND"
  prune ""
  check "prune drops closed PR"          ""             "$(cat "$STATE")"
  check "reopened settles"               ""             "$(classify 30 cafe false)"
  check "reopened fires NEW"             "NEW"          "$(classify 30 cafe false)"

  # A newline in a title used to split one record into two and write a
  # number-less line no helper could ever remove.
  printf '40 beef false\n' > "$STATE"; : > "$PEND"
  check "prune keeps open PR"            "40 beef false" "$(prune "40"; cat "$STATE")"

  # prune must survive the shapes a half-written state file can take.
  printf '' > "$STATE"; : > "$PEND"
  check "prune on empty state"           ""             "$(prune "1 2"; cat "$STATE")"
  printf '50 f00d false\n\n51 baad true\n' > "$STATE"
  check "prune skips blank lines"        "50 f00d false" "$(prune "50"; cat "$STATE")"

  # The empty-poll guard. These drive should_trust_empty() ITSELF — an earlier
  # revision re-implemented the rule here, and mutation testing showed every
  # mutant of the real rule (including -ge 3 -> -ge 1, which restores the
  # review storm) passing the whole suite.
  # NOT via $( ): should_trust_empty updates EMPTY_POLLS, and a command
  # substitution would run it in a subshell where the counter never carries
  # forward — the assertions would then pass against a rule that never counts.
  check_verdict() { # check_verdict <label> <expected> <count>
    local got
    if should_trust_empty "$3"; then got=trust; else got=hold; fi
    check "$1" "$2" "$got"
  }
  EMPTY_POLLS=0
  check_verdict "empty poll 1 is not trusted" "hold"  0
  check_verdict "empty poll 2 is not trusted" "hold"  0
  check_verdict "empty poll 3 is trusted"     "trust" 0
  check_verdict "non-empty is always trusted" "trust" 4
  check_verdict "  (and resets the streak)"   "hold"  0
  check_verdict "  (streak really restarted)" "hold"  0
  check_verdict "  (third again trusts)"      "trust" 0
  # And the state consequence the rule exists to protect.
  printf '60 aaaa false\n61 bbbb false\n' > "$STATE"; : > "$PEND"
  EMPTY_POLLS=0
  should_trust_empty 0 && prune ""
  check "1 empty poll does not wipe"     "60 aaaa false
61 bbbb false" "$(cat "$STATE")"
  should_trust_empty 0 && prune ""
  should_trust_empty 0 && prune ""
  check "3 empty polls do prune"         ""             "$(cat "$STATE")"

  # --- poll_once: the DECISION, not just the rule -------------------------
  # Every assertion below drives the real loop body. Mutating the call site
  # (inverting the should_trust_empty test, blanking prune's argument, forcing
  # count=0) used to survive the whole suite while still emitting the storm.
  LIMIT=100; truncated_warned=0; EMPTY_POLLS=0
  : > "$STATE"; : > "$PEND"
  rec() { printf '%s\t%s\tfalse\talice\tbr\ttitle\n' "$1" "$2"; }
  poll() { poll_once "$1" > "$STATE.events" 2>&1; }   # NOT $( ): EMPTY_POLLS must carry

  poll "$(rec 1 aaaa; rec 2 bbbb)"
  check "poll 1: two PRs settle silently"  ""    "$(cat "$STATE.events")"
  poll "$(rec 1 aaaa; rec 2 bbbb)"
  check "poll 2: both fire NEW"            "2"   "$(grep -c '^NEW PR' "$STATE.events")"
  poll "$(rec 1 aaaa; rec 2 bbbb)"
  check "poll 3: steady state is silent"   ""    "$(cat "$STATE.events")"

  # The storm case: an exit-0-but-empty response must not wipe state, and must
  # not re-announce anything on the following poll.
  poll ""
  check "empty poll: no NEW/UPDATED"       "0"   "$(grep -c '^NEW PR\|^UPDATED PR' "$STATE.events" || true)"
  check "empty poll: state survives"       "2"   "$(grep -c . "$STATE")"
  poll "$(rec 1 aaaa; rec 2 bbbb)"
  check "recovery: nothing re-fires"       ""    "$(cat "$STATE.events")"

  # Three consecutive empties are believed, and the state is then pruned.
  poll ""; poll ""; poll ""
  check "3 empty polls prune state"        "0"   "$(grep -c . "$STATE" || true)"

  # A push still reports through the real loop body.
  : > "$STATE"; : > "$PEND"; EMPTY_POLLS=0
  poll "$(rec 5 aaaa)"; poll "$(rec 5 aaaa)"
  poll "$(rec 5 cccc)"; poll "$(rec 5 cccc)"
  check "push emits one UPDATED"           "1"   "$(grep -c '^UPDATED PR #5' "$STATE.events")"
  rm -f "$STATE.events"

  # --- integration: run the REAL script, end to end -----------------------
  # Everything below the self-test's own `exit` — the arming baseline, the
  # lock, and the `while true` loop that calls poll_once — was unreachable
  # from here, so `poll_once "$cur"` -> `:` (a watcher that never polls)
  # passed the entire suite. These cases re-enter this same file as a child
  # process with a stub `gh` ahead of it on PATH: no network, no token.
  ITROOT=$(mktemp -d)
  trap 'rm -rf "$STATE" "$PEND" "$STATE.tmp" "$PEND.tmp" "$STATE.events" "$ITROOT"' EXIT
  mkdir -p "$ITROOT/bin"
  # The stub reads its records from a file rather than embedding them, so the
  # payload's tabs and quotes cannot break the generated script. (They did:
  # an earlier version emitted `; exit 0 ;;` on its own line — a bash syntax
  # error — so `gh` failed and every integration case reported 0.)
  export PR_WATCH_TEST_RECORDS="$ITROOT/records"
  cat > "$ITROOT/bin/gh" <<'STUB'
#!/usr/bin/env bash
case "$*" in
  *"repo view"*) echo "owner/repo" ;;
  *"pr list"*)   cat "$PR_WATCH_TEST_RECORDS" ;;
esac
exit 0
STUB
  chmod +x "$ITROOT/bin/gh"
  stub_gh() { printf '%b' "$1" > "$ITROOT/records"; }   # one record set, both queries
  run_watcher() { # run_watcher <state-dir> <seconds> ; echoes captured stdout
    local sd="$1" secs="$2" log="$ITROOT/log.$RANDOM"
    ( PATH="$ITROOT/bin:$PATH" bash "$0" --repo owner/repo --state-dir "$sd" --poll 1 > "$log" 2>&1 ) &
    local p=$!
    sleep "$secs"; kill "$p" 2>/dev/null; wait "$p" 2>/dev/null
    cat "$log"
  }

  stub_gh '1\taaaa\tfalse\talice\tbr\ttitle one\n'
  out=$(run_watcher "$ITROOT/s1" 4)
  check "integration: watcher arms"       "1"  "$(printf '%s' "$out" | grep -c 'armed on' || true)"
  # The decisive one: a PR that appears AFTER arming must be reported. This is
  # what `poll_once "$cur"` -> `:` breaks and what nothing else here catches —
  # a watcher that never polls is otherwise indistinguishable from a quiet one.
  # Baseline on PR 1 only, then introduce PR 2 while the loop is running.
  stub_gh '1\taaaa\tfalse\talice\tbr\tt\n'
  ( PATH="$ITROOT/bin:$PATH" bash "$0" --repo owner/repo --state-dir "$ITROOT/s2" --poll 1 \
      > "$ITROOT/s2.log" 2>&1 ) &
  wpid=$!
  sleep 2                                                    # let it arm
  stub_gh '1\taaaa\tfalse\talice\tbr\tt\n2\tbbbb\tfalse\tbob\tbr2\tt2\n'
  sleep 8                                                    # appear + settle + fire
  # 8s, not 4: the event needs two 1s poll cycles and was measured firing at
  # 1.05-2.06s. 4s left only ~2x headroom, the tightest margin in this suite
  # and the one a loaded CI runner could plausibly eat.
  kill "$wpid" 2>/dev/null; wait "$wpid" 2>/dev/null
  check "integration: loop emits NEW"     "1"  "$(grep -c 'NEW PR #2' "$ITROOT/s2.log" || true)"

  # Two watchers on one state dir: exactly one arms, the other exits.
  stub_gh '1\taaaa\tfalse\talice\tbr\tt\n'
  ( PATH="$ITROOT/bin:$PATH" bash "$0" --repo owner/repo --state-dir "$ITROOT/s3" --poll 1 > "$ITROOT/a.log" 2>&1 ) &
  ( PATH="$ITROOT/bin:$PATH" bash "$0" --repo owner/repo --state-dir "$ITROOT/s3" --poll 1 > "$ITROOT/b.log" 2>&1 ) &
  sleep 3
  check "integration: one watcher arms"   "1"  "$(cat "$ITROOT/a.log" "$ITROOT/b.log" | grep -c 'armed on' || true)"
  check "integration: the other exits"    "1"  "$(cat "$ITROOT/a.log" "$ITROOT/b.log" | grep -c 'already owns\|lost the race' || true)"
  for p in $(jobs -p); do kill "$p" 2>/dev/null; done
  wait 2>/dev/null

  # A watcher releases ITS OWN lock on the way out — and only its own. Without
  # this, inverting release_lock's ownership check (release what you do not
  # own, keep what you do) passes every other assertion here.
  # NOTE: launched without a `( ... ) &` wrapper so $! is unambiguously this
  # script. (Bash 5.1 forks-and-execs in place for a single-command subshell, so
  # the wrapper would usually work too — the earlier claim that it swallowed the
  # TERM was wrong. The real reason the inverted release_lock survived was the
  # `-e` test below, not the launch form. Kept explicit rather than relying on
  # an optimisation.)
  stub_gh '1\taaaa\tfalse\talice\tbr\tt\n'
  PATH="$ITROOT/bin:$PATH" bash "$0" --repo owner/repo --state-dir "$ITROOT/s5" --poll 1 \
      > "$ITROOT/s5.log" 2>&1 &
  wpid=$!
  sleep 2
  check "integration: lock is held"       "1"  "$([ -L "$ITROOT/s5/lock" ] && echo 1 || echo 0)"
  check "  (and names our watcher)"       "$wpid" "$(readlink "$ITROOT/s5/lock" 2>/dev/null)"
  kill -TERM "$wpid" 2>/dev/null; wait "$wpid" 2>/dev/null
  # -L, not -e: the lock is a symlink to a bare pid, so it is ALWAYS dangling
  # and -e (which follows the link) reports false whether or not it exists.
  # That made the first version of this assertion vacuous.
  check "integration: lock released"      "0"  "$([ -L "$ITROOT/s5/lock" ] && echo 1 || echo 0)"

  # A dead owner is reclaimed rather than deadlocked on.
  mkdir -p "$ITROOT/s4"; ln -s 999999 "$ITROOT/s4/lock"
  out=$(run_watcher "$ITROOT/s4" 4)
  check "integration: reclaims dead lock" "1"  "$(printf '%s' "$out" | grep -c 'reclaiming stale lock' || true)"
  check "integration: and then arms"      "1"  "$(printf '%s' "$out" | grep -c 'armed on' || true)"

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
chmod 700 "$STATE_DIR" 2>/dev/null || true   # state dir lives under a shared /tmp
STATE="$STATE_DIR/state"
PEND="$STATE_DIR/pending"
GHERR="$STATE_DIR/gh.err"

# The state files are read-modify-written non-atomically, so two watchers on
# one repo would interleave and lose events. mkdir is the atomic primitive.
#
# Reclaiming a stale lock has to go back through mkdir, not just overwrite the
# pid: two watchers starting together both find the same dead pid, and if the
# reclaim path simply falls through, BOTH believe they own it.
LOCK="$STATE_DIR/lock"
#
# The lock is a SYMLINK whose target is the owner's pid. That is deliberate:
# creating a symlink is a single atomic operation that carries the owner's
# identity with it, so there is no window between "the lock exists" and "the
# lock says who owns it".
#
# Two earlier designs failed exactly in that window. `mkdir` + write-a-pid-file
# is two steps: a watcher reading between them sees an owner-less lock. Adding
# a grace period only narrowed it — measured double-arms at a 1.5 s stall. And
# reclaiming via `mv` is not atomic the way it looks: `mv` into an EXISTING
# destination succeeds and moves the source INSIDE it, so the put-back path
# nested the stale lock rather than restoring it, and the `|| rm -rf` fallback
# never ran because `mv` returned 0.
lock_owner() { readlink "$LOCK" 2>/dev/null; }
acquire_lock() {
  local attempt owner
  for attempt in 1 2 3; do
    # `ln -s X dir` does NOT fail when dir is a directory — it creates the link
    # INSIDE it and returns 0, so every watcher "succeeds" and none of them own
    # anything. That is a live upgrade path: the previous revision of this
    # script used `mkdir "$LOCK"` + a pid file. Clear a non-symlink first.
    if [ -e "$LOCK" ] && [ ! -L "$LOCK" ]; then
      echo "PR-WATCH: clearing a non-symlink lock left by an older watcher layout"
      rm -rf "$LOCK"
    fi
    if ln -s "$$" "$LOCK" 2>/dev/null; then
      # Settle, then confirm the link still names US. A watcher that started at
      # the same instant may have judged the pre-existing lock dead and removed
      # ours between our create and now. Whoever the link names at the end is
      # the single owner; everyone else exits here.
      sleep 0.3
      [ "$(lock_owner)" = "$$" ] && return 0
      echo "PR-WATCH ERROR: another watcher took the lock during acquisition — exiting"
      return 1
    fi
    owner=$(lock_owner)
    if [ -n "${owner:-}" ] && kill -0 "$owner" 2>/dev/null; then
      echo "PR-WATCH ERROR: another watcher (pid $owner) already owns $STATE_DIR — exiting"
      return 1
    fi
    # Dead owner. Remove ONLY if the link still names the pid we judged dead —
    # an unconditional `rm` here is what let a loser delete a fresh winner's
    # link and leave several watchers each believing they had armed.
    echo "PR-WATCH: reclaiming stale lock from pid ${owner:-<unknown>}"
    [ "$(lock_owner)" = "${owner:-}" ] && rm -f "$LOCK"
  done
  echo "PR-WATCH ERROR: lost the race to reclaim $LOCK — another watcher won; exiting"
  return 1
}
acquire_lock || exit 1

# Only release a lock we still own. A watcher that was signalled after another
# process reclaimed its lock would otherwise delete the NEW owner's lock on the
# way out, and the damage compounds from there.
release_lock() {
  [ "$(lock_owner)" = "$$" ] && rm -rf "$LOCK"
  return 0
}
# INT/TERM must EXIT, not just release. A bash signal handler returns to the
# loop by default, which would leave a watcher polling on with no lock — a
# ghost that keeps consuming API calls and emitting events nobody owns.
#
# Three accepted caveats. Bash defers the handler until the in-flight `sleep`
# returns, so a stop takes up to POLL seconds to land. SIGKILL runs no handler
# at all, leaking the lock dir until the next watcher's stale-pid reclaim
# clears it. And in the documented launch mode — a background job of a
# non-interactive shell — SIGINT is inherited ignored and bash will not install
# a handler for it, so the INT trap is inert there; TERM is the operative
# signal, and it is what the Monitor tool sends.
trap 'release_lock' EXIT
trap 'release_lock; exit 130' INT
trap 'release_lock; exit 143' TERM

QUERY='.[] | "\(.number)\t\(.headRefOid)\t\(.isDraft)\t\(.author.login)\t\(.headRefName)\t\(.title)"'

# Baseline: every currently-open PR at its current head, so nothing already
# open fires until it changes. Re-arming re-baselines — a push that landed
# while the monitor was down is absorbed, not reported.
#
# The empty-response hazard is WORSE here than in the poll loop: an exit-0 blip
# during arming produces an empty baseline, and then every open PR fires as NEW
# on the first poll. One blip, not three. So confirm an empty baseline by
# repeating the query before believing the repo really has no open PRs.
baseline=""
for attempt in 1 2 3; do
  if ! baseline=$(gh pr list --repo "$REPO" --state open --limit "$LIMIT" \
                    --json number,headRefOid,isDraft \
                    -q '.[] | "\(.number) \(.headRefOid) \(.isDraft)"' 2>"$GHERR"); then
    echo "PR-WATCH ERROR: baseline query failed (gh auth?) — monitor exiting$(
      why=$(head -1 "$GHERR" 2>/dev/null); [ -n "${why:-}" ] && echo " — $why")"
    exit 1
  fi
  [ -n "$baseline" ] && break
  [ "$attempt" = 3 ] && break
  echo "PR-WATCH: baseline came back empty (attempt $attempt) — retrying before arming"
  sleep 2
done
printf '%s' "${baseline:+$baseline$'\n'}" > "$STATE"
: > "$PEND"
echo "PR-WATCH: armed on $REPO — new PRs + pushes to $(wc -l < "$STATE" | tr -d ' ') open PR(s), ${POLL}s poll, 1-cycle debounce$([ "$SKIP_DRAFTS" = 1 ] && echo ', drafts deferred')"

fails=0
truncated_warned=0
while true; do
  if cur=$(gh pr list --repo "$REPO" --state open --limit "$LIMIT" \
             --json number,headRefOid,isDraft,author,headRefName,title -q "$QUERY" 2>"$GHERR"); then
    [ "$fails" -ge 5 ] && echo "PR-WATCH: recovered, polling again"
    fails=0

    poll_once "$cur"
  else
    fails=$((fails + 1))
    # Re-warn periodically: a watcher that warned once an hour ago and has been
    # dead since is indistinguishable from a quiet repo. Carry gh's own first
    # line of stderr — "gh auth" vs "rate limit" vs DNS are different problems
    # and the operator cannot see this process's stderr.
    if [ "$fails" = 5 ] || { [ "$fails" -gt 5 ] && [ $(( fails % 30 )) = 0 ]; }; then
      why=$(head -1 "$GHERR" 2>/dev/null)
      echo "PR-WATCH WARNING: $fails consecutive 'gh pr list' failures — PR polling is broken${why:+ — $why}"
    fi
  fi
  sleep "$POLL"
done
