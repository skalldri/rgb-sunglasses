#pragma once

#include <stdint.h>

/**
 * @brief BT-free API for the Power Debug GATT service (power plan PR E).
 *
 * Implemented in power_debug_service.cpp (compiled only when
 * CONFIG_APP_POWER_DEBUG_SERVICE=y). The charger status thread in power.cpp
 * gathers the inputs (BQ25792 limits/status + TPS25750 PD info + charger
 * policy snapshot) and calls power_debug_service_update() so power.cpp never
 * needs a BT header itself — same decoupling pattern as battery_service.h.
 *
 * All characteristics are read+notify debug telemetry; they only notify when
 * a value actually changes, so a steady-state update cadence is cheap on air.
 */

/* Bit assignments for the "Power Flags" characteristic (uint8 bitmask). The
 * companion app decodes these positionally — append-only, never renumber. */
#define POWER_DEBUG_FLAG_VBAT_PRESENT   (1U << 0) /* battery attached (VBAT_PRESENT_STAT) */
#define POWER_DEBUG_FLAG_VBUS_PRESENT   (1U << 1) /* USB input present */
#define POWER_DEBUG_FLAG_IINDPM_ACTIVE  (1U << 2) /* input CURRENT limit regulating */
#define POWER_DEBUG_FLAG_VINDPM_ACTIVE  (1U << 3) /* input VOLTAGE limit regulating (source sag) */
#define POWER_DEBUG_FLAG_VSYSMIN_REG    (1U << 4) /* VSYS held at VSYSMIN (VBAT below it) */
#define POWER_DEBUG_FLAG_WD_EXPIRED     (1U << 5) /* BQ I2C watchdog expired (config reverted) */
#define POWER_DEBUG_FLAG_CHARGE_GATED   (1U << 6) /* user wants charge ON but policy gated it */

/** One Power Debug telemetry sample (see power_debug_service_update()). */
struct power_debug_info {
    uint32_t input_limit_ma;  /**< IINDPM readback (bq25792_limits.iindpm_ma) */
    uint8_t power_flags;      /**< POWER_DEBUG_FLAG_* bitmask */
    uint8_t pd_source_type;   /**< enum tps25750_power_source (tps25750.h) */
    uint32_t pd_available_mv; /**< negotiated input budget voltage */
    uint32_t pd_available_ma; /**< negotiated input budget current */
    uint32_t ico_result_ma;   /**< ICO-discovered input limit (REG19 readback) */
};

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Publishes one Power Debug sample to the BLE characteristics.
 *
 * Called from the charger status thread at the reconcile cadence (every
 * ~4th 500 ms tick — gathering the inputs costs a bq25792_get_limits burst
 * plus a cheap TPS25750 host-register read, so it is deliberately not done
 * every tick). Characteristics only notify on change.
 *
 * @param info One complete telemetry sample; ignored if NULL.
 */
void power_debug_service_update(const struct power_debug_info *info);

#ifdef __cplusplus
};
#endif
