---
paths:
  - "fw/src/storage/**"
  - "fw/src/usb/**"
  - "fw/drivers/flashdisk/**"
  - "fw/pm_static_*.yml"
  - "fw/boards/**/*.dts"
  - "fw/boards/**/*.dtsi"
  - "fw/tests/storage/**"
  - "fw/tools/*glim*.py"
---

# Flash layout, the `/NAND:` disk, USB composite device

Loads when you read storage, USB, partition or devicetree files. Mounting the disk from a host,
copying assets and reformatting: `.claude/skills/provision-device/references/nand-disk.md`.

## Internal vs external flash

**The running firmware lives in INTERNAL flash. External flash holds assets and settings — not the
firmware image.** Stated because it is a recurring agent mistake (corrected again 2026-08-15).
Internal (1 MB): `mcuboot`, `app` (the image that actually executes), `coredump_partition`.
External (MX25R6435F, 8 MB): `settings_storage` NVS at 0x11C000 (BT bonds + persisted config) and
`fat_storage` at 0x124000 (the `/NAND:` volume — GLIM assets, `.llext` extensions, captures, drained
coredumps). The `mcuboot_secondary` / `mcuboot_secondary_1` regions at the bottom of external flash
are **OTA staging only**: an image sits there between upload and the next boot, then MCUboot moves
it into internal flash.

Consequences: a J-Link flash writes internal flash (+ the netcore image) and does **not** disturb
assets or bonds — reprovisioning after a plain reflash is unnecessary; conversely, erasing external
flash to "reset the firmware" destroys assets and pairing while leaving the running image untouched.
Authoritative layout: `fw/pm_static_rgb_sunglasses_proto0_nrf5340_cpuapp.yml` (the firmware confirms
it at boot — `flashdisk: offset 124000, … volume size 7192576`).

## `/NAND:` — the FAT volume and its USB exposure

The board exposes a ~6.9 MiB FAT filesystem over USB Mass Storage (SCSI Bulk-Only, interface 4 of
the composite USB device). Zephyr mounts it at `/NAND:` (`fw/src/storage/storage.cpp`; LUN
registered in `fw/src/usb/usb_init.c` as `USBD_DEFINE_MSC_LUN(nand, "NAND", "RGB-SG", "FlashDisk",
"0.00")`). `fatfs reformat` needs `CONFIG_FILE_SYSTEM_MKFS=y` (on for proto0). FatFs is built
`FF_FS_LOCK=0` (an unsynchronized unlink succeeds against an open file — see
`.claude/rules/extension-file-management.md`) and `FF_FS_TINY` (open FILs share one sector window).

**The flashdisk driver under `/NAND:` is a PATCHED IN-REPO COPY of the SDK's**
(`fw/drivers/flashdisk/flashdisk.c`, enabled by `CONFIG_DISK_DRIVER_FLASH_PATCHED` with the SDK's
`CONFIG_DISK_DRIVER_FLASH=n` — issue #380). NCS v3.1.1's copy swallows every disk-write error
(`disk_flash_access_write()` computes `rc` and then `return 0;`), so a failed QSPI erase/program
silently lost FatFS FAT/directory sectors — the root cause of the "file size exceeds cluster chain"
fsck corruption. The copy carries upstream zephyr commit `81db3fff8f` (the one-line fix) plus
failure instrumentation: per-disk error counters via the `flashdisk stats` shell command /
`flashdisk_stats.h`, and a LOG_ERR with op/address/errno on every underlying flash-op failure.
**Delete the whole directory and re-enable `CONFIG_DISK_DRIVER_FLASH` once NCS ships a Zephyr
containing that commit** — the `storage.fat_flashdisk_fault.sdk_tripwire` twister scenario asserts
the SDK bug is still present and will fail when that day comes; don't "fix" that scenario, act on
it. Keep the copy byte-diffable against upstream: no changes beyond the marked `/* PATCHED */` blocks.

**The proto0 BOM part is MX25R6435FZA`IH0`, and its `-H-` ordering option makes High Performance
mode the factory default** — the MX25R6435F *family* default is Ultra Low Power (5 of the 8
orderable variants are L-parts), so an L-variant re-BOM would invalidate the 32 MHz QSPI SCK and
must revisit it. The volatile L/H bit reverts to the ordering default at every power-on (datasheet
PDF p.77/p.31, checked in at `fw/docs/datasheets/MX25R6435F/`), and the BOM is the single point of
truth: L and H variants share the same JEDEC ID, and the `nordic,qspi-nor` driver exposes no public
API to read CR2 back (a ~10-line hand-rolled boot-time RDCR via `nrfx_qspi_cinstr_xfer` is possible
if variant detection is ever wanted — issue #387). No software mode switch is needed on this board,
and none is possible through this driver (it ignores `mxicy,mx25r-power-mode`; its WRSR helper
can't reach CR2) — don't reintroduce the "stuck in low-power mode" theory issue #380 briefly
carried. QSPI SCK runs at 32 MHz (HP mode allows 80; the old 8 MHz was the ULP ceiling — see the
mx25r64 DTS node comment).

**`storage` is a reserved macro in NCS** — `nrf/include/flash_map_pm.h` defines
`#define storage settings_storage` (conditionally). The `UTIL_CAT` macro inside `SHELL_CMD_REGISTER`
double-expands its arguments, so `SHELL_CMD_REGISTER(storage, ...)` silently registers a command
named `settings_storage`. Use `fatfs` (or any token not in `flash_map_pm.h`) for FAT-disk commands.

**GLIM format**: `fw/src/storage/GLIM_FORMAT.md` is the normative spec, with
`fw/src/storage/glim_decoder.{h,cpp}` the reference implementation; converters live in `fw/tools/`
and are gated by CI's `python-tests` job (run locally **in the devcontainer**, from the repo root:
`cd fw && pytest tools/tests/ -v` — the macOS tools venv deliberately carries only what the
converters import, not pytest or the `beat_lab` scipy/librosa stack). `fw/scripts/img_to_c.py` is a
broken stub (it never writes any output) — do not use it.

## CDC-ACM TX FIFO (why `hw-flow-control` matters)

The `zephyr,cdc-acm-uart` driver's `poll_out` silently **drops bytes** when the TX ring buffer is
full and `hw-flow-control` is NOT set. With the default 1024-byte FIFO a multi-frame MCUmgr
`taskstat` response (~1850 wire bytes) overflows mid-stream and the client times out. Fixed on the
**MCUmgr** port only, `cdc_acm_uart1`, in
`fw/boards/others/rgb_sunglasses_proto0/rgb_sunglasses_proto0_nrf5340_cpuapp_common.dts`:

```dts
cdc_acm_uart1: cdc_acm_uart1 {
    compatible = "zephyr,cdc-acm-uart";
    hw-flow-control;      /* poll_out blocks instead of dropping */
    tx-fifo-size = <4096>;
};
```

`hw-flow-control` makes `poll_out` sleep 1 ms and retry when the buffer is full; `tx-fifo-size =
4096` holds a full taskstat response without blocking at all. The shell port (`cdc_acm_uart0`) does
not set it.
