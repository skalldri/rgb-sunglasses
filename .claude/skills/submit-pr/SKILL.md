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

## 2. Build proto0

Run the exact build from `/build-proto0`:

```bash
west build \
  --build-dir fw/build \
  fw \
  --board rgb_sunglasses_proto0/nrf5340/cpuapp \
  --sysbuild \
  -- -DBOARD_ROOT="$(pwd)/fw"
```

**Gate**: if this fails, stop. Do not proceed to the DK build or any subsequent step. Report the error clearly.

---

## 3. Build DK

Run the exact build from `/build-dk`:

```bash
west build \
  --build-dir fw/build-dk \
  fw \
  --board rgb_sunglasses_dk/nrf5340/cpuapp \
  --sysbuild \
  -- -DBOARD_ROOT="$(pwd)/fw"
```

**Gate**: if this fails, stop. Report the error, especially if it is a flash overflow (the DK has a tight budget).

---

## 4. Run tests and collect coverage

```bash
twister \
  -T fw/tests \
  -p native_sim \
  --coverage \
  --coverage-tool lcov \
  --outdir fw/twister-out
```

**Gate**: if any test fails or errors, stop. List the failing suites and do not proceed.

---

## 5. Check patch coverage ≥ 50%

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
  --extract fw/twister-out/coverage/coverage.info \
  $PATTERNS \
  --output-file /tmp/patch-coverage.info

lcov --summary /tmp/patch-coverage.info
```

Parse the `lines......:` percentage from the summary. **Gate**: if it is below 50%, stop. Report which files are under-covered and do not submit the PR. The user must add tests before proceeding.

If the lcov extract produces an empty tracefile (i.e., the changed files are not compiled in tests), that counts as 0% coverage — stop and report.

---

## 6. On-device + companion-app verification (REQUIRED for anything touching device↔app communication)

Determine whether the branch could **conceivably** affect the companion app or device↔app communication. Err on the side of yes. Triggers include (not exhaustive):

- Anything under `fw/src/bluetooth/`, `fw/src/extensions/`, or any `*_bt.cpp` adapter
- GATT surface changes: services, characteristics, descriptors (CUD/CPF/CCC), UUIDs, the metadata blob, notification behavior, write/read handler semantics
- Animation registration, animation IDs / `Animation` enum, is-active plumbing, persisted last-active
- MCUmgr / firmware-update flow, advertising/pairing/connection parameters, MTU or connection tuning
- Value encodings the app decodes (colors, strings, dropdowns, bool wire sizes)

If none apply (e.g. pure internal refactor, audio DSP math, docs), note "device/app verification: N/A — <reason>" in the PR body and skip to step 7.

If any apply, **build-and-test gates alone are not sufficient** — flash the board (`fw/scripts/jlink-flash.sh`) and verify against the real companion app over BLE:

Acquire both hardware locks up front, together, before doing anything below:

```bash
scripts/hw-lock.sh acquire board app
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
passed, failed, or was waived after acquiring:

```bash
scripts/hw-lock.sh release board app
```

---

## 7. Push and create PR

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
