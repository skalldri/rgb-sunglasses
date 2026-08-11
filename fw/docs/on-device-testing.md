# On-device (HIL) testing — architecture

How the on-device integration suite (issue #333) works, why each piece
exists, and how each consumer runs it. The test-author-facing rules live in
`fw/tests_device/README.md`; the app+device E2E plan is
`fw/docs/e2e-test-plan.md`.

## The stack

```
run-device-tests.sh          entry point: lock gate, USB fixup, J-Link SN probe,
  |                          hardware-map generation, tier -> marker mapping
  v
twister --device-testing     builds fw/ as a sysbuild app (REAL production
  -T fw -s app.device.*      image: MCUboot + app + netcore), flashes it via
  --hardware-map ... \
  --west-flash               `west flash --runner jlink --dev-id <SN>`
  |
  v
pytest (harness: pytest)     fw/tests_device/ — pytest-twister-harness gives
  |                          dut/shell fixtures; our conftest adds RgbShell,
  |                          device_state, the SMP-port mcumgr override
  v
tty-bridge.py (serial_pty)   PTY bridge that re-resolves the board's shell CDC
                             port by USB identity across re-enumerations
```

Key mechanics, each load-bearing:

- **`sysbuild: true` in fw/testcase.yaml** makes twister build this exact
  application (fw/ is the twister test app), not a synthetic test binary — so
  what's tested is what ships. `--west-flash` is mandatory: without it a
  sysbuild device test is silently SKIPPED by twister, not failed.
- **Out-of-tree board**: twister needs BOTH `-A fw/boards` (platform
  discovery) and `-x=BOARD_ROOT=<repo>/fw` (CMake). run-device-tests.sh
  passes both.
- **`fixture: rgbsg_proto0`** in every scenario means the suite is inert on
  any machine whose hardware map doesn't declare that fixture — CI's
  `twister -T fw/tests -p native_sim` never even scans fw/testcase.yaml.
- **`serial_pty` + tty-bridge.py** solves the soft-USB problem: the board's
  console is a CDC-ACM function served by the nRF5340 itself, so it drops off
  the bus during every flash/reset and returns under a different ttyACM minor
  (no udev in the devcontainer to stabilize names). Twister/pytest hold a PTY
  that never dies; the bridge re-resolves the real port underneath (sysfs, by
  VID 2fe3/PID 0001/interface 00, mknod'ing missing nodes) and buffers writes
  across the gap. Bridge diagnostics go to `$RGBSG_BRIDGE_LOG` only — its
  stdout/stderr ARE the device stream.
- **Test-image divergence from production is exactly two configs**
  (fw/testcase.yaml `extra_configs`): `CONFIG_SHELL_VT100_COLORS=n` (the
  pytest harness reads the console raw, no ANSI stripping) and
  `CONFIG_APP_CRASH_TEST_COMMANDS=y` (adds `crash panic|mpu` + `fatfs corrupt`
  for the destructive tier; adds commands only). Identical `extra_configs`
  across all scenarios keeps one build reusable for every tier.
- **MCUmgr port override**: pytest-twister-harness's stock `mcumgr` fixture
  targets the SHELL serial path; this board's SMP server is a separate CDC
  function (interface 02). `fw/tests_device/conftest.py` overrides the
  fixture to resolve it by USB identity.

## Consumers

| Consumer | Flow |
| --- | --- |
| Human at the bench | `fw/scripts/run-device-tests.sh [--tier ...]` — no lock, script does USB fixup + map generation |
| AI agent | `Monitor(hold board)` first; same command (script is lock-check-only, like jlink-flash.sh / provision-device.sh) |
| CI (north star) | see below |

Fast iteration while authoring tests: `--standalone` bypasses twister and
runs pytest directly against the existing `fw/build` image (flashed once per
session, `--dut-scope=session`), so the loop is seconds, not minutes.
`--test-only` re-runs twister against its existing build artifacts.

## DFU tier policy (decided at design time)

Proto0 is `SB_CONFIG_MCUBOOT_MODE_OVERWRITE_ONLY=y` — there is **no revert
path**. The dfu tier is therefore forward-only (upload a tweak-bumped image
built from the same source → `image test` → reset → verify boot →
`image confirm`), and it may run unattended: on failure the J-Link is
attached and a reflash recovers the board. The **bootloader** update path
(`mcuboot_update commit`) is NOT part of any automated tier — it stays
human-supervised (E2E-03 in the E2E plan).

## GitHub Actions self-hosted runner (north star — sketch only, no workflow yet)

When a Linux machine with a permanently-attached proto0 exists:

- Runner labels `[self-hosted, linux, proto0]`.
- Triggers: `push` to main + `workflow_dispatch` + nightly cron. **Never
  `pull_request`** — this repo is public, and a fork PR executes the fork's
  workflow copy on the runner (same doctrine as the Mac Mini iOS runner; see
  `app/README.md`'s runner-security section).
- One global concurrency group (a single physical board),
  `cancel-in-progress: false` — a superseded firmware build is still a valid
  regression run.
- The job coexists with interactive agents via the same lock everything else
  uses: `HW_LOCK_HOLDER_ID="gha-$GITHUB_RUN_ID" scripts/hw-lock.sh hold board
  --wait 1800 &` + a trap that releases on exit. (The lock scripts' CLAUDECODE
  gate doesn't fire in CI — holding is the job's own responsibility.)
- Full rebuild on the runner (no artifact pull): the dfu tier re-signs images
  at a bumped version, which must match the checked-out source.
- Upload `fw/twister-device-out/` (twister.json, handler.log, pytest report)
  as the run artifact.

## Relationship to the native_sim suite

Nothing here replaces `/test-fw` (60 native_sim suites, seconds-fast, run in
CI on every PR). The on-device suite covers only what native_sim structurally
cannot: real USB/CDC, FAT on real NAND, NVS on real flash, the extension
sandbox's MPU/K_USER behavior, coredump capture, MCUboot/DFU, real I2C to the
TPS25750/BQ25792, and timing/stack behavior under real load. When a check can
run on native_sim (command parsing, pure logic), it belongs there — see
`/add-fw-test`'s shell-backend-dummy pattern.
