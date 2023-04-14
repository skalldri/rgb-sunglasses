#pragma once

#define BQ25792_REG_CHARGER_STATUS_0_ADDR 0x1B
#define BQ25792_REG_CHARGER_STATUS_1_ADDR 0x1C
#define BQ25792_REG_CHARGER_STATUS_2_ADDR 0x1D
#define BQ25792_REG_CHARGER_STATUS_3_ADDR 0x1E
#define BQ25792_REG_CHARGER_STATUS_4_ADDR 0x1F

struct bq25792_dev_data
{
};

struct bq25792_dev_config
{
    struct i2c_dt_spec i2c;
};