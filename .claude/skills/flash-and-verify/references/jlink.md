# Flashing via J-Link (fast path)

When `/check-hardware` reports the J-Link `Status: OK`, prefer flashing over it instead of the slow
MCUmgr/UART upload path (`mcumgr.md`). Linux devcontainer only — macOS has no SEGGER tooling
(`macos-host.md`).

`jlink-flash.sh` refuses to run unless this session holds the `board` hardware lock (root
`CLAUDE.md` "Hardware locking"). If you're iterating (build → flash → check over `mcp__serial__*` →
adjust → rebuild → reflash), hold the lock across the whole cycle rather than releasing between
passes.

```bash
fw/scripts/jlink-flash.sh                  # uses fw/build by default
fw/scripts/jlink-flash.sh /path/to/build    # explicit build dir
fw/scripts/jlink-flash.sh --recover         # unlock + mass-erase first (memory/protection error);
                                            # needs the BATTERY connected — see the script header
fw/scripts/jlink-flash.sh -- --skip-rebuild # extra args forwarded to `west flash`
```

`jlink-flash.sh` auto-detects the attached J-Link's serial number and runs
`west flash -d <build-dir> --dev-id <serial>` — no need to look up `--dev-id` yourself.
`/check-hardware` also prints the serial under the J-Link section (`Serial: ...`).

- This triggers a `west build` rebuild-check first (fast no-op if nothing changed), then flashes via
  the **`nrfutil` runner** (not raw `JLinkExe`) — it programs both `merged_CPUNET.hex` (netcore) and
  `merged.hex` (appcore), each with erase → program → verify → reset.
- **A STAGED MCUmgr OTA SILENTLY REVERTS ANY J-LINK FLASH ON THE NEXT BOOT** (observed 2026-08-11,
  PR #341 debugging): a J-Link flash writes slot 0, but a pending `image test` image sitting in slot 1
  makes MCUboot overwrite slot 0 with it on the very next boot — the flash "succeeds", verifies,
  resets, and ~40 s later (the slot-copy time) the board is running the OTHER image, with zero errors
  anywhere. The shared board can carry a staged OTA from another agent's session, and right after an
  app-driven OTA the first J-Link reflash likewise boots the OTA'd image. After any J-Link flash,
  verify what's actually running (`mcumgr image list`: active slot hash, and no `pending` flags; the
  boot banner's version) — the on-device suite's `no_staged_ota` fixture in `fw/tests_device/conftest.py`
  automates this for HIL runs. If a pending image is present, `mcumgr image confirm`/erase it (or
  coordinate with whoever staged it) before trusting any flash.
- Typical total time: ~30-45 s, plus ~15 s for USB re-enumeration. Re-run `/check-hardware` to
  confirm both ttyACM ports are back before further serial/mcumgr commands.
- This is the only way to reflash the bootloaders (MCUboot/b0n); MCUmgr can only update the
  application images (MCUboot also has the `mcuboot_update` sideload/commit shell path).
- **The default build dir is resolved relative to the script's own location, not the caller's cwd
  or the main checkout** (`REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"`), so
  `fw/scripts/jlink-flash.sh` with no arguments correctly uses *this* worktree's `fw/build`.
- `JLinkExe -CommandFile` only opens the USB connection lazily, on the first command that needs it —
  a command file containing just `Exit` never touches USB. The probe (and `jlink-flash.sh`) use
  `ShowHWStatus` to force the connect; that banner is also where the `S/N:` serial comes from.

## J-Link "Cannot connect" / nrfutil "Failed to open connection": run fix-usb-dev-nodes.sh

Almost always a missing (or bogus 0-byte regular-file) `/dev/bus/usb` node after re-enumeration —
the devcontainer has no udev. **Run `fw/scripts/fix-usb-dev-nodes.sh` before every J-Link flash
attempt and again after the board re-enumerates**; a failed flash → fix → retry cycle converging on
the second attempt is normal. The full symptom table — including the distinct APPROTECT/debug-port
lockout and its `nrfutil device recover` procedure — lives in `/debug-fw`.

A wedged shell UART is recovered with an SWD reset, not a reflash: `serial-shell.md`.
