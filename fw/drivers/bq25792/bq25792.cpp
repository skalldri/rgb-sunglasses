// NOTE: On both boards the bq25792 devicetree node is a child of the tps25750 node,
// so every register access here is reached via the TPS25750 I2Cm bridge — each I2C
// transfer becomes a CMD1/DATA1 4CC task sequence in drivers/tps25750/tps25750.c,
// serialized by that driver's per-device task_mutex (PR #111). That mutex is internal
// to the bridge and covers ONE transfer at a time: a multi-transfer sequence here
// (e.g. a read-modify-write) is NOT atomic against other bridge users (PD-controller
// 4CC tasks, other bq25792 callers) and must bring its own serialization — see the
// "multi-step I2C/register transaction" coding rule in fw/CLAUDE.md.
#include <zephyr/drivers/bq25792/bq25792.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

#include "bq25792_init.h"
#include "bq25792_irq.h"

LOG_MODULE_REGISTER(bq25792, LOG_LEVEL_INF);

// Must be included after LOG_MODULE_REGISTER() since this contains LOG_XXX statements
// TODO: fix this!
#include "bq25792_priv.h"

namespace {

/* Scoped holder of bq25792_dev_data::xact_mutex — every multi-transfer
 * sequence (read-modify-write, write + read-back-verify) must hold it for the
 * whole sequence; the bridge's task_mutex only serializes single transfers. */
class XactGuard {
   public:
    explicit XactGuard(const struct device* dev)
        : mutex_(&((struct bq25792_dev_data*)dev->data)->xact_mutex) {
        k_mutex_lock(mutex_, K_FOREVER);
    }
    ~XactGuard() { k_mutex_unlock(mutex_); }
    XactGuard(const XactGuard&) = delete;
    XactGuard& operator=(const XactGuard&) = delete;

   private:
    struct k_mutex* mutex_;
};

/* Write one field of a register, then read the register back and verify the
 * field took. The TPS25750 host-interface TRM ("I2Cw Task Completion")
 * mandates read-back confirmation for writes through the I2Cm bridge: a
 * successful I2Cw only means the write was queued, and the PD controller's
 * own event-driven charger writes share that queue. Caller must hold the
 * xact_mutex (XactGuard). */
template <typename Reg, typename Field>
int set_field_verified(const struct bq25792_dev_config* cfg, uint32_t value) {
    Reg reg(cfg);
    // The constructor already read the register but swallowed any bus error —
    // re-read explicitly so a dead bus can't turn into a bogus RMW.
    int ret = reg.read();
    if (ret) {
        return ret;
    }

    ret = reg.template set<Field>(value, true /* flush */);
    if (ret) {
        // Failure has been reported to the caller — the destructor must not
        // silently retry the write and apply the change behind their back.
        reg.discard();
        return ret;
    }

    ret = reg.read();
    if (ret) {
        return ret;
    }

    if (reg.template get<Field>(0, false) != value) {
        LOG_ERR("%s read-back mismatch (wanted %u)", Field::getName(), value);
        return -EIO;
    }

    return 0;
}

/* Flush a single-field write, discarding the pending local change on failure
 * (see I2CDeviceRegister::discard()) so the register object's destructor
 * can't silently retry a write whose failure was already reported. */
template <typename Field, typename Reg>
int set_field(Reg& reg, uint32_t value) {
    int ret = reg.template set<Field>(value, true /* flush */);
    if (ret) {
        reg.discard();
    }
    return ret;
}

}  // namespace

/* X-Macro Definition for bq25792_dump() function */
#define REG(_regName)      \
    {                      \
        _regName tmp(cfg); \
        tmp.dump();        \
    }

int bq25792_dump(const struct device* dev) {
    if (!dev) {
        LOG_ERR("NULL-device pointer");
        return -ENODEV;
    }

    const struct bq25792_dev_config* cfg = (const struct bq25792_dev_config*)dev->config;

    if (!device_is_ready(cfg->i2c.bus)) {
        LOG_ERR("bus not ready");
        return -ENODEV;
    }

#if defined(CONFIG_DUMP_DEVICE_REGISTERS)

    REG_LIST

#endif

    return 0;
}
#undef REG

int bq25792_temp_override(const struct device* dev, bool enable) {
    if (!dev) {
        LOG_ERR("NULL-device pointer");
        return -ENODEV;
    }

    const struct bq25792_dev_config* cfg = (const struct bq25792_dev_config*)dev->config;

    XactGuard guard(dev);  // ctor-read + flush = two transfers
    BQ25792_NTC_CONTROL_1 reg(cfg);

    uint32_t ts_ignore = enable ? 1 : 0;
    LOG_INF("Setting TS_IGNORE to %u", ts_ignore);

    return set_field<BQ25792_NTC_CONTROL_1_TS_IGNORE>(reg, ts_ignore);
}

int bq25792_adc_enable(const struct device* dev, bool enable) {
    if (!dev) {
        LOG_ERR("NULL-device pointer");
        return -ENODEV;
    }

    const struct bq25792_dev_config* cfg = (const struct bq25792_dev_config*)dev->config;

    XactGuard guard(dev);  // ctor-read + flush = two transfers
    BQ25792_ADC_CONTROL reg(cfg);

    uint32_t adc_en = enable ? 1 : 0;
    LOG_INF("Setting ADC_EN to %u", adc_en);

    return set_field<BQ25792_ADC_CONTROL_ADC_EN>(reg, adc_en);
}

int bq25792_pfm_enable(const struct device* dev, bool enable) {
    if (!dev) {
        LOG_ERR("NULL-device pointer");
        return -ENODEV;
    }

    const struct bq25792_dev_config* cfg = (const struct bq25792_dev_config*)dev->config;

    XactGuard guard(dev);  // ctor-read + flush = two transfers
    BQ25792_CHARGER_CONTROL_3 reg(cfg);

    uint32_t pfm_fwd_dis = enable ? 0 : 1;
    LOG_INF("Setting PFM_FWD_DIS to %u", pfm_fwd_dis);

    return set_field<BQ25792_CHARGER_CONTROL_3_PFM_FWD_DIS>(reg, pfm_fwd_dis);
}

int bq25792_set_charge_frequency(const struct device* dev, bq25792_charge_frequency_t freq) {
    if (!dev) {
        LOG_ERR("NULL-device pointer");
        return -ENODEV;
    }

    if (freq > NUM_CHARGE_FREQUENCY) {
        LOG_ERR("Invalid frequency setting %u", freq);
        return -EINVAL;
    }

    const struct bq25792_dev_config* cfg = (const struct bq25792_dev_config*)dev->config;

    XactGuard guard(dev);  // ctor-read + flush = two transfers
    BQ25792_CHARGER_CONTROL_4 reg(cfg);

    uint32_t pwm_freq = (freq == bq25792_charge_frequency_t::LOW) ? 1 : 0;
    LOG_INF("Setting PWM_FREQ to %u", pwm_freq);

    return set_field<BQ25792_CHARGER_CONTROL_4_PWM_FREQ>(reg, pwm_freq);
}

int bq25792_dump_charge_parameters(const struct device* dev) {
    const struct bq25792_dev_config* cfg = (const struct bq25792_dev_config*)dev->config;

    BQ25792_MINIMAL_SYSTEM_VOLTAGE minimalSystemVoltage(cfg);
    BQ25792_RECHARGE_CONTROL rechargeControl(cfg);
    BQ25792_CHARGE_VOLTAGE_LIMIT chargeVoltageLimit(cfg);
    BQ25792_VBAT_ADC vbatAdc(cfg);

    BQ25792_CHARGER_CONTROL_0 chargerControl0(cfg);

    BQ25792_CHARGER_STATUS_0 chargerStatus0(cfg);
    BQ25792_CHARGER_STATUS_1 chargerStatus1(cfg);
    BQ25792_CHARGER_STATUS_2 chargerStatus2(cfg);
    BQ25792_CHARGER_STATUS_3 chargerStatus3(cfg);
    BQ25792_CHARGER_STATUS_4 chargerStatus4(cfg);

    BQ25792_FAULT_STATUS_0 faultStatus0(cfg);
    BQ25792_FAULT_STATUS_1 faultStatus1(cfg);

    minimalSystemVoltage.dump();
    rechargeControl.dump();
    chargeVoltageLimit.dump();
    vbatAdc.dump();
    chargerControl0.dump();
    chargerStatus0.dump();
    chargerStatus1.dump();
    chargerStatus2.dump();
    chargerStatus3.dump();
    chargerStatus4.dump();
    faultStatus0.dump();
    faultStatus1.dump();

    return 0;
}

int bq25792_get_charge_status(const struct device* dev, uint8_t* chg_stat) {
    if (!dev || !chg_stat) {
        return -EINVAL;
    }
    const struct bq25792_dev_config* cfg = (const struct bq25792_dev_config*)dev->config;
    BQ25792_CHARGER_STATUS_1 reg(cfg);
    *chg_stat = (uint8_t)reg.get<BQ25792_CHARGER_STATUS_1_CHG_STAT>(0, true /* read from hw */);
    return 0;
}

int bq25792_get_vbat_mv(const struct device* dev, int32_t* vbat_mv) {
    if (!dev || !vbat_mv) {
        return -EINVAL;
    }
    const struct bq25792_dev_config* cfg = (const struct bq25792_dev_config*)dev->config;
    BQ25792_VBAT_ADC reg(cfg);
    uint32_t raw = reg.get<BQ25792_VBAT_ADC_VBAT_ADC>(0, true /* read from hw */);
    *vbat_mv = (int32_t)BQ25792_ADC_VOLTAGE_UnitConversion::conversion(raw);
    return 0;
}

int bq25792_set_charge_enable(const struct device* dev, bool enabled) {
    if (!dev) {
        LOG_ERR("NULL-device pointer");
        return -ENODEV;
    }
    const struct bq25792_dev_config* cfg = (const struct bq25792_dev_config*)dev->config;
    XactGuard guard(dev);  // ctor-read + flush = two transfers
    BQ25792_CHARGER_CONTROL_0 chargerControl0(cfg);
    LOG_INF("Setting EN_CHG to %u", enabled ? 1 : 0);
    // Propagate the I2C flush result — callers (e.g. the BLE charging toggle)
    // must be able to reject the request when the bus write fails.
    return set_field<BQ25792_CHARGER_CONTROL_0_EN_CHG>(chargerControl0, enabled ? 1 : 0);
}

int bq25792_get_charge_enable(const struct device* dev, bool* enabled) {
    if (!dev || !enabled) {
        return -EINVAL;
    }
    const struct bq25792_dev_config* cfg = (const struct bq25792_dev_config*)dev->config;
    BQ25792_CHARGER_CONTROL_0 reg(cfg);
    // Explicit read so bus errors propagate — a failed bridged read must not
    // decode as "EN_CHG=0" (callers reconcile against this value).
    int ret = reg.read();
    if (ret) {
        return ret;
    }
    *enabled = reg.get<BQ25792_CHARGER_CONTROL_0_EN_CHG>(0, false) != 0;
    return 0;
}

int bq25792_ibat_sense_enable(const struct device* dev, bool enable) {
    if (!dev) {
        LOG_ERR("NULL-device pointer");
        return -ENODEV;
    }
    const struct bq25792_dev_config* cfg = (const struct bq25792_dev_config*)dev->config;
    XactGuard guard(dev);  // ctor-read + flush = two transfers
    BQ25792_CHARGER_CONTROL_5 reg(cfg);

    uint32_t en_ibat = enable ? 1 : 0;
    LOG_INF("Setting EN_IBAT to %u", en_ibat);

    return set_field<BQ25792_CHARGER_CONTROL_5_EN_IBAT>(reg, en_ibat);
}

int bq25792_get_ibat_ma(const struct device* dev, int32_t* ibat_ma) {
    if (!dev || !ibat_ma) {
        return -EINVAL;
    }
    const struct bq25792_dev_config* cfg = (const struct bq25792_dev_config*)dev->config;
    BQ25792_IBAT_ADC reg(cfg);
    uint32_t raw = reg.get<BQ25792_IBAT_ADC_IBAT_ADC>(0, true /* read from hw */);
    *ibat_ma = (int32_t)BQ25792_ADC_CURRENT_UnitConversion::conversion(raw);
    return 0;
}

int bq25792_get_vbus_mv(const struct device* dev, int32_t* vbus_mv) {
    if (!dev || !vbus_mv) {
        return -EINVAL;
    }
    const struct bq25792_dev_config* cfg = (const struct bq25792_dev_config*)dev->config;
    BQ25792_VBUS_ADC reg(cfg);
    uint32_t raw = reg.get<BQ25792_VBUS_ADC_VBUS_ADC>(0, true /* read from hw */);
    *vbus_mv = (int32_t)BQ25792_ADC_VOLTAGE_UnitConversion::conversion(raw);
    return 0;
}

int bq25792_get_ibus_ma(const struct device* dev, int32_t* ibus_ma) {
    if (!dev || !ibus_ma) {
        return -EINVAL;
    }
    const struct bq25792_dev_config* cfg = (const struct bq25792_dev_config*)dev->config;
    BQ25792_IBUS_ADC reg(cfg);
    uint32_t raw = reg.get<BQ25792_IBUS_ADC_IBUS_ADC>(0, true /* read from hw */);
    *ibus_ma = (int32_t)BQ25792_ADC_CURRENT_UnitConversion::conversion(raw);
    return 0;
}

int bq25792_get_vsys_mv(const struct device* dev, int32_t* vsys_mv) {
    if (!dev || !vsys_mv) {
        return -EINVAL;
    }
    const struct bq25792_dev_config* cfg = (const struct bq25792_dev_config*)dev->config;
    BQ25792_VSYS_ADC reg(cfg);
    // The constructor's read swallows bus errors — re-read explicitly so this
    // getter propagates them (unlike the legacy bq25792_get_* getters).
    int ret = reg.read();
    if (ret) {
        return ret;
    }
    uint32_t raw = reg.get<BQ25792_VSYS_ADC_VSYS_ADC>(0, false);
    *vsys_mv = (int32_t)BQ25792_ADC_VOLTAGE_UnitConversion::conversion(raw);
    return 0;
}

int bq25792_get_status(const struct device* dev, struct bq25792_status* status) {
    if (!dev || !status) {
        return -EINVAL;
    }
    const struct bq25792_dev_config* cfg = (const struct bq25792_dev_config*)dev->config;

    // REG1B..REG1E (Charger_Status_0..3) are consecutive 8-bit registers —
    // fetch all four in one burst read (a single bridged I2Cm transaction)
    // and decode locally. Datasheet SLUSDG1C Tables 9-36..9-39.
    uint8_t raw[4];
    int ret = i2c_burst_read_dt(&cfg->i2c, BQ25792_REG_CHARGER_STATUS_0_ADDR, raw, sizeof(raw));
    if (ret) {
        LOG_ERR("status burst read failed: %d", ret);
        return ret;
    }

    // Decode via the same RegisterField definitions the register classes use,
    // so bit positions live in exactly one place (bq25792_priv.h).
    status->iindpm_active = BQ25792_CHARGER_STATUS_0_IINDPM_STAT::getValue(raw[0]) != 0;
    status->vindpm_active = BQ25792_CHARGER_STATUS_0_VINDPM_STAT::getValue(raw[0]) != 0;
    status->wd_expired = BQ25792_CHARGER_STATUS_0_WD_STAT::getValue(raw[0]) != 0;
    status->poor_source = BQ25792_CHARGER_STATUS_0_POORSRC_STAT::getValue(raw[0]) != 0;
    status->power_good = BQ25792_CHARGER_STATUS_0_PG_STAT::getValue(raw[0]) != 0;
    status->vbus_present = BQ25792_CHARGER_STATUS_0_VBUS_PRESENT_STAT::getValue(raw[0]) != 0;

    status->chg_stat = (uint8_t)BQ25792_CHARGER_STATUS_1_CHG_STAT::getValue(raw[1]);
    status->vbus_stat = (uint8_t)BQ25792_CHARGER_STATUS_1_VBUS_STAT::getValue(raw[1]);
    status->bc12_done = BQ25792_CHARGER_STATUS_1_BC1_2_DONE_STAT::getValue(raw[1]) != 0;

    status->ico_stat = (uint8_t)BQ25792_CHARGER_STATUS_2_ICO_STAT::getValue(raw[2]);
    status->thermal_regulation = BQ25792_CHARGER_STATUS_2_TREG_STAT::getValue(raw[2]) != 0;
    status->dpdm_ongoing = BQ25792_CHARGER_STATUS_2_DPDM_STAT::getValue(raw[2]) != 0;
    status->vbat_present = BQ25792_CHARGER_STATUS_2_VBAT_PRESENT_STAT::getValue(raw[2]) != 0;

    status->adc_done = BQ25792_CHARGER_STATUS_3_ADC_DONE_STAT::getValue(raw[3]) != 0;
    status->vsysmin_regulation = BQ25792_CHARGER_STATUS_3_VSYS_STAT::getValue(raw[3]) != 0;
    status->chg_timer_expired = BQ25792_CHARGER_STATUS_3_CHG_TMR_STAT::getValue(raw[3]) != 0;
    status->trickle_timer_expired =
        BQ25792_CHARGER_STATUS_3_TRICHG_TMR_STAT::getValue(raw[3]) != 0;
    status->precharge_timer_expired =
        BQ25792_CHARGER_STATUS_3_PRECHG_TMR_STAT::getValue(raw[3]) != 0;

    return 0;
}

const char* bq25792_vbus_stat_str(uint8_t vbus_stat) {
    // Datasheet SLUSDG1C Table 9-37, VBUS_STAT_3:0 encodings.
    switch (vbus_stat) {
        case 0x0:
            return "No input";
        case 0x1:
            return "USB SDP (500mA)";
        case 0x2:
            return "USB CDP (1.5A)";
        case 0x3:
            return "USB DCP (3.25A)";
        case 0x4:
            return "HVDCP (1.5A)";
        case 0x5:
            return "Unknown adapter (3A)";
        case 0x6:
            return "Non-standard adapter";
        case 0x7:
            return "OTG mode";
        case 0x8:
            return "Not qualified adapter";
        case 0xB:
            return "Powered from VBUS";
        default:
            return "Reserved";
    }
}

int bq25792_get_limits(const struct device* dev, struct bq25792_limits* limits) {
    if (!dev || !limits) {
        return -EINVAL;
    }
    const struct bq25792_dev_config* cfg = (const struct bq25792_dev_config*)dev->config;
    int ret;

    {
        BQ25792_CHARGE_CURRENT_LIMIT reg(cfg);
        if ((ret = reg.read())) {
            return ret;
        }
        // REG03 ICHG_8:0, 10mA/LSB (Table 9-16)
        limits->ichg_ma = reg.get<BQ25792_CHARGE_CURRENT_LIMIT_ICHG>(0, false) * 10;
    }
    {
        BQ25792_INPUT_VOLTAGE_LIMIT reg(cfg);
        if ((ret = reg.read())) {
            return ret;
        }
        // REG05 VINDPM_7:0, 100mV/LSB (Table 9-17)
        limits->vindpm_mv = reg.get<BQ25792_INPUT_VOLTAGE_LIMIT_VINDPM>(0, false) * 100;
    }
    {
        BQ25792_INPUT_CURRENT_LIMIT reg(cfg);
        if ((ret = reg.read())) {
            return ret;
        }
        // REG06 IINDPM_8:0, 10mA/LSB (Table 9-18)
        limits->iindpm_ma = reg.get<BQ25792_INPUT_CURRENT_LIMIT_IINDPM>(0, false) *
                            BQ25792_REG_INPUT_CURRENT_LIMIT_IINDPM_SCALE;
    }
    {
        BQ25792_ICO_CURRENT_LIMIT reg(cfg);
        if ((ret = reg.read())) {
            return ret;
        }
        // REG19 ICO_ILIM_8:0, 10mA/LSB (Table 9-35)
        limits->ico_ilim_ma = reg.get<BQ25792_ICO_CURRENT_LIMIT_ICO_ILIM>(0, false) * 10;
    }
    {
        BQ25792_CHARGER_CONTROL_1 reg(cfg);
        if ((ret = reg.read())) {
            return ret;
        }
        limits->watchdog = (uint8_t)reg.get<BQ25792_CHARGER_CONTROL_1_WATCHDOG>(0, false);
        limits->vac_ovp = (uint8_t)reg.get<BQ25792_CHARGER_CONTROL_1_VAC_OVP>(0, false);
    }

    return 0;
}

int bq25792_set_charge_current_ma(const struct device* dev, uint32_t ichg_ma) {
    if (!dev) {
        return -ENODEV;
    }
    const struct bq25792_dev_config* cfg = (const struct bq25792_dev_config*)dev->config;

    // REG03 ICHG_8:0: 10mA/LSB, range 50mA-5000mA, clamped low by the part
    // (datasheet SLUSDG1C Table 9-16, p.57). Clamp here too so the read-back
    // comparison can't fail on an out-of-range request.
    uint32_t clamped = CLAMP(ichg_ma, 50, 5000);
    if (clamped != ichg_ma) {
        LOG_WRN("ICHG %umA clamped to %umA", ichg_ma, clamped);
    }
    LOG_INF("Setting ICHG to %umA", clamped);

    XactGuard guard(dev);
    return set_field_verified<BQ25792_CHARGE_CURRENT_LIMIT, BQ25792_CHARGE_CURRENT_LIMIT_ICHG>(
        cfg, clamped / 10);
}

int bq25792_set_input_current_limit_ma(const struct device* dev, uint32_t iindpm_ma) {
    if (!dev) {
        return -ENODEV;
    }
    const struct bq25792_dev_config* cfg = (const struct bq25792_dev_config*)dev->config;

    // REG06 IINDPM_8:0: 10mA/LSB, range 100mA-3300mA (datasheet SLUSDG1C
    // Table 9-18, p.59). The part re-derives this register from D+/D-
    // detection on every plug-in — callers own re-application.
    uint32_t clamped = CLAMP(iindpm_ma, 100, 3300);
    if (clamped != iindpm_ma) {
        LOG_WRN("IINDPM %umA clamped to %umA", iindpm_ma, clamped);
    }
    LOG_INF("Setting IINDPM to %umA", clamped);

    XactGuard guard(dev);
    return set_field_verified<BQ25792_INPUT_CURRENT_LIMIT, BQ25792_INPUT_CURRENT_LIMIT_IINDPM>(
        cfg, clamped / 10);
}

int bq25792_set_input_voltage_limit_mv(const struct device* dev, uint32_t vindpm_mv) {
    if (!dev) {
        return -ENODEV;
    }
    const struct bq25792_dev_config* cfg = (const struct bq25792_dev_config*)dev->config;

    // REG05 VINDPM_7:0: 100mV/LSB, range 3600mV-22000mV (datasheet SLUSDG1C
    // Table 9-17, p.58). Reset to 3600mV on adapter unplug and re-derived
    // from the VBUS measurement on plug-in — callers own re-application.
    uint32_t clamped = CLAMP(vindpm_mv, 3600, 22000);
    if (clamped != vindpm_mv) {
        LOG_WRN("VINDPM %umV clamped to %umV", vindpm_mv, clamped);
    }
    LOG_INF("Setting VINDPM to %umV", clamped);

    XactGuard guard(dev);
    return set_field_verified<BQ25792_INPUT_VOLTAGE_LIMIT, BQ25792_INPUT_VOLTAGE_LIMIT_VINDPM>(
        cfg, clamped / 100);
}

int bq25792_watchdog_disable(const struct device* dev) {
    if (!dev) {
        return -ENODEV;
    }
    const struct bq25792_dev_config* cfg = (const struct bq25792_dev_config*)dev->config;

    // REG10 WATCHDOG_2:0 = 0h disables the I2C watchdog (datasheet SLUSDG1C
    // Table 9-26, p.68; POR is 5h = 40s). On expiry the watchdog reverts all
    // watchdog-scoped registers (ICHG, EN_CHG, ...) to POR defaults — which
    // for this hardware is NOT a safe state (EN_CHG=1 with no battery causes
    // the documented §9.3.6 SYS collapse). Disabling is deliberate policy.
    LOG_INF("Disabling I2C watchdog");

    XactGuard guard(dev);
    return set_field_verified<BQ25792_CHARGER_CONTROL_1, BQ25792_CHARGER_CONTROL_1_WATCHDOG>(cfg,
                                                                                             0);
}

int bq25792_watchdog_feed(const struct device* dev) {
    if (!dev) {
        return -ENODEV;
    }
    const struct bq25792_dev_config* cfg = (const struct bq25792_dev_config*)dev->config;

    // REG10 WD_RST = 1 resets the watchdog timer and self-clears (datasheet
    // SLUSDG1C Table 9-26, p.68) — no read-back verify, the bit may already
    // be 0 again by the time we re-read.
    XactGuard guard(dev);  // ctor-read + flush = two transfers
    BQ25792_CHARGER_CONTROL_1 reg(cfg);
    int ret = reg.read();
    if (ret) {
        return ret;
    }
    return set_field<BQ25792_CHARGER_CONTROL_1_WD_RST>(reg, 1);
}

int bq25792_hiz_enable(const struct device* dev, bool enable) {
    if (!dev) {
        return -ENODEV;
    }
    const struct bq25792_dev_config* cfg = (const struct bq25792_dev_config*)dev->config;

    // REG0F EN_HIZ (bit 2): 1 = input high-impedance — the charger stops
    // drawing from VBUS and the system runs from battery; USB data is
    // unaffected (D+/D- don't route through the charger). POR 0; the part
    // AUTO-CLEARS this bit when an adapter is plugged in at VBUS (datasheet
    // SLUSDG1C Table 9-25, p.66) — so a replug always restores input power.
    LOG_INF("Setting EN_HIZ to %u", enable ? 1 : 0);

    XactGuard guard(dev);
    return set_field_verified<BQ25792_CHARGER_CONTROL_0, BQ25792_CHARGER_CONTROL_0_EN_HIZ>(
        cfg, enable ? 1 : 0);
}

int bq25792_set_vac_ovp(const struct device* dev, uint8_t vac_ovp) {
    if (!dev) {
        return -ENODEV;
    }
    if (vac_ovp > 0x3) {
        return -EINVAL;
    }
    const struct bq25792_dev_config* cfg = (const struct bq25792_dev_config*)dev->config;

    // REG10 VAC_OVP_1:0 (bits 5:4): 0h=26V, 1h=18V, 2h=12V, 3h=7V (datasheet
    // SLUSDG1C Table 9-26, p.68). Note the datasheet's register-level reset
    // annotations for REG10 are self-inconsistent — programming this
    // explicitly makes the threshold deterministic regardless of POR.
    LOG_INF("Setting VAC_OVP to %u", vac_ovp);

    XactGuard guard(dev);
    return set_field_verified<BQ25792_CHARGER_CONTROL_1, BQ25792_CHARGER_CONTROL_1_VAC_OVP>(
        cfg, vac_ovp);
}

int bq25792_ico_enable(const struct device* dev, bool enable) {
    if (!dev) {
        return -ENODEV;
    }
    const struct bq25792_dev_config* cfg = (const struct bq25792_dev_config*)dev->config;

    // REG0F EN_ICO (bit 4): POR 0 = ICO disabled (datasheet SLUSDG1C Table
    // 9-25, p.66). With ICO on, the effective input limit is REG19's result.
    LOG_INF("Setting EN_ICO to %u", enable ? 1 : 0);

    XactGuard guard(dev);
    return set_field_verified<BQ25792_CHARGER_CONTROL_0, BQ25792_CHARGER_CONTROL_0_EN_ICO>(
        cfg, enable ? 1 : 0);
}

int bq25792_register_irq_callback(const struct device* dev, bq25792_irq_callback_t cb,
                                  void* user_data) {
    if (!dev) {
        return -ENODEV;
    }

    struct bq25792_dev_data* data = (struct bq25792_dev_data*)dev->data;
    data->irq_cb = cb;
    data->irq_cb_user_data = user_data;
    return 0;
}

void bq25792_irq(const struct device* port, struct gpio_callback* cb, gpio_port_pins_t pins) {
    struct bq25792_dev_data* data = CONTAINER_OF(cb, struct bq25792_dev_data, callback);

    LOG_DBG("BQ25792 interrupt received!");

    if (data->irq_cb) {
        data->irq_cb(data->dev, data->irq_cb_user_data);
    }
}