# On-device (HIL) integration test suite

Programmatic pass/fail tests that run against the **real production firmware
image** flashed onto a physical proto0 board, driving its Zephyr shell over
USB CDC. Issue #333's first deliverable. The companion deliverable — the
AI-agent-driven app+device E2E plan — is `fw/docs/e2e-test-plan.md`; the
architecture doc is `fw/docs/on-device-testing.md`.

## How to run

```bash
# Everything non-destructive (smoke + integration), ~10 min including flash:
fw/scripts/run-device-tests.sh

# Just the 1-minute read-only sanity pass:
fw/scripts/run-device-tests.sh --tier smoke

# Fast inner loop while writing a test: reuses the LAST TWISTER DEVICE BUILD
# (flashes it once per session, then just runs pytest). Not fw/build — that
# image has VT100 on, which the harness cannot parse; the script fails fast
# if pointed at one:
fw/scripts/run-device-tests.sh --standalone -k test_ibat

# Build the twister image without touching hardware (works lock-free):
fw/scripts/run-device-tests.sh --build-only
```

**Agents**: hold the `board` hw-lock for the WHOLE run
(`Monitor(command: "scripts/hw-lock.sh hold board", persistent: true)`), same
rules as `/flash-and-verify`. The script is check-only — it refuses without
the lock (when `CLAUDECODE` is set) and never acquires it itself.

**Humans**: no lock needed on a solo bench; the script does everything
(USB-node fixup, J-Link discovery, hardware-map generation).

## Tiers (pytest markers)

| Marker | Contract | Default? |
| --- | --- | --- |
| `smoke` | read-only, no reboots, ~1 min | yes |
| `integration` | may reboot; restores every setting it touches; never loses data | yes |
| `destructive` | wipes settings/FAT, forces crashes; ends with reprovision | explicit `--tier destructive` |
| `dfu` | MCUmgr firmware-update loop | explicit `--tier dfu` |
| `soak` | minutes-to-hours drift/timing assertions | explicit `--tier soak` |

Destructive tests run LAST within a run and their teardown reprovisions the
board (fw/scripts/provision-device.sh) — the board must always be left usable.

## House rules

- **Assert `retval`, not prose.** `RgbShell.exec()` runs `retval` after every
  command and fails on non-zero. Parse output only for values, and keep every
  parser in `helpers/rgb_shell.py` — one place to fix when a format changes.
- **Power tests are configuration-aware, never skip-happy.** The CI rig may
  have no battery. Every power test runs in ANY power setup and branches its
  assertions on `device_state` (probed once from `power bq limits` /
  `power policy`); the run log banner states the detected configuration.
  `requires_*` markers are reserved for tests with NO meaningful assertion
  outside one configuration (e.g. watchdog retention needs actual charging).
- **No single-shot equality against a moving reconciler.** Anything the
  firmware converges toward on its own tick (charger policy, PD contract,
  conn params) is asserted by polling to convergence with a deadline.
- **`requires_provisioned` FAILS, `requires_*` power markers SKIP.**
  Provisioning is a fixable setup error; power topology is a legitimate rig
  difference. Silent skips on CI would hollow out coverage.
- **NEVER run from a test, under any tier**: `power boost` (irreversible UICR
  write), `mcuboot_update commit` (bootloader flash), `power pd go2p`,
  `power bq hiz enable` (kills a battery-less board), any `power bq`/`power pd`
  register write. Read-only `power` commands are fine.
- **Extension fault injectors**: hello's params 2/3 are Crash/Hang. Only the
  destructive tier may write them, and only on purpose.
- The suite REQUIRES `--dut-scope=session` (run-device-tests.sh and
  testcase.yaml both pass it): one flash + one boot per run, session-scoped
  fixtures.

## Why twister + pytest (and not bare scripts)

Zephyr's own device-testing stack (`twister --device-testing` +
pytest-twister-harness) already solves flashing via `west flash --runner
jlink`, serial capture, JUnit reporting, and fixture-gating on hardware
presence — and it's the same mechanism a future GitHub Actions self-hosted
runner invokes unchanged. The one local quirk is the board's soft-USB CDC
console (it re-enumerates mid-flash under a new ttyACM minor, and the
devcontainer has no udev): the hardware map therefore uses `serial_pty:` with
`fw/scripts/tty-bridge.py`, which re-resolves the port by USB identity
underneath a PTY that never dies.
