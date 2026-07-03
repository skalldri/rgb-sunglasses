#include <animations/animation_imu_source.h>
#include <imu/imu.h>

#if defined(CONFIG_ANIMATION_TILT)
#include <animations/tilt_animation.h>
#endif

namespace {
/* Single shared instance: only one IMU animation is active at a time,
 * so a single drain-and-cache source serves all of them without double-reads. */
class ImuAnimationImuSource : public AnimationImuSource {
   public:
    /* Drain the message queue and cache the most recent frame.
     * Called once per animation tick so the animation sees a consistent snapshot. */
    void update() override {
        imu_analysis_result tmp;
        while (k_msgq_get(&imu_result_q, &tmp, K_NO_WAIT) == 0) {
            cache_ = tmp;
        }
    }

    float getAccelX() const override { return cache_.accel_x; }
    float getAccelY() const override { return cache_.accel_y; }
    float getAccelZ() const override { return cache_.accel_z; }
    float getGyroX() const override { return cache_.gyro_x; }
    float getGyroY() const override { return cache_.gyro_y; }
    float getGyroZ() const override { return cache_.gyro_z; }

   private:
    imu_analysis_result cache_ = {};
};

ImuAnimationImuSource sImuSource;
}  // namespace

#if defined(CONFIG_ANIMATION_TILT)
void tilt_animation_bind_default_imu_dependencies() {
    TiltAnimation::getInstance()->setImuSource(&sImuSource);
}
#endif

#if defined(CONFIG_APP_EXTENSION_HOST)
#include <extensions/extension_host.h>
/* Sandboxed extensions receive the same drain-and-cache IMU snapshot via
 * rgbx_inputs (the host copies the getters into the extension's input block
 * each tick — extensions never touch this source directly). */
void extension_host_bind_default_imu_dependencies() {
    extension_host::set_imu_source(&sImuSource);
}
#endif
