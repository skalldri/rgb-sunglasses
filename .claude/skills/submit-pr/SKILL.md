---
name: submit-pr
description: Pre-PR validation gate for firmware — builds proto0, runs tests, checks changed-line (patch) coverage > 70%, requires on-device + companion-app verification for any change touching device↔app communication, then creates the GitHub PR
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

## 2. Build proto0 and run tests — in parallel

These two are independent (separate build dirs — `fw/build`, `fw/twister-out`;
Twister's `native_sim` target uses the host toolchain, not the nRF
cross-compiler), so launch both as **concurrent background tasks**:

```bash
# Proto0 (same as /build-proto0)
west build --build-dir fw/build fw --board rgb_sunglasses_proto0/nrf5340/cpuapp --sysbuild -- -DBOARD_ROOT="$(pwd)/fw"

# Tests + coverage
twister -T fw/tests -p native_sim --coverage --coverage-tool lcov --outdir fw/twister-out
```

Start both before waiting on either; wait for both, then evaluate every gate
even if an earlier one failed — one report should cover everything wrong.

(The legacy DK board is no longer built on main — its board support and CI live
on the `dk-support` branch, issue #203.)

**Gates** (evaluate both, report every failure found — not just the first):
- proto0 build failed → stop. Do not proceed to later steps. Report the error clearly.
- Any Twister test failed or errored → stop. List the failing suites.
- Record proto0 FLASH/RAM `%age Used` (the `Memory region` summary in the
  build log) — required fields in the PR body (step 7).

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

## 4. Check patch coverage > 70% (changed lines, not whole files)

```bash
git diff --name-only origin/main...HEAD -- 'fw/src/**' | grep -E '\.(c|cpp)$'
```

If that prints nothing (header-only or non-source changes), skip this step. Otherwise run
the pipeline in [references/patch-coverage.md](references/patch-coverage.md) against step
2's `fw/twister-out/coverage.info`. It measures **changed-line (patch) coverage** — the
fraction of the lines this branch *added/modified* that a test executes, the same metric
Codecov's `codecov/patch` check enforces — and **exits non-zero when it is ≤ 70%**.
Do NOT substitute whole-file `lcov --summary` coverage: it ignores which lines are new and
gives a false pass (a PR read 87.7% whole-file while Codecov's patch coverage was 40.7%
and failing). **Gates**:
- Script exits non-zero (patch coverage ≤ 70%) → stop. It prints the exact uncovered
  line numbers per file; add tests for those branches (`/add-fw-test`), re-run step 2, and
  re-check. Do not submit the PR until it passes.
- No executable added lines (`tot == 0`, script passes) is fine — the change is
  comments/headers/declarations only. But a **new source file not compiled into any test**
  shows up as all-lines-missing = 0% and fails. The fix is a suite covering it:
  `/add-fw-test`. Waiving this gate requires *explicit user approval* AND a follow-up issue
  tracking the missing tests, recorded in the PR body
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
[references/device-verification.md](references/device-verification.md) exactly: it
carries the full procedure (hold BOTH `board`+`app` hw-locks up front, exercise every
changed read/write/notify path cross-checked against the firmware serial shell, verify
notify reception, always release both locks) and *why* the gate exists (PR #89).

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

Draft the title (≤ 70 chars) and body in the house style — the full template
(`## Summary` with measured numbers, `## Pre-PR checks` fields, the mandatory
"**not exercised**" statement), worked example, and waiver norms are in
[references/pr-body-style.md](references/pr-body-style.md).

```bash
gh pr create --title "<title>" --body "$(cat <<'EOF'
<body per references/pr-body-style.md>
EOF
)"
```

Report the PR URL to the user. For responding to review comments later (the pending-
review 422 trap and its workaround), see root CLAUDE.md "GitHub PR review comments via
`gh api`".
