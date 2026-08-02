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
 *    BEAT_AGC         off|sim (default off). "off" = fixed gain, matches a
 *                     freeze-gain device recording. "sim" = mirror the AGC
 *                     loop in sound.cpp (32-frame RMS window, target window,
 *                     rate limit, history reset on step), applying gain
 *                     digitally relative to the recording gain.
 *    BEAT_GAIN        recording's PDM gain register value (default 0x28);
 *                     sim mode starts from here
 *    BEAT_TARGET_LOW/BEAT_TARGET_HIGH/BEAT_RATE_LIMIT
 *                     AGC sim params (defaults 0.005/0.008/10)
 *    BEAT_BUCKETS     "1" appends the 20 display-bucket energies per D-line
 *
 * native_sim links the host glibc (EXTERNAL_LIBC), so getenv/fopen/printf
 * operate on real host paths. Host-vs-device float caveat: x86 SSE vs
 * Cortex-M33 VFMA contraction means results are close but not bit-identical —
 * fw/tools/beat_lab/compare.py applies a relative tolerance.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_dsp.h"
#include "audio_tap_format.h" /* shared D-line/#PARAMS format — single source of truth */

/* Terminates the native_sim process with the given exit code (a plain return
 * from main() would leave the simulated kernel idling forever). Declared here
 * instead of including posix_board_if.h to avoid include-path fragility. */
extern "C" void posix_exit(int exit_code);

namespace {

float clampf(float v, float lo, float hi) { return fminf(fmaxf(v, lo), hi); }

/* Env-driven provider mirroring DefaultAudioDspConfigProvider's defaults AND
 * clamps — the device silently clamps out-of-range values (AudioConfig and the
 * default provider agree on the ranges), so the replay must clamp identically
 * or a parameter sweep could report a "best" value the hardware cannot run. */
class EnvConfigProvider : public AudioDspConfigProvider {
   public:
    float getFluxGamma() override { return gamma_; }
    void setFluxGamma(float v) override { gamma_ = clampf(v, 1.0f, 100000.0f); }
    float getBeatFluxFloor() override { return floor_; }
    void setBeatFluxFloor(float v) override { floor_ = clampf(v, 0.0f, 1.0f); }
    float getBeatAlpha() override { return alpha_; }
    void setBeatAlpha(float v) override { alpha_ = clampf(v, 0.1f, 20.0f); }
    uint32_t getBeatRefractoryFrames() override { return refractory_; }
    void setBeatRefractoryFrames(uint32_t v) override { refractory_ = v > 255 ? 255 : v; }

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
        const char *r = getenv("BEAT_REFRACTORY");
        if (r != nullptr) {
            setBeatRefractoryFrames((uint32_t)strtoul(r, nullptr, 10));
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
    float gamma_ = 1000.0f;
    float floor_ = 0.005f;
    float alpha_ = 3.5f;
    uint32_t refractory_ = 5;
};

EnvConfigProvider sEnvProvider;

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
                 uint32_t rate_limit) {
    size_t len = audio_tap_format_params(
        sEnvProvider.getFluxGamma(), sEnvProvider.getBeatAlpha(),
        sEnvProvider.getBeatFluxFloor(), sEnvProvider.getBeatRefractoryFrames(), agc_frozen,
        gain, target_low, target_high, rate_limit, s_line, sizeof(s_line));
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

    const char *agc_env = getenv("BEAT_AGC");
    const bool agc_sim = (agc_env != nullptr && strcmp(agc_env, "sim") == 0);
    if (agc_env != nullptr && !agc_sim && strcmp(agc_env, "off") != 0) {
        fprintf(stderr, "replay: BEAT_AGC must be off|sim\n");
        free(wav.samples);
        return 1;
    }

    uint8_t gain = 0x28;
    const char *gain_env = getenv("BEAT_GAIN");
    if (gain_env != nullptr) {
        gain = (uint8_t)strtoul(gain_env, nullptr, 0);
    }
    const uint8_t start_gain = gain;

    float target_low = 0.005f, target_high = 0.008f;
    uint32_t rate_limit = 10;
    if (const char *s = getenv("BEAT_TARGET_LOW")) {
        target_low = strtof(s, nullptr);
    }
    if (const char *s = getenv("BEAT_TARGET_HIGH")) {
        target_high = strtof(s, nullptr);
    }
    if (const char *s = getenv("BEAT_RATE_LIMIT")) {
        rate_limit = (uint32_t)strtoul(s, nullptr, 10);
    }
    const bool buckets = (getenv("BEAT_BUCKETS") != nullptr &&
                          strcmp(getenv("BEAT_BUCKETS"), "1") == 0);

    audio_dsp_init();
    emit_params(out, !agc_sim, gain, target_low, target_high, rate_limit);

    /* AGC sim state — mirrors sound.cpp's audio_dsp_thread_func() loop. */
    float rms_history[32] = {0};
    uint8_t rms_idx = 0;
    int frames_since = 0;

    int16_t block[AUDIO_FFT_SIZE];
    struct audio_analysis_result result;
    uint32_t seq = 0;

    for (uint32_t off = 0; off + AUDIO_FFT_SIZE <= wav.num_samples; off += AUDIO_FFT_SIZE) {
        if (agc_sim && gain != start_gain) {
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

        if (agc_sim) {
            float rms = compute_rms(block, AUDIO_FFT_SIZE);
            rms_history[rms_idx] = rms;
            rms_idx = (uint8_t)((rms_idx + 1) % 32);
            float smoothed = 0.0f;
            for (int i = 0; i < 32; i++) {
                smoothed += rms_history[i];
            }
            smoothed /= 32.0f;

            frames_since++;
            if ((uint32_t)frames_since >= rate_limit) {
                bool changed = false;
                if (smoothed < target_low && gain < 0x50) {
                    gain++;
                    changed = true;
                } else if (smoothed > target_high && gain > 0x00) {
                    gain--;
                    changed = true;
                }
                if (changed) {
                    /* Mirrors the firmware's current behavior (full history reset per
                     * gain step) so the replay reproduces the AGC/detector interaction
                     * under investigation. */
                    audio_dsp_reset_history();
                    frames_since = 0;
                }
            }
        }

        float rms = compute_rms(block, AUDIO_FFT_SIZE);
        audio_dsp_process(block, seq++, &result);
        emit_frame(out, &result, rms, gain, buckets);
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
