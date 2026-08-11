#pragma once

#include <animations/animation_beat_source.h>
#include <animations/animation_parameter_source.h>

#include <atomic>
#include <cstdint>

/**
 * @brief Color-mode selector carried in the metadata byte (bits 24-31) of the 4-byte
 * Color characteristic value (issue #259).
 *
 * Wire layout of the uint32 (little-endian on the wire: b, g, r, mode):
 *   - Static: bits 0-23 are the 0x00RRGGBB color, exactly as before this feature.
 *   - Special modes: bits 16-23 (the R byte) are reinterpreted as a speed/rate
 *     property (0 = slowest, 255 = fastest, never "stopped"); bits 0-15 reserved.
 *
 * This enum is the AUTHORITATIVE definition of the mode values. The companion app
 * mirrors it in app/constants/bluetooth.ts (COLOR_MODE_*) — keep them in lockstep.
 * Any value not listed here (including 0xFF, the upper byte of the 0xFFFFFFFF
 * default persisted on existing devices) must render as Static.
 */
enum class ColorMode : uint8_t {
    Static = 0x00,
    SpectrumSweep = 0x01,
    RandomOnBeat = 0x02,
    RandomOnActivate = 0x03,
    RandomTimerFade = 0x04,
};

/**
 * @brief Integer 6-sector hue wheel at full saturation/value.
 *
 * hue1536 in [0, 1536): sector = hue/256 walks R -> Y -> G -> C -> B -> M, ramp =
 * hue%256 blends within the sector. Every output has at least one channel at 255 so
 * the color survives the pattern controller's global-brightness scaling (a "dim"
 * color drawn at low channel values is invisible on the panel — see fw/CLAUDE.md).
 * Returns 0x00RRGGBB. Exposed for tests.
 */
uint32_t anim_color_from_hue(uint16_t hue1536);

/**
 * @brief Distinct SpectrumSweep starting phase for the @p index -th of @p count
 * concurrent resolvers, spread evenly around the hue wheel.
 *
 * Deterministic rather than random: two sweeps must differ, but they must also be
 * reproducible across activations, and a random offset would reintroduce the
 * activation-time hue jump that skipping the re-roll exists to prevent.
 *
 * @param index Resolver index.
 * @param count Total resolvers to spread across (0 is treated as 1).
 * @return Starting phase in the accumulator's Q16 units.
 */
uint32_t anim_sweep_phase_offset(uint16_t index, uint16_t count);

/**
 * @brief Resolves a raw mode-carrying Color characteristic value into the effective
 * per-tick 0x00RRGGBB color.
 *
 * Wraps the adapter's raw characteristic source; animations keep reading their color
 * through the same AnimationUint32ParameterSource interface and stay unaware of
 * modes (the BT/animation decoupling is untouched — this class is BT-free and is
 * only instantiated by the BT adapters and the extension host).
 *
 * Threading: get() must only be called from the render-tick path (single-threaded —
 * same invariant as the shared SoundAnimationAudioSource). The per-mode state is
 * `mutable` because the inherited get() is const. notifyActivated() may be called
 * from any thread (BT RX / shell / pattern controller) — it only sets an atomic
 * flag, consumed by the next get(); re-activating the already-active animation
 * races that flag by at most one tick of reset timing, which is benign.
 */
class ColorModeSource : public AnimationUint32ParameterSource {
   public:
    using RandomFn = uint32_t (*)();   // production: sys_rand32_get
    using UptimeFn = int64_t (*)();    // production: k_uptime_get

    /**
     * @param raw Underlying mode-carrying characteristic value.
     * @param rng Random source (production: sys_rand32_get).
     * @param now Uptime source (production: k_uptime_get).
     * @param sweepPhaseOffsetQ16 Starting phase for SpectrumSweep, in the same Q16 units
     *        as the internal accumulator. Defaults to 0, which is every built-in
     *        animation: they have one COLOR characteristic each, so there is nothing to
     *        separate from. Callers that build SEVERAL resolvers which can sweep at once
     *        must give each a distinct offset (see @ref anim_sweep_phase_offset and
     *        issue #344) — otherwise identically-configured sweeps stay bit-identical
     *        forever, since reset zeroes the phase and deliberately skips the random
     *        re-roll for this mode.
     */
    ColorModeSource(const AnimationUint32ParameterSource &raw, RandomFn rng, UptimeFn now,
                    uint32_t sweepPhaseOffsetQ16 = 0)
        : raw_(raw), rng_(rng), now_(now), sweepPhaseOffsetQ16_(sweepPhaseOffsetQ16) {}

    uint32_t get() const override;

    /** @brief Arm a state reset (new random color, restarted phase) for the next get(). */
    void notifyActivated() { resetPending_.store(true, std::memory_order_relaxed); }

    /** @brief Per-instance beat source override (tests). nullptr = use the default. */
    void setBeatSource(AnimationBeatSource *src) { beatSource_ = src; }

    /** @brief Boot-time wiring of the shared production beat source (CONFIG_AUDIO). */
    static void setDefaultBeatSource(AnimationBeatSource *src);

   private:
    /** @brief New hue always >= 60 degrees (256/1536) away from base, so consecutive
     * random picks are visibly different (RGB-space random would produce muddy
     * near-duplicates). */
    uint16_t rollHueFrom(uint16_t base) const { return (base + 256u + (rng_() % 1024u)) % 1536u; }

    const AnimationUint32ParameterSource &raw_;
    RandomFn rng_;
    UptimeFn now_;
    AnimationBeatSource *beatSource_ = nullptr;
    uint32_t sweepPhaseOffsetQ16_ = 0;
    // mutable: advanced from the const get(); each resolver owns its own cursor so
    // several can observe the same beat (issue #344).
    mutable AnimationBeatCursor beatCursor_;
    static AnimationBeatSource *sDefaultBeatSource_;  // constant-init (see .cpp)

    // mutable: consumed (exchange) inside the const get(); see threading note.
    mutable std::atomic<bool> resetPending_{false};

    // Per-mode state, valid only while stateValid_ && the mode byte is unchanged.
    // mutable: see the threading note above.
    mutable bool stateValid_ = false;
    mutable uint8_t lastMode_ = 0;
    mutable int64_t lastNowMs_ = 0;      // SpectrumSweep integration timestamp
    mutable uint32_t huePhase16_ = 0;    // SpectrumSweep hue, Q16 in [0, 1536<<16)
    mutable uint16_t currentHue_ = 0;    // last rolled hue (RandomOnBeat/OnActivate)
    mutable uint16_t prevHue_ = 0;       // RandomTimerFade lerp endpoints
    mutable uint16_t targetHue_ = 0;
    mutable int64_t segmentStartMs_ = 0;  // RandomTimerFade segment epoch
};

/**
 * @brief Registers the production (audio-backed) default beat source. Defined in
 * src/sound/animation_adapters/audio_animations_sound.cpp; call from
 * animation_registry_defaults.cpp under CONFIG_AUDIO. When never called (audio
 * disabled), RandomOnBeat degrades to RandomOnActivate behavior.
 */
void color_mode_bind_default_beat_source();
