---
name: test-fw
description: Run the firmware Twister test suite on native_sim and collect lcov code coverage
allowed-tools: Bash, Read
---

Run all firmware tests using Twister with lcov coverage. Output goes to `fw/twister-out`.

```bash
twister \
  -T /workspaces/rgb-sunglasses/fw/tests \
  -p native_sim \
  --coverage \
  --coverage-tool lcov \
  --outdir /workspaces/rgb-sunglasses/fw/twister-out
```

## Steps

1. Run the command above. Twister exits non-zero if any test fails.
2. Once complete, read `fw/twister-out/twister.log` (last 50 lines) for the PASSED/FAILED/ERROR summary.
3. Report: number of test cases passed, failed, and errored. List any failing test suites by name.
4. Report the overall line coverage percentage from:
   ```bash
   lcov --summary /workspaces/rgb-sunglasses/fw/twister-out/coverage/coverage.info
   ```
5. If any tests **failed**: do NOT proceed with a PR. Fix the failing test(s) and re-run.
6. The coverage report (`fw/twister-out/coverage/coverage.info`) is consumed by `/submit-pr` to check patch coverage.
