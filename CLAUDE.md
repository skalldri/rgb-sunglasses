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

## "Remember" instructions

When the user says "Remember" (or "Remember that"), update the appropriate CLAUDE.md file immediately with the information. Prefer the root `CLAUDE.md` for cross-cutting rules and `fw/CLAUDE.md` for firmware-specific facts.

## Installing tools

When installing any new CLI tool or dependency, **always add it to the devcontainer** (`.devcontainer/Dockerfile` or `postCreateCommand` in `.devcontainer/devcontainer.json`) so it is available to all users after a rebuild. Do not rely on ad-hoc `apt install` or `pip install` commands that only affect the current container instance.

## Session startup

At the start of every new conversation, run `/check-hardware` before doing anything else.
Report the results to the user as a brief summary table, then continue with whatever they asked for.

## Repository layout

| Directory | Contents |
|-----------|----------|
| `fw/` | Zephyr RTOS firmware (nRF5340). See `fw/CLAUDE.md` for build/test commands. |
| `app/` | React Native companion app (Expo). See `app/CLAUDE.md` for architecture and agent notes. |
| `.devcontainer/` | Devcontainer definition and setup scripts. |
| `.claude/commands/` | Project slash commands (skills). |
