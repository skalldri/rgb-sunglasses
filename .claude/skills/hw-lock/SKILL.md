---
name: hw-lock
description: Hold, release, or check exclusive-access locks on the shared dev board (+J-Link) and the shared Android phone, so multiple Claude Code agent worktrees don't collide on real hardware
allowed-tools: Bash(scripts/hw-lock.sh *)
---

Root `CLAUDE.md` § "Hardware locking" (always loaded) is canonical for the core model
(`hold` is the only way to take a lock, the `Monitor`+`check` invocation, exclusive until
the `hold` task stops, waiter nudges, release timing). This skill: command surface + details.

Resources: `board` (dev board + J-Link, together) and `app` (the one phone) — nothing more
granular. Locks are directories under the repo's shared `.git` dir, visible from every worktree
(and therefore per-host — correct, since the hardware is attached to one host at a time).
Works on macOS too: the script re-execs into Homebrew bash ≥ 4 (from `scripts/macos-setup.sh`)
when run under the stock bash 3.2.

## hold

```
Monitor(command: "scripts/hw-lock.sh hold board", description: "board hw-lock heartbeat", persistent: true)
```
```bash
timeout 15 bash -c 'until scripts/hw-lock.sh check board >/dev/null 2>&1; do sleep 0.5; done'
```

- Need both? `hold board app` — acquiring is all-or-nothing.
- On conflict (default): fails immediately and names the current holder (session id,
  worktree, branch, held-for). Don't hand-roll a retry loop — use `--wait`.

### `--wait SECONDS` — queue instead of bailing out

```
Monitor(command: "scripts/hw-lock.sh hold board --wait 300", persistent: true)   # block up to 5 min
```

- Strict FIFO by arrival: a ticket per requested resource; a waiter attempts the acquire
  only when its ticket is the oldest still-valid one on **every** resource it needs. Dead
  waiters (owning process gone) don't block the line — their tickets are skipped.
- Polls every 5s (`--poll-interval SECONDS` to change) until acquired or deadline; logs the blocker, queue position, progress ~30s.
- Still all-or-nothing per attempt (partial grabs roll back), so two agents each waiting on a pair the other holds can't deadlock.
- `--wait` affects only the acquire phase — no "it's your turn" notification; you're blocked in the foreground `check` poll for the duration.

### Re-holding a lock your own session already holds: adoption

Root `CLAUDE.md` covers the core model (adopts instead of refusing/queueing; `--wait` is
never futile on your own hold). Detail not in `CLAUDE.md`: the superseded sibling process,
if still alive, becomes a harmless no-op after adoption — its release is pid-guarded, so
when it eventually exits it won't yank the lock out from under the adopter; only whichever
process is the *current* tracked pid ever releases.

## Launching the companion app

Hold `app` first — `app/scripts/launch-app.sh` only verifies the lock, never acquires (usage conventions: `app/CLAUDE.md`):

```
Monitor(command: "scripts/hw-lock.sh hold app", persistent: true)
```
```bash
timeout 15 bash -c 'until scripts/hw-lock.sh check app >/dev/null 2>&1; do sleep 0.5; done'
```

Metro/lock lifecycle is asymmetric (release stops Metro; Metro dying never releases):

- `launch-app.sh` records its pid against the lock right before exec-ing into Metro
  (`note-metro-pid`, internal subcommand), so same-session release stops Metro precisely,
  not by process-name pattern-matching.
- Orphan sweep: a Metro/expo process left by a now-dead session is killed by the next
  `hold app` (targeted `pgrep`+`kill`, never `pkill`/`killall`) before the hold considers
  itself established — the pattern-based backstop for a `hold` killed before it recorded a pid.

## release

```bash
scripts/hw-lock.sh release board app
scripts/hw-lock.sh release --all         # release everything this session currently holds
```

- Normal path: stop the `hold` task (`TaskStop`) — its exit trap releases automatically.
- Releasing by hand? Do it in every exit path, including early-failure branches.
- When to release: root `CLAUDE.md` (hold across iteration loops; the waiter-notice is the signal).

## status / waiters

```bash
scripts/hw-lock.sh status                # both resources
scripts/hw-lock.sh status board
scripts/hw-lock.sh waiters board          # prints just the queue depth, e.g. "0" or "2"
```

- `[STALE]`: worktree gone from disk, or tracked pid no longer running (every lock is
  pid-tracked, since `hold` always sets one) — either auto-reclaims on the next `hold`.
- `waiters` is the machine-readable count behind `status`'s "(N waiting)" suffix.

## Waiter nudges — three channels (all advisory; none block or auto-release)

- Every tool call against a held resource gets an `additionalContext` reminder (`.claude/hooks/hw-lock-guard.sh`).
- Every new turn gets the same (`.claude/hooks/hw-lock-waiter-notice.sh`, a `UserPromptSubmit` hook).
- `hold` itself prints `WAITER_PRESENT: ...` as a `Monitor` event — the only channel that reaches you fully idle; re-notifies at most ~every 10 min per ongoing waiter, resets when the queue empties.

## Escape hatches (use sparingly, prefer asking the user first)

- `hold board --steal` — take the lock even though the holder's worktree still exists
  (e.g. a previous agent crashed without releasing).
- `release board --force` — release a lock this session doesn't own.
- `release app --force` on a still-alive `hold`: plain `release` refuses (stop the owning
  process instead); `--force` leaves the lock FREE while the process keeps using the
  resource — another agent can acquire and collide. Metro under `--force`: same-session
  still stops the tracked Metro (only the hold-pid liveness check is bypassed);
  cross-session leaves the other session's Metro alone (warning only) — the acquire-time
  orphan sweep cleans it up on the next `hold app`.

## PreToolUse hook implementation notes (`.claude/settings.json`)

The guard denies `mcp__serial__*`/`mcp__execbro__*` calls and Bash invoking
`jlink-flash.sh`/`provision-device.sh`/`JLinkExe`/`nrfutil`/`mcumgr`/`west flash`/`adb`/
`expo run:android` (`nrfutil` is guarded too, beyond root `CLAUDE.md`'s list).

- The Bash matcher is keyword-scoped (`Bash(*jlink-flash.sh*)`, `Bash(*mcumgr*)`, …), not
  a bare `"matcher": "Bash"` — avoids spawning the dispatcher on every command and keeps
  a dispatcher bug from breaking all Bash usage instead of just the hardware sliver.
- The match is substring, not semantic: a read-only command merely mentioning a guarded
  filename (e.g. `grep ... fw/scripts/jlink-flash.sh`) is denied too — prefer
  `Read`/`Grep` over `Bash` for pure text search on those files.
- The hook `command` never uses `${CLAUDE_PROJECT_DIR}` — with worktrees nested inside the
  main checkout it always resolves to the main checkout, so a worktree-only copy of
  `hw-lock-guard.sh` would never be found. Instead an inline `python3 -c` shim reads the
  hook's stdin JSON, takes its `cwd` field (reliably the session's actual worktree, per the
  hook I/O contract), and execs `<cwd>/.claude/hooks/hw-lock-guard.sh` with stdin forwarded
  — failing open (allow) if that path doesn't exist, so older worktrees aren't broken.

## What this does NOT enforce

- The Bash pattern list is a heuristic, not exhaustive — e.g. an ad-hoc `mount` of the
  board's NAND disk isn't caught.
- The hook only applies inside a Claude Code session; humans or external processes are
  unaffected. Outside Claude Code, only `fw/scripts/jlink-flash.sh` and
  `fw/scripts/provision-device.sh` self-refuse without the `board` lock (root `CLAUDE.md`).
- No session can push a notification into another running session's conversation —
  `hold`'s printed notice works only because it's *your own* `Monitor` task. A fully-gone
  (not merely idle) process hears nothing — but its tracked pid is gone too, so the next
  `hold`/`check` reclaims the lock as stale automatically.
