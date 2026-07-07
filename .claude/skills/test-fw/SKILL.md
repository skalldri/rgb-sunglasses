---
name: test-fw
description: Run the firmware Twister test suite on native_sim and collect lcov code coverage. Use when asked to run the firmware tests, iterate on a failing Twister suite or scenario, or check overall test coverage.
allowed-tools: Bash, Read
---

Run all firmware tests using Twister with lcov coverage. Output goes to `fw/twister-out`.

```bash
twister \
  -T fw/tests \
  -p native_sim \
  --coverage \
  --coverage-tool lcov \
  --outdir fw/twister-out
```

## Steps

1. Run the command above. Twister exits non-zero if any test fails.
2. Once complete, read `fw/twister-out/twister.log` (last 50 lines) for the PASSED/FAILED/ERROR summary.
3. Report: number of test cases passed, failed, and errored. List any failing test suites by name.
4. Report the overall line coverage percentage from:
   ```bash
   lcov --summary fw/twister-out/coverage.info
   ```
5. If any tests **failed**: do NOT proceed with a PR. Fix the failing test(s) and re-run.
6. The coverage report (`fw/twister-out/coverage.info`) is consumed by `/submit-pr` to check patch coverage.

## Targeted runs — iterate on one failure without rebuilding everything

The full run's cost is per-suite builds, not test execution. To iterate on a
single failing suite, point `-T` at that suite's directory (any path under
`fw/tests/`, e.g. `fw/tests/animations/zigzag_animation_di` or
`fw/tests/bt_state_observer`):

```bash
twister -T fw/tests/<area>/<name> -p native_sim --outdir fw/twister-out-one
```

To run a single scenario when a suite directory defines more than one, append
`-s` with the dotted scenario name from that suite's `testcase.yaml`:

```bash
twister -T fw/tests/drivers/emul_bmi270 -p native_sim -s drivers.emul_bmi270.trigger --outdir fw/twister-out-one
```

**Use a scratch `--outdir` (as above) for targeted runs.** Twister's default
clean policy *renames* an existing outdir (to `twister-out.1`, `.2`, …; `-c`
deletes it instead) — reusing `fw/twister-out` for a targeted run would
displace the full-run `coverage.info` that `/submit-pr` needs. After the fix,
finish with the full run into `fw/twister-out` so coverage is complete.

## Failure triage

Per-scenario artifacts live at
`fw/twister-out/native_sim_native/host/<path-relative-to-fw/tests>/<dotted.scenario>/`
(e.g. `.../host/animations/zigzag_animation_di/animations.zigzag_animation_di/`):

- **`build.log`** — compile/link output. Read this when twister reports a
  build error (status ERROR / "CMake build failure").
- **`handler.log`** — runtime ztest output (assert messages, panics). Read
  this when the test built but FAILED.
- **`zephyr/zephyr.exe`** — the native_sim binary; it can be re-executed
  directly or under gdb. See `/debug-fw` for that workflow.

## Coverage: what this skill does and does NOT check

- This skill reports **overall** line coverage only
  (`lcov --summary fw/twister-out/coverage.info`).
- The **≥ 50% patch coverage gate** is a separate check computed by
  `/submit-pr`, which extracts the changed files from
  `fw/twister-out/coverage.info`. **Passing `/test-fw` alone does not satisfy
  the gate** — a green run with low coverage on your changed files will still
  fail `/submit-pr`.
- Always feed the patch extract from **this checkout's** `fw/twister-out/coverage.info`:
  - Never point it at `fw/twister-out-ci` — that is CI's outdir
    (`.github/workflows/build.yaml`), which writes coverage under
    `fw/twister-out-ci/coverage/`, a different layout.
  - Never point it at another checkout's/worktree's `twister-out` — it
    reflects different sources.

For **writing or fixing** a test suite (prj.conf / testcase.yaml / CMakeLists
incantations), use `/add-fw-test`, not this skill.
