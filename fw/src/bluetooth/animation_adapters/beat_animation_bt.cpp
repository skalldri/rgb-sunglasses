#include <animations/animation_is_active_binding.h>
#include <animations/animation_shuffle_include_binding.h>
#include <animations/beat_animation.h>
#include <bluetooth/animation_is_active_characteristic.h>
#include <bluetooth/animation_shuffle_include_characteristic.h>
#include <bluetooth/bt_service_cpp.h>
#include <bluetooth/persistent_characteristic.h>

constexpr bt_uuid_128 kBeatConfigServiceUuid =
    BT_ANIMATION_SERVICE_UUID(static_cast<uint16_t>(Animation::Beat));

BtGattPrimaryService<kBeatConfigServiceUuid> beatPrimaryService;
BtGattPersistentCharacteristic<"beat/color", "Color", false, BtGattColor, BtGattColor{0xFFFFFFFF}>
    beatColor;

using BeatIsActiveCharacteristic = IsActiveCharacteristic<Animation::Beat>;
BeatIsActiveCharacteristic beatIsActive;

constexpr BtGattString<24> kBeatAnimationName = makeBtGattString<24>("Beat");
BtGattReadOnlyCharacteristic<kAnimationNameCharacteristicUuid, "Animation Name", BtGattString<24>,
                             kBeatAnimationName>
    beatAnimationName;

// APPEND-ONLY: new providers go after every existing one (fixed UUID, so auto-UUID
// positions don't shift either way — but bonded phones cache handles per table).
ShuffleIncludeCharacteristic<"beat/shuffle"> beatShuffleInclude;

BtGattServer beatConfigServer(beatPrimaryService, beatColor, beatIsActive, beatAnimationName,
                              beatShuffleInclude);
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

static bool beat_shuffle_included() {
    return beatShuffleInclude.value();
}

struct BeatIsActiveBindingRegistrar {
    BeatIsActiveBindingRegistrar() {
        BeatAnimationIsActive::registerSetter(beat_set_is_active);
        AnimationShuffleIncludeBinding<Animation::Beat>::registerGetter(beat_shuffle_included);
    }
};

[[maybe_unused]] BeatIsActiveBindingRegistrar sBeatIsActiveBindingRegistrar;

void beat_animation_bind_default_bt_dependencies() {
    BeatAnimation::getInstance()->setColor(sDefaultColorSource);
}
