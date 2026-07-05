---
name: hw-lock
description: Acquire, release, or check exclusive-access locks on the shared dev board (+J-Link) and the shared Android phone, so multiple Claude Code agent worktrees don't collide on real hardware
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

Acquire every resource you'll need in **one call** — acquiring is all-or-nothing:

```bash
scripts/hw-lock.sh acquire board        # about to flash / provision / open mcp__serial__* to the board
scripts/hw-lock.sh acquire app           # about to launch/drive the phone via mcp__execbro__*/ADB
scripts/hw-lock.sh acquire board app     # need both at once (e.g. submit-pr step 6)
```

If any requested resource is already held by someone else, the whole call
fails immediately (no waiting) and names the current holder (session id,
worktree, branch, how long). Anything this same call freshly acquired is
rolled back automatically — you never end up holding a partial set. Don't
hand-roll a retry loop around this — use `--wait` instead (below) if you'd
rather block than bail out.

### Queueing instead of bailing out: `--wait`

```bash
scripts/hw-lock.sh acquire board --wait 300                 # block up to 5 min
scripts/hw-lock.sh acquire board app --wait 600 --poll-interval 10
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
It holds the `app` lock for Metro's entire lifetime, releases it automatically
whenever that process stops (however it stops), and refuses to start a second
Metro instance — even from this same session if you forgot one is already
running. Pass `--wait SECONDS` (e.g. `app/scripts/launch-app.sh --wait 600
--device Pixel_9_Pro`) to block until another agent's Metro session frees up
instead of failing immediately.

## When done

```bash
scripts/hw-lock.sh release board app
scripts/hw-lock.sh release --all         # release everything this session currently holds
```

Release in every exit path of whatever you were doing, including early-failure
branches — not just the success path. (Not needed for `app/scripts/launch-app.sh`
— it releases automatically when the Metro process it started stops.)

## Checking status

```bash
scripts/hw-lock.sh status                # both resources
scripts/hw-lock.sh status board
```

Flags a lock `[STALE]` if its worktree no longer exists on disk, or (for locks
acquired with `--pid`, like the app-launcher's) if the tracked process is no
longer running — either case auto-reclaims on the next `acquire`.

## Escape hatches (use sparingly, prefer asking the user first)

- `scripts/hw-lock.sh acquire board --steal` — forcibly take the lock even
  though the holder's worktree still exists (e.g. a previous agent crashed
  without releasing).
- `scripts/hw-lock.sh release board --force` — release a lock this session
  doesn't own.

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
