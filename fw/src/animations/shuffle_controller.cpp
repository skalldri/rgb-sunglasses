#include <animations/shuffle_controller.h>

ShuffleController::ShuffleController(ShuffleAnimationPool &pool, ShuffleConfigSource &config,
                                     ShuffleRandomFn rng, uint64_t graceMs)
    : pool_(pool), config_(config), rng_(rng), graceMs_(graceMs) {}

void ShuffleController::rearm() {
    // Read min/max fresh on every re-arm so BLE config changes take effect on the next
    // dwell period. min > max is tolerated by design (the two values are separate GATT
    // characteristics written one at a time, so transient/persistent inversion is a
    // normal state, never rejected) — swap at pick time.
    uint64_t minS = config_.minDurationS();
    uint64_t maxS = config_.maxDurationS();
    if (minS > maxS) {
        uint64_t tmp = minS;
        minS = maxS;
        maxS = tmp;
    }

    // span fits uint64 even at uint32-max bounds; min == max => span 1 => exact duration.
    // Modulo bias is negligible for "how long to show an LED animation" and not worth a
    // rejection-sampling loop.
    const uint64_t spanS = maxS - minS + 1u;
    targetMs_ = (minS + (rng_() % spanS)) * 1000u;
    dwellMs_ = 0;
}

Animation ShuffleController::pickNext(Animation current) {
    // Two passes: count the eligible candidates, then walk to the r-th one. The pool is
    // small (registry entries), so this stays trivially cheap and needs no allocation.
    size_t eligible = 0;
    const size_t poolCount = pool_.count();
    for (size_t i = 0; i < poolCount; i++) {
        const Animation id = pool_.idAt(i);
        if (id != current && pool_.isEligible(id)) {
            eligible++;
        }
    }

    if (eligible == 0) {
        // Pool-of-0 or pool-of-1 (only the current animation is eligible): nothing to
        // switch to. None doubles as "stay put" — it is never itself a valid pick since
        // isEligible(None) is false in every pool implementation.
        return Animation::None;
    }

    size_t r = rng_() % eligible;
    for (size_t i = 0; i < poolCount; i++) {
        const Animation id = pool_.idAt(i);
        if (id != current && pool_.isEligible(id)) {
            if (r == 0) {
                return id;
            }
            r--;
        }
    }

    // Unreachable if the pool is stable within one onFrame() call; be safe anyway.
    return Animation::None;
}

ShuffleController::Decision ShuffleController::onFrame(Animation current, uint32_t dtMs,
                                                       bool animationAtGoodSwitchPoint) {
    Decision decision;

    if (!config_.enabled()) {
        // Disable mid-wait fully resets: re-enabling starts a fresh dwell + pick.
        armed_ = false;
        return decision;
    }

    if (!armed_ || current != lastSeen_) {
        // Enable edge, or the animation changed under us (manual BLE/shell change, boot
        // restore, or our own switch landing last frame): restart the dwell from here.
        armed_ = true;
        lastSeen_ = current;
        rearm();
        return decision;
    }

    dwellMs_ += dtMs;

    if (dwellMs_ < targetMs_) {
        return decision;
    }

    // Past the picked duration: switch at the animation's natural boundary, or force the
    // switch once the grace cap expires so a signal that never comes (e.g. a misbehaving
    // extension) can't pin shuffle on one animation forever.
    if (!animationAtGoodSwitchPoint && dwellMs_ < targetMs_ + graceMs_) {
        return decision;
    }

    const Animation next = pickNext(current);
    if (next == Animation::None) {
        // Nothing else eligible right now — re-arm a fresh period instead of re-picking
        // every frame, and try again after it elapses.
        rearm();
        return decision;
    }

    decision.switchNow = true;
    decision.next = next;
    // Optimistically track the switch landing so the next frame starts the new dwell
    // immediately (one deterministic rearm per switch). If the caller's switch fails
    // (e.g. the animation got unregistered), current != lastSeen_ next frame and the
    // mismatch branch above restarts the dwell — same recovery as a manual change.
    lastSeen_ = next;
    rearm();
    return decision;
}
