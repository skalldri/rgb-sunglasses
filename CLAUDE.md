# CLAUDE.md — RGB Sunglasses Project

## Memory policy

**Always use in-repo files for memory.** This devcontainer is rebuilt frequently, so `~/.claude/` is ephemeral and must never be used to store facts that need to survive across sessions. Record everything worth remembering in:

- This file (cross-cutting agent behavior, project-wide facts)
- `fw/CLAUDE.md` (firmware-specific guidance)
- `app/CLAUDE.md` (React Native app guidance)
- Other committed files in the repo

Never write to `~/.claude/projects/` or any other `~/.claude/` path for persistent notes.

## Working with hardware

Hardware iterations are slow and mistakes can cause damage. Before flashing anything:

- Read the relevant source code to confirm assumptions (Kconfig deps, handler logic, buffer sizes)
- Verify changes in `build/fw/zephyr/include/generated/zephyr/autoconf.h` before uploading
- Don't rely on web search results for Kconfig symbol names — check the actual NCS source under `/root/ncs/v3.1.1/`
- Verify memory-accounting claims against the linker map (`build/fw/zephyr/zephyr.map`) before proposing size/config changes — e.g. `.noinit` buffers (like the llext heap) ARE counted in the linker's RAM percentage, and secondary reports (footprint scripts) use different accounting than what governs link success

### NEVER write unverified commands or data into hardware parts

**Never send a command, register write, 4CC task, or configuration value to a physical
part (I2C/SPI peripheral, PD controller, charger, sensor, etc.) based on memory,
inference, or pattern-matching. LLM-recalled datasheet content is a hallucination until
proven otherwise, and a wrong write can permanently damage or wedge a chip.**

Before ANY write to a hardware part that is not already an established, in-repo,
hardware-proven code path:

1. **Obtain the authoritative source first** — the actual datasheet / technical
   reference manual (a PDF or excerpt provided by the user, or a doc checked into the
   repo). Web search summaries, training-data recall, and "the other constants look
   like this" pattern-matching do NOT count.
2. **If the source is not available, STOP and ask the user for it.** Do not "try
   something plausible and see" — hardware is not a REPL. The user would rather be
   asked than have a part bricked.
3. Cite the doc section for the exact bytes/values being written in the code comment,
   so the next reader can re-verify.
4. Reads are comparatively safe; writes are the danger. A define existing unused in
   the codebase is NOT evidence it is correct — unused code was never
   hardware-validated.

This rule exists because of a real incident (2026-07-05): unverified TPS25750 4CC
commands ("GO2P"/"Go2P" — spelling and semantics asserted from memory, not from the
TRM) were written to CMD1 on live hardware while attempting to force a patch
re-download, and the part ended up in a broken state. The correct move at step zero
was: "I don't have the TPS25750 host-interface TRM — please provide it before I write
anything to this chip."

## Hardware locking

Multiple agents, each in its own worktree, share one physical dev board
(+J-Link) and one physical Android phone. Before flashing, provisioning,
opening an `mcp__serial__*` connection to the board, or driving the phone via
`mcp__execbro__*`/ADB, hold the relevant lock. `hold` is the *only* way to
take a lock — there is no bare "acquire and forget." Launch it as a
long-lived task via the `Monitor` tool, then confirm before touching
hardware:

```
Monitor(command: "scripts/hw-lock.sh hold board", description: "board hw-lock heartbeat", persistent: true)
```
```bash
timeout 15 bash -c 'until scripts/hw-lock.sh check board >/dev/null 2>&1; do sleep 0.5; done'
```

When done, either stop the `hold` task (`TaskStop` — its own exit trap
releases automatically) or run `scripts/hw-lock.sh release board app`
yourself. **Holding a lock means exclusive access for as long as you keep the
`hold` task running — full stop.** It's never released by a timer or by a
hardware surface going quiet (e.g. the J-Link de-enumerating mid-flash is
normal and is never evidence the lock is safe to release); the only things
that end a hold are you stopping it, or the process dying (crash, kill,
container restart), which the stale-pid reclaim already handles safely on
the next attempt.

By default a conflicting `hold` fails immediately. Pass `--wait SECONDS`
(e.g. `hold board --wait 300`) to queue instead of bailing out — a real
FIFO queue (ticket per resource, oldest arrival goes first), not independent
agents racing each other when the resource frees up, and still strictly
all-or-nothing on every attempt so waiting agents can never deadlock on each
other.

If **your own session already holds** the resource and you run `hold` again
(e.g. after a heartbeat/`Monitor` task died or was lost across a context
reset), the new `hold` **adopts** the lock — it takes over as the tracked
heartbeat and reports success immediately, rather than refusing or waiting on
itself. So the recovery move after any board-lock heartbeat failure is simply
to **re-run `hold`**: if a prior in-session hold is still alive it's adopted, if
it died cleanly the lock was already released, and if it died hard the stale-pid
reclaim clears it first — every case ends with you holding a live heartbeat. A
`--wait` on a lock your own session holds is never futile now (it can't be — the
adopt path returns before queueing). A **different** session's hold still
conflicts exactly as before.

A `PreToolUse` hook (`.claude/hooks/hw-lock-guard.sh`) auto-denies every
`mcp__serial__*`/`mcp__execbro__*` call and known hardware-touching Bash
commands (`jlink-flash.sh`, `provision-device.sh`, `JLinkExe`, `mcumgr`,
`west flash`, `adb`, `expo run:android`) unless the relevant lock is held —
this is a backstop, not a substitute for holding proactively, since a denial
interrupts whatever flow triggered it. `fw/scripts/jlink-flash.sh` and
`fw/scripts/provision-device.sh` hard-refuse to run without the `board` lock
on their own, independent of the hook, so they're covered even outside a
Claude Code session — and neither ever acquires the lock itself, only checks
it. Launch the companion app via `app/scripts/launch-app.sh` (never call
`npx expo run:android` directly) — it follows the same check-only pattern
now: it refuses to run unless `app` is already held, and it no longer
acquires the lock itself, but the relationship isn't fully symmetric.
Stopping the `hold` task (or `release app`, same-session) now also stops
Metro if it's still running — releasing the lock guarantees Metro has
quit. Metro stopping or crashing on its own, though, still does not release
the lock — you still manage that side yourself. (A human force-releasing a
*different* session's still-live lock does not kill that session's Metro —
only same-session release does; the acquire-time cleanup below handles it
instead.) If a Metro/expo process is still running from an earlier, now-dead
session when `app` is next held, that hold kills it automatically before
considering itself established.

While holding a lock, if another agent is queued waiting for it, you'll be
nudged three ways: an `additionalContext` reminder on your next hardware tool
call, the same on your next turn (`UserPromptSubmit` hook), and — the one
that reaches you even fully idle — a notice `hold` itself prints, delivered
via the `Monitor` task's event stream. None of these ever force a release;
they're purely advisory.

**Release once you're genuinely done — not preemptively, and not so late
you're just squatting on it.** Don't release and re-acquire between steps of
one ongoing task you expect to repeat (e.g. a build → flash → test → build →
flash → test iteration loop) — hold across the whole cycle; releasing
between passes you're about to repeat just adds a race for zero benefit. But
don't keep holding "just in case" once the task that needed the resource is
actually finished — the waiter-notice above tells you exactly when someone
else needs it, which is the right moment to wrap up, not something to guess
at preemptively.

See `.claude/skills/hw-lock/SKILL.md` for the full command surface (status,
`--steal`, `--force`, `release --all`) and known enforcement limitations.

## "Remember" instructions

When the user says "Remember" (or "Remember that"), update the appropriate CLAUDE.md file immediately with the information. Prefer the root `CLAUDE.md` for cross-cutting rules and `fw/CLAUDE.md` for firmware-specific facts.

## Git workflow — ALWAYS branch before committing

**Never commit directly to `main`.** Always create a feature branch first (`git checkout -b <branch-name>`), then commit, push the branch, and open a PR. Do this before editing any files if possible, but at minimum before the first `git commit`.

### GitHub PR review comments via `gh api`

- While this account has a **pending (draft) review** on a PR, the API rejects ALL new review comments from it with 422 "user can only have one pending review per pull request" — both `POST /pulls/<n>/reviews` and standalone `POST /pulls/<n>/comments` (standalone line comments are single-comment reviews internally). Never touch or submit the user's pending review; fall back to regular PR comments (`gh pr comment`) with `https://github.com/<owner>/<repo>/blob/<sha>/<path>#L<line>` permalinks, which render the referenced snippet inline.
- Once a review is submitted, reply to its line-comment threads with `gh api repos/<owner>/<repo>/pulls/<n>/comments/<comment_id>/replies -f body=...`.

## Process management — NEVER use pkill

**Never use `pkill` or `killall` inside the devcontainer.** These commands kill processes across the entire container (including the container init, the MCP server, and the VS Code server), which crashes the devcontainer and terminates the session. To stop a background process, use its PID from `$!` or find it with `pgrep` and send a targeted `kill <pid>`. To restart Metro/Expo, just launch a new `npx expo run:android --device <device name> --app-id com.autom8ed.rgbsunglassesapp.dev` — it starts a fresh Metro instance.

## Installing tools

When installing any new CLI tool or dependency, **always add it to the devcontainer** (`.devcontainer/Dockerfile` or `postCreateCommand` in `.devcontainer/devcontainer.json`) so it is available to all users after a rebuild. Do not rely on ad-hoc `apt install` or `pip install` commands that only affect the current container instance.

## Session startup

**Your first output in every new conversation must be the environment status summary table — before any task work, even when the user opens with a specific request.** A `SessionStart` hook (configured in `.claude/settings.json`) already runs `check-hardware` and `check-software` automatically and injects their output into context as "Environment status (auto-checked at session start)", so you normally do **not** need to re-run the skills — just read that injected block and surface it. Only run `/check-hardware` / `/check-software` yourself if that injected block is missing.

Render the results as a brief markdown table (hardware: dev board, J-Link, Android/ADB; software: `gh` and any other tools). If any tool is NOT AUTHENTICATED or NOT READY, call it out explicitly in that first message — **don't wait until it blocks a later step** (e.g. `gh` auth blocks PR creation). Having the data in context is not enough; the user needs to see it up front.

Before working on any subsystem, **read its CLAUDE.md first** — those files are the project's persistent memory and contain critical workflow rules (correct commands, known pitfalls, launch procedures) that are not derivable from the code alone. Skipping them leads to doing the wrong thing (e.g. launching the Android app incorrectly). Specifically:

- About to touch firmware (`fw/`)? Read `fw/CLAUDE.md` first.
- About to touch the app (`app/`)? Read `app/CLAUDE.md` first.

## Repository layout

| Directory           | Contents                                                                                 |
| ------------------- | ---------------------------------------------------------------------------------------- |
| `fw/`               | Zephyr RTOS firmware (nRF5340). See `fw/CLAUDE.md` for build/test commands.              |
| `app/`              | React Native companion app (Expo). See `app/CLAUDE.md` for architecture and agent notes. |
| `.devcontainer/`    | Devcontainer definition and setup scripts.                                               |
| `.claude/skills/`   | Project skills (slash commands).                                                         |
| `.claude/hooks/`    | Claude Code hooks (e.g. the hardware-lock `PreToolUse` guard).                           |
| `scripts/`          | Cross-cutting host tooling shared by all subsystems — currently just `hw-lock.sh`, the multi-agent hardware-lock coordinator (see "Hardware locking" above). |
