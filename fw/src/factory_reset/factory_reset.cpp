/* Boot-time two-phase factory reset (issues #162, #265).
 *
 * If the user is holding the Up + Down D-PAD buttons when the app boots, boot
 * pauses and both status LEDs flash white (phase 1). Releasing during phase 1
 * resumes a normal boot with nothing erased. Once the chord has been held for
 * CONFIG_APP_FACTORY_RESET_HOLD_MS (10 s), the LEDs go SOLID white while the
 * settings erase runs (the NVS settings partition — all app config + BT
 * bonds; phones must re-pair), then switch to flashing amber (phase 2).
 * Releasing anywhere in that window stops there — a "soft" reset — and the
 * device reboots. Holding through the full
 * CONFIG_APP_FACTORY_RESET_PHASE2_HOLD_MS window continues to the full
 * reset: the LEDs go solid amber while the coredump partition is erased and
 * the FAT filesystem on the NAND disk is re-created (GLIM assets and .llext
 * extensions are lost; re-provision afterwards). Each solid-color step is
 * the visible confirmation that its erase actually ran: white flash -> solid
 * white (settings gone) -> amber flash -> solid amber (files gone). The
 * settings erase is deliberately committed at the phase boundary, before the
 * user's phase-2 choice — release timing selects the same outcomes either
 * way, and the sequenced LED feedback is the point.
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

#include <settings/persistent_value_store.h>
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
#include <status_led/status_led_math.h>
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
 * amber — the palette's Orange via the status_led_math helpers, scaled to the
 * same moderate brightness as the white: settings (and thus the configured
 * status-LED brightness factor) aren't loaded yet. White is kept local rather
 * than added to StatusColor — a boot-only indication, not part of the public
 * shell-parsed palette. */
#if DT_HAS_ALIAS(led_strip_2)

// Moderate boot-time brightness (out of 255) for both phase colors.
constexpr uint8_t kBootLedLevel = 128;

void set_leds(void*, LedState state) {
    const struct device* strip = DEVICE_DT_GET(DT_ALIAS(led_strip_2));
    if (!device_is_ready(strip)) {
        return;
    }
    struct led_rgb px = {.r = 0, .g = 0, .b = 0};
    switch (state) {
        case LedState::Phase1:
            px = {.r = kBootLedLevel, .g = kBootLedLevel, .b = kBootLedLevel};  // white
            break;
        case LedState::Phase2:
            // Amber: the shipped palette's Orange at boot brightness, derived
            // through the same helpers status_led.cpp renders with so a
            // palette tune can never drift this color out of sync.
            px = status_led_scale_brightness(
                status_led_color_to_rgb(StatusColor::Orange), kBootLedLevel);
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
    /* Unmounts the live volume. With CONFIG_FS_FATFS_REENTRANT this carries
     * the volume-mutex re-init hazard documented above cmd_storage_reformat
     * (fw/src/storage/storage.cpp) — and `factory_reset now` runs this on
     * the shell thread of a FULLY LIVE system (GLIM playback, the coredump
     * wq tick and an active capture are all possible; the boot-gesture path
     * is the quiet one). What actually bounds the damage is the
     * SYS_REBOOT_COLD ~100 ms later in reset_and_reboot(): an orphaned
     * waiter or a stuck inherited priority does not outlive the reset. Any
     * future NON-rebooting caller of this op inherits a permanent-hang
     * hazard and must quiesce FS users first. */
    int rc = storage_fat_wipe_for_reset();
    if (rc != 0) {
        LOG_ERR("FAT reformat failed: %d", rc);
    }
    return rc;
}
#endif

}  // namespace

/* Which erase steps to run. The boot path splits the work across the two
 * phases (SettingsOnly at the phase-1 boundary, FilesOnly if phase 2 is held
 * through); the 'factory_reset now' shell command still runs Everything in
 * one shot. */
enum class Scope {
    SettingsOnly,  // NVS settings partition (all app config + BT bonds)
    FilesOnly,     // coredump partition + FAT reformat (settings already done)
    Everything,    // all three
};

/* Erase the requested scope and report the first error. Unused ops stay null
 * and are skipped by perform_reset. Steps that fail are logged by their op
 * wrappers; later steps still run (a partial reset beats an aborted one, and
 * the erases are independent). Callers reboot regardless of the return value
 * — an erased-but-unformatted state still comes up clean via
 * CONFIG_FS_FATFS_MOUNT_MKFS and NVS's tolerance of an erased partition. */
static int factory_reset_perform(Scope scope) {
    const bool settings = scope != Scope::FilesOnly;
    const bool files = scope != Scope::SettingsOnly;

    if (settings) {
        // A queued debounced settings save (persistent_value_store's work
        // item, armed by any recent config write) firing after the erase
        // would resurrect the just-erased config on next boot — or write
        // into the partition mid-erase. Cancel it synchronously first. At
        // boot (SYS_INIT 0) nothing is queued yet and this is a no-op.
        persistent_value_store::cancel_pending_save();
    }
    const ResetOps ops = {
        .erase_settings = settings ? erase_settings_op : nullptr,
#if defined(CONFIG_DEBUG_COREDUMP)
        .erase_coredump = files ? erase_coredump_op : nullptr,
#else
        .erase_coredump = nullptr,
#endif
#if defined(FACTORY_RESET_HAS_FAT)
        .reformat_fat = files ? reformat_fat_op : nullptr,
#else
        .reformat_fat = nullptr,
#endif
    };

    switch (scope) {
        case Scope::SettingsOnly:
            LOG_WRN("settings reset: erasing settings partition (BT bonds included)");
            break;
        case Scope::FilesOnly:
            LOG_WRN("factory reset: erasing coredump and FAT storage");
            break;
        case Scope::Everything:
            LOG_WRN("factory reset: erasing settings, coredump, and FAT storage");
            break;
    }
    int rc = factory_reset_core::perform_reset(ops);
    if (rc == 0) {
        LOG_WRN("erase complete");
    } else {
        LOG_ERR("erase finished with errors (first: %d)", rc);
    }
    return rc;
}

/* HoldIo::commit_phase1 — runs inside the hold loop at the phase-1 deadline,
 * LEDs already solid white. A nonzero return makes the loop skip the solid
 * dwell so a failed erase never gets the success confirmation. */
static int commit_settings_erase(void*) {
    return factory_reset_perform(Scope::SettingsOnly);
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

    if (CONFIG_APP_FACTORY_RESET_PHASE2_HOLD_MS == 0) {
        // Legacy single-phase mode: there is no phase-2 release window, so
        // don't promise one — the full erase fires at the phase-1 deadline.
        LOG_WRN("factory reset armed: hold Up+Down %u ms (white flash) for a FULL reset "
                "(single-phase mode, PHASE2_HOLD_MS=0)",
                CONFIG_APP_FACTORY_RESET_HOLD_MS);
    } else {
        LOG_WRN("factory reset armed: hold Up+Down %u ms (white flash) for settings reset; "
                "keep holding %u ms more (amber flash) for full reset",
                CONFIG_APP_FACTORY_RESET_HOLD_MS, CONFIG_APP_FACTORY_RESET_PHASE2_HOLD_MS);
    }

    const HoldConfig cfg = {
        .hold_duration_ms = CONFIG_APP_FACTORY_RESET_HOLD_MS,
        .phase2_hold_ms = CONFIG_APP_FACTORY_RESET_PHASE2_HOLD_MS,
        // The settings-partition erase measures ~450 ms on proto0; the pad
        // roughly doubles the solid-white commit indication (~950 ms total)
        // so it reads as a deliberate step, not a flicker.
        .commit_hold_ms = 500,
        .poll_interval_ms = 20,
        .flash_half_period_ms = 100,
    };
    const HoldIo io = {
        .chord_held = chord_held,
        .set_leds = set_leds,
        .sleep_ms = sleep_ms,
        .commit_phase1 = commit_settings_erase,
        .ctx = nullptr,
    };

    const Decision decision = factory_reset_core::run_hold_loop(cfg, io);
    if (decision == Decision::ContinueBoot) {
        LOG_INF("factory reset canceled (chord released); resuming boot");
        return 0;
    }

    // The settings erase already ran inside the hold loop (solid white at the
    // phase boundary). A phase-2 release stops there; holding through phase 2
    // finishes the job — solid amber while the coredump + FAT erase runs
    // (~2-4 s) so the user can tell it's working.
    if (decision == Decision::FullReset) {
        set_leds(nullptr, LedState::Phase2);
        factory_reset_perform(Scope::FilesOnly);
        set_leds(nullptr, LedState::Off);
    }

    sys_reboot(SYS_REBOOT_COLD);
    CODE_UNREACHABLE;
}

/* Priority 0 (literal, per project rule): must run before bluetooth_init's
 * settings_load() and button_init (both literal 1), and before mount_fat
 * (CONFIG_APPLICATION_INIT_PRIORITY = 90) — the FAT volume must not be
 * mounted when the boot-path mkfs runs. */
SYS_INIT(factory_reset_boot_check, APPLICATION, 0);

#if defined(CONFIG_SHELL)

/* Shared body of both subcommands: warn, erase the scope, reboot. */
static int reset_and_reboot(const struct shell* sh, const char* warning, Scope scope) {
    shell_warn(sh, "%s", warning);

    factory_reset_perform(scope);

    // Give the shell transport a moment to flush the warning before the
    // reboot drops the USB connection.
    k_msleep(100);
    sys_reboot(SYS_REBOOT_COLD);
    return 0;
}

static int cmd_factory_reset_now(const struct shell* sh, size_t argc, char** argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    return reset_and_reboot(
        sh, "Factory reset: erasing all settings, coredumps, and files, then rebooting.",
        Scope::Everything);
}

/* The one-shot runtime equivalent of the boot gesture's phase-2 release.
 * 'appcfg erase' is the same settings-partition erase without the reboot. */
static int cmd_factory_reset_soft(const struct shell* sh, size_t argc, char** argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    return reset_and_reboot(
        sh,
        "Settings reset: erasing all settings (BT bonds included), then rebooting. "
        "Files are kept.",
        Scope::SettingsOnly);
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
