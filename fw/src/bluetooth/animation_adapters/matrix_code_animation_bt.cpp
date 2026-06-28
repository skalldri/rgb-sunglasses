#include <animations/animation_is_active_binding.h>
#include <animations/matrix_code_animation.h>
#include <bluetooth/animation_is_active_characteristic.h>
#include <bluetooth/bt_service_cpp.h>
#include <bluetooth/persistent_characteristic.h>

constexpr bt_uuid_128 kMatrixCodeConfigServiceUuid =
    BT_ANIMATION_SERVICE_UUID(static_cast<uint16_t>(Animation::MatrixCode));

BtGattPrimaryService<kMatrixCodeConfigServiceUuid> matrixCodePrimaryService;

// ms per row step — lower = faster falling drops
BtGattPersistentCharacteristic<"matrix/drop_speed_ms", "Drop Speed Ms", false, uint32_t, 80>
    matrixCodeDropSpeedMs;

// ms for a pixel to fully fade from full brightness to dark
BtGattPersistentCharacteristic<"matrix/fade_time_ms", "Fade Time Ms", false, uint32_t, 600>
    matrixCodeFadeTimeMs;

// 0–100: probability per tick that an idle column spawns a new drop
BtGattPersistentCharacteristic<"matrix/density", "Density", false, uint32_t, 40>
    matrixCodeDensity;

// Drop colour; default is classic phosphor green (#00FF41)
BtGattPersistentCharacteristic<"matrix/color", "Color", false, BtGattColor,
                               BtGattColor{0x0000FF41}>
    matrixCodeColor;

using MatrixCodeIsActiveCharacteristic = IsActiveCharacteristic<Animation::MatrixCode>;
MatrixCodeIsActiveCharacteristic matrixCodeIsActive;

constexpr BtGattString<24> kMatrixCodeAnimationName = makeBtGattString<24>("Matrix Code");
BtGattReadOnlyCharacteristic<kAnimationNameCharacteristicUuid, "Animation Name", BtGattString<24>,
                             kMatrixCodeAnimationName>
    matrixCodeAnimationName;

BtGattServer matrixCodeConfigServer(matrixCodePrimaryService, matrixCodeDropSpeedMs,
                                    matrixCodeFadeTimeMs, matrixCodeDensity, matrixCodeColor,
                                    matrixCodeIsActive, matrixCodeAnimationName);
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
MatrixCodeAnimationDependencies sDefaultDeps(sDefaultDropSpeedSource, sDefaultFadeTimeSource,
                                             sDefaultDensitySource, sDefaultColorSource);
}  // namespace

using MatrixCodeAnimationIsActive = AnimationIsActiveBinding<Animation::MatrixCode>;

static void matrix_code_set_is_active(bool active) {
    matrixCodeIsActive.setActive(active);
}

struct MatrixCodeIsActiveBindingRegistrar {
    MatrixCodeIsActiveBindingRegistrar() {
        MatrixCodeAnimationIsActive::registerSetter(matrix_code_set_is_active);
    }
};

[[maybe_unused]] MatrixCodeIsActiveBindingRegistrar sMatrixCodeIsActiveBindingRegistrar;

void matrix_code_animation_bind_default_dependencies() {
    MatrixCodeAnimation::getInstance()->setDependencies(sDefaultDeps);
}
