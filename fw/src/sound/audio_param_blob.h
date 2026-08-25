#pragma once
/* Bulk "Audio Param Ranges" blob — every tunable's range, default, step, unit and enum
 * labels in one read.
 *
 * WHY A BLOB WHEN 0x2906 EXISTS. Both exist, and they are projections of the SAME table row
 * (kAudioParams), generated at compile time, so they cannot drift. The standard Valid Range
 * descriptor makes the device self-describing to any generic BLE tool; this blob is what the
 * companion app actually reads, because discovery already performs ~170 sequential GATT reads
 * (use-ble-connection.ts) and adding 14 more descriptor round-trips to the fast path is the
 * wrong trade. 0x2906 also covers only min/max: there is no standard descriptor for a step, a
 * default, an enum label set, or a log-vs-linear hint, and a slider needs all four.
 *
 * WHY NOT EXTEND THE SHARED METADATA BLOB. That blob is appended to EVERY service (~20, mostly
 * animations), so extending its entry format spends rodata across the whole image — and
 * CONFIG_APP_BT_METADATA_CHARACTERISTIC exists precisely because its duplicated strings once
 * overflowed an image slot. Worse, the app's parseMetadataBlob() returns null on ANY version
 * mismatch, so bumping to v2 would push every already-installed app onto the per-descriptor
 * fallback for every service. This carries its own version byte and leaves that one at 1.
 *
 * Wire format, little-endian throughout:
 *   [version: 1][entry_count: 1]
 *   per entry, in GATT declaration order:
 *     [type: 1]            0 = float32, 1 = uint32, 2 = enum (uint32 on the wire)
 *     [unit_len: 1][unit bytes]        ASCII, no NUL. Empty for a dimensionless parameter.
 *     [enum_len: 1][enum label bytes]  "\n"-separated, no NUL. Empty unless type == enum.
 *     [default: f32][min: f32][max: f32][step: f32]
 *
 * Values are float32 even for the integer types: they are exactly representable at these
 * magnitudes (frame counts under 256), and one numeric encoding means the app has one decoder
 * rather than a type-dependent branch that could silently read the wrong four bytes.
 */

#include <array>
#include <cstddef>
#include <cstdint>

#include "audio_param_table.h"

#define AUDIO_PARAM_BLOB_VERSION 1

constexpr size_t audioParamBlobEntrySize(const AudioParamSpec &p) {
    const size_t unitLen = audioParamStrLen(p.unit);
    const size_t enumLen = p.enumLabels != nullptr ? audioParamStrLen(p.enumLabels) : 0;
    return 1 /* type */ + 1 + unitLen + 1 + enumLen + 4 * 4;
}

constexpr size_t audioParamBlobSize() {
    size_t total = 2; /* version + count */
    for (size_t i = 0; i < kAudioParamCount; i++) {
        total += audioParamBlobEntrySize(kAudioParams[i]);
    }
    return total;
}

/* The app reads this in one go at MTU 247, or six ATT_READ_BLOB round-trips at MTU 23. Either
 * way it happens once, on screen focus — not per parameter and not per render. */
inline constexpr size_t kAudioParamBlobSize = audioParamBlobSize();

constexpr std::array<uint8_t, kAudioParamBlobSize> audioParamBlobBuild() {
    std::array<uint8_t, kAudioParamBlobSize> out{};
    size_t o = 0;
    out[o++] = AUDIO_PARAM_BLOB_VERSION;
    out[o++] = (uint8_t)kAudioParamCount;

    const auto putF32 = [&out, &o](float v) {
        const AudioParamRangeBytes pair = audioParamRangeBytesFromFloats(v, 0.0f);
        for (int k = 0; k < 4; k++) out[o++] = pair.b[k];
    };
    const auto putStr = [&out, &o](const char *s) {
        const size_t n = s != nullptr ? audioParamStrLen(s) : 0;
        out[o++] = (uint8_t)n;
        for (size_t k = 0; k < n; k++) out[o++] = (uint8_t)s[k];
    };

    for (size_t i = 0; i < kAudioParamCount; i++) {
        const AudioParamSpec &p = kAudioParams[i];
        out[o++] = (uint8_t)p.type;
        putStr(p.unit);
        putStr(p.enumLabels);
        putF32(p.def);
        putF32(p.min);
        putF32(p.max);
        putF32(p.step);
    }
    return out;
}

inline constexpr std::array<uint8_t, kAudioParamBlobSize> kAudioParamBlob = audioParamBlobBuild();

/* A blob whose declared length disagreed with its content would be read as truncated garbage
 * by every client, so pin the relationship rather than trusting two independent walks of the
 * table to agree. */
static_assert(kAudioParamBlob.size() == kAudioParamBlobSize, "blob size mismatch");
static_assert(kAudioParamBlob[0] == AUDIO_PARAM_BLOB_VERSION, "version byte must lead");
static_assert(kAudioParamBlob[1] == kAudioParamCount, "entry count must follow the version");
