/* WAV-replay harness for the firmware beat detector (issue #264).
 *
 * Compiles the REAL fw/src/sound/audio_dsp.cpp (identical translation unit,
 * identical SDK CMSIS-DSP) into a native_sim executable. Two modes:
 *
 *  - Twister selftest (no BEAT_WAV env var): synthesizes a silence→onset
 *    sequence, asserts a band-0 beat fires, prints "REPLAY SELFTEST PASS".
 *    This keeps the harness itself CI-covered by /test-fw.
 *
 *  - CLI replay (BEAT_WAV=<path>): streams a 16 kHz/mono/16-bit WAV through
 *    audio_dsp_process() in 512-sample blocks, emitting one D-line per frame
 *    in the same text format as the firmware's "sound dump" / record_wav
 *    sidecar CSV (producer: tap_frame_format() in fw/src/sound/sound.cpp;
 *    decoder: fw/tools/beat_lab/frames.py — the three must stay in sync).
 *
 * Environment variables (CLI mode):
 *    BEAT_WAV         input WAV path (required for CLI mode)
 *    BEAT_OUT         output path (default: stdout)
 *    BEAT_GAMMA/BEAT_ALPHA/BEAT_FLOOR/BEAT_REFRACTORY
 *                     DSP params (default: firmware defaults)
 *    BEAT_SF_DELTA    mode-1 additive offset above the running median
 *    BEAT_THRESHOLD_MODE
 *                     0 = mean+alpha*sigma (default), 1 = median+sf_delta
 *    BEAT_AGC         off|sim|sim_reset (default off). "off" = fixed gain,
 *                     matches a freeze-gain device recording. "sim" = mirror
 *                     the AGC loop in sound.cpp (32-frame RMS window, target
 *                     window, rate limit, gain-step COMPENSATION via
 *                     audio_dsp_compensate_gain_change), applying gain
 *                     digitally relative to the recording gain. "sim_reset" =
 *                     the pre-Phase-1 behavior (full history reset per step),
 *                     kept so Phase 1's improvement is A/B-measurable offline
 *                     on the same WAV.
 *    BEAT_GAIN        recording's PDM gain register value (default 0x28);
 *                     sim mode starts from here
 *    BEAT_TARGET_LOW/BEAT_TARGET_HIGH/BEAT_RATE_LIMIT
 *                     AGC sim params (defaults now come from audio_param_table.h:
 *                     0.002 / 0.05 / 10 — this line used to say 0.005/0.008/10,
 *                     stale since the Phase 2 retune)
 *    BEAT_BUCKETS     "1" appends the 20 display-bucket energies per D-line
 *
 * native_sim links the host glibc (EXTERNAL_LIBC), so getenv/fopen/printf
 * operate on real host paths. Host-vs-device float caveat: x86 SSE vs
 * Cortex-M33 VFMA contraction means results are close but not bit-identical —
 * fw/tools/beat_lab/compare.py applies a relative tolerance.
 */
#include <math.h>
#include <sound/audio_param_table.h> /* shared D-line/#PARAMS format — single source of truth */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agc_controller.h" /* the REAL firmware AGC policy, compiled in for sim mode */
#include "audio_dsp.h"
#include "audio_tap_format.h"

/* Terminates the native_sim process with the given exit code (a plain return
 * from main() would leave the simulated kernel idling forever). Declared here
 * instead of including posix_board_if.h to avoid include-path fragility. */
extern "C" void posix_exit(int exit_code);

namespace {

/* Env-driven provider taking its defaults AND clamps from audio_param_table.h — the same
 * header the firmware compiles. The device silently clamps out-of-range values, so the replay
 * must clamp identically or a parameter sweep could report a "best" value the hardware cannot
 * run.
 *
 * This used to be a hand-written copy and had already drifted in shape: it clamped with
 * fminf/fmaxf, which returns the LOW BOUND for NaN where the device's std::clamp returned NaN.
 * Sharing the table removes that class of divergence entirely. */
class EnvConfigProvider : public AudioDspConfigProvider {
   public:
    float getFluxGamma() override { return gamma_; }
    void setFluxGamma(float v) override { gamma_ = audioParamClampF<kAudioParamFluxGamma>(v); }
    float getBeatFluxFloor() override { return floor_; }
    void setBeatFluxFloor(float v) override {
        floor_ = audioParamClampF<kAudioParamBeatFluxFloor>(v);
    }
    float getBeatAlpha() override { return alpha_; }
    void setBeatAlpha(float v) override { alpha_ = audioParamClampF<kAudioParamBeatAlpha>(v); }
    uint32_t getBeatRefractoryFrames() override { return refractory_; }
    void setBeatRefractoryFrames(uint32_t v) override {
        refractory_ = audioParamClampU<kAudioParamBeatRefractoryFrames>(v);
    }
    float getSfDelta() override { return sfDelta_; }
    void setSfDelta(float v) override { sfDelta_ = audioParamClampF<kAudioParamSfDelta>(v); }
    uint32_t getThresholdMode() override { return thresholdMode_; }
    void setThresholdMode(uint32_t v) override {
        thresholdMode_ = audioParamClampU<kAudioParamThresholdMode>(v);
    }

    void loadFromEnv() {
        /* Through the setters so env values get the same clamping as the device. */
        float v;
        if (envFloat("BEAT_GAMMA", &v)) {
            setFluxGamma(v);
        }
        if (envFloat("BEAT_FLOOR", &v)) {
            setBeatFluxFloor(v);
        }
        if (envFloat("BEAT_ALPHA", &v)) {
            setBeatAlpha(v);
        }
        if (envFloat("BEAT_SF_DELTA", &v)) {
            setSfDelta(v);
        }
        const char *r = getenv("BEAT_REFRACTORY");
        if (r != nullptr) {
            setBeatRefractoryFrames((uint32_t)strtoul(r, nullptr, 10));
        }
        const char *m = getenv("BEAT_THRESHOLD_MODE");
        if (m != nullptr) {
            setThresholdMode((uint32_t)strtoul(m, nullptr, 10));
        }
    }

   private:
    static bool envFloat(const char *name, float *out) {
        const char *s = getenv(name);
        if (s == nullptr) {
            return false;
        }
        *out = strtof(s, nullptr);
        return true;
    }
    float gamma_ = audioParamDefaultF<kAudioParamFluxGamma>();
    float floor_ = audioParamDefaultF<kAudioParamBeatFluxFloor>();
    float alpha_ = audioParamDefaultF<kAudioParamBeatAlpha>();
    uint32_t refractory_ = audioParamDefaultU<kAudioParamBeatRefractoryFrames>();
    float sfDelta_ = audioParamDefaultF<kAudioParamSfDelta>();
    uint32_t thresholdMode_ = audioParamDefaultU<kAudioParamThresholdMode>();
};

EnvConfigProvider sEnvProvider;

/* Env-driven AGC tunables for the closed-loop sim, mirroring the firmware
 * defaults/clamps (DefaultAgcConfigProvider in sound.cpp). */
class EnvAgcProvider : public AgcConfigProvider {
   public:
    EnvAgcProvider() {
        float f;
        if (envFloat("BEAT_TARGET_LOW", &f)) {
            setTargetLow(f);
        }
        if (envFloat("BEAT_TARGET_HIGH", &f)) {
            setTargetHigh(f);
        }
        if (envFloat("BEAT_GATE", &f)) {
            setNoiseGateRms(f);
        }
        envU32("BEAT_RATE_LIMIT", [this](uint32_t v) { setRateLimitFrames(v); });
        envU32("BEAT_ATTACK", [this](uint32_t v) { setAttackFrames(v); });
        envU32("BEAT_RELEASE", [this](uint32_t v) { setReleaseFrames(v); });
    }

    float getTargetLow() override { return tlow_; }
    void setTargetLow(float v) override { tlow_ = audioParamClampF<kAudioParamAgcTargetLow>(v); }
    float getTargetHigh() override { return thigh_; }
    /* Floor 0.02 mirrors the firmware's semantic-migration clamp (see
     * AudioConfig::setTargetHigh). */
    void setTargetHigh(float v) override { thigh_ = audioParamClampF<kAudioParamAgcTargetHigh>(v); }
    uint32_t getRateLimitFrames() override { return rate_; }
    void setRateLimitFrames(uint32_t v) override {
        rate_ = audioParamClampU<kAudioParamAgcRateLimitFrames>(v);
    }
    uint32_t getAttackFrames() override { return attack_; }
    void setAttackFrames(uint32_t v) override {
        attack_ = audioParamClampU<kAudioParamAgcAttackFrames>(v);
    }
    uint32_t getReleaseFrames() override { return release_; }
    void setReleaseFrames(uint32_t v) override {
        release_ = audioParamClampU<kAudioParamAgcReleaseFrames>(v);
    }
    float getNoiseGateRms() override { return gate_; }
    void setNoiseGateRms(float v) override { gate_ = audioParamClampF<kAudioParamNoiseGateRms>(v); }

   private:
    static bool envFloat(const char *name, float *out) {
        const char *s = getenv(name);
        if (s == nullptr) {
            return false;
        }
        *out = strtof(s, nullptr);
        return true;
    }
    template <typename F>
    static void envU32(const char *name, F set) {
        const char *s = getenv(name);
        if (s != nullptr) {
            set((uint32_t)strtoul(s, nullptr, 10));
        }
    }
    /* Defaults and clamps come from audio_param_table.h, the same header the firmware
     * compiles, so they can no longer go stale by hand — which used to matter a great deal
     * here: `--agc sim` simulating a policy no board runs is exactly the PR #279 review
     * finding (a stale 0.008 targetHigh made the sim ratchet gain to the floor on music the
     * real firmware holds steady on). */
    float tlow_ = audioParamDefaultF<kAudioParamAgcTargetLow>();
    float thigh_ = audioParamDefaultF<kAudioParamAgcTargetHigh>();
    uint32_t rate_ = audioParamDefaultU<kAudioParamAgcRateLimitFrames>();
    uint32_t attack_ = audioParamDefaultU<kAudioParamAgcAttackFrames>();
    uint32_t release_ = audioParamDefaultU<kAudioParamAgcReleaseFrames>();
    float gate_ = audioParamDefaultF<kAudioParamNoiseGateRms>();
};

/* ── D-line emission — field order comes from the shared audio_tap_format.h,
 *    the same header sound.cpp's producers compile ────────────────────────── */

char s_line[512];

void emit_frame(FILE *out, const struct audio_analysis_result *r, float rms, uint8_t gain,
                bool buckets) {
    size_t len = audio_tap_format_frame(r, rms, gain, buckets, s_line, sizeof(s_line));
    fwrite(s_line, 1, len, out);
    fputc('\n', out);
}

void emit_params(FILE *out, bool agc_frozen, uint8_t gain, float target_low, float target_high,
                 uint32_t rate_limit, uint32_t attack, uint32_t release, float gate) {
    size_t len = audio_tap_format_params(
        sEnvProvider.getFluxGamma(), sEnvProvider.getBeatAlpha(),
        sEnvProvider.getBeatFluxFloor(), sEnvProvider.getBeatRefractoryFrames(), agc_frozen,
        gain, target_low, target_high, rate_limit, attack, release, gate,
        sEnvProvider.getSfDelta(), sEnvProvider.getThresholdMode(), s_line, sizeof(s_line));
    fwrite(s_line, 1, len, out);
    fputc('\n', out);
}

/* Same formula as agc_compute_rms() in sound.cpp (peak tracking omitted:
 * the current AGC decision logic never reads it). */
float compute_rms(const int16_t *pcm, uint32_t n) {
    float sum_sq = 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        float s = (float)pcm[i] * (1.0f / 32768.0f);
        sum_sq += s * s;
    }
    return sqrtf(sum_sq / (float)n);
}

/* ── Minimal chunk-walking WAV reader (16 kHz / mono / 16-bit PCM only) ───── */

struct WavData {
    int16_t *samples;
    uint32_t num_samples;
};

bool read_le32(FILE *f, uint32_t *v) {
    uint8_t b[4];
    if (fread(b, 1, 4, f) != 4) {
        return false;
    }
    *v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return true;
}

bool load_wav(const char *path, WavData *out) {
    FILE *f = fopen(path, "rb");
    if (f == nullptr) {
        fprintf(stderr, "replay: cannot open %s\n", path);
        return false;
    }
    char id[4];
    uint32_t riff_size;
    if (fread(id, 1, 4, f) != 4 || memcmp(id, "RIFF", 4) != 0 || !read_le32(f, &riff_size) ||
        fread(id, 1, 4, f) != 4 || memcmp(id, "WAVE", 4) != 0) {
        fprintf(stderr, "replay: %s is not a RIFF/WAVE file\n", path);
        fclose(f);
        return false;
    }

    bool have_fmt = false;
    uint16_t audio_format = 0, num_channels = 0, bits_per_sample = 0;
    uint32_t sample_rate = 0;
    out->samples = nullptr;
    out->num_samples = 0;

    /* Walk chunks: tolerate LIST/INFO etc. so host-generated WAVs also load. */
    while (fread(id, 1, 4, f) == 4) {
        uint32_t chunk_size;
        if (!read_le32(f, &chunk_size)) {
            break;
        }
        if (memcmp(id, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            if (chunk_size < 16 || fread(fmt, 1, 16, f) != 16) {
                break;
            }
            audio_format = (uint16_t)(fmt[0] | (fmt[1] << 8));
            num_channels = (uint16_t)(fmt[2] | (fmt[3] << 8));
            sample_rate = (uint32_t)fmt[4] | ((uint32_t)fmt[5] << 8) | ((uint32_t)fmt[6] << 16) |
                          ((uint32_t)fmt[7] << 24);
            bits_per_sample = (uint16_t)(fmt[14] | (fmt[15] << 8));
            have_fmt = true;
            if (chunk_size > 16) {
                fseek(f, (long)(chunk_size - 16), SEEK_CUR);
            }
        } else if (memcmp(id, "data", 4) == 0) {
            out->num_samples = chunk_size / 2;
            out->samples = (int16_t *)malloc(chunk_size);
            if (out->samples == nullptr ||
                fread(out->samples, 1, chunk_size, f) != chunk_size) {
                fprintf(stderr, "replay: short read of data chunk\n");
                free(out->samples);
                out->samples = nullptr;
                break;
            }
        } else {
            /* Chunks are word-aligned; odd sizes have a pad byte. */
            fseek(f, (long)(chunk_size + (chunk_size & 1)), SEEK_CUR);
        }
    }
    fclose(f);

    if (!have_fmt || out->samples == nullptr) {
        fprintf(stderr, "replay: %s missing fmt/data chunk\n", path);
        free(out->samples);
        return false;
    }
    if (audio_format != 1 || num_channels != 1 || sample_rate != 16000 || bits_per_sample != 16) {
        fprintf(stderr,
                "replay: %s must be 16 kHz mono 16-bit PCM (got fmt=%u ch=%u rate=%u bits=%u)\n",
                path, audio_format, num_channels, sample_rate, bits_per_sample);
        free(out->samples);
        return false;
    }
    return true;
}

/* ── CLI replay mode ──────────────────────────────────────────────────────── */

int run_replay(const char *wav_path) {
    sEnvProvider.loadFromEnv();
    audio_dsp_set_config_provider(&sEnvProvider);

    WavData wav;
    if (!load_wav(wav_path, &wav)) {
        return 1;
    }

    FILE *out = stdout;
    const char *out_path = getenv("BEAT_OUT");
    if (out_path != nullptr) {
        out = fopen(out_path, "w");
        if (out == nullptr) {
            fprintf(stderr, "replay: cannot open %s for writing\n", out_path);
            free(wav.samples);
            return 1;
        }
    }

    /* Modes:
     *   off        — fixed gain, no gate: matches a freeze-gain device capture
     *   sim        — the REAL AgcController (agc_controller.cpp, same TU as the
     *                firmware): closed-loop policy incl. attack/release, clip
     *                fast path, noise gate (beats cleared while silent) and
     *                Phase-1 gain compensation
     *   sim_legacy — pre-Phase-2 symmetric window policy + Phase-1 compensation
     *   sim_reset  — pre-Phase-1 behavior (full history reset per step)
     * The legacy modes exist so each phase's improvement stays A/B-measurable
     * offline on the same WAV. */
    enum class AgcMode { kOff, kSim, kSimLegacy, kSimReset };
    AgcMode mode = AgcMode::kOff;
    const char *agc_env = getenv("BEAT_AGC");
    if (agc_env != nullptr) {
        if (strcmp(agc_env, "sim") == 0) {
            mode = AgcMode::kSim;
        } else if (strcmp(agc_env, "sim_legacy") == 0) {
            mode = AgcMode::kSimLegacy;
        } else if (strcmp(agc_env, "sim_reset") == 0) {
            mode = AgcMode::kSimReset;
        } else if (strcmp(agc_env, "off") != 0) {
            fprintf(stderr, "replay: BEAT_AGC must be off|sim|sim_legacy|sim_reset\n");
            free(wav.samples);
            return 1;
        }
    }
    const bool any_sim = mode != AgcMode::kOff;

    uint8_t gain = 0x28;
    const char *gain_env = getenv("BEAT_GAIN");
    if (gain_env != nullptr) {
        gain = (uint8_t)strtoul(gain_env, nullptr, 0);
    }
    const uint8_t start_gain = gain;

    EnvAgcProvider agc_cfg; /* targets/rate/attack/release/gate from env */
    const bool buckets = (getenv("BEAT_BUCKETS") != nullptr &&
                          strcmp(getenv("BEAT_BUCKETS"), "1") == 0);

    audio_dsp_init();
    emit_params(out, !any_sim, gain, agc_cfg.getTargetLow(), agc_cfg.getTargetHigh(),
                agc_cfg.getRateLimitFrames(), agc_cfg.getAttackFrames(),
                agc_cfg.getReleaseFrames(), agc_cfg.getNoiseGateRms());

    /* Legacy-mode sim state — mirrors the pre-Phase-2 inline loop in sound.cpp. */
    float rms_history[32] = {0};
    uint8_t rms_idx = 0;
    int frames_since = 0;

    AgcController ctrl; /* kSim mode */

    int16_t block[AUDIO_FFT_SIZE];
    struct audio_analysis_result result;
    uint32_t seq = 0;

    for (uint32_t off = 0; off + AUDIO_FFT_SIZE <= wav.num_samples; off += AUDIO_FFT_SIZE) {
        if (any_sim && gain != start_gain) {
            /* Each register step = 0.5 dB of amplitude. The recording was made at
             * start_gain; simulate the hardware applying `gain` instead, clipping
             * to int16 exactly as the PDM front-end would. */
            float scale = powf(10.0f, 0.025f * (float)((int)gain - (int)start_gain));
            for (int i = 0; i < AUDIO_FFT_SIZE; i++) {
                float v = (float)wav.samples[off + i] * scale;
                block[i] = (int16_t)fmaxf(-32768.0f, fminf(32767.0f, v));
            }
        } else {
            memcpy(block, &wav.samples[off], sizeof(block));
        }

        float rms = compute_rms(block, AUDIO_FFT_SIZE);
        bool silent = false;
        AgcDecision d = {0, false, false};

        if (mode == AgcMode::kSim) {
            /* Mirror the fixed firmware loop: DECIDE before processing (the
             * silent flag gates this frame's beats) but APPLY the step only
             * after — this block is in the current gain domain. */
            int16_t peak = 0;
            for (int i = 0; i < AUDIO_FFT_SIZE; i++) {
                int16_t a = (int16_t)(block[i] < 0 ? -block[i] : block[i]);
                if (a > peak) {
                    peak = a;
                }
            }
            d = ctrl.update(agc_cfg, rms, peak, gain, true);
            silent = d.silent;
        }

        audio_dsp_process(block, seq++, &result);
        if (silent) {
            /* Mirror sound.cpp's noise gate: no beat output during silence. */
            for (int b = 0; b < AUDIO_NUM_BANDS; b++) {
                result.beat[b] = false;
            }
        }
        /* Emit before any step so the D-line's gain column is the gain this
         * block was "captured" at — same as the firmware tap. */
        emit_frame(out, &result, rms, gain, buckets);

        if (mode == AgcMode::kSim) {
            if (d.gain_steps != 0) {
                /* Same apply sequence as sound.cpp's agc_apply_gain(). */
                gain = (uint8_t)((int)gain + d.gain_steps);
                audio_dsp_compensate_gain_change(d.gain_steps);
                ctrl.notifyGainChange(d.gain_steps);
            }
        } else if (mode == AgcMode::kSimLegacy || mode == AgcMode::kSimReset) {
            /* Legacy inline policy — decided and applied AFTER processing (the
             * fixed ordering); a step only affects the NEXT block's scaling. */
            rms_history[rms_idx] = rms;
            rms_idx = (uint8_t)((rms_idx + 1) % 32);
            float smoothed = 0.0f;
            for (int i = 0; i < 32; i++) {
                smoothed += rms_history[i];
            }
            smoothed /= 32.0f;

            frames_since++;
            if ((uint32_t)frames_since >= agc_cfg.getRateLimitFrames()) {
                int step = 0;
                if (smoothed < agc_cfg.getTargetLow() && gain < 0x50) {
                    gain++;
                    step = 1;
                } else if (smoothed > agc_cfg.getTargetHigh() && gain > 0x00) {
                    gain--;
                    step = -1;
                }
                if (step != 0) {
                    /* BOTH legacy modes rescale the RMS window so their AGC
                     * trajectories are identical on the same WAV — the A/B then
                     * isolates the ONLY intended difference (detector-history
                     * handling); previously sim_reset skipped the rescale and
                     * the two modes fed different PCM into the detector. */
                    float amp = audio_dsp_gain_amplitude_ratio(step);
                    for (int i = 0; i < 32; i++) {
                        rms_history[i] *= amp;
                    }
                    if (mode == AgcMode::kSimReset) {
                        /* Pre-Phase-1 firmware behavior: full history reset per
                         * gain step — kept for offline A/B against the fix. */
                        audio_dsp_reset_history();
                    } else {
                        /* Phase-1 firmware behavior: carry detector state across
                         * the step (audio_dsp_compensate_gain_change). */
                        audio_dsp_compensate_gain_change(step);
                    }
                    frames_since = 0;
                }
            }
        }
    }

    fprintf(out, "#DONE frames=%u dropped=0\n", seq);
    if (out != stdout) {
        fclose(out);
    }
    free(wav.samples);
    fprintf(stderr, "replay: processed %u frames from %s\n", seq, wav_path);
    return 0;
}

/* ── Twister selftest mode ────────────────────────────────────────────────── */

int run_selftest(void) {
    audio_dsp_init();

    int16_t pcm[AUDIO_FFT_SIZE];
    struct audio_analysis_result result;

    /* Fill history with silence, then inject a loud 100 Hz sine (bins 3-4 →
     * band 0) — same recipe as fw/tests/sound/audio_dsp. */
    memset(pcm, 0, sizeof(pcm));
    for (int i = 0; i < 32; i++) {
        audio_dsp_process(pcm, (uint32_t)i, &result);
    }
    for (int i = 0; i < AUDIO_FFT_SIZE; i++) {
        double t = (double)i / 16000.0;
        pcm[i] = (int16_t)(32767.0 * sin(2.0 * M_PI * 100.0 * t));
    }
    audio_dsp_process(pcm, 32, &result);

    if (!result.beat[0]) {
        printf("REPLAY SELFTEST FAIL: no beat on band 0 onset\n");
        return 1;
    }
    if (!(result.band_flux[0] > 0.0f)) {
        printf("REPLAY SELFTEST FAIL: band_flux[0] not populated\n");
        return 1;
    }

    /* Exercise the emitter so format regressions crash loudly in CI. */
    emit_frame(stdout, &result, 0.5f, 0x28, true);
    printf("REPLAY SELFTEST PASS\n");
    return 0;
}

}  // namespace

int main(void) {
    const char *wav_path = getenv("BEAT_WAV");
    int rc = (wav_path != nullptr) ? run_replay(wav_path) : run_selftest();
    posix_exit(rc);
    return rc; /* unreachable */
}
