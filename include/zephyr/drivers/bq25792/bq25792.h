#pragma once

#ifdef __cplusplus
extern "C" {
#endif

int bq25792_dump(const struct device *dev);

int bq25792_temp_override(const struct device *dev, bool enable);

int bq25792_adc_enable(const struct device *dev, bool enable);

int bq25792_pfm_enable(const struct device *dev, bool enable);

typedef enum {
    HIGH = 0,
    LOW = 1,

    NUM_CHARGE_FREQUENCY
} bq25792_charge_frequency_t;

int bq25792_set_charge_frequency(const struct device *dev, bq25792_charge_frequency_t freq);

#ifdef __cplusplus
};
#endif