---
name: submit-pr
description: Pre-PR validation gate for firmware — builds both boards, runs tests, checks patch coverage ≥ 50%, requires on-device + companion-app verification for any change touching device↔app communication, then creates the GitHub PR
allowed-tools: Bash, Read, Task, AskUserQuestion, mcp__serial, mcp__execbro
---

Run all pre-PR checks for firmware changes, then push and open a pull request. **Do not push or create a PR if any check fails.**

**This skill IS the project's merge gate.** `main` has no required status checks — the
ruleset enforces only "PR required" + squash merge (as of 2026-07 — re-verify: `gh api
repos/skalldri/rgb-sunglasses/rulesets`), so all CI is advisory and skipping this skill
bypasses every check. Never "just push" a firmware PR.

## 1. Verify branch state

```bash
git status -sb
git log --oneline origin/main..HEAD
```

- If working tree is dirty (uncommitted changes), stop and tell the user.
- If there are no commits ahead of `main`, stop — nothing to PR.
- If still on `main`: a `PreToolUse` hook (`.claude/hooks/destructive-guard.sh`) denies
  `git commit` there — create a feature branch *first* (root CLAUDE.md "Git workflow").

## 2. Build proto0, build DK, and run tests — in parallel

These three are independent (separate build dirs — `fw/build`, `fw/build-dk`,
`fw/twister-out`; Twister's `native_sim` target uses the host toolchain, not the
nRF cross-compiler), so launch all three as **concurrent background tasks**:

```bash
# Proto0 (same as /build-proto0)
west build --build-dir fw/build fw --board rgb_sunglasses_proto0/nrf5340/cpuapp --sysbuild -- -DBOARD_ROOT="$(pwd)/fw"

# DK (same as /build-dk)
west build --build-dir fw/build-dk fw --board rgb_sunglasses_dk/nrf5340/cpuapp --sysbuild -- -DBOARD_ROOT="$(pwd)/fw"

# Tests + coverage
twister -T fw/tests -p native_sim --coverage --coverage-tool lcov --outdir fw/twister-out
```

Start all three before waiting on any; wait for all three, then evaluate every gate
even if an earlier one failed — one report should cover everything wrong. Capture each
build's `Memory region ... %age Used` summary (FLASH/RAM) — the PR body needs it (step 7).

**Gates** (evaluate all three, report every failure found — not just the first):
- proto0 build failed → stop. Do not proceed to later steps. Report the error clearly.
- DK build failed → stop. Report the error, especially if it's a flash
  overflow (the DK has a tight budget).
- Any Twister test failed or errored → stop. List the failing suites.

## 3. If the diff touches `app/**`: run the /validate-app trio

```bash
git diff --name-only origin/main...HEAD -- 'app/**'
```

If that prints anything, CI alone cannot clear it — app CI runs **only jest**, never
`tsc` or lint — so run all three device-free checks (details + `npm ci` precondition
for fresh worktrees: `/validate-app`):

```bash
cd app && npm test -- --ci
cd app && npx tsc --noEmit      # no "typecheck" npm script exists — use this exact form
cd app && npm run lint
```
**Gate**: any of the three failing → stop; report all failures found.

## 4. Check patch coverage ≥ 50%

```bash
git diff --name-only origin/main...HEAD -- 'fw/src/**' | grep -E '\.(c|cpp)$'
```

If that prints nothing (header-only or non-source changes), skip this step. Otherwise run
the pipeline in [references/patch-coverage.md](references/patch-coverage.md) against step
2's `fw/twister-out/coverage.info` and read the `lines......:` percentage. **Gates**:
- Below 50% → stop. Report which files are under-covered and do not submit the PR;
  the user must add tests before proceeding.
- Empty extracted tracefile → the changed files are not compiled into any test at
  all: that counts as **0% coverage**, stop and report. The fix is a new suite
  covering those files: `/add-fw-test`. Waiving this gate requires *explicit user
  approval* AND a follow-up issue tracking the missing tests, recorded in the PR body
  (precedent: PR #82's override, tracked by issue #83) — never waive silently.

## 5. On-device + companion-app verification (REQUIRED for anything touching device↔app communication)

Determine whether the branch could **conceivably** affect the companion app or device↔app communication. Err on the side of yes. Triggers include (not exhaustive):

- Anything under `fw/src/bluetooth/`, `fw/src/extensions/`, or any `*_bt.cpp` adapter
- GATT surface changes: services, characteristics, descriptors (CUD/CPF/CCC), UUIDs, the metadata blob, notification behavior, write/read handler semantics
- Animation registration, animation IDs / `Animation` enum, is-active plumbing, persisted last-active
- MCUmgr / firmware-update flow, advertising/pairing/connection parameters, MTU or connection tuning
- Value encodings the app decodes (colors, strings, dropdowns, bool wire sizes)

If none apply (e.g. pure internal refactor, audio DSP math, docs), note "device/app verification: N/A — <reason>" in the PR body and skip to step 6.

If any apply, **build-and-test gates alone are not sufficient** — flash the board and
verify against the real companion app over BLE, following
[references/device-verification.md](references/device-verification.md) exactly: hold
BOTH `board` and `app` hw-locks up front (one combined `hold` via `Monitor`; never
flash or drive the phone without them), exercise every changed read/write/notify path
end-to-end cross-checked against the firmware serial shell, verify notify reception,
and always release both locks when the step finishes — pass, fail, or waived. The
reference also records *why* this gate exists (PR #89: a dead Is Active mirror that
every build, test, and shell check missed).

**Bugs found here get fixed in the SAME PR and documented in its body** — the house
norm (PRs #43 and #55 each shipped extra fixes surfaced only by this step). After
fixing, re-run the affected gates above (builds/tests always; coverage if C/C++ changed).

**Gate**: if no board or phone is available, use AskUserQuestion to ask whether to
proceed without this verification — never silently skip it. A waiver needs the user's
explicit approval plus a follow-up issue, recorded in the PR body. Record what was
verified (or why it was waived) either way.

## 6. Review the diff before pushing

Run the `fw-code-reviewer` agent (`.claude/agents/fw-code-reviewer.md`, via the Task
tool) on the branch's `fw/` diff — it checks the repo owner's documented review
standards. Fix confirmed findings and re-run affected gates. Not a hard gate, but
pushing an unreviewed firmware diff wastes a review round-trip.

## 7. Push and create PR

All gates passed. Push the branch and open a PR:

```bash
git push -u origin HEAD
```

Draft the title (≤ 70 chars) and body in the house style — full template, worked
example, and waiver norms in
[references/pr-body-style.md](references/pr-body-style.md). In short: `## Summary` is
a root-cause → fix narrative with **measured numbers** (FLASH/RAM deltas, timings);
`## Pre-PR checks` lists proto0 build PASS w/ FLASH+RAM %, DK build PASS w/ FLASH %
(no overflow), Twister pass count, patch coverage %, and device/app verification
**including an honest statement of what was NOT exercised**.

```bash
gh pr create --title "<title>" --body "$(cat <<'EOF'
<body per references/pr-body-style.md>
EOF
)"
```

Report the PR URL to the user. For responding to review comments later (the pending-
review 422 trap and its workaround), see root CLAUDE.md "GitHub PR review comments via
`gh api`".
