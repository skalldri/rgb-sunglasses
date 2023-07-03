#include <zephyr/shell/shell.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#include <zephyr/drivers/tps25750/tps25750.h>
#include <zephyr/drivers/bq25792/bq25792.h>
#include <zephyr/drivers/flash.h>

#include <tps25750/tps25750-config.h>

#include <zephyr/sys/reboot.h>

#include <zephyr/logging/log.h>

// Needed to modify VDD voltage
#include <nrf5340_application.h>
#include <nrf5340_application_bitfields.h>

LOG_MODULE_REGISTER(power);

const struct device *pd = DEVICE_DT_GET(DT_NODELABEL(tps25750));
const struct device *bq = DEVICE_DT_GET(DT_NODELABEL(bq25792));
const struct device *flash = DEVICE_DT_GET(DT_NODELABEL(flash_controller));

/**
 * @brief Check if we are in 3.3v mode. If not, enable it and return true. Else return false if we are already in 3.3v mode.
 *
 * @return true reboot is needed
 * @return false no reboot is needed
 */
bool check_and_enable_3v3(void)
{
    uint32_t currentVreghvout = (NRF_UICR_S->VREGHVOUT & UICR_VREGHVOUT_VREGHVOUT_Msk);

    // First check the current mode
    if (currentVreghvout == UICR_VREGHVOUT_VREGHVOUT_3V3)
    {
        LOG_INF("System is in 3.3v mode!");
        return false;
    }
    else if (currentVreghvout != UICR_VREGHVOUT_VREGHVOUT_DEFAULT)
    {
        LOG_ERR(
            "Current UICR mode is non-default (%u != %lu)! Cannot change without a mass-chip-erase",
            currentVreghvout,
            UICR_VREGHVOUT_VREGHVOUT_DEFAULT);
        return false;
    }

    uint32_t newVreghvoutValue = (NRF_UICR_S->VREGHVOUT & ~UICR_VREGHVOUT_VREGHVOUT_Msk) | UICR_VREGHVOUT_VREGHVOUT_3V3;

    // Write the change using the flash APIs
    int ret = flash_write(flash, (uint32_t) & (NRF_UICR_S->VREGHVOUT), &newVreghvoutValue, sizeof(newVreghvoutValue));

    if (ret)
    {
        LOG_ERR("Failed to write UICR->VREGHVOUT: %d", ret);
        return false;
    }
    
    return true;
}

static int cmd_power_bq_dump(const struct shell *shell,
                             size_t argc, char **argv, void *data)
{
    bq25792_dump(bq);
    return 0;
}

static int cmd_power_bq_temp_override(const struct shell *shell,
                             size_t argc, char **argv, void *data)
{
    int selection = (int)data;
    bq25792_temp_override(bq, (bool) selection);
    return 0;
}

static int cmd_power_bq_adc_enable(const struct shell *shell,
                             size_t argc, char **argv, void *data)
{
    int selection = (int)data;
    bq25792_adc_enable(bq, (bool) selection);
    return 0;
}

static int cmd_power_bq_pfm_enable(const struct shell *shell,
                             size_t argc, char **argv, void *data)
{
    int selection = (int)data;
    bq25792_pfm_enable(bq, (bool) selection);
    return 0;
}

static int cmd_power_bq_freq_change(const struct shell *shell,
                             size_t argc, char **argv, void *data)
{
    int selection = (int)data;
    bq25792_set_charge_frequency(bq, (bq25792_charge_frequency_t) selection);
    return 0;
}

static int cmd_power_pd_dump(const struct shell *shell,
                             size_t argc, char **argv, void *data)
{
    tps25750_dump(pd);
    return 0;
}

static int cmd_power_pd_clear_dead_battery(const struct shell *shell,
                                           size_t argc, char **argv, void *data)
{
    tps25750_clear_dead_battery(pd);
    return 0;
}

static int cmd_power_pd_patch(const struct shell *shell,
                              size_t argc, char **argv, void *data)
{
    shell_print(shell, "Sending '%s' patch to device...", argv[0]);

    int selection = (int)data;
    if (selection == 1)
    {
        tps25750_download_patch(pd, tps25750x_lowRegion_i2c_array, gSizeLowRegionArray);
    }
    else if (selection == 2)
    {
        // tps25750_download_patch(pd, tps25750x_fullFlash_i2c_array, gSizeFullFlashArray);
        shell_error(shell, "Full Flash not supported");
    }
    else if (selection == 3)
    {
        tps25750_download_patch(pd, NULL, 0);
    }
    else
    {
        shell_error(shell, "Unknown patch type %d", selection);
        return -ENOEXEC;
    }

    return 0;
}

static int cmd_power_sys_boost(const struct shell *shell,
                               size_t argc, char **argv, void *data)
{
    uint32_t currentVreghvout = (NRF_UICR_S->VREGHVOUT & UICR_VREGHVOUT_VREGHVOUT_Msk);

    // First check the current mode
    if (currentVreghvout == UICR_VREGHVOUT_VREGHVOUT_3V3)
    {
        shell_print(
            shell,
            "Current UICR already indicates 3.3v output! Nothing to do");
        return 0;
    }
    else if (currentVreghvout != UICR_VREGHVOUT_VREGHVOUT_DEFAULT)
    {
        shell_print(
            shell,
            "Current UICR mode is non-default (%u != %lu)! Cannot change without a mass-chip-erase",
            currentVreghvout,
            UICR_VREGHVOUT_VREGHVOUT_DEFAULT);
        return -ENOEXEC;
    }

    uint32_t newVreghvoutValue = (NRF_UICR_S->VREGHVOUT & ~UICR_VREGHVOUT_VREGHVOUT_Msk) | UICR_VREGHVOUT_VREGHVOUT_3V3;

    // Write the change using the flash APIs
    int ret = flash_write(flash, (uint32_t) & (NRF_UICR_S->VREGHVOUT), &newVreghvoutValue, sizeof(newVreghvoutValue));

    if (ret)
    {
        shell_error(shell, "Failed to write UICR->VREGHVOUT: %d", ret);
        return ret;
    }
    else
    {
        shell_print(shell, "UICR updated! Restart device");
    }

    return 0;
}

SHELL_SUBCMD_DICT_SET_CREATE(sub_patch, cmd_power_pd_patch,
                             (low_region, 1, "low-region binary"),
                             (full_flash, 2, "full-flash binary"),
                             (none, 3, "No patch, just boot"));

// Subcommands for "power pd"
SHELL_STATIC_SUBCMD_SET_CREATE(sub_power_pd,
                               SHELL_CMD(dump, NULL, "Dump TPS25750 Registers to console", cmd_power_pd_dump),
                               SHELL_CMD(clear_dbfg, NULL, "Clear TPS25750 dead battery flag", cmd_power_pd_clear_dead_battery),
                               SHELL_CMD(patch, &sub_patch, "Download TPS25750 firmware patch", NULL),
                               SHELL_SUBCMD_SET_END);

SHELL_SUBCMD_DICT_SET_CREATE(sub_temp_override, cmd_power_bq_temp_override,
                             (disable, 0, "disable temp monitor override"),
                             (enable, 1, "enable temp monitor override"));

SHELL_SUBCMD_DICT_SET_CREATE(sub_adc, cmd_power_bq_adc_enable,
                             (disable, 0, "disable internal adc"),
                             (enable, 1, "enable internal adc"));

SHELL_SUBCMD_DICT_SET_CREATE(sub_pfm, cmd_power_bq_pfm_enable,
                             (disable, 0, "disable PFM"),
                             (enable, 1, "enable PFM"));

SHELL_SUBCMD_DICT_SET_CREATE(sub_freq, cmd_power_bq_freq_change,
                             (high, bq25792_charge_frequency_t::HIGH, "1.5Mhz PWM Frequency"),
                             (low, bq25792_charge_frequency_t::LOW, "750 Khz PWM Frequency"));

SHELL_STATIC_SUBCMD_SET_CREATE(sub_power_bq,
                               SHELL_CMD(dump, NULL, "Dump BQ25792 Registers to console", cmd_power_bq_dump),
                               SHELL_CMD(temp_override, &sub_temp_override, "Override BQ25792 battery temperature monitoring", NULL),
                               SHELL_CMD(adc, &sub_adc, "Enable/Disable BQ25792 ADC", NULL),
                               SHELL_CMD(pfm, &sub_pfm, "Enable/Disable BQ25792 Pulse Frequency Modulation (PFM)", NULL),
                               SHELL_CMD(freq, &sub_freq, "Change BQ25792 PWM Frequency", NULL),
                               SHELL_SUBCMD_SET_END);
// Subcommands for "power"
SHELL_STATIC_SUBCMD_SET_CREATE(sub_power,
                               SHELL_CMD(pd, &sub_power_pd, "TPS25750 PD Controller Commands", NULL),
                               SHELL_CMD(bq, &sub_power_bq, "BQ25792 Battery Charger Commands", NULL),
                               SHELL_CMD(boost, NULL, "Increase NRF5340 VDD to 3.3v", cmd_power_sys_boost),
                               SHELL_SUBCMD_SET_END);

/* Creating root (level 0) command "demo" */
SHELL_CMD_REGISTER(power, &sub_power, "Power commands", NULL);

static int init_check_and_enable_3v3(const struct device *dev)
{
    if (check_and_enable_3v3()) {
        sys_reboot(SYS_REBOOT_WARM);

        // Does not return
    }

    return 0;
}

SYS_INIT(init_check_and_enable_3v3, APPLICATION, 0);