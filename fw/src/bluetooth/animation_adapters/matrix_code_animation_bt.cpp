#include <animations/animation_is_active_binding.h>
#include <animations/animation_shuffle_include_binding.h>
#include <animations/color_mode_source.h>
#include <animations/matrix_code_animation.h>
#include <zephyr/random/random.h>
#include <bluetooth/animation_is_active_characteristic.h>
#include <bluetooth/animation_shuffle_include_characteristic.h>
#include <bluetooth/bt_service_cpp.h>
#include <bluetooth/persistent_characteristic.h>

constexpr bt_uuid_128 kMatrixCodeConfigServiceUuid =
    BT_ANIMATION_SERVICE_UUID(static_cast<uint16_t>(Animation::MatrixCode));

BtGattPrimaryService<kMatrixCodeConfigServiceUuid> matrixCodePrimaryService;

// ms per row step — lower = faster falling drops
BtGattPersistentCharacteristic<"matrix_code/drop_speed_ms", "Drop Speed Ms", false, uint32_t, 80>
    matrixCodeDropSpeedMs;

// ms for a pixel to fully fade from full brightness to dark
BtGattPersistentCharacteristic<"matrix_code/fade_time_ms", "Fade Time Ms", false, uint32_t, 600>
    matrixCodeFadeTimeMs;

// 0–100: % chance per second that an idle column spawns a new drop (tick-rate-independent)
BtGattPersistentCharacteristic<"matrix_code/density", "Density", false, uint32_t, 40>
    matrixCodeDensity;

// Drop colour; default is classic phosphor green (#00FF41)
BtGattPersistentCharacteristic<"matrix_code/color", "Color", false, BtGattColor,
                               BtGattColor{0x0000FF41}>
    matrixCodeColor;

using MatrixCodeIsActiveCharacteristic = IsActiveCharacteristic<Animation::MatrixCode>;
MatrixCodeIsActiveCharacteristic matrixCodeIsActive;

constexpr BtGattString<24> kMatrixCodeAnimationName = makeBtGattString<24>("Matrix Code");
BtGattReadOnlyCharacteristic<kAnimationNameCharacteristicUuid, "Animation Name", BtGattString<24>,
                             kMatrixCodeAnimationName>
    matrixCodeAnimationName;

// APPEND-ONLY: new providers go after every existing one (fixed UUID, so auto-UUID
// positions don't shift either way — but bonded phones cache handles per table).
ShuffleIncludeCharacteristic<"matrix_code/shuffle"> matrixCodeShuffleInclude;

BtGattServer matrixCodeConfigServer(matrixCodePrimaryService, matrixCodeDropSpeedMs,
                                    matrixCodeFadeTimeMs, matrixCodeDensity, matrixCodeColor,
                                    matrixCodeIsActive, matrixCodeAnimationName,
                                    matrixCodeShuffleInclude);
BT_GATT_SERVER_REGISTER(matrixCodeConfigServerStatic, matrixCodeConfigServer);

namespace {
class MatrixCodeDropSpeedSource : public AnimationUint32ParameterSource {
   public:
    uint32_t get() const override { return matrixCodeDropSpeedMs; }
};

class MatrixCodeFadeTimeSource : public AnimationUint32ParameterSource {
   public:
    uint32_t get() const override { return matrixCodeFadeTimeMs; }
};

class MatrixCodeDensitySource : public AnimationUint32ParameterSource {
   public:
    uint32_t get() const override { return matrixCodeDensity; }
};

class MatrixCodeColorSource : public AnimationUint32ParameterSource {
   public:
    uint32_t get() const override { return static_cast<BtGattColor>(matrixCodeColor); }
};

MatrixCodeDropSpeedSource sDefaultDropSpeedSource;
MatrixCodeFadeTimeSource sDefaultFadeTimeSource;
MatrixCodeDensitySource sDefaultDensitySource;
MatrixCodeColorSource sDefaultColorSource;
// Resolves the color value's mode byte (issue #259) so the animation always sees
// an effective 0x00RRGGBB through the same interface.
ColorModeSource sMatrixCodeColorMode(sDefaultColorSource, sys_rand32_get, k_uptime_get);
MatrixCodeAnimationDependencies sDefaultDeps(sDefaultDropSpeedSource, sDefaultFadeTimeSource,
                                             sDefaultDensitySource, sMatrixCodeColorMode);
}  // namespace

using MatrixCodeAnimationIsActive = AnimationIsActiveBinding<Animation::MatrixCode>;

static void matrix_code_set_is_active(bool active) {
    if (active) {
        // Fires for every activation source (BLE write, shell, boot restore,
        // shuffle) — arms the RandomOnActivate re-roll / mode-state reset.
        sMatrixCodeColorMode.notifyActivated();
    }
    matrixCodeIsActive.setActive(active);
}

static bool matrix_code_shuffle_included() {
    return matrixCodeShuffleInclude.value();
}

struct MatrixCodeIsActiveBindingRegistrar {
    MatrixCodeIsActiveBindingRegistrar() {
        MatrixCodeAnimationIsActive::registerSetter(matrix_code_set_is_active);
        AnimationShuffleIncludeBinding<Animation::MatrixCode>::registerGetter(
            matrix_code_shuffle_included);
    }
};

[[maybe_unused]] MatrixCodeIsActiveBindingRegistrar sMatrixCodeIsActiveBindingRegistrar;

void matrix_code_animation_bind_default_dependencies() {
    MatrixCodeAnimation::getInstance()->setDependencies(sDefaultDeps);
}
