#include <animations/animation_is_active_binding.h>
#include <animations/rainbow_animation.h>
#include <bluetooth/animation_is_active_characteristic.h>
#include <bluetooth/bt_service_cpp.h>

constexpr bt_uuid_128 kRainbowConfigServiceUuid =
    BT_ANIMATION_SERVICE_UUID(static_cast<uint16_t>(Animation::Rainbow));

BtGattPrimaryService<kRainbowConfigServiceUuid> rainbowPrimaryService;
BtGattAutoReadWriteCharacteristic<"Step Time Ms", uint32_t, 100> rainbowStepTimeMs;
BtGattAutoReadWriteCharacteristic<"Rainbow Width Pixels", uint32_t, 5> rainbowWidthPix;

using RainbowIsActiveCharacteristic = IsActiveCharacteristic<Animation::Rainbow>;
RainbowIsActiveCharacteristic rainbowIsActive;

BtGattServer rainbowConfigServer(rainbowPrimaryService, rainbowStepTimeMs, rainbowWidthPix,
                                 rainbowIsActive);
BT_GATT_SERVER_REGISTER(rainbowConfigServerStatic, rainbowConfigServer);

namespace {
class RainbowStepTimeSource : public AnimationUint32ParameterSource {
   public:
    uint32_t get() const override { return rainbowStepTimeMs; }
};

class RainbowWidthSource : public AnimationUint32ParameterSource {
   public:
    uint32_t get() const override { return rainbowWidthPix; }
};

RainbowStepTimeSource sDefaultStepTimeSource;
RainbowWidthSource sDefaultWidthSource;
RainbowAnimationDependencies sDefaultDeps(sDefaultStepTimeSource, sDefaultWidthSource);
}  // namespace

using RainbowAnimationIsActive = AnimationIsActiveBinding<Animation::Rainbow>;

static void rainbow_set_is_active(bool active) {
    rainbowIsActive.setActive(active);
}

struct RainbowIsActiveBindingRegistrar {
    RainbowIsActiveBindingRegistrar() {
        RainbowAnimationIsActive::registerSetter(rainbow_set_is_active);
    }
};

[[maybe_unused]] RainbowIsActiveBindingRegistrar sRainbowIsActiveBindingRegistrar;

void rainbow_animation_bind_default_dependencies() {
    RainbowAnimation::getInstance()->setDependencies(sDefaultDeps);
}
