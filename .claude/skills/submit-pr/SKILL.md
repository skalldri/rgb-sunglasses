---
name: submit-pr
description: Pre-PR validation gate for firmware — builds both boards, runs tests, checks patch coverage ≥ 50%, requires on-device + companion-app verification for any change touching device↔app communication, then creates the GitHub PR
allowed-tools: Bash, Read, AskUserQuestion, mcp__serial, mcp__execbro
---

Run all pre-PR checks for firmware changes, then push and open a pull request. **Do not push or create a PR if any check fails.**

---

## 1. Verify branch state

```bash
git status -sb
git log --oneline origin/main..HEAD
```

- If working tree is dirty (uncommitted changes), stop and tell the user.
- If there are no commits ahead of `main`, stop — nothing to PR.

---

## 2. Build proto0, build DK, and run tests — in parallel

These three are independent of each other (separate build dirs — `fw/build`,
`fw/build-dk`, `fw/twister-out` — and Twister's `native_sim` target uses the
host toolchain, not the nRF cross-compiler used by the two board builds), so
launch all three as **concurrent background tasks** rather than running them
one after another and waiting on each in turn:

```bash
# Proto0 (same as /build-proto0)
west build --build-dir fw/build fw --board rgb_sunglasses_proto0/nrf5340/cpuapp --sysbuild -- -DBOARD_ROOT="$(pwd)/fw"

# DK (same as /build-dk)
west build --build-dir fw/build-dk fw --board rgb_sunglasses_dk/nrf5340/cpuapp --sysbuild -- -DBOARD_ROOT="$(pwd)/fw"

# Tests + coverage
twister -T fw/tests -p native_sim --coverage --coverage-tool lcov --outdir fw/twister-out
```

Start all three before waiting on any of them, then wait for all three to
finish before evaluating gates below — don't judge one gate while the others
are still running, and check every one of the three even if an earlier one
already failed, so a single report covers everything that's wrong rather than
surfacing failures one slow retry at a time.

**Gates** (evaluate all three, report every failure found — not just the first):
- proto0 build failed → stop. Do not proceed to patch coverage or device
  verification. Report the error clearly.
- DK build failed → stop. Report the error, especially if it's a flash
  overflow (the DK has a tight budget).
- Any Twister test failed or errored → stop. List the failing suites.

---

## 3. Check patch coverage ≥ 50%

Identify every C/C++ source file changed in this branch relative to `main`:

```bash
git diff --name-only origin/main...HEAD -- 'fw/src/**' | grep -E '\.(c|cpp)$'
```

If no C/C++ source files were changed, skip this step (header-only or non-source changes).

For each changed file, extract its coverage from the lcov report and aggregate:

```bash
# Build a space-separated list of lcov glob patterns for the changed files
PATTERNS=$(git diff --name-only origin/main...HEAD -- 'fw/src/**' \
  | grep -E '\.(c|cpp)$' \
  | sed 's|.*|"*/&"|' \
  | tr '\n' ' ')

# Extract and summarise
eval lcov \
  --extract fw/twister-out/coverage.info \
  $PATTERNS \
  --output-file /tmp/patch-coverage.info

lcov --summary /tmp/patch-coverage.info
```

Parse the `lines......:` percentage from the summary. **Gate**: if it is below 50%, stop. Report which files are under-covered and do not submit the PR. The user must add tests before proceeding.

If the lcov extract produces an empty tracefile (i.e., the changed files are not compiled in tests), that counts as 0% coverage — stop and report.

---

## 4. On-device + companion-app verification (REQUIRED for anything touching device↔app communication)

Determine whether the branch could **conceivably** affect the companion app or device↔app communication. Err on the side of yes. Triggers include (not exhaustive):

- Anything under `fw/src/bluetooth/`, `fw/src/extensions/`, or any `*_bt.cpp` adapter
- GATT surface changes: services, characteristics, descriptors (CUD/CPF/CCC), UUIDs, the metadata blob, notification behavior, write/read handler semantics
- Animation registration, animation IDs / `Animation` enum, is-active plumbing, persisted last-active
- MCUmgr / firmware-update flow, advertising/pairing/connection parameters, MTU or connection tuning
- Value encodings the app decodes (colors, strings, dropdowns, bool wire sizes)

If none apply (e.g. pure internal refactor, audio DSP math, docs), note "device/app verification: N/A — <reason>" in the PR body and skip to step 5.

If any apply, **build-and-test gates alone are not sufficient** — flash the board (`fw/scripts/jlink-flash.sh`) and verify against the real companion app over BLE:

Hold both hardware locks up front, together, before doing anything below —
`hold` is the only way to take a lock, launched via `Monitor`:

```
Monitor(command: "scripts/hw-lock.sh hold board app", description: "board+app hw-lock heartbeat for submit-pr verification", persistent: true)
```
```bash
timeout 15 bash -c 'until scripts/hw-lock.sh check board >/dev/null 2>&1 && scripts/hw-lock.sh check app >/dev/null 2>&1; do sleep 0.5; done'
```

If this fails, report who holds the conflicting resource(s) and stop — do not
fall back to flashing or driving the phone without it. (This is separate from
the "no board or phone available at all" case below — if hardware simply isn't
present, skip locking and go straight to the `AskUserQuestion` waiver.)

1. Connect the app (phone via ADB + execbro, or ask the user to drive their phone; read `app/CLAUDE.md` first for launch/tap/fiber procedures) and confirm discovery completes with no fallback/mismatch warnings.
2. Exercise every changed read/write/notify path end-to-end **and cross-check against the firmware's own source of truth** (the `mcp__serial__*` shell, e.g. `ext param`, `glim`), not just the app UI — optimistic updates make the UI lie (see app/CLAUDE.md "Verifying a write/notify round-trip").
3. If the change involves notifications, verify the app *receives* them (a value changes in the app without a re-read) — notify failures are firmware-log-only and completely silent app-side.

**Why this gate exists**: shell-level testing cannot see BLE-visible state. On PR #89 the extensions' Is Active mirror was completely dead (a registration-ordering bug returned `-ENOENT` into an ignored return value) while every build, Twister suite, and serial-shell check passed — only a real app connection exposed it, plus a second bug (a pushback notification losing a race against the app's optimistic update) that needed an ATT error instead.

**Gate**: if no board or phone is available, use AskUserQuestion to ask whether to proceed without this verification — never silently skip it. Record what was verified (or why it was waived) in the PR body.

**Always release both locks when this step finishes**, whether verification
passed, failed, or was waived after acquiring — stop the `hold` Monitor task
(`TaskStop`, which releases automatically via its own exit trap) or run:

```bash
scripts/hw-lock.sh release board app --force
```

---

## 5. Push and create PR

All gates passed. Push the branch and open a PR:

```bash
git push -u origin HEAD
```

Draft the PR title and body based on the commits:
- Title: concise summary of the change (≤ 70 chars)
- Body: bullet-point summary, link to relevant issues if any, note the build/test status

```bash
gh pr create --title "<title>" --body "$(cat <<'EOF'
## Summary
<bullets>

## Pre-PR checks
- proto0 build: PASS
- DK build: PASS
- Twister tests: PASS (<N> tests)
- Patch coverage: PASS (<X>% on changed files)
- Device/app verification: <what was verified on hardware + app, or "N/A — <reason>">


🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

Report the PR URL to the user.
