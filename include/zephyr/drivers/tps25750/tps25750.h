#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

    int tps25750_dump(const struct device *dev);

    int tps25750_download_patch(const struct device *dev, const char *patch, uint32_t patchSize);

    int tps25750_clear_dead_battery(const struct device *dev);

#if defined(CONFIG_TPS25750_INTERNAL_PATCH)
    int tps25750_get_patch(const char **patch, size_t *patch_size);
#endif

#ifdef __cplusplus
};
#endif