#include <animations/animation_imu_source.h>
#include <animations/animation_renderer.h>
#include <animations/tilt_animation.h>
#include <zephyr/ztest.h>

namespace {

/* Gyro-driven mock IMU source. Accel is unused by the gyro-based Tilt animation
 * but the interface still requires the accessors, so they return zero. */
class MutableImuSource : public AnimationImuSource {
   public:
    void update() override {}

    float getAccelX() const override { return 0.0f; }
    float getAccelY() const override { return 0.0f; }
    float getAccelZ() const override { return 0.0f; }
    float getGyroX() const override { return gyro_x_; }
    float getGyroY() const override { return gyro_y_; }
    float getGyroZ() const override { return gyro_z_; }

    void setGyro(float x, float y, float z) {
        gyro_x_ = x;
        gyro_y_ = y;
        gyro_z_ = z;
    }

   private:
    float gyro_x_ = 0.0f;
    float gyro_y_ = 0.0f;
    float gyro_z_ = 0.0f;
};

static constexpr size_t kTestWidth = 40;
static constexpr size_t kTestHeight = 12;
static constexpr size_t kTickMs = 16;

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

void copyCapture(PixelColor dst[kTestWidth][kTestHeight]) {
    for (size_t x = 0; x < kTestWidth; x++)
        for (size_t y = 0; y < kTestHeight; y++)
            dst[x][y] = sPixels[x][y];
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

/* Reset the animation, apply a constant gyro rate, tick `ticks` times, and leave
 * the final frame in sPixels. */
void renderMotion(TiltAnimation *anim, MutableImuSource &imu, float gx, float gy, float gz,
                  size_t ticks) {
    imu.setGyro(gx, gy, gz);
    anim->setImuSource(&imu);
    anim->init();

    CapturingTestRenderer renderer;
    resetCapture();
    for (size_t i = 0; i < ticks; i++) {
        anim->tick(renderer, kTickMs);
    }
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
    anim->tick(renderer, kTickMs);

    zassert_true(allPixelsDark(), "Display should be all black with no IMU source injected");
}

/* Stationary (all gyro rates zero) still fills the display with rainbow colors. */
ZTEST(tilt_animation_di_tests, test_stationary_renders_rainbow) {
    MutableImuSource imu;
    TiltAnimation *anim = TiltAnimation::getInstance();

    renderMotion(anim, imu, 0.0f, 0.0f, 0.0f, /*ticks=*/1);

    zassert_true(allPixelsNonBlack(), "Stationary Tilt should produce a fully-lit rainbow display");
}

/* Yaw rate (gyro about +X) scrolls the rainbow — output differs from stationary. */
ZTEST(tilt_animation_di_tests, test_yaw_scrolls_output) {
    MutableImuSource imu;
    TiltAnimation *anim = TiltAnimation::getInstance();

    renderMotion(anim, imu, 0.0f, 0.0f, 0.0f, 1);
    PixelColor stationary[kTestWidth][kTestHeight];
    copyCapture(stationary);

    renderMotion(anim, imu, 5.0f, 0.0f, 0.0f, 1);

    zassert_true(framesAreDifferent(stationary), "Yaw motion should scroll the rainbow");
}

/* Pitch rate (gyro about +Y) scrolls the rainbow — output differs from stationary. */
ZTEST(tilt_animation_di_tests, test_pitch_scrolls_output) {
    MutableImuSource imu;
    TiltAnimation *anim = TiltAnimation::getInstance();

    renderMotion(anim, imu, 0.0f, 0.0f, 0.0f, 1);
    PixelColor stationary[kTestWidth][kTestHeight];
    copyCapture(stationary);

    renderMotion(anim, imu, 0.0f, 5.0f, 0.0f, 1);

    zassert_true(framesAreDifferent(stationary), "Pitch motion should scroll the rainbow");
}

/* Roll rate (gyro about +Z) rotates the rainbow axis — output differs from stationary. */
ZTEST(tilt_animation_di_tests, test_roll_rotates_output) {
    MutableImuSource imu;
    TiltAnimation *anim = TiltAnimation::getInstance();

    renderMotion(anim, imu, 0.0f, 0.0f, 0.0f, 1);
    PixelColor stationary[kTestWidth][kTestHeight];
    copyCapture(stationary);

    /* A few ticks of roll to accumulate a clear rotation. */
    renderMotion(anim, imu, 0.0f, 0.0f, 5.0f, 3);

    zassert_true(framesAreDifferent(stationary), "Roll motion should rotate the rainbow axis");
}

/* Large gyro rates must not crash or produce black pixels (fmodf wrap handles it). */
ZTEST(tilt_animation_di_tests, test_large_gyro_stays_lit) {
    MutableImuSource imu;
    TiltAnimation *anim = TiltAnimation::getInstance();

    renderMotion(anim, imu, 1000.0f, -1000.0f, 1000.0f, 5);

    zassert_true(allPixelsNonBlack(), "Extreme gyro rates should stay lit, not produce black");
}
