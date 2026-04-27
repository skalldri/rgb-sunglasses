#pragma once

#include <animations/animation.h>
#include <animations/animation_parameter_source.h>

class TextAnimationSlotSource
{
public:
    virtual ~TextAnimationSlotSource() = default;
    virtual const char *getStringFromSlot(size_t slot) const = 0;
};

class TextAnimationUpNextSource
{
public:
    virtual ~TextAnimationUpNextSource() = default;
    virtual size_t consumeCurrentAndAdvance(size_t numSlots) = 0;
};

class TextAnimationDependencies
{
public:
    TextAnimationDependencies(
        const AnimationUint32ParameterSource &stepTimeMs,
        const AnimationUint32ParameterSource &color,
        const TextAnimationSlotSource &slotSource,
        TextAnimationUpNextSource &upNextSource)
        : stepTimeMs(stepTimeMs),
          color(color),
          slotSource(slotSource),
          upNextSource(upNextSource)
    {
    }

    const AnimationUint32ParameterSource &stepTimeMs;
    const AnimationUint32ParameterSource &color;
    const TextAnimationSlotSource &slotSource;
    TextAnimationUpNextSource &upNextSource;
};

class TextAnimation : public BaseAnimationTemplate<TextAnimation, Animation::Text>
{
public:
    static constexpr size_t kMaxMsgLen = 255;
    static constexpr size_t kNumStringSlots = 20;

    TextAnimation();

    void setDependencies(const TextAnimationDependencies &deps);

    void init() override;
    void tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs) override;

private:
    const char *getStringFromSlot(size_t slot);

    size_t getUpNext();

    const TextAnimationDependencies *deps_ = nullptr;

    char currentMessage[kMaxMsgLen];

    // Current cycle time within the animation cycle
    size_t currentCycleTimeMs = 0;

    int32_t currentTextOffset = 0;
};

void text_animation_bind_default_dependencies();