#include "power_debug_service.h"

#include <bluetooth/bt_service_cpp.h>

/* Service id 6 — ids 1-5 are taken by CoreConfig, AudioConfig, mcuboot_info,
 * mcuboot_updater and the battery service. Characteristic UUIDs are
 * auto-assigned in declaration order (suffixes ...0000 through ...0005); the
 * companion app's UUID_POWER_DEBUG_SERVICE constant in
 * app/constants/bluetooth.ts must match. APPEND-ONLY once shipped. */
constexpr bt_uuid_128 kPowerDebugServiceUuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 6, 0x56789abc0000));

BtGattPrimaryService<kPowerDebugServiceUuid> powerDebugPrimaryService;
/* All non-notifiable (they used to notify): this is a debug service rendered
 * only on the app's battery detail page, which re-reads on focus — and Android
 * caps notification registrations at ~15 per app (BTA_GATTC_NOTIF_REG_MAX), so
 * six always-on debug notifies were a luxury the SMP/DFU characteristic paid
 * for. power_debug_service_update() still stores fresh values every sample. */
/* Position 0: effective input current limit (IINDPM readback) — the register
 * the TPS bundle / BC1.2 / charger policy all fight over; the first stop for
 * "why is charging slow" (see `power bq limits`). */
BtGattAutoReadOnlyCharacteristic<"Input Limit (mA)", uint32_t, 0> inputLimitMa;
/* Position 1: POWER_DEBUG_FLAG_* bitmask (power_debug_service.h). */
BtGattAutoReadOnlyCharacteristic<"Power Flags", uint8_t, 0> powerFlags;
/* Position 2: enum tps25750_power_source — how the input budget was
 * established (none / Type-C tier / explicit PD contract). */
BtGattAutoReadOnlyCharacteristic<"PD Source Type", uint8_t, 0> pdSourceType;
/* Positions 3-4: the negotiated input budget. */
BtGattAutoReadOnlyCharacteristic<"PD Available (mV)", uint32_t, 0> pdAvailableMv;
BtGattAutoReadOnlyCharacteristic<"PD Available (mA)", uint32_t, 0> pdAvailableMa;
/* Position 5: ICO-discovered input limit (BQ25792 REG19; only meaningful when
 * EN_ICO is on — plumbed but default-off, see CONFIG_APP_CHARGER_USE_ICO
 * discussion in docs/plans/power-management-overhaul.md). */
BtGattAutoReadOnlyCharacteristic<"ICO Result (mA)", uint32_t, 0> icoResultMa;

BtGattServer powerDebugServer(powerDebugPrimaryService, inputLimitMa, powerFlags, pdSourceType,
                              pdAvailableMv, pdAvailableMa, icoResultMa);
BT_GATT_SERVER_REGISTER(powerDebugServerStatic, powerDebugServer);

void power_debug_service_update(const struct power_debug_info *info) {
    if (info == nullptr) {
        return;
    }

    /* Plain stores now that nothing here notifies; the inputs are register
     * readbacks / decoded contract data refreshed every charger sample, so a
     * read at any time sees current values. */
    inputLimitMa  = info->input_limit_ma;
    powerFlags    = info->power_flags;
    pdSourceType  = info->pd_source_type;
    pdAvailableMv = info->pd_available_mv;
    pdAvailableMa = info->pd_available_ma;
    icoResultMa   = info->ico_result_ma;
}
