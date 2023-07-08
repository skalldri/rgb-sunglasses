#pragma once

#ifdef __cplusplus
extern "C" {
#endif

int vm3011_dump(const struct device *dev);

int vm3011_clear_dout(const struct device *dev);

#ifdef __cplusplus
};
#endif