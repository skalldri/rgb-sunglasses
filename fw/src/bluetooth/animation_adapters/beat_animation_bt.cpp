#include <animations/animation_is_active_binding.h>
#include <animations/beat_animation.h>
#include <bluetooth/animation_is_active_characteristic.h>
#include <bluetooth/bt_service_cpp.h>

constexpr bt_uuid_128 kBeatConfigServiceUuid =
    BT_ANIMATION_SERVICE_UUID(static_cast<uint16_t>(Animation::Beat));

BtGattPrimaryService<kBeatConfigServiceUuid> beatPrimaryService;
BtGattAutoReadWriteCharacteristic<"Color", BtGattColor, BtGattColor{0xFFFFFFFF}> beatColor;

using BeatIsActiveCharacteristic = IsActiveCharacteristic<Animation::Beat>;
BeatIsActiveCharacteristic beatIsActive;

constexpr BtGattString<24> kBeatAnimationName = makeBtGattString<24>("Beat");
BtGattReadOnlyCharacteristic<kAnimationNameCharacteristicUuid, "Animation Name", BtGattString<24>,
                             kBeatAnimationName>
    beatAnimationName;

BtGattServer beatConfigServer(beatPrimaryService, beatColor, beatIsActive, beatAnimationName);
BT_GATT_SERVER_REGISTER(beatConfigServerStatic, beatConfigServer);

namespace {
class BeatColorSource : public AnimationUint32ParameterSource {
   public:
    uint32_t get() const override { return static_cast<BtGattColor>(beatColor); }
};

BeatColorSource sDefaultColorSource;
}  // namespace

using BeatAnimationIsActive = AnimationIsActiveBinding<Animation::Beat>;

static void beat_set_is_active(bool active) {
    beatIsActive.setActive(active);
}

struct BeatIsActiveBindingRegistrar {
    BeatIsActiveBindingRegistrar() { BeatAnimationIsActive::registerSetter(beat_set_is_active); }
};

[[maybe_unused]] BeatIsActiveBindingRegistrar sBeatIsActiveBindingRegistrar;

void beat_animation_bind_default_bt_dependencies() {
    BeatAnimation::getInstance()->setColor(sDefaultColorSource);
}
