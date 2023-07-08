#include <zephyr/shell/shell.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#include <zephyr/drivers/vm3011/vm3011.h>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(sound);

const struct device *vm3011 = DEVICE_DT_GET(DT_NODELABEL(vm3011));
const struct device *pdm0 = DEVICE_DT_GET(DT_NODELABEL(pdm0));

static int cmd_sound_vm_dump(const struct shell *shell,
                             size_t argc, char **argv, void *data)
{
    vm3011_dump(vm3011);
    return 0;
}

static int cmd_sound_vm_clear(const struct shell *shell,
                             size_t argc, char **argv, void *data)
{
    vm3011_clear_dout(vm3011);
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_sound_vm,
                               SHELL_CMD(dump, NULL, "Dump VM3011 Registers to console", cmd_sound_vm_dump),
                               SHELL_CMD(clear, NULL, "Clera VM3011 DOUT pin", cmd_sound_vm_clear),
                               SHELL_SUBCMD_SET_END);

// Subcommands for "sound"
SHELL_STATIC_SUBCMD_SET_CREATE(sub_sound,
                               SHELL_CMD(vm, &sub_sound_vm, "VM3011 Commands", NULL),
                               SHELL_SUBCMD_SET_END);

/* Creating root (level 0) command "sound" */
SHELL_CMD_REGISTER(sound, &sub_sound, "Sound commands", NULL);