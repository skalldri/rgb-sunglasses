#pragma once

#include <animations/animation.h>
#include <animations/animation_beat_source.h>
#include <animations/animation_parameter_source.h>

class PulseAnimationDependencies {
   public:
    PulseAnimationDependencies(const AnimationUint32ParameterSource &color,
                                const AnimationUint32ParameterSource &periodMs,
                                const AnimationBoolParameterSource &breathingEnabled,
                                const AnimationBoolParameterSource &beatSyncEnabled)
        : color(color),
          periodMs(periodMs),
          breathingEnabled(breathingEnabled),
          beatSyncEnabled(beatSyncEnabled) {}

    const AnimationUint32ParameterSource &color;
    const AnimationUint32ParameterSource &periodMs;
    const AnimationBoolParameterSource &breathingEnabled;
    const AnimationBoolParameterSource &beatSyncEnabled;
};

/**
 * @brief Solid-color panel with three selectable brightness envelopes (issue #148).
 *
 * - Neither toggle set: constant full brightness. This is the "flashlight" case.
 * - Breathing: triangle wave over period_ms, the original behaviour.
 * - Beat sync: each detected beat snaps to full brightness, then ramps back down
 *   over half a period — the same downward ramp breathing draws, retriggered by the
 *   music instead of by the clock.
 *
 * The two toggles are mutually exclusive, enforced at BLE-write time by the adapter
 * so the app can render them as two independent switches (see
 * src/bluetooth/animation_adapters/pulse_animation_bt.cpp). tick() still needs a
 * precedence for the case where persisted settings somehow hold both: beat sync wins.
 */
class PulseAnimation : public BaseAnimationTemplate<PulseAnimation, Animation::Pulse> {
   public:
    void setDependencies(const PulseAnimationDependencies &deps);

    /**
     * @brief Wire the beat feed used by the beat-sync envelope.
     *
     * Bound at boot from the sound adapter (CONFIG_AUDIO only). Left null on
     * audio-less builds, and beat sync then renders as constant full brightness
     * rather than a permanently dark panel — a toggle whose input does not exist
     * should not look like a hardware fault.
     */
    void setBeatSource(AnimationBeatSource *source) { beatSource_ = source; }

    void init() override;
    void tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs) override;

   private:
    /** @brief Re-arm beat sync: zero the envelope and discard any stale latched beat. */
    void armBeatSync();

    const PulseAnimationDependencies *deps_ = nullptr;
    AnimationBeatSource *beatSource_ = nullptr;

    // Position within the current breathing cycle, in ms; wraps at period_ms.
    size_t currentCycleTimeMs = 0;

    // Beat-sync envelope in [0, 1]: set to 1 on a beat, decays between them.
    float beatEnvelope_ = 0.0f;

    // Tracks beat-sync entry so the envelope can be re-armed. init() alone is not
    // enough: it runs only on an animation switch, while the toggle is a BLE write
    // that leaves the active animation untouched. Without this, flipping Beat Sync
    // off freezes the envelope mid-decay and flipping it back on resumes from that
    // frozen value in a silent room.
    bool beatSyncWasActive_ = false;
};

void pulse_animation_bind_default_dependencies();
void pulse_animation_bind_default_sound_dependencies();
