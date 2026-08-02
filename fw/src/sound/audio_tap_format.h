#pragma once
/* Text wire format for beat-detection frame dumps (issue #264 debugging
 * environment) — the single source of truth for field ORDER, shared by both
 * producers so they can never drift:
 *
 *   - firmware: "sound dump" + the record_wav sidecar CSV (fw/src/sound/sound.cpp)
 *   - host replay harness: fw/tests/sound/audio_dsp_replay/src/main.cpp
 *
 * Decoder: fw/tools/beat_lab/frames.py (update it if this format changes).
 *
 * Floats are emitted as 8-hex-char IEEE-754 bit patterns: exact round-trip,
 * compact, and free of %f (CONFIG_CBPRINTF_FP_SUPPORT=n on the firmware side).
 *
 *   #PARAMS gamma=<hex8> alpha=<hex8> floor=<hex8> refractory=<u> \
 *           agc_frozen=<0|1> gain=<hex2> target_low=<hex8> target_high=<hex8> \
 *           rate_limit=<u>
 *   D,<seq>,<gain hex2>,<beatmask hex1>,<rms>,<e0..e3>,<f0..f3>,<m0..m3>,<s0..s3>[,<d0..d19>]
 *   #DONE frames=<u> dropped=<u>
 *
 * Header-only and Zephyr-free (stdio/string only) so the native_sim replay app
 * can include it unchanged.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio_dsp.h"

static inline uint32_t audio_tap_f32_bits(float v) {
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    return bits;
}

static inline void audio_tap_append_f32(char *buf, size_t cap, size_t *pos, float v) {
    if (*pos >= cap) {
        return;
    }
    int n = snprintf(buf + *pos, cap - *pos, ",%08x", audio_tap_f32_bits(v));
    if (n > 0) {
        *pos += (size_t)n;
    }
}

/* Renders one D-line (no trailing newline) into buf; returns its length. */
static inline size_t audio_tap_format_frame(const struct audio_analysis_result *r, float rms,
                                            uint8_t gain, bool buckets, char *buf, size_t cap) {
    uint8_t beatmask = 0;
    for (int b = 0; b < AUDIO_NUM_BANDS; b++) {
        if (r->beat[b]) {
            beatmask |= (uint8_t)(1u << b);
        }
    }
    size_t pos = (size_t)snprintf(buf, cap, "D,%u,%02x,%x", r->seq, gain, beatmask);
    audio_tap_append_f32(buf, cap, &pos, rms);
    for (int b = 0; b < AUDIO_NUM_BANDS; b++) {
        audio_tap_append_f32(buf, cap, &pos, r->band_energy[b]);
    }
    for (int b = 0; b < AUDIO_NUM_BANDS; b++) {
        audio_tap_append_f32(buf, cap, &pos, r->band_flux[b]);
    }
    for (int b = 0; b < AUDIO_NUM_BANDS; b++) {
        audio_tap_append_f32(buf, cap, &pos, r->band_mean[b]);
    }
    for (int b = 0; b < AUDIO_NUM_BANDS; b++) {
        audio_tap_append_f32(buf, cap, &pos, r->band_sigma[b]);
    }
    if (buckets) {
        for (int b = 0; b < AUDIO_NUM_DISPLAY_BUCKETS; b++) {
            audio_tap_append_f32(buf, cap, &pos, r->display_bucket_energy[b]);
        }
    }
    return pos < cap ? pos : cap - 1;
}

/* Renders the #PARAMS snapshot line (no trailing newline); returns its length.
 * Values are passed explicitly because the two producers source them
 * differently (firmware: config providers; replay: environment variables). */
static inline size_t audio_tap_format_params(float gamma, float alpha, float flux_floor,
                                             uint32_t refractory, bool agc_frozen, uint8_t gain,
                                             float target_low, float target_high,
                                             uint32_t rate_limit, char *buf, size_t cap) {
    int n = snprintf(buf, cap,
                     "#PARAMS gamma=%08x alpha=%08x floor=%08x refractory=%u agc_frozen=%d "
                     "gain=%02x target_low=%08x target_high=%08x rate_limit=%u",
                     audio_tap_f32_bits(gamma), audio_tap_f32_bits(alpha),
                     audio_tap_f32_bits(flux_floor), refractory, agc_frozen ? 1 : 0, gain,
                     audio_tap_f32_bits(target_low), audio_tap_f32_bits(target_high), rate_limit);
    return (n < 0) ? 0 : ((size_t)n < cap ? (size_t)n : cap - 1);
}
