#pragma once

/**
 * Charger policy engine — the single owner of BQ25792 configuration writes
 * after boot (docs/plans/power-management-overhaul.md, PR B).
 *
 * BT-free and C-callable: gating must work on builds without BLE. Callers:
 *   - the charger status thread in power.cpp (boot init + per-tick reconcile)
 *   - the BLE "Charging Enabled" characteristic (battery_service.cpp)
 *   - the `power bq charge enable` shell command
 *
 * Core behaviors:
 *   - No-battery gating: charge enable is only applied to the part while a
 *     battery is present (VBAT_PRESENT_STAT). With charge enabled and no
 *     battery, the BQ25792 cycles BAT/SYS to BATOVP (datasheet SLUSDG1C
 *     §9.3.6) and brown-outs the system — the proto0 reboot loop. The user's
 *     persisted intent is preserved and re-applied when a battery appears.
 *   - Watchdog-disabled invariant: the BQ I2C watchdog reverts EN_CHG/ICHG to
 *     POR defaults on expiry (charge-enabled @ 2A — NOT a safe state here).
 *     Disabled at boot and re-disabled if anything re-arms it.
 *   - Reconcile-on-mismatch: configuration is periodically read back and
 *     rewritten only on divergence (the TPS25750 config bundle writes some of
 *     these registers on PD events over the same I2Cm port).
 */

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/drivers/bq25792/bq25792.h>

#ifdef __cplusplus
extern "C" {
#endif

struct charger_policy_snapshot {
    bool user_charge_enable;      /* persisted user intent */
    bool effective_charge_enable; /* what the policy last programmed */
    bool vbat_present;            /* latest battery-presence reading */
    bool vbus_present;            /* latest input-presence reading */
    bool charge_gated;            /* user wants charge ON but no battery */
    uint32_t charge_current_ma;   /* ICHG target; 0 = unmanaged */
    uint32_t vindpm_mv;           /* VINDPM target */
    uint32_t iindpm_ma;           /* IINDPM target; 0 = unmanaged (legacy src) */
    uint8_t pd_source;            /* enum tps25750_power_source */
    uint32_t pd_available_mv;     /* negotiated input budget */
    uint32_t pd_available_ma;
    uint32_t wd_redisable_count;  /* times the watchdog had to be re-disabled */
};

/**
 * @brief One-time boot configuration. Call from the charger status thread
 * before the first charger_policy_tick(), after settings_load() has run
 * (the persisted user intent comes from the caller).
 *
 * Order is load-bearing: watchdog first (so nothing later can be reverted),
 * then presence, VINDPM, ICHG, gated EN_CHG, IBAT sensing.
 *
 * @param user_charge_enable Persisted "Charging Enabled" intent.
 * @param charge_current_ma  ICHG target in mA; 0 leaves ICHG unmanaged
 *                           (the configurable charge current lands in PR D).
 */
void charger_policy_boot_init(bool user_charge_enable, uint32_t charge_current_ma);

/**
 * @brief Update the user's charge-enable intent (BLE write / shell).
 *
 * With a battery present this applies EN_CHG immediately; a hardware failure
 * is returned so the BLE layer can reject the ATT write. With NO battery
 * present the intent is accepted (returns 0) but not applied — gating is the
 * policy's job, and rejecting would make the app toggle unusable on the
 * bench. The snapshot's charge_gated flag exposes that state.
 */
int charger_policy_set_user_charge_enable(bool enabled);

/**
 * @brief Set the ICHG target in mA (clamped by the caller/Kconfig; 0 =
 * unmanaged). Applied immediately and enforced by reconcile. Wired to a BLE
 * characteristic + shell in PR D.
 */
int charger_policy_set_charge_current_ma(uint32_t ma);

/**
 * @brief Per-tick policy work, driven by the charger status thread (~500ms).
 *
 * @param status This tick's fresh bq25792_get_status() result (the thread
 *               already reads it for telemetry/LED).
 */
void charger_policy_tick(const struct bq25792_status *status);

/** @brief Copy out the current policy state (shell / debug surfaces). */
void charger_policy_get_snapshot(struct charger_policy_snapshot *out);

#ifdef __cplusplus
};
#endif
