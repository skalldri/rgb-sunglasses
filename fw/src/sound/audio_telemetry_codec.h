#pragma once
/* Packed wire format for live audio telemetry (the Audio Tuning screen's meters).
 *
 * Why this exists: AGC and beat detection have to be tuned at the venue, and until now
 * the only window into the DSP was the USB serial shell — useless with the glasses on
 * someone's face. This is the format that gets the same numbers to the phone over BLE.
 *
 * Header-only and Zephyr-free (stdint/math only), same discipline as audio_tap_format.h,
 * so the ztest suite compiles it directly as the unit under test and any host decoder can
 * share it unchanged.
 *
 * THREE NESTED TIERS, each a byte-exact PREFIX of the next:
 *
 *   tier 1  METERS    20 B   everything the meters need
 *   tier 2  STATS     28 B   + raw mean/sigma, for the calibration wizard's offline replay
 *   tier 3  SPECTRUM  48 B   + the 20 display buckets
 *
 * 20 is not a round number chosen for looks: it is ATT_MTU 23 minus the 3-byte notify
 * header, so tier 1 survives a link that never negotiated an MTU. That is a real, durable
 * state on the OnePlus stale-GATT split-brain (issue #115), and bt_gatt_notify() cannot
 * fragment — an over-length payload is simply dropped. The firmware therefore clamps the
 * tier it sends from the live MTU and stamps the ACTUAL tier into the header, so a
 * degraded link keeps streaming meters instead of going silent.
 *
 * QUANTISATION. Every magnitude is one byte on a 0.5 dB log ladder (see q_log below).
 * That costs ~6% relative error, which is invisible on a meter and irrelevant to a
 * slider. It is NOT adequate to re-derive a beat decision from, which is exactly why the
 * per-band beat bits are carried explicitly: a consumer must never recompute
 * `flux > threshold` from these bytes and expect to match the detector.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "audio_dsp.h"

/* Wire version. Bump only on an incompatible change; a consumer that sees an unknown
 * version must discard the frame rather than guess at its layout. */
#define AUDIO_TELEMETRY_VERSION 1

/* Tier numbering is shared with the control characteristic, so there is no off-by-one
 * between "what the app asked for" and "what the header says was sent". 0 means off and
 * is never a payload tier. */
#define AUDIO_TELEMETRY_TIER_OFF 0
#define AUDIO_TELEMETRY_TIER_METERS 1
#define AUDIO_TELEMETRY_TIER_STATS 2
#define AUDIO_TELEMETRY_TIER_SPECTRUM 3
#define AUDIO_TELEMETRY_TIER_MAX AUDIO_TELEMETRY_TIER_SPECTRUM

#define AUDIO_TELEMETRY_SIZE_METERS 20
#define AUDIO_TELEMETRY_SIZE_STATS 28
#define AUDIO_TELEMETRY_SIZE_SPECTRUM 48

/* The ATT floor this format is built around: MTU 23 - 3 bytes of notify header. */
#define AUDIO_TELEMETRY_UNNEGOTIATED_ATT_PAYLOAD 20

/* flags byte */
#define AUDIO_TELEMETRY_FLAG_SILENT 0x01
#define AUDIO_TELEMETRY_FLAG_CLIPPED 0x02
#define AUDIO_TELEMETRY_FLAG_AGC_FROZEN 0x04
#define AUDIO_TELEMETRY_FLAG_THRESHOLD_MODE 0x08
#define AUDIO_TELEMETRY_BEAT_SHIFT 4

#ifdef __cplusplus
/* The tier sizes and the field offsets in audio_telemetry_pack() are written out
 * independently, so pin the relationship here rather than trusting two hand-maintained
 * lists to agree. Each tier is a prefix of the next, so every size is the previous one
 * plus exactly the fields that tier adds. */
static_assert(AUDIO_TELEMETRY_SIZE_METERS == 12 + 2 * AUDIO_NUM_BANDS,
              "meters tier size disagrees with its field layout");
static_assert(AUDIO_TELEMETRY_SIZE_STATS == AUDIO_TELEMETRY_SIZE_METERS + 2 * AUDIO_NUM_BANDS,
              "stats tier must be the meters tier plus mean[] and sigma[]");
static_assert(AUDIO_TELEMETRY_SIZE_SPECTRUM ==
                  AUDIO_TELEMETRY_SIZE_STATS + AUDIO_NUM_DISPLAY_BUCKETS,
              "spectrum tier must be the stats tier plus the display buckets");
/* The whole point of the meters tier: it must survive a link that never negotiated an
 * MTU. If this ever fails, the fix is to shrink the tier, not to raise the constant. */
static_assert(AUDIO_TELEMETRY_SIZE_METERS <= AUDIO_TELEMETRY_UNNEGOTIATED_ATT_PAYLOAD,
              "meters tier no longer fits an unnegotiated ATT MTU of 23");
static_assert(AUDIO_TELEMETRY_TIER_MAX <= 0x0F, "tier must fit the header's low nibble");
static_assert(AUDIO_TELEMETRY_VERSION <= 0x0F, "version must fit the header's high nibble");
#endif

/* One frame's worth of telemetry, before packing. Plain floats: the producer should not
 * have to think about the wire encoding, and the tests get to state expectations in real
 * units. */
struct audio_telemetry_frame {
    uint16_t seq;      /* low 16 bits of the analysis frame counter */
    uint8_t dropped;   /* wrapping count of sends that carried no new frame or failed */
    int8_t gain_steps; /* PDM gain relative to the 0 dB park, in 0.5 dB register steps */

    float rms_input_referred; /* smoothed, input-referred - the noise gate's own comparand */
    float rms_instant;        /* per-frame RMS - the attack path's comparand */
    float peak;               /* per-frame peak, normalised to 0..1 */
    float noise_floor;        /* input-referred noise-floor estimate */

    uint8_t clip_count;        /* wrapping */
    uint8_t frames_since_step; /* saturating at 255 */

    bool silent;
    bool clipped;
    bool agc_frozen;
    uint8_t threshold_mode; /* 0 = mean+alpha*sigma, 1 = median+delta */
    uint8_t beat_mask;      /* bit b = band b fired since the last send (sticky-OR) */

    float flux[AUDIO_NUM_BANDS];
    /* The EFFECTIVE fire line the detector actually applied this frame, already resolved
     * for the current threshold mode AND the absolute floor. Carrying the resolved value
     * rather than the raw statistics is what lets a consumer plot one honest line without
     * knowing which mode produced it — the mode flag is there only to label it. */
    float threshold[AUDIO_NUM_BANDS];

    /* Tier 2+. The RAW statistics, which the resolved threshold above cannot be inverted
     * back into (the floor clamp is lossy). The calibration wizard needs these to replay
     * a recorded window against candidate sensitivities without re-recording. */
    float mean[AUDIO_NUM_BANDS];
    float sigma[AUDIO_NUM_BANDS];

    /* Tier 3. */
    float buckets[AUDIO_NUM_DISPLAY_BUCKETS];
};

/**
 * @brief Quantise a magnitude onto the 0.5 dB log ladder.
 *
 * q == 0 is reserved for "zero, or below the representable floor". q in [1, 255] encodes
 * 20*log10(v) = q/2 - 100 dB, so q=200 is 0 dBFS (1.0), q=1 is -99.5 dBFS and q=255 is
 * +27.5 dB.
 *
 * The range is chosen against measured signals, not guessed: the floor sits ~35 dB below
 * the room noise this device sees at 0 dB gain (0.0006), and the ceiling ~17 dB above the
 * largest band-0 flux observed on music (~3.5).
 *
 * Non-finite input maps to 0 rather than propagating: a NaN reaching a meter would render
 * as a blank or a wild spike, and there is no honest value to show.
 */
static inline uint8_t audio_telemetry_q_log(float v) {
    if (!(v > 0.0f)) { /* also catches NaN, whose comparisons are all false */
        return 0;
    }
    const float q = 40.0f * log10f(v) + 200.0f;
    if (q < 1.0f) {
        return 0; /* below the floor is indistinguishable from zero on a meter */
    }
    if (q > 255.0f) {
        return 255;
    }
    return (uint8_t)(q + 0.5f);
}

/**
 * @brief Inverse of audio_telemetry_q_log().
 *
 * Deliberately a loop of multiplies rather than powf(): float pow is compiled out
 * firmware-wide (see audio_dsp_gain_amplitude_ratio(), which exists for the same reason).
 * The per-step constants are literally the same numbers, because one quantiser step and
 * one PDM gain register step are both 0.5 dB — that is not a coincidence worth hiding.
 */
static inline float audio_telemetry_dq_log(uint8_t q) {
    if (q == 0) {
        return 0.0f;
    }
    const float kStepUp = 1.0592537f;   /* 10^(1/40) = 10^0.025 */
    const float kStepDown = 0.9440609f; /* 10^-0.025 */
    const int steps = (int)q - 200;
    const int n = steps > 0 ? steps : -steps;
    float v = 1.0f;
    for (int i = 0; i < n; i++) {
        v *= (steps > 0) ? kStepUp : kStepDown;
    }
    return v;
}

/** @brief Wire size of a tier, or 0 if the tier is not a payload tier. */
static inline size_t audio_telemetry_tier_size(uint8_t tier) {
    switch (tier) {
        case AUDIO_TELEMETRY_TIER_METERS:
            return AUDIO_TELEMETRY_SIZE_METERS;
        case AUDIO_TELEMETRY_TIER_STATS:
            return AUDIO_TELEMETRY_SIZE_STATS;
        case AUDIO_TELEMETRY_TIER_SPECTRUM:
            return AUDIO_TELEMETRY_SIZE_SPECTRUM;
        default:
            return 0;
    }
}

/**
 * @brief Highest tier that fits in `att_payload` bytes, or TIER_OFF if even meters do not.
 *
 * `att_payload` is ATT_MTU - 3. The caller clamps with this rather than assuming its
 * requested tier fits, because bt_gatt_notify() cannot fragment: one byte over and the
 * frame is dropped outright, silently, for as long as the link stays degraded.
 */
static inline uint8_t audio_telemetry_tier_for_mtu(uint8_t requested, size_t att_payload) {
    uint8_t tier = requested > AUDIO_TELEMETRY_TIER_MAX ? AUDIO_TELEMETRY_TIER_MAX : requested;
    while (tier != AUDIO_TELEMETRY_TIER_OFF && audio_telemetry_tier_size(tier) > att_payload) {
        tier--;
    }
    return tier;
}

/**
 * @brief Pack one frame at `tier` into `out`.
 *
 * @return bytes written, or 0 if the tier is invalid or `cap` is too small. The header
 *         records the tier actually written, so a consumer never has to infer it from the
 *         length (and a future tier can be added without breaking the parse).
 */
static inline size_t audio_telemetry_pack(const struct audio_telemetry_frame *f, uint8_t tier,
                                          uint8_t *out, size_t cap) {
    const size_t size = audio_telemetry_tier_size(tier);
    if (size == 0 || out == NULL || f == NULL || cap < size) {
        return 0;
    }

    memset(out, 0, size);

    out[0] = (uint8_t)((AUDIO_TELEMETRY_VERSION << 4) | (tier & 0x0F));

    uint8_t flags = 0;
    if (f->silent)
        flags |= AUDIO_TELEMETRY_FLAG_SILENT;
    if (f->clipped)
        flags |= AUDIO_TELEMETRY_FLAG_CLIPPED;
    if (f->agc_frozen)
        flags |= AUDIO_TELEMETRY_FLAG_AGC_FROZEN;
    if (f->threshold_mode)
        flags |= AUDIO_TELEMETRY_FLAG_THRESHOLD_MODE;
    flags |= (uint8_t)((f->beat_mask & 0x0F) << AUDIO_TELEMETRY_BEAT_SHIFT);
    out[1] = flags;

    out[2] = (uint8_t)(f->seq & 0xFF); /* little-endian, matching every other characteristic */
    out[3] = (uint8_t)(f->seq >> 8);
    out[4] = f->dropped;
    out[5] = (uint8_t)f->gain_steps; /* lossless: the register step IS 0.5 dB */
    out[6] = audio_telemetry_q_log(f->rms_input_referred);
    out[7] = audio_telemetry_q_log(f->rms_instant);
    out[8] = audio_telemetry_q_log(f->peak);
    out[9] = audio_telemetry_q_log(f->noise_floor);
    out[10] = f->clip_count;
    out[11] = f->frames_since_step;

    for (int b = 0; b < AUDIO_NUM_BANDS; b++) {
        out[12 + b] = audio_telemetry_q_log(f->flux[b]);
        out[16 + b] = audio_telemetry_q_log(f->threshold[b]);
    }
    if (tier == AUDIO_TELEMETRY_TIER_METERS) {
        return size;
    }

    for (int b = 0; b < AUDIO_NUM_BANDS; b++) {
        out[20 + b] = audio_telemetry_q_log(f->mean[b]);
        out[24 + b] = audio_telemetry_q_log(f->sigma[b]);
    }
    if (tier == AUDIO_TELEMETRY_TIER_STATS) {
        return size;
    }

    for (int i = 0; i < AUDIO_NUM_DISPLAY_BUCKETS; i++) {
        out[28 + i] = audio_telemetry_q_log(f->buckets[i]);
    }
    return size;
}

/** @brief Version nibble of a packed frame. */
static inline uint8_t audio_telemetry_packed_version(const uint8_t *buf) {
    return (uint8_t)(buf[0] >> 4);
}

/** @brief Tier nibble of a packed frame — what was ACTUALLY sent, not what was asked for. */
static inline uint8_t audio_telemetry_packed_tier(const uint8_t *buf) {
    return (uint8_t)(buf[0] & 0x0F);
}

/**
 * @brief Unpack into `f`, filling only the fields the tier carries and zeroing the rest.
 *
 * @return true on success; false on a version mismatch, an unknown tier, or a buffer
 *         shorter than the tier claims. Never partially fills on failure.
 */
static inline bool audio_telemetry_unpack(const uint8_t *buf, size_t len,
                                          struct audio_telemetry_frame *f) {
    if (buf == NULL || f == NULL || len < 1) {
        return false;
    }
    if (audio_telemetry_packed_version(buf) != AUDIO_TELEMETRY_VERSION) {
        return false;
    }

    const uint8_t tier = audio_telemetry_packed_tier(buf);
    const size_t size = audio_telemetry_tier_size(tier);
    if (size == 0 || len < size) {
        return false;
    }

    memset(f, 0, sizeof(*f));

    const uint8_t flags = buf[1];
    f->silent = (flags & AUDIO_TELEMETRY_FLAG_SILENT) != 0;
    f->clipped = (flags & AUDIO_TELEMETRY_FLAG_CLIPPED) != 0;
    f->agc_frozen = (flags & AUDIO_TELEMETRY_FLAG_AGC_FROZEN) != 0;
    f->threshold_mode = (flags & AUDIO_TELEMETRY_FLAG_THRESHOLD_MODE) ? 1 : 0;
    f->beat_mask = (uint8_t)((flags >> AUDIO_TELEMETRY_BEAT_SHIFT) & 0x0F);

    f->seq = (uint16_t)(buf[2] | ((uint16_t)buf[3] << 8));
    f->dropped = buf[4];
    f->gain_steps = (int8_t)buf[5];
    f->rms_input_referred = audio_telemetry_dq_log(buf[6]);
    f->rms_instant = audio_telemetry_dq_log(buf[7]);
    f->peak = audio_telemetry_dq_log(buf[8]);
    f->noise_floor = audio_telemetry_dq_log(buf[9]);
    f->clip_count = buf[10];
    f->frames_since_step = buf[11];

    for (int b = 0; b < AUDIO_NUM_BANDS; b++) {
        f->flux[b] = audio_telemetry_dq_log(buf[12 + b]);
        f->threshold[b] = audio_telemetry_dq_log(buf[16 + b]);
    }
    if (tier == AUDIO_TELEMETRY_TIER_METERS) {
        return true;
    }

    for (int b = 0; b < AUDIO_NUM_BANDS; b++) {
        f->mean[b] = audio_telemetry_dq_log(buf[20 + b]);
        f->sigma[b] = audio_telemetry_dq_log(buf[24 + b]);
    }
    if (tier == AUDIO_TELEMETRY_TIER_STATS) {
        return true;
    }

    for (int i = 0; i < AUDIO_NUM_DISPLAY_BUCKETS; i++) {
        f->buckets[i] = audio_telemetry_dq_log(buf[28 + i]);
    }
    return true;
}
