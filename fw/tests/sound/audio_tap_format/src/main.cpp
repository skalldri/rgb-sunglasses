/* Unit tests for the frame-dump wire format (fw/src/sound/audio_tap_format.h).
 *
 * That header is the single source of truth for field ORDER across four
 * producers/consumers that are compiled separately and can therefore drift
 * silently: the firmware's `sound dump`, the CONFIG_APP_AUDIO_DEBUG record_wav
 * sidecar, the capture path's combined <wav>.csv sidecar, the native_sim replay
 * app, plus the host decoders fw/tools/beat_lab/frames.py and
 * fw/sim/node/dline.ts. Both decoders reject any row that is not exactly 21 or
 * 41 fields, and neither firmware nor CI had anything asserting the producer
 * still emits those counts — a stray field would only show up as an unreadable
 * capture, after the recording session it ruined.
 *
 * The header is header-only and Zephyr-free, so this suite needs no fixture and
 * links nothing. */
#include <zephyr/ztest.h>

#include <stdlib.h>
#include <string.h>

#include "audio_tap_format.h"

namespace {

/* Longest D-line the format can emit, which is what sizes the firmware's line
 * buffers (AUDIO_CSV_LINE_MAX in sound.cpp, s_tap_line's 512 bytes). */
constexpr size_t kLineCap = 512;

size_t count_fields(const char *s) {
    size_t n = 1;
    for (; *s; s++) {
        if (*s == ',') {
            n++;
        }
    }
    return n;
}

/* Returns the nth comma-separated field, copied into `out`. */
void field(const char *s, size_t index, char *out, size_t out_cap) {
    for (size_t i = 0; i < index; i++) {
        s = strchr(s, ',');
        zassert_not_null(s, "field %u does not exist", (unsigned)index);
        s++;
    }
    const char *end = strchr(s, ',');
    size_t len = end ? (size_t)(end - s) : strlen(s);
    zassert_true(len < out_cap, "field %u too long", (unsigned)index);
    memcpy(out, s, len);
    out[len] = '\0';
}

float field_as_float(const char *s, size_t index) {
    char buf[32];
    field(s, index, buf, sizeof(buf));
    uint32_t bits = (uint32_t)strtoul(buf, nullptr, 16);
    float v;
    memcpy(&v, &bits, sizeof(v));
    return v;
}

/* A result with every slot set to a distinct, recognisable value, so a
 * mis-ordered field shows up as a wrong number rather than a plausible one. */
struct audio_analysis_result make_result() {
    struct audio_analysis_result r = {};
    r.seq = 12345;
    for (int b = 0; b < AUDIO_NUM_BANDS; b++) {
        r.band_energy[b] = 1.0f + (float)b;
        r.band_flux[b] = 10.0f + (float)b;
        r.band_mean[b] = 100.0f + (float)b;
        r.band_sigma[b] = 1000.0f + (float)b;
        r.beat[b] = false;
    }
    for (int d = 0; d < AUDIO_NUM_DISPLAY_BUCKETS; d++) {
        r.display_bucket_energy[d] = 0.5f + (float)d;
    }
    return r;
}

}  // namespace

/* The arity contract with frames.py and dline.ts. If this fails, every host
 * consumer will reject the capture, so bump those decoders in the same change. */
ZTEST(audio_tap_format, test_field_counts) {
    struct audio_analysis_result r = make_result();
    char buf[kLineCap];

    size_t n = audio_tap_format_frame(&r, 0.25f, 0x28, false, buf, sizeof(buf));
    zassert_equal(count_fields(buf), 21, "D-line without buckets must be 21 fields");
    zassert_equal(n, strlen(buf), "returned length must match the rendered string");

    n = audio_tap_format_frame(&r, 0.25f, 0x28, true, buf, sizeof(buf));
    zassert_equal(count_fields(buf), 41, "D-line with buckets must be 41 fields");
    zassert_equal(n, strlen(buf), "returned length must match the rendered string");
}

/* Field ORDER: rms, then all band energies, all fluxes, all means, all sigmas,
 * then the display buckets — grouped by statistic, not interleaved per band. */
ZTEST(audio_tap_format, test_field_order) {
    struct audio_analysis_result r = make_result();
    char buf[kLineCap];
    audio_tap_format_frame(&r, 0.25f, 0x28, true, buf, sizeof(buf));

    zassert_equal(field_as_float(buf, 4), 0.25f, "field 4 is rms");
    for (int b = 0; b < AUDIO_NUM_BANDS; b++) {
        zassert_equal(field_as_float(buf, 5 + b), r.band_energy[b], "band_energy[%d]", b);
        zassert_equal(field_as_float(buf, 5 + AUDIO_NUM_BANDS + b), r.band_flux[b],
                      "band_flux[%d]", b);
        zassert_equal(field_as_float(buf, 5 + 2 * AUDIO_NUM_BANDS + b), r.band_mean[b],
                      "band_mean[%d]", b);
        zassert_equal(field_as_float(buf, 5 + 3 * AUDIO_NUM_BANDS + b), r.band_sigma[b],
                      "band_sigma[%d]", b);
    }
    for (int d = 0; d < AUDIO_NUM_DISPLAY_BUCKETS; d++) {
        zassert_equal(field_as_float(buf, 5 + 4 * AUDIO_NUM_BANDS + d),
                      r.display_bucket_energy[d], "display_bucket_energy[%d]", d);
    }
}

/* The whole reason floats travel as hex: CONFIG_CBPRINTF_FP_SUPPORT is off
 * firmware-wide, and the device-vs-host replay gate compares to ~1e-7, so the
 * transport itself must lose nothing. */
ZTEST(audio_tap_format, test_floats_round_trip_bit_exactly) {
    const float awkward[] = {
        0.0f, -0.0f, 1.0f, -1.0f, 0.1f, 1e-30f, 3.4e38f, 1.0f / 3.0f, 2.0e-4f,
    };
    struct audio_analysis_result r = {};
    char buf[kLineCap];

    for (size_t i = 0; i < ARRAY_SIZE(awkward); i++) {
        r.band_energy[0] = awkward[i];
        audio_tap_format_frame(&r, awkward[i], 0x00, false, buf, sizeof(buf));
        uint32_t want, got;
        float rms = field_as_float(buf, 4);
        float e0 = field_as_float(buf, 5);
        memcpy(&want, &awkward[i], sizeof(want));
        memcpy(&got, &rms, sizeof(got));
        zassert_equal(want, got, "rms bit pattern changed for value %u", (unsigned)i);
        memcpy(&got, &e0, sizeof(got));
        zassert_equal(want, got, "band_energy bit pattern changed for value %u", (unsigned)i);
    }
}

/* Beats are packed one bit per band into a single hex nibble — which is also
 * why a fifth band (or a "silent" flag squeezed in here) would break the wire
 * format rather than just widening it. */
ZTEST(audio_tap_format, test_beatmask_packs_every_band) {
    struct audio_analysis_result r = {};
    char buf[kLineCap];
    char mask[8];

    for (int b = 0; b < AUDIO_NUM_BANDS; b++) {
        memset(r.beat, 0, sizeof(r.beat));
        r.beat[b] = true;
        audio_tap_format_frame(&r, 0.0f, 0x00, false, buf, sizeof(buf));
        field(buf, 3, mask, sizeof(mask));
        zassert_equal(strtoul(mask, nullptr, 16), 1u << b, "band %d must set bit %d", b, b);
    }

    for (int b = 0; b < AUDIO_NUM_BANDS; b++) {
        r.beat[b] = true;
    }
    audio_tap_format_frame(&r, 0.0f, 0x00, false, buf, sizeof(buf));
    field(buf, 3, mask, sizeof(mask));
    zassert_equal(strtoul(mask, nullptr, 16), (1u << AUDIO_NUM_BANDS) - 1u,
                  "all bands beating must set every bit");
    zassert_equal(strlen(mask), 1, "the beatmask must stay a single hex digit");
}

/* seq and gain are the two non-float fields. gain is fixed-width so a column
 * split on it stays aligned; seq is not, which is what makes the worst-case
 * line length below depend on it. */
ZTEST(audio_tap_format, test_seq_and_gain_fields) {
    struct audio_analysis_result r = {};
    r.seq = 7;
    char buf[kLineCap];
    char f[32];

    audio_tap_format_frame(&r, 0.0f, 0x5, false, buf, sizeof(buf));
    zassert_equal(strncmp(buf, "D,", 2), 0, "rows must be tagged D,");
    field(buf, 1, f, sizeof(f));
    zassert_equal(strtoul(f, nullptr, 10), 7, "seq is decimal");
    field(buf, 2, f, sizeof(f));
    zassert_str_equal(f, "05", "gain is two zero-padded hex digits");
}

/* The firmware formats rows straight into a batch buffer at a running offset,
 * so an under-sized line bound would corrupt the file rather than truncate one
 * row. This pins the worst case the buffers must hold: the widest seq, every
 * float at 8 hex digits, buckets included. */
ZTEST(audio_tap_format, test_worst_case_line_fits_the_firmware_buffers) {
    struct audio_analysis_result r = make_result();
    r.seq = UINT32_MAX;
    char buf[kLineCap];

    size_t n = audio_tap_format_frame(&r, 1.0f / 3.0f, 0xff, true, buf, sizeof(buf));
    zassert_equal(n, strlen(buf), "must not report a truncated length");
    /* +1 for the newline the sidecar writers append after the returned length. */
    zassert_true(n + 1 < kLineCap, "worst-case D-line (%u B) must fit a %u B line buffer",
                 (unsigned)n, (unsigned)kLineCap);
}

/* A too-small buffer must stay inside it. The helper clamps to cap-1 rather
 * than reporting what it would have written, which is what lets callers use the
 * return value as a write offset. */
ZTEST(audio_tap_format, test_small_buffer_does_not_overflow) {
    struct audio_analysis_result r = make_result();
    char guarded[64];
    memset(guarded, 0x7e, sizeof(guarded));

    const size_t cap = 32;
    size_t n = audio_tap_format_frame(&r, 0.25f, 0x28, true, guarded, cap);
    zassert_true(n < cap, "returned length must stay inside the buffer");
    for (size_t i = cap; i < sizeof(guarded); i++) {
        zassert_equal((unsigned char)guarded[i], 0x7e, "wrote past cap at byte %u", (unsigned)i);
    }
}

/* The #PARAMS header is what the replay harness reads to reproduce a capture,
 * so its keys are as much of a contract as the D-line's field order. */
ZTEST(audio_tap_format, test_params_line) {
    char buf[512];
    size_t n = audio_tap_format_params(/*gamma=*/1000.0f, /*alpha=*/0.3f, /*flux_floor=*/0.08f,
                                       /*refractory=*/5, /*agc_frozen=*/true, /*gain=*/0x3a,
                                       /*target_low=*/0.002f, /*target_high=*/0.05f,
                                       /*rate_limit=*/10, /*attack_frames=*/3,
                                       /*release_frames=*/15, /*noise_gate=*/0.0006f,
                                       /*sf_delta=*/0.10f, /*threshold_mode=*/1, buf,
                                       sizeof(buf));
    zassert_equal(n, strlen(buf), "returned length must match the rendered string");
    zassert_equal(strncmp(buf, "#PARAMS ", 8), 0, "params line must be tagged #PARAMS");

    /* Every key the decoders look for, with a value that is not the default so
     * a dropped argument cannot pass by coincidence. */
    zassert_not_null(strstr(buf, " refractory=5"), "refractory");
    zassert_not_null(strstr(buf, " agc_frozen=1"), "agc_frozen");
    zassert_not_null(strstr(buf, " gain=3a"), "gain is two zero-padded hex digits");
    zassert_not_null(strstr(buf, " rate_limit=10"), "rate_limit");
    zassert_not_null(strstr(buf, " attack=3"), "attack");
    zassert_not_null(strstr(buf, " release=15"), "release");
    zassert_not_null(strstr(buf, " mode=1"), "threshold mode");

    char expect[32];
    snprintf(expect, sizeof(expect), " alpha=%08x", audio_tap_f32_bits(0.3f));
    zassert_not_null(strstr(buf, expect), "alpha travels as a hex bit pattern");
    snprintf(expect, sizeof(expect), " gate=%08x", audio_tap_f32_bits(0.0006f));
    zassert_not_null(strstr(buf, expect), "noise gate travels as a hex bit pattern");
}

ZTEST_SUITE(audio_tap_format, NULL, NULL, NULL, NULL, NULL);
