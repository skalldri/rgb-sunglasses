#pragma once

#include <animations/animation.h>
#include <animations/animation_parameter_source.h>

class MyEyesAnimationSlotSource {
   public:
    virtual ~MyEyesAnimationSlotSource() = default;
    virtual const char *getStringFromSlot(size_t slot) const = 0;
};

class MyEyesAnimationUpNextSource {
   public:
    virtual ~MyEyesAnimationUpNextSource() = default;
    virtual size_t consumeCurrentAndAdvance(size_t numSlots) = 0;
};

class MyEyesAnimationDependencies {
   public:
    MyEyesAnimationDependencies(const AnimationUint32ParameterSource &blinkSpeedMs,
                                const AnimationUint32ParameterSource &color,
                                const MyEyesAnimationSlotSource &slotSource,
                                MyEyesAnimationUpNextSource &upNextSource,
                                const AnimationUint32ParameterSource &dwellTimeMs)
        : blinkSpeedMs(blinkSpeedMs),
          color(color),
          slotSource(slotSource),
          upNextSource(upNextSource),
          dwellTimeMs(dwellTimeMs) {}

    const AnimationUint32ParameterSource &blinkSpeedMs;
    const AnimationUint32ParameterSource &color;
    const MyEyesAnimationSlotSource &slotSource;
    MyEyesAnimationUpNextSource &upNextSource;
    // How long each eye slot displays before tick() advances to the next one
    // (issue #260 autonomous cycling); clamped to a 500 ms floor in tick().
    const AnimationUint32ParameterSource &dwellTimeMs;
};

enum class EyeState {
    Open,              // Eye is fully open, we are not within a blink cycle
    OpenInBlinkCycle,  // Eye is fully open, but we have more blinks to perform in this cycle
    BlinkClosing,      // Eye is in the process of shutting
    Closed,            // Eye is fully closed
    BlinkOpening       // Eye is in the process of opening
};

class MyEyesAnimation : public BaseAnimationTemplate<MyEyesAnimation, Animation::MyEyes> {
   public:
    static constexpr size_t kMaxEyeLen = 3;
    static constexpr size_t kNumStringSlots = 20;
    static constexpr size_t kLeftEyePos = 5;    // X-start coordinate of left eye
    static constexpr size_t kRightEyePos = 28;  // X-start coordinate of left eye

    MyEyesAnimation();

    void setDependencies(const MyEyesAnimationDependencies &deps);

    void init() override;
    void tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs) override;
    bool isAtGoodSwitchPoint() const override { return dwellTracker_.isAtBoundary(); }

    // Shuffle's wait budget for the dwell boundary: how much longer the current eyes
    // display before tick() advances — so shuffle switches on a slot boundary instead
    // of mid-dwell (same contract as TextAnimation's end-of-scroll signal).
    uint32_t goodSwitchPointGraceMs() const override { return dwellTracker_.remainingMs(); }

   private:
    const char *getStringFromSlot(size_t slot);

    size_t getUpNext();

    const MyEyesAnimationDependencies *deps_ = nullptr;

    // Buffer to store the currently rendered eyes
    char currentEyes[kMaxEyeLen];

    // Current eye state. Start open
    EyeState currentEyeState = EyeState::Open;

    // How long since the last blink cycle?
    size_t timeSinceLastBlinkCycleMs = 0;

    // Track if we are currently blinking
    bool blinkCycleInProgress = false;

    // How many times are we going to blink in this cycle?
    size_t numBlinksInThisCycle = 1;

    // The amount of time spent in the current state
    size_t timeInCurrentStateMs = 0;

    // Drives the autonomous advance to the next slot once the configured dwell time
    // elapses (issue #260), with deferred-consume boundary semantics — see
    // SlotDwellTracker in animation_base.h.
    SlotDwellTracker dwellTracker_;
};

void my_eyes_animation_bind_default_dependencies();