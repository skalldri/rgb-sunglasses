#pragma once

#include <cstddef>

#include <singleton.h>
#include <configuration_provider.h>

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
};