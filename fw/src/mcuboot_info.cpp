#include <bluetooth/bt_gatt_traits.h>
#include <bluetooth/bt_service_cpp.h>
#include <bootutil/boot_status.h>
#include <bootutil/image.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/retention/blinfo.h>
#include <zephyr/shell/shell.h>

LOG_MODULE_REGISTER(mcuboot_info, LOG_LEVEL_INF);

static constexpr bt_uuid_128 kMcubootInfoServiceUuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 3, 0x56789abc0000));

BtGattPrimaryService<kMcubootInfoServiceUuid> mcubootInfoPrimaryService;
BtGattAutoReadOnlyCharacteristic<"MCUboot Version", BtGattString<32>, BtGattString<32>{}>
    mcubootVersionCharacteristic;

BtGattServer mcubootInfoServer(mcubootInfoPrimaryService, mcubootVersionCharacteristic);
BT_GATT_SERVER_REGISTER(mcubootInfoServerStatic, mcubootInfoServer);

static void read_and_format_version(char* buf, size_t bufsz) {
    struct image_version ver = {};
    int rc = blinfo_lookup(BLINFO_BOOTLOADER_VERSION, (char*)&ver, sizeof(ver));
    if (rc == (int)sizeof(ver)) {
        snprintf(buf, bufsz, "%u.%u.%u+%u", ver.iv_major, ver.iv_minor, ver.iv_revision,
                 ver.iv_build_num);
    } else {
        snprintf(buf, bufsz, "<unavailable rc=%d>", rc);
    }
}

static int mcuboot_info_init(void) {
    char buf[32];
    read_and_format_version(buf, sizeof(buf));
    LOG_INF("MCUboot version: %s", buf);

    BtGattString<32> s = {};
    strncpy(s.data(), buf, sizeof(buf) - 1);
    mcubootVersionCharacteristic = s;
    return 0;
}
/* blinfo_init runs at APPLICATION, CONFIG_RETENTION_BOOTLOADER_INFO_INIT_PRIORITY.
 * Run one step later so blinfo has already parsed the retained-mem TLV area. */
#define MCUBOOT_INFO_INIT_PRIORITY (CONFIG_RETENTION_BOOTLOADER_INFO_INIT_PRIORITY + 1)
SYS_INIT(mcuboot_info_init, APPLICATION, MCUBOOT_INFO_INIT_PRIORITY);

static int cmd_mcuboot_version(const struct shell* sh, size_t argc, char** argv) {
    char buf[32];
    read_and_format_version(buf, sizeof(buf));
    shell_print(sh, "MCUboot version: %s", buf);
    return 0;
}
SHELL_CMD_REGISTER(mcuboot_version, NULL, "Print MCUboot bootloader version", cmd_mcuboot_version);
