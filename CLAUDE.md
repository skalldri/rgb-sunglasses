# CLAUDE.md — RGB Sunglasses Project

## Memory policy

**Always use in-repo files for memory.** This devcontainer is rebuilt frequently, so `~/.claude/` is ephemeral and must never be used to store facts that need to survive across sessions. Record everything worth remembering in:

- This file (cross-cutting agent behavior, project-wide facts)
- `fw/CLAUDE.md` (firmware-specific guidance)
- Other committed files in the repo

Never write to `~/.claude/projects/` or any other `~/.claude/` path for persistent notes.

## Session startup

At the start of every new conversation, run `/check-hardware` before doing anything else.
Report the results to the user as a brief summary table, then continue with whatever they asked for.

## Repository layout

| Directory | Contents |
|-----------|----------|
| `fw/` | Zephyr RTOS firmware (nRF5340). See `fw/CLAUDE.md` for build/test commands. |
| `app/` | React Native companion app (Expo). |
| `.devcontainer/` | Devcontainer definition and setup scripts. |
| `.claude/commands/` | Project slash commands (skills). |
