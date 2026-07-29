#include <animations/animation_is_active_binding.h>
#include <animations/animation_shuffle_include_binding.h>
#include <animations/color_mode_source.h>
#include <animations/zigzag_animation.h>
#include <zephyr/random/random.h>
#include <bluetooth/animation_is_active_characteristic.h>
#include <bluetooth/animation_shuffle_include_characteristic.h>
#include <bluetooth/bt_service_cpp.h>
#include <bluetooth/persistent_characteristic.h>

constexpr bt_uuid_128 kZigZagConfigServiceUuid =
    BT_ANIMATION_SERVICE_UUID(static_cast<uint16_t>(Animation::ZigZag));

BtGattPrimaryService<kZigZagConfigServiceUuid> zigzagPrimaryService;
BtGattPersistentCharacteristic<"zigzag/step_time_ms", "Step Time Ms", false, uint32_t, 200>
    zigzagStepTimeMs;
BtGattPersistentCharacteristic<"zigzag/color", "Color", false, BtGattColor,
                               BtGattColor{0xFFFFFFFF}>
    zigzagColor;

using ZigZagIsActiveCharacteristic = IsActiveCharacteristic<Animation::ZigZag>;
ZigZagIsActiveCharacteristic zigzagIsActive;

constexpr BtGattString<24> kZigZagAnimationName = makeBtGattString<24>("ZigZag");
BtGattReadOnlyCharacteristic<kAnimationNameCharacteristicUuid, "Animation Name", BtGattString<24>,
                             kZigZagAnimationName>
    zigzagAnimationName;

// APPEND-ONLY: new providers go after every existing one (fixed UUID, so auto-UUID
// positions don't shift either way — but bonded phones cache handles per table).
ShuffleIncludeCharacteristic<"zigzag/shuffle"> zigzagShuffleInclude;

BtGattServer zigzagConfigServer(zigzagPrimaryService, zigzagStepTimeMs, zigzagColor,
                                zigzagIsActive, zigzagAnimationName, zigzagShuffleInclude);
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
// Resolves the color value's mode byte (issue #259) so the animation always sees
// an effective 0x00RRGGBB through the same interface.
ColorModeSource sZigZagColorMode(sDefaultColorSource, sys_rand32_get, k_uptime_get);
ZigZagAnimationDependencies sDefaultDeps(sDefaultStepTimeSource, sZigZagColorMode);
}  // namespace

using ZigZagAnimationIsActive = AnimationIsActiveBinding<Animation::ZigZag>;

static void zigzag_set_is_active(bool active) {
    if (active) {
        // Fires for every activation source (BLE write, shell, boot restore,
        // shuffle) — arms the RandomOnActivate re-roll / mode-state reset.
        sZigZagColorMode.notifyActivated();
    }
    zigzagIsActive.setActive(active);
}

static bool zigzag_shuffle_included() {
    return zigzagShuffleInclude.value();
}

struct ZigZagIsActiveBindingRegistrar {
    ZigZagIsActiveBindingRegistrar() {
        ZigZagAnimationIsActive::registerSetter(zigzag_set_is_active);
        AnimationShuffleIncludeBinding<Animation::ZigZag>::registerGetter(zigzag_shuffle_included);
    }
};

[[maybe_unused]] ZigZagIsActiveBindingRegistrar sZigZagIsActiveBindingRegistrar;

void zigzag_animation_bind_default_dependencies() {
    ZigZagAnimation::getInstance()->setDependencies(sDefaultDeps);
}
