---
description: Pre-PR validation gate for firmware — builds both boards, runs tests, checks patch coverage ≥ 50%, then creates the GitHub PR
allowed-tools: Bash, Read, AskUserQuestion
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
  --build-dir /workspaces/rgb-sunglasses/fw/build \
  /workspaces/rgb-sunglasses/fw \
  --board rgb_sunglasses_proto0/nrf5340/cpuapp \
  --sysbuild \
  -- -DBOARD_ROOT="/workspaces/rgb-sunglasses/fw"
```

**Gate**: if this fails, stop. Do not proceed to the DK build or any subsequent step. Report the error clearly.

---

## 3. Build DK

Run the exact build from `/build-dk`:

```bash
west build \
  --build-dir /workspaces/rgb-sunglasses/fw/build-dk \
  /workspaces/rgb-sunglasses/fw \
  --board rgb_sunglasses_dk/nrf5340/cpuapp \
  --sysbuild \
  -- -DBOARD_ROOT="/workspaces/rgb-sunglasses/fw"
```

**Gate**: if this fails, stop. Report the error, especially if it is a flash overflow (the DK has a tight budget).

---

## 4. Run tests and collect coverage

```bash
twister \
  -T /workspaces/rgb-sunglasses/fw/tests \
  -p native_sim \
  --coverage \
  --coverage-tool lcov \
  --outdir /workspaces/rgb-sunglasses/fw/twister-out
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
  --extract /workspaces/rgb-sunglasses/fw/twister-out/coverage/coverage.info \
  $PATTERNS \
  --output-file /tmp/patch-coverage.info

lcov --summary /tmp/patch-coverage.info
```

Parse the `lines......:` percentage from the summary. **Gate**: if it is below 50%, stop. Report which files are under-covered and do not submit the PR. The user must add tests before proceeding.

If the lcov extract produces an empty tracefile (i.e., the changed files are not compiled in tests), that counts as 0% coverage — stop and report.

---

## 6. Push and create PR

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

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

Report the PR URL to the user.
