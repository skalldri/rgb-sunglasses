---
name: provision-device
description: Provision (or verify provisioning of) a connected board's NAND flash — FAT filesystem health, known GLIM assets, and all animation extensions
allowed-tools: Bash, Read, AskUserQuestion, mcp__serial
---

Provisions a connected board's external NAND flash: confirms the FAT filesystem is healthy, generates and pushes every known `.glim` asset, and compiles and pushes every animation extension. Safe to re-run on an already-provisioned board (idempotent — it regenerates and overwrites files).

`fw/scripts/provision-device.sh` does all the host-side mechanical work (locating/mounting the USB mass-storage disk, running the GLIM generators, building extensions, copying files). It never reformats the filesystem itself. This skill wraps it with the parts that require live interaction with the board's Zephyr shell — which per `fw/CLAUDE.md` must go through `mcp__serial__*` tools, never raw Bash reads/writes to `/dev/ttyACM0` — including the **only** reformat path: the firmware's own `fatfs reformat` shell command. Never reformat the NAND disk with a host-side `mkfs.vfat` — the firmware owns the partition and `fatfs reformat` is the documented, tested way to rebuild it.

---

## 1. Check hardware

Run `/check-hardware`. Stop if the dev board isn't detected.

---

## 2. Device-side FAT health check

Over `mcp__serial__*` (connect to the Zephyr shell, `/dev/ttyACM0`), run `glim list` (or `fs ls /NAND:` if that's unavailable). Watch for I/O errors or "mount failed" style output.

- **If healthy**: continue to step 3.
- **If unhealthy**: use `AskUserQuestion` to confirm running the destructive reformat (`fatfs reformat` — erases all files on `/NAND:`). If confirmed:
  1. Send `fatfs reformat` over the shell.
  2. Reboot the board (`kernel reboot warm`).
  3. Wait ~15s for `ttyACM*` to re-enumerate, then re-run `/check-hardware`.

---

## 3. Provision

Run:

```bash
fw/scripts/provision-device.sh
```

- **If it fails to mount the disk** (corrupt/unformatted FAT slipped past step 2's check, or step 2 was skipped): go back to step 2 and run `fatfs reformat` over the shell, reboot, then re-run this script. Do not reach for `mkfs.vfat` — the script deliberately doesn't offer it.
- **If it fails because the build dir isn't configured**: tell the user to run `/build-proto0` first (do not build it yourself as a side effect of provisioning), then re-run this skill.
- **If it succeeds**: the known GLIM assets (`nyan_cat.glim`, `bad_apple.glim`) and every extension under `fw/extensions/*/` have been copied to `/NAND:/glim/` and `/NAND:/ext/`.

---

## 4. Reboot the device

The firmware only discovers new files on a fresh FAT mount. Find the MCUmgr port from `/check-hardware`, then:

```bash
mcumgr --conntype serial --connstring dev=/dev/ttyACMN,baud=115200 reset
```

Wait ~15s for the board to re-enumerate, then re-run `/check-hardware` to get the refreshed `ttyACM*` ports.

---

## 5. Verify

Over `mcp__serial__*`:

- `glim list` (or the `rgb_sunglasses.glim_list` plugin tool) — expect both `nyan_cat.glim` and `bad_apple.glim`.
- `ext list` — expect `hello` and `plasma`, neither marked `[FAULTED]`.

Report a short summary table to the user: filesystem health, GLIM files present, extensions present.
