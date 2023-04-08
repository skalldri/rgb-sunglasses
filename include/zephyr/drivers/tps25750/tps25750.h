#pragma once

#ifdef __cplusplus
extern "C" {
#endif

int tps25750_dump(const struct device *dev);

int tps25750_download_patch(const struct device *dev);

#ifdef __cplusplus
};
#endif