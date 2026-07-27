#include "shuffle_service.h"

#include <bluetooth/bt_service_cpp.h>
#include <bluetooth/persistent_characteristic.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(shuffle_service, LOG_LEVEL_INF);

/* Service id 7 — ids 1-6 are taken by CoreConfig, AudioConfig, mcuboot_info,
 * mcuboot_updater, battery and power_debug. Characteristic UUIDs are auto-assigned in
 * declaration order (suffixes ...0000 through ...0002); the companion app's constants in
 * app/constants/bluetooth.ts must match that order. The app's Controls-page shuffle
 * button is hard-coded to the Enabled characteristic (...0000 — same literal as the
 * service UUID; expected, Battery precedent). */
constexpr bt_uuid_128 kShuffleServiceUuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 7, 0x56789abc0000));

BtGattPrimaryService<kShuffleServiceUuid> shufflePrimaryService;
// These three moved here from the Core Config service (issue #243, previously issue
// #121's "core/shuffle_*" keys — new keys, prior persisted shuffle state is deliberately
// abandoned; the orphaned NVS entries have no registry entry and are ignored).
// min > max is a tolerated state (swapped at pick time by ShuffleController) — the two
// durations are written one at a time over BLE, so no write is ever rejected here.
BtGattPersistentCharacteristic<"shuffle/enabled", "Shuffle Enabled", true, bool, false>
    shuffleEnabled;
BtGattPersistentCharacteristic<"shuffle/min_s", "Shuffle Min Duration (s)", true, uint32_t, 30>
    shuffleMinS;
BtGattPersistentCharacteristic<"shuffle/max_s", "Shuffle Max Duration (s)", true, uint32_t, 120>
    shuffleMaxS;

BtGattServer shuffleServer(shufflePrimaryService, shuffleEnabled, shuffleMinS, shuffleMaxS);
BT_GATT_SERVER_REGISTER(shuffleServerStatic, shuffleServer);

bool shuffle_service_get_enabled(void) {
    bool enabled = shuffleEnabled;
    return enabled;
}

// No validation on the duration pair: min > max is a legitimate transient/persistent
// state (see the characteristic declarations above); ShuffleController swaps at pick time.
uint32_t shuffle_service_get_min_duration_s(void) {
    uint32_t minS = shuffleMinS;
    return minS;
}

uint32_t shuffle_service_get_max_duration_s(void) {
    uint32_t maxS = shuffleMaxS;
    return maxS;
}

void shuffle_service_set_enabled(bool enabled) {
    // operator= notifies subscribers but bypasses onWrite (that hook is remote-write
    // only), so persist explicitly — mark_dirty() exists for exactly this shell path.
    shuffleEnabled = enabled;
    shuffleEnabled.mark_dirty();
    if (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
        persistent_value_store::request_save();
    }
}
