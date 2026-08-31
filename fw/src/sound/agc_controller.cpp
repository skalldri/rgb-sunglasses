/* AGC decision logic — see agc_controller.h for the policy description and
 * why this file must stay BT-free and Zephyr-free. */
#include "agc_controller.h"

#include "audio_dsp.h" /* audio_dsp_gain_amplitude_ratio() — the single 0.5 dB/step encoding */

AgcDecision AgcController::update(AgcConfigProvider &cfg, float rms, int16_t peak,
                                  uint8_t current_gain, bool allow_adjust) {
    AgcDecision d = {0, false, false};

    /* Ingest this frame's level into the 1 s window. */
    history_[history_idx_] = rms;
    history_idx_ = (uint8_t)((history_idx_ + 1) % kHistoryLen);
    float sum = 0.0f;
    for (int i = 0; i < kHistoryLen; i++) {
        sum += history_[i];
    }
    smoothed_ = sum / (float)kHistoryLen;
    frames_since_step_++;

    /* Silence detection compares the INPUT-REFERRED level: smoothed RMS
     * normalized back to the 0 dB park gain. Mic self-noise and room ambience
     * are input-referred, so whether the room is silent cannot depend on where
     * the AGC currently sits — hardware-found (issue #264 Phase 2 verify): a
     * quiet room whose amplified noise floor landed between the output-domain
     * gate and targetLow let the release path climb to +20 dB with the gate
     * open, resurrecting the noise-beat failure this gate exists to stop. */
    float input_ref =
        smoothed_ * audio_dsp_gain_amplitude_ratio((int)kGainPark - (int)current_gain);
    input_referred_ = input_ref;
    /* Asymmetric min-tracker on the input-referred level: instant down, ~5 min up
     * (1e-4 per frame at 31.25 fps is a ~320 s time constant). Down-fast is what makes it
     * a FLOOR rather than an average — one quiet moment is evidence about the room, one
     * loud moment is not. */
    if (frames_ingested_ < (uint32_t)kHistoryLen) {
        /* Window still filling — smoothed_ is not yet a real measurement of anything. */
        frames_ingested_++;
        if (frames_ingested_ == (uint32_t)kHistoryLen) {
            noise_floor_ = input_ref;
        }
    } else if (input_ref < noise_floor_) {
        noise_floor_ = input_ref;
    } else {
        noise_floor_ += (input_ref - noise_floor_) * 1.0e-4f;
    }

    d.silent = input_ref < cfg.getNoiseGateRms();
    if (d.silent) {
        silent_frames_++;
    } else {
        silent_frames_ = 0;
    }

    if (!allow_adjust) {
        /* Frozen: keep levels/silence live for status + gating, never step. */
        attack_run_ = 0;
        release_run_ = 0;
        return d;
    }

    /* Near-clip fast path: the only signal that genuinely can't wait for the
     * rate limit — int16 capture is saturating. −2 steps (−1 dB) per frame
     * until the peak is back under the ceiling. */
    if (peak >= kClipPeak) {
        d.clipped = true;
        attack_run_ = 0;
        release_run_ = 0;
        int room = (int)current_gain - (int)kGainMin;
        int steps = (room >= 2) ? -2 : -room;
        if (steps != 0) {
            d.gain_steps = (int8_t)steps;
            frames_since_step_ = 0;
        }
        return d;
    }

    /* Attack counts INSTANTANEOUS RMS (react to loud content within ~100 ms);
     * release counts SMOOTHED RMS (only creep up once it's been quiet a
     * while), and never while the noise gate says silence — that's the
     * ramp-to-+20dB-amplifying-fan-noise failure this phase removes. */
    if (rms > cfg.getTargetHigh()) {
        attack_run_++;
    } else {
        attack_run_ = 0;
    }
    if (!d.silent && smoothed_ < cfg.getTargetLow()) {
        release_run_++;
    } else {
        release_run_ = 0;
    }

    const bool gap_ok = frames_since_step_ >= cfg.getRateLimitFrames();
    if (gap_ok && attack_run_ >= cfg.getAttackFrames() && current_gain > kGainMin) {
        d.gain_steps = -1;
        attack_run_ = 0;
        frames_since_step_ = 0;
    } else if (gap_ok && release_run_ >= cfg.getReleaseFrames() && current_gain < kGainMax) {
        d.gain_steps = 1;
        release_run_ = 0;
        frames_since_step_ = 0;
    } else if (d.silent && silent_frames_ >= (uint32_t)kParkAfterSilentFrames &&
               frames_since_step_ >= (uint32_t)kParkStepIntervalFrames &&
               current_gain != kGainPark) {
        /* Sustained silence: drift toward the 0 dB park value so the next
         * song starts from a sane gain instead of wherever the last one left
         * the register. */
        d.gain_steps = (current_gain > kGainPark) ? -1 : 1;
        frames_since_step_ = 0;
    }
    return d;
}

void AgcController::notifyGainChange(int steps) {
    if (steps == 0) {
        return;
    }
    /* Same |steps| > 4 rule as audio_dsp_compensate_gain_change(): a big manual
     * jump is a genuine discontinuity, and extrapolating the window across e.g.
     * +40 steps fabricates levels no real signal can produce (RMS ≤ 1.0), which
     * the unfrozen loop would then "correct" against the operator. Flush — the
     * window refills within one second. */
    if (steps > 4 || steps < -4) {
        for (int i = 0; i < kHistoryLen; i++) {
            history_[i] = 0.0f;
        }
        smoothed_ = 0.0f;
        /* The noise floor has to un-seed with the window it was measured from. Leaving
         * frames_ingested_ saturated here would let the very next update() take a floor
         * reading over 31 zeros and one real frame — about 1/32 of the truth — and because
         * the tracker only ever snaps DOWN instantly, it would then need its full ~5 minute
         * rise (≈12 minutes to within 10%) to climb back. reset() already clears these two
         * for exactly this reason; this path simply predates the tracker. */
        noise_floor_ = 0.0f;
        input_referred_ = 0.0f;
        frames_ingested_ = 0;
        return;
    }
    float amp = audio_dsp_gain_amplitude_ratio(steps);
    for (int i = 0; i < kHistoryLen; i++) {
        history_[i] *= amp;
    }
    smoothed_ *= amp;
}

void AgcController::reset() {
    for (int i = 0; i < kHistoryLen; i++) {
        history_[i] = 0.0f;
    }
    history_idx_ = 0;
    smoothed_ = 0.0f;
    attack_run_ = 0;
    release_run_ = 0;
    silent_frames_ = 0;
    frames_since_step_ = 0;
    noise_floor_ = 0.0f;
    input_referred_ = 0.0f;
    frames_ingested_ = 0;
}
