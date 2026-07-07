---
name: worktree-setup
description: "First task in a fresh git worktree or new session: what exists, what is missing (node_modules, build dirs, twister output), and the traps that make commands silently operate on the wrong checkout. Run before building, testing, or editing in a worktree."
allowed-tools: Bash, Read, Grep
---

# Worktree / session orientation

Agent worktrees live at `.claude/worktrees/<name>/` under the main checkout. They share
git refs and one physical dev board + phone with every other agent, but **not** the
filesystem: anything gitignored (installed deps, build output) does not exist in a
fresh worktree.

## 1. Orient yourself

```bash
git rev-parse --show-toplevel      # THIS checkout's root — your repo root for everything
git rev-parse --git-common-dir     # the ONE shared .git, under the main checkout's root
git branch --show-current
```

Locks, refs, and branches are shared across worktrees via the common git dir; files are
not. Before touching a subsystem, read its CLAUDE.md first (root CLAUDE.md rule):
`fw/CLAUDE.md` for firmware, `app/CLAUDE.md` for the app.

## 2. What a fresh worktree LACKS (expected, not an error)

| Missing | Fix | Notes |
|---|---|---|
| `app/node_modules` | `cd app && npm ci` | ~30s; `postinstall` reapplies the ble-plx patch. **Never** symlink from the main checkout — Metro breaks (see `app/CLAUDE.md`, "Running the app from inside a git worktree"). |
| `fw/build` (proto0) | `/build-proto0` | First build is pristine and slow; the skill handles it. |
| `fw/build-dk` (DK) | `/build-dk` | Separate dir on purpose — never mix boards in one build dir. |
| `fw/twister-out` | `/test-fw` creates it | See coverage trap below. |

**Coverage trap:** the MAIN checkout usually has a populated `fw/twister-out/` of its
own. If a command reaches it via an absolute path, your lcov summaries /
patch-coverage extracts silently read *another checkout's stale* `coverage.info`. Only ever use `fw/twister-out/coverage.info` relative to
**this** worktree root, produced by a `/test-fw` run you made here.

## 3. THE PATH TRAP (PR #94 — real incident)

Skills and docs once hardcoded absolute paths into the main checkout (the container's
top-level repo clone). Inside a worktree those resolve to the **main checkout** —
commands "succeed" while building, editing, or measuring the wrong tree.

- Never use absolute paths that point into the main checkout in commands, scripts,
  skills, or docs. Always cwd-relative from the worktree root.
- If you need an absolute path (some tools require one), derive it:
  `"$(git rev-parse --show-toplevel)/..."`.
- This is a **review-blocking rule** for any new script or skill you author.

## 4. Branch discipline

Never commit to `main` (root CLAUDE.md, "Git workflow"). A PreToolUse hook
(`.claude/hooks/destructive-guard.sh`) hard-denies `git commit` while on `main`, so
create the feature branch **first**, ideally before editing anything:

```bash
git checkout -b <feature-branch>
```

## 5. Shared hardware — one board, one phone, many agents

Every worktree shares a single dev board (+J-Link) and a single Android phone. Any
step that touches them — flashing, provisioning, serial, ADB, launching the app on
device — goes through the hardware lock first: see the **/hw-lock** skill and root
CLAUDE.md "Hardware locking" for the hold/release mechanics. The lock storage lives
under `$(git rev-parse --git-common-dir)/hardware-locks/`, i.e. inside the one shared
`.git` — which is exactly why it coordinates across all worktrees.

## 6. The guard hook, explained (a denial is not a broken environment)

A PreToolUse guard (`.claude/hooks/hw-lock-guard.sh`) denies `mcp__serial__*` /
`mcp__execbro__*` calls and any Bash command whose **text** contains a
hardware-tooling token, unless the matching lock is held. Board tokens:
`jlink-flash.sh`, `provision-device.sh`, `JLinkExe`, `nrfutil device`, `mcumgr`,
`west flash`. App tokens: `adb`, `expo run:android`, `adb-connect.sh`
(grep anchors: `BASH_BOARD_PATTERNS` / `BASH_APP_PATTERNS` in the hook; board
patterns are checked first).

It matches command text, so read-only commands false-positive: `grep mcumgr app/...`
or `cat app/services/mcumgr.ts` get denied without the **`board`** lock (`mcumgr` is
a board token, even in an app-side filename); `adb` anywhere in a command text
requires the `app` lock. A denial means
**"take the lock or avoid the token"** — for read-only work use the Read/Grep *tools*
(not Bash), or rephrase to dodge the literal token (e.g. glob `app/services/mcu*.ts`).

## 7. What task is this? Route before improvising

Consult the **Task routing** table in root `CLAUDE.md` — it maps each task archetype
(add an animation, debug BLE, memory budget work, ...) to the right skill. Do not
re-derive workflows the table already routes.

## 8. Quick sanity checklist

```bash
gh auth status                      # NOT AUTHENTICATED blocks PR creation — surface it now
ls app/node_modules >/dev/null      # error => run: cd app && npm ci (before any app work)
ls fw/build 2>/dev/null             # empty/missing => first /build-proto0 will be slow; expected
```

A `SessionStart` hook (registered in `.claude/settings.json`) already ran
`check-hardware` / `check-software` and injected the results into context as
"Environment status (auto-checked at session start)" — read that block instead of
re-running the skills. Root CLAUDE.md's "Session startup" rule applies: your first
output of a new conversation must surface that status as a brief markdown table,
calling out anything NOT READY / NOT AUTHENTICATED.

## Done — next steps by task type

Building: **/build-proto0** (or **/build-dk**). Testing: **/test-fw**. App checks
without a phone: **/validate-app**. Hardware: **/hw-lock** first. PR: **/submit-pr**.
