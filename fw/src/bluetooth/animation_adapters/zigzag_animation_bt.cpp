#include <animations/animation_is_active_binding.h>
#include <animations/zigzag_animation.h>
#include <bluetooth/animation_is_active_characteristic.h>
#include <bluetooth/bt_service_cpp.h>

constexpr bt_uuid_128 kZigZagConfigServiceUuid =
    BT_ANIMATION_SERVICE_UUID(static_cast<uint16_t>(Animation::ZigZag));

BtGattPrimaryService<kZigZagConfigServiceUuid> zigzagPrimaryService;
BtGattAutoReadWriteCharacteristic<"Step Time Ms", uint32_t, 200> zigzagStepTimeMs;
BtGattAutoReadWriteCharacteristic<"Color", BtGattColor, BtGattColor{0xFFFFFFFF}> zigzagColor;

using ZigZagIsActiveCharacteristic = IsActiveCharacteristic<Animation::ZigZag>;
ZigZagIsActiveCharacteristic zigzagIsActive;

constexpr BtGattString<24> kZigZagAnimationName = makeBtGattString<24>("ZigZag");
BtGattReadOnlyCharacteristic<kAnimationNameCharacteristicUuid, "Animation Name", BtGattString<24>,
                             kZigZagAnimationName>
    zigzagAnimationName;

BtGattServer zigzagConfigServer(zigzagPrimaryService, zigzagStepTimeMs, zigzagColor,
                                zigzagIsActive, zigzagAnimationName);
BT_GATT_SERVER_REGISTER(zigzagConfigServerStatic, zigzagConfigServer);

namespace {
class ZigZagStepTimeSource : public AnimationUint32ParameterSource {
   public:
    uint32_t get() const override { return zigzagStepTimeMs; }
};

class ZigZagColorSource : public AnimationUint32ParameterSource {
   public:
    uint32_t get() const override { return static_cast<BtGattColor>(zigzagColor); }
};

ZigZagStepTimeSource sDefaultStepTimeSource;
ZigZagColorSource sDefaultColorSource;
ZigZagAnimationDependencies sDefaultDeps(sDefaultStepTimeSource, sDefaultColorSource);
}  // namespace

using ZigZagAnimationIsActive = AnimationIsActiveBinding<Animation::ZigZag>;

static void zigzag_set_is_active(bool active) {
    zigzagIsActive.setActive(active);
}

struct ZigZagIsActiveBindingRegistrar {
    ZigZagIsActiveBindingRegistrar() {
        ZigZagAnimationIsActive::registerSetter(zigzag_set_is_active);
    }
};

[[maybe_unused]] ZigZagIsActiveBindingRegistrar sZigZagIsActiveBindingRegistrar;

void zigzag_animation_bind_default_dependencies() {
    ZigZagAnimation::getInstance()->setDependencies(sDefaultDeps);
}
