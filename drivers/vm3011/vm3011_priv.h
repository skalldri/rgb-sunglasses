#pragma once

#include <zephyr/drivers/InternalDeviceRegister.hpp>
#include "vm3011_init.h"

/**
 * @brief
 *
 * @tparam reg
 * @tparam registerWidth
 * @tparam U
 * @tparam Us
 */
template <const char *name, uint8_t reg, size_t registerWidth, typename U, typename... Us>
class VM3011Register : public I2CDeviceRegister<name, reg, registerWidth, U, Us...>
{
    using Base = I2CDeviceRegister<name, reg, registerWidth, U, Us...>;

public:
    VM3011Register(const struct vm3011_dev_config *cfg) : Base(&(cfg->i2c)) {}
};

/**
 * Register 0x00 - I2C_CNTRL
 *
 */
#define VM3011_REG_I2C_CNTRL_ADDR 0x00
static const char I2C_CNTRL_NAME[] = "I2C_CNTRL";

static const char DOUT_CLEAR_NAME[] = "DOUT_CLEAR";
using VM3011_I2C_CNTRL_DOUT_CLEAR = RegisterField<DOUT_CLEAR_NAME, 4, 1>;

static const char WDT_DLY_NAME[] = "WDT_DLY";
using VM3011_I2C_CNTRL_WDT_DLY = RegisterField<WDT_DLY_NAME, 1, 2>;

static const char WDT_ENABLE_NAME[] = "WDT_ENABLE";
using VM3011_I2C_CNTRL_WDT_ENABLE = RegisterField<WDT_ENABLE_NAME, 0, 1>;

using VM3011_I2C_CNTRL =
    VM3011Register<
        I2C_CNTRL_NAME,
        VM3011_REG_I2C_CNTRL_ADDR,
        8,
        VM3011_I2C_CNTRL_WDT_ENABLE,
        VM3011_I2C_CNTRL_WDT_DLY,
        VM3011_I2C_CNTRL_DOUT_CLEAR>;

/**
 * Register 0x01 - WOS_PGA_GAIN
 *
 */
#define VM3011_REG_WOS_PGA_GAIN_ADDR 0x01
static const char WOS_PGA_GAIN_NAME[] = "WOS_PGA_GAIN";

using VM3011_WOS_PGA_GAIN_WOS_PGA_GAIN = RegisterField<WOS_PGA_GAIN_NAME, 0, 5>;

using VM3011_WOS_PGA_GAIN =
    VM3011Register<
        WOS_PGA_GAIN_NAME,
        VM3011_REG_WOS_PGA_GAIN_ADDR,
        8,
        VM3011_WOS_PGA_GAIN_WOS_PGA_GAIN>;

/**
 * Register 0x02 - WOS_FILTER
 *
 */
#define VM3011_REG_WOS_FILTER_ADDR 0x02
static const char WOS_FILTER_NAME[] = "WOS_FILTER";

static const char WOS_LPF_NAME[] = "WOS_LPF";
using VM3011_WOS_FILTER_WOS_LPF = RegisterField<WOS_LPF_NAME, 0, 2>;

static const char WOS_HPF_NAME[] = "WOS_HPF";
using VM3011_WOS_FILTER_WOS_HPF = RegisterField<WOS_HPF_NAME, 2, 2>;

using VM3011_WOS_FILTER =
    VM3011Register<
        WOS_FILTER_NAME,
        VM3011_REG_WOS_FILTER_ADDR,
        8,
        VM3011_WOS_FILTER_WOS_LPF,
        VM3011_WOS_FILTER_WOS_HPF>;

/**
 * Register 0x03 - WOS_PGA_MIN_THR
 *
 */
#define VM3011_REG_WOS_PGA_MIN_THR_ADDR 0x03
static const char WOS_PGA_MIN_THR_NAME[] = "WOS_PGA_MIN_THR";

using VM3011_WOS_PGA_MIN_THR_WOS_PGA_MIN_THR = RegisterField<WOS_PGA_MIN_THR_NAME, 0, 5>;

static const char FAST_MODE_CNT_NAME[] = "FAST_MODE_CNT";
using VM3011_WOS_PGA_MIN_THR_FAST_MODE_CNT = RegisterField<FAST_MODE_CNT_NAME, 5, 2>;

using VM3011_WOS_PGA_MIN_THR =
    VM3011Register<
        WOS_PGA_MIN_THR_NAME,
        VM3011_REG_WOS_PGA_MIN_THR_ADDR,
        8,
        VM3011_WOS_PGA_MIN_THR_WOS_PGA_MIN_THR,
        VM3011_WOS_PGA_MIN_THR_FAST_MODE_CNT>;

/**
 * Register 0x04 - WOS_PGA_MAX_THR
 *
 */
#define VM3011_REG_WOS_PGA_MAX_THR_ADDR 0x04
static const char WOS_PGA_MAX_THR_NAME[] = "WOS_PGA_MAX_THR";

using VM3011_WOS_PGA_MAX_THR_WOS_PGA_MAX_THR = RegisterField<WOS_PGA_MAX_THR_NAME, 0, 5>;

static const char WOS_RMS_NAME[] = "WOS_RMS";
using VM3011_WOS_PGA_MAX_THR_WOS_RMS = RegisterField<WOS_RMS_NAME, 5, 1>;

using VM3011_WOS_PGA_MAX_THR =
    VM3011Register<
        WOS_PGA_MAX_THR_NAME,
        VM3011_REG_WOS_PGA_MAX_THR_ADDR,
        8,
        VM3011_WOS_PGA_MAX_THR_WOS_PGA_MAX_THR,
        VM3011_WOS_PGA_MAX_THR_WOS_RMS>;

/**
 * Register 0x05 - WOS_THRESH
 *
 */
#define VM3011_REG_WOS_THRESH_ADDR 0x05
static const char WOS_THRESH_NAME[] = "WOS_THRESH";

using VM3011_WOS_THRESH_WOS_THRESH = RegisterField<WOS_THRESH_NAME, 0, 3>;

using VM3011_WOS_THRESH =
    VM3011Register<
        WOS_THRESH_NAME,
        VM3011_REG_WOS_THRESH_ADDR,
        8,
        VM3011_WOS_THRESH_WOS_THRESH>;

#define REG_LIST \
    REG(VM3011_I2C_CNTRL) \
    REG(VM3011_WOS_PGA_GAIN) \
    REG(VM3011_WOS_FILTER) \
    REG(VM3011_WOS_PGA_MIN_THR) \
    REG(VM3011_WOS_PGA_MAX_THR) \
    REG(VM3011_WOS_THRESH)

