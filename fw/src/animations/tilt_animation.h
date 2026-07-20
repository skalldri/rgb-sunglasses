#pragma once

#include <animations/animation.h>
#include <animations/animation_imu_source.h>

class TiltAnimation : public BaseAnimationTemplate<TiltAnimation, Animation::Tilt> {
   public:
    /* Injected by src/imu/animation_adapters/imu_animations_imu.cpp.
     * Pass nullptr to clear (renders black until a source is re-injected). */
    void setImuSource(AnimationImuSource *source);

    void init() override;
    void tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs) override;

   private:
    AnimationImuSource *imuSource_ = nullptr;

    /* Gyro-integrated state (see tilt_animation.cpp for the model). Gyro-only for
     * now — no bias correction or smoothing, so these slowly drift; a filter is a
     * planned follow-up. */
    float scrollX_ = 0.0f;  /* horizontal scroll (px), integrated from yaw (gyro +X) */
    float scrollY_ = 0.0f;  /* vertical scroll (px), integrated from pitch (gyro +Y) */
    float angleRad_ = 0.0f; /* rainbow-axis rotation (rad), integrated from roll (gyro +Z) */
};

void tilt_animation_bind_default_imu_dependencies();
void tilt_animation_bind_default_bt_dependencies();
