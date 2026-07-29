#include <animations/animation_is_active_binding.h>
#include <animations/animation_shuffle_include_binding.h>
#include <animations/beat_animation.h>
#include <animations/color_mode_source.h>
#include <zephyr/random/random.h>
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
// Resolves the color value's mode byte (issue #259) so the animation always sees
// an effective 0x00RRGGBB through the same interface. RandomOnBeat shares the
// drain-time beat latch with the Beat animation's own isBeat() flash logic (see
// audio_animations_sound.cpp), so neither steals the other's beats.
ColorModeSource sBeatColorMode(sDefaultColorSource, sys_rand32_get, k_uptime_get);
}  // namespace

using BeatAnimationIsActive = AnimationIsActiveBinding<Animation::Beat>;

static void beat_set_is_active(bool active) {
    if (active) {
        // Fires for every activation source (BLE write, shell, boot restore,
        // shuffle) — arms the RandomOnActivate re-roll / mode-state reset.
        sBeatColorMode.notifyActivated();
    }
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
    BeatAnimation::getInstance()->setColor(sBeatColorMode);
}
