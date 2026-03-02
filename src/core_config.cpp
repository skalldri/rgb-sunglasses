#include <core_config.h>

#include <bluetooth/bt_service_cpp.h>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(core_config, LOG_LEVEL_INF);

constexpr bt_uuid_128 kCoreConfigServiceUuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, CoreConfig::kBtServiceIdNum, 0x56789abc0000));

BtGattPrimaryService<kCoreConfigServiceUuid> coreConfigPrimaryService;
BtGattAutoReadWriteNotifyCharacteristic<"Brightness (0-1000)", uint32_t, 20> coreBrightness;
BtGattAutoReadWriteNotifyCharacteristic<"Display Thread Rate * 1000 (ms)", uint32_t, 33300> coreDisplayThreadRateMs;
BtGattAutoReadWriteNotifyCharacteristic<"Render Thread Rate * 1000 (ms)", uint32_t, 11100> coreRenderThreadRateMs;

BtGattServer coreConfigServer(
    coreConfigPrimaryService,
    coreBrightness,
    coreDisplayThreadRateMs,
    coreRenderThreadRateMs);
BT_GATT_SERVER_REGISTER(coreConfigServerStatic, coreConfigServer);

float CoreConfig::getBrightnessFactor()
{
    uint32_t brightnessUint = coreBrightness;

    // Clamp to sane values
    if (brightnessUint > 1000)
    {
        brightnessUint = 1000;
        coreBrightness = 1000; // Clamp the BT variable as well
    }

    return ((float)brightnessUint) / 1000.0f;
}

float CoreConfig::getDisplayRateMs()
{
    uint32_t displayRateUint = coreDisplayThreadRateMs;
    return ((float)displayRateUint) / 1000.0f;
}

float CoreConfig::getRenderRateMs()
{
    uint32_t renderRateUint = coreRenderThreadRateMs;
    return ((float)renderRateUint) / 1000.0f;
}
