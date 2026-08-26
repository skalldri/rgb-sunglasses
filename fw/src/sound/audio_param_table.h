#pragma once

/**
 * @file audio_param_table.h
 * @brief Single source of truth for the 14 tunable audio parameters.
 *
 * Before this table existed the default value and the clamp range of every audio
 * parameter were written out FOUR times, with no cross-check:
 *
 *   1. DefaultAudioDspConfigProvider  (audio_dsp.cpp)   — beat-detection params
 *   2. DefaultAgcConfigProvider       (sound.cpp)       — AGC params
 *   3. AudioConfig                    (audio_config.cpp) — the BT/settings-backed impl
 *   4. EnvConfigProvider/EnvAgcProvider (tests/sound/audio_dsp_replay/src/main.cpp)
 *
 * They had already diverged in shape (the replay harness used fminf/fmaxf and a
 * bare `v > 255 ? 255 : v` rather than std::clamp), and the targetHigh
 * 0.001 -> 0.02 migration had to be hand-propagated to all four. That mattered
 * more than it looks: the offline replay harness is what tuning decisions are
 * made with, so a clamp that disagrees with the device can report an optimum
 * the hardware cannot reach.
 *
 * Everything here is constexpr, Zephyr-free, BT-free and free of project
 * headers, so all four consumers (including the native_sim replay app, which
 * links the host libc) can include it unchanged.
 *
 * ORDER IS LOAD-BEARING: kAudioParams is in GATT declaration order, which is the
 * order BtGattServer assigns positional characteristic UUIDs in
 * (composeAutoCharacteristicUuid, bt_service_cpp.h). Appending is safe; inserting
 * or reordering silently renumbers every characteristic after the insertion point
 * and breaks every installed companion app. Append only.
 */

#include <bit>
#include <cstddef>
#include <cstdint>

enum class AudioParamType : uint8_t {
    F32 = 0,
    U32 = 1,
    ENUM = 2,
};

/**
 * @brief Metadata for one tunable audio parameter.
 *
 * `def`/`min`/`max`/`step` are held as float for every type; the U32/ENUM
 * accessors cast. Every integer bound in this table (0, 1, 20, 100, 255) is
 * exactly representable as a float, as is every float bound, so the cast is
 * lossless — audioParamTableSelfCheck() asserts that.
 *
 * `unit` is a display hint for the companion app, NOT a Bluetooth SIG unit UUID.
 * Almost every parameter here is genuinely dimensionless; the four frame counts
 * are in units of one 32 ms analysis frame, and converting them to milliseconds
 * is deliberately left to the app (the wire value is frames, so labelling the
 * characteristic itself with a time unit would make a generic BLE browser
 * display a wrong number).
 */
struct AudioParamSpec {
    const char *key;   /* Zephyr settings key, verbatim */
    const char *label; /* GATT CUD label, verbatim */
    AudioParamType type;
    float def;
    float min;
    float max;
    float step;             /* UI increment hint; > 0 always */
    const char *unit;       /* "" when dimensionless */
    const char *enumLabels; /* "\n"-separated; nullptr unless type == ENUM */
};

/* Index constants. These name positions in kAudioParams AND, identically, the
 * positional GATT characteristic index within the Audio Analysis Config service. */
enum : size_t {
    kAudioParamFluxGamma = 0,
    kAudioParamBeatFluxFloor,
    kAudioParamBeatAlpha,
    kAudioParamBeatRefractoryFrames,
    kAudioParamAgcTargetLow,
    kAudioParamAgcTargetHigh,
    kAudioParamAgcRateLimitFrames,
    kAudioParamFftSmoothingCoeff,
    kAudioParamFftEnergyScale,
    kAudioParamAgcAttackFrames,
    kAudioParamAgcReleaseFrames,
    kAudioParamNoiseGateRms,
    kAudioParamSfDelta,
    kAudioParamThresholdMode,
    kAudioParamCount,
};

/* One analysis frame. Every *Frames parameter below is in these units.
 * Mirrors BLOCK_CAPTURE_TIME_MS in sound.h; audio_config.cpp static_asserts they agree. */
inline constexpr uint32_t kAudioParamFrameMs = 32;

inline constexpr AudioParamSpec kAudioParams[] = {
    /* --- Beat detection ------------------------------------------------- */

    /* Log-compression factor in flux = log1p(gamma*E) - log1p(gamma*E_prev).
     * Equalises sensitivity across quiet and loud passages. */
    {"audio/flux_gamma", "Flux Gamma", AudioParamType::F32, 1000.0f, 1.0f, 100000.0f, 1.0f, "",
     nullptr},

    /* Absolute flux floor a beat must exceed, independent of the adaptive threshold.
     *
     * Retuned 0.005 -> 0.08 (issue #264, post-Phase-3). The floor was measured as
     * PROVABLY INERT at the old operating point — identical fire counts across
     * 0.005..0.105 — and that stopped being true when Phase 3 dropped alpha
     * 3.5 -> 0.3. A much lower adaptive threshold lets small noise-flux events
     * through, and an absolute floor is exactly the right tool against them: it is
     * scale-fixed, so it bites on quiet-room noise without touching real music,
     * whose band-0 onset flux is > 1.0.
     *
     * Measured over the corpus (with the gate at its new 0.0006): raising the floor
     * to 0.08 cut quiet-room beats from 4 to 1 per 40 s with ZERO change to any music
     * clip's F-score. Above 0.08 it starts clipping real beats (worst-clip music F
     * 0.291 -> 0.278 at 0.12), so this is the peak, not an arbitrary safe number. */
    {"audio/beat_flux_floor", "Beat Flux Floor", AudioParamType::F32, 0.08f, 0.0f, 1.0f, 0.005f, "",
     nullptr},

    /* Mode 0 only: threshold = mean + alpha*sigma over the 32-frame (~1 s) flux history.
     *
     * Retuned 3.5 -> 0.3 in Phase 3 (issue #264). 3.5 was never measured — it mutes
     * the detector on steady music, because the beats sit in the flux history and
     * inflate sigma.
     *
     * This value is shared by ALL FOUR bands (read once per frame, not per band), so
     * it was validated on all four rather than on band 0 alone. F at alpha=0.3 vs each
     * (clip, band) pair's own optimum, over the 3-clip corpus, gives a max regret of
     * 0.036 — lower than alpha=0.2 (0.042) or 0.5 (0.079), so 0.3 is the best single
     * value for the whole bank, not just for bass. Per-band F at 0.3
     * (base60/loud30/newbase):
     *   band 0  0.294 / 0.289 / 0.352     band 1  0.222 / 0.201 / 0.277
     *   band 2  0.199 / 0.276 / 0.187     band 3  0.175 / 0.108 / 0.250
     * versus 0.014 / 0.000 / 0.129 (band 0) at the old 3.5 — every band improves.
     * Firing stays clear of saturation: 3.3-4.0 fires/s against the ~5.2/s ceiling the
     * 5-frame refractory imposes.
     *
     * A per-band alpha would buy at most 0.036 F and add four tuning knobs; not worth
     * it until the Phase 5 beat-grid work changes the picture. */
    {"audio/beat_alpha", "Beat Alpha", AudioParamType::F32, 0.3f, 0.1f, 20.0f, 0.05f, "", nullptr},

    /* Per-band minimum gap between beats. Max 255 is a hard limit, not a taste
     * judgement: the per-band refractory counter in audio_dsp.cpp is a uint8_t. */
    {"audio/beat_refractory_frames", "Beat Refractory Frames", AudioParamType::U32, 5.0f, 0.0f,
     255.0f, 1.0f, "frames", nullptr},

    /* --- AGC ------------------------------------------------------------- */

    /* Release path: SMOOTHED (1 s) RMS below this for releaseFrames -> +1 gain step.
     *
     * Derived from the ABGT 250 baseline captures (Phase 2 PR): release creep stops
     * where mic self-noise meets it (~+11 dB) instead of ramping to +20 dB; music
     * smoothed RMS at the converged gain (median 0.0045) sits well above it -> 0 steps
     * in 60 s of music (was 22). */
    {"audio/agc_target_low", "AGC Target Low", AudioParamType::F32, 0.002f, 0.001f, 0.1f, 0.0005f,
     "", nullptr},

    /* Attack path: INSTANTANEOUS RMS above this for attackFrames -> -1 gain step.
     *
     * 0.05 gives attack headroom ~11 dB above music's p99 instantaneous RMS at
     * listening volume — attack only engages on genuinely loud (festival) input; the
     * near-clip peak path is the hard ceiling.
     *
     * The 0.02 FLOOR is deliberate settings migration, not a taste judgement: Phase 2
     * changed this comparison's semantics (it compares instantaneous, not smoothed,
     * RMS now), so the getter clamping a stale sub-0.02 persisted value up to 0.02 on
     * first read is what migrates an already-provisioned board. */
    {"audio/agc_target_high", "AGC Target High", AudioParamType::F32, 0.05f, 0.02f, 0.5f, 0.005f,
     "", nullptr},

    /* Minimum gap between any two AGC gain steps. */
    {"audio/agc_rate_limit_frames", "AGC Rate Limit Frames", AudioParamType::U32, 10.0f, 1.0f,
     100.0f, 1.0f, "frames", nullptr},

    /* --- FFT bar visualisation (no effect on beat detection) ------------- */

    {"audio/fft_smoothing_coeff", "FFT Smoothing Coeff", AudioParamType::F32, 0.3f, 0.0f, 1.0f,
     0.01f, "", nullptr},

    {"audio/fft_energy_scale", "FFT Energy Scale", AudioParamType::F32, 20.0f, 0.1f, 1000.0f, 0.5f,
     "", nullptr},

    /* --- AGC, Phase 2 additions ------------------------------------------ */

    /* Consecutive over-targetHigh frames before an attack step. */
    {"audio/agc_attack_frames", "AGC Attack Frames", AudioParamType::U32, 3.0f, 1.0f, 20.0f, 1.0f,
     "frames", nullptr},

    /* Consecutive under-targetLow frames before a release step. Deliberately slower
     * than attack, so one quiet bar does not crank the gain. */
    {"audio/agc_release_frames", "AGC Release Frames", AudioParamType::U32, 15.0f, 1.0f, 100.0f,
     1.0f, "frames", nullptr},

    /* Input-referred RMS below which the frame counts as silence: gain is held,
     * release is blocked, and ALL beat output is suppressed. 0 disables the gate.
     *
     * Retuned 0.001 -> 0.0006 (issue #264, post-Phase-3) in response to a field
     * report: with music playing the glasses sometimes did not react at all, and
     * turning the volume up fixed it. Measured cause — the gate sat ABOVE the bottom
     * half of real music. Smoothed input-referred RMS over the corpus: quiet room p95
     * 0.00049, but normal-volume music p5 0.00061, and a whole normal-volume capture
     * had 52.9% of its frames below the 0.001 gate, i.e. all beat output suppressed.
     * See docs/plans/2026-08-02-beat-detection-phase3-and-beyond.md 5.5.1.
     *
     * The margin between quiet-room p95 and music p5 is only 1.25x, so this parameter
     * has very little room either side. Treat any retune as a corpus measurement, not
     * a guess. */
    {"audio/noise_gate_rms", "AGC Noise Gate RMS", AudioParamType::F32, 0.0006f, 0.0f, 0.02f,
     0.0001f, "", nullptr},

    /* --- Beat detection, Phase 3 additions -------------------------------- */

    /* Mode 1 only: threshold = median + sfDelta over the flux history. Structurally
     * band-blind — band 0 flux peaks around 3.5 and band 3 around 0.2, so one absolute
     * offset cannot suit all four. That is why mode 0 remains the default. */
    {"audio/sf_delta", "Beat SF Delta", AudioParamType::F32, 0.10f, 0.0f, 2.0f, 0.01f, "", nullptr},

    /* 0 = mean + alpha*sigma (default), 1 = median + sfDelta.
     * Bounds must track AUDIO_THRESHOLD_MODE_* in audio_dsp.h; audio_dsp.cpp
     * static_asserts that they do. */
    {"audio/threshold_mode", "Beat Threshold Mode", AudioParamType::ENUM, 0.0f, 0.0f, 1.0f, 1.0f,
     "", "Average (mean + alpha x sigma)\nMedian (median + delta)"},
};

static_assert(sizeof(kAudioParams) / sizeof(kAudioParams[0]) == kAudioParamCount,
              "kAudioParams and the kAudioParam* index enum have drifted apart");

/**
 * @brief constexpr string equality, for binding declaration-site literals to this table.
 *
 * Exists so audio_config.cpp can static_assert that the settings key and CUD label
 * written at each BtGattPersistentCharacteristic declaration still agree with the
 * corresponding table entry. Those NTTPs must be spelled as literals, so they cannot
 * be generated from the table; this makes them unable to disagree with it silently.
 * (std::string_view would do the same job but pulls a heavyweight header into every
 * consumer, including the native_sim replay app.)
 */
/** constexpr strlen — the table's strings are compile-time literals. */
constexpr size_t audioParamStrLen(const char *s) {
    size_t n = 0;
    while (s != nullptr && s[n] != '\0') n++;
    return n;
}

constexpr bool audioParamStrEq(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

/* ---------------------------------------------------------------------------
 * Canonical clamps. Every consumer must route through these.
 * ------------------------------------------------------------------------- */

/**
 * @brief Clamp a float parameter to its table range, rejecting non-finite input.
 *
 * NaN maps to the parameter's DEFAULT, not to min. This is a deliberate, and
 * behaviour-changing, unification of the four previous copies, which disagreed:
 * std::clamp(NaN, lo, hi) returns NaN (every comparison is false), while the
 * replay harness's fminf(fmaxf(NaN, lo), hi) returns lo.
 *
 * Letting NaN through is not merely inconsistent, it is harmful. A NaN threshold
 * makes every comparison in the detector false, silently disabling beat output or
 * the AGC — which is exactly why the shell path already rejects it
 * (parse_finite_float in sound.cpp). The BLE path had no equivalent guard, so a
 * NaN written over GATT was accepted, persisted to NVS, and reloaded on every
 * subsequent boot. Worse, the getters' clamp-write-back is `if (clamped != value)`,
 * and NaN != NaN is TRUE, so a persisted NaN re-entered that branch on every DSP
 * frame forever.
 *
 * Infinities need no special case: +inf > max and -inf < min already saturate.
 */
template <size_t I>
constexpr float audioParamClampF(float v) {
    static_assert(I < kAudioParamCount, "audio parameter index out of range");
    /* v != v is true only for NaN, and unlike std::isnan it is usable in a
     * constant expression on every toolchain this builds with. */
    if (v != v) {
        return kAudioParams[I].def;
    }
    if (v < kAudioParams[I].min) {
        return kAudioParams[I].min;
    }
    if (v > kAudioParams[I].max) {
        return kAudioParams[I].max;
    }
    return v;
}

/** @brief Clamp an integer parameter to its table range. */
template <size_t I>
constexpr uint32_t audioParamClampU(uint32_t v) {
    static_assert(I < kAudioParamCount, "audio parameter index out of range");
    constexpr uint32_t lo = static_cast<uint32_t>(kAudioParams[I].min);
    constexpr uint32_t hi = static_cast<uint32_t>(kAudioParams[I].max);
    return v < lo ? lo : (v > hi ? hi : v);
}

/** @brief This parameter's default, as a float. */
template <size_t I>
constexpr float audioParamDefaultF() {
    static_assert(I < kAudioParamCount, "audio parameter index out of range");
    static_assert(kAudioParams[I].type == AudioParamType::F32,
                  "audioParamDefaultF() used on a non-float parameter");
    return kAudioParams[I].def;
}

/** @brief This parameter's default, as a uint32. */
template <size_t I>
constexpr uint32_t audioParamDefaultU() {
    static_assert(I < kAudioParamCount, "audio parameter index out of range");
    static_assert(kAudioParams[I].type != AudioParamType::F32,
                  "audioParamDefaultU() used on a float parameter");
    return static_cast<uint32_t>(kAudioParams[I].def);
}

/* ---------------------------------------------------------------------------
 * Compile-time table self-check.
 *
 * These are invariants of the table itself, checked once at build time so a
 * malformed entry cannot reach a device. Range VALUES are pinned separately, by
 * fw/tests/sound/audio_param_table, against the literals that were in the four
 * hand-written copies before this table replaced them.
 *
 * It takes the table as a PARAMETER rather than reading kAudioParams directly, so the
 * test suite can feed it deliberately-malformed tables. Without that, the static_assert
 * below proves only that the check passes for the real table — never that it would FAIL
 * for a bad one, which is the property that actually matters. A self-check nobody has
 * watched reject something is indistinguishable from `return true`.
 * ------------------------------------------------------------------------- */

/* ── Valid Range (0x2906) descriptor bytes ──────────────────────────────────────────────
 *
 * The descriptor's value is "lower inclusive then upper inclusive, each encoded in the format
 * the characteristic's CPF declares" — so these are two little-endian IEEE-754 floats for a
 * FLOAT32 characteristic and two little-endian uint32s for a UINT32 one. There is no generic
 * numeric encoding to fall back on, and getting it wrong produces a plausible-looking range
 * that is silently nonsense to every tool that renders it.
 *
 * Built here rather than at the declaration site so the descriptor and the bulk blob are two
 * projections of the SAME table row and cannot drift.
 */
struct AudioParamRangeBytes {
    uint8_t b[8];
};

/** The one place a 32-bit value becomes little-endian bytes on this wire. */
constexpr void audioParamPutLe32(uint8_t *out, uint32_t v) {
    out[0] = (uint8_t)(v & 0xFF);
    out[1] = (uint8_t)((v >> 8) & 0xFF);
    out[2] = (uint8_t)((v >> 16) & 0xFF);
    out[3] = (uint8_t)((v >> 24) & 0xFF);
}

constexpr AudioParamRangeBytes audioParamRangeBytesFromU32(uint32_t lo, uint32_t hi) {
    AudioParamRangeBytes out{};
    audioParamPutLe32(&out.b[0], lo);
    audioParamPutLe32(&out.b[4], hi);
    return out;
}

/* Delegates rather than repeating the shift/mask ladder. Two copies had to be edited in
 * lockstep, and a fix applied to one and not the other would produce descriptor bytes that
 * differ by characteristic TYPE — precisely the plausible-looking-but-wrong range the comment
 * above warns about. */
constexpr AudioParamRangeBytes audioParamRangeBytesFromFloats(float lo, float hi) {
    return audioParamRangeBytesFromU32(std::bit_cast<uint32_t>(lo),
                                       std::bit_cast<uint32_t>(hi));
}

/** Descriptor bytes for a FLOAT32 parameter. */
template <size_t I>
constexpr AudioParamRangeBytes audioParamRangeBytesF() {
    static_assert(I < kAudioParamCount, "parameter index out of range");
    static_assert(kAudioParams[I].type == AudioParamType::F32,
                  "float range bytes requested for a non-float parameter");
    return audioParamRangeBytesFromFloats(kAudioParams[I].min, kAudioParams[I].max);
}

/** Descriptor bytes for a UINT32 or ENUM parameter. */
template <size_t I>
constexpr AudioParamRangeBytes audioParamRangeBytesU() {
    static_assert(I < kAudioParamCount, "parameter index out of range");
    static_assert(kAudioParams[I].type != AudioParamType::F32,
                  "integer range bytes requested for a float parameter");
    return audioParamRangeBytesFromU32((uint32_t)kAudioParams[I].min, (uint32_t)kAudioParams[I].max);
}

constexpr bool audioParamTableSelfCheck(const AudioParamSpec *params, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const AudioParamSpec &p = params[i];

        if (p.key == nullptr || p.key[0] == '\0') {
            return false;
        }
        if (p.label == nullptr || p.label[0] == '\0') {
            return false;
        }
        if (p.unit == nullptr) {
            return false; /* dimensionless is "", never nullptr */
        }
        if (!(p.min <= p.def && p.def <= p.max)) {
            return false;
        }
        if (!(p.step > 0.0f)) {
            return false;
        }
        if (p.step > (p.max - p.min)) {
            return false;
        }

        /* Enum parameters must carry labels; nothing else may. */
        if ((p.type == AudioParamType::ENUM) != (p.enumLabels != nullptr)) {
            return false;
        }

        /* Integer-typed parameters must have bounds and a default that survive the
         * float round-trip the table stores them in, or audioParamClampU() would
         * silently clamp to a different number than the table advertises. */
        if (p.type != AudioParamType::F32) {
            if (p.min < 0.0f) {
                return false;
            }
            if (static_cast<float>(static_cast<uint32_t>(p.min)) != p.min ||
                static_cast<float>(static_cast<uint32_t>(p.max)) != p.max ||
                static_cast<float>(static_cast<uint32_t>(p.def)) != p.def) {
                return false;
            }
        }
    }

    /* Settings keys and CUD labels are both externally visible identifiers; a
     * duplicate in either would make two characteristics collide in NVS or be
     * indistinguishable in the app. */
    for (size_t i = 0; i < count; ++i) {
        for (size_t j = i + 1; j < count; ++j) {
            const char *a = params[i].key;
            const char *b = params[j].key;
            size_t k = 0;
            while (a[k] != '\0' && a[k] == b[k]) {
                ++k;
            }
            if (a[k] == '\0' && b[k] == '\0') {
                return false;
            }

            a = params[i].label;
            b = params[j].label;
            k = 0;
            while (a[k] != '\0' && a[k] == b[k]) {
                ++k;
            }
            if (a[k] == '\0' && b[k] == '\0') {
                return false;
            }
        }
    }

    return true;
}

static_assert(audioParamTableSelfCheck(kAudioParams, kAudioParamCount),
              "kAudioParams failed its structural self-check");
