#include <core_config.h>

#include <bluetooth/read_write_variable.h>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(core_config, LOG_LEVEL_INF);

BT_SVC_UUID_DEFINE(CoreConfig);

using Brightness = BT_SVC_READ_WRITE_VAR_CHRC_DEFINE(CoreConfig, 0, uint32_t, 20);
using DisplayThreadRateMs = BT_SVC_READ_WRITE_VAR_CHRC_DEFINE(CoreConfig, 1, uint32_t, 33300);
using RenderThreadRateMs = BT_SVC_READ_WRITE_VAR_CHRC_DEFINE(CoreConfig, 2, uint32_t, 11100);

BT_GATT_SERVICE_DEFINE(core_config_service,
    BT_SVC_UUID_REFERENCE(CoreConfig),
    BT_SVC_READ_WRITE_VAR_CHRC_REFERENCE(CoreConfig, 0, "Brightness (0-1000)"),
    BT_SVC_READ_WRITE_VAR_CHRC_REFERENCE(CoreConfig, 1, "Display Thread Rate * 1000 (ms)"),
    BT_SVC_READ_WRITE_VAR_CHRC_REFERENCE(CoreConfig, 2, "Render Thread Rate * 1000 (ms)"),
);

float CoreConfig::getBrightnessFactor() {
    uint32_t brightnessUint = Brightness::getInstance();

    // Clamp to sane values
    if (brightnessUint > 1000) {
        brightnessUint = 1000;
        Brightness::getInstance() = 1000; // Clamp the BT variable as well
    }

    return ((float)brightnessUint) / 1000.0f;
}

float CoreConfig::getDisplayRateMs() {
    uint32_t displayRateUint = DisplayThreadRateMs::getInstance();
    return ((float)displayRateUint) / 1000.0f;
}

float CoreConfig::getRenderRateMs() {
    uint32_t renderRateUint = RenderThreadRateMs::getInstance();
    return ((float)renderRateUint) / 1000.0f;
}

