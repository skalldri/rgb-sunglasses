#pragma once

/*
 * Test stub shadowing fw/src/core_config.h by include-path precedence (the
 * CMakeLists adds this dir with BEFORE). The real CoreConfig is backed by
 * BtGattPersistentCharacteristic members and drags in the whole BT GATT
 * stack, which this suite does not build. led_controller.cpp only needs
 * CoreConfig::getInstance() as the lazy-fallback ConfigurationProvider when
 * no provider was injected.
 */

#include <configuration_provider.h>

class CoreConfig : public ConfigurationProvider {
   public:
    static CoreConfig &getInstance() {
        static CoreConfig instance;
        return instance;
    }

    float getBrightnessFactor() override {
        return 1.0f;
    }

    /* 10 ms = one native_sim tick per display update, so tests only need to
     * sleep a few tens of ms per condition. */
    float getDisplayRateMs() override {
        return 10.0f;
    }

    float getRenderRateMs() override {
        return 10.0f;
    }
};
