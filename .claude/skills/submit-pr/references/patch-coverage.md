# Patch-coverage extraction pipeline

Referenced from `/submit-pr` step 4. Computes line coverage over only the C/C++
files changed on this branch, using the Twister lcov report produced by step 2
(`fw/twister-out/coverage.info`). Run from the repo root.

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

Read the `lines......:` percentage from the `lcov --summary` output — that is
the patch-coverage number the ≥ 50% gate in SKILL.md is evaluated against.

If the extract produces an **empty tracefile** (lcov errors or the summary shows
no lines), none of the changed files are compiled into any test — SKILL.md
treats that as 0% coverage. The fix is `/add-fw-test`; the only alternative is a
waiver with explicit user approval plus a follow-up issue tracking the missing
tests, both recorded in the PR body (SKILL.md step 4; precedent: PR #82's
override, tracked by issue #83) — never waive silently.

To name which specific files are under-covered in the failure report:

```bash
lcov --list /tmp/patch-coverage.info
```
