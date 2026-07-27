#include <animations/animation_is_active_binding.h>
#include <animations/animation_shuffle_include_binding.h>
#include <animations/rainbow_animation.h>
#include <bluetooth/animation_is_active_characteristic.h>
#include <bluetooth/animation_shuffle_include_characteristic.h>
#include <bluetooth/bt_service_cpp.h>
#include <bluetooth/persistent_characteristic.h>

constexpr bt_uuid_128 kRainbowConfigServiceUuid =
    BT_ANIMATION_SERVICE_UUID(static_cast<uint16_t>(Animation::Rainbow));

BtGattPrimaryService<kRainbowConfigServiceUuid> rainbowPrimaryService;
BtGattPersistentCharacteristic<"rainbow/step_time_ms", "Step Time Ms", false, uint32_t, 100>
    rainbowStepTimeMs;
BtGattPersistentCharacteristic<"rainbow/width_pixels", "Rainbow Width Pixels", false, uint32_t, 5>
    rainbowWidthPix;

using RainbowIsActiveCharacteristic = IsActiveCharacteristic<Animation::Rainbow>;
RainbowIsActiveCharacteristic rainbowIsActive;

constexpr BtGattString<24> kRainbowAnimationName = makeBtGattString<24>("Rainbow");
BtGattReadOnlyCharacteristic<kAnimationNameCharacteristicUuid, "Animation Name", BtGattString<24>,
                             kRainbowAnimationName>
    rainbowAnimationName;

// APPEND-ONLY: new providers go after every existing one (fixed UUID, so auto-UUID
// positions don't shift either way — but bonded phones cache handles per table).
ShuffleIncludeCharacteristic<"rainbow/shuffle"> rainbowShuffleInclude;

BtGattServer rainbowConfigServer(rainbowPrimaryService, rainbowStepTimeMs, rainbowWidthPix,
                                 rainbowIsActive, rainbowAnimationName, rainbowShuffleInclude);
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

static bool rainbow_shuffle_included() {
    return rainbowShuffleInclude.value();
}

struct RainbowIsActiveBindingRegistrar {
    RainbowIsActiveBindingRegistrar() {
        RainbowAnimationIsActive::registerSetter(rainbow_set_is_active);
        AnimationShuffleIncludeBinding<Animation::Rainbow>::registerGetter(
            rainbow_shuffle_included);
    }
};

[[maybe_unused]] RainbowIsActiveBindingRegistrar sRainbowIsActiveBindingRegistrar;

void rainbow_animation_bind_default_dependencies() {
    RainbowAnimation::getInstance()->setDependencies(sDefaultDeps);
}
