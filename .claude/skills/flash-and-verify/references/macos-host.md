# macOS host (Mac Mini)

The full firmware dev loop (build → flash → serial verify) also runs natively on the Mac Mini when
the board is attached there. One-time setup: `scripts/macos-setup.sh` (idempotent — Homebrew bash
for hw-lock, Go + mcumgr, NCS v3.1.1 west workspace + Zephyr SDK at `~/ncs`, `serial_mcp`, and the
GLIM converter tooling: ffmpeg + a python venv with Pillow/numpy/lz4/yt-dlp). iOS app builds have
their own setup (`app/scripts/macos-setup.sh`, `.claude/skills/launch-app/references/ios.md`).

| Aspect | devcontainer | macOS host |
|---|---|---|
| Shell port | `/dev/ttyACM0` (iface x.0) | `/dev/cu.usbmodem*`, lower suffix (data iface 1) |
| MCUmgr port | `/dev/ttyACM1`/`ttyACM2` (iface x.2) | `/dev/cu.usbmodem*`, higher suffix (data iface 3) |
| Flashing (app + netcore) | J-Link fast path (`jlink-flash.sh`) | **MCUmgr OTA only** (`fw/scripts/mcumgr-flash.sh`) — no SEGGER tooling |
| MCUboot reflash | J-Link, or `mcuboot_update` shell path | `mcuboot_update` sideload/commit shell path only |
| b0n (netcore bootloader) reflash | J-Link | not possible — use the devcontainer |
| Build env | west on PATH | `. scripts/fw-env.sh` first (skills do this) |
| Twister tests | `/test-fw` (native_sim) | **not supported** (native_sim is Linux-only) — use CI or the devcontainer |
| GLIM converter deps | pip/apt in the image | `. scripts/tools-env.sh` first (venv from `scripts/macos-setup.sh`) |
| /NAND: disk mount | `dmesg`/`lsblk`/`mount` | Finder/`diskutil` (`/Volumes`); same sync → eject → reboot discipline |
| NCS SDK path | `/root/ncs/v3.1.1` | `~/ncs/v3.1.1` (docs citing `/root/ncs/...` map here) |

Never hardcode the `cu.usbmodem` names — discover them via `/check-hardware` (they can shift on
re-enumeration). Everything else — hw-lock discipline, `mcp__serial__*` usage, the shell command
surface — is identical; `scripts/hw-lock.sh` re-execs itself into Homebrew bash ≥ 4 on macOS. Locks
are per-host (`$GIT_COMMON_DIR`), which is fine: the board is attached to exactly one host at a time.

**`fw-env.sh` and `tools-env.sh` activate different python venvs** — whichever is sourced last owns
`python3`, and a `west build` after `tools-env.sh` configures against the wrong interpreter. Use
separate shells: one for building firmware, one for generating GLIM assets.

**The NAND disk mounts on macOS**, and `/provision-device` is supported there
(`fw/scripts/provision-device.sh` finds the disk via IOKit and uses `diskutil`). Verified end-to-end
on the Mac Mini 2026-08-13: assets generated, extensions built, files copied to `/Volumes/NO NAME`.
The same FAT-concurrency rule applies: write → `sync` → `diskutil unmount` → **reboot the board**.

## Three macOS-specific behaviours that will otherwise waste hours

- **A locked screen means no disk — macOS ejects it on sight.** Root-caused 2026-08-13 (issue #367):
  when the console is locked, `loginwindow` "RegisterDiskArbCallbacks to block disk mounts during
  screen lock" (its own log wording) — on the board's next attach, DiskArbitration probes the FAT
  successfully (`msdos_fskit success`), a mount-approval client dissents with `0xF8DA0008`
  (kDAReturnNotPermitted), and **loginwindow requests a full eject ~2.7 s after the disk appears**.
  The `IOMedia` exists for under a second, so polling `ioreg`/`diskutil` makes it look like the disk
  was never published. Zephyr's MSC then latches the eject (`medium_loaded = false`,
  hardcoded-removable LUN per `usbd_msc_scsi.c`) until the board reboots. Recovery: **unlock the
  screen, then reboot the board** — nothing is wrong with the board, the cable, or the FAT. Do not
  chase USB re-enumeration, absence duration, or replugging: a replug only ever "fixed" this because
  a human replugging is standing at an unlocked Mac. Three more measured facts: the shield engages on
  **display dim** (`kLWLockFromDisplayDim`), not just explicit lock, so `sudo pmset -a displaysleep 0`
  is the setting that keeps the disk reachable (`macos-setup.sh` checks it and prints the command,
  but deliberately never modifies machine config itself); `AutomountDisksWithoutUserLogin` does
  **not** stop the lock-shield eject (tested — identical dissent + eject; it only covers mounting with
  no user logged in); and `IOConsoleLocked` stays *false* during the dim-shield window, so
  `provision-device.sh`'s fail-fast guard catches hard locks but not a freshly dimmed display — if
  provisioning reports no disk right after the display blanks, that's why.
- **macOS `cp` writes AppleDouble sidecars (`._name`) onto FAT, and the firmware treats them as real
  assets.** Hardware-verified: `glim list` showed `._4096.glim`, `._bad_apple.glim`,
  `._nyan_cat.glim` next to the real files, with the 4 KB `._4096.glim` **selected** as the active
  animation. `COPYFILE_DISABLE=1` does NOT prevent this (it governs tar/copyfile, not `cp` —
  hardware-verified); `provision-device.sh` uses `cp -X` plus a post-copy `._*` sweep, and any manual
  `cp` to the board must do the same. (The extension registry rejects sidecars via manifest
  validation, so only GLIM is user-visibly affected.)
- **`MODE_SENSE_06 failed ... DetermineMediumWriteProtectState` in the macOS log is benign noise.**
  It is emitted on *every* attach, including ones that mount perfectly — Zephyr's MODE SENSE(6)
  handler only accepts page code `0x3F`. Do not chase it.

Note `log` is shadowed by a zsh builtin: a bare `log show ...` fails with "too many arguments" and
the empty output reads as "nothing logged". Always use `/usr/bin/log`.
