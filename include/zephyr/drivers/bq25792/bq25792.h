#pragma once

#ifdef __cplusplus
extern "C" {
#endif

int bq25792_dump(const struct device *dev);

int bq25792_temp_override(const struct device *dev, bool enable);

#ifdef __cplusplus
};
#endif