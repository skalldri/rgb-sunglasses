#include <animations/pulse_animation.h>
#include <zephyr/sys/__assert.h>

void PulseAnimation::setDependencies(const PulseAnimationDependencies &deps) {
    deps_ = &deps;
}

void PulseAnimation::init() {
    currentCycleTimeMs = 0;
    beatEnvelope_ = 0.0f;
    // Forces the entry re-arm in tick() on the first beat-sync tick of this
    // activation, which is what actually makes the panel start dark — zeroing the
    // envelope here is not sufficient on its own (see armBeatSync).
    beatSyncWasActive_ = false;
}

void PulseAnimation::armBeatSync() {
    beatEnvelope_ = 0.0f;
    // Resynchronise the cursor to the source's current count, so beats detected while
    // Pulse was inactive or not in beat-sync mode are not reported on entry. Without
    // this, playing Beat with music and then switching to Pulse (or just toggling Beat
    // Sync on) makes the first consumeBeat() report those stale beats and flash the
    // panel at full brightness in silence — the exact thing starting from zero is meant
    // to avoid. Zeroing the envelope alone does not achieve it.
    if (beatSource_ != nullptr) {
        beatCursor_.resync(*beatSource_);
    }
}

void PulseAnimation::tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs) {
    __ASSERT(deps_, "PulseAnimation::tick before setDependencies");

    // Guard against a zero (or otherwise degenerate) period_ms making the modulo
    // below divide by zero; a 1ms period just breathes as fast as tick() allows.
    uint32_t periodMs = deps_->periodMs.get();
    if (periodMs == 0) {
        periodMs = 1;
    }

    currentCycleTimeMs += timeSinceLastTickMs;
    currentCycleTimeMs %= periodMs;

    // Beat sync wins over breathing; see the mutual-exclusion note in the header.
    // Deliberately NOT folded together with the null-source check below: with both
    // flags somehow set on an audio-less build, folding them would fall through to
    // breathing, contradicting both "beat sync wins" and the constant-full-brightness
    // fallback documented on setBeatSource().
    const bool beatSync = deps_->beatSyncEnabled.get();

    if (beatSync != beatSyncWasActive_) {
        beatSyncWasActive_ = beatSync;
        if (beatSync) {
            armBeatSync();
        }
    }

    float brightness;
    if (beatSync && beatSource_ == nullptr) {
        // No beat feed bound (audio-less build): hold lit rather than sit dark, so a
        // toggle whose input does not exist cannot look like a hardware fault.
        brightness = 1.0f;
    } else if (beatSync) {
        if (beatCursor_.consumeBeat(*beatSource_)) {
            beatEnvelope_ = 1.0f;
        } else {
            // Ramp down over half a period, so one beat draws the same falling edge
            // the breathing envelope draws and period_ms keeps one meaning for the
            // user across both modes.
            uint32_t decayMs = periodMs / 2;
            if (decayMs == 0) {
                decayMs = 1;
            }
            float step = (float)timeSinceLastTickMs / (float)decayMs;
            beatEnvelope_ = (beatEnvelope_ > step) ? (beatEnvelope_ - step) : 0.0f;
        }
        brightness = beatEnvelope_;
    } else if (deps_->breathingEnabled.get()) {
        // Triangle-wave breathing envelope: ramps 0 -> 1 across the first half of
        // the period, then back 1 -> 0 across the second half.
        float phase = (float)currentCycleTimeMs / (float)periodMs;
        brightness = (phase < 0.5f) ? (phase * 2.0f) : (2.0f - (phase * 2.0f));
    } else {
        // Flashlight: constant full brightness.
        brightness = 1.0f;
    }

    uint32_t color = deps_->color.get();
    float red = (float)((color >> 16) & 0xFF) * brightness;
    float green = (float)((color >> 8) & 0xFF) * brightness;
    float blue = (float)((color >> 0) & 0xFF) * brightness;

    for (size_t x = 0; x < renderer.displayWidth(); x++) {
        for (size_t y = 0; y < renderer.displayHeight(); y++) {
            renderer.setPixel(x, y, red, green, blue);
        }
    }
}
