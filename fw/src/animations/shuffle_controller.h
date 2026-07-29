#pragma once

#include <animations/animation_types.h>

#include <cstddef>
#include <cstdint>

/**
 * @brief Injected view of the animation pool the shuffle can pick from (issue #121).
 * Production: the animation registry + extension fault state (see pattern_controller.cpp).
 * Tests: a fixture array. Kept abstract so the decision logic below is Zephyr- and
 * BT-free and fully coverable on native_sim.
 */
class ShuffleAnimationPool {
   public:
    virtual ~ShuffleAnimationPool() = default;
    virtual size_t count() = 0;
    virtual Animation idAt(size_t index) = 0;
    /** @brief false excludes an id from shuffle (Animation::None, faulted extension slots). */
    virtual bool isEligible(Animation id) = 0;
};

/** @brief Injected config (production: CoreConfig getters; tests: fixture values). */
class ShuffleConfigSource {
   public:
    virtual ~ShuffleConfigSource() = default;
    virtual bool enabled() = 0;
    virtual uint32_t minDurationS() = 0;
    virtual uint32_t maxDurationS() = 0;
};

/** @brief Random source (production: sys_rand32_get; tests: a scripted sequence). */
using ShuffleRandomFn = uint32_t (*)();

/**
 * @brief The shuffle-mode decision state machine. Pure logic: no threads, no clock —
 * time is fed in as per-frame dt. Call onFrame() once per render frame, on the
 * pattern-controller thread, AFTER the active animation's tick() and only while no
 * indicator overlay is active.
 */
class ShuffleController {
   public:
    /**
     * @param graceMs The fixed wait every animation gets past its dwell target before
     * the switch is forced — the anti-hang guard (CONFIG_APP_SHUFFLE_GRACE_S).
     * @param maxGraceMs Ceiling on the *extra* wait an animation may REQUEST via
     * onFrame()'s requestedGraceMs (CONFIG_APP_SHUFFLE_MAX_GRACE_S). 0 (the default)
     * disables animation-requested grace entirely and reproduces the original issue-#121
     * behaviour exactly.
     */
    ShuffleController(ShuffleAnimationPool &pool, ShuffleConfigSource &config,
                      ShuffleRandomFn rng, uint64_t graceMs, uint64_t maxGraceMs = 0);

    struct Decision {
        bool switchNow = false;
        Animation next = Animation::None;
    };

    /**
     * @brief One shuffle step.
     * @param current The currently-active animation.
     * @param dtMs Nominal time covered by the frame just ticked.
     * @param animationAtGoodSwitchPoint The active animation's isAtGoodSwitchPoint().
     * @param requestedGraceMs The active animation's goodSwitchPointGraceMs() — how far
     * away its next natural boundary is measured from NOW, NOT an offset from the dwell
     * target. 0 means "no request" (every animation but the GLIM player and the text
     * animation, and unavoidably every sandboxed extension, which has no rgbx hook for
     * this), which leaves graceMs alone in charge exactly as before.
     *
     * Detects external animation changes itself (current != last seen -> dwell resets),
     * so callers of pattern_controller_change_to_animation() need no shuffle hooks.
     */
    Decision onFrame(Animation current, uint32_t dtMs, bool animationAtGoodSwitchPoint,
                     uint32_t requestedGraceMs = 0);

   private:
    Animation pickNext(Animation current);
    void rearm();
    uint64_t computeForcedSwitchDeadlineMs(uint32_t requestedGraceMs) const;

    ShuffleAnimationPool &pool_;
    ShuffleConfigSource &config_;
    ShuffleRandomFn rng_;
    uint64_t graceMs_;
    uint64_t maxGraceMs_;

    uint64_t dwellMs_ = 0;
    uint64_t targetMs_ = 0;
    bool armed_ = false;  // false when disabled / just constructed
    Animation lastSeen_ = Animation::None;

    // The dwell at which the switch is forced regardless of the good-moment signal.
    // Latched on the first frame past targetMs_ and held for the rest of the dwell (see
    // computeForcedSwitchDeadlineMs), so exactly one grace window exists per dwell.
    uint64_t graceDeadlineMs_ = 0;
    bool graceDeadlineSet_ = false;
};
