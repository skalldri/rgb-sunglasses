#pragma once

/**
 * @brief Runtime-tunable spectrogram visualization parameters.
 *
 * The five values describe the dB-window mapping in fft_bar_mapping.h: floor/range/tilt are
 * the window, energyScale is the legacy relative-gain knob, and smoothingCoeff is the
 * per-step EMA weight applied to the resulting bar HEIGHT.
 *
 * Its own header — not fft_bars_animation.h — so that the two things that need the
 * INTERFACE (AudioConfig, which implements it, and the `sound dsp params` diagnostic, which
 * prints through an injected pointer) never include the concrete animation class. The
 * animation consumes the interface; the sound layer stays below it (same one-directional
 * rule as the BT/animation split — see fw/CLAUDE.md).
 *
 * The native_sim DI suite (fw/tests/animations/fft_bars_animation_di/) installs its own
 * fake, or calls FftBarsAnimation::clearConfigSource() to exercise the constexpr fallbacks in
 * tick() (which static_assert equal to the audio_param_table.h defaults).
 */
class FftVisualizationConfigSource {
   public:
    virtual ~FftVisualizationConfigSource() = default;

    /** EMA weight applied to the newest bar height; 1-this is applied to history. */
    virtual float getSmoothingCoeff() const = 0;

    /** Legacy `audio/fft_energy_scale`: relative gain, 0 dB at kFftBarEnergyScaleUnity. */
    virtual float getEnergyScale() const = 0;

    /** dB of bucket power at which a bar starts to light. */
    virtual float getFloorDb() const = 0;

    /** dB from an empty bar to a full one. */
    virtual float getRangeDb() const = 0;

    /** Pink-noise compensation, dB added per octave above bucket 0. */
    virtual float getTiltDbPerOctave() const = 0;
};
