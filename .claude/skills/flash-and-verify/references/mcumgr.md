# MCUmgr

`mcumgr` is installed in the devcontainer (built from source during image build) and on the Mac Mini
(via `scripts/macos-setup.sh`). The MCUmgr port is USB interface x.2 on Linux (`/dev/cu.usbmodem*`
with the higher suffix on macOS) — run `/check-hardware` to find the current port; it shifts after
resets (`serial-shell.md` "ttyACM node numbering shifts"). Holding the `board` lock is required (the
hook denies `mcumgr` without it).

```bash
# Run /check-hardware first to identify the current MCUmgr port (may be ttyACM1, ttyACM2, etc.)
CONN="--conntype serial --connstring dev=/dev/ttyACM2,baud=115200"  # example — verify first

mcumgr $CONN image list       # list firmware images
mcumgr $CONN echo "hello"     # connectivity check (the port that answers is the MCUmgr one)
mcumgr $CONN reset            # soft-reset the device
mcumgr $CONN taskstat         # list all threads with stack/runtime info
```

If mcumgr times out after a reset, find the port by trying each one:

```bash
for p in /dev/ttyACM*; do
    echo -n "$p: "
    mcumgr --conntype serial --connstring dev=$p,baud=115200 echo ping 2>&1 | head -1
done
```

- `taskstat` requires `CONFIG_THREAD_MONITOR=y`, `CONFIG_MCUMGR_GRP_OS_TASKSTAT=y`, and a
  large-enough TX FIFO on the CDC-ACM mcumgr port (the `hw-flow-control` note in
  `.claude/rules/fw-storage-usb.md`). All three are set on proto0.
- There is no `stat` group (`MCUMGR_GRP_STAT` is deliberately not enabled, and
  `CONFIG_FLASH_SIMULATOR_STATS=n`).
- `shell exec` returns status=8 (ENOTSUP) — the Zephyr shell is on the other CDC port, not the
  MCUmgr transport.
- The FS group (8) is fenced to `/NAND:/ext`, plus this firmware's FILE_MGMT group (64):
  `.claude/rules/extension-file-management.md`.

## Image layout

The board exposes two images via MCUmgr (confirmed from `image list`):

| image | Slot | Core |
| ----- | ---- | ---- |
| 0 | 0 | App core (rgb-sunglasses) |
| 1 | 0 | Net core (ipc_radio) |

Versions: a dev build reports the in-repo `fw/VERSION` (`0.0.0-stable`, so `0.0.0`); release builds
are stamped from the `fw-vX.Y.Z` tag by CI (`.github/workflows/release.yaml`). The net core
(`ipc_radio`) has no VERSION file of its own.

## Firmware update flow (OTA via MCUmgr)

**Prefer the wrapper `fw/scripts/mcumgr-flash.sh`** — it auto-detects the MCUmgr port, uploads the
app + net-core images (from `dfu_application.zip`), parses the slot hashes, tests, resets, and
confirms. Default (`--app`) uses the running app's SMP server; `--recovery` targets MCUboot
serial-recovery mode (hold the Left button/P1.11 at reset) for a board that won't boot the app. It
requires the `board` lock **only when run by an agent** (`CLAUDECODE` set), so human end-users can
run it lock-free. Human-facing runbook: `fw/docs/flashing-without-jlink.md` (published at
<https://rgb-sunglasses.autom8ed.com/recovery>).

Under the hood: `image upload` the signed image (`fw/build/fw/zephyr/zephyr.signed.bin`, ~3-4 min over
serial), `image test <hash>`, `reset`, and check the result. The bootloader is **overwrite-only**
(`.claude/rules/fw-sysbuild-mcuboot.md`): the tested image is installed on that reboot and there is no
revert, so a later `image confirm` is bookkeeping, not a safety net. The step-by-step procedure with
re-enumeration handling is `/flash-and-verify` §6; the app-driven OTA is `/ota-via-app`. Prefer the
J-Link fast path (`jlink.md`) when a J-Link is attached — but a staged OTA overwrites a later J-Link
flash (see there).
