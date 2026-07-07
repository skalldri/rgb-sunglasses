# On-device symptom → cause table (Table 3 of /debug-fw)

**Every row requires the `board` hardware lock — see `/hw-lock` — held for the whole
diagnosis session.** The `PreToolUse` guard auto-denies serial MCP calls without it.
Talk to the Zephyr shell only through `mcp__serial__*` tools (never raw `/dev/ttyACM*` —
rule in `fw/CLAUDE.md`). Re-flashing after a fix: `/flash-and-verify`.

> **DANGER ZONE reminder:** all commands in this file are reads. Any register/4CC/patch
> **write** to a physical part requires the datasheet/TRM first per root `CLAUDE.md`
> ("NEVER write unverified commands or data into hardware parts"); `power boost` writes
> UICR irreversibly. See the callout in `SKILL.md`.

## Symptom table

| Symptom | Likely cause | Verify with | Fix / pointer |
|---|---|---|---|
| Animation "looks crashed" — LEDs dark/black but the shell responds | Animation draws dim source values. The pattern controller multiplies every channel by the global brightness factor before it reaches the strip — default `core/brightness` is 20/1000 = **0.02**, so pixels drawn at e.g. 32/255 render invisible (real incident) | `anim get` shows the expected animation is active; then grep the animation's `tick()` for low RGB constants. Mechanism: `pattern_controller_set_pixel_in_framebuffer` / `sBrightnessForFrame` in `fw/src/pattern_controller.cpp`; default in `fw/src/core_config.cpp` (`"core/brightness"` … `20`) | Draw near full-scale 255 in the animation; brightness is a user setting, not something the animation compensates for |
| An animation renders, but the **wrong** one — `anim set` seems ignored | An active BT indicator overlay (advertising/connecting/pairing) always overrides `currentAnimation` (`getBestRenderAnimation` in `fw/src/pattern_controller.cpp`) | `anim get` prints the selected animation even while the overlay is drawing — mismatch between what you see and `anim get` = indicator active | `anim indicator clear`, or use the `mcp__serial__rgb_sunglasses_set_animation` MCP tool, which clears the indicator before setting |
| Intermittent I2C errors **plus** physically implausible readings (e.g. VBAT reads back the VBUS value; 2S pack "at" 4990 mV) | Interleaved multi-step bridged transaction: BQ25792 is reached via the TPS25750 I2Cm bridge, a non-atomic CMD1/DATA1 4CC sequence; a second concurrent caller silently corrupts data | This exact signature was root-caused in issue #109 / PR #111. Check any *new* multi-step register sequence for a missing `k_mutex` around the **whole** sequence; the fixed pattern is `task_mutex` + `*_locked` inner functions + bounded ~1 s poll loops returning `-ETIMEDOUT` in `fw/drivers/tps25750/tps25750.c` | Copy the PR #111 pattern; regression suite: `fw/tests/drivers/emul_tps25750` (its concurrency test needs an artificial `emul_tps25750_set_cmd_delay_ms` blocking window to reproduce races on native_sim) |
| Huge positive current values while discharging (thousands of mA the wrong way) | Missing two's-complement sign extension on a 16-bit ADC field (BQ25792 IBAT/IBUS — PR #106) | `power bq status` while charger unplugged: discharge current should be small and negative-signed. Decode tests: `fw/tests/drivers/bq25792_decode/src/main.cpp` | Sign-extend per the datasheet; add a case to `bq25792_decode` |
| `%f` prints the literal string `%f` in logs/shell | Float printf support is compiled out: `CONFIG_CBPRINTF_FP_SUPPORT=n` in `fw/prj.conf` (deliberate, size) | `grep CBPRINTF fw/prj.conf` | Use `fmt_fixed4()` (see `fw/src/sound/sound.cpp`) or integer fixed-point — do **not** flip the Kconfig |
| Whole system freezes/stutters during settings saves or other flash writes | Flash I/O running on a cooperative-priority thread starves everything (PR #51) | Find the writer: `grep -rn "flash_write\|fs_write" fw/src/<suspect>` and check the calling thread's priority (negative = cooperative) | Move the I/O to a low-priority workqueue — pattern: `persistent_value_lowpri_workq` in `fw/src/settings/persistent_value_store.cpp` |
| A setting silently doesn't persist across reboot | (a) Persistent-value registry full: fixed cap `kMaxRegistryEntries = 96` in `fw/src/settings/persistent_value_registry.cpp`, new entries dropped with `-ENOMEM` (issue #114); (b) settings partition vs Kconfig sector-count mismatch (PR #63: `CONFIG_SETTINGS_NVS_SECTOR_COUNT=8` expects 32 KB) | (a) boot log contains `Persisted value table is full`; (b) compare `CONFIG_SETTINGS_NVS_SECTOR_COUNT` × 4 KB in autoconf.h against the settings partition size in the Partition Manager report | (a) issue #114 tracks removing the cap — don't just bump the constant without checking RAM (`/rom-ram-budget`); (b) fix the Kconfig/partition pair together |
| New `K_USER` thread faults instantly (MPU fault at startup) | In-place `K_THREAD_DEFINE` → `K_USER` conversion — statically-defined threads can't be moved into a mem domain on this SoC | Compare against the one working example: dynamic `k_thread_create(..., K_FP_REGS \| K_USER, K_FOREVER)` + access grants + `k_mem_domain` including `z_libc_partition`, in `imu_init()` (`fw/src/imu/imu.cpp`) | Copy the `imu_init` pattern verbatim; see also `fw/CLAUDE.md` userspace notes |
| Shell/log spammed every frame | (a) An animation calls `setPixel` off-display — `set_pixel_in_framebuffer` (`fw/src/led_controller.cpp`) LOG_ERRs per pixel (`Pixel at %u, %u is off the edge`); nose-cutout pixels return `-2` silently and are fine. (b) An info-level log sits in a steady-state/per-tick path (PR #110 rule) | Read the spamming message text, grep it in `fw/src` | (a) clamp coordinates to `displayWidth()`/`displayHeight()`; (b) demote to `LOG_DBG` or log only on state *change* |
| A `.glim` file copied to NAND doesn't show in `glim list` | `glim_registry` scans `/NAND:/glim` **only at boot** (`fw/src/storage/glim_registry.h`), and the firmware's FAT cache goes stale if the host writes under it | `glim list` before vs after reboot | Host side: copy → `sync` → `umount`; then **reboot the board**. Corrupt FAT: firmware `fatfs reformat` — never host `mkfs.vfat` |
| Extension shows the FAULT banner and won't re-activate from the app | A faulted extension slot stays dead by design; BLE re-activation is rejected (`extension '%s' is faulted — activation rejected` in `fw/src/extensions/extension_host.cpp`) | `ext list` / boot log shows the fault; log line names the extension | Only `ext select <slot>` clears the fault and retries. If it re-faults, debug the extension itself (`/add-extension`) |
| Shell UART wedged, `/dev/ttyACM*` missing/shifted, J-Link `Cannot connect`, nrfutil `Failed to open connection` | Udev-less WSL2 devcontainer: device nodes go stale after every re-enumeration (each flash/reboot shifts ttyACM numbering) | `ls /dev/ttyACM*` vs `ls -d /sys/class/tty/ttyACM*` | Run `fw/scripts/fix-usb-dev-nodes.sh` (needs root), then re-run `/check-hardware` to re-identify shell vs MCUmgr ports. Details on port probing: `fw/CLAUDE.md` |
| Flashing repeatedly dies at the **same verify step** with SWD/`DebugPort` errors, even though the `/dev` nodes are fine (`fix-usb-dev-nodes.sh` + retry does **not** converge) | APPROTECT/debug-port lockout — distinct from the stale-node row above, which converges on the second attempt | Rule out the stale-node cause first (previous row); node present and healthy + identical verify-step failure on every retry = lockout | `nrfutil device recover --serial-number <sn> --core network`, then the same command with `--core application`, then reflash (`/flash-and-verify`). Recovery **mass-erases internal flash**; BLE bonds survive — they live in the settings partition on **external** flash |
| Board reboots once right after the very first flash | **Normal.** `init_check_and_enable_3v3` (`fw/src/power.cpp`) writes UICR `VREGHVOUT` to 3.3 V on first boot and warm-reboots. Repeated reboot *loops* are a different problem — capture the boot banner/fault dump over serial before theorizing | Boot log shows the 3.3 V message once, then never again | Nothing to fix for the single reboot |

## Read-only serial diagnostic inventory

All safe to run while holding the board lock. "Healthy" is the steady state on a
provisioned proto0.

| Command | What healthy looks like |
|---|---|
| `anim get` | Prints one lowercase name (e.g. `zigzag`, `glim_player`); `unknown` means an enum/name mapping gap |
| `anim indicator clear` | (State-changing but benign) returns silently; use when an overlay masks the animation |
| `glim list` / `glim get_selected` | Lists provisioned `.glim` files (nyan_cat, bad_apple after `/provision-device`); empty list + FS errors in log = storage problem |
| `ext list` / `ext stats` | Provisioned extensions listed, none marked faulted; stats show per-tick µs timings well under the ~11 ms frame budget |
| `power bq status` | Plausible physics: 2S VBAT roughly 6000–8400 mV, VBUS ≈ 5000 mV only when USB present, small signed currents, sensible charge status. VBAT == VBUS is the PR #111 corruption signature |
| `power pd dump` / `power bq dump` | Register dumps complete without I2C error lines |
| `power vreghvout` | Reports the 3.3 V UICR mode on any board that has booted this firmware |
| `bt_conn_info` | With the app connected: LE interval/latency/timeout printed; `No active BLE connection` otherwise. (Note: there is **no** `bt_state` shell command — connection diagnostics beyond this live in `/debug-ble`) |
| `serial print` | Prints device serial number and BT name |
| `sound rms` | A number that moves with ambient noise (audio pipeline alive) |
| `mcuboot_version` | Prints the bootloader version read from the retained bootloader-info block MCUboot wrote before the app started (`blinfo_lookup` in `fw/src/mcuboot_info.cpp` — no live IPC involved); `<unavailable rc=...>` means the retention/blinfo area was not parsed |

**Not diagnostic:** `fatfs` has exactly one subcommand, `fatfs reformat`, and it is
**DESTRUCTIVE** (erases all NAND files). It is the *recovery* path for a corrupt FAT —
never a health check.

## Mining past incidents with `gh` (read-only)

Full RCAs live in the tracker, not the tree. Repo: `skalldri/rgb-sunglasses`.

```bash
gh pr view <N>                 # PR body only (Pre-PR checks, root-cause narrative)
gh pr view <N> --comments      # discussion comments — separate call, body NOT included
gh api repos/skalldri/rgb-sunglasses/pulls/<N>/comments --paginate   # inline review threads
gh issue view <N>              # incident write-ups
```

Richest sources: issue **#41** (BLE discovery latency RCA), issue **#109** + PR **#111**
(TPS25750 I2Cm bridge race / register-corruption RCA — note the 2026-07-05 PD-controller
*bricking* incident is recorded only in root `CLAUDE.md`, not in the tracker), issue
**#115** (Android GATT-cache split-brain), PR **#89** review threads (the owner's review
standards, 40+ comments).
