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
| `claude/skills/`    | Project skills (slash commands).                                                         |
