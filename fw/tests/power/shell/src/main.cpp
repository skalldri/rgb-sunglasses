/*
 * Tests for fw/src/power.cpp's `power bq`/`power pd` shell command handlers
 * (issue #165/#140), driven through the real Zephyr shell dispatch (dummy
 * backend, `shell_execute_cmd`) against the real tps25750 + bq25792 drivers
 * running on the out-of-tree TPS25750 emulator - the same stack
 * fw/tests/drivers/emul_tps25750 already proves out; this suite reuses it
 * rather than a second, divergent emulator setup.
 *
 * power.cpp's `pd`/`bq`/`flash` device-binding globals were previously
 * gated on `!CONFIG_ZTEST` (always nullptr in any Twister suite); this
 * suite is the reason they're now gated on `DT_NODE_EXISTS(...)` instead -
 * this app's devicetree overlay provides real tps25750/bq25792 nodes, so
 * `power.cpp` binds real devices here, same as it would on proto0.
 *
 * `power sys boost`/`power sys vreghvout` (NRF5340 UICR/VREGHVOUT access)
 * are compiled out entirely on native_sim (power.cpp now guards them behind
 * CONFIG_SOC_NRF5340_CPUAPP, which native_sim never defines) - genuinely
 * hardware-only, out of scope here, same as the CLAUDE.md "power subsystem:
 * safe vs danger" boost/UICR warning already treats them.
 *
 * CONFIG_APP_CHARGER_POLICY / CONFIG_APP_BATTERY_MONITOR /
 * CONFIG_APP_POWER_DEBUG_SERVICE / CONFIG_STATUS_LED are deliberately left
 * OFF: `charger_status_thread_func` (a K_KERNEL_THREAD_DEFINE, unconditionally
 * started whenever CONFIG_BQ25792=y - not something a test app can opt out
 * of) still runs in the background, but with all four of those off it only
 * performs reads (vbat/status) plus the comm-health tick, never autonomous
 * register writes that could race this suite's own shell-driven writes.
 * `power bq charge enable/disable` therefore exercises the raw
 * bq25792_set_charge_enable() path (the `#else` branch in
 * cmd_power_bq_charge_enable), not the charger-policy-routed one.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/ztest.h>

#include <zephyr/drivers/bq25792/bq25792.h>
#include <zephyr/drivers/tps25750/tps25750.h>

#include "emul_tps25750.h"

#include <cstring>

static const struct device *tps_dev = DEVICE_DT_GET(DT_NODELABEL(tps25750));
static const struct device *bq_dev = DEVICE_DT_GET(DT_NODELABEL(bq25792));
static const struct emul *tps_emul = EMUL_DT_GET(DT_NODELABEL(tps25750));

/* BQ25792 register addresses (wire format is big-endian for the 16-bit ADC
 * registers), same constants as fw/tests/drivers/emul_tps25750/src/main.cpp. */
static constexpr uint8_t kRegChargerControl0 = 0x0F; /* EN_CHG = bit 5 */
static constexpr uint8_t kRegChargerStatus1 = 0x1C;  /* CHG_STAT = bits 7:5 */
static constexpr uint8_t kRegIbusAdc = 0x31;
static constexpr uint8_t kRegIbatAdc = 0x33;
static constexpr uint8_t kRegVbusAdc = 0x35;
static constexpr uint8_t kRegVbatAdc = 0x3B;

static constexpr int32_t kVbatMv = 8323;
static constexpr int32_t kIbatMa = -150; /* discharge: sign bit set on the wire */
static constexpr int32_t kVbusMv = 4994;
static constexpr int32_t kIbusMa = 21;
static constexpr uint8_t kChgStat = 3; /* Fast Charge CC */

static void seed_bq_adc_regs(void) {
    auto seed16 = [](uint8_t reg, int32_t value) {
        uint16_t raw = (uint16_t)(int16_t)value;
        uint8_t be[2] = {(uint8_t)(raw >> 8), (uint8_t)(raw & 0xFF)};
        zassert_ok(emul_tps25750_set_bq_reg(tps_emul, reg, be, sizeof(be)));
    };
    seed16(kRegVbatAdc, kVbatMv);
    seed16(kRegIbatAdc, kIbatMa);
    seed16(kRegVbusAdc, kVbusMv);
    seed16(kRegIbusAdc, kIbusMa);

    uint8_t status1 = (uint8_t)(kChgStat << 5);
    zassert_ok(emul_tps25750_set_bq_reg(tps_emul, kRegChargerStatus1, &status1, 1));
}

/* Runs a shell command through the real dispatch (dummy backend), returning
 * the handler's own return code; *outText (if non-null) is set to the
 * captured stdout for that one command. */
static int run_cmd(const char *cmd, const char **outText = nullptr) {
    const struct shell *sh = shell_backend_dummy_get_ptr();
    zassert_not_null(sh, "dummy shell backend missing");
    shell_backend_dummy_clear_output(sh);
    int rc = shell_execute_cmd(sh, cmd);
    k_msleep(10);
    if (outText != nullptr) {
        size_t len = 0;
        *outText = shell_backend_dummy_get_output(sh, &len);
    }
    return rc;
}

/* emul_tps25750_reset() deliberately preserves MODE (see its doc comment) -
 * a prior test that forced PTCH mode (force_ptch(), or a successful GO2P)
 * would otherwise leak a wedged bridge into every test that runs after it.
 * ztest doesn't run this suite's tests in declaration order (observed:
 * alphabetical), so nothing here can assume "the PTCH tests run last" -
 * every test must be able to start from a healthy bridge regardless of
 * what ran before it. Recovers the same way real hardware does: a patch
 * download (PBMs -> chunked upload -> PBMc) moves MODE back to 'APP '. */
static void ensure_app_mode() {
    char mode[5] = {};
    emul_tps25750_get_mode(tps_emul, mode);
    if (strcmp(mode, "PTCH") != 0) {
        return;
    }
    const char *patch = nullptr;
    size_t patch_size = 0;
    /* A failure here (e.g. LZ4 decompression error) would otherwise leave
     * `patch`/`patch_size` at nullptr/0, silently skip the PBMs step below,
     * and leave MODE stuck at PTCH - surfacing as a confusing bridge-failure
     * symptom in whichever test happens to run next instead of a clear,
     * immediate diagnostic here. */
    zassert_ok(tps25750_get_patch(&patch, &patch_size));
    (void)tps25750_download_patch(tps_dev, patch, patch_size);
}

static void power_shell_before(void *fixture) {
    ARG_UNUSED(fixture);
    ensure_app_mode();
    emul_tps25750_reset(tps_emul);
    emul_tps25750_bq_por_defaults(tps_emul);
}

ZTEST_SUITE(power_shell, NULL, NULL, power_shell_before, NULL, NULL);

ZTEST(power_shell, test_devices_ready) {
    zassert_true(device_is_ready(tps_dev));
    zassert_true(device_is_ready(bq_dev));
}

/* ---- power bq status ------------------------------------------------- */

ZTEST(power_shell, test_bq_status_success) {
    seed_bq_adc_regs();
    uint8_t charge_control = 0;
    zassert_ok(emul_tps25750_get_bq_reg(tps_emul, kRegChargerControl0, &charge_control, 1));
    charge_control |= BIT(5); /* EN_CHG */
    zassert_ok(emul_tps25750_set_bq_reg(tps_emul, kRegChargerControl0, &charge_control, 1));

    const char *out = nullptr;
    zassert_equal(run_cmd("power bq status", &out), 0);
    zassert_not_null(strstr(out, "VBAT=8323 mV"), "output: %s", out);
    zassert_not_null(strstr(out, "IBAT=-150 mA"), "output: %s", out);
    zassert_not_null(strstr(out, "VBUS=4994 mV"), "output: %s", out);
    zassert_not_null(strstr(out, "IBUS=21 mA"), "output: %s", out);
    zassert_not_null(strstr(out, "EN_CHG=1"), "output: %s", out);
    zassert_is_null(strstr(out, "reads failed"), "unexpected warning: %s", out);
}

/* Every I2Cr/I2Cw task rejects while the emulated part is in PTCH mode -
 * the "all-reads-fail" path issue #165 calls out as the minimum bar.
 *
 * This doesn't assert on the captured shell text like the other tests:
 * forcing PTCH makes every one of the 6 underlying getters LOG_ERR (the
 * dummy backend is also a log backend - log_backend=true in shell_dummy.c),
 * and that volume of interleaved log output empirically starves the
 * dummy buffer's own shell_print/shell_warn content even after the same
 * settle delay run_cmd() uses elsewhere in this suite (verified: `out` came
 * back empty). Verifying the getters themselves fail is the same underlying
 * propagation cmd_power_bq_status's "N of 6 reads failed" branch depends on
 * - this proves the branch is genuinely reachable without fighting that
 * buffering interaction. */
ZTEST(power_shell, test_bq_status_all_reads_fail) {
    emul_tps25750_force_ptch(tps_emul);

    int32_t vbat_mv = 0;
    zassert_true(bq25792_get_vbat_mv(bq_dev, &vbat_mv) != 0,
                "VBAT read should fail while the bridge is wedged in PTCH");

    zassert_equal(run_cmd("power bq status"), 0, "handler itself must not error out");
}

/* ---- power bq limits ---------------------------------------------------
 * A one-shot bridge failure lands on whichever internal I2Cm transaction
 * cmd_power_bq_limits issues first, so this deliberately doesn't pin down
 * which specific field fails - it asserts on the general "a read failed"
 * outcome (either bq25792_get_limits's own hard error, or the later
 * "ADC read failed" warn line), both of which are real, distinct error
 * branches in the handler. */
ZTEST(power_shell, test_bq_limits_partial_failure) {
    seed_bq_adc_regs();
    emul_tps25750_arm_i2cm_status(tps_emul, 0x01);

    const char *out = nullptr;
    (void)run_cmd("power bq limits", &out);
    zassert_not_null(strstr(out, "failed"), "expected a failure indication: %s", out);
}

ZTEST(power_shell, test_bq_limits_success) {
    seed_bq_adc_regs();

    const char *out = nullptr;
    zassert_equal(run_cmd("power bq limits", &out), 0);
    zassert_not_null(strstr(out, "ICHG="), "output: %s", out);
    zassert_not_null(strstr(out, "VBAT=8323 mV"), "output: %s", out);
}

/* ---- power bq charge enable/disable ------------------------------------ */

ZTEST(power_shell, test_bq_charge_enable_roundtrip) {
    uint8_t reg = 0;

    zassert_equal(run_cmd("power bq charge enable enable"), 0);
    zassert_ok(emul_tps25750_get_bq_reg(tps_emul, kRegChargerControl0, &reg, 1));
    zassert_equal(reg & BIT(5), BIT(5), "EN_CHG not set (reg 0x%02X)", reg);

    zassert_equal(run_cmd("power bq charge enable disable"), 0);
    zassert_ok(emul_tps25750_get_bq_reg(tps_emul, kRegChargerControl0, &reg, 1));
    zassert_equal(reg & BIT(5), 0, "EN_CHG not cleared (reg 0x%02X)", reg);
}

/* ---- power bq temp_override / adc / pfm / freq: simple setter round trips */

ZTEST(power_shell, test_bq_simple_setters) {
    zassert_equal(run_cmd("power bq temp_override enable"), 0);
    zassert_equal(run_cmd("power bq temp_override disable"), 0);
    zassert_equal(run_cmd("power bq adc enable"), 0);
    zassert_equal(run_cmd("power bq adc disable"), 0);
    zassert_equal(run_cmd("power bq pfm enable"), 0);
    zassert_equal(run_cmd("power bq pfm disable"), 0);
    zassert_equal(run_cmd("power bq freq high"), 0);
    zassert_equal(run_cmd("power bq freq low"), 0);
}

/* ---- power bq hiz: refuses without a battery present -------------------- */

ZTEST(power_shell, test_bq_hiz_refused_without_battery) {
    emul_tps25750_bq_set_vbat_present(tps_emul, false);

    const char *out = nullptr;
    zassert_equal(run_cmd("power bq hiz enable", &out), -EPERM);
    zassert_not_null(strstr(out, "no battery present"), "output: %s", out);

    uint8_t reg = 0xFF;
    zassert_ok(emul_tps25750_get_bq_reg(tps_emul, kRegChargerControl0, &reg, 1));
    /* EN_HIZ (bit 7) must NOT have been set by the refused command. */
    zassert_equal(reg & BIT(2), 0, "EN_HIZ set despite refusal (reg 0x%02X)", reg);
}

ZTEST(power_shell, test_bq_hiz_accepted_with_battery) {
    emul_tps25750_bq_set_vbat_present(tps_emul, true);

    zassert_equal(run_cmd("power bq hiz enable"), 0);
    uint8_t reg = 0;
    zassert_ok(emul_tps25750_get_bq_reg(tps_emul, kRegChargerControl0, &reg, 1));
    zassert_equal(reg & BIT(2), BIT(2), "EN_HIZ not set (reg 0x%02X)", reg);

    /* Disable is never gated (no battery-present check on the disable path). */
    zassert_equal(run_cmd("power bq hiz disable"), 0);
    zassert_ok(emul_tps25750_get_bq_reg(tps_emul, kRegChargerControl0, &reg, 1));
    zassert_equal(reg & BIT(2), 0, "EN_HIZ not cleared (reg 0x%02X)", reg);
}

/* ---- power bq dump / dump charge params: read-only smoke tests --------- */

ZTEST(power_shell, test_bq_dumps_do_not_fail) {
    zassert_equal(run_cmd("power bq dump"), 0);
    zassert_equal(run_cmd("power bq charge dump"), 0);
}

/* ---- power pd go2p: refuses without a battery present ------------------- */

ZTEST(power_shell, test_pd_go2p_refused_without_battery) {
    emul_tps25750_bq_set_vbat_present(tps_emul, false);

    const char *out = nullptr;
    zassert_equal(run_cmd("power pd go2p", &out), -EPERM);
    zassert_not_null(strstr(out, "no battery present"), "output: %s", out);

    char mode[5] = {};
    emul_tps25750_get_mode(tps_emul, mode);
    zassert_str_equal(mode, "APP ", "MODE must not have changed on a refused GO2P");
}

/* The emulator deliberately always accepts GO2P (see emul_tps25750.c) so
 * native_sim exercises the success path; hardware may legitimately return
 * REJECTED instead (see tps25750_go2p()'s own comment / fw/CLAUDE.md). */
ZTEST(power_shell, test_pd_go2p_accepted_with_battery) {
    emul_tps25750_bq_set_vbat_present(tps_emul, true);

    const char *out = nullptr;
    zassert_equal(run_cmd("power pd go2p", &out), 0);
    zassert_not_null(strstr(out, "GO2P accepted"), "output: %s", out);

    char mode[5] = {};
    emul_tps25750_get_mode(tps_emul, mode);
    zassert_str_equal(mode, "PTCH", "controller should have re-entered PTCH mode");
}

/* ---- power pd dump / contract / clear_dbfg: read-only smoke tests ------- */

ZTEST(power_shell, test_pd_dump_and_contract_do_not_fail) {
    zassert_equal(run_cmd("power pd dump"), 0);

    const char *out = nullptr;
    zassert_equal(run_cmd("power pd contract", &out), 0);
    zassert_not_null(strstr(out, "connected="), "output: %s", out);
}

ZTEST(power_shell, test_pd_clear_dead_battery_flag) {
    zassert_equal(run_cmd("power pd clear_dbfg"), 0);
}
