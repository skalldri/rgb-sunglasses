#include <zephyr/shell/shell.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#include <zephyr/drivers/tps25750/tps25750.h>

static int cmd_power_pd_dump(const struct shell *shell,
                            size_t argc, char **argv, void *data)
{   
    const struct device *pd = DEVICE_DT_GET(DT_NODELABEL(tps25750));

    tps25750_dump(pd);
    return 0;
}

// Subcommands for "power pd"
SHELL_STATIC_SUBCMD_SET_CREATE(sub_power_pd,
        SHELL_CMD(dump,   NULL, "Dump TPS25750 Registers to console", cmd_power_pd_dump),
        SHELL_SUBCMD_SET_END
);

// Subcommands for "power"
SHELL_STATIC_SUBCMD_SET_CREATE(sub_power,
        SHELL_CMD(pd, &sub_power_pd, "TPS25750 PD Controller Commands", NULL),
        SHELL_SUBCMD_SET_END
);

/* Creating root (level 0) command "demo" */
SHELL_CMD_REGISTER(power, &sub_power, "Power commands", NULL);