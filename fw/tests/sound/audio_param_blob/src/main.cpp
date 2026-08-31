/* Tests for the Audio Param Ranges blob and the Valid Range (0x2906) descriptor bytes.
 *
 * The point of this suite is the CROSS-CHECK. The device now describes each parameter's range
 * twice — once in a standard descriptor for generic BLE tools, once in a bulk blob for the
 * companion app — and two descriptions of the same thing that are allowed to disagree
 * eventually will. Both are generated from kAudioParams, so this asserts the generation is
 * faithful in both directions rather than trusting that a shared source makes them equal.
 */

#include <zephyr/ztest.h>

#include <cstring>

#include "audio_param_blob.h"
#include "audio_param_table.h"

namespace {

float readF32(const uint8_t *p) {
    uint32_t bits = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
                    ((uint32_t)p[3] << 24);
    float out;
    memcpy(&out, &bits, sizeof(out));
    return out;
}

/* Walk the blob to the start of entry `index`, mirroring what the app's decoder does — a
 * hand-written walk rather than a shared helper, so a layout change has to break both. */
size_t entryOffset(size_t index) {
    size_t o = 2;
    for (size_t i = 0; i < index; i++) {
        o += 1;                          /* type */
        o += 1 + kAudioParamBlob[o];     /* unit: o points AT the length byte here */
        o += 1 + kAudioParamBlob[o];     /* enum labels, same shape */
        o += 16;                         /* def/min/max/step */
    }
    return o;
}

} // namespace

ZTEST_SUITE(audio_param_blob, nullptr, nullptr, nullptr, nullptr, nullptr);

ZTEST(audio_param_blob, test_header) {
    zassert_equal(kAudioParamBlob[0], AUDIO_PARAM_BLOB_VERSION);
    zassert_equal(kAudioParamBlob[1], kAudioParamCount);
    zassert_true(kAudioParamBlobSize > 2, "blob carries no entries");
}

ZTEST(audio_param_blob, test_every_entry_round_trips) {
    for (size_t i = 0; i < kAudioParamCount; i++) {
        const AudioParamSpec &p = kAudioParams[i];
        size_t o = entryOffset(i);

        zassert_equal(kAudioParamBlob[o], (uint8_t)p.type, "param %u: wrong type", (unsigned)i);
        o++;

        const uint8_t unitLen = kAudioParamBlob[o++];
        zassert_equal(unitLen, audioParamStrLen(p.unit), "param %u: unit length", (unsigned)i);
        for (uint8_t k = 0; k < unitLen; k++) {
            zassert_equal(kAudioParamBlob[o + k], (uint8_t)p.unit[k], "param %u: unit byte %u",
                          (unsigned)i, k);
        }
        o += unitLen;

        const uint8_t enumLen = kAudioParamBlob[o++];
        const size_t expectEnum = p.enumLabels != nullptr ? audioParamStrLen(p.enumLabels) : 0;
        zassert_equal(enumLen, expectEnum, "param %u: enum length", (unsigned)i);
        o += enumLen;

        zassert_within(readF32(&kAudioParamBlob[o]), p.def, 1e-9f, "param %u: default",
                       (unsigned)i);
        zassert_within(readF32(&kAudioParamBlob[o + 4]), p.min, 1e-9f, "param %u: min",
                       (unsigned)i);
        zassert_within(readF32(&kAudioParamBlob[o + 8]), p.max, 1e-9f, "param %u: max",
                       (unsigned)i);
        zassert_within(readF32(&kAudioParamBlob[o + 12]), p.step, 1e-9f, "param %u: step",
                       (unsigned)i);
    }
}

ZTEST(audio_param_blob, test_walk_consumes_the_whole_blob) {
    /* If the walk ends anywhere but the end, either the size computation or the writer is
     * wrong — and a blob with trailing slack would decode as a truncated final entry rather
     * than failing loudly. */
    size_t o = entryOffset(kAudioParamCount);
    zassert_equal(o, kAudioParamBlobSize, "walk ended at %u of %u", (unsigned)o,
                  (unsigned)kAudioParamBlobSize);
}

ZTEST(audio_param_blob, test_enum_labels_only_on_enum_params) {
    for (size_t i = 0; i < kAudioParamCount; i++) {
        size_t o = entryOffset(i);
        const uint8_t type = kAudioParamBlob[o++];
        o += 1 + kAudioParamBlob[o];
        const uint8_t enumLen = kAudioParamBlob[o];
        if (type == (uint8_t)AudioParamType::ENUM) {
            zassert_true(enumLen > 0, "param %u is an enum with no labels", (unsigned)i);
        } else {
            zassert_equal(enumLen, 0, "param %u is not an enum but carries labels", (unsigned)i);
        }
    }
}

/* ── the cross-check ── */

ZTEST(audio_param_blob, test_descriptor_bytes_agree_with_the_blob) {
    /* Valid Range 0x2906 is "lower inclusive then upper inclusive, in the characteristic's own
     * CPF format". For the float params that is two LE float32s, which must be the same two
     * numbers the blob carries as min and max. */
    const AudioParamRangeBytes fluxGamma = audioParamRangeBytesF<kAudioParamFluxGamma>();
    size_t o = entryOffset(kAudioParamFluxGamma);
    o++;                                  /* type */
    o += 1 + kAudioParamBlob[o];          /* unit */
    o += 1 + kAudioParamBlob[o];          /* enum */
    o += 4;                               /* skip default; min follows */

    zassert_within(readF32(fluxGamma.b), readF32(&kAudioParamBlob[o]), 1e-9f,
                   "descriptor min disagrees with blob min");
    zassert_within(readF32(fluxGamma.b + 4), readF32(&kAudioParamBlob[o + 4]), 1e-9f,
                   "descriptor max disagrees with blob max");
}

ZTEST(audio_param_blob, test_integer_descriptor_uses_integer_encoding) {
    /* A uint32 characteristic's Valid Range must be two LE uint32s, NOT two floats. Emitting
     * float bytes here would render as a wildly wrong range in any tool that honours the CPF
     * format — and would look perfectly plausible in a hex dump. */
    const AudioParamRangeBytes r = audioParamRangeBytesU<kAudioParamBeatRefractoryFrames>();
    const uint32_t lo = (uint32_t)r.b[0] | ((uint32_t)r.b[1] << 8) | ((uint32_t)r.b[2] << 16) |
                        ((uint32_t)r.b[3] << 24);
    const uint32_t hi = (uint32_t)r.b[4] | ((uint32_t)r.b[5] << 8) | ((uint32_t)r.b[6] << 16) |
                        ((uint32_t)r.b[7] << 24);
    zassert_equal(lo, (uint32_t)kAudioParams[kAudioParamBeatRefractoryFrames].min);
    zassert_equal(hi, (uint32_t)kAudioParams[kAudioParamBeatRefractoryFrames].max);
    zassert_true(hi > lo, "refractory range is inverted or empty");
}

ZTEST(audio_param_blob, test_every_float_descriptor_matches_its_table_row) {
    /* Spot-checking one parameter would miss a per-parameter mistake, which is the only kind
     * a generated table can realistically make. */
    const AudioParamRangeBytes all[] = {
        audioParamRangeBytesF<kAudioParamFluxGamma>(),
        audioParamRangeBytesF<kAudioParamBeatFluxFloor>(),
        audioParamRangeBytesF<kAudioParamBeatAlpha>(),
        audioParamRangeBytesF<kAudioParamAgcTargetLow>(),
        audioParamRangeBytesF<kAudioParamAgcTargetHigh>(),
        audioParamRangeBytesF<kAudioParamFftSmoothingCoeff>(),
        audioParamRangeBytesF<kAudioParamFftEnergyScale>(),
        audioParamRangeBytesF<kAudioParamNoiseGateRms>(),
        audioParamRangeBytesF<kAudioParamSfDelta>(),
    };
    const size_t idx[] = {
        kAudioParamFluxGamma,       kAudioParamBeatFluxFloor,     kAudioParamBeatAlpha,
        kAudioParamAgcTargetLow,    kAudioParamAgcTargetHigh,     kAudioParamFftSmoothingCoeff,
        kAudioParamFftEnergyScale,  kAudioParamNoiseGateRms,      kAudioParamSfDelta,
    };
    for (size_t k = 0; k < ARRAY_SIZE(all); k++) {
        zassert_within(readF32(all[k].b), kAudioParams[idx[k]].min, 1e-9f,
                       "param %u descriptor min", (unsigned)idx[k]);
        zassert_within(readF32(all[k].b + 4), kAudioParams[idx[k]].max, 1e-9f,
                       "param %u descriptor max", (unsigned)idx[k]);
    }
}

ZTEST(audio_param_blob, test_blob_fits_a_long_read) {
    /* Not a hard protocol limit, but a blob that outgrew a handful of ATT_READ_BLOB round
     * trips would make screen focus noticeably slow on a degraded link. 512 is the maximum
     * attribute value length GATT permits. */
    zassert_true(kAudioParamBlobSize <= 512, "blob %u exceeds the maximum attribute length",
                 (unsigned)kAudioParamBlobSize);
}
