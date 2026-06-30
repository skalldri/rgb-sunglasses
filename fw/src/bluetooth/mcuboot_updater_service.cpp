#include <mcuboot_updater.h>
#include <string.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(mcuboot_updater_svc, LOG_LEVEL_INF);

/*
 * Service UUID (service ID 4, continuing the 56789abc0000 scheme):
 *   12345678-1234-5678-0004-56789abc0000
 *
 * Using raw BT_GATT_SERVICE_DEFINE (not BtGattServer<>) because:
 * - The Data characteristic uses WRITE_WITHOUT_RESP (command stream, not stored value)
 * - The Control characteristic is purely command-driven, not data-backed
 * Neither maps to the storage-centric onWrite() pattern the template is designed for.
 */

static const struct bt_uuid_128 kServiceUuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 4, 0x56789abc0000));

/* Status (R+Notify): 4 bytes {state:u8, progress:u8, error:u8, flash_unlocked:u8} */
static const struct bt_uuid_128 kStatusUuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 4, 0x56789abc0001));

/* Data (Write without response): raw binary package chunks, ≤244 bytes each */
static const struct bt_uuid_128 kDataUuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 4, 0x56789abc0002));

/* Control (Write with response): command byte + optional payload */
static const struct bt_uuid_128 kControlUuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 4, 0x56789abc0003));

/*
 * Control command bytes:
 *   0x01 <size:u32 LE>  — Begin upload (5-byte payload)
 *   0x02                — Validate
 *   0x03                — Commit (irreversible — flashes and reboots)
 *   0x04                — Abort (returns to LOCKED)
 *   0x05                — Unlock (LOCKED → IDLE)
 *   0x06                — RequestUpdaterReboot: sets boot-mode=0xB1, reboots in 200 ms
 */
static constexpr uint8_t kCmdBegin                = 0x01;
static constexpr uint8_t kCmdValidate             = 0x02;
static constexpr uint8_t kCmdCommit               = 0x03;
static constexpr uint8_t kCmdAbort                = 0x04;
static constexpr uint8_t kCmdUnlock               = 0x05;
static constexpr uint8_t kCmdRequestUpdaterReboot = 0x06;

/* ============================================================================
 * Attribute callbacks
 * ============================================================================ */
static ssize_t status_read_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                               void *buf, uint16_t len, uint16_t offset)
{
    struct McubootUpdaterStatus st = mcuboot_updater_get_status();
    uint8_t packed[4] = {
        (uint8_t)st.state,
        st.progress,
        (uint8_t)st.error,
        st.flash_unlocked,
    };
    return bt_gatt_attr_read(conn, attr, buf, len, offset, packed, sizeof(packed));
}

static void ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    LOG_DBG("MCUboot updater status notifications %s",
            (value == BT_GATT_CCC_NOTIFY) ? "enabled" : "disabled");
}

static ssize_t data_write_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                              const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(offset);
    ARG_UNUSED(flags);

    if (len == 0 || len > 244) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    int rc = mcuboot_updater_write_chunk((const uint8_t *)buf, len);
    if (rc == -EBUSY) {
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }
    if (rc == -EOVERFLOW) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    if (rc != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }
    return (ssize_t)len;
}

static ssize_t control_write_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                 const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(offset);

    if (flags & BT_GATT_WRITE_FLAG_PREPARE) {
        return 0;
    }
    if (len < 1) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    const uint8_t *cmd = (const uint8_t *)buf;
    int rc;

    switch (cmd[0]) {
    case kCmdUnlock:
        rc = mcuboot_updater_unlock();
        break;

    case kCmdBegin: {
        if (len != 5) {
            return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
        }
        uint32_t size;
        memcpy(&size, cmd + 1, 4); /* LE u32 */
        rc = mcuboot_updater_begin(size);
        break;
    }

    case kCmdValidate:
        rc = mcuboot_updater_validate();
        break;

    case kCmdCommit:
        rc = mcuboot_updater_commit();
        break;

    case kCmdAbort:
        mcuboot_updater_abort();
        rc = 0;
        break;

    case kCmdRequestUpdaterReboot:
        rc = mcuboot_updater_request_updater_reboot();
        break;

    default:
        return BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED);
    }

    if (rc != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }
    return (ssize_t)len;
}

/* ============================================================================
 * GATT service definition
 * Attribute index map:
 *   0 — Primary service declaration
 *   1 — Status characteristic declaration
 *   2 — Status characteristic value         ← used in bt_gatt_notify()
 *   3 — Status CCC descriptor
 *   4 — Data characteristic declaration
 *   5 — Data characteristic value
 *   6 — Control characteristic declaration
 *   7 — Control characteristic value
 * ============================================================================ */
BT_GATT_SERVICE_DEFINE(mcuboot_updater_svc,
    BT_GATT_PRIMARY_SERVICE(&kServiceUuid),

    BT_GATT_CHARACTERISTIC(&kStatusUuid.uuid,
        BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
        BT_GATT_PERM_READ_ENCRYPT,
        status_read_cb, NULL, NULL),
    BT_GATT_CCC(ccc_cfg_changed, BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),

    BT_GATT_CHARACTERISTIC(&kDataUuid.uuid,
        BT_GATT_CHRC_WRITE_WITHOUT_RESP,
        BT_GATT_PERM_WRITE_ENCRYPT,
        NULL, data_write_cb, NULL),

    BT_GATT_CHARACTERISTIC(&kControlUuid.uuid,
        BT_GATT_CHRC_WRITE,
        BT_GATT_PERM_WRITE_ENCRYPT,
        NULL, control_write_cb, NULL),
);

/* ============================================================================
 * Status notification callback — called from the updater worker thread.
 * bt_gatt_notify() is thread-safe (posts to the BT host thread queue internally).
 * ============================================================================ */
static void status_notification_callback(struct McubootUpdaterStatus status)
{
    uint8_t packed[4] = {
        (uint8_t)status.state,
        status.progress,
        (uint8_t)status.error,
        status.flash_unlocked,
    };
    /* attrs[2] is the Status characteristic value — see index map above */
    int rc = bt_gatt_notify(NULL, &mcuboot_updater_svc.attrs[2], packed, sizeof(packed));
    if (rc && rc != -ENOTCONN) {
        LOG_WRN("Status notify failed: %d (state=%d)", rc, status.state);
    }
}

/* ============================================================================
 * SYS_INIT: wire up the notification callback with the updater core module.
 * ============================================================================ */
static int mcuboot_updater_service_init(void)
{
    mcuboot_updater_init(status_notification_callback);
    return 0;
}
SYS_INIT(mcuboot_updater_service_init, APPLICATION, CONFIG_APP_MCUBOOT_UPDATER_INIT_PRIORITY);
static_assert(CONFIG_APP_MCUBOOT_UPDATER_INIT_PRIORITY > CONFIG_MCUBOOT_INFO_INIT_PRIORITY,
              "MCUboot updater must init after MCUboot info service");
