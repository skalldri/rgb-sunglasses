---
paths:
  - "fw/sysbuild/**"
  - "fw/sysbuild.cmake"
  - "fw/Kconfig.sysbuild"
  - "fw/conf/**"
  - "fw/mcuboot_hooks/**"
  - "fw/src/mcuboot_*"
  - "fw/tests/mcuboot_updater/**"
  - "fw/ipc_radio/**"
  - "fw/scripts/build-fw.sh"
---

# Sysbuild, per-image overlays and MCUboot

Loads when you read sysbuild config, the bootloader/netcore images, or the MCUboot updater. The
firmware is four images: MCUboot (appcore bootloader), rgb-sunglasses (`fw`, appcore app), b0n
(netcore bootloader), ipc_radio (netcore app).

## Per-image Kconfig/devicetree overlays (sysbuild)

This is a sysbuild project with 4 images sharing one board-level devicetree. To scope a change to a
single image (e.g. MCUboot only), use sysbuild's per-image config directory convention, not
`fw/conf/<board>/sysbuild.cmake`:

```
fw/sysbuild/<image-name>/prj.conf                          # per-image Kconfig fragment
fw/sysbuild/<image-name>/boards/<board>.conf                # per-image, per-board Kconfig fragment
fw/sysbuild/<image-name>/boards/<board>.overlay              # per-image, per-board devicetree overlay
```

e.g. `fw/sysbuild/mcuboot/boards/rgb_sunglasses_proto0_nrf5340_cpuapp.overlay` only applies to the
MCUboot image. These are auto-discovered by Zephyr's CMake — no wiring needed.

**`add_overlay_dts(${DEFAULT_IMAGE}, ...)` in `fw/conf/<board>/sysbuild.cmake` targets the main "fw"
app image, not MCUboot.** `${DEFAULT_IMAGE}` is sysbuild's default/main image, which is `fw` in this
project. Don't reach for it when you want MCUboot, b0n, or ipc_radio — use the per-image
`fw/sysbuild/<image-name>/` directory instead.

**A newly-added overlay file may not be picked up without a `--pristine` rebuild.** Zephyr's overlay
auto-discovery (`zephyr_file(CONF_FILES ... DTC_OVERLAY_FILE ...)`) is gated behind
`if(NOT DEFINED DTC_OVERLAY_FILE)` — if a prior configure cached `DTC_OVERLAY_FILE` (even as an
empty string) in `build/<image>/CMakeCache.txt`, auto-discovery is permanently skipped on every
incremental build, even after adding the right file. If a new overlay doesn't take effect, check
`grep DTC_OVERLAY_FILE build/<image>/CMakeCache.txt` — if it's defined-but-empty, do a full
`--pristine` rebuild instead of debugging the overlay. Deleting just `build/<image>/CMakeCache.txt`
has NOT been validated for overlay rediscovery. (A per-image fragment silently not applying is also
why the netcore's sleep-clock fragment is enforced by a `BUILD_ASSERT` in `fw/ipc_radio/src/main.c`.)

## MCUboot VERSION incremental build

Editing `fw/sysbuild/mcuboot/VERSION` alone does NOT trigger ninja to recompile. Force a rebuild of
the version-stamped objects by deleting `fw/build/mcuboot/CMakeCache.txt` (forces a cmake
reconfigure) and then touching `fw/build/mcuboot/zephyr/include/generated/zephyr/app_version_override.h`
(forces recompile of `boot_record.c.obj` and `banner.c.obj`).

## Overwrite-only bootloader

MCUboot is built overwrite-only (`SB_CONFIG_MCUBOOT_MODE_OVERWRITE_ONLY=y` in
`fw/conf/rgb_sunglasses_proto0_nrf5340_cpuapp/sysbuild.conf`,
i.e. `CONFIG_BOOT_UPGRADE_ONLY`), whose Kconfig help says it *"prevents the fallback recovery"*;
this SoC's architecture cannot support a swap mode. A staged image is installed on the next boot and
**cannot be rolled back** — an `image test` without `confirm` still comes back `active confirmed`.
App-side consequence: `.claude/rules/app-firmware-update.md`. J-Link consequence (a staged OTA
overwrites a fresh J-Link flash): `.claude/skills/flash-and-verify/references/jlink.md`.

## MCUboot and LED data pins (GPIO retention across warm resets)

MCUboot never links the SPI/LED_STRIP drivers, so the 3 WS2812 data-in pins (P0.29, P1.05, P1.01 —
see `fw/docs/proto0-board-pinout.md`) are unmanaged during MCUboot's runtime. On the nRF53, GPIO
peripheral state (direction/level) is retained across a CPU/software (warm) reset — only a
power-on/brownout reset clears it. So if the app was driving a data line high before a warm reset
(e.g. the `sys_reboot(SYS_REBOOT_WARM)` after MCUmgr's `image test`/`reset`), MCUboot inherits that
stuck-high state, and a WS2812 strip can read it as a steady "on" signal and pull max-brightness
current for the whole bootloader window — risking a brownout that prevents boot entirely.

Fixed via Zephyr's GPIO hogs (`CONFIG_GPIO_HOGS`, auto-enabled by `gpio-hog` devicetree nodes): see
`fw/sysbuild/mcuboot/boards/rgb_sunglasses_proto0_nrf5340_cpuapp.overlay`. It forces all 3 pins to a
driven-low output very early in boot (`SYS_INIT` priority 41), independent of any driver, for
MCUboot's entire runtime. If a future hardware revision adds LED data pins, extend this overlay.
