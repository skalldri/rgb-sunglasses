#pragma once
/* Control-word decoding for the telemetry service — the pure half of onWriteChecked().
 *
 * Split out for the same reason as audio_frame_fold, extension_tick_budget and
 * conn_param_governor_core: this is the one piece of the service that VALIDATES untrusted
 * input, and validation nobody has watched reject something is the least trustworthy code
 * to leave untested. The service file itself needs the BT stack (bt_gatt_get_mtu,
 * bt_conn_foreach, a registered GATT server), so testing the decode through it would mean
 * standing up Bluetooth on native_sim to check some shifts and clamps.
 *
 * Header-only and dependency-free so the ztest suite compiles it directly.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include "audio_telemetry_codec.h"

/* Control word layout:
 *
 *   [ 7:0]  tier     0 = off, 1 = meters, 2 = +raw stats, 3 = +spectrum
 *   [15:8]  rate_hz  0 => caller's default; clamped to [1, kMaxRateHz]
 *   [23:16] hold_s   0 => caller's default; clamped to [min_hold_s, 255]
 *   [31:24] reserved MUST be zero
 */
#define AUDIO_TELEMETRY_CTRL_TIER_MASK 0x000000FFu
#define AUDIO_TELEMETRY_CTRL_RATE_SHIFT 8
#define AUDIO_TELEMETRY_CTRL_HOLD_SHIFT 16
#define AUDIO_TELEMETRY_CTRL_RESERVED_MASK 0xFF000000u

/* The analysis rate itself. Asking for more frames per second than the DSP produces is a
 * request that cannot be honoured, so it is clamped rather than believed. */
#define AUDIO_TELEMETRY_CTRL_MAX_RATE_HZ 32

/* The hold field is 8 bits wide, so 255 s is the longest hold the wire format can express.
 * This is NOT enforced at runtime and must not be: `hold` is masked out of that same 8-bit
 * field, so a clamp against it could never fire, and an unfireable clamp reads like a
 * guarantee while proving nothing. CONFIG_APP_AUDIO_TELEMETRY_MAX_HOLD_S carries `range 5 255`
 * so a configured maximum above the field width is rejected at build time instead — the one
 * place the check can actually fail. The constant stays for callers that need to state the
 * format's ceiling. */
#define AUDIO_TELEMETRY_CTRL_MAX_HOLD_S 255

struct audio_telemetry_control {
    bool valid;      /* false => reject the ATT write; every field below is meaningless */
    int error;       /* negative errno to return when !valid */
    uint8_t tier;    /* AUDIO_TELEMETRY_TIER_* */
    uint8_t rate_hz; /* resolved and clamped; meaningless when tier == OFF */
    uint16_t hold_s; /* resolved and clamped; meaningless when tier == OFF */
};

/**
 * @brief Decode, default and clamp one control word.
 *
 * @param control       the raw 32-bit value written
 * @param default_rate  rate to use when the field is 0 (Kconfig)
 * @param default_hold  hold to use when the field is 0
 * @param min_hold_s    lower clamp for hold
 * @param max_hold_s    upper clamp for hold (itself capped at the 8-bit field width)
 */
static inline struct audio_telemetry_control audio_telemetry_control_parse(
    uint32_t control, uint8_t default_rate, uint16_t default_hold, uint16_t min_hold_s,
    uint16_t max_hold_s) {
    struct audio_telemetry_control out = {false, 0, AUDIO_TELEMETRY_TIER_OFF, 0, 0};

    if ((control & AUDIO_TELEMETRY_CTRL_RESERVED_MASK) != 0) {
        /* Reserved bits are how a future field gets added. Accepting them now would make
         * that field unusable — an old build would silently ignore what a new app meant. */
        out.error = -EINVAL;
        return out;
    }

    const uint8_t tier = (uint8_t)(control & AUDIO_TELEMETRY_CTRL_TIER_MASK);
    if (tier > AUDIO_TELEMETRY_TIER_MAX) {
        out.error = -EINVAL;
        return out;
    }

    out.valid = true;
    out.tier = tier;
    if (tier == AUDIO_TELEMETRY_TIER_OFF) {
        return out; /* a stop needs no rate or hold */
    }

    uint8_t rate = (uint8_t)((control >> AUDIO_TELEMETRY_CTRL_RATE_SHIFT) & 0xFF);
    if (rate == 0) {
        rate = default_rate;
    }
    if (rate > AUDIO_TELEMETRY_CTRL_MAX_RATE_HZ) {
        rate = AUDIO_TELEMETRY_CTRL_MAX_RATE_HZ;
    }
    if (rate == 0) {
        rate = 1; /* a misconfigured default must not divide by zero downstream */
    }
    out.rate_hz = rate;

    uint16_t hold = (uint16_t)((control >> AUDIO_TELEMETRY_CTRL_HOLD_SHIFT) & 0xFF);
    if (hold == 0) {
        hold = default_hold;
    }
    if (hold < min_hold_s) {
        hold = min_hold_s;
    }
    if (hold > max_hold_s) {
        hold = max_hold_s;
    }
    out.hold_s = hold;

    return out;
}
