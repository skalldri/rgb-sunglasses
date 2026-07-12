/*
 * End-to-end tests for the TPS25750 driver against the out-of-tree emulator
 * (fw/drivers/emul_tps25750): the real bq25792 driver reads/writes through
 * the real tps25750 I2Cm bridge (CMD1/DATA1 4CC task protocol, serialized by
 * the driver's task_mutex) into the emulated chip.
 *
 * The concurrency test is the regression test for the task_mutex fix: two
 * threads hammer different BQ registers with distinct sentinel values. The
 * emulated CMD1 busy window (emul_tps25750_set_cmd_delay_ms) forces each
 * bridged transfer through tps25750_read_cmd_status()'s k_msleep(10) —
 * without the mutex the threads interleave there, one caller's DATA1 request
 * overwrites the other's in-flight transaction, and reads fail or return the
 * OTHER register's value (the exact VBUS-for-VBAT corruption observed on
 * hardware). With the mutex, every read must return its own sentinel.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>

#include <zephyr/drivers/bq25792/bq25792.h>
#include <zephyr/drivers/tps25750/tps25750.h>

#include "emul_tps25750.h"

static const struct device *tps_dev = DEVICE_DT_GET(DT_NODELABEL(tps25750));
static const struct device *bq_dev = DEVICE_DT_GET(DT_NODELABEL(bq25792));
static const struct emul *tps_emul = EMUL_DT_GET(DT_NODELABEL(tps25750));

/* BQ25792 register addresses (wire format is big-endian for the 16-bit ADC
 * registers; ADC LSB = 1mV / 1mA; currents are two's complement). Kept as
 * local constants — the driver's own definitions live in C++ template
 * machinery (bq25792_priv.h) not meant for inclusion here.
 */
static constexpr uint8_t kRegChargerControl0 = 0x0F; /* EN_CHG = bit 5 */
static constexpr uint8_t kRegChargerStatus1 = 0x1C;  /* CHG_STAT = bits 7:5 */
static constexpr uint8_t kRegIbusAdc = 0x31;
static constexpr uint8_t kRegIbatAdc = 0x33;
static constexpr uint8_t kRegVbusAdc = 0x35;
static constexpr uint8_t kRegVbatAdc = 0x3B;

/* Distinct, sign-exercising sentinels. */
static constexpr int32_t kVbatMv = 8323;
static constexpr int32_t kIbatMa = -150; /* discharge: sign bit set on the wire */
static constexpr int32_t kVbusMv = 4994;
static constexpr int32_t kIbusMa = 21;
static constexpr uint8_t kChgStat = 7; /* Charge Termination Done */

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

ZTEST(emul_tps25750, test_devices_ready) {
    zassert_true(device_is_ready(tps_dev));
    zassert_true(device_is_ready(bq_dev));
}

/* Every BQ getter routed through the real I2Cm bridge returns the seeded
 * value — including the negative (two's complement) battery current. */
ZTEST(emul_tps25750, test_bridge_read) {
    seed_bq_adc_regs();

    int32_t vbat = 0, ibat = 0, vbus = 0, ibus = 0;
    uint8_t chg_stat = 0xFF;

    zassert_ok(bq25792_get_vbat_mv(bq_dev, &vbat));
    zassert_ok(bq25792_get_ibat_ma(bq_dev, &ibat));
    zassert_ok(bq25792_get_vbus_mv(bq_dev, &vbus));
    zassert_ok(bq25792_get_ibus_ma(bq_dev, &ibus));
    zassert_ok(bq25792_get_charge_status(bq_dev, &chg_stat));

    zassert_equal(vbat, kVbatMv, "VBAT %d != %d", vbat, kVbatMv);
    zassert_equal(ibat, kIbatMa, "IBAT %d != %d", ibat, kIbatMa);
    zassert_equal(vbus, kVbusMv, "VBUS %d != %d", vbus, kVbusMv);
    zassert_equal(ibus, kIbusMa, "IBUS %d != %d", ibus, kIbusMa);
    zassert_equal(chg_stat, kChgStat);
}

/* Writes go through the bridge's I2Cw task and land in the emulated register
 * file; read-back round-trips. */
ZTEST(emul_tps25750, test_bridge_write) {
    uint8_t reg = 0;
    bool enabled = false;

    zassert_ok(bq25792_set_charge_enable(bq_dev, true));
    zassert_ok(emul_tps25750_get_bq_reg(tps_emul, kRegChargerControl0, &reg, 1));
    zassert_equal(reg & BIT(5), BIT(5), "EN_CHG not set (reg 0x%02X)", reg);
    zassert_ok(bq25792_get_charge_enable(bq_dev, &enabled));
    zassert_true(enabled);

    zassert_ok(bq25792_set_charge_enable(bq_dev, false));
    zassert_ok(emul_tps25750_get_bq_reg(tps_emul, kRegChargerControl0, &reg, 1));
    zassert_equal(reg & BIT(5), 0, "EN_CHG not cleared (reg 0x%02X)", reg);
    zassert_ok(bq25792_get_charge_enable(bq_dev, &enabled));
    zassert_false(enabled);
}

/* ---- config setters: 16-bit endianness + write/read-back-verify ---- */

/* Regression test for the InternalDeviceRegister::flush() endianness fix:
 * ICHG (REG03) is a 16-bit register, and the emulator stores the register
 * file in wire (big-endian) order — an unswapped flush() would land the
 * bytes reversed and both the raw check and the driver read-back fail. */
ZTEST(emul_tps25750, test_set_charge_current_16bit_round_trip) {
    emul_tps25750_bq_por_defaults(tps_emul);

    /* 1000mA -> ICHG field 100 (0x064): wire bytes must be {0x00, 0x64}. */
    zassert_ok(bq25792_set_charge_current_ma(bq_dev, 1000));

    uint8_t be[2] = {0xFF, 0xFF};
    zassert_ok(emul_tps25750_get_bq_reg(tps_emul, 0x03, be, sizeof(be)));
    zassert_equal(be[0], 0x00, "REG03 MSB 0x%02X (byte-swapped write?)", be[0]);
    zassert_equal(be[1], 0x64, "REG04 LSB 0x%02X (byte-swapped write?)", be[1]);

    struct bq25792_limits limits;
    zassert_ok(bq25792_get_limits(bq_dev, &limits));
    zassert_equal(limits.ichg_ma, 1000);
}

ZTEST(emul_tps25750, test_set_input_limits_and_readback) {
    emul_tps25750_bq_por_defaults(tps_emul);

    struct bq25792_limits limits;
    zassert_ok(bq25792_get_limits(bq_dev, &limits));
    /* POR defaults visible through the driver before any write. */
    zassert_equal(limits.ichg_ma, 2000);
    zassert_equal(limits.iindpm_ma, 3000);
    zassert_equal(limits.vindpm_mv, 3600);
    zassert_equal(limits.watchdog, 0x5); /* 40s */
    zassert_equal(limits.vac_ovp, 0x0);  /* 26V per field-table POR */

    zassert_ok(bq25792_set_input_current_limit_ma(bq_dev, 1500));
    zassert_ok(bq25792_set_input_voltage_limit_mv(bq_dev, 4600));
    zassert_ok(bq25792_get_limits(bq_dev, &limits));
    zassert_equal(limits.iindpm_ma, 1500);
    zassert_equal(limits.vindpm_mv, 4600);

    /* Out-of-range requests clamp to the datasheet range instead of failing
     * read-back verification. */
    zassert_ok(bq25792_set_charge_current_ma(bq_dev, 99999));
    zassert_ok(bq25792_get_limits(bq_dev, &limits));
    zassert_equal(limits.ichg_ma, 5000);

    zassert_ok(bq25792_set_input_voltage_limit_mv(bq_dev, 0));
    zassert_ok(bq25792_get_limits(bq_dev, &limits));
    zassert_equal(limits.vindpm_mv, 3600);
}

ZTEST(emul_tps25750, test_watchdog_disable_and_ico_enable) {
    emul_tps25750_bq_por_defaults(tps_emul);

    zassert_ok(bq25792_watchdog_disable(bq_dev));

    uint8_t reg10 = 0xFF;
    zassert_ok(emul_tps25750_get_bq_reg(tps_emul, 0x10, &reg10, 1));
    zassert_equal(reg10 & 0x7, 0, "WATCHDOG field not cleared (REG10 0x%02X)", reg10);
    /* VAC_OVP (bits 5:4) must be untouched by the field-scoped RMW. */
    zassert_equal((reg10 >> 4) & 0x3, 0x0);

    /* Feed is a plain RMW of WD_RST — just prove it reaches the register. */
    zassert_ok(bq25792_watchdog_feed(bq_dev));
    zassert_ok(emul_tps25750_get_bq_reg(tps_emul, 0x10, &reg10, 1));
    zassert_equal(reg10 & BIT(3), BIT(3), "WD_RST not set (REG10 0x%02X)", reg10);

    uint8_t reg0f = 0;
    zassert_ok(bq25792_ico_enable(bq_dev, true));
    zassert_ok(emul_tps25750_get_bq_reg(tps_emul, 0x0F, &reg0f, 1));
    zassert_equal(reg0f & BIT(4), BIT(4), "EN_ICO not set (REG0F 0x%02X)", reg0f);
    /* EN_CHG (bit 5, POR 1) must survive the RMW. */
    zassert_equal(reg0f & BIT(5), BIT(5), "EN_CHG clobbered (REG0F 0x%02X)", reg0f);

    zassert_ok(bq25792_ico_enable(bq_dev, false));
    zassert_ok(emul_tps25750_get_bq_reg(tps_emul, 0x0F, &reg0f, 1));
    zassert_equal(reg0f & BIT(4), 0);
}

/* A bridged-write failure surfaces from the setter instead of silently
 * leaving the register unchanged (read-back-verify contract). */
ZTEST(emul_tps25750, test_setter_propagates_bridge_failure) {
    emul_tps25750_bq_por_defaults(tps_emul);

    emul_tps25750_arm_cmd_wedge(tps_emul);
    zassert_equal(bq25792_set_charge_current_ma(bq_dev, 900), -ETIMEDOUT);

    /* Register unchanged at its POR value — checked via the backdoor, since
     * the wedge behavior persists on the bridge until the suite resets. */
    uint8_t be[2] = {0};
    zassert_ok(emul_tps25750_get_bq_reg(tps_emul, 0x03, be, sizeof(be)));
    zassert_equal(be[0], 0x00);
    zassert_equal(be[1], 0xC8); /* 2A */
}

/* ---- combined status decode (REG1B..REG1E burst) ---- */

ZTEST(emul_tps25750, test_get_status_decode) {
    /* IINDPM_STAT + WD_STAT + PG + VBUS_PRESENT; CHG_STAT=fast charge,
     * VBUS_STAT=SDP, BC1.2 done; ICO in progress + VBAT present; VSYSMIN
     * regulation active. Bit positions per SLUSDG1C Tables 9-36..9-39. */
    uint8_t regs[4] = {
        (uint8_t)(BIT(7) | BIT(5) | BIT(3) | BIT(0)), /* REG1B */
        (uint8_t)((3 << 5) | (1 << 1) | BIT(0)),      /* REG1C */
        (uint8_t)((1 << 6) | BIT(0)),                 /* REG1D */
        (uint8_t)BIT(4),                              /* REG1E */
    };
    zassert_ok(emul_tps25750_set_bq_reg(tps_emul, 0x1B, regs, sizeof(regs)));

    struct bq25792_status st;
    zassert_ok(bq25792_get_status(bq_dev, &st));

    zassert_true(st.iindpm_active);
    zassert_false(st.vindpm_active);
    zassert_true(st.wd_expired);
    zassert_false(st.poor_source);
    zassert_true(st.power_good);
    zassert_true(st.vbus_present);

    zassert_equal(st.chg_stat, 3); /* fast charge (CC) */
    zassert_equal(st.vbus_stat, 1); /* USB SDP */
    zassert_true(st.bc12_done);

    zassert_equal(st.ico_stat, 1); /* optimization in progress */
    zassert_false(st.thermal_regulation);
    zassert_true(st.vbat_present);

    zassert_true(st.vsysmin_regulation);
    zassert_false(st.chg_timer_expired);
}

ZTEST(emul_tps25750, test_get_status_propagates_bridge_failure) {
    struct bq25792_status st;

    emul_tps25750_arm_cmd_wedge(tps_emul);
    zassert_equal(bq25792_get_status(bq_dev, &st), -ETIMEDOUT);
}

ZTEST(emul_tps25750, test_get_vsys_mv) {
    /* VSYS_ADC (0x3D), 16-bit big-endian on the wire, 1mV/LSB. */
    uint8_t be[2] = {0x1F, 0x40}; /* 8000mV */
    zassert_ok(emul_tps25750_set_bq_reg(tps_emul, 0x3D, be, sizeof(be)));

    int32_t vsys = 0;
    zassert_ok(bq25792_get_vsys_mv(bq_dev, &vsys));
    zassert_equal(vsys, 8000);
}

/* ---- TPS25750 PD-status host registers ---- */

/* Build a USB-PD fixed-supply PDO: voltage in 50mV units (bits 19:10),
 * current in 10mA units (bits 9:0) — TRM Tables 2-22/2-23. */
static constexpr uint32_t fixed_pdo(uint32_t mv, uint32_t ma) {
    return ((mv / 50) << 10) | (ma / 10);
}

ZTEST(emul_tps25750, test_pd_power_info_disconnected) {
    /* POWER_STATUS all zeros: PowerConnection=0. */
    struct tps25750_pd_power_info info;
    zassert_ok(tps25750_get_pd_power_info(tps_dev, &info));
    zassert_false(info.connected);
    zassert_equal(info.source, TPS25750_PWR_NONE);
    zassert_equal(info.available_ma, 0);
}

ZTEST(emul_tps25750, test_pd_power_info_typec_tiers) {
    /* TypeCCurrent bits 3:2 of POWER_STATUS (TRM Table 2-33), plus
     * PowerConnection (bit 0) and SourceSink=sink (bit 1). */
    const struct {
        uint8_t tier;
        enum tps25750_power_source source;
        uint32_t ma;
    } cases[] = {
        {0x0, TPS25750_PWR_TYPEC_DEFAULT, 500},
        {0x1, TPS25750_PWR_TYPEC_1A5, 1500},
        {0x2, TPS25750_PWR_TYPEC_3A0, 3000},
    };

    for (const auto &c : cases) {
        uint8_t ps[2] = {(uint8_t)(BIT(0) | BIT(1) | (c.tier << 2)), 0x00};
        zassert_ok(emul_tps25750_set_host_reg(tps_emul, 0x3F, ps, sizeof(ps)));

        struct tps25750_pd_power_info info;
        zassert_ok(tps25750_get_pd_power_info(tps_dev, &info));
        zassert_true(info.connected);
        zassert_true(info.sinking);
        zassert_equal(info.source, c.source, "tier %u", c.tier);
        zassert_equal(info.available_mv, 5000);
        zassert_equal(info.available_ma, c.ma, "tier %u: %u mA", c.tier, info.available_ma);
    }
}

ZTEST(emul_tps25750, test_pd_power_info_explicit_contract) {
    /* TypeCCurrent=11b: explicit PD contract -> decode ACTIVE_CONTRACT_PDO.
     * 9V/1.65A fixed PDO. */
    uint8_t ps[2] = {(uint8_t)(BIT(0) | BIT(1) | (0x3 << 2)), 0x00};
    zassert_ok(emul_tps25750_set_host_reg(tps_emul, 0x3F, ps, sizeof(ps)));

    uint32_t pdo = fixed_pdo(9000, 1650);
    uint8_t pdo_payload[6] = {
        (uint8_t)(pdo & 0xFF),         (uint8_t)((pdo >> 8) & 0xFF),
        (uint8_t)((pdo >> 16) & 0xFF), (uint8_t)((pdo >> 24) & 0xFF),
        0x00,                          0x00,
    };
    zassert_ok(emul_tps25750_set_host_reg(tps_emul, 0x34, pdo_payload, sizeof(pdo_payload)));

    struct tps25750_pd_power_info info;
    zassert_ok(tps25750_get_pd_power_info(tps_dev, &info));
    zassert_equal(info.source, TPS25750_PWR_PD_CONTRACT);
    zassert_equal(info.available_mv, 9000);
    zassert_equal(info.available_ma, 1650);
    zassert_equal(info.raw_pdo, pdo);
}

ZTEST(emul_tps25750, test_pd_power_info_nonfixed_contract_falls_back) {
    /* An augmented (PPS) PDO — bits 31:30 = 11b — must not be decoded as a
     * fixed contract; conservative USB-default budget instead. */
    uint8_t ps[2] = {(uint8_t)(BIT(0) | BIT(1) | (0x3 << 2)), 0x00};
    zassert_ok(emul_tps25750_set_host_reg(tps_emul, 0x3F, ps, sizeof(ps)));

    uint32_t pps_pdo = (0x3u << 30) | fixed_pdo(9000, 3000);
    uint8_t pdo_payload[4] = {
        (uint8_t)(pps_pdo & 0xFF),
        (uint8_t)((pps_pdo >> 8) & 0xFF),
        (uint8_t)((pps_pdo >> 16) & 0xFF),
        (uint8_t)((pps_pdo >> 24) & 0xFF),
    };
    zassert_ok(emul_tps25750_set_host_reg(tps_emul, 0x34, pdo_payload, sizeof(pdo_payload)));

    struct tps25750_pd_power_info info;
    zassert_ok(tps25750_get_pd_power_info(tps_dev, &info));
    zassert_equal(info.source, TPS25750_PWR_UNKNOWN);
    zassert_equal(info.available_mv, 5000);
    zassert_equal(info.available_ma, 500);
}

ZTEST(emul_tps25750, test_read_tx_sink_caps) {
    /* Header byte: numValidPDOs=2 (bits 2:0), then two LE32 PDOs. */
    uint32_t pdo1 = fixed_pdo(5000, 3000);
    uint32_t pdo2 = fixed_pdo(9000, 1500);
    uint8_t payload[9] = {
        0x02,
        (uint8_t)(pdo1 & 0xFF),
        (uint8_t)((pdo1 >> 8) & 0xFF),
        (uint8_t)((pdo1 >> 16) & 0xFF),
        (uint8_t)((pdo1 >> 24) & 0xFF),
        (uint8_t)(pdo2 & 0xFF),
        (uint8_t)((pdo2 >> 8) & 0xFF),
        (uint8_t)((pdo2 >> 16) & 0xFF),
        (uint8_t)((pdo2 >> 24) & 0xFF),
    };
    zassert_ok(emul_tps25750_set_host_reg(tps_emul, 0x33, payload, sizeof(payload)));

    uint32_t pdos[7] = {0};
    uint8_t num = 0;
    zassert_ok(tps25750_read_tx_sink_caps(tps_dev, pdos, ARRAY_SIZE(pdos), &num));
    zassert_equal(num, 2);
    zassert_equal(pdos[0], pdo1);
    zassert_equal(pdos[1], pdo2);
}

/* ---- concurrency regression test for the task_mutex (commit 442f47f) ---- */

#define HAMMER_ITERATIONS 100
#define HAMMER_STACK_SIZE 2048
#define HAMMER_PRIO 5

K_THREAD_STACK_DEFINE(vbat_hammer_stack, HAMMER_STACK_SIZE);
K_THREAD_STACK_DEFINE(vbus_hammer_stack, HAMMER_STACK_SIZE);
static struct k_thread vbat_hammer_thread;
static struct k_thread vbus_hammer_thread;
static atomic_t vbat_failures;
static atomic_t vbus_failures;

static void vbat_hammer(void *, void *, void *) {
    for (int i = 0; i < HAMMER_ITERATIONS; i++) {
        int32_t vbat = 0;
        /* Getters don't propagate I2C errors (a failed read leaves the value
         * stale/zero), so the sentinel comparison is the actual check. */
        if (bq25792_get_vbat_mv(bq_dev, &vbat) != 0 || vbat != kVbatMv) {
            atomic_inc(&vbat_failures);
        }
        /* Deterministic simulated time lets equal-period threads settle into
         * a stable alternation that can dodge the race; sleeping on
         * different iteration strides keeps the two threads' bridge
         * sequences continuously de-phased. */
        if (i % 3 == 0) {
            k_msleep(10);
        }
    }
}

static void vbus_hammer(void *, void *, void *) {
    for (int i = 0; i < HAMMER_ITERATIONS; i++) {
        int32_t vbus = 0;
        if (bq25792_get_vbus_mv(bq_dev, &vbus) != 0 || vbus != kVbusMv) {
            atomic_inc(&vbus_failures);
        }
        if (i % 4 == 0) {
            k_msleep(10);
        }
    }
}

ZTEST(emul_tps25750, test_concurrent_bridge_reads_are_serialized) {
    seed_bq_adc_regs();

    /* Busy window longer than one poll period: every bridged transfer takes
     * at least two k_msleep(10) rounds in the CMD1 poll — the interleave
     * points that corrupted transactions before the task_mutex existed. */
    emul_tps25750_set_cmd_delay_ms(tps_emul, 15);

    atomic_set(&vbat_failures, 0);
    atomic_set(&vbus_failures, 0);

    k_thread_create(&vbat_hammer_thread, vbat_hammer_stack, HAMMER_STACK_SIZE, vbat_hammer, NULL,
                    NULL, NULL, HAMMER_PRIO, 0, K_NO_WAIT);
    k_thread_create(&vbus_hammer_thread, vbus_hammer_stack, HAMMER_STACK_SIZE, vbus_hammer, NULL,
                    NULL, NULL, HAMMER_PRIO, 0, K_NO_WAIT);

    zassert_ok(k_thread_join(&vbat_hammer_thread, K_SECONDS(60)));
    zassert_ok(k_thread_join(&vbus_hammer_thread, K_SECONDS(60)));

    zassert_equal(atomic_get(&vbat_failures), 0,
                  "%ld/%d VBAT reads failed or returned another register's value",
                  atomic_get(&vbat_failures), HAMMER_ITERATIONS);
    zassert_equal(atomic_get(&vbus_failures), 0,
                  "%ld/%d VBUS reads failed or returned another register's value",
                  atomic_get(&vbus_failures), HAMMER_ITERATIONS);
}

/* ---- error paths, driven directly through the bridge's i2c API (the BQ
 * getters swallow I2C errors, so assert on the bridge's own return) ---- */

ZTEST(emul_tps25750, test_cmd_rejected_returns_ebusy) {
    uint8_t buf[2];

    emul_tps25750_arm_cmd_reject(tps_emul);
    zassert_equal(i2c_burst_read(tps_dev, 0x6B, kRegVbatAdc, buf, sizeof(buf)), -EBUSY);
}

ZTEST(emul_tps25750, test_cmd_wedged_returns_etimedout) {
    uint8_t buf[2];

    emul_tps25750_arm_cmd_wedge(tps_emul);
    /* ~1s of simulated polling (100 x 10ms) before the driver's bounded
     * CMD1 poll gives up — instant in wall-clock time on native_sim. */
    zassert_equal(i2c_burst_read(tps_dev, 0x6B, kRegVbatAdc, buf, sizeof(buf)), -ETIMEDOUT);
}

ZTEST(emul_tps25750, test_i2cm_status_failure_returns_efault) {
    uint8_t buf[2];

    /* 107 == 0x6B: the exact status byte from the original hardware bug
     * ("PD Controller I2CM read failure: 107"). */
    emul_tps25750_arm_i2cm_status(tps_emul, 107);
    zassert_equal(i2c_burst_read(tps_dev, 0x6B, kRegVbatAdc, buf, sizeof(buf)), -EFAULT);

    /* One-shot: the next transfer works again. */
    zassert_ok(i2c_burst_read(tps_dev, 0x6B, kRegVbatAdc, buf, sizeof(buf)));
}

ZTEST(emul_tps25750, test_clear_dead_battery_sends_dbfg) {
    char four_cc[5];

    zassert_ok(tps25750_clear_dead_battery(tps_dev));
    emul_tps25750_last_4cc(tps_emul, four_cc);
    zassert_mem_equal(four_cc, "DBfg", 4);
}

/* ---- patch download ---- */

/* In both scenarios MODE is "APP " by the time tests run (booted there, or
 * transitioned there by the boot-time download), so a host-initiated
 * download must short-circuit to "Patch already loaded" and succeed. */
ZTEST(emul_tps25750, test_download_patch_already_loaded) {
    char mode[5];

    emul_tps25750_get_mode(tps_emul, mode);
    zassert_mem_equal(mode, "APP ", 4);
    zassert_ok(tps25750_download_patch(tps_dev, NULL, 0));
}

/* Only meaningful in the drivers.emul_tps25750.patch_download scenario: the
 * emulated part booted in PTCH mode and CONFIG_TPS25750_INTERNAL_PATCH made
 * tps25750_init run the full download during boot; assert what the emulator
 * recorded. */
ZTEST(emul_tps25750, test_patch_download_at_boot) {
#if defined(CONFIG_TPS25750_INTERNAL_PATCH)
    struct emul_tps25750_patch_stats stats;
    char mode[5];
    const char *patch = NULL;
    size_t patch_size = 0;

    emul_tps25750_get_patch_stats(tps_emul, &stats);
    emul_tps25750_get_mode(tps_emul, mode);

    zassert_true(stats.pbms_seen, "driver never sent PBMs");
    zassert_true(stats.pbmc_seen, "driver never sent PBMc");
    zassert_equal(stats.patch_address, 0x15);
    zassert_equal(stats.timeout, 0x32); /* 5s, hardcoded in the driver */

    zassert_ok(tps25750_get_patch(&patch, &patch_size));
    zassert_equal(stats.announced_size, patch_size);
    zassert_equal(stats.rx_count, patch_size, "received %u of %zu patch bytes", stats.rx_count,
                  patch_size);
    uint32_t expected_hash = emul_tps25750_hash_update(EMUL_TPS25750_HASH_INIT,
                                                       (const uint8_t *)patch, patch_size);
    zassert_equal(stats.rx_hash, expected_hash, "patch bytes corrupted in transfer");

    /* The download's success is what moved the part PTCH -> APP. */
    zassert_mem_equal(mode, "APP ", 4);
#else
    ztest_test_skip();
#endif
}

static void suite_before(void *fixture) {
    ARG_UNUSED(fixture);
    emul_tps25750_reset(tps_emul);
}

ZTEST_SUITE(emul_tps25750, NULL, NULL, suite_before, NULL, NULL);
