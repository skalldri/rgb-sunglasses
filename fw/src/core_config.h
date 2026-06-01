#pragma once

#include <configuration_provider.h>
#include <singleton.h>

#include <cstddef>

class CoreConfig : public Singleton<CoreConfig>, public ConfigurationProvider {
   public:
    static constexpr size_t kServiceIdNum = 1;

    /**
     * @brief Returns a value between 0 and 1 representing the current display brightness
     *
     * @return float
     */
    float getBrightnessFactor() override;

    float getDisplayRateMs() override;

    float getRenderRateMs() override;

    /**
     * @brief Returns a value between 0 and 1 representing the status LED brightness.
     */
    float getStatusLedBrightnessFactor();
};