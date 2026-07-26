#include <bluetooth/bt_service_cpp.h>
#include <bluetooth/persistent_characteristic.h>
#include <core_config.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(core_config, LOG_LEVEL_INF);

constexpr bt_uuid_128 kCoreConfigServiceUuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, CoreConfig::kServiceIdNum, 0x56789abc0000));

BtGattPrimaryService<kCoreConfigServiceUuid> coreConfigPrimaryService;
BtGattPersistentCharacteristic<"core/brightness", "Brightness (0-1000)", true, uint32_t, 20>
    coreBrightness;
BtGattPersistentCharacteristic<"core/display_thread_rate_ms", "Display Thread Rate * 1000 (ms)",
                                true, uint32_t, 33300>
    coreDisplayThreadRateMs;
BtGattPersistentCharacteristic<"core/render_thread_rate_ms", "Render Thread Rate * 1000 (ms)",
                                true, uint32_t, 11100>
    coreRenderThreadRateMs;
BtGattPersistentCharacteristic<"core/status_led_brightness", "Status LED Brightness (0-1000)",
                                true, uint32_t, 20>
    coreStatusLedBrightness;
// Shuffle mode (issue #121). Appended AFTER every existing characteristic and always
// compiled in, even when CONFIG_APP_SHUFFLE=n (only the behavior is gated): the
// BtGattServer pack below assigns characteristic UUIDs positionally, so conditional
// membership would shift the UUIDs of anything declared later between builds.
// min > max is a tolerated state (swapped at pick time by ShuffleController) — the two
// durations are written one at a time over BLE, so no write is ever rejected here.
BtGattPersistentCharacteristic<"core/shuffle_enabled", "Shuffle Enabled", true, bool, false>
    coreShuffleEnabled;
BtGattPersistentCharacteristic<"core/shuffle_min_s", "Shuffle Min Duration (s)", true, uint32_t,
                                30>
    coreShuffleMinS;
BtGattPersistentCharacteristic<"core/shuffle_max_s", "Shuffle Max Duration (s)", true, uint32_t,
                                120>
    coreShuffleMaxS;

BtGattServer coreConfigServer(coreConfigPrimaryService, coreBrightness, coreDisplayThreadRateMs,
                              coreRenderThreadRateMs, coreStatusLedBrightness,
                              coreShuffleEnabled, coreShuffleMinS, coreShuffleMaxS);
BT_GATT_SERVER_REGISTER(coreConfigServerStatic, coreConfigServer);

float CoreConfig::getBrightnessFactor() {
    uint32_t brightnessUint = coreBrightness;

    // Clamp to sane values
    if (brightnessUint > 1000) {
        brightnessUint = 1000;
        coreBrightness = 1000;  // Clamp the BT variable as well
    }

    return ((float)brightnessUint) / 1000.0f;
}

float CoreConfig::getDisplayRateMs() {
    uint32_t displayRateUint = coreDisplayThreadRateMs;
    return ((float)displayRateUint) / 1000.0f;
}

float CoreConfig::getRenderRateMs() {
    uint32_t renderRateUint = coreRenderThreadRateMs;
    return ((float)renderRateUint) / 1000.0f;
}

float CoreConfig::getStatusLedBrightnessFactor() {
    uint32_t brightnessUint = coreStatusLedBrightness;

    // Clamp to sane values
    if (brightnessUint > 1000) {
        brightnessUint = 1000;
        coreStatusLedBrightness = 1000;
    }

    return ((float)brightnessUint) / 1000.0f;
}

bool CoreConfig::getShuffleEnabled() {
    bool enabled = coreShuffleEnabled;
    return enabled;
}

// No validation on the duration pair: min > max is a legitimate transient/persistent
// state (see the characteristic declarations above); ShuffleController swaps at pick time.
uint32_t CoreConfig::getShuffleMinDurationS() {
    uint32_t minS = coreShuffleMinS;
    return minS;
}

uint32_t CoreConfig::getShuffleMaxDurationS() {
    uint32_t maxS = coreShuffleMaxS;
    return maxS;
}

void CoreConfig::setShuffleEnabled(bool enabled) {
    // operator= notifies subscribers but bypasses onWrite (that hook is remote-write
    // only), so persist explicitly — mark_dirty() exists for exactly this shell path.
    coreShuffleEnabled = enabled;
    coreShuffleEnabled.mark_dirty();
    if (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
        persistent_value_store::request_save();
    }
}
