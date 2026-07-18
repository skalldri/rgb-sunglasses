---
name: flash-and-verify
description: "Flash firmware to the physical board over J-Link and verify it on-device via the serial shell — the full hardware iteration loop (build → flash → verify), including USB re-enumeration handling and MCUmgr OTA updates. HARDWARE skill: requires the board lock for the whole loop."
---

Read `fw/CLAUDE.md` first if you haven't — it is the authoritative memory for every mechanism below; this skill sequences it into one loop.

> **DANGER — this loop never writes power/PD registers.** On-device verification here is read-only shell diagnostics only. Anything beyond that on the TPS25750/BQ25792 (register writes, 4CC tasks, `power pd patch`, `power bq charge/adc/pfm/freq/temp_override`, `power boost`) goes through root `CLAUDE.md`'s "NEVER write unverified commands or data into hardware parts" rule: obtain the datasheet/TRM first or stop and ask the user. A hallucinated 4CC write already bricked a part once (2026-07-05).

## 1. Hold the `board` lock — first, and across the ENTIRE loop

`hold` via `Monitor` is the **only** way to take it (details: the `/hw-lock` skill):

```
Monitor(command: "scripts/hw-lock.sh hold board", description: "board hw-lock heartbeat", persistent: true)
```
```bash
timeout 15 bash -c 'until scripts/hw-lock.sh check board >/dev/null 2>&1; do sleep 0.5; done'
```

If the poll fails, someone else holds it — report the holder and stop; don't steal without asking. Hold across the whole build → flash → test → rebuild cycle; **never release between iterations you're about to repeat**. The J-Link de-enumerating mid-flash is normal and never a reason to release. When another agent queues for the lock you get a waiter notice — that's the signal to wrap up, not a forced release.

## 2. Pre-flash gates (hardware iterations are slow — verify before flashing)

1. Build first: `/build-proto0`.
2. If the change involves Kconfig, confirm it actually landed before flashing:
   ```bash
   grep CONFIG_MY_SYMBOL fw/build/fw/zephyr/include/generated/zephyr/autoconf.h
   ```
3. Run `fw/scripts/fix-usb-dev-nodes.sh` (standard pre-flash step). The devcontainer has no udev, so re-enumeration leaves missing/bogus `/dev/bus/usb/*` nodes — symptoms are `JLinkExe` "Cannot connect to J-Link" or `nrfutil` "Failed to open connection" while `lsusb` shows `1366:0101` fine.

## 3. Flash over J-Link

```bash
fw/scripts/jlink-flash.sh                   # default: this worktree's fw/build (proto0)
fw/scripts/jlink-flash.sh /path/to/build    # explicit build dir
fw/scripts/jlink-flash.sh -- --skip-rebuild # args after -- forward to west flash
```

It self-gates on the `board` lock (refuses without it), auto-detects the J-Link serial, runs a `west build` no-op check, then flashes netcore + appcore via nrfutil (~30-45 s). This is the only routine way to reflash bootloaders — b0n always; MCUboot alone also has the `mcuboot_update sideload`/`commit` shell path (see `fw/CLAUDE.md` "Useful shell commands"). MCUmgr below updates application images only. **If a flash fails**: rerun `fw/scripts/fix-usb-dev-nodes.sh`, retry — this converges on the 2nd attempt. If the node is fine but flashing repeatedly dies at the same verify step with SWD/DebugPort errors, that's the APPROTECT lockout — symptom table and `nrfutil device recover` procedure live in `/debug-fw`.

## 4. USB re-enumeration — after every flash or reset

- Wait ~15 s. `/dev/ttyACM*` disappears and takes longer than expected to come back.
- **ttyACM numbering SHIFTS on every re-enumeration.** Re-run `/check-hardware` before any serial use — it recreates missing `/dev` nodes and identifies the ports by USB interface number: shell = interface **x.0**, MCUmgr = interface **x.2** (may be ttyACM0/1/2/...).
- Already-open `mcp__serial__*` connections go stale (`[Errno 5]` on write). `serial_close` the stale connection_id, then `serial_open` the **new** shell path — never retry the old path/id.

## 5. Verify on-device via the serial shell

**`mcp__serial__*` tools only — never raw Bash on `/dev/ttyACM*`** (races the MCP reader thread; see `fw/CLAUDE.md` "Serial Console"). Wait for the `uart:~$` prompt before commanding. A backlog of old `[00:00:00.xxx]` boot logs on port-open is not a fresh reboot — check `kernel uptime`.

- **Animations**: use the plugin tools `mcp__serial__rgb_sunglasses_set_animation` / `get_animation` / `clear_indicator` (and `glim_list`/`glim_select`/`glim_set_loop_mode`), not hand-rolled `anim` writes. `set_animation` clears the BT indicator first and verifies via `anim get` — a raw `anim set` leaves an active advertising/connecting overlay masking the animation you're trying to see.
- **Read-only diagnostics** (all safe): `bt_conn_info` (actual negotiated LE connection parameters), `power bq status` (battery/VBUS voltage, current, charge status), `ext stats` (extension tick timing), `kernel threads`, `mcuboot_version`.
- **Cross-check app-visible behavior against the shell as source of truth.** A BLE write that "looked right" in the app UI can be an optimistic update masking a failed write/notify — confirm the value via the shell (`anim get`, `glim get_selected`, ...) before calling it verified (house norm; see app/CLAUDE.md "Verifying a write/notify round-trip").

## 6. MCUmgr OTA path (no J-Link needed; app images only)

**Prefer the wrapper script** — it auto-detects the port, uploads app + net-core images from `dfu_application.zip`, auto-parses the slot hashes, tests, resets, and confirms:

```bash
fw/scripts/mcumgr-flash.sh                 # app-mode OTA (board still boots), proto0 fw/build
fw/scripts/mcumgr-flash.sh --app-only      # skip the net-core image (faster)
fw/scripts/mcumgr-flash.sh --recovery      # board is in MCUboot serial recovery (Left button held at reset)
fw/scripts/mcumgr-flash.sh --build-dir <dir>  # override the build dir (default fw/build)
```

It still needs the `board` lock when run by an agent (it self-gates on `CLAUDECODE`), and takes ~3-4 min for the app image. Runbook: `fw/docs/flashing-without-jlink.md`. To drive it by hand instead:

```bash
# Identify the current MCUmgr port with /check-hardware first — it shifts (step 4)
CONN="--conntype serial --connstring dev=/dev/ttyACM1,baud=115200"   # example
mcumgr $CONN echo hello                                    # connectivity check
mcumgr $CONN image upload fw/build/fw/zephyr/zephyr.signed.bin  # ~678 KiB at ~3.5 KiB/s ≈ 3-4 min: be patient
mcumgr $CONN image list                                    # grab the slot-1 hash of image 0
mcumgr $CONN image test <hash>
mcumgr $CONN reset
# wait ~15 s for re-enumeration, then /check-hardware (step 4)
```

After a successful test boot, `mcumgr $CONN image confirm` (or let the app confirm over BLE) — otherwise MCUboot reverts on the next reset. On failure to boot it reverts automatically. Size/rate figures as of 2026-07 — re-verify.

## 7. Installing files (GLIM assets / .llext extensions)

The board's 4 MiB FAT disk mounts over USB mass storage — find/mount procedure: `fw/CLAUDE.md` "USB Flash Disk (`/NAND:` ...)". Copy `.glim` files to the mounted disk's `glim/` dir, `.llext` extensions to `ext/`, then **`sync` → `umount` → REBOOT the board** (warm reboot is enough) — both registries scan at boot only, and writing while the firmware has the volume mounted corrupts reads until reboot.

- GLIM selection persists by file **name**, not index — enumeration order shifts across boots.
- Corrupt FAT → the firmware's own `fatfs reformat` shell command, **never** a host-side mkfs (the firmware owns the partition). Then reboot and re-copy files.
- Full provisioning (all known assets + extensions, FS health) → `/provision-device`. Don't re-implement it here.
- GLIM container format → `fw/src/storage/GLIM_FORMAT.md`; converters live in `fw/tools/` (host-side; validated by `pytest fw/tools/tests -v`).

## 8. Phone-in-the-loop (companion app) — needs the `app` lock

Only when verifying device↔app behavior. Full procedure: app/CLAUDE.md "Launching the App" — summary:

1. Hold `app` the same way as step 1 (`scripts/hw-lock.sh hold app`; combined: `hold board app`, all-or-nothing).
2. Fresh worktree: `cd app && npm ci` (real install — never symlink `node_modules`).
3. Launch **only** via `app/scripts/launch-app.sh` (self-gates on the `app` lock), as a harness-managed background task (`run_in_background: true`, no `&`, no redirects). Never `npx expo run:android` directly.
4. Poll `http://localhost:8081/status` until `packager-status:running`.
5. Drive the phone via `mcp__execbro__*` (scan_metro/connect_metro, get_logs, screenshots, tap). Phone missing from `adb devices`? Try `adb connect <ip>:5555` first, or `app/scripts/adb-connect.sh` — pairing state lives on the phone.

## 9. When genuinely done

Stop the `hold` Monitor task(s) (its exit trap releases the lock; releasing `app` also stops Metro) — once the whole task is finished, not preemptively between iterations, and not squatting after you're done.
