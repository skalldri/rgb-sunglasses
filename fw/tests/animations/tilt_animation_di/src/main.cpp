#include <animations/animation_imu_source.h>
#include <animations/animation_renderer.h>
#include <animations/tilt_animation.h>
#include <zephyr/ztest.h>

namespace {

class MutableImuSource : public AnimationImuSource {
   public:
    void update() override {}

    float getAccelX() const override { return accel_x_; }
    float getAccelY() const override { return accel_y_; }
    float getAccelZ() const override { return accel_z_; }
    float getGyroX() const override { return 0.0f; }
    float getGyroY() const override { return 0.0f; }
    float getGyroZ() const override { return 0.0f; }

    void setAccel(float x, float y, float z) {
        accel_x_ = x;
        accel_y_ = y;
        accel_z_ = z;
    }

   private:
    float accel_x_ = 0.0f;
    float accel_y_ = 0.0f;
    float accel_z_ = -9.81f;
};

static constexpr size_t kTestWidth = 40;
static constexpr size_t kTestHeight = 12;

struct PixelColor {
    uint8_t r = 0, g = 0, b = 0;
    bool isBlack() const { return r == 0 && g == 0 && b == 0; }
};

PixelColor sPixels[kTestWidth][kTestHeight];

class CapturingTestRenderer : public AnimationRenderer {
   public:
    size_t displayWidth() const override { return kTestWidth; }
    size_t displayHeight() const override { return kTestHeight; }
    void setPixel(size_t x, size_t y, uint8_t r, uint8_t g, uint8_t b) override {
        if (x < kTestWidth && y < kTestHeight) {
            sPixels[x][y] = {r, g, b};
        }
    }
};

void resetCapture() {
    for (size_t x = 0; x < kTestWidth; x++) {
        for (size_t y = 0; y < kTestHeight; y++) {
            sPixels[x][y] = {};
        }
    }
}

bool allPixelsDark() {
    for (size_t x = 0; x < kTestWidth; x++) {
        for (size_t y = 0; y < kTestHeight; y++) {
            if (!sPixels[x][y].isBlack())
                return false;
        }
    }
    return true;
}

bool allPixelsNonBlack() {
    for (size_t x = 0; x < kTestWidth; x++) {
        for (size_t y = 0; y < kTestHeight; y++) {
            if (sPixels[x][y].isBlack())
                return false;
        }
    }
    return true;
}

/* Compare two captured frames — returns true if they differ in at least one pixel. */
bool framesAreDifferent(PixelColor other[kTestWidth][kTestHeight]) {
    for (size_t x = 0; x < kTestWidth; x++) {
        for (size_t y = 0; y < kTestHeight; y++) {
            if (sPixels[x][y].r != other[x][y].r || sPixels[x][y].g != other[x][y].g ||
                sPixels[x][y].b != other[x][y].b) {
                return true;
            }
        }
    }
    return false;
}
}  // namespace

ZTEST_SUITE(tilt_animation_di_tests, NULL, NULL, NULL, NULL, NULL);

/* Without an IMU source the display is rendered all black. */
ZTEST(tilt_animation_di_tests, test_no_source_renders_black) {
    TiltAnimation *anim = TiltAnimation::getInstance();
    anim->setImuSource(nullptr);  /* explicitly clear any source left by a previous test */
    anim->init();

    CapturingTestRenderer renderer;
    resetCapture();
    anim->tick(renderer, 16);

    zassert_true(allPixelsDark(), "Display should be all black with no IMU source injected");
}

/* With a flat IMU (accel_x=0, accel_z=-9.81) the display fills with rainbow colors. */
ZTEST(tilt_animation_di_tests, test_flat_renders_rainbow) {
    MutableImuSource imu;
    imu.setAccel(0.0f, 0.0f, -9.81f);  /* flat / level */

    TiltAnimation *anim = TiltAnimation::getInstance();
    anim->setImuSource(&imu);
    anim->init();

    CapturingTestRenderer renderer;
    resetCapture();
    anim->tick(renderer, 16);

    zassert_true(allPixelsNonBlack(), "Flat tilt should produce a fully-lit rainbow display");
}

/* Tilting left (accel_x < 0) vs right (accel_x > 0) shifts the displayed hues. */
ZTEST(tilt_animation_di_tests, test_tilt_changes_output) {
    MutableImuSource imu;
    TiltAnimation *anim = TiltAnimation::getInstance();
    anim->setImuSource(&imu);
    anim->init();

    CapturingTestRenderer renderer;

    /* Capture left-tilt frame. */
    imu.setAccel(-9.81f, 0.0f, 0.0f);
    resetCapture();
    anim->tick(renderer, 16);

    PixelColor left[kTestWidth][kTestHeight];
    for (size_t x = 0; x < kTestWidth; x++)
        for (size_t y = 0; y < kTestHeight; y++)
            left[x][y] = sPixels[x][y];

    /* Capture right-tilt frame. */
    imu.setAccel(+9.81f, 0.0f, 0.0f);
    resetCapture();
    anim->tick(renderer, 16);

    zassert_true(framesAreDifferent(left),
                 "Left-tilt and right-tilt should produce different pixel colors");
}

/* Clamping: accel_x far outside ±1g must not crash or produce black pixels. */
ZTEST(tilt_animation_di_tests, test_extreme_accel_clamped) {
    MutableImuSource imu;
    imu.setAccel(100.0f, 0.0f, 0.0f);  /* way beyond ±9.81 */

    TiltAnimation *anim = TiltAnimation::getInstance();
    anim->setImuSource(&imu);
    anim->init();

    CapturingTestRenderer renderer;
    resetCapture();
    anim->tick(renderer, 16);

    zassert_true(allPixelsNonBlack(),
                 "Extreme accel should be clamped, not produce a black display");
}
