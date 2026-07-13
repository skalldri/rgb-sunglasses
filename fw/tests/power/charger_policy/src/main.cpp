/*
 * Charger-policy tests against the real tps25750 + bq25792 drivers and the
 * TPS25750 emulator (same DT topology as proto0). Covers the PR B behaviors
 * of docs/plans/power-management-overhaul.md:
 *   - no-battery gating at boot and on runtime insertion/removal edges
 *   - the watchdog-disabled invariant (boot disable, re-arm reconcile,
 *     expiry-driven reapply) — the fix for the no-battery reboot loop
 *   - VINDPM programming + reconcile against external writers
 *   - gated acceptance of user charge-enable intent with no battery
 *
 * Register addresses/bits cited from BQ25792 datasheet SLUSDG1C:
 * REG0F[5]=EN_CHG (Table 9-25), REG10[2:0]=WATCHDOG (Table 9-26),
 * REG05=VINDPM 100mV/LSB (Table 9-17), REG1D[0]=VBAT_PRESENT (Table 9-38).
 */

#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <power/charger_policy.h>
#include <zephyr/drivers/bq25792/bq25792.h>

#include "emul_tps25750.h"

static const struct device *bq_dev = DEVICE_DT_GET(DT_NODELABEL(bq25792));
static const struct emul *tps_emul = EMUL_DT_GET(DT_NODELABEL(tps25750));

static uint8_t bq_reg(uint8_t reg) {
    uint8_t v = 0xFF;
    zassert_ok(emul_tps25750_get_bq_reg(tps_emul, reg, &v, 1));
    return v;
}

static bool en_chg(void) { return (bq_reg(0x0F) & BIT(5)) != 0; }

/* Run one policy tick fed from a real bridged status read, exactly like the
 * charger status thread does. */
static void tick(void) {
    struct bq25792_status st;
    zassert_ok(bq25792_get_status(bq_dev, &st));
    charger_policy_tick(&st);
}

/* A reconcile readback runs every 4th tick — 4 ticks always include one. */
static void tick_through_reconcile(void) {
    for (int i = 0; i < 4; i++) {
        tick();
    }
}

static void suite_before(void *fixture) {
    ARG_UNUSED(fixture);
    emul_tps25750_reset(tps_emul);
    emul_tps25750_bq_por_defaults(tps_emul);
}

ZTEST_SUITE(charger_policy, NULL, NULL, suite_before, NULL, NULL);

ZTEST(charger_policy, test_boot_no_battery_gates_charge) {
    /* POR defaults leave VBAT_PRESENT=0 and EN_CHG=1 — the exact power-up
     * state behind the reboot loop. */
    zassert_true(en_chg(), "POR default must be charge-enabled");

    charger_policy_boot_init(true /* user wants charging */, 0);

    zassert_false(en_chg(), "no battery -> EN_CHG must be gated off");
    zassert_equal(bq_reg(0x10) & 0x7, 0, "watchdog must be disabled at boot");
    zassert_equal(bq_reg(0x05), 4600 / 100, "VINDPM must be programmed");
    zassert_true((bq_reg(0x14) & BIT(5)) != 0, "IBAT sensing must be enabled");

    struct charger_policy_snapshot snap;
    charger_policy_get_snapshot(&snap);
    zassert_true(snap.user_charge_enable);
    zassert_false(snap.effective_charge_enable);
    zassert_true(snap.charge_gated);

    /* The disabled watchdog can no longer expire and revert EN_CHG. */
    zassert_false(emul_tps25750_bq_expire_watchdog(tps_emul),
                  "expiry must be a no-op once disabled");
    zassert_false(en_chg());
}

ZTEST(charger_policy, test_boot_with_battery_applies_user_intent) {
    emul_tps25750_bq_set_vbat_present(tps_emul, true);

    charger_policy_boot_init(true, 0);
    zassert_true(en_chg(), "battery present + user ON -> charging enabled");

    charger_policy_boot_init(false, 0);
    zassert_false(en_chg(), "user OFF -> charging disabled regardless of battery");
}

ZTEST(charger_policy, test_runtime_removal_and_reinsertion_edges) {
    emul_tps25750_bq_set_vbat_present(tps_emul, true);
    charger_policy_boot_init(true, 0);
    zassert_true(en_chg());

    /* Battery removed while charging: the very next tick must gate off. */
    emul_tps25750_bq_set_vbat_present(tps_emul, false);
    tick();
    zassert_false(en_chg(), "removal edge must clear EN_CHG within one tick");

    /* Re-inserted: user intent re-applies. */
    emul_tps25750_bq_set_vbat_present(tps_emul, true);
    tick();
    zassert_true(en_chg(), "insertion edge must restore the user's intent");
}

ZTEST(charger_policy, test_gated_user_write_accepted_without_hardware_touch) {
    charger_policy_boot_init(false, 0); /* no battery, user OFF */
    zassert_false(en_chg());

    /* Turning charging ON with no battery: accepted (BLE write must not be
     * rejected), persisted as intent, but the part stays off. */
    zassert_ok(charger_policy_set_user_charge_enable(true));
    zassert_false(en_chg(), "gated intent must not energize the charger");

    struct charger_policy_snapshot snap;
    charger_policy_get_snapshot(&snap);
    zassert_true(snap.charge_gated);

    /* Battery appears later: the stored intent applies. */
    emul_tps25750_bq_set_vbat_present(tps_emul, true);
    tick();
    zassert_true(en_chg());
}

ZTEST(charger_policy, test_watchdog_rearm_is_reconciled) {
    emul_tps25750_bq_set_vbat_present(tps_emul, true);
    charger_policy_boot_init(true, 0);

    struct charger_policy_snapshot before;
    charger_policy_get_snapshot(&before);

    /* An external writer (e.g. the TPS bundle after a PD event) re-arms the
     * watchdog at its 40s POR setting. */
    uint8_t rearmed = 0x05;
    zassert_ok(emul_tps25750_set_bq_reg(tps_emul, 0x10, &rearmed, 1));

    tick_through_reconcile();
    zassert_equal(bq_reg(0x10) & 0x7, 0, "reconcile must re-disable the watchdog");

    struct charger_policy_snapshot after;
    charger_policy_get_snapshot(&after);
    zassert_true(after.wd_redisable_count > before.wd_redisable_count);
}

ZTEST(charger_policy, test_watchdog_expiry_triggers_full_reapply) {
    emul_tps25750_bq_set_vbat_present(tps_emul, true);
    charger_policy_boot_init(false, 0); /* user does NOT want charging */
    zassert_false(en_chg());

    /* Re-arm + expire: the emulated expiry reverts EN_CHG to POR (enabled) —
     * the unsafe reversion the policy must undo. */
    uint8_t rearmed = 0x05;
    zassert_ok(emul_tps25750_set_bq_reg(tps_emul, 0x10, &rearmed, 1));
    zassert_true(emul_tps25750_bq_expire_watchdog(tps_emul));
    zassert_true(en_chg(), "expiry must have reverted EN_CHG to POR-enabled");

    /* The next tick sees WD_STAT and reapplies everything. */
    tick();
    zassert_false(en_chg(), "reapply must restore the user's OFF intent");
    zassert_equal(bq_reg(0x10) & 0x7, 0, "reapply must re-disable the watchdog");
}

ZTEST(charger_policy, test_vindpm_divergence_reconciled) {
    emul_tps25750_bq_set_vbat_present(tps_emul, true);
    charger_policy_boot_init(true, 0);
    zassert_equal(bq_reg(0x05), 4600 / 100);

    /* External writer drags VINDPM back to the 3600mV POR floor. */
    uint8_t por = 3600 / 100;
    zassert_ok(emul_tps25750_set_bq_reg(tps_emul, 0x05, &por, 1));

    tick_through_reconcile();
    zassert_equal(bq_reg(0x05), 4600 / 100, "reconcile must restore the VINDPM target");
}

ZTEST(charger_policy, test_ichg_managed_when_target_set) {
    emul_tps25750_bq_set_vbat_present(tps_emul, true);
    charger_policy_boot_init(true, 900);

    struct bq25792_limits limits;
    zassert_ok(bq25792_get_limits(bq_dev, &limits));
    zassert_equal(limits.ichg_ma, 900, "ICHG target must be programmed at boot");

    /* External writer (TPS bundle event) knocks it back to its 500mA value. */
    uint8_t bundle[2] = {0x00, 50}; /* 500mA, big-endian wire order */
    zassert_ok(emul_tps25750_set_bq_reg(tps_emul, 0x03, bundle, sizeof(bundle)));

    tick_through_reconcile();
    zassert_ok(bq25792_get_limits(bq_dev, &limits));
    zassert_equal(limits.ichg_ma, 900, "reconcile must restore the ICHG target");

    /* Target 0 = unmanaged: the policy must leave external values alone. */
    zassert_ok(charger_policy_set_charge_current_ma(0));
    zassert_ok(emul_tps25750_set_bq_reg(tps_emul, 0x03, bundle, sizeof(bundle)));
    tick_through_reconcile();
    zassert_ok(bq25792_get_limits(bq_dev, &limits));
    zassert_equal(limits.ichg_ma, 500, "unmanaged ICHG must not be reconciled");
}

/* ---- review-fix regressions: apply-then-commit + quantized targets ---- */

ZTEST(charger_policy, test_rejected_enable_write_does_not_linger) {
    emul_tps25750_bq_set_vbat_present(tps_emul, true);
    charger_policy_boot_init(false, 0);
    zassert_false(en_chg());

    /* The bridged EN_CHG write fails: the setter must report the error AND
     * roll the stored intent back — a later battery edge must not silently
     * apply the rejected value. */
    emul_tps25750_arm_cmd_wedge(tps_emul);
    zassert_not_equal(charger_policy_set_user_charge_enable(true), 0);
    emul_tps25750_reset(tps_emul);
    emul_tps25750_bq_por_defaults(tps_emul);
    uint8_t clear_en = 0x82; /* EN_CHG off, siblings intact */
    zassert_ok(emul_tps25750_set_bq_reg(tps_emul, 0x0F, &clear_en, 1));

    /* Battery removal + reinsertion edges re-apply the stored intent. */
    emul_tps25750_bq_set_vbat_present(tps_emul, false);
    tick();
    emul_tps25750_bq_set_vbat_present(tps_emul, true);
    tick();
    zassert_false(en_chg(), "rejected intent must not be applied on a later edge");

    struct charger_policy_snapshot snap;
    charger_policy_get_snapshot(&snap);
    zassert_false(snap.user_charge_enable);
}

ZTEST(charger_policy, test_rejected_charge_current_does_not_become_target) {
    emul_tps25750_bq_set_vbat_present(tps_emul, true);
    charger_policy_boot_init(true, 900);

    emul_tps25750_arm_cmd_wedge(tps_emul);
    zassert_not_equal(charger_policy_set_charge_current_ma(2000), 0);
    emul_tps25750_reset(tps_emul);
    emul_tps25750_bq_por_defaults(tps_emul);
    emul_tps25750_bq_set_vbat_present(tps_emul, true);

    /* Reconcile must converge on the previous 900, not the rejected 2000. */
    tick_through_reconcile();
    struct bq25792_limits limits;
    zassert_ok(bq25792_get_limits(bq_dev, &limits));
    zassert_equal(limits.ichg_ma, 900, "rejected value must not be reconciled in");
}

ZTEST(charger_policy, test_unaligned_target_quantized_no_divergence_loop) {
    emul_tps25750_bq_set_vbat_present(tps_emul, true);
    charger_policy_boot_init(true, 0);

    /* 55mA is not a 10mA multiple: the policy stores the quantized 50 so the
     * readback always matches and reconcile never rewrites forever. */
    zassert_ok(charger_policy_set_charge_current_ma(55));
    struct charger_policy_snapshot snap;
    charger_policy_get_snapshot(&snap);
    zassert_equal(snap.charge_current_ma, 50, "target must be stored quantized");

    tick_through_reconcile();
    struct bq25792_limits limits;
    zassert_ok(bq25792_get_limits(bq_dev, &limits));
    zassert_equal(limits.ichg_ma, 50);

    /* Second reconcile pass: register unchanged (no divergence rewrites).
     * Detect a rewrite via the emulator's last-4CC: seed a marker command
     * by reading (I2Cr) then reconcile — if reconcile wrote, last_4cc would
     * be I2Cw. */
    tick_through_reconcile();
    char four_cc[5];
    emul_tps25750_last_4cc(tps_emul, four_cc);
    zassert_mem_equal(four_cc, "I2Cr", 4, "steady-state reconcile must not rewrite (saw %s)",
                      four_cc);
}
