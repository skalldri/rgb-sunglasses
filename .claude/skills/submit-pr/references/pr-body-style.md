# House PR body style

Distilled from this repo's merged PRs (see e.g. #43, #55, #82, #106 for the pattern).
Referenced from `/submit-pr` step 7.

## Template

```markdown
## Summary
- <Root cause → fix narrative: what was broken/missing, WHY (the mechanism), and what
  the change does about it. One bullet per logical change.>
- <Measured numbers wherever the change claims an effect: FLASH/RAM before → after %,
  µs timings, byte counts. "Reduces FLASH from 94.6% to 64.64%" — never "reduces FLASH".>
- <Bugs found during device/app verification and fixed in this same PR, each with its
  own bullet explaining the mechanism (house norm — precedent PRs #43, #55).>

Fixes #<issue> <!-- if applicable -->

## Pre-PR checks
- proto0 build: PASS (FLASH <X>%, RAM <Y>%)
- Twister tests: PASS (<passed>/<total> tests, <N> configurations)
- Patch coverage: PASS (<X>% on changed files) <!-- or the waiver line below -->
- Device/app verification: <what was exercised on real hardware + app, step by step>
  - **Not exercised**: <explicit honest list of paths/conditions NOT tested and why>


🤖 Generated with [Claude Code](https://claude.com/claude-code)
```

Numbers in the checks line come from the step-2 build output (`Memory region ...
%age Used`) and the Twister summary. Test counts grow over time (34 configurations as
of 2026-07) — always report what this run actually printed, never a remembered figure.

## The "not exercised" caveat is mandatory, not optional polish

Every device/app verification statement includes an explicit, honest list of what was
NOT exercised (e.g. "not exercised: OTA update path, bonded-reconnect behavior"). A
verification claim without its boundary reads as a claim of *total* verification —
which PR #89 proved nobody can honestly make. If verification was waived entirely,
say so and link the follow-up issue.

## Waivers (coverage gate or device verification)

A gate is waived only with **explicit user approval** AND a **follow-up issue**
tracking the debt, both recorded in the PR body. Precedent: PR #82 shipped with the
coverage gate overridden, tracked by issue #83 ("No native_sim test coverage for
imu.cpp and sound.cpp"). Silent waivers are review-blocking.

## Working with the PR after creation

- `gh pr view <n>` shows the **body only**; `gh pr view <n> --comments` shows the
  **comments only** — run both when reading a PR's full discussion.
- Responding to inline review comments, and the 422 "one pending review" trap: root
  CLAUDE.md, section "GitHub PR review comments via `gh api`". Do not restate it here —
  that section is the source of truth.
