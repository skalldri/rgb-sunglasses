#pragma once
/* Automatic gain control decision logic (issue #264 Phase 2).
 *
 * Deliberately BT-free and Zephyr-free (pure C++ over stdint) so the same
 * translation unit compiles into: the firmware (sound.cpp applies its
 * decisions to the PDM gain registers), the native_sim unit suite
 * (fw/tests/sound/agc), and the WAV-replay harness
 * (fw/tests/sound/audio_dsp_replay) — the replay's closed-loop AGC simulation
 * IS this code, so offline target sweeps transfer to hardware.
 *
 * Policy (fw/docs/audio-fft.md Phase 2c, adapted after baseline measurement):
 *  - Near-clip fast path: block peak ≥ kClipPeak → −2 steps immediately,
 *    bypassing the rate limit. This is the true loudness ceiling (the RMS
 *    window is a comfort band; only int16 capture can clip).
 *  - Attack: instantaneous RMS above targetHigh for attackFrames consecutive
 *    frames → −1 step (subject to the min-gap).
 *  - Release: smoothed (1 s) RMS below targetLow for releaseFrames consecutive
 *    frames → +1 step (subject to the min-gap), never while silent.
 *  - Silence hold: INPUT-REFERRED smoothed RMS below noiseGateRms → hold gain
 *    (a quiet room must not ramp to +20 dB amplifying the mic's own noise —
 *    the issue #264 complaint); after ~10 s of sustained silence, drift one
 *    step every ~2 s toward the 0 dB park value so the next song starts from a
 *    sane gain.
 *  - The `silent` flag also lets the caller suppress beat output entirely.
 *
 * DELIBERATE TRADE-OFF: because the gate is input-referred (gain-invariant),
 * a source quieter than noiseGateRms at the input is never chased — no amount
 * of AGC gain changes its verdict, so release stays blocked and beats stay
 * suppressed. This is the point: "chasing" such sources is exactly what
 * amplified mic noise into false beats (hardware-measured: +20 dB and 711
 * noise-beats/90 s in a quiet room). The margin between room noise (~0.0006
 * input-referred) and barely-audible music (~0.0008) is genuinely thin — no
 * threshold separates them robustly. For high-sensitivity environments, lower
 * the gate at runtime, or set it to 0 to disable gating entirely and restore
 * the pre-Phase-2 chase behavior.
 */

#include <stdint.h>

/**
 * @brief Runtime-tunable AGC parameters. Decouples the controller (and
 * sound.cpp's shell commands) from any concrete BT/Settings-backed
 * implementation — see AudioDspConfigProvider in audio_dsp.h for the full
 * rationale. Implemented by DefaultAgcConfigProvider (sound.cpp) and the
 * BT-backed AudioConfig (audio_config.cpp).
 */
class AgcConfigProvider {
   public:
    virtual ~AgcConfigProvider() = default;

    /** Raise gain when smoothed RMS stays below this (release path). */
    virtual float getTargetLow() = 0;
    virtual void setTargetLow(float value) = 0;

    /** Lower gain when instantaneous RMS stays above this (attack path). */
    virtual float getTargetHigh() = 0;
    virtual void setTargetHigh(float value) = 0;

    /** Minimum frames between gain adjustments (min-gap for attack/release). */
    virtual uint32_t getRateLimitFrames() = 0;
    virtual void setRateLimitFrames(uint32_t value) = 0;

    /** Consecutive over-targetHigh frames required before an attack step. */
    virtual uint32_t getAttackFrames() = 0;
    virtual void setAttackFrames(uint32_t value) = 0;

    /** Consecutive under-targetLow frames required before a release step. */
    virtual uint32_t getReleaseFrames() = 0;
    virtual void setReleaseFrames(uint32_t value) = 0;

    /** INPUT-REFERRED (0 dB-gain-normalized) smoothed RMS below this = silence:
     * hold/park gain, suppress beats. Input-referred so the decision is
     * independent of the current AGC gain (mic/room noise scales with gain;
     * the room doesn't get louder because the AGC turned up). */
    virtual float getNoiseGateRms() = 0;
    virtual void setNoiseGateRms(float value) = 0;
};

/** One frame's AGC decision. */
struct AgcDecision {
    int8_t gain_steps; /* signed register-step change to apply now (0 = hold) */
    bool silent;       /* smoothed RMS is below the noise gate */
    bool clipped;      /* near-clip fast path fired this frame */
};

class AgcController {
   public:
    static constexpr uint8_t kGainMin = 0x00;  /* −20 dB */
    static constexpr uint8_t kGainMax = 0x50;  /* +20 dB */
    static constexpr uint8_t kGainPark = 0x28; /*   0 dB */
    static constexpr int16_t kClipPeak = 32000;
    static constexpr int kHistoryLen = 32; /* ~1 s at 32 ms/frame */
    /* Silence-park timing, in frames (~31.25 fps). */
    static constexpr int kParkAfterSilentFrames = 312;  /* ~10 s */
    static constexpr int kParkStepIntervalFrames = 62;  /* ~2 s per drift step */

    /**
     * @brief Ingest one frame's levels and decide a gain action.
     *
     * @param cfg          tunables (read fresh each frame — the BT-backed
     *                     provider can change at any time)
     * @param rms          this block's normalized RMS (agc_compute_rms)
     * @param peak         this block's peak |sample| (raw int16 magnitude)
     * @param current_gain current PDM gain register value
     * @param allow_adjust false = ingest levels + report silence only, never
     *                     step (the "sound agc freeze" debug path)
     *
     * The caller must apply a nonzero gain_steps to the hardware and then call
     * notifyGainChange() — sound.cpp's agc_apply_gain() does both.
     */
    AgcDecision update(AgcConfigProvider &cfg, float rms, int16_t peak, uint8_t current_gain,
                       bool allow_adjust);

    /**
     * @brief Rescale the internal RMS window into a new gain domain after the
     * hardware gain changed by `steps` (0.5 dB amplitude each) — from update()
     * decisions and manual "sound agc gain" changes alike (single rescale
     * path, called by agc_apply_gain()).
     */
    void notifyGainChange(int steps);

    /** Zero all state (boot / after PDM stream restart). */
    void reset();

    float smoothedRms() const { return smoothed_; }

    /**
     * @brief Input-referred noise-floor estimate: how quiet this room actually is.
     *
     * Asymmetric min-tracker — snaps down instantly, recovers over ~5 minutes — so a
     * momentary loud passage cannot raise it, but moving to a genuinely louder room
     * eventually does. Input-referred for the same reason the noise gate is: the room does
     * not get louder because the AGC turned up.
     *
     * This is the highest-value number the tuning screen can show. `noiseGateRms`'s default
     * was derived offline from exactly this measurement (quiet-room p95 0.00049 vs
     * normal-volume music p5 0.00061 — a 1.25x margin), and without it a venue operator is
     * setting that gate blind. Tracked even while frozen: it is a measurement, not a
     * control action.
     *
     * Zero until the smoothing window has filled (kHistoryLen frames, ~1 s).
     */
    float noiseFloor() const { return noise_floor_; }

    /** Frames since the last gain step — how settled the AGC currently is. */
    uint32_t framesSinceStep() const { return frames_since_step_; }

    /**
     * @brief Smoothed RMS normalised back to the 0 dB park — the quantity the noise gate
     * actually compares, and what a meter should display.
     *
     * This is computed once per frame inside update() anyway; exposing it removes three
     * hand-written copies of `smoothedRms() * audio_dsp_gain_amplitude_ratio(park - gain)`
     * (this class, the telemetry publish site, and the shell status command — the last of
     * which spelled the park as a magic 0x28) and one redundant run of the ratio loop per
     * frame on the DSP thread.
     *
     * Safe to read between frames: notifyGainChange() rescales the window by the same
     * factor the gain moved, so the product is gain-invariant across a step.
     *
     * Zero until the first frame has been ingested.
     */
    float inputReferredRms() const { return input_referred_; }

   private:
    float history_[kHistoryLen] = {};
    uint8_t history_idx_ = 0;
    float smoothed_ = 0.0f;
    uint32_t attack_run_ = 0;
    uint32_t release_run_ = 0;
    uint32_t silent_frames_ = 0;
    uint32_t frames_since_step_ = 0;
    float input_referred_ = 0.0f;
    float noise_floor_ = 0.0f;
    /* Frames ingested since reset, saturating at kHistoryLen. The noise floor is seeded
     * only once the smoothing window has actually FILLED.
     *
     * Seeding earlier is a trap worth naming: smoothed_ is a mean over a window that starts
     * full of zeros, so for the first ~1 s it reads far below the real room level. A
     * min-tracker seeded from that latches the artificially low value and then needs its
     * full ~5 minute rise time to climb out — reporting a silent room the whole way. Caught
     * by test_noise_floor_drops_immediately_but_rises_slowly. */
    uint32_t frames_ingested_ = 0;
};
