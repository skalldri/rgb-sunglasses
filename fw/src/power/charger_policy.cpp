#include "charger_policy.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/tps25750/tps25750.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(charger_policy, LOG_LEVEL_INF);

static const struct device *bq = DEVICE_DT_GET(DT_NODELABEL(bq25792));
static const struct device *pd = DEVICE_DT_GET(DT_NODELABEL(tps25750));

/* All state below is guarded by s_lock: setters arrive on the BT RX thread
 * (BLE writes) and the shell thread, concurrent with the charger status
 * thread's tick (fw/CLAUDE.md multi-step transaction rule). */
static struct k_mutex s_lock;
static bool s_initialized;
static bool s_user_charge_enable;
static bool s_effective_charge_enable;
static bool s_vbat_present;
static bool s_vbus_present;
static uint32_t s_charge_current_ma; /* 0 = unmanaged (until PR D) */
static uint32_t s_vindpm_mv = CONFIG_APP_VINDPM_MV;
static uint32_t s_iindpm_ma; /* 0 only before the first derive (then always
                              * managed; 500mA floor for legacy/none sources
                              * since AUTO_INDET is bypassed) */
static struct tps25750_pd_power_info s_pd_info;
static uint32_t s_wd_redisable_count;
static uint32_t s_tick_count;
static bool s_wd_expired_prev;
/* Bumped on every target change; the unlocked reconcile pass re-checks it
 * before writing so it never applies stale targets over a concurrent setter. */
static uint32_t s_config_gen;

/* Quantize a target to what the hardware can actually hold, so reconcile
 * compares like-for-like and can never diverge forever on a target the part
 * silently truncated (10mA LSB for ICHG, 100mV for VINDPM — SLUSDG1C Tables
 * 9-16/9-17). 0 stays 0 (= unmanaged). */
static uint32_t quantize_ichg_ma(uint32_t ma) {
    return ma == 0 ? 0 : (CLAMP(ma, 50, 5000) / 10) * 10;
}
static uint32_t quantize_vindpm_mv(uint32_t mv) { return (CLAMP(mv, 3600, 22000) / 100) * 100; }

/* Reconcile readbacks run every Nth tick: readbacks are bridged I2Cm
 * transactions, and divergence (a TPS-bundle event write, a watchdog re-arm)
 * is rare — a 2s reaction window is fine and keeps steady-state bridge
 * traffic low. */
static constexpr uint32_t kReconcileTickInterval = 4;

/* Apply EN_CHG = (user intent && battery present). Caller holds s_lock. */
static int apply_effective_charge_enable_locked(void) {
    bool target = s_user_charge_enable && s_vbat_present;

    int ret = bq25792_set_charge_enable(bq, target);
    if (ret != 0) {
        LOG_ERR("EN_CHG=%u apply failed: %d", target ? 1 : 0, ret);
        return ret;
    }
    s_effective_charge_enable = target;

    if (s_user_charge_enable && !s_vbat_present) {
        /* Event-driven (presence edge / user write), not per-tick. */
        LOG_WRN("charge enable gated: no battery present (SLUSDG1C 9.3.6)");
    }
    return 0;
}

/* Derive IINDPM/VINDPM targets from the negotiated input budget. Caller holds
 * s_lock. Returns true when a target changed (caller should re-apply).
 *
 * - Explicit PD contract / Type-C 1.5A/3A advertisement: the budget is known
 *   digitally — IINDPM = min(contract mA, CONFIG_APP_INPUT_CURRENT_LIMIT_MAX_MA),
 *   VINDPM = 90% of the contract voltage (rounded down to the 100mV LSB,
 *   floored at the Kconfig fallback for 5V-class contracts) so a sagging
 *   source folds back before collapsing.
 * - Type-C default / unknown / disconnected: manage IINDPM at the 500mA USB
 *   default floor. The BQ's BC1.2 detection used to be the lawful budget for
 *   legacy sources, but it is now bypassed (AUTO_INDET_EN=0 in
 *   apply_config_locked — D+/D- are not connected on this board), so nothing
 *   else would ever program a budget for them; 500mA is the lawful floor for
 *   any USB source (deliberately probing past it belongs to ICO, kept off
 *   per the plan). This also covers the brief window after a fresh attach
 *   before the TPS reports the negotiated budget.
 */
static bool derive_input_targets_locked(void) {
    uint32_t iindpm_target = 500;
    uint32_t vindpm_target = CONFIG_APP_VINDPM_MV;

    switch (s_pd_info.source) {
        case TPS25750_PWR_PD_CONTRACT:
        case TPS25750_PWR_TYPEC_1A5:
        case TPS25750_PWR_TYPEC_3A0:
            iindpm_target =
                (CLAMP(MIN(s_pd_info.available_ma, CONFIG_APP_INPUT_CURRENT_LIMIT_MAX_MA), 100,
                       3300) /
                 10) *
                10; /* IINDPM LSB/range: SLUSDG1C Table 9-18 */
            vindpm_target =
                MAX((uint32_t)CONFIG_APP_VINDPM_MV,
                    ((s_pd_info.available_mv * 9 / 10) / 100) * 100); /* 90%, 100mV LSB */
            break;
        default:
            break;
    }

    bool changed = (iindpm_target != s_iindpm_ma) || (vindpm_target != s_vindpm_mv);
    s_iindpm_ma = iindpm_target;
    s_vindpm_mv = vindpm_target;
    if (changed) {
        LOG_INF("input budget: source=%d -> IINDPM=%umA VINDPM=%umV", (int)s_pd_info.source,
                s_iindpm_ma, s_vindpm_mv);
    }
    return changed;
}

/* Write the managed configuration to the part. Caller holds s_lock. */
static void apply_config_locked(void) {
    /* Watchdog first: on expiry it reverts EN_CHG/ICHG to POR defaults
     * (charge-enabled @ 2A), which reintroduces the no-battery reboot loop
     * this module exists to prevent. */
    if (bq25792_watchdog_disable(bq) != 0) {
        LOG_ERR("watchdog disable failed");
    }

    /* Bypass D+/D- input type detection: the BQ's D+/D- pins are not
     * connected on this design, so BC1.2 probes floating pins and can latch
     * VBUS_STAT "Not qualified adaptor" — which blocks the converter and
     * thus ALL charging until a detection pass happens to succeed
     * (hardware-diagnosed 2026-07-17: board stopped charging on every
     * source, survived replug/HIZ-toggle/full POR). With detection bypassed
     * the converter starts right after poor-source qualification and this
     * module owns IINDPM for every source type (see
     * derive_input_targets_locked). AUTO_INDET_EN is watchdog/REG_RST-reset
     * back to enabled (SLUSDG1C Table 9-27), so it lives in this reapply
     * path, not just boot. */
    if (bq25792_auto_indet_enable(bq, false) != 0) {
        LOG_ERR("AUTO_INDET_EN disable failed");
    }

    /* Deterministic input-OVP headroom for any contract voltage we would
     * accept (0h = 26V; the datasheet's REG10 reset annotations are
     * self-inconsistent, and hardware read back 26V — make it explicit). */
    if (bq25792_set_vac_ovp(bq, 0x0) != 0) {
        LOG_ERR("VAC_OVP apply failed");
    }

    if (bq25792_set_input_voltage_limit_mv(bq, s_vindpm_mv) != 0) {
        LOG_ERR("VINDPM=%umV apply failed", s_vindpm_mv);
    }

    /* INVARIANT: every caller must run derive_input_targets_locked() before
     * this function (boot_init does; the WD-reapply path inherits a prior
     * tick's derive), so s_iindpm_ma is 0 only if that ordering is ever
     * broken. With AUTO_INDET bypassed, skipping the IINDPM write here would
     * silently leave the input budget unprogrammed — the exact no-budget bug
     * the 500mA floor exists to prevent — so a 0 here is loud, not skipped. */
    if (s_iindpm_ma != 0) {
        if (bq25792_set_input_current_limit_ma(bq, s_iindpm_ma) != 0) {
            LOG_ERR("IINDPM=%umA apply failed", s_iindpm_ma);
        }
    } else {
        LOG_ERR("apply_config called before input targets were derived");
    }

    if (s_charge_current_ma != 0) {
        if (bq25792_set_charge_current_ma(bq, s_charge_current_ma) != 0) {
            LOG_ERR("ICHG=%umA apply failed", s_charge_current_ma);
        }
    }
}

/* The mutex must exist before ANY caller — a BLE write on the BT RX thread
 * can race the charger thread's boot_init. Priority 0 runs before
 * bluetooth_init/button_init (both priority 1) and before K_THREAD_DEFINE
 * threads are scheduled. */
static int charger_policy_sysinit(void) {
    k_mutex_init(&s_lock);
    return 0;
}
SYS_INIT(charger_policy_sysinit, APPLICATION, 0);

void charger_policy_boot_init(bool user_charge_enable, uint32_t charge_current_ma) {
    k_mutex_lock(&s_lock, K_FOREVER);

    s_user_charge_enable = user_charge_enable;
    /* Same ceiling as the runtime setter: a persisted value written under an
     * older build's higher CONFIG_APP_CHARGE_CURRENT_MAX_MA must not bypass
     * this build's pack/wiring limit at boot. */
    if (charge_current_ma > CONFIG_APP_CHARGE_CURRENT_MAX_MA) {
        LOG_WRN("persisted ICHG %umA exceeds max %umA — clamping", charge_current_ma,
                CONFIG_APP_CHARGE_CURRENT_MAX_MA);
        charge_current_ma = CONFIG_APP_CHARGE_CURRENT_MAX_MA;
    }
    s_charge_current_ma = quantize_ichg_ma(charge_current_ma);
    s_vindpm_mv = quantize_vindpm_mv(s_vindpm_mv);

    /* Input budget first, so the initial apply already reflects any contract
     * the TPS25750 landed before we booted (cheap host-register read). */
    if (tps25750_get_pd_power_info(pd, &s_pd_info) != 0) {
        LOG_WRN("PD power info unavailable at boot; using fallback input targets");
        s_pd_info = {};
    }
    (void)derive_input_targets_locked();

    apply_config_locked();

    struct bq25792_status st;
    if (bq25792_get_status(bq, &st) == 0) {
        s_vbat_present = st.vbat_present;
        s_vbus_present = st.vbus_present;
    } else {
        /* Fail safe: treat an unreadable part as battery-absent so we never
         * enable charging into an unknown state. The first successful tick
         * corrects this. */
        LOG_ERR("boot status read failed; gating charge until status reads");
        s_vbat_present = false;
    }

    (void)apply_effective_charge_enable_locked();

    if (bq25792_ibat_sense_enable(bq, true) != 0) {
        LOG_ERR("IBAT sense enable failed");
    }

    LOG_INF("boot: user=%u vbat=%u effective=%u ichg=%umA vindpm=%umV",
            s_user_charge_enable ? 1 : 0, s_vbat_present ? 1 : 0,
            s_effective_charge_enable ? 1 : 0, s_charge_current_ma, s_vindpm_mv);

    s_initialized = true;
    k_mutex_unlock(&s_lock);
}

int charger_policy_set_user_charge_enable(bool enabled) {
    k_mutex_lock(&s_lock, K_FOREVER);
    bool prev = s_user_charge_enable;
    s_user_charge_enable = enabled;
    s_config_gen++;

    int ret = 0;
    if (s_initialized) {
        ret = apply_effective_charge_enable_locked();
        /* Gated acceptance: with no battery the write is intent-only and
         * always succeeds (apply targets EN_CHG=0, likely already there). */
        if (ret != 0) {
            /* The BLE layer rejects the ATT write and rolls its storage back
             * on error — the policy must roll back too, or the rejected
             * intent lingers and gets silently applied on the next battery
             * presence / watchdog edge. */
            s_user_charge_enable = prev;
        }
    }
    k_mutex_unlock(&s_lock);
    return ret;
}

int charger_policy_set_charge_current_ma(uint32_t ma) {
    /* Backstop clamp to the build's pack/wiring ceiling — the BLE layer
     * rejects out-of-range writes before calling here, but the shell path
     * (and any future caller) goes through this too. */
    if (ma > CONFIG_APP_CHARGE_CURRENT_MAX_MA) {
        LOG_WRN("ICHG %umA clamped to CONFIG_APP_CHARGE_CURRENT_MAX_MA (%umA)", ma,
                CONFIG_APP_CHARGE_CURRENT_MAX_MA);
        ma = CONFIG_APP_CHARGE_CURRENT_MAX_MA;
    }
    ma = quantize_ichg_ma(ma);

    k_mutex_lock(&s_lock, K_FOREVER);
    uint32_t prev = s_charge_current_ma;
    s_charge_current_ma = ma;
    s_config_gen++;

    int ret = 0;
    if (s_initialized && ma != 0) {
        ret = bq25792_set_charge_current_ma(bq, ma);
        if (ret != 0) {
            /* Same rollback contract as the enable path: a rejected write
             * must not linger as the reconcile target. */
            s_charge_current_ma = prev;
        }
    }
    k_mutex_unlock(&s_lock);
    return ret;
}

void charger_policy_tick(const struct bq25792_status *status) {
    if (status == NULL) {
        return;
    }

    k_mutex_lock(&s_lock, K_FOREVER);
    if (!s_initialized) {
        k_mutex_unlock(&s_lock);
        return;
    }

    s_vbus_present = status->vbus_present;

    /* Battery-presence edges. Removal while charging is the runtime half of
     * the no-battery reboot loop; insertion re-applies the user's intent. */
    if (status->vbat_present != s_vbat_present) {
        LOG_INF("battery %s", status->vbat_present ? "inserted" : "removed");
        s_vbat_present = status->vbat_present;
        (void)apply_effective_charge_enable_locked();
    }

    /* A watchdog expiry reverted our configuration — reapply everything now
     * rather than waiting for the readback rotation. Edge-triggered: WD_STAT
     * semantics on a latched flag must not cause a reapply every tick. */
    if (status->wd_expired && !s_wd_expired_prev) {
        LOG_WRN("BQ watchdog expired — reapplying configuration");
        s_wd_redisable_count++;
        apply_config_locked();
        (void)apply_effective_charge_enable_locked();
    }
    s_wd_expired_prev = status->wd_expired;

    /* Input-budget tracking: the PD info is a cheap (non-bridged) host read,
     * so poll it every tick; a contract change re-derives and re-applies the
     * input targets immediately (500ms worst-case reaction to a re-plug).
     * Decode into a LOCAL struct and commit only on success — a mid-decode
     * I2C failure must not leave s_pd_info a stale/fresh mix in snapshots. */
    struct tps25750_pd_power_info pd_info;
    if (tps25750_get_pd_power_info(pd, &pd_info) == 0) {
        s_pd_info = pd_info;
        if (derive_input_targets_locked()) {
            s_config_gen++;
            if (bq25792_set_input_voltage_limit_mv(bq, s_vindpm_mv) != 0) {
                LOG_ERR("VINDPM=%umV apply failed", s_vindpm_mv);
            }
            /* Every source type derives a managed budget now (500mA floor for
             * legacy/none — AUTO_INDET is bypassed, so no BC1.2 rewrite ever
             * programs one). This also covers the PD-Hard-Reset-without-VBUS-
             * drop case: losing the contract re-derives the 500mA floor here
             * within one tick. */
            if (bq25792_set_input_current_limit_ma(bq, s_iindpm_ma) != 0) {
                LOG_ERR("IINDPM=%umA apply failed", s_iindpm_ma);
            }
        }
    }

    bool do_reconcile = (++s_tick_count % kReconcileTickInterval) == 0;
    uint32_t gen = s_config_gen;
    uint32_t t_vindpm = s_vindpm_mv;
    uint32_t t_ichg = s_charge_current_ma;
    uint32_t t_iindpm = s_iindpm_ma;
    bool t_en = s_effective_charge_enable;

    k_mutex_unlock(&s_lock);

    if (!do_reconcile) {
        return;
    }

    /* Periodic reconcile: read the managed registers back and rewrite only
     * on divergence (e.g. the TPS25750 bundle wrote them on a PD event, or
     * BQ auto-INDET re-derived VINDPM on a re-plug).
     *
     * The readbacks (several sequential bridged I2Cm transactions, each with
     * its own bounded poll) run OUTSIDE s_lock: the BLE write path blocks on
     * s_lock from the BT RX thread, and holding it across a slow/wedged
     * bridge pass would stall Bluetooth RX long enough to drop the
     * connection. Divergence writes re-take the lock and are skipped if a
     * setter changed the targets mid-pass (generation check) — the next
     * reconcile pass converges on the new targets instead. */
    struct bq25792_limits limits;
    bool limits_ok = (bq25792_get_limits(bq, &limits) == 0);
    bool en_chg = t_en;
    bool en_ok = (bq25792_get_charge_enable(bq, &en_chg) == 0);

    k_mutex_lock(&s_lock, K_FOREVER);
    if (gen == s_config_gen) {
        if (limits_ok) {
            if (limits.watchdog != 0) {
                LOG_WRN("watchdog re-armed (setting %u) — disabling again", limits.watchdog);
                s_wd_redisable_count++;
                (void)bq25792_watchdog_disable(bq);
            }
            if (limits.vindpm_mv != t_vindpm) {
                LOG_INF("VINDPM diverged (%umV != %umV) — reconciling", limits.vindpm_mv,
                        t_vindpm);
                (void)bq25792_set_input_voltage_limit_mv(bq, t_vindpm);
            }
            if (t_iindpm != 0 && limits.iindpm_ma != t_iindpm) {
                LOG_INF("IINDPM diverged (%umA != %umA) — reconciling", limits.iindpm_ma,
                        t_iindpm);
                (void)bq25792_set_input_current_limit_ma(bq, t_iindpm);
            }
            if (t_ichg != 0 && limits.ichg_ma != t_ichg) {
                LOG_INF("ICHG diverged (%umA != %umA) — reconciling", limits.ichg_ma, t_ichg);
                (void)bq25792_set_charge_current_ma(bq, t_ichg);
            }
        }

        if (en_ok && en_chg != s_effective_charge_enable) {
            LOG_WRN("EN_CHG diverged (%u != %u) — reconciling", en_chg ? 1 : 0,
                    s_effective_charge_enable ? 1 : 0);
            (void)apply_effective_charge_enable_locked();
        }
    }

    k_mutex_unlock(&s_lock);
}

void charger_policy_get_snapshot(struct charger_policy_snapshot *out) {
    if (out == NULL) {
        return;
    }
    k_mutex_lock(&s_lock, K_FOREVER);
    out->user_charge_enable = s_user_charge_enable;
    out->effective_charge_enable = s_effective_charge_enable;
    out->vbat_present = s_vbat_present;
    out->vbus_present = s_vbus_present;
    out->charge_gated = s_user_charge_enable && !s_vbat_present;
    out->charge_current_ma = s_charge_current_ma;
    out->vindpm_mv = s_vindpm_mv;
    out->iindpm_ma = s_iindpm_ma;
    out->pd_source = (uint8_t)s_pd_info.source;
    out->pd_available_mv = s_pd_info.available_mv;
    out->pd_available_ma = s_pd_info.available_ma;
    out->wd_redisable_count = s_wd_redisable_count;
    k_mutex_unlock(&s_lock);
}
