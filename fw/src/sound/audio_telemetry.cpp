/* Live audio telemetry accumulator — see audio_telemetry.h for the threading contract and
 * why this file is BT-free. */
#include "audio_telemetry.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include "animation_adapters/audio_frame_fold.h"

namespace {

/* Guards the pending snapshot. A spinlock rather than a mutex because the DSP thread's side
 * of this must never block: it is the sole dmic_read() consumer and missing its 32 ms
 * deadline costs an audio frame. Both critical sections are straight-line assignment. */
struct k_spinlock s_lock;

/* Checked before taking the lock at all, so a device with nobody streaming pays one atomic
 * read per analysis frame and nothing else. */
atomic_t s_active = ATOMIC_INIT(0);

/* The analysis half of the snapshot is folded with the SAME function the render path uses
 * (audio_frame_fold), not a second copy of that policy. Latest-frame-wins for every field
 * except the beat flags, which OR across the window — dropping a beat because its frame was
 * not the one sent is precisely the bug issue #376 was about, and it would corrupt every
 * tap-agreement measurement the calibration wizard makes. */
struct audio_analysis_result s_result;

/* Scalars that are not part of audio_analysis_result and so cannot be folded by it. */
float s_rms_input_referred;
float s_rms_instant;
float s_peak_max; /* MAX over the window: a clip two frames ago is still the story */
float s_noise_floor;
float s_alpha;
float s_beat_flux_floor;
uint32_t s_threshold_mode;
uint32_t s_frames_since_step;
int8_t s_gain_steps;
bool s_silent;
bool s_agc_frozen;
bool s_clipped_sticky; /* OR over the window: a clip is an event, not a level */

bool s_have_new;      /* a frame has arrived since the last take() */
uint8_t s_dropped;    /* wrapping; ticks the DSP had no new frame for */
uint8_t s_clip_count; /* wrapping; clip events since reset */

/* The fire line the detector ACTUALLY applied, resolved for the current mode and then
 * floored.
 *
 * Resolved here — once, on the device that owns the algorithm — so the app can plot one
 * honest line without knowing which mode produced it, and without re-deriving a formula
 * that would silently drift from audio_dsp.cpp. Runs at take() time, on the workqueue,
 * because the DSP thread has no business doing arithmetic it does not need. */
float resolve_threshold(float mean, float sigma, uint32_t mode, float alpha, float floor_v) {
    /* Mode 1 stores the already-resolved threshold in the sigma slot; mode 0 stores raw
     * mean/stddev and the fire test is mean + alpha*sigma. The two slots are deliberately
     * mode-dependent — see the struct comment in audio_dsp.h.
     *
     * MUST TRACK audio_dsp.cpp's detector (the `thr`/fire-test block in
     * audio_dsp_process): this is a second derivation of the same fire line, and a new
     * threshold mode or a change to the floor rule updates the detector while silently
     * leaving the meter drawing the old formula — the app would show a line that beats
     * visibly cross without firing, which is the exact confusion this stream exists to
     * end. Exporting band_threshold[] from audio_analysis_result is the real fix; it is
     * deferred because that struct's layout is frozen for msgq/tap/extension-ABI
     * compatibility (audio_dsp.h) and costs ~96 B of always-on RAM across its instances. */
    const float t = (mode == AUDIO_THRESHOLD_MODE_MEDIAN_DELTA) ? sigma : mean + alpha * sigma;
    return t > floor_v ? t : floor_v;
}

}  // namespace

void audio_telemetry_publish(const struct audio_analysis_result *result, float rms_input_referred,
                             float rms_instant, float peak_norm, float noise_floor,
                             int8_t gain_steps, uint32_t frames_since_step, bool silent,
                             bool clipped, bool agc_frozen, uint32_t threshold_mode, float alpha,
                             float beat_flux_floor) {
    if (!atomic_get(&s_active) || result == NULL) {
        return;
    }

    k_spinlock_key_t key = k_spin_lock(&s_lock);

    /* `!s_have_new` is the start of a new send window, which is exactly what
     * audio_frame_fold's firstInBatch means: clear the previous window's beat flags and
     * start OR-ing afresh. */
    audio_frame_fold(s_result, *result, !s_have_new);

    if (clipped) {
        s_clip_count++;
    }
    s_clipped_sticky = s_have_new ? (s_clipped_sticky || clipped) : clipped;
    s_peak_max = s_have_new ? (peak_norm > s_peak_max ? peak_norm : s_peak_max) : peak_norm;

    s_rms_input_referred = rms_input_referred;
    s_rms_instant = rms_instant;
    s_noise_floor = noise_floor;
    s_gain_steps = gain_steps;
    s_frames_since_step = frames_since_step;
    s_silent = silent;
    s_agc_frozen = agc_frozen;
    s_threshold_mode = threshold_mode;
    s_alpha = alpha;
    s_beat_flux_floor = beat_flux_floor;

    s_have_new = true;
    k_spin_unlock(&s_lock, key);
}

bool audio_telemetry_take(struct audio_telemetry_frame *out) {
    if (out == NULL) {
        return false;
    }

    /* Copied out under the lock, used after it. */
    uint32_t mode;
    float alpha;
    float floor_v;

    k_spinlock_key_t key = k_spin_lock(&s_lock);

    const bool had_new = s_have_new;
    if (!had_new) {
        /* Nothing arrived since the last take. The caller may still send — a meter that
         * freezes is better than one that goes blank — but the app has to be able to tell
         * it is looking at a repeat, which is what `dropped` is for. */
        s_dropped++;
    }

    /* No memset: every field below is assigned unconditionally, so zeroing first was pure
     * dead work — and it ran with interrupts off, contended against the DSP thread's 32 ms
     * deadline. Only padding would have depended on it, and nothing compares this struct
     * bytewise (the service compares the PACKED bytes, after quantisation). */
    out->seq = (uint16_t)(s_result.seq & 0xFFFF);
    out->dropped = s_dropped;
    out->gain_steps = s_gain_steps;
    out->rms_input_referred = s_rms_input_referred;
    out->rms_instant = s_rms_instant;
    out->peak = s_peak_max;
    out->noise_floor = s_noise_floor;
    out->clip_count = s_clip_count;
    out->frames_since_step = (uint8_t)(s_frames_since_step > 255u ? 255u : s_frames_since_step);
    out->silent = s_silent;
    out->clipped = s_clipped_sticky;
    out->agc_frozen = s_agc_frozen;
    out->threshold_mode = (uint8_t)(s_threshold_mode & 0x1);

    uint8_t beats = 0;
    for (int b = 0; b < AUDIO_NUM_BANDS; b++) {
        if (s_result.beat[b]) {
            beats |= (uint8_t)(1u << b);
        }
        out->flux[b] = s_result.band_flux[b];
        out->mean[b] = s_result.band_mean[b];
        out->sigma[b] = s_result.band_sigma[b];
    }
    out->beat_mask = beats;

    for (int i = 0; i < AUDIO_NUM_DISPLAY_BUCKETS; i++) {
        out->buckets[i] = s_result.display_bucket_energy[i];
    }

    mode = s_threshold_mode;
    alpha = s_alpha;
    floor_v = s_beat_flux_floor;

    /* Window closed. The next publish() sees !s_have_new and starts a fresh fold — which is
     * what clears the beat flags, via audio_frame_fold's firstInBatch. The event flags this
     * file owns are cleared here; the cumulative clip_count is not, because that is the
     * long-run signal rather than a per-window one. */
    s_have_new = false;
    s_clipped_sticky = false;

    k_spin_unlock(&s_lock, key);

    /* Deliberately OUTSIDE the lock: this reads only the locals and the caller's own frame,
     * so holding the DSP thread off for four branch+FMA+clamp sequences bought nothing. */
    for (int b = 0; b < AUDIO_NUM_BANDS; b++) {
        out->threshold[b] = resolve_threshold(out->mean[b], out->sigma[b], mode, alpha, floor_v);
    }

    return had_new;
}

void audio_telemetry_set_active(bool active) {
    atomic_set(&s_active, active ? 1 : 0);
}

bool audio_telemetry_is_active(void) {
    return atomic_get(&s_active) != 0;
}

void audio_telemetry_reset(void) {
    k_spinlock_key_t key = k_spin_lock(&s_lock);
    memset(&s_result, 0, sizeof(s_result));
    s_rms_input_referred = 0.0f;
    s_rms_instant = 0.0f;
    s_peak_max = 0.0f;
    s_noise_floor = 0.0f;
    s_alpha = 0.0f;
    s_beat_flux_floor = 0.0f;
    s_threshold_mode = 0;
    s_frames_since_step = 0;
    s_gain_steps = 0;
    s_silent = false;
    s_agc_frozen = false;
    s_clipped_sticky = false;
    s_have_new = false;
    s_dropped = 0;
    s_clip_count = 0;
    k_spin_unlock(&s_lock, key);
}
