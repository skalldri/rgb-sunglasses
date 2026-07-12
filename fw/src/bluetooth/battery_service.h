#pragma once

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief BT-free API for the battery monitoring GATT service (issue #97).
 *
 * Implemented in battery_service.cpp (compiled only when
 * CONFIG_APP_BATTERY_MONITOR=y). power.cpp's charger status thread calls
 * these so it never needs a BT header itself — same decoupling rule as the
 * animation/BT split.
 */

/**
 * @brief Returns the persisted "Charging Enabled" user intent.
 *
 * Restored from settings before BT came up (defaults to ON). The charger
 * status thread feeds this into charger_policy_boot_init(), which owns the
 * actual EN_CHG hardware write (gated on battery presence) — this service no
 * longer touches EN_CHG at boot itself. Valid once settings_load() has run
 * (bluetooth_init() at SYS_INIT(APPLICATION, 1), which always precedes
 * K_THREAD_DEFINE thread scheduling).
 */
bool battery_service_get_charge_enable(void);

/**
 * @brief Returns the persisted "Charge Current (mA)" target (defaults to
 * CONFIG_APP_CHARGE_CURRENT_MA). Same boot-ordering contract as
 * battery_service_get_charge_enable().
 */
uint32_t battery_service_get_charge_current_ma(void);

/**
 * @brief Publishes one battery telemetry sample to the BLE characteristics.
 *
 * Values are quantized (10 mV / 10 mA) before assignment so ADC jitter does
 * not notify subscribed clients on every 500 ms sample; the characteristics
 * themselves only notify on change.
 *
 * @param vbat_mv  Battery voltage in millivolts.
 * @param ibat_ma  Battery current in milliamps (positive = charging).
 * @param vbus_mv  VBUS (USB input) voltage in millivolts.
 * @param ibus_ma  VBUS input current in milliamps.
 * @param chg_stat Raw BQ25792 CHG_STAT field (0-7).
 */
void battery_service_update(int32_t vbat_mv, int32_t ibat_ma, int32_t vbus_mv, int32_t ibus_ma,
                            uint8_t chg_stat);
