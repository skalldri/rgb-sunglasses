#pragma once

/**
 * @brief Abstract source of device-level display and render configuration.
 *
 * Implementations may read from BLE GATT characteristics (CoreConfig),
 * hard-coded defaults, or test doubles.
 */
class ConfigurationProvider
{
public:
    virtual ~ConfigurationProvider() = default;

    /** @return Brightness multiplier in [0, 1]. */
    virtual float getBrightnessFactor() = 0;

    /** @return Target display thread interval in milliseconds. */
    virtual float getDisplayRateMs() = 0;

    /** @return Target render thread interval in milliseconds. */
    virtual float getRenderRateMs() = 0;
};
