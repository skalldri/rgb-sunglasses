---
name: validate-app
description: "Run all companion-app checks that need no phone or hardware lock: jest unit tests, TypeScript typecheck, eslint. Use after any app/ change and before any PR touching app code — CI only runs jest, so clean CI does not mean clean types or lint."
---

# Validate the companion app (device-free)

Three checks cover the app's entire device-free validation surface. CI
(`.github/workflows/app-ci.yml`) runs only `npm test -- --ci` plus a Gradle
`assembleDebug` — it runs **neither** `tsc` **nor** lint, so a green CI run does not
mean the types or lint are clean. Run all three locally.

## 0. Precondition — install dependencies (fresh worktree)

A fresh worktree has **no `app/node_modules`**; jest, tsc, and eslint all fail without it.

```bash
cd app && npm ci
```

- Takes ~30 s. The `postinstall` script runs `patch-package`, which applies
  `app/patches/react-native-ble-plx+3.5.0.patch` automatically.
- **NEVER symlink `node_modules` from the main checkout** — Metro cannot resolve
  modules through a symlink outside the project root; this broke a real session
  (see `app/CLAUDE.md`, "Running the app from inside a git worktree").
- Skip this step only if `app/node_modules/` already exists from an earlier `npm ci`.

## 1. Unit tests (jest)

```bash
cd app && npm test            # local run
cd app && npm test -- --ci    # exact CI invocation
```

Single file: `cd app && npx jest __tests__/ble-value-codec.test.ts` (substitute any
file from `app/__tests__/`).

Pass looks like `Tests: ... passed`, exit 0. Failure prints the failing suite/assertion
and exits non-zero.

## 2. Typecheck (tsc)

```bash
cd app && npx tsc --noEmit
```

There is **no `typecheck` npm script** in `app/package.json` — do not invent one or
assume `npm run typecheck` works. Pass = no output, exit 0. Failure = `error TSxxxx`
lines with file:line locations.

## 3. Lint (eslint)

```bash
cd app && npm run lint
```

Runs `expo lint` with the flat config in `app/eslint.config.js`. Pass = no
errors, exit 0.

## Pitfalls

- **New test imports an unmocked native/expo module** → fails with native-module
  errors (e.g. "cannot find native module"). Fix by adding the mock to
  `app/jest.setup.ts` — which already mocks reanimated, expo-router, both
  `expo-file-system` entrypoints (`/next` and `/legacy`), and `react-native-ble-plx`,
  among others — **not** per-test, if more than one suite needs it. Read
  `app/jest.setup.ts` first to see what's already covered.
- **Image imports in tests** resolve via `app/test/fileMock.js` (wired in
  `app/jest.config.js`); no action needed unless you add a new image extension.
- **patch-package dual-invocation trap**: bare `npx patch-package` (no args)
  *applies* the checked-in patches — safe. `npx patch-package react-native-ble-plx`
  *regenerates* the patch file by diffing current `node_modules`, and running it on an
  already-patched tree **corrupts** `app/patches/react-native-ble-plx+3.5.0.patch`.
  Never run the package-name form unless you are deliberately saving a new patch edit
  (procedure in `app/CLAUDE.md`, "Known Issues & Quirks").
- **hw-lock hook false positive**: the `PreToolUse` guard
  (`.claude/hooks/hw-lock-guard.sh`) denies any Bash command whose *text* matches
  `\bmcumgr\b` (needs the `board` lock) or `\badb\b` (needs the `app` lock) — even a
  read-only `grep something app/services/mcumgr.ts`. For read-only work on
  `app/services/mcumgr.ts`, use the Read/Grep *tools* (not guarded), or avoid the
  literal token in Bash with a glob: `ls app/services/mcu*.ts`,
  `cat app/services/mcu*.ts`. Do NOT take a hardware lock just to grep a file.

## Test placement invariant

Every module in `app/services/` has a matching test file in `app/__tests__/`
(e.g. `app/services/battery.ts` ↔ `app/__tests__/battery.test.ts`; all 8 service
modules covered as of 2026-07 — re-verify). If you add or split a service module,
add the matching `app/__tests__/<name>.test.ts` in the same change.

## Scope — what this skill does NOT cover

These three checks are the complete device-free surface. Actually driving the app on
the phone is a separate, hardware-locked activity: it requires holding the `app` lock
and launching via `app/scripts/launch-app.sh` — follow the **mandatory launch
procedure in `app/CLAUDE.md`** ("Launching the App" and the worktree section) exactly;
do not improvise with `npx expo run:android`.

## Exit criteria

All three checks green (tests pass, `tsc --noEmit` silent, lint clean) → proceed to
`/submit-pr` for the PR gate. If any check fails, fix and re-run that check before
moving on.
