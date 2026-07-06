---
name: hw-lock
description: Hold, release, or check exclusive-access locks on the shared dev board (+J-Link) and the shared Android phone, so multiple Claude Code agent worktrees don't collide on real hardware
allowed-tools: Bash(scripts/hw-lock.sh *)
---

Multiple Claude Code agents can run at once, each in its own git worktree, but there
is only one physical nRF5340 dev board (+J-Link) and one physical Android phone.
`scripts/hw-lock.sh` coordinates exclusive "checkout" of these two resources across
agents via lock directories under the repo's shared `.git` dir (visible identically
from every worktree).

Resources: `board` (dev board + J-Link, used together) and `app` (the one physical
Android phone). Nothing more granular than this.

---

## Before touching hardware

`hold` is the **only** way to take a lock — there is deliberately no bare
"acquire and forget" subcommand. It always runs as a long-lived, foregrounded
process, so launch it via the `Monitor` tool (`persistent: true`), then confirm
via a short `check` poll before touching anything:

```
Monitor(command: "scripts/hw-lock.sh hold board", description: "board hw-lock heartbeat", persistent: true)
```
```bash
timeout 15 bash -c 'until scripts/hw-lock.sh check board >/dev/null 2>&1; do sleep 0.5; done'
```

Need both resources at once (e.g. `submit-pr`'s combined step)? `hold board app`
— acquiring is all-or-nothing, same as before. If any requested resource is
already held by someone else, `hold` fails immediately (no waiting) and names
the current holder (session id, worktree, branch, how long); `check`'s exit
code in the confirm-loop reflects that failure. Don't hand-roll a retry loop
around this — use `--wait` (below) if you'd rather block than bail out.

**`hold` holds the lock for exactly as long as it keeps running, full stop —
no timer, no hardware-state signal (the J-Link de-enumerating mid-flash, a
board going quiet, etc.) ever releases it early.** If another agent needs
what you're holding, `hold` only ever tells you (see "Getting nudged" below)
— it never auto-evicts. You decide when to actually let go, by stopping the
`hold` task (`TaskStop`, which releases automatically via its own exit trap)
or by running `release ... --force` yourself.

### Queueing instead of bailing out: `--wait`

```
Monitor(command: "scripts/hw-lock.sh hold board --wait 300", persistent: true)   # block up to 5 min
```

With `--wait SECONDS`, a conflict doesn't fail immediately — it queues **in
strict FIFO arrival order** and blocks (polling, default every 5s) until
either every requested resource is acquired or the deadline passes, whichever
comes first, logging who it's waiting on, your queue position, and a progress
update roughly every 30s. This is a real ordered queue, not independent
retry-and-race: each waiter registers a ticket per requested resource when it
starts waiting, and only ever attempts the actual acquire once its own ticket
is the oldest still-valid one on **every** resource it needs — so if two
agents are both waiting on the same resource, whoever started waiting first
gets it first when it frees up, not whoever's poll happens to fire first. A
waiter that crashes or is killed doesn't block the line — its ticket is
recognized as dead (owning process no longer running) and skipped by whoever
else checks the queue next. Still strictly all-or-nothing on **every** poll:
an attempt is only ever made when it's fully your turn, and it still rolls
back anything freshly grabbed if any resource in the set conflicts, so you
never end up holding part of the set while queued for the rest — two agents
each waiting on a pair of resources the other holds can't deadlock on each
other this way. Without `--wait` (the default), behavior is unchanged: fail
fast, no queueing.

Note the asymmetry: `--wait` only affects the *acquire* phase. Once `hold` is
actually holding the lock, there is no equivalent "notify me the instant it's
my turn" for the confirm-loop above — you're blocked in that foreground
`check` poll for as long as the wait takes, exactly as you would be with a
synchronous wait. What `hold` adds is the *other* direction: once you're in,
it watches for waiters on your behalf so you don't have to keep checking.

**A `PreToolUse` hook auto-denies hardware-touching tool calls when the
relevant lock isn't held** — every `mcp__serial__*`/`mcp__execbro__*` call, and
Bash commands invoking `jlink-flash.sh`/`provision-device.sh`/`JLinkExe`/
`nrfutil`/`mcumgr`/`west flash`/`adb`/`expo run:android`. This is a backstop,
not a substitute for acquiring proactively — a denial interrupts whatever flow
triggered it, so acquire the lock *before* starting hardware work rather than
finding out from a denial mid-task.

Two implementation details worth knowing if you ever touch `.claude/settings.json`'s
`PreToolUse` config:
- **The Bash matcher is scoped with `if` conditions per hardware-touching
  pattern (`Bash(*jlink-flash.sh*)`, `Bash(*mcumgr*)`, etc.), not a bare
  `"matcher": "Bash"`.** Matching every Bash call unconditionally means the
  dispatcher spawns on every single command in the session, not just
  hardware-touching ones — expensive and, worse, means any bug in the
  dispatcher breaks *all* Bash usage, not just the hardware-related sliver.
  **This is a heuristic substring match, not semantic** — a read-only command
  that merely mentions one of these filenames as an argument (e.g. `grep ...
  fw/scripts/jlink-flash.sh`) matches and gets denied too, even though it
  never touches hardware. Prefer `Read`/`Grep` over `Bash` for pure text
  search on these files if you hit this.
- **The hook `command` never references `${CLAUDE_PROJECT_DIR}`.** That
  variable is documented to resolve to the git project root "regardless of
  the working directory when the hook runs" — in this repo, where worktrees
  nest inside the main checkout, that means it always points at the main
  checkout, not the worktree actually running the session, so a worktree-only
  branch's copy of `hw-lock-guard.sh` would never be found (silent allow-by-error
  at best, hard failures at worst). Instead, `command` is a small inline
  `python3 -c` bootstrap that reads the hook's own stdin JSON first, pulls the
  `cwd` field out of it (this *is* reliably the calling session's actual
  worktree, per Claude Code's hook I/O contract), and only then execs
  `<cwd>/.claude/hooks/hw-lock-guard.sh` with the same stdin forwarded through.
  It fails open (allow) if that path doesn't exist, rather than erroring, so
  an older worktree without this feature isn't broken by it.

## Launching the companion app

Don't call `npx expo run:android` directly — use `app/scripts/launch-app.sh`
instead (same "background it, don't use `&` by hand" convention as before).
**It no longer manages the lock itself** — it only verifies the `app` lock is
already held by this session and hard-refuses to run otherwise, exactly like
`fw/scripts/jlink-flash.sh`/`fw/scripts/provision-device.sh` already do for
`board`. Acquire `app` yourself first:

```
Monitor(command: "scripts/hw-lock.sh hold app", persistent: true)
```
```bash
timeout 15 bash -c 'until scripts/hw-lock.sh check app >/dev/null 2>&1; do sleep 0.5; done'
```

then launch `app/scripts/launch-app.sh --device <name>` as usual. Metro's
lifetime and the lock's lifetime are now independent: stopping Metro (or it
crashing) does **not** release the lock, and stopping the `hold` task does
**not** stop Metro — manage the two separately. If a leftover Metro/expo
process is still running from an earlier, now-dead session when `hold app`
next acquires, `hold` detects and kills it automatically before considering
itself established (never `pkill`/`killall` — targeted `pgrep`+`kill` only),
so you don't have to hunt down orphans by hand.

## When done

```bash
scripts/hw-lock.sh release board app
scripts/hw-lock.sh release --all         # release everything this session currently holds
```

Or just stop the `hold` task (`TaskStop`) — its own exit trap releases
automatically, which is the normal way to end a hold. Release in every exit
path if you're doing it by hand instead, including early-failure branches —
not just the success path.

**Release once you're actually done — not preemptively, and not so late
that you're just squatting on it:**

- Don't release and immediately re-acquire between steps of one ongoing
  task. A cycle you expect to repeat — e.g. iterating on a firmware fix:
  build, flash, observe behavior over `mcp__serial__*`, adjust code, rebuild,
  reflash — should hold the lock across the **entire** loop, not release
  between passes. Releasing between iterations you're about to repeat adds a
  real race (another agent's `hold` could grab it in the gap) for zero
  benefit, since you need the same resource again within seconds anyway.
- Don't keep holding "just in case" once the task that actually needed the
  resource is finished (the PR is up, the feature is verified, you've moved
  on to unrelated work). `hold`'s own waiter-notice (see "Getting nudged"
  above) tells you exactly when someone else actually needs it — that's the
  right moment to wrap up, not something to guess at preemptively by
  releasing early out of caution.
- When genuinely unsure whether you'll need it again soon within the same
  task, err toward holding a bit longer rather than releasing — let the
  waiter-notice, not a guess about idle time, be the signal to actually let
  go.

## Checking status

```bash
scripts/hw-lock.sh status                # both resources
scripts/hw-lock.sh status board
```

Flags a lock `[STALE]` if its worktree no longer exists on disk, or if the
tracked process (every lock is pid-tracked now, since `hold` always sets one)
is no longer running — either case auto-reclaims on the next `hold`.

```bash
scripts/hw-lock.sh waiters board          # prints just the queue depth, e.g. "0" or "2"
```

Machine-readable count of other sessions currently queued (via `--wait`) for
a resource — the number `status`'s "(N waiting)" suffix is derived from, for
scripts/hooks that want to act on it directly.

## Getting nudged when someone else is waiting

Holding a lock indefinitely is fine as long as nobody else needs it. Once
someone does, three independent channels surface that back into your own
conversation:

- Every tool call you make against a resource you hold (`mcp__serial__*`,
  `mcp__execbro__*`, or the guarded Bash commands) gets an `additionalContext`
  reminder attached if anyone is queued for it (`.claude/hooks/hw-lock-guard.sh`).
- Every new turn you take gets the same reminder, even if you're not touching
  hardware tools at that moment (`.claude/hooks/hw-lock-waiter-notice.sh`,
  a `UserPromptSubmit` hook).
- **`hold` itself prints a notice** (`WAITER_PRESENT: ...`) the moment it
  detects someone queued, surfaced as a `Monitor` event — this is the one
  channel that reaches you even if you're doing nothing else at all (no
  hardware tool calls, no new turns), which is the actual gap the first two
  can't cover. It re-notifies at most every ~10 minutes while the same waiter
  is still there, and resets the moment the queue empties, so it won't spam
  a long hold.

None of the three ever block or auto-release anything — all purely advisory.
When you see one, wrap up and release as soon as reasonably possible rather
than continuing to hold the resource indefinitely.

## Escape hatches (use sparingly, prefer asking the user first)

- `scripts/hw-lock.sh hold board --steal` — forcibly take the lock even
  though the holder's worktree still exists (e.g. a previous agent crashed
  without releasing).
- `scripts/hw-lock.sh release board --force` — release a lock this session
  doesn't own.
- `scripts/hw-lock.sh release app --force` — release a lock whose tracked
  process (every `hold`, including a still-running `launch-app.sh`'s Metro
  session tracked via its own `hold`) is **still alive**. A plain `release`
  refuses this and tells you to stop the owning process instead (which
  releases the lock automatically) — bypassing that with `--force` leaves
  the lock FREE while the process keeps actually using the resource, so
  another agent can now acquire it and collide with it. Only reach for this
  if you specifically want the lock freed *without* stopping the process
  (rare) and understand that risk.

## What this does NOT enforce

The `PreToolUse` hook (`.claude/hooks/hw-lock-guard.sh`) covers every
`mcp__serial__*`/`mcp__execbro__*` call and a known list of hardware-touching
Bash commands — this is broad, but the Bash pattern list is a heuristic, not
exhaustive (e.g. an ad-hoc `mount` of the board's NAND disk isn't caught), and
the hook only applies inside a Claude Code session — a human or external
process operating the hardware directly is unaffected. `fw/scripts/jlink-flash.sh`
and `fw/scripts/provision-device.sh` additionally hard-refuse to run without
the `board` lock on their own, independent of the hook, so those two specific
scripts are covered even outside Claude Code. See root `CLAUDE.md`'s "Hardware
locking" section.

**There is still no way for one session to push a notification directly into
a different, already-running session's conversation.** `hold`'s printed
notice works because it's *your own* background task — the harness re-invokes
your session when your own task produces output, even if you're otherwise
idle; it's not a channel any other session can address into you directly. If
your process is fully gone (killed, container restarted) rather than merely
idle, nothing reaches you — but in that case the lock's tracked pid is also
gone, so the next `hold`/`check` attempt reclaims it as stale automatically
rather than staying stuck.
