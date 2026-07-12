#include "power.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/reboot.h>

#if defined(CONFIG_TPS25750)
#include <zephyr/drivers/tps25750/tps25750.h>
#endif

#if defined(CONFIG_BQ25792)
#include <zephyr/drivers/bq25792/bq25792.h>
#include <status_led/status_led.h>
#endif

#if defined(CONFIG_APP_BATTERY_MONITOR)
// BT-free API only (see battery_service.h) - power.cpp stays free of BT headers.
#include <bluetooth/battery_service.h>
#endif

#if defined(CONFIG_FLASH)
#include <zephyr/drivers/flash.h>
#endif

// Needed to modify VDD voltage
#if defined(CONFIG_SOC_NRF5340_CPUAPP)
#include <nrf5340_application.h>
#include <nrf5340_application_bitfields.h>
#endif

#include <zephyr/logging/log.h>

#include <array>

LOG_MODULE_REGISTER(power);

#if !defined(CONFIG_ZTEST)
const struct device *pd = DEVICE_DT_GET(DT_NODELABEL(tps25750));
const struct device *bq = DEVICE_DT_GET(DT_NODELABEL(bq25792));
const struct device *flash = DEVICE_DT_GET(DT_NODELABEL(flash_controller));
#else
const struct device *pd = nullptr;
const struct device *bq = nullptr;
const struct device *flash = nullptr;
#endif

#if defined(CONFIG_SOC_NRF5340_CPUAPP)
/**
 * @brief Check if we are in 3.3v mode. If not, enable it and return true. Else return false if we
 * are already in 3.3v mode.
 *
 * @return true reboot is needed
 * @return false no reboot is needed
 */
bool check_and_enable_3v3(void) {
    uint32_t currentVreghvout = (NRF_UICR_S->VREGHVOUT & UICR_VREGHVOUT_VREGHVOUT_Msk);

    // First check the current mode
    if (currentVreghvout == UICR_VREGHVOUT_VREGHVOUT_3V3) {
        LOG_INF("System is in 3.3v mode!");
        return false;
    } else if (currentVreghvout != UICR_VREGHVOUT_VREGHVOUT_DEFAULT) {
        LOG_ERR(
            "Current UICR mode is non-default (%u != %lu)! Cannot change without a mass-chip-erase",
            currentVreghvout, UICR_VREGHVOUT_VREGHVOUT_DEFAULT);
        return false;
    }

    uint32_t newVreghvoutValue =
        (NRF_UICR_S->VREGHVOUT & ~UICR_VREGHVOUT_VREGHVOUT_Msk) | UICR_VREGHVOUT_VREGHVOUT_3V3;

    // Write the change using the flash APIs
    int ret = flash_write(flash, (uint32_t) & (NRF_UICR_S->VREGHVOUT), &newVreghvoutValue,
                          sizeof(newVreghvoutValue));

    if (ret) {
        LOG_ERR("Failed to write UICR->VREGHVOUT: %d", ret);
        return false;
    }

    return true;
}

static int init_check_and_enable_3v3(void) {
    if (check_and_enable_3v3()) {
        sys_reboot(SYS_REBOOT_WARM);

        // Does not return
    }

    return 0;
}

SYS_INIT(init_check_and_enable_3v3, APPLICATION, 0);
#endif  // CONFIG_SOC_NRF5340_CPUAPP

#if defined(CONFIG_BQ25792)

enum class Bq25792ChargeStatus : uint8_t {
    NotCharging           = 0,
    TrickleCharge         = 1,
    PreCharge             = 2,
    FastChargeCC          = 3,
    TaperChargeCV         = 4,
    Reserved              = 5,
    TopOffTimerActive     = 6,
    ChargeTerminationDone = 7,
};

static void charger_status_thread_func(void *, void *, void *);

// Kernel-only thread: K_KERNEL_* skips the 1KB CONFIG_USERSPACE privileged stack;
// this stack can never host a K_USER thread.
K_KERNEL_THREAD_DEFINE(charger_status_thread, 1024, charger_status_thread_func, NULL, NULL, NULL, 7, 0,
                0);

static void charger_status_thread_func(void *, void *, void *) {
    if (!device_is_ready(bq)) {
        LOG_ERR("BQ25792 not ready; charger status LED disabled");
        return;
    }

    /* Enable the ADC so VBAT readings are available, and override the NTC
     * thermal check (no thermistor fitted on proto0). */
    bq25792_adc_enable(bq, true);
    bq25792_temp_override(bq, true);

#if defined(CONFIG_APP_BATTERY_MONITOR)
    /* Enable IBAT sensing and apply the persisted charging-enable setting.
     * Safe to do here: settings_load() ran in bluetooth_init() at
     * SYS_INIT(APPLICATION, 1), before any K_THREAD_DEFINE thread runs. */
    battery_service_apply_boot_state();
#endif

    while (true) {
        /* Read VBAT first — the charger may report TrickleCharge even with no
         * battery present, so we must gate on voltage before trusting CHG_STAT. */
        int32_t vbat_mv = 0;
        bool vbat_ok    = (bq25792_get_vbat_mv(bq, &vbat_mv) == 0);

        uint8_t chg_stat = 0;
        bool chg_ok      = (bq25792_get_charge_status(bq, &chg_stat) == 0);

#if defined(CONFIG_APP_BATTERY_MONITOR)
        /* Publish battery telemetry to the BLE characteristics. Only publish a
         * complete, successfully-read sample — never garbage from a failed read. */
        int32_t ibat_ma = 0;
        int32_t vbus_mv = 0;
        int32_t ibus_ma = 0;
        if (vbat_ok && chg_ok && bq25792_get_ibat_ma(bq, &ibat_ma) == 0 &&
            bq25792_get_vbus_mv(bq, &vbus_mv) == 0 && bq25792_get_ibus_ma(bq, &ibus_ma) == 0) {
            battery_service_update(vbat_mv, ibat_ma, vbus_mv, ibus_ma, chg_stat);
        }
#endif

        if (chg_ok) {
#if defined(CONFIG_STATUS_LED)
            StatusIndication       indication;
            StatusColor            color  = StatusColor::Green;
            Bq25792ChargeStatus    status = static_cast<Bq25792ChargeStatus>(chg_stat);

            /* Dead/absent battery — override everything else with blinking red.
             * 6000 mV threshold: a healthy 2S pack reads >7V at rest; values
             * below 6V indicate a dead or disconnected pack. */
            if (vbat_ok && vbat_mv < 6000) {
                indication = StatusIndication::Blinking;
                color      = StatusColor::Red;
            } else {
                switch (status) {
                    case Bq25792ChargeStatus::TrickleCharge:
                        [[fallthrough]];
                    case Bq25792ChargeStatus::PreCharge:
                        indication = StatusIndication::Breathing;
                        break;
                    case Bq25792ChargeStatus::FastChargeCC:
                        [[fallthrough]];
                    case Bq25792ChargeStatus::TaperChargeCV:
                        indication = StatusIndication::FastBreathing;
                        break;
                    case Bq25792ChargeStatus::TopOffTimerActive:
                        [[fallthrough]];
                    case Bq25792ChargeStatus::ChargeTerminationDone:
                        indication = StatusIndication::Solid;
                        break;
                    default: { /* NotCharging, Reserved — running on battery */
                        /* 2S LiPo: cutoff 7000 mV (0%), full 8400 mV (100%).
                         * Thresholds: >=7700 mV = >=50% green, >=7350 mV = >=25% orange,
                         * >=7000 mV = >=0% red. */
                        if (vbat_ok) {
                            if (vbat_mv >= 7700) {
                                indication = StatusIndication::Solid;
                                color      = StatusColor::Green;
                            } else if (vbat_mv >= 7350) {
                                indication = StatusIndication::Solid;
                                color      = StatusColor::Orange;
                            } else {
                                indication = StatusIndication::Solid;
                                color      = StatusColor::Red;
                            }
                        } else {
                            indication = StatusIndication::Off;
                        }
                        break;
                    }
                }
            }

            status_led_set(0, indication, color);
#endif /* CONFIG_STATUS_LED */
        }

        k_msleep(500);
    }
}

#endif /* CONFIG_BQ25792 */

#if defined(CONFIG_SHELL)
#include <zephyr/shell/shell.h>

static int cmd_power_bq_dump(const struct shell *shell, size_t argc, char **argv, void *data) {
    bq25792_dump(bq);
    return 0;
}

static int cmd_power_bq_temp_override(const struct shell *shell, size_t argc, char **argv,
                                      void *data) {
    int selection = (int)data;
    bq25792_temp_override(bq, (bool)selection);
    return 0;
}

static int cmd_power_bq_adc_enable(const struct shell *shell, size_t argc, char **argv,
                                   void *data) {
    int selection = (int)data;
    bq25792_adc_enable(bq, (bool)selection);
    return 0;
}

static int cmd_power_bq_pfm_enable(const struct shell *shell, size_t argc, char **argv,
                                   void *data) {
    int selection = (int)data;
    bq25792_pfm_enable(bq, (bool)selection);
    return 0;
}

static int cmd_power_bq_freq_change(const struct shell *shell, size_t argc, char **argv,
                                    void *data) {
    int selection = (int)data;
    bq25792_set_charge_frequency(bq, (bq25792_charge_frequency_t)selection);
    return 0;
}

static int cmd_power_bq_dump_charge_params(const struct shell *shell, size_t argc, char **argv,
                                           void *data) {
    bq25792_dump_charge_parameters(bq);
    return 0;
}

static int cmd_power_bq_charge_enable(const struct shell *shell, size_t argc, char **argv,
                                      void *data) {
    int selection = (int)data;
    // Known accepted gap: this bypasses the BLE "Charging Enabled" characteristic
    // (battery_service.cpp), so a connected app shows a stale toggle until it
    // re-reads. Debug-only command; the app path is the source of truth.
    bq25792_set_charge_enable(bq, (bool)selection);
    return 0;
}

static int cmd_power_bq_status(const struct shell *shell, size_t argc, char **argv, void *data) {
    int32_t vbat_mv  = 0;
    int32_t ibat_ma  = 0;
    int32_t vbus_mv  = 0;
    int32_t ibus_ma  = 0;
    uint8_t chg_stat = 0;
    bool en_chg      = false;

    bq25792_get_vbat_mv(bq, &vbat_mv);
    bq25792_get_ibat_ma(bq, &ibat_ma);
    bq25792_get_vbus_mv(bq, &vbus_mv);
    bq25792_get_ibus_ma(bq, &ibus_ma);
    bq25792_get_charge_status(bq, &chg_stat);
    bq25792_get_charge_enable(bq, &en_chg);

    /* Integer prints only - no %f (CONFIG_CBPRINTF_FP_SUPPORT=n). */
    shell_print(shell, "VBAT=%d mV IBAT=%d mA VBUS=%d mV IBUS=%d mA CHG_STAT=%u EN_CHG=%u",
                vbat_mv, ibat_ma, vbus_mv, ibus_ma, chg_stat, en_chg ? 1 : 0);
    return 0;
}

static int cmd_power_bq_limits(const struct shell *shell, size_t argc, char **argv, void *data) {
    // Read-only diagnostic: the whole picture for the "charging too slow"
    // class of symptom — which DPM limit is engaged, what the limit registers
    // actually contain (and thus who programmed them), and whether the I2C
    // watchdog has been expiring behind our back.
    struct bq25792_limits limits;
    int ret = bq25792_get_limits(bq, &limits);
    if (ret) {
        shell_error(shell, "bq25792_get_limits failed: %d", ret);
        return ret;
    }

    struct bq25792_status st;
    ret = bq25792_get_status(bq, &st);
    if (ret) {
        shell_error(shell, "bq25792_get_status failed: %d", ret);
        return ret;
    }

    /* REG10 field encodings, datasheet SLUSDG1C Table 9-26 */
    static const char *const kWatchdog[] = {"disabled", "0.5s", "1s",  "2s",
                                            "20s",      "40s",  "80s", "160s"};
    static const char *const kVacOvp[] = {"26V", "18V", "12V", "7V"};

    shell_print(shell, "ICHG=%u mA  IINDPM=%u mA  VINDPM=%u mV  ICO_ILIM=%u mA", limits.ichg_ma,
                limits.iindpm_ma, limits.vindpm_mv, limits.ico_ilim_ma);
    shell_print(shell, "WATCHDOG=%s  VAC_OVP=%s", kWatchdog[limits.watchdog & 0x7],
                kVacOvp[limits.vac_ovp & 0x3]);
    shell_print(shell, "IINDPM_STAT=%u VINDPM_STAT=%u WD_STAT=%u POORSRC=%u PG=%u VBUS_PRESENT=%u",
                st.iindpm_active ? 1 : 0, st.vindpm_active ? 1 : 0, st.wd_expired ? 1 : 0,
                st.poor_source ? 1 : 0, st.power_good ? 1 : 0, st.vbus_present ? 1 : 0);
    shell_print(shell, "CHG_STAT=%u  VBUS_STAT=%u (%s)  BC1.2_DONE=%u", st.chg_stat, st.vbus_stat,
                bq25792_vbus_stat_str(st.vbus_stat), st.bc12_done ? 1 : 0);
    shell_print(shell, "ICO_STAT=%u TREG=%u DPDM_ONGOING=%u VBAT_PRESENT=%u VSYSMIN_REG=%u",
                st.ico_stat, st.thermal_regulation ? 1 : 0, st.dpdm_ongoing ? 1 : 0,
                st.vbat_present ? 1 : 0, st.vsysmin_regulation ? 1 : 0);

    int32_t vsys_mv = 0;
    ret = bq25792_get_vsys_mv(bq, &vsys_mv);
    if (ret) {
        shell_warn(shell, "VSYS read failed: %d (ADC enabled?)", ret);
    } else {
        shell_print(shell, "VSYS=%d mV", vsys_mv);
    }

    // Legacy getters always return 0 for non-null args and swallow I2C errors
    // (fw/CLAUDE.md power-subsystem caveat) — a failed read shows as 0/stale.
    int32_t vbat_mv = 0, ibat_ma = 0, vbus_mv = 0, ibus_ma = 0;
    bq25792_get_vbat_mv(bq, &vbat_mv);
    bq25792_get_ibat_ma(bq, &ibat_ma);
    bq25792_get_vbus_mv(bq, &vbus_mv);
    bq25792_get_ibus_ma(bq, &ibus_ma);
    shell_print(shell, "VBAT=%d mV IBAT=%d mA VBUS=%d mV IBUS=%d mA", vbat_mv, ibat_ma, vbus_mv,
                ibus_ma);

    return 0;
}

static int cmd_power_pd_dump(const struct shell *shell, size_t argc, char **argv, void *data) {
    tps25750_dump(pd);
    return 0;
}

static int cmd_power_pd_contract(const struct shell *shell, size_t argc, char **argv, void *data) {
    // Read-only diagnostic: what input power budget did the TPS25750 actually
    // negotiate, and what could it negotiate (advertised sink caps)?
    struct tps25750_pd_power_info info;
    int ret = tps25750_get_pd_power_info(pd, &info);
    if (ret) {
        shell_error(shell, "tps25750_get_pd_power_info failed: %d", ret);
        return ret;
    }

    static const char *const kSource[] = {
        "none",       "Type-C default (500mA)",  "Type-C 1.5A",
        "Type-C 3.0A", "explicit PD contract",   "unknown/undecoded contract",
    };
    shell_print(shell, "connected=%u sinking=%u source=%s", info.connected ? 1 : 0,
                info.sinking ? 1 : 0, kSource[info.source]);
    shell_print(shell, "available: %u mV @ %u mA (%u mW)", info.available_mv, info.available_ma,
                (uint32_t)(((uint64_t)info.available_mv * info.available_ma) / 1000));
    shell_print(shell, "raw: POWER_STATUS=0x%04x PDO=0x%08x RDO=0x%08x", info.raw_power_status,
                info.raw_pdo, info.raw_rdo);

    uint32_t pd_status = 0;
    ret = tps25750_read_pd_status(pd, &pd_status);
    if (ret == 0) {
        /* CCPullUp bits 3:2 (TRM Table 2-35): 0 none / 1 default / 2 1.5A / 3 3.0A */
        static const char *const kCcPullUp[] = {"none", "default", "1.5A", "3.0A"};
        shell_print(shell, "PD_STATUS=0x%08x (role=%s, CCPullUp=%s)", pd_status,
                    (pd_status & BIT(6)) ? "source" : "sink", kCcPullUp[(pd_status >> 2) & 0x3]);
    } else {
        shell_warn(shell, "PD_STATUS read failed: %d", ret);
    }

    uint32_t sink_pdos[7] = {0};
    uint8_t num_pdos = 0;
    ret = tps25750_read_tx_sink_caps(pd, sink_pdos, ARRAY_SIZE(sink_pdos), &num_pdos);
    if (ret == 0) {
        shell_print(shell, "advertised sink caps: %u PDO(s)", num_pdos);
        for (uint8_t i = 0; i < MIN(num_pdos, (uint8_t)ARRAY_SIZE(sink_pdos)); i++) {
            uint32_t pdo = sink_pdos[i];
            if ((pdo >> 30) == 0x0) {
                shell_print(shell, "  [%u] fixed: %u mV @ %u mA (raw 0x%08x)", i,
                            ((pdo >> 10) & 0x3FF) * 50, (pdo & 0x3FF) * 10, pdo);
            } else {
                shell_print(shell, "  [%u] non-fixed PDO (raw 0x%08x)", i, pdo);
            }
        }
    } else {
        shell_warn(shell, "TX_SINK_CAPS read failed: %d", ret);
    }

    return 0;
}

static int cmd_power_pd_clear_dead_battery(const struct shell *shell, size_t argc, char **argv,
                                           void *data) {
    tps25750_clear_dead_battery(pd);
    return 0;
}

static int cmd_power_pd_patch(const struct shell *shell, size_t argc, char **argv, void *data) {
    shell_print(shell, "Sending '%s' patch to device...", argv[0]);

    int selection = (int)data;
    if (selection == 1) {
        const char *patch;
        size_t patch_size;
        tps25750_get_patch(&patch, &patch_size);
        tps25750_download_patch(pd, patch, patch_size);
    } else if (selection == 2) {
        // tps25750_download_patch(pd, tps25750x_fullFlash_i2c_array, gSizeFullFlashArray);
        shell_error(shell, "Full Flash not supported");
    } else if (selection == 3) {
        tps25750_download_patch(pd, NULL, 0);
    } else {
        shell_error(shell, "Unknown patch type %d", selection);
        return -ENOEXEC;
    }

    return 0;
}

static int cmd_power_sys_vreghvout(const struct shell *shell, size_t argc, char **argv,
                                   void *data) {
    uint32_t currentVreghvout = (NRF_UICR_S->VREGHVOUT & UICR_VREGHVOUT_VREGHVOUT_Msk);

    if (currentVreghvout == UICR_VREGHVOUT_VREGHVOUT_3V3) {
        shell_print(shell, "VREGHVOUT = 0x%08X (3.3V)", currentVreghvout);
    } else if (currentVreghvout == UICR_VREGHVOUT_VREGHVOUT_DEFAULT) {
        shell_print(shell, "VREGHVOUT = 0x%08X (default/1.8V)", currentVreghvout);
    } else {
        shell_print(shell, "VREGHVOUT = 0x%08X (unknown)", currentVreghvout);
    }

    return 0;
}

static int cmd_power_sys_boost(const struct shell *shell, size_t argc, char **argv, void *data) {
    uint32_t currentVreghvout = (NRF_UICR_S->VREGHVOUT & UICR_VREGHVOUT_VREGHVOUT_Msk);

    // First check the current mode
    if (currentVreghvout == UICR_VREGHVOUT_VREGHVOUT_3V3) {
        shell_print(shell, "Current UICR already indicates 3.3v output! Nothing to do");
        return 0;
    } else if (currentVreghvout != UICR_VREGHVOUT_VREGHVOUT_DEFAULT) {
        shell_print(
            shell,
            "Current UICR mode is non-default (%u != %lu)! Cannot change without a mass-chip-erase",
            currentVreghvout, UICR_VREGHVOUT_VREGHVOUT_DEFAULT);
        return -ENOEXEC;
    }

    uint32_t newVreghvoutValue =
        (NRF_UICR_S->VREGHVOUT & ~UICR_VREGHVOUT_VREGHVOUT_Msk) | UICR_VREGHVOUT_VREGHVOUT_3V3;

    // Write the change using the flash APIs
    int ret = flash_write(flash, (uint32_t) & (NRF_UICR_S->VREGHVOUT), &newVreghvoutValue,
                          sizeof(newVreghvoutValue));

    if (ret) {
        shell_error(shell, "Failed to write UICR->VREGHVOUT: %d", ret);
        return ret;
    } else {
        shell_print(shell, "UICR updated! Restart device");
    }

    return 0;
}

SHELL_SUBCMD_DICT_SET_CREATE(sub_patch, cmd_power_pd_patch, (low_region, 1, "low-region binary"),
                             (full_flash, 2, "full-flash binary"),
                             (none, 3, "No patch, just boot"));

// Subcommands for "power pd"
SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_power_pd, SHELL_CMD(dump, NULL, "Dump TPS25750 Registers to console", cmd_power_pd_dump),
    SHELL_CMD(contract, NULL,
              "Print negotiated PD contract / Type-C budget + advertised sink caps (read-only)",
              cmd_power_pd_contract),
    SHELL_CMD(clear_dbfg, NULL, "Clear TPS25750 dead battery flag",
              cmd_power_pd_clear_dead_battery),
    SHELL_CMD(patch, &sub_patch, "Download TPS25750 firmware patch", NULL), SHELL_SUBCMD_SET_END);

SHELL_SUBCMD_DICT_SET_CREATE(sub_temp_override, cmd_power_bq_temp_override,
                             (disable, 0, "disable temp monitor override"),
                             (enable, 1, "enable temp monitor override"));

SHELL_SUBCMD_DICT_SET_CREATE(sub_adc, cmd_power_bq_adc_enable, (disable, 0, "disable internal adc"),
                             (enable, 1, "enable internal adc"));

SHELL_SUBCMD_DICT_SET_CREATE(sub_pfm, cmd_power_bq_pfm_enable, (disable, 0, "disable PFM"),
                             (enable, 1, "enable PFM"));

SHELL_SUBCMD_DICT_SET_CREATE(sub_freq, cmd_power_bq_freq_change,
                             (high, bq25792_charge_frequency_t::HIGH, "1.5Mhz PWM Frequency"),
                             (low, bq25792_charge_frequency_t::LOW, "750 Khz PWM Frequency"));

SHELL_SUBCMD_DICT_SET_CREATE(sub_charge_enable, cmd_power_bq_charge_enable,
                             (disable, 0, "disable charging"), (enable, 1, "enable charging"));

SHELL_STATIC_SUBCMD_SET_CREATE(sub_charge,
                               SHELL_CMD(dump, NULL, "Dump BQ25792 Charging Parameters to console",
                                         cmd_power_bq_dump_charge_params),
                               SHELL_CMD(enable, &sub_charge_enable,
                                         "Enable/Disable BQ25792 Charging", NULL),
                               SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_power_bq, SHELL_CMD(dump, NULL, "Dump BQ25792 Registers to console", cmd_power_bq_dump),
    SHELL_CMD(status, NULL, "Print battery/VBUS voltage, current, charge status and EN_CHG",
              cmd_power_bq_status),
    SHELL_CMD(limits, NULL,
              "Print ICHG/IINDPM/VINDPM/ICO/watchdog readbacks + DPM status flags (read-only)",
              cmd_power_bq_limits),
    SHELL_CMD(temp_override, &sub_temp_override, "Override BQ25792 battery temperature monitoring",
              NULL),
    SHELL_CMD(adc, &sub_adc, "Enable/Disable BQ25792 ADC", NULL),
    SHELL_CMD(pfm, &sub_pfm, "Enable/Disable BQ25792 Pulse Frequency Modulation (PFM)", NULL),
    SHELL_CMD(freq, &sub_freq, "Change BQ25792 PWM Frequency", NULL),
    SHELL_CMD(charge, &sub_charge, "Change BQ25792 Charge Parameters", NULL), SHELL_SUBCMD_SET_END);
// Subcommands for "power"
SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_power, SHELL_CMD(pd, &sub_power_pd, "TPS25750 PD Controller Commands", NULL),
    SHELL_CMD(bq, &sub_power_bq, "BQ25792 Battery Charger Commands", NULL),
    SHELL_CMD(boost, NULL, "Increase NRF5340 VDD to 3.3v", cmd_power_sys_boost),
    SHELL_CMD(vreghvout, NULL, "Print current VREGHVOUT register value", cmd_power_sys_vreghvout),
    SHELL_SUBCMD_SET_END);

/* Creating root (level 0) command "power" */
SHELL_CMD_REGISTER(power, &sub_power, "Power commands", NULL);

#endif