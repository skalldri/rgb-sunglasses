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
    size_t tiltOffset_ = 0;
};

void tilt_animation_bind_default_imu_dependencies();
void tilt_animation_bind_default_bt_dependencies();
