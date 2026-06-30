#include <storage/appcfg_erase.h>
#include <zephyr/storage/flash_map.h>

#if defined(CONFIG_SHELL)
#include <zephyr/shell/shell.h>
#endif

int storage_erase_settings_partition(void) {
    const struct flash_area *fa;
    int rc = flash_area_open(FIXED_PARTITION_ID(settings_storage), &fa);
    if (rc) {
        return rc;
    }
    rc = flash_area_erase(fa, 0, fa->fa_size);
    flash_area_close(fa);
    return rc;
}

#if defined(CONFIG_SHELL)

static int cmd_appcfg_erase(const struct shell *sh, size_t argc, char **argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    shell_warn(sh, "Erasing settings partition — all config will reset to defaults on next boot.");

    int rc = storage_erase_settings_partition();
    if (rc) {
        shell_error(sh, "Erase failed: %d", rc);
        return rc;
    }

    shell_print(sh, "Done. Reboot to apply.");
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_appcfg,
    SHELL_CMD(erase, NULL,
              "Erase the NVS settings partition (DESTRUCTIVE — resets all config on next boot)",
              cmd_appcfg_erase),
    SHELL_SUBCMD_SET_END);
/* "appcfg" matches the key prefix used in persistent_value_store.cpp. */
SHELL_CMD_REGISTER(appcfg, &sub_appcfg, "Application config (NVS settings) management", NULL);

#endif /* CONFIG_SHELL */
