#pragma once

#include <animations/animation.h>
#include <animations/animation_parameter_source.h>

class MatrixCodeAnimationDependencies {
   public:
    MatrixCodeAnimationDependencies(const AnimationUint32ParameterSource &dropSpeedMs,
                                    const AnimationUint32ParameterSource &fadeTimeMs,
                                    const AnimationUint32ParameterSource &density,
                                    const AnimationUint32ParameterSource &color)
        : dropSpeedMs(dropSpeedMs), fadeTimeMs(fadeTimeMs), density(density), color(color) {}

    const AnimationUint32ParameterSource &dropSpeedMs;
    const AnimationUint32ParameterSource &fadeTimeMs;
    const AnimationUint32ParameterSource &density;
    const AnimationUint32ParameterSource &color;
};

static constexpr size_t kMatrixMaxCols = 40;
static constexpr size_t kMatrixMaxRows = 12;

class MatrixCodeAnimation : public BaseAnimationTemplate<MatrixCodeAnimation, Animation::MatrixCode> {
   public:
    void setDependencies(const MatrixCodeAnimationDependencies &deps);
    void init() override;
    void tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs) override;

   private:
    const MatrixCodeAnimationDependencies *deps_ = nullptr;

    struct ColumnState {
        bool active;
        uint8_t headY;
        size_t dropTimerMs;
    };

    ColumnState columns_[kMatrixMaxCols] = {};
    uint8_t brightness_[kMatrixMaxCols][kMatrixMaxRows] = {};
};

void matrix_code_animation_bind_default_dependencies();
