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

/* Byte offsets. Named once and used by BOTH pack and unpack, with the tier sizes DERIVED
 * from them rather than written out independently.
 *
 * The in-repo round-trip test would catch a transposition made in one direction only, but
 * the app decodes this format independently from the doc comment above — an external
 * decoder has nothing to round-trip against. Naming the offsets gives that doc exact
 * symbols to cite and makes the layout single-definition on this side. */
#define AUDIO_TELEMETRY_OFF_HEADER 0
#define AUDIO_TELEMETRY_OFF_FLAGS 1
#define AUDIO_TELEMETRY_OFF_SEQ 2       /* uint16 LE */
#define AUDIO_TELEMETRY_OFF_DROPPED 4
#define AUDIO_TELEMETRY_OFF_GAIN 5      /* int8, 0.5 dB steps from the park */
#define AUDIO_TELEMETRY_OFF_RMS_IN 6
#define AUDIO_TELEMETRY_OFF_RMS_INST 7
#define AUDIO_TELEMETRY_OFF_PEAK 8
#define AUDIO_TELEMETRY_OFF_NOISE 9
#define AUDIO_TELEMETRY_OFF_CLIPS 10
#define AUDIO_TELEMETRY_OFF_SINCE_STEP 11
#define AUDIO_TELEMETRY_OFF_FLUX 12
#define AUDIO_TELEMETRY_OFF_THRESHOLD (AUDIO_TELEMETRY_OFF_FLUX + AUDIO_NUM_BANDS)
#define AUDIO_TELEMETRY_OFF_MEAN (AUDIO_TELEMETRY_OFF_THRESHOLD + AUDIO_NUM_BANDS)
#define AUDIO_TELEMETRY_OFF_SIGMA (AUDIO_TELEMETRY_OFF_MEAN + AUDIO_NUM_BANDS)
#define AUDIO_TELEMETRY_OFF_BUCKETS (AUDIO_TELEMETRY_OFF_SIGMA + AUDIO_NUM_BANDS)

#define AUDIO_TELEMETRY_SIZE_METERS (AUDIO_TELEMETRY_OFF_THRESHOLD + AUDIO_NUM_BANDS)
#define AUDIO_TELEMETRY_SIZE_STATS (AUDIO_TELEMETRY_OFF_BUCKETS)
#define AUDIO_TELEMETRY_SIZE_SPECTRUM \
    (AUDIO_TELEMETRY_OFF_BUCKETS + AUDIO_NUM_DISPLAY_BUCKETS)

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
/* The sizes are now DERIVED from the offsets, so asserting one against the other would be
 * tautological. Pin the wire contract instead: these are the numbers an external decoder
 * hard-codes, and they must not move without a version bump. */
static_assert(AUDIO_TELEMETRY_SIZE_METERS == 20, "meters tier is a wire constant");
static_assert(AUDIO_TELEMETRY_SIZE_STATS == 28, "stats tier is a wire constant");
static_assert(AUDIO_TELEMETRY_SIZE_SPECTRUM == 48, "spectrum tier is a wire constant");
static_assert(AUDIO_TELEMETRY_OFF_FLUX == 12, "flux offset is a wire constant");
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
    /* Wrapping count of ticks the DSP had no new frame for. NOT a transport counter — a
     * failed notify is invisible to the firmware (notify() returns no status). */
    uint8_t dropped;
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
 * Not powf(): float pow is compiled out firmware-wide, which is why
 * audio_dsp_gain_amplitude_ratio() exists in the form it does.
 */
static inline float audio_telemetry_dq_log(uint8_t q) {
    if (q == 0) {
        return 0.0f;
    }
    /* Delegates to the ladder audio_dsp.h calls "the single authoritative encoding" of
     * 0.5 dB per step, rather than re-deriving it. One quantiser step and one PDM gain
     * register step are the same 0.5 dB, so q-200 IS a step count — for q >= 1 this is
     * bit-for-bit what a local copy of the constants would produce. Acknowledging a
     * duplication does not prevent it drifting; calling the function does, and a precision
     * fix there now reaches the meters instead of silently leaving them a few percent off. */
    return audio_dsp_gain_amplitude_ratio((int)q - 200);
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

    out[AUDIO_TELEMETRY_OFF_HEADER] = (uint8_t)((AUDIO_TELEMETRY_VERSION << 4) | (tier & 0x0F));

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
    out[AUDIO_TELEMETRY_OFF_FLAGS] = flags;

    /* little-endian, matching every other characteristic on this device */
    out[AUDIO_TELEMETRY_OFF_SEQ] = (uint8_t)(f->seq & 0xFF);
    out[AUDIO_TELEMETRY_OFF_SEQ + 1] = (uint8_t)(f->seq >> 8);
    out[AUDIO_TELEMETRY_OFF_DROPPED] = f->dropped;
    out[AUDIO_TELEMETRY_OFF_GAIN] = (uint8_t)f->gain_steps; /* lossless: the step IS 0.5 dB */
    out[AUDIO_TELEMETRY_OFF_RMS_IN] = audio_telemetry_q_log(f->rms_input_referred);
    out[AUDIO_TELEMETRY_OFF_RMS_INST] = audio_telemetry_q_log(f->rms_instant);
    out[AUDIO_TELEMETRY_OFF_PEAK] = audio_telemetry_q_log(f->peak);
    out[AUDIO_TELEMETRY_OFF_NOISE] = audio_telemetry_q_log(f->noise_floor);
    out[AUDIO_TELEMETRY_OFF_CLIPS] = f->clip_count;
    out[AUDIO_TELEMETRY_OFF_SINCE_STEP] = f->frames_since_step;

    for (int b = 0; b < AUDIO_NUM_BANDS; b++) {
        out[AUDIO_TELEMETRY_OFF_FLUX + b] = audio_telemetry_q_log(f->flux[b]);
        out[AUDIO_TELEMETRY_OFF_THRESHOLD + b] = audio_telemetry_q_log(f->threshold[b]);
    }
    if (tier == AUDIO_TELEMETRY_TIER_METERS) {
        return size;
    }

    for (int b = 0; b < AUDIO_NUM_BANDS; b++) {
        out[AUDIO_TELEMETRY_OFF_MEAN + b] = audio_telemetry_q_log(f->mean[b]);
        out[AUDIO_TELEMETRY_OFF_SIGMA + b] = audio_telemetry_q_log(f->sigma[b]);
    }
    if (tier == AUDIO_TELEMETRY_TIER_STATS) {
        return size;
    }

    for (int i = 0; i < AUDIO_NUM_DISPLAY_BUCKETS; i++) {
        out[AUDIO_TELEMETRY_OFF_BUCKETS + i] = audio_telemetry_q_log(f->buckets[i]);
    }
    return size;
}

/** @brief Version nibble of a packed frame. */
static inline uint8_t audio_telemetry_packed_version(const uint8_t *buf) {
    return (uint8_t)(buf[AUDIO_TELEMETRY_OFF_HEADER] >> 4);
}

/** @brief Tier nibble of a packed frame — what was ACTUALLY sent, not what was asked for. */
static inline uint8_t audio_telemetry_packed_tier(const uint8_t *buf) {
    return (uint8_t)(buf[AUDIO_TELEMETRY_OFF_HEADER] & 0x0F);
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

    const uint8_t flags = buf[AUDIO_TELEMETRY_OFF_FLAGS];
    f->silent = (flags & AUDIO_TELEMETRY_FLAG_SILENT) != 0;
    f->clipped = (flags & AUDIO_TELEMETRY_FLAG_CLIPPED) != 0;
    f->agc_frozen = (flags & AUDIO_TELEMETRY_FLAG_AGC_FROZEN) != 0;
    f->threshold_mode = (flags & AUDIO_TELEMETRY_FLAG_THRESHOLD_MODE) ? 1 : 0;
    f->beat_mask = (uint8_t)((flags >> AUDIO_TELEMETRY_BEAT_SHIFT) & 0x0F);

    f->seq = (uint16_t)(buf[AUDIO_TELEMETRY_OFF_SEQ] |
                        ((uint16_t)buf[AUDIO_TELEMETRY_OFF_SEQ + 1] << 8));
    f->dropped = buf[AUDIO_TELEMETRY_OFF_DROPPED];
    f->gain_steps = (int8_t)buf[AUDIO_TELEMETRY_OFF_GAIN];
    f->rms_input_referred = audio_telemetry_dq_log(buf[AUDIO_TELEMETRY_OFF_RMS_IN]);
    f->rms_instant = audio_telemetry_dq_log(buf[AUDIO_TELEMETRY_OFF_RMS_INST]);
    f->peak = audio_telemetry_dq_log(buf[AUDIO_TELEMETRY_OFF_PEAK]);
    f->noise_floor = audio_telemetry_dq_log(buf[AUDIO_TELEMETRY_OFF_NOISE]);
    f->clip_count = buf[AUDIO_TELEMETRY_OFF_CLIPS];
    f->frames_since_step = buf[AUDIO_TELEMETRY_OFF_SINCE_STEP];

    for (int b = 0; b < AUDIO_NUM_BANDS; b++) {
        f->flux[b] = audio_telemetry_dq_log(buf[AUDIO_TELEMETRY_OFF_FLUX + b]);
        f->threshold[b] = audio_telemetry_dq_log(buf[AUDIO_TELEMETRY_OFF_THRESHOLD + b]);
    }
    if (tier == AUDIO_TELEMETRY_TIER_METERS) {
        return true;
    }

    for (int b = 0; b < AUDIO_NUM_BANDS; b++) {
        f->mean[b] = audio_telemetry_dq_log(buf[AUDIO_TELEMETRY_OFF_MEAN + b]);
        f->sigma[b] = audio_telemetry_dq_log(buf[AUDIO_TELEMETRY_OFF_SIGMA + b]);
    }
    if (tier == AUDIO_TELEMETRY_TIER_STATS) {
        return true;
    }

    for (int i = 0; i < AUDIO_NUM_DISPLAY_BUCKETS; i++) {
        f->buckets[i] = audio_telemetry_dq_log(buf[AUDIO_TELEMETRY_OFF_BUCKETS + i]);
    }
    return true;
}
