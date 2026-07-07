---
name: verify
description: "Project verify skill: how to prove a change in THIS repo actually works, routed by what was touched — firmware, app, tools, extensions, CI/skills. Used by the built-in verify flow before committing nontrivial changes."
---

# Verifying a change in rgb-sunglasses

This skill is **pure routing**: find every row below that matches files in your
diff (`git diff --name-only origin/main...HEAD` or your working tree), then do
what the linked skill says — the exact commands live in those skills, not here.
A change often matches several rows; do all of them.

- **Minimum verification** never needs hardware and must always be done.
- **Full verification** needs the physical board and/or phone and the matching
  hardware lock (root `CLAUDE.md` "Hardware locking", `/hw-lock`).
- Fresh worktree or new session? Run `/worktree-setup` first — otherwise builds
  and tests can silently operate on the wrong checkout.

## Routing table

| Changed area | Minimum verification (no hardware) | Full verification (hardware, lock required) |
| --- | --- | --- |
| `fw/src/**`, `fw/Kconfig`, `fw/prj.conf`, `fw/conf/**`, `fw/boards/**` | `/build-proto0` **and** `/build-dk` (see note 1) + a targeted Twister run over the affected subsystem (note 2) | `/flash-and-verify` — flash and cross-check behavior via the serial shell (`board` lock) |
| `fw/tests/**` | Run just the changed suite (`/test-fw` "Targeted runs" has the scoped `twister -T fw/tests/<area>/<name>` form) and confirm its **dotted scenario name** appears in the results (note 3) | n/a — the suites run on `native_sim` only |
| `app/**` | `/validate-app` — jest + TypeScript + eslint (note 4) | Launch on the phone per `app/CLAUDE.md` (hold the `app` lock, use `app/scripts/launch-app.sh` — never raw `npx expo run:android`); verify writes/notifies against the firmware serial shell, not the app UI (`app/CLAUDE.md` "Verifying a write/notify round-trip") |
| `fw/tools/**`, `fw/scripts/**` Python | `python3 -m pytest fw/tools/tests -v` from the repo root (note 5 says which command applies where) | n/a |
| `fw/extensions/**`, `fw/include/rgbx/**`, `fw/src/extensions/**` | Compile every extension with `fw/extensions/build.sh` (requires an existing proto0 build in `fw/build` — run `/build-proto0` first) + run the `extensions.manifest` Twister suite (`fw/tests/extensions/manifest`); `/add-extension` covers both | Runtime behavior of a `.llext` (load, relocation, rendering) is **hardware-only** — a clean cross-compile proves nothing about runtime; install and observe per `/add-extension` + `/flash-and-verify` |
| Device↔app communication — GATT services/characteristics/UUIDs, `*_bt.cpp` adapters, MCUmgr/DFU flow, value encodings the app decodes, animation IDs | Both the firmware row and the app row above | **BOTH sides on real hardware before the PR** — this is `/submit-pr` step 5's hard gate, not optional (note 6) |
| `.claude/**` (skills, hooks, agents), `scripts/`, root and `docs/` markdown | Self-review + literally dry-running the commands **is the entire gate** — these paths trigger no CI at all (note 7) | n/a |
| `.github/workflows/**` | Workflow edits DO trigger `build.yaml` (full firmware CI) on the PR — read the resulting run; there is no local workflow runner in the container. Remember CI is advisory (see "Merge gate reality") | n/a |

## Notes

1. **Always both boards.** proto0 is the day-to-day target, but DK flash
   overflow is the classic silent failure: a change that fits proto0 can
   overflow the DK, and you only find out at `/submit-pr` time (or in advisory
   CI). For size analysis see `/rom-ram-budget`.
2. If a changed `.c`/`.cpp` file is exercised by **no** suite, `/submit-pr`'s
   `lcov --extract` produces an empty tracefile, which it counts as **0% patch
   coverage** and stops the PR. Find or write the suite now (`/add-fw-test`),
   not at PR time. `/test-fw` is the full-suite + coverage run.
3. A suite with a malformed or misnamed `testcase.yaml` can silently not run —
   "no failures" then looks identical to "passed". Grep the Twister output for
   the suite's dotted name (e.g. `extensions.manifest`) before believing it.
4. App CI (`.github/workflows/app-ci.yml`) runs **only jest and a debug
   Android build** — no `tsc`, no eslint. Green CI does not mean clean types
   or lint; `/validate-app` is the only gate for those.
5. CI runs this as `build.yaml`'s `python-tests` job, but the pytest suite
   covers only the GLIM converter tools. Other Python under `fw/tools/` or
   `fw/scripts/` has no tests — for those, `python3 -m py_compile <file>` plus
   a dry-run on a real sample input is the minimum.
6. Why both sides: on PR #89 every build, Twister suite, and serial-shell
   check passed while the extensions' Is Active notify path was completely
   dead BLE-side — only a real app connection exposed it. The house norm (per
   PR history and `/submit-pr`): the PR body states exactly what **was**
   exercised on hardware and what was **not**. "Device/app verification:
   N/A — <reason>" or an explicit user-approved waiver is honest; silence is
   not. Never claim verification you did not perform.
7. `build.yaml` is path-filtered to `fw/**`, `.github/workflows/**`,
   `.devcontainer/**`; app CI to `app/**`. Nothing runs on `.claude/**`,
   `scripts/`, or markdown. So: for hook scripts, execute them with synthetic
   hook-event JSON on stdin and check both the allow and deny outcomes; for
   skills, execute every command block exactly as written in a scratch
   directory; for shared shell tooling (`scripts/hw-lock.sh` etc.), `bash -n`
   plus exercising the affected subcommands.

## Merge gate reality

`main` requires a PR (squash merge, zero required approvals) but has **zero
required status checks** — every CI check is advisory, and a red build does not
technically block a merge (as of 2026-07 — re-verify with
`gh api "repos/{owner}/{repo}/rulesets"`). The ≥50% patch-coverage gate, the
both-boards build requirement, and the device↔app verification gate all live in
`/submit-pr`, which is therefore the project's **only real merge gate**. Do not
bypass it for nontrivial firmware changes.
