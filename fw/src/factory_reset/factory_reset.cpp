/* Boot-time two-phase factory reset (issues #162, #265).
 *
 * If the user is holding the Up + Down D-PAD buttons when the app boots, boot
 * pauses and both status LEDs flash white (phase 1). Once the chord has been
 * held for CONFIG_APP_FACTORY_RESET_HOLD_MS (10 s), the LEDs switch to
 * flashing amber (phase 2) — releasing anywhere in that window performs a
 * "soft" reset: erase the NVS settings partition only (all app config + BT
 * bonds — phones must re-pair) and reboot. Holding through the full
 * CONFIG_APP_FACTORY_RESET_PHASE2_HOLD_MS window instead performs the full
 * reset: settings, the coredump partition, and a re-created FAT filesystem
 * on the NAND disk (GLIM assets and .llext extensions are lost; re-provision
 * afterwards). Releasing during phase 1 resumes a normal boot with nothing
 * erased. Nothing is erased until the hold loop returns its decision — the
 * lazy erase is deliberate (outcomes match erasing settings eagerly at the
 * phase boundary, and a power loss mid-phase-2 leaves settings intact).
 *
 * Why Up + Down and not all 4 D-PAD buttons (as issue #162 originally asked):
 * Left (button1) is `mcuboot-button0`, MCUboot's serial-recovery entry button
 * (CONFIG_BOOT_SERIAL_ENTRANCE_GPIO). On a cold power-on or pin reset with
 * Left held, MCUboot enters DFU recovery and the app never runs — the all-4
 * chord would only have worked after software reboots. Up + Down avoids
 * button1 entirely, so the gesture works on every reset type.
 *
 * Blocking in SYS_INIT is deliberate and safe here: this hook runs at
 * APPLICATION priority 0, before any application thread is scheduled and
 * before bluetooth_init's settings_load() (priority 1) or the FAT mount
 * (priority 90), and no watchdog is configured. "Pause boot" is exactly the
 * requested behavior, and the no-flash-I/O-from-cooperative-threads rule
 * doesn't apply — there are no other threads to starve yet.
 *
 * Holding the chord through the post-reset reboot simply re-arms the check
 * and runs another full hold cycle — idempotent and harmless.
 */

#include "factory_reset_core.h"

#include <storage/appcfg_erase.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

#if defined(CONFIG_DEBUG_COREDUMP)
// coredump.h has no extern "C" guards of its own (NCS v3.1.1), so without
// this wrapper the coredump_cmd() reference gets C++-mangled and fails to link.
extern "C" {
#include <zephyr/debug/coredump.h>
}
#endif

#if defined(CONFIG_FAT_FILESYSTEM_ELM) && defined(CONFIG_FILE_SYSTEM_MKFS)
#include <storage/storage.h>
#define FACTORY_RESET_HAS_FAT 1
#endif

#if DT_HAS_ALIAS(led_strip_2)
#include <zephyr/drivers/led_strip.h>
#endif

#if defined(CONFIG_SHELL)
#include <zephyr/shell/shell.h>
#endif

LOG_MODULE_REGISTER(factory_reset);

namespace {

using factory_reset_core::Decision;
using factory_reset_core::HoldConfig;
using factory_reset_core::HoldIo;
using factory_reset_core::LedState;
using factory_reset_core::ResetOps;

// The reset chord: Up (sw0) + Down (sw3). See the header comment for why
// Left/Right are deliberately excluded.
const struct gpio_dt_spec chord_buttons[] = {
    GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios),
    GPIO_DT_SPEC_GET(DT_ALIAS(sw3), gpios),
};

// Both pins are ACTIVE_LOW + PULL_UP in devicetree; gpio_pin_get_dt() returns
// 1 (logical "active") while the button is held. A read error counts as "not
// held" so a GPIO fault can never trap the device in the hold loop.
bool chord_held(void*) {
    for (const auto& btn : chord_buttons) {
        if (gpio_pin_get_dt(&btn) != 1) {
            return false;
        }
    }
    return true;
}

/* The status_led module's render thread isn't running during SYS_INIT (static
 * threads start only after the APPLICATION level completes), so drive the
 * strip directly instead of via status_led_set(). Phase 1 is white, phase 2
 * amber — the palette's Orange (255,128,0) scaled to the same moderate
 * brightness as the white: settings (and thus the configured status-LED
 * brightness factor) aren't loaded yet. Both colors are kept local rather
 * than added to StatusColor — boot-only indications, not part of the public
 * shell-parsed palette. */
#if DT_HAS_ALIAS(led_strip_2)
void set_leds(void*, LedState state) {
    const struct device* strip = DEVICE_DT_GET(DT_ALIAS(led_strip_2));
    if (!device_is_ready(strip)) {
        return;
    }
    struct led_rgb px = {.r = 0, .g = 0, .b = 0};
    switch (state) {
        case LedState::Phase1:
            px = {.r = 128, .g = 128, .b = 128};  // white
            break;
        case LedState::Phase2:
            px = {.r = 128, .g = 64, .b = 0};  // amber
            break;
        case LedState::Off:
            break;
    }
    struct led_rgb buf[2] = {px, px};
    led_strip_update_rgb(strip, buf, ARRAY_SIZE(buf));
}
#else
// Boards without status LEDs: the hold loop still works, silently.
void set_leds(void*, LedState) {}
#endif

void sleep_ms(void*, uint32_t ms) {
    k_msleep(ms);
}

int erase_settings_op() {
    int rc = storage_erase_settings_partition();
    if (rc != 0) {
        LOG_ERR("settings partition erase failed: %d", rc);
    }
    return rc;
}

#if defined(CONFIG_DEBUG_COREDUMP)
int erase_coredump_op() {
    // ERASE_STORED_DUMP is a full-partition erase in the NCS flash backend —
    // unlike the coredump manager's INVALIDATE, which only clears the header.
    int rc = coredump_cmd(COREDUMP_CMD_ERASE_STORED_DUMP, NULL);
    if (rc < 0) {
        LOG_ERR("coredump partition erase failed: %d", rc);
        return rc;
    }
    return 0;
}
#endif

#if defined(FACTORY_RESET_HAS_FAT)
int reformat_fat_op() {
    int rc = storage_fat_wipe_for_reset();
    if (rc != 0) {
        LOG_ERR("FAT reformat failed: %d", rc);
    }
    return rc;
}
#endif

}  // namespace

/* Erase the requested scope and report the first error. full == false is the
 * soft reset: settings only (coredump/FAT ops stay null and are skipped by
 * perform_reset). Steps that fail are logged by their op wrappers; later
 * steps still run (a partial reset beats an aborted one, and the erases are
 * independent). Callers reboot regardless of the return value — an
 * erased-but-unformatted state still comes up clean via
 * CONFIG_FS_FATFS_MOUNT_MKFS and NVS's tolerance of an erased partition. */
static int factory_reset_perform(bool full) {
    const ResetOps ops = {
        .erase_settings = erase_settings_op,
#if defined(CONFIG_DEBUG_COREDUMP)
        .erase_coredump = full ? erase_coredump_op : nullptr,
#else
        .erase_coredump = nullptr,
#endif
#if defined(FACTORY_RESET_HAS_FAT)
        .reformat_fat = full ? reformat_fat_op : nullptr,
#else
        .reformat_fat = nullptr,
#endif
    };

    if (full) {
        LOG_WRN("factory reset: erasing settings, coredump, and FAT storage");
    } else {
        LOG_WRN("settings reset: erasing settings partition (BT bonds included)");
    }
    int rc = factory_reset_core::perform_reset(ops);
    if (rc == 0) {
        LOG_WRN("%s reset complete", full ? "factory" : "settings");
    } else {
        LOG_ERR("%s reset finished with errors (first: %d)", full ? "factory" : "settings", rc);
    }
    return rc;
}

static int factory_reset_boot_check(void) {
    for (const auto& btn : chord_buttons) {
        if (!gpio_is_ready_dt(&btn) || gpio_pin_configure_dt(&btn, GPIO_INPUT) != 0) {
            // Never let a GPIO problem block boot; button_init (priority 1)
            // will complain about the same pins moments later.
            return 0;
        }
    }

    if (!chord_held(nullptr)) {
        return 0;
    }

    LOG_WRN("factory reset armed: hold Up+Down %u ms (white flash) for settings reset; "
            "keep holding %u ms more (amber flash) for full reset",
            CONFIG_APP_FACTORY_RESET_HOLD_MS, CONFIG_APP_FACTORY_RESET_PHASE2_HOLD_MS);

    const HoldConfig cfg = {
        .hold_duration_ms = CONFIG_APP_FACTORY_RESET_HOLD_MS,
        .phase2_hold_ms = CONFIG_APP_FACTORY_RESET_PHASE2_HOLD_MS,
        .poll_interval_ms = 20,
        .flash_half_period_ms = 100,
    };
    const HoldIo io = {
        .chord_held = chord_held,
        .set_leds = set_leds,
        .sleep_ms = sleep_ms,
        .ctx = nullptr,
    };

    const Decision decision = factory_reset_core::run_hold_loop(cfg, io);
    if (decision == Decision::ContinueBoot) {
        LOG_INF("factory reset canceled (chord released); resuming boot");
        return 0;
    }

    // Solid color while erasing (~2-4 s) so the user can tell it's working —
    // white for a settings-only reset, amber for the full one, confirming
    // which reset was committed.
    const bool full = decision == Decision::FullReset;
    set_leds(nullptr, full ? LedState::Phase2 : LedState::Phase1);
    factory_reset_perform(full);
    set_leds(nullptr, LedState::Off);

    sys_reboot(SYS_REBOOT_COLD);
    CODE_UNREACHABLE;
}

/* Priority 0 (literal, per project rule): must run before bluetooth_init's
 * settings_load() and button_init (both literal 1), and before mount_fat
 * (CONFIG_APPLICATION_INIT_PRIORITY = 90) — the FAT volume must not be
 * mounted when the boot-path mkfs runs. */
SYS_INIT(factory_reset_boot_check, APPLICATION, 0);

#if defined(CONFIG_SHELL)

static int cmd_factory_reset_now(const struct shell* sh, size_t argc, char** argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    shell_warn(sh, "Factory reset: erasing all settings, coredumps, and files, then rebooting.");

    factory_reset_perform(true);

    // Give the shell transport a moment to flush the warning before the
    // reboot drops the USB connection.
    k_msleep(100);
    sys_reboot(SYS_REBOOT_COLD);
    return 0;
}

/* The one-shot runtime equivalent of the boot gesture's phase-2 release.
 * 'appcfg erase' is the same settings-partition erase without the reboot. */
static int cmd_factory_reset_soft(const struct shell* sh, size_t argc, char** argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    shell_warn(sh, "Settings reset: erasing all settings (BT bonds included), then rebooting. "
                   "Files are kept.");

    factory_reset_perform(false);

    // Same flush delay as 'now' before the reboot drops the USB connection.
    k_msleep(100);
    sys_reboot(SYS_REBOOT_COLD);
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_factory_reset,
    SHELL_CMD(now, NULL,
              "Erase settings + coredump + FAT storage and reboot (DESTRUCTIVE)",
              cmd_factory_reset_now),
    SHELL_CMD(soft, NULL,
              "Erase settings only (incl. BT bonds) and reboot; files kept "
              "('appcfg erase' is the same erase without the reboot)",
              cmd_factory_reset_soft),
    SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(factory_reset, &sub_factory_reset,
                   "Factory reset (see 'factory_reset now' / 'factory_reset soft')", NULL);

#endif /* CONFIG_SHELL */
