#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/shell/shell.h>

#if defined(CONFIG_VM3011)
#include <zephyr/drivers/vm3011/vm3011.h>
#endif
#include <math.h> /* sqrtf */
#include <stdio.h> /* snprintf (fmt_fixed4) */
#include <stdlib.h>
#include <string.h> /* memcpy (audio tap) */
#include <zephyr/audio/dmic.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>

#include <algorithm>

#include "audio_dsp.h"
#if defined(CONFIG_APP_AUDIO_DEBUG)
#include "audio_tap_format.h"
#endif
#include "sound.h"

/* ── Adaptive Gain Control ───────────────────────────────────────────────────
 * Adjusts PDM hardware gain to keep the RMS signal level inside a target
 * window.  Gain steps are rate-limited to prevent pumping.  After every
 * adjustment audio_dsp_reset_history() is called because the amplitude
 * discontinuity would otherwise look like a beat onset.
 *
 * Thresholds calibrated to the actual microphone output. RMS measured with
 * active music: 0.015–0.025 (sparse due to gaps between notes).
 * Target window [0.005, 0.008] keeps signal quiet to prevent FFT saturation.
 * All threshold values are tunable via shell commands and (see audio_config.cpp) BT. */
#define AGC_GAIN_MIN 0x00 /* −20 dB (PDM GAINL/GAINR register floor)   */
#define AGC_GAIN_MAX 0x50 /* +20 dB (PDM GAINL/GAINR register ceiling) */

namespace {
/* %f-free float printing (issue #79 ROM investigation): CONFIG_CBPRINTF_FP_SUPPORT and
 * CONFIG_PICOLIBC_IO_FLOAT are disabled project-wide to save ~10KB FLASH, so this file
 * (the only float-printing code in the app) formats via fixed-point integers instead.
 * Renders v with 4 decimal places into buf, e.g. "0.0123" / "-1.5000". */
const char *fmt_fixed4(float v, char *buf, size_t len) {
    /* Casting a non-finite float to unsigned below is undefined; print the value's
     * class by name instead of garbage digits. */
    if (!isfinite(v)) {
        snprintf(buf, len, "%s", isnan(v) ? "nan" : (v < 0.0f ? "-inf" : "inf"));
        return buf;
    }
    const char *sign = "";
    if (v < 0.0f) {
        sign = "-";
        v = -v;
    }
    unsigned scaled = (unsigned)(v * 10000.0f + 0.5f);
    snprintf(buf, len, "%s%u.%04u", sign, scaled / 10000u, scaled % 10000u);
    return buf;
}

/* Parse a complete, finite float from s. Rejects empty input, trailing garbage
 * (partial parses), and NaN/Inf. The NaN rejection matters: range checks of the
 * form (v < lo || v > hi) are both false for NaN, so without this a "nan" argument
 * would sail through validation and poison the AGC threshold comparisons (which
 * are then always false, silently disabling gain adjustment). */
bool parse_finite_float(const char *s, float *out) {
    char *end = nullptr;
    float v = strtof(s, &end);
    if (end == s || *end != '\0' || !isfinite(v)) {
        return false;
    }
    *out = v;
    return true;
}

/* AGC gain register → tenths of a dB, exactly (each step = 0.5 dB, 0x00 = −20 dB).
 * Integer math so it can be printed without %f. */
int agc_gain_db10(uint8_t gain) { return (int)gain * 5 - 200; }

/* Default AgcConfigProvider: identical defaults/clamps to the historical static
 * variables this replaces, used until sound_set_agc_config_provider() injects the real
 * BT-backed implementation (see audio_dsp_bind_default_bt_dependencies() below). */
class DefaultAgcConfigProvider : public AgcConfigProvider {
   public:
    float getTargetLow() override { return targetLow_; }
    void setTargetLow(float value) override { targetLow_ = std::clamp(value, 0.001f, 0.1f); }

    float getTargetHigh() override { return targetHigh_; }
    void setTargetHigh(float value) override { targetHigh_ = std::clamp(value, 0.001f, 0.2f); }

    uint32_t getRateLimitFrames() override { return rateLimitFrames_; }
    void setRateLimitFrames(uint32_t value) override {
        rateLimitFrames_ = std::clamp<uint32_t>(value, 1, 100);
    }

   private:
    float targetLow_ = 0.005f;
    float targetHigh_ = 0.008f;
    uint32_t rateLimitFrames_ = 10;
};

DefaultAgcConfigProvider sDefaultAgcProvider;
AgcConfigProvider *sAgcProvider = &sDefaultAgcProvider;
}  // namespace

void sound_set_agc_config_provider(AgcConfigProvider *provider) {
    sAgcProvider = provider ? provider : &sDefaultAgcProvider;
}

/* PDM gain register pointers — set once in configure_pdm(), used by AGC loop. */
static volatile uint32_t *s_gain_l;
static volatile uint32_t *s_gain_r;

static uint8_t s_agc_gain = 0x28;   /* current gain register value (0 dB) */
static bool s_agc_frozen = false;   /* debug: "sound agc freeze" halts gain adjustment */
static int s_agc_frames_since = 0;  /* frames elapsed since last adjustment */
static float s_latest_rms = 0.0f;   /* latest instantaneous RMS */
static float s_smoothed_rms = 0.0f; /* 1-second averaged RMS for AGC decisions */
static int16_t s_latest_peak = 0;   /* latest peak sample magnitude */

/* 1-second RMS history (32 frames at 32 ms/frame) */
#define AGC_HISTORY_LEN 32
static float s_rms_history[AGC_HISTORY_LEN];
static uint8_t s_rms_history_idx = 0;

/* Serializes agc_apply_gain() between its two callers (DSP thread's AGC loop,
 * shell thread's "sound agc gain") — the steps computation and the RMS-window
 * rescale are a multi-word read-modify-write that must not interleave, or the
 * detector compensation is fed a step count that doesn't match the real
 * amplitude change. */
K_MUTEX_DEFINE(s_agc_apply_mutex);

/* Apply a new AGC gain register value as one atomic operation: write the
 * hardware registers, carry the beat detector's previous-frame state across the
 * amplitude discontinuity (audio_dsp_compensate_gain_change; falls back to a
 * full history reset beyond ±4 steps), and rescale the AGC's own RMS window
 * into the new gain domain so the smoothed RMS tracks the change instantly
 * instead of lagging it for up to a second and triggering over-stepping.
 * Shared by the AGC loop (±1 steps) and the manual "sound agc gain" command
 * (arbitrary jumps). No-op if the gain is unchanged or PDM isn't configured.
 *
 * ORDERING: the caller must invoke this only AFTER audio_dsp_process() has
 * consumed the last block captured at the old gain — see the contract on
 * audio_dsp_compensate_gain_change(). */
static void agc_apply_gain(uint8_t new_gain) {
    k_mutex_lock(&s_agc_apply_mutex, K_FOREVER);
    int steps = (int)new_gain - (int)s_agc_gain;
    if (steps == 0 || s_gain_l == NULL || s_gain_r == NULL) {
        k_mutex_unlock(&s_agc_apply_mutex);
        return;
    }
    s_agc_gain = new_gain;
    *s_gain_l = s_agc_gain;
    *s_gain_r = s_agc_gain;
    /* Reset-per-step used to blind the detector for ~1 s after every step —
     * ~30% of all frames during music (issue #264, hardware-measured: 16 steps
     * in 30 s of ABGT at listening volume). */
    audio_dsp_compensate_gain_change(steps);
    if (steps > 4 || steps < -4) {
        /* Same rule as the detector side: a big manual jump is a genuine
         * discontinuity. Extrapolating the RMS window across e.g. +40 steps
         * would fabricate impossible levels (RMS is bounded by 1.0; ×10 scaling
         * would then drive the unfrozen loop right back down). Flush instead —
         * it refills within one second. */
        memset(s_rms_history, 0, sizeof(s_rms_history));
        s_smoothed_rms = 0.0f;
    } else {
        float amp = audio_dsp_gain_amplitude_ratio(steps);
        for (int i = 0; i < AGC_HISTORY_LEN; i++) {
            s_rms_history[i] *= amp;
        }
        s_smoothed_rms *= amp;
    }
    k_mutex_unlock(&s_agc_apply_mutex);
}

static float agc_compute_rms(const int16_t *pcm, uint32_t n) {
    float sum_sq = 0.0f;
    int16_t peak = 0;
    for (uint32_t i = 0; i < n; i++) {
        int16_t sample = pcm[i];
        int16_t abs_sample = (sample < 0) ? -sample : sample;
        if (abs_sample > peak)
            peak = abs_sample;

        float s = (float)sample * (1.0f / 32768.0f);
        sum_sq += s * s;
    }
    s_latest_peak = peak;

    return sqrtf(sum_sq / (float)n);
}

LOG_MODULE_REGISTER(sound);

K_MSGQ_DEFINE(audio_result_q, sizeof(struct audio_analysis_result), 4, 4);

#if defined(CONFIG_APP_AUDIO_DEBUG)
/* ── Audio tap (issue #264 debugging environment) ────────────────────────────
 * "sound mic record_wav" and "sound dump" must capture the EXACT PCM frames +
 * analysis results the DSP thread processed, seq-aligned, so an off-device
 * replay of the recorded WAV through the same audio_dsp.cpp can be compared
 * frame-for-frame against on-device behavior. The DSP thread stays the sole
 * dmic_read() consumer; when armed, it tees each frame into this queue and the
 * shell command drains it. (The previous record_wav implementation called
 * configure_pdm()/dmic_read() from the shell thread while the DSP thread had
 * the stream active — dmic_nrfx_pdm rejects configure-while-active with
 * -EBUSY, and two consumers would each have stolen half the blocks.)
 *
 * 16 entries ≈ 512 ms of buffering to ride out FAT write/erase stalls on the
 * shell thread (hardware-measured: flash sector erases stall the drain loop for
 * hundreds of ms; 8 entries dropped frames on every recording). On overflow the
 * frame is counted in s_tap_dropped, NOT purged: the recorder needs contiguity
 * accounting, not freshest-wins. */
struct audio_tap_frame {
    int16_t pcm[AUDIO_FFT_SIZE];
    struct audio_analysis_result result;
    float rms;
    uint8_t gain;
};
K_MSGQ_DEFINE(audio_tap_q, sizeof(struct audio_tap_frame), 16, 4);
static atomic_t s_tap_armed;
static atomic_t s_tap_dropped;
#endif /* CONFIG_APP_AUDIO_DEBUG */

/* Set once the DSP thread's capture loop is actually streaming; lets shell
 * commands distinguish "no frames yet" from "audio pipeline never started". */
static atomic_t s_dsp_running;

#if defined(CONFIG_VM3011)
const struct device *vm3011 = DEVICE_DT_GET(DT_NODELABEL(vm3011));
#endif
const struct device *pdm0 = DEVICE_DT_GET(DT_NODELABEL(pdm0));

// Number of PCM samples the driver will generate in 1s
#define SAMPLE_RATE_HZ 16000

// Sample size. Nordic PDM / DMIC / PCM pipeline only supports 16-bit samples
#define SAMPLE_BIT_WIDTH 16

// We will store the sample as an int16_t
#define BYTES_PER_SAMPLE sizeof(int16_t)  // (SAMPLE_BIT_WIDTH / 8) would be better

// How much time (in ms) is captured in each block?
#define BLOCK_CAPTURE_TIME_MS 32

// How many audio channels are we capturing? Nordic supports 1 or 2
// We only have 1 mic, so 1
#define NUM_AUDIO_CHANNELS 1

#define BLOCK_SIZE_HELPER(_sample_rate_hz, _number_of_channels, _block_time_ms)                    \
    (BYTES_PER_SAMPLE * ((float)_sample_rate_hz / ((float)MSEC_PER_SEC / (float)_block_time_ms)) * \
     _number_of_channels)

// Size of a block required to capture the specified amount of time of PCM samples
#define BLOCK_SIZE_FLOAT \
    BLOCK_SIZE_HELPER(SAMPLE_RATE_HZ, NUM_AUDIO_CHANNELS, BLOCK_CAPTURE_TIME_MS)

#define BLOCK_SIZE ((size_t)(BLOCK_SIZE_FLOAT))

// Verify that the float arithmetic in BLOCK_SIZE_HELPER produces a whole number of bytes.
// If the sample rate and capture time don't divide evenly, the result truncates silently
// when used as an integer (e.g. in K_MEM_SLAB_DEFINE_STATIC), dropping required bytes.
static_assert(BLOCK_SIZE == BLOCK_SIZE_FLOAT,
              "BLOCK_SIZE is not an integer — SAMPLE_RATE_HZ and BLOCK_CAPTURE_TIME_MS produce a "
              "fractional sample count; adjust them so (SAMPLE_RATE_HZ * BLOCK_CAPTURE_TIME_MS) is "
              "divisible by MSEC_PER_SEC");

// Number of blocks available to the driver
// MCU must keep up with the PCM system: reading block contents
// and freeing them so the driver can continue grabing more
#define BLOCK_COUNT DT_PROP(DT_NODELABEL(pdm0), queue_size)

// Alignment of the entries in the memory slab. Must be a power of 2, and the data size
// of the memory slab must also be a multiple of N
#define MEM_SLAB_ALIGNMENT BYTES_PER_SAMPLE
static_assert(BLOCK_SIZE % MEM_SLAB_ALIGNMENT == 0,
              "Block size must be a multiple of the alignment");

// Define the memory slab that the driver will grab blocks from to fill
K_MEM_SLAB_DEFINE_STATIC(mem_slab, BLOCK_SIZE,
                         BLOCK_COUNT + 1,  // Add an extra block to keep the driver happy
                         MEM_SLAB_ALIGNMENT);

// Milliseconds to wait for a block to be read by the driver
#define READ_TIMEOUT (BLOCK_CAPTURE_TIME_MS * 2)

static struct pcm_stream_cfg stream;
static struct dmic_cfg cfg;

void audio_dsp_thread_func(void *a, void *b, void *c);

// Stack size verified against real beat/fft_bars animation load (issue #75): high-water
// mark stayed at 692 B out of the previous 8096 B budget. 2048 B leaves ~3x margin
// (includes headroom for K_FP_REGS' FPU context save).
// (That rationale now also lives in CONFIG_APP_AUDIO_DSP_THREAD_STACK_SIZE's help text.)
// Kernel-only thread: K_KERNEL_* skips the 1KB CONFIG_USERSPACE privileged stack;
// this stack can never host a K_USER thread.
K_KERNEL_THREAD_DEFINE(audio_dsp_thread,
                       CONFIG_APP_AUDIO_DSP_THREAD_STACK_SIZE,  // stack size
                       audio_dsp_thread_func, NULL, NULL, NULL,
                       CONFIG_APP_AUDIO_DSP_THREAD_PRIORITY,  // Priority
                       K_FP_REGS,                             // Options
                       0                                      // Startup delay
);

// This thread was cooperative (-7) until issue #267. A running cooperative thread is never
// preempted, so a CMSIS-DSP FFT here stalled every rendering thread for its full duration.
// It must stay preemptible and ranked below both rendering threads.
BUILD_ASSERT(CONFIG_APP_AUDIO_DSP_THREAD_PRIORITY > CONFIG_APP_LED_DISPLAY_THREAD_PRIORITY &&
                 CONFIG_APP_AUDIO_DSP_THREAD_PRIORITY >
                     CONFIG_APP_PATTERN_CONTROLLER_THREAD_PRIORITY,
             "audio_dsp_thread must rank below both rendering threads");
BUILD_ASSERT(CONFIG_APP_AUDIO_DSP_THREAD_PRIORITY >= 0 &&
                 CONFIG_APP_AUDIO_DSP_THREAD_PRIORITY < CONFIG_NUM_PREEMPT_PRIORITIES,
             "CONFIG_APP_AUDIO_DSP_THREAD_PRIORITY must be a valid preemptible priority");

int configure_pdm() {
    if (!device_is_ready(pdm0)) {
        LOG_ERR("%s is not ready", pdm0->name);
        return -ENODEV;
    }

    // Information about the PCM stream we want the driver to create
    stream = {
        .pcm_rate = SAMPLE_RATE_HZ,
        .pcm_width = SAMPLE_BIT_WIDTH,
        .block_size = BLOCK_SIZE,
        .mem_slab = &mem_slab,
    };

    cfg = {
        .io =
            {
                /* These fields can be used to limit the PDM clock
                 * configurations that the driver is allowed to use
                 * to those supported by the microphone.
                 */
                .min_pdm_clk_freq = 1100000,  // 1.1Mhz
                .max_pdm_clk_freq = 3500000,  // 3.5Mhz
                .min_pdm_clk_dc = 40,
                .max_pdm_clk_dc = 60,
            },
        .streams = &stream,
        .channel =
            {
                .req_chan_map_lo = dmic_build_channel_map(
                    0,               // Channel number
                    0,               // PDM hardware controller number, always 0 on this board
                    PDM_CHAN_LEFT),  // microphone is configured as a left channel which
                                     // means it will emit data on the FALLING edge. We want the PDM
                                     // circuitry to read data on the RISING edge of the clock, so
                                     // we must tell it we are a RIGHT microphone
                .req_num_chan = NUM_AUDIO_CHANNELS,  // Requested number of audio channels
                .req_num_streams = 1,  // Nordic driver only supports a single PCM stream
            },
    };

    // cfg.channel.req_chan_map_lo |= dmic_build_channel_map(
    //     1, // Channel number
    //     0, // PDM hardware controller number, always 0 on this board
    //     PDM_CHAN_RIGHT);

    // Calculate the total recording buffer time
    LOG_INF("DMIC Configuration: sample rate: %u hz, sample bit width: %u", SAMPLE_RATE_HZ,
            SAMPLE_BIT_WIDTH);
    LOG_INF("DMIC Configuration: block size: %u bytes, num blocks: %u", BLOCK_SIZE, BLOCK_COUNT);
    LOG_INF("DMIC Configuration: total recording buffer %u ms",
            BLOCK_COUNT * BLOCK_CAPTURE_TIME_MS);

    int ret = dmic_configure(pdm0, &cfg);

    /* Store gain register pointers in file-scope so AGC can access them later.
     * Zephyr provides no public API for PDM gain, so we write the hardware directly. */
    s_gain_l = (volatile uint32_t *)(DT_REG_ADDR_RAW(DT_NODELABEL(pdm0)) + 0x518);
    s_gain_r = (volatile uint32_t *)(DT_REG_ADDR_RAW(DT_NODELABEL(pdm0)) + 0x51C);

    LOG_INF("Gain L Register Address: 0x%p", s_gain_l);
    LOG_INF("Gain R Register Address: 0x%p", s_gain_r);

    *s_gain_l = s_agc_gain;
    *s_gain_r = s_agc_gain;

    LOG_INF("Gain L Register Value: 0x%d", *s_gain_l);
    LOG_INF("Gain R Register Value: 0x%d", *s_gain_r);

    return ret;
}

void audio_dsp_thread_func(void *a, void *b, void *c) {
    if (!device_is_ready(pdm0)) {
        LOG_ERR("%s is not ready, cannot run audio DSP thread", pdm0->name);
        return;
    }

    int ret = configure_pdm();
    if (ret < 0) {
        LOG_ERR("Failed to configure PDM (%d), cannot run audio DSP thread", ret);
        return;
    }

    ret = dmic_trigger(pdm0, DMIC_TRIGGER_START);
    if (ret < 0) {
        LOG_ERR("DMIC START trigger failed: %d", ret);
        return;
    }

    audio_dsp_init();
    audio_dsp_bind_default_bt_dependencies();
    uint32_t seq = 0;
    atomic_set(&s_dsp_running, 1);

    int consecutive_failures = 0;

    while (true) {
        void *buffer = NULL;
        uint32_t size = 0;

        ret = dmic_read(pdm0, 0, &buffer, &size, READ_TIMEOUT);
        if (ret) {
            LOG_ERR("Failed to read block %d", ret);
            if (buffer != NULL) {
                k_mem_slab_free(&mem_slab, buffer);
            }
            /* Self-heal a dead PDM stream (hardware-observed: a long enough
             * scheduling stall — e.g. sustained QSPI writes plus host USB-MSC
             * reads — overruns the driver's DMA queue and dmic_nrfx_pdm parks
             * itself in an error state; every subsequent read returns -EAGAIN
             * forever). STOP resets the driver's state machine; if START still
             * fails, fall back to a full reconfigure. */
            if (++consecutive_failures >= 3) {
                LOG_WRN("PDM stream appears dead; attempting restart");
                dmic_trigger(pdm0, DMIC_TRIGGER_STOP);
                ret = dmic_trigger(pdm0, DMIC_TRIGGER_START);
                if (ret < 0) {
                    ret = configure_pdm();
                    if (ret == 0) {
                        ret = dmic_trigger(pdm0, DMIC_TRIGGER_START);
                    }
                    LOG_WRN("PDM reconfigure+restart: %d", ret);
                }
                consecutive_failures = 0;
                /* The restart gap is an amplitude discontinuity like a gain step. */
                audio_dsp_reset_history();
            }
            continue;
        }
        consecutive_failures = 0;

        const int16_t *pcm = static_cast<const int16_t *>(buffer);

        /* AGC levels for this block. */
        float rms = agc_compute_rms(pcm, AUDIO_FFT_SIZE);
        s_latest_rms = rms; /* Instantaneous RMS for diagnostics */

        /* Process THIS block before any gain change is applied: the block was
         * captured at the CURRENT gain, so the detector's previous-frame state
         * (same domain) stays consistent. Applying the step first fed an
         * old-domain block against new-domain state — a false flux of
         * ~0.115/step, i.e. a spurious beat on every AGC step (PR #277 review).
         * The gain decision therefore moves BELOW audio_dsp_process(). */
        struct audio_analysis_result result;
        audio_dsp_process(pcm, seq++, &result);

#if defined(CONFIG_APP_AUDIO_DEBUG)
        if (atomic_get(&s_tap_armed)) {
            /* Static, not stack: the frame is ~1.2 KB and this thread's stack is 2 KB.
             * Safe because this thread is the only producer. */
            static struct audio_tap_frame tap;
            memcpy(tap.pcm, pcm, sizeof(tap.pcm));
            tap.result = result;
            tap.rms = rms;
            tap.gain = s_agc_gain; /* pre-step: the gain this block was captured at */
            if (k_msgq_put(&audio_tap_q, &tap, K_NO_WAIT) != 0) {
                atomic_inc(&s_tap_dropped);
            }
        }
#endif

        k_mem_slab_free(&mem_slab, buffer);

        /* AGC decision + application — AFTER the block was processed (see the
         * ordering note above). The register write lands mid-capture of the
         * next DMA block, so that one transitional block is a bounded mix of
         * old/new gain (≤ 0.5 dB across it) — far below the full-step error
         * this ordering removes.
         *
         * Update the 1-second RMS window (same ring structure as the beat
         * detector's flux history). */
        s_rms_history[s_rms_history_idx] = rms;
        s_rms_history_idx = (s_rms_history_idx + 1) % AGC_HISTORY_LEN;
        float sum_rms = 0.0f;
        for (int i = 0; i < AGC_HISTORY_LEN; i++) {
            sum_rms += s_rms_history[i];
        }
        s_smoothed_rms = sum_rms / (float)AGC_HISTORY_LEN;

        /* Check for gain adjustment every getRateLimitFrames() frames.
         * "sound agc freeze" (debug) skips adjustment entirely so recordings can be
         * made at a known fixed gain. */
        s_agc_frames_since++;
        if (!s_agc_frozen &&
            static_cast<uint32_t>(s_agc_frames_since) >= sAgcProvider->getRateLimitFrames()) {
            uint8_t target_gain = s_agc_gain;

            /* Use smoothed RMS (1-second average) for stable decisions. */
            if (s_smoothed_rms < sAgcProvider->getTargetLow() && s_agc_gain < AGC_GAIN_MAX) {
                target_gain++;
            } else if (s_smoothed_rms > sAgcProvider->getTargetHigh() &&
                       s_agc_gain > AGC_GAIN_MIN) {
                target_gain--;
            }

            if (target_gain != s_agc_gain) {
                agc_apply_gain(target_gain);
                s_agc_frames_since = 0;
                int db10 = agc_gain_db10(s_agc_gain);
                char rms_buf[16];
                LOG_DBG("AGC: gain=0x%02x (%s%d.%u dB) smoothed_rms=%s", s_agc_gain,
                        db10 < 0 ? "-" : "", abs(db10) / 10, (unsigned)(abs(db10) % 10),
                        fmt_fixed4(s_smoothed_rms, rms_buf, sizeof(rms_buf)));
            }
        }

        // Log beats including noise-floor stats for threshold tuning
        for (int b = 0; b < AUDIO_NUM_BANDS; b++) {
            // Disabled output for now
            if (false && result.beat[b]) {
                char e_buf[16], m_buf[16], sg_buf[16], t_buf[16];
                LOG_INF(
                    "beat band=%d energy=%s mean=%s sigma=%s "
                    "threshold=%s seq=%u",
                    b, fmt_fixed4(result.band_energy[b], e_buf, sizeof(e_buf)),
                    fmt_fixed4(result.band_mean[b], m_buf, sizeof(m_buf)),
                    fmt_fixed4(result.band_sigma[b], sg_buf, sizeof(sg_buf)),
                    fmt_fixed4(result.band_mean[b] + 2.0f * result.band_sigma[b], t_buf,
                               sizeof(t_buf)),
                    result.seq);
            }
        }

        // Publish result; drop oldest if the queue is full
        if (k_msgq_put(&audio_result_q, &result, K_NO_WAIT) == -ENOMSG) {
            k_msgq_purge(&audio_result_q);
            k_msgq_put(&audio_result_q, &result, K_NO_WAIT);
        }
    }
}

/*
// OG sound recording function: never really worked on the DevKit board, superceeded by new
// recording-to-flash function
static int cmd_sound_mic_record(const struct shell *shell,
                                size_t argc, char **argv, void *data)
{
    int ret;

    if (!device_is_ready(pdm0))
    {
        shell_error(shell, "%s is not ready", pdm0->name);
        return -ENOEXEC;
    }

    ret = configure_pdm();
    if (ret < 0)
    {
        shell_error(shell, "Failed to configure the driver: %d", ret);
        return ret;
    }

    shell_print(shell, "PCM output rate: %u, channels: %u",
                cfg.streams[0].pcm_rate, cfg.channel.req_num_chan);

    ret = dmic_trigger(pdm0, DMIC_TRIGGER_START);
    if (ret < 0)
    {
        shell_error(shell, "START trigger failed: %d", ret);
        return ret;
    }

    shell_print(shell, "*** START PCM DATA ***");

    for (size_t i = 0; i < BLOCK_COUNT; i++)
    {
        void *buffer;
        uint32_t size;

        ret = dmic_read(
            pdm0,
            0, // Stream ID
            &buffer,
            &size,
            READ_TIMEOUT);

        if (ret)
        {
            shell_error(shell, "*** Failed to read block sample ID %u: %d", i, ret);
            continue;
        }

        shell_hexdump(shell, reinterpret_cast<const uint8_t *>(buffer), size);

        k_mem_slab_free(&mem_slab, buffer);
    }

    shell_print(shell, "*** STOP PCM DATA ***");

    // Stop the driver just in case
    ret = dmic_trigger(pdm0, DMIC_TRIGGER_STOP);
    if (ret < 0)
    {
        shell_error(shell, "STOP trigger failed: %d", ret);
        return ret;
    }

    return 0;
}
*/

#if defined(CONFIG_VM3011)
static int cmd_sound_vm_dump(const struct shell *shell, size_t argc, char **argv, void *data) {
    vm3011_dump(vm3011);
    return 0;
}

static int cmd_sound_vm_clear(const struct shell *shell, size_t argc, char **argv, void *data) {
    vm3011_clear_dout(vm3011);
    return 0;
}

// Subcommands for "sound vm"
SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_sound_vm, SHELL_CMD(dump, NULL, "Dump VM3011 Registers to console", cmd_sound_vm_dump),
    SHELL_CMD(clear, NULL, "Clear VM3011 DOUT pin", cmd_sound_vm_clear), SHELL_SUBCMD_SET_END);
#endif  // defined(CONFIG_VM3011)

// WAV file header layout (44 bytes, little-endian PCM)
struct __attribute__((packed)) wav_header {
    char riff_id[4];        // "RIFF"
    uint32_t file_size;     // total bytes after this field
    char wave_id[4];        // "WAVE"
    char fmt_id[4];         // "fmt "
    uint32_t fmt_size;      // 16 for PCM
    uint16_t audio_format;  // 1 = PCM
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;    // sample_rate * num_channels * bytes_per_sample
    uint16_t block_align;  // num_channels * bytes_per_sample
    uint16_t bits_per_sample;
    char data_id[4];     // "data"
    uint32_t data_size;  // raw PCM byte count
};

#if defined(CONFIG_APP_AUDIO_DEBUG)
/* PCM payload alignment for record_wav. The FAT volume has 4096-byte sectors and
 * Zephyr builds FatFS with FF_FS_TINY=1 (one shared sector window for ALL open
 * files — zephyr_fatfs_config.h). A canonical 44-byte header leaves every batched
 * PCM write misaligned, forcing a read-modify-write through that shared window per
 * sector, made worse by the sidecar CSV file competing for the same window. A
 * standard RIFF "JUNK" padding chunk between "fmt " and "data" pushes the PCM
 * payload to exactly offset 4096, so the sector-multiple batch writes take FatFS's
 * direct whole-sector path and never touch the window. All RIFF readers skip JUNK
 * chunks (Python's wave module, librosa, and the replay harness's chunk walker).
 *
 * Layout: RIFF(12) + fmt(24) + JUNK(8 + WAV_JUNK_PAD) + data hdr(8) = 4096. */
#define WAV_DATA_OFFSET 4096
#define WAV_JUNK_PAD (WAV_DATA_OFFSET - 12 - 24 - 8 - 8)
#endif /* CONFIG_APP_AUDIO_DEBUG */

#define DEFAULT_WAV_PATH "/NAND:/sound.wav"
#define DEFAULT_RECORD_DURATION_S 10

#if defined(CONFIG_APP_AUDIO_DEBUG)
/* Tap frame text format: field order lives in audio_tap_format.h (shared with
 * the host replay harness so the two producers can never drift; decoder in
 * fw/tools/beat_lab/frames.py). The wrappers below just bind the firmware's
 * data sources (tap frame struct, config providers, AGC state). */
static size_t tap_frame_format(const struct audio_tap_frame *f, bool buckets, char *buf,
                               size_t cap) {
    return audio_tap_format_frame(&f->result, f->rms, f->gain, buckets, buf, cap);
}

/* Renders the #PARAMS snapshot line (no trailing newline); returns its length.
 * Params can be changed at runtime (BLE/shell) — captures assume they stay
 * stable for the duration; this snapshot is what the replay harness replays with. */
static size_t tap_params_format(char *buf, size_t cap) {
    AudioDspConfigProvider *dsp = audio_dsp_get_config_provider();
    return audio_tap_format_params(dsp->getFluxGamma(), dsp->getBeatAlpha(),
                                   dsp->getBeatFluxFloor(), dsp->getBeatRefractoryFrames(),
                                   s_agc_frozen, s_agc_gain, sAgcProvider->getTargetLow(),
                                   sAgcProvider->getTargetHigh(),
                                   sAgcProvider->getRateLimitFrames(), buf, cap);
}

/* Shell-side drain buffers, shared by record_wav and dump: too big for the shell
 * thread's stack, and safe as statics because only one shell command runs at a time. */
static struct audio_tap_frame s_tap_drain;
static char s_tap_line[512];

/* Write len bytes at absolute offset `pos`, retrying across transient disk errors.
 * Sustained recording occasionally hits a QSPI-level hiccup (hardware-observed:
 * fs_write returns -EIO after 12-20 s of continuous writes; the nordic,qspi-nor
 * driver maps an internal busy/timeout into a failed WREN → FatFS FR_DISK_ERR).
 * FatFS then latches a sticky error flag on the FIL object, so every later
 * fs_write/fs_seek fails instantly — the only way to continue is close + reopen
 * (clears the flag) + seek back to the tracked offset. The tap queue holds ~512 ms
 * of frames, so a brief retry pause need not drop anything. */
static int tap_write_at_retry(struct fs_file_t *f, const char *path, off_t pos,
                              const void *buf, size_t len, uint32_t *io_retries) {
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt > 0 || fs_tell(f) != pos) {
            if (fs_seek(f, pos, FS_SEEK_SET) < 0) {
                goto reopen;
            }
        }
        if (fs_write(f, buf, len) == (ssize_t)len) {
            return 0;
        }
    reopen:
        (*io_retries)++;
        fs_close(f);
        k_msleep(50);
        if (fs_open(f, path, FS_O_WRITE) < 0) {
            return -EIO;
        }
    }
    return -EIO;
}

/* record_wav write batching. The NAND FAT volume has 4096-byte sectors; writing one
 * 1024 B PCM block (and a ~170 B CSV line to a SECOND file) per 32 ms frame forces a
 * read-modify-write of a 4 KB sector per call, alternating between two cluster chains
 * — measured on hardware at ~6x slower than real time (93 frames captured, 448
 * dropped for a 3 s recording). Batching both streams to sector-sized writes keeps
 * the drain loop faster than the 31.25 fps producer. */
#define TAP_WAV_BATCH_FRAMES 8 /* 8 x 1024 B = two FAT sectors per write */
static int16_t s_wav_batch[TAP_WAV_BATCH_FRAMES * AUDIO_FFT_SIZE];
/* CSV accumulator: flushed in exact 4096-byte (sector) chunks, with headroom for
 * the line that overflows the sector boundary. The #PARAMS header line is padded
 * to a full sector so every chunk lands sector-aligned — hardware-measured: with a
 * misaligned CSV stream, each flush paid the FF_FS_TINY shared-window RMW penalty
 * and stalled the drain loop ~545 ms at a ~25-frame cadence (1 dropped frame per
 * flush); sector-aligned flushes eliminate those drops. */
#define TAP_CSV_CHUNK 4096
static char s_csv_batch[TAP_CSV_CHUNK + sizeof(s_tap_line)];

/* Tap-based recording: the DSP thread stays the sole dmic_read() consumer and we
 * drain its tap (see the audio_tap_frame comment block), capturing the exact
 * frames the detector analyzed plus the per-frame sidecar CSV. */
static int record_wav_tap(const struct shell *shell, uint32_t duration_s, const char *path) {
    int ret;

    const uint32_t total_frames = (duration_s * MSEC_PER_SEC) / BLOCK_CAPTURE_TIME_MS;

    /* Fail early if the volume can't hold the whole capture: WAV (1024 B/frame)
     * + sidecar CSV (~175 B/frame) + both prologues + 64 KB slack for FAT
     * metadata and whatever else writes to the disk meanwhile. Running out
     * mid-capture would burn the write-retry path on -ENOSPC (which can never
     * clear) and leave the NAND full. */
    struct fs_statvfs vfs;
    if (fs_statvfs("/NAND:", &vfs) == 0) {
        uint64_t free_bytes = (uint64_t)vfs.f_bfree * vfs.f_frsize;
        uint64_t needed = (uint64_t)total_frames * (BLOCK_SIZE + 175) + WAV_DATA_OFFSET +
                          TAP_CSV_CHUNK + 64 * 1024;
        if (needed > free_bytes) {
            uint32_t max_s =
                (uint32_t)((free_bytes > WAV_DATA_OFFSET + TAP_CSV_CHUNK + 64 * 1024)
                               ? (free_bytes - WAV_DATA_OFFSET - TAP_CSV_CHUNK - 64 * 1024) /
                                     ((BLOCK_SIZE + 175) * (MSEC_PER_SEC / BLOCK_CAPTURE_TIME_MS))
                               : 0);
            shell_error(shell, "Not enough free space for %u s (max ~%u s free)", duration_s,
                        max_s);
            return -ENOSPC;
        }
    }

    char csv_path[96];
    int n = snprintf(csv_path, sizeof(csv_path), "%s.csv", path);
    if (n < 0 || n >= (int)sizeof(csv_path)) {
        shell_error(shell, "Path too long: %s", path);
        return -EINVAL;
    }

    struct fs_file_t f;
    struct fs_file_t fcsv;
    fs_file_t_init(&f);
    fs_file_t_init(&fcsv);
    ret = fs_open(&f, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
    if (ret < 0) {
        shell_error(shell, "Failed to open %s: %d", path, ret);
        return ret;
    }
    ret = fs_open(&fcsv, csv_path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
    if (ret < 0) {
        shell_error(shell, "Failed to open %s: %d", csv_path, ret);
        fs_close(&f);
        return ret;
    }

    // Write a placeholder header; sizes will be patched after recording.
    // The prologue is one full 4096-byte sector (see WAV_DATA_OFFSET above):
    // RIFF+fmt, then a JUNK padding chunk, then the data chunk header, so the
    // PCM payload starts sector-aligned.
    struct wav_header hdr = {
        .riff_id = {'R', 'I', 'F', 'F'},
        .file_size = 0,
        .wave_id = {'W', 'A', 'V', 'E'},
        .fmt_id = {'f', 'm', 't', ' '},
        .fmt_size = 16,
        .audio_format = 1,
        .num_channels = NUM_AUDIO_CHANNELS,
        .sample_rate = SAMPLE_RATE_HZ,
        .byte_rate = SAMPLE_RATE_HZ * NUM_AUDIO_CHANNELS * BYTES_PER_SAMPLE,
        .block_align = NUM_AUDIO_CHANNELS * BYTES_PER_SAMPLE,
        .bits_per_sample = SAMPLE_BIT_WIDTH,
        .data_id = {'d', 'a', 't', 'a'},
        .data_size = 0,
    };

    memset(s_csv_batch, 0, sizeof(s_csv_batch)); /* not in CSV use yet - free scratch */
    memcpy(&s_csv_batch[0], &hdr, 36);           /* RIFF(12) + fmt(24), no data hdr yet */
    const uint32_t junk_size = WAV_JUNK_PAD;
    memcpy(&s_csv_batch[36], "JUNK", 4);
    memcpy(&s_csv_batch[40], &junk_size, 4);
    /* bytes 44..4087 stay zero (JUNK payload) */
    memcpy(&s_csv_batch[WAV_DATA_OFFSET - 8], "data", 4);
    /* data_size placeholder at WAV_DATA_OFFSET - 4 stays zero */

    ret = fs_write(&f, s_csv_batch, WAV_DATA_OFFSET);
    if (ret != WAV_DATA_OFFSET) {
        shell_error(shell, "Failed to write WAV header: %d", ret);
        fs_close(&f);
        fs_close(&fcsv);
        return -EIO;
    }

    /* #PARAMS line, space-padded to one full sector so all following CSV chunk
     * writes are sector-aligned (see s_csv_batch comment). Checked: a short write
     * here would silently desync csv_file_pos from the file's real size, corrupting
     * every later positioned write. */
    size_t len = tap_params_format(s_csv_batch, TAP_CSV_CHUNK - 1);
    memset(&s_csv_batch[len], ' ', TAP_CSV_CHUNK - 1 - len);
    s_csv_batch[TAP_CSV_CHUNK - 1] = '\n';
    if (fs_write(&fcsv, s_csv_batch, TAP_CSV_CHUNK) != TAP_CSV_CHUNK) {
        shell_error(shell, "Failed to write CSV header");
        fs_close(&f);
        fs_close(&fcsv);
        return -EIO;
    }
    off_t csv_file_pos = TAP_CSV_CHUNK; /* tracked for tap_write_at_retry */
    off_t wav_pos = WAV_DATA_OFFSET;
    uint32_t io_retries = 0;

    if (!s_agc_frozen) {
        shell_warn(shell,
                   "AGC is not frozen - gain may step mid-recording. For replay-comparable "
                   "captures run 'sound agc freeze on' (or 'sound agc gain <v>') first.");
    }

    /* Arm the tap with compare-and-swap so two concurrent shell sessions can't
     * both pass a check-then-act gap and drain each other's frames. */
    if (!atomic_cas(&s_tap_armed, 0, 1)) {
        shell_error(shell, "Tap already armed (another capture in progress?)");
        fs_close(&f);
        fs_close(&fcsv);
        return -EBUSY;
    }

    shell_print(shell, "Recording %u s (%u frames) to %s + %s ...", duration_s, total_frames,
                path, csv_path);

    // Drain from a clean queue (a frame produced between arm and purge is discarded
    // harmlessly - seq alignment comes from the frames themselves, not the arm time)
    k_msgq_purge(&audio_tap_q);
    atomic_set(&s_tap_dropped, 0);

    uint32_t total_bytes = 0;
    uint32_t frames_captured = 0;
    uint32_t wav_batched = 0;          /* frames accumulated in s_wav_batch */
    size_t csv_pos = 0;                /* bytes accumulated in s_csv_batch */
    uint32_t consecutive_timeouts = 0; /* tap-empty polls in a row */
    bool io_error = false;

    while (frames_captured < total_frames) {
        ret = k_msgq_get(&audio_tap_q, &s_tap_drain, K_MSEC(1000));
        if (ret != 0) {
            /* The producer can legitimately stall for a moment (flash-erase
             * lockouts, the PDM self-heal restart) — dropped frames are already
             * accounted by the tap, so ride it out and only abort when the
             * stream looks genuinely dead. */
            if (++consecutive_timeouts >= 5) {
                shell_error(shell, "Tap produced nothing for 5 s - aborting at frame %u (%d)",
                            frames_captured, ret);
                io_error = true;
                break;
            }
            continue;
        }
        consecutive_timeouts = 0;

        memcpy(&s_wav_batch[wav_batched * AUDIO_FFT_SIZE], s_tap_drain.pcm,
               sizeof(s_tap_drain.pcm));
        wav_batched++;
        if (wav_batched == TAP_WAV_BATCH_FRAMES) {
            if (tap_write_at_retry(&f, path, wav_pos, s_wav_batch, sizeof(s_wav_batch),
                                   &io_retries) != 0) {
                shell_error(shell, "WAV write failed at frame %u (after retries)",
                            frames_captured);
                io_error = true;
                break;
            }
            wav_pos += (off_t)sizeof(s_wav_batch);
            total_bytes += sizeof(s_wav_batch);
            wav_batched = 0;
        }

        len = tap_frame_format(&s_tap_drain, false, s_tap_line, sizeof(s_tap_line) - 1);
        s_tap_line[len] = '\n';
        memcpy(&s_csv_batch[csv_pos], s_tap_line, len + 1);
        csv_pos += len + 1;
        if (csv_pos >= TAP_CSV_CHUNK) {
            /* Flush exactly one sector; carry the overflow to the front. A line
             * may straddle the boundary — that's fine, it's one byte stream. */
            if (tap_write_at_retry(&fcsv, csv_path, csv_file_pos, s_csv_batch, TAP_CSV_CHUNK,
                                   &io_retries) != 0) {
                shell_error(shell, "CSV write failed at frame %u (after retries)",
                            frames_captured);
                io_error = true;
                break;
            }
            csv_file_pos += TAP_CSV_CHUNK;
            csv_pos -= TAP_CSV_CHUNK;
            memmove(s_csv_batch, &s_csv_batch[TAP_CSV_CHUNK], csv_pos);
        }
        frames_captured++;
    }

    atomic_set(&s_tap_armed, 0);
    uint32_t dropped = (uint32_t)atomic_get(&s_tap_dropped);

    /* Flush partial batches (frames that didn't fill a whole sector). */
    if (!io_error && wav_batched > 0) {
        size_t tail = wav_batched * sizeof(s_tap_drain.pcm);
        if (tap_write_at_retry(&f, path, wav_pos, s_wav_batch, tail, &io_retries) == 0) {
            total_bytes += tail;
        } else {
            shell_error(shell, "WAV write failed on final flush");
        }
    }
    if (!io_error && csv_pos > 0) {
        if (tap_write_at_retry(&fcsv, csv_path, csv_file_pos, s_csv_batch, csv_pos,
                               &io_retries) != 0) {
            shell_error(shell, "CSV write failed on final flush");
        } else {
            csv_file_pos += (off_t)csv_pos;
        }
    }

    // Patch the two size fields: RIFF file_size at offset 4, data_size at
    // WAV_DATA_OFFSET - 4 (see the prologue layout above). Use the retry helper —
    // a transient disk error during the drain leaves the FIL error flag set, and
    // these patches are what make the WAV readable at all — so a failure here must
    // be reported, not swallowed.
    uint32_t file_size = WAV_DATA_OFFSET - 8 + total_bytes;  // -8: RIFF id + size field
    bool patch_ok =
        tap_write_at_retry(&f, path, 4, &file_size, sizeof(file_size), &io_retries) == 0;
    patch_ok = tap_write_at_retry(&f, path, WAV_DATA_OFFSET - 4, &total_bytes,
                                  sizeof(total_bytes), &io_retries) == 0 &&
               patch_ok;
    if (!patch_ok) {
        shell_error(shell, "WAV header patch failed - the file will not parse as WAV");
    }
    fs_close(&f);

    len = (size_t)snprintf(s_tap_line, sizeof(s_tap_line), "#DONE frames=%u dropped=%u\n",
                           frames_captured, dropped);
    if (tap_write_at_retry(&fcsv, csv_path, csv_file_pos, s_tap_line, len, &io_retries) != 0) {
        shell_error(shell, "CSV #DONE trailer write failed");
    }
    fs_close(&fcsv);

    /* A capture that aborted must not look like a success: the files above were
     * finalized (headers patched, #DONE written) so what WAS captured stays
     * parseable, but the caller — including the MCP plugin, which keys on the
     * "Wrote ..." line — has to see a failure. */
    if (io_error) {
        shell_error(shell,
                    "ABORTED: capture incomplete - %u of %u frames saved to %s "
                    "(%u dropped, %u io retries)",
                    frames_captured, total_frames, path, dropped, io_retries);
        return -EIO;
    }

    shell_print(shell, "Wrote %u bytes of PCM to %s (%u frames, %u dropped, %u io retries)",
                total_bytes, path, frames_captured, dropped, io_retries);
    if (dropped != 0) {
        shell_warn(shell, "Dropped frames leave gaps: WAV and seq numbers will not be "
                          "contiguous - prefer a re-record for replay comparison");
    }
    return 0;
}

static int cmd_sound_dump(const struct shell *shell, size_t argc, char **argv) {
    // argv[1] = frame count, argv[2] = optional "buckets" to append display buckets
    uint32_t frames = (uint32_t)strtoul(argv[1], NULL, 10);
    if (frames == 0 || frames > 100000) {
        shell_error(shell, "Frame count must be in [1, 100000]");
        return -EINVAL;
    }
    bool buckets = false;
    if (argc > 2) {
        if (strcmp(argv[2], "buckets") != 0) {
            shell_error(shell, "Usage: sound dump <frames> [buckets]");
            return -EINVAL;
        }
        buckets = true;
    }
    if (!atomic_get(&s_dsp_running)) {
        shell_error(shell, "Audio DSP thread is not streaming");
        return -ENOEXEC;
    }
    /* Compare-and-swap arm — same rationale as record_wav. */
    if (!atomic_cas(&s_tap_armed, 0, 1)) {
        shell_error(shell, "Tap already armed (another capture in progress?)");
        return -EBUSY;
    }

    size_t len = tap_params_format(s_tap_line, sizeof(s_tap_line));
    ARG_UNUSED(len);
    shell_print(shell, "%s", s_tap_line);

    k_msgq_purge(&audio_tap_q);
    atomic_set(&s_tap_dropped, 0);

    uint32_t captured = 0;
    for (uint32_t i = 0; i < frames; i++) {
        if (k_msgq_get(&audio_tap_q, &s_tap_drain, K_MSEC(1000)) != 0) {
            shell_error(shell, "Tap timed out at frame %u", i);
            break;
        }
        tap_frame_format(&s_tap_drain, buckets, s_tap_line, sizeof(s_tap_line));
        shell_print(shell, "%s", s_tap_line);
        captured++;
    }

    atomic_set(&s_tap_armed, 0);
    shell_print(shell, "#DONE frames=%u dropped=%u", captured,
                (uint32_t)atomic_get(&s_tap_dropped));
    return 0;
}
#endif /* CONFIG_APP_AUDIO_DEBUG */

/* Direct-capture fallback: used when the DSP thread is not streaming (e.g. it
 * failed at boot and you want raw mic data to diagnose why, or the tap recorder
 * is compiled out via CONFIG_APP_AUDIO_DEBUG=n). Configures and drives the PDM
 * stream itself — safe exactly because no other dmic_read() consumer exists —
 * and writes a plain 44-byte-header WAV with no analysis sidecar (nothing is
 * computing analysis). Keeps mic capture available in every build/failure
 * combination instead of gating the board's only capture command on the debug
 * tap. */
static int record_wav_direct(const struct shell *shell, uint32_t duration_s, const char *path) {
    int ret;
    const uint32_t total_blocks = (duration_s * MSEC_PER_SEC) / BLOCK_CAPTURE_TIME_MS;

    if (!device_is_ready(pdm0)) {
        shell_error(shell, "%s is not ready", pdm0->name);
        return -ENOEXEC;
    }
    ret = configure_pdm();
    if (ret < 0) {
        shell_error(shell, "Failed to configure the driver: %d", ret);
        return ret;
    }

    struct fs_file_t f;
    fs_file_t_init(&f);
    ret = fs_open(&f, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
    if (ret < 0) {
        shell_error(shell, "Failed to open %s: %d", path, ret);
        return ret;
    }

    // Placeholder header; sizes patched after recording (canonical 44-byte
    // layout — no sector-alignment JUNK chunk: this path has no CSV competing
    // for the FatFS window and diagnostic captures tolerate slower writes).
    struct wav_header hdr = {
        .riff_id = {'R', 'I', 'F', 'F'},
        .file_size = 0,
        .wave_id = {'W', 'A', 'V', 'E'},
        .fmt_id = {'f', 'm', 't', ' '},
        .fmt_size = 16,
        .audio_format = 1,
        .num_channels = NUM_AUDIO_CHANNELS,
        .sample_rate = SAMPLE_RATE_HZ,
        .byte_rate = SAMPLE_RATE_HZ * NUM_AUDIO_CHANNELS * BYTES_PER_SAMPLE,
        .block_align = NUM_AUDIO_CHANNELS * BYTES_PER_SAMPLE,
        .bits_per_sample = SAMPLE_BIT_WIDTH,
        .data_id = {'d', 'a', 't', 'a'},
        .data_size = 0,
    };
    ret = fs_write(&f, &hdr, sizeof(hdr));
    if (ret != sizeof(hdr)) {
        shell_error(shell, "Failed to write WAV header: %d", ret);
        fs_close(&f);
        return -EIO;
    }

    ret = dmic_trigger(pdm0, DMIC_TRIGGER_START);
    if (ret < 0) {
        shell_error(shell, "START trigger failed: %d", ret);
        fs_close(&f);
        return ret;
    }

    shell_print(shell, "Recording %u s to %s (direct capture, no analysis sidecar) ...",
                duration_s, path);

    uint32_t total_bytes = 0;
    for (uint32_t i = 0; i < total_blocks; i++) {
        void *buffer = NULL;
        uint32_t size = 0;

        ret = dmic_read(pdm0, 0, &buffer, &size, READ_TIMEOUT);
        if (ret) {
            shell_error(shell, "Failed to read block %u: %d", i, ret);
            if (buffer != NULL) {
                k_mem_slab_free(&mem_slab, buffer);
            }
            continue;
        }

        ssize_t written = fs_write(&f, buffer, size);
        if (written != (ssize_t)size) {
            shell_error(shell, "Short write on block %u (%d of %u bytes)", i, (int)written,
                        size);
        } else {
            total_bytes += size;
        }

        k_mem_slab_free(&mem_slab, buffer);
    }

    dmic_trigger(pdm0, DMIC_TRIGGER_STOP);

    // Patch the two size fields in the header
    hdr.data_size = total_bytes;
    hdr.file_size = sizeof(hdr) - 8 + total_bytes;  // -8: RIFF id + file_size itself
    fs_seek(&f, 0, FS_SEEK_SET);
    fs_write(&f, &hdr, sizeof(hdr));
    fs_close(&f);

    shell_print(shell, "Wrote %u bytes of PCM to %s", total_bytes, path);
    return 0;
}

static int cmd_sound_mic_record_wav(const struct shell *shell, size_t argc, char **argv,
                                    void *data) {
    // argv[1] = optional duration in seconds, argv[2] = optional output path
    uint32_t duration_s = DEFAULT_RECORD_DURATION_S;
    const char *path = DEFAULT_WAV_PATH;

    if (argc > 1) {
        duration_s = (uint32_t)strtoul(argv[1], NULL, 10);
        /* Upper bound: WAV + sidecar CSV together consume ~37 KB/s against the
         * ~6.9 MB volume, so ~180 s is the honest ceiling; the free-space check
         * in record_wav_tap() enforces the actual headroom. */
        if (duration_s == 0 || duration_s > 180) {
            shell_error(shell, "Duration must be in [1, 180] seconds");
            return -EINVAL;
        }
    }
    if (argc > 2) {
        path = argv[2];
    }

#if defined(CONFIG_APP_AUDIO_DEBUG)
    if (atomic_get(&s_dsp_running)) {
        return record_wav_tap(shell, duration_s, path);
    }
    shell_warn(shell,
               "DSP thread not streaming - falling back to direct capture (no analysis "
               "sidecar)");
#else
    if (atomic_get(&s_dsp_running)) {
        shell_error(shell, "DSP thread owns the PDM stream and the tap recorder is compiled "
                           "out - rebuild with CONFIG_APP_AUDIO_DEBUG=y");
        return -ENOTSUP;
    }
#endif
    return record_wav_direct(shell, duration_s, path);
}

// Subcommands for "sound mic"
SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_sound_mic,
    /*SHELL_CMD(record, NULL, "Record sound to console (hex)", cmd_sound_mic_record),*/
    SHELL_CMD_ARG(record_wav, NULL,
                  "Record sound to WAV file [duration_s] [path] (+ analysis .csv sidecar "
                  "when the DSP tap is available)",
                  cmd_sound_mic_record_wav, 0, 2),
    SHELL_SUBCMD_SET_END);

static int cmd_sound_agc_status(const struct shell *shell, size_t argc, char **argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    /* Each register step = 0.5 dB; 0x00 = −20 dB, 0x28 = 0 dB, 0x50 = +20 dB. */
    int db10 = agc_gain_db10(s_agc_gain);
    float peak_norm = (float)s_latest_peak / 32768.0f;
    char b1[16], b2[16];
    shell_print(shell, "AGC gain: 0x%02x (%s%d.%u dB)", s_agc_gain, db10 < 0 ? "-" : "",
                abs(db10) / 10, (unsigned)(abs(db10) % 10));
    shell_print(shell, "  Smoothed RMS (1s): %s | Instantaneous: %s",
                fmt_fixed4(s_smoothed_rms, b1, sizeof(b1)),
                fmt_fixed4(s_latest_rms, b2, sizeof(b2)));
    shell_print(shell, "  Peak: %d (%s norm)", s_latest_peak,
                fmt_fixed4(peak_norm, b1, sizeof(b1)));
    shell_print(shell, "  Target window: [%s, %s] | Rate limit: %u frames",
                fmt_fixed4(sAgcProvider->getTargetLow(), b1, sizeof(b1)),
                fmt_fixed4(sAgcProvider->getTargetHigh(), b2, sizeof(b2)),
                sAgcProvider->getRateLimitFrames());
    return 0;
}

static int cmd_sound_agc_target_low(const struct shell *shell, size_t argc, char **argv) {
    if (argc == 1) {
        char buf[16];
        shell_print(shell, "AGC target low: %s",
                    fmt_fixed4(sAgcProvider->getTargetLow(), buf, sizeof(buf)));
        return 0;
    }
    if (argc != 2) {
        shell_error(shell, "Usage: sound agc target-low [<value>]");
        return -EINVAL;
    }
    float val;
    if (!parse_finite_float(argv[1], &val) || val < 0.001f || val > 0.1f) {
        shell_error(shell, "Value must be a number in range [0.001, 0.1]");
        return -EINVAL;
    }
    sAgcProvider->setTargetLow(val);
    char buf[16];
    shell_print(shell, "AGC target low set to %s", fmt_fixed4(val, buf, sizeof(buf)));
    return 0;
}

static int cmd_sound_agc_target_high(const struct shell *shell, size_t argc, char **argv) {
    if (argc == 1) {
        char buf[16];
        shell_print(shell, "AGC target high: %s",
                    fmt_fixed4(sAgcProvider->getTargetHigh(), buf, sizeof(buf)));
        return 0;
    }
    if (argc != 2) {
        shell_error(shell, "Usage: sound agc target-high [<value>]");
        return -EINVAL;
    }
    float val;
    if (!parse_finite_float(argv[1], &val) || val < 0.001f || val > 0.2f) {
        shell_error(shell, "Value must be a number in range [0.001, 0.2]");
        return -EINVAL;
    }
    sAgcProvider->setTargetHigh(val);
    char buf[16];
    shell_print(shell, "AGC target high set to %s", fmt_fixed4(val, buf, sizeof(buf)));
    return 0;
}

static int cmd_sound_agc_rate(const struct shell *shell, size_t argc, char **argv) {
    if (argc == 1) {
        shell_print(shell, "AGC rate limit: %u frames (~%u ms)", sAgcProvider->getRateLimitFrames(),
                    sAgcProvider->getRateLimitFrames() * 32);
        return 0;
    }
    if (argc != 2) {
        shell_error(shell, "Usage: sound agc rate [<frames>]");
        return -EINVAL;
    }
    uint32_t val = (uint32_t)strtoul(argv[1], NULL, 10);
    if (val < 1 || val > 100) {
        shell_error(shell, "Value must be in range [1, 100] frames");
        return -EINVAL;
    }
    sAgcProvider->setRateLimitFrames(val);
    shell_print(shell, "AGC rate limit set to %u frames (~%u ms)", val, val * 32);
    return 0;
}

#if defined(CONFIG_APP_AUDIO_DEBUG)
static int cmd_sound_agc_freeze(const struct shell *shell, size_t argc, char **argv) {
    if (argc == 1) {
        shell_print(shell, "AGC freeze: %s", s_agc_frozen ? "on" : "off");
        return 0;
    }
    if (strcmp(argv[1], "on") == 0) {
        s_agc_frozen = true;
    } else if (strcmp(argv[1], "off") == 0) {
        s_agc_frozen = false;
    } else {
        shell_error(shell, "Usage: sound agc freeze [on|off]");
        return -EINVAL;
    }
    shell_print(shell, "AGC freeze: %s", s_agc_frozen ? "on" : "off");
    return 0;
}

static int cmd_sound_agc_gain(const struct shell *shell, size_t argc, char **argv) {
    if (argc == 1) {
        int db10 = agc_gain_db10(s_agc_gain);
        shell_print(shell, "AGC gain: 0x%02x (%s%d.%u dB), freeze: %s", s_agc_gain,
                    db10 < 0 ? "-" : "", abs(db10) / 10, (unsigned)(abs(db10) % 10),
                    s_agc_frozen ? "on" : "off");
        return 0;
    }
    char *end = nullptr;
    unsigned long v = strtoul(argv[1], &end, 0); /* base 0: accepts 0x28 and 40 */
    if (end == argv[1] || *end != '\0' || v > AGC_GAIN_MAX) {
        shell_error(shell, "Gain must be in [0x00, 0x50] (each step = 0.5 dB, 0x28 = 0 dB)");
        return -EINVAL;
    }
    if (s_gain_l == NULL || s_gain_r == NULL) {
        /* configure_pdm() hasn't run (DSP thread failed at boot) - nothing to write to. */
        shell_error(shell, "PDM not configured; gain registers unavailable");
        return -ENOEXEC;
    }
    /* Freeze FIRST: it stops the DSP thread's loop from initiating new steps,
     * shrinking the concurrent-apply window before our own apply (the mutex in
     * agc_apply_gain serializes whatever remains). Manual gain implies freeze
     * anyway - the point is a known, fixed gain for recordings. */
    s_agc_frozen = true;
    /* Registers + detector compensation + RMS-window rescale in one place;
     * small nudges compensate exactly, jumps of more than 4 steps fall back to
     * the full history/window reset inside the call. */
    agc_apply_gain((uint8_t)v);
    int db10 = agc_gain_db10(s_agc_gain);
    shell_print(shell, "AGC gain set to 0x%02x (%s%d.%u dB), freeze forced on", s_agc_gain,
                db10 < 0 ? "-" : "", abs(db10) / 10, (unsigned)(abs(db10) % 10));
    return 0;
}
#endif /* CONFIG_APP_AUDIO_DEBUG */

static int cmd_sound_dsp_params(const struct shell *shell, size_t argc, char **argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    AudioDspConfigProvider *p = audio_dsp_get_config_provider();
    char b1[16], b2[16], b3[16];
    shell_print(shell, "gamma: %s | floor: %s | alpha: %s | refractory: %u frames",
                fmt_fixed4(p->getFluxGamma(), b1, sizeof(b1)),
                fmt_fixed4(p->getBeatFluxFloor(), b2, sizeof(b2)),
                fmt_fixed4(p->getBeatAlpha(), b3, sizeof(b3)), p->getBeatRefractoryFrames());
    return 0;
}

static int cmd_sound_dsp_set(const struct shell *shell, size_t argc, char **argv) {
    if (argc != 3) {
        shell_error(shell, "Usage: sound dsp set <gamma|floor|alpha|refractory> <value>");
        return -EINVAL;
    }
    AudioDspConfigProvider *p = audio_dsp_get_config_provider();
    const char *name = argv[1];
    char buf[16];

    if (strcmp(name, "refractory") == 0) {
        char *end = nullptr;
        unsigned long v = strtoul(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0') {
            shell_error(shell, "Invalid value: %s", argv[2]);
            return -EINVAL;
        }
        p->setBeatRefractoryFrames((uint32_t)v);
        /* Read back through the getter so the printed value reflects clamping. */
        shell_print(shell, "refractory set to %u frames", p->getBeatRefractoryFrames());
        return 0;
    }

    float v;
    if (!parse_finite_float(argv[2], &v)) {
        shell_error(shell, "Invalid value: %s", argv[2]);
        return -EINVAL;
    }
    if (strcmp(name, "gamma") == 0) {
        p->setFluxGamma(v);
        shell_print(shell, "gamma set to %s", fmt_fixed4(p->getFluxGamma(), buf, sizeof(buf)));
    } else if (strcmp(name, "floor") == 0) {
        p->setBeatFluxFloor(v);
        shell_print(shell, "floor set to %s",
                    fmt_fixed4(p->getBeatFluxFloor(), buf, sizeof(buf)));
    } else if (strcmp(name, "alpha") == 0) {
        p->setBeatAlpha(v);
        shell_print(shell, "alpha set to %s", fmt_fixed4(p->getBeatAlpha(), buf, sizeof(buf)));
    } else {
        shell_error(shell, "Unknown parameter '%s' (gamma|floor|alpha|refractory)", name);
        return -EINVAL;
    }
    return 0;
}

static int cmd_sound_rms(const struct shell *shell, size_t argc, char **argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    float peak_norm = (float)s_latest_peak / 32768.0f;
    char b1[16], b2[16];
    shell_print(shell, "Smoothed RMS (1s): %s", fmt_fixed4(s_smoothed_rms, b1, sizeof(b1)));
    shell_print(shell, "Instantaneous RMS: %s | Peak: %d (%s norm)",
                fmt_fixed4(s_latest_rms, b1, sizeof(b1)), s_latest_peak,
                fmt_fixed4(peak_norm, b2, sizeof(b2)));
    shell_print(shell, "Target window: [%s, %s]",
                fmt_fixed4(sAgcProvider->getTargetLow(), b1, sizeof(b1)),
                fmt_fixed4(sAgcProvider->getTargetHigh(), b2, sizeof(b2)));
    return 0;
}

// Subcommands for "sound agc"
// clang-format off
SHELL_STATIC_SUBCMD_SET_CREATE(sub_sound_agc,
                               SHELL_CMD_ARG(target-low, NULL, "Get/set AGC target-low threshold", cmd_sound_agc_target_low, 0, 1),
                               SHELL_CMD_ARG(target-high, NULL, "Get/set AGC target-high threshold", cmd_sound_agc_target_high, 0, 1),
                               SHELL_CMD_ARG(status, NULL, "Show current AGC status", cmd_sound_agc_status, 0, 0),
                               SHELL_CMD_ARG(rate, NULL, "Get/set AGC rate limit (frames)", cmd_sound_agc_rate, 0, 1),
#if defined(CONFIG_APP_AUDIO_DEBUG)
                               SHELL_CMD_ARG(freeze, NULL, "Get/set AGC freeze (halt gain adjustment)", cmd_sound_agc_freeze, 0, 1),
                               SHELL_CMD_ARG(gain, NULL, "Get/set PDM gain register directly (implies freeze)", cmd_sound_agc_gain, 0, 1),
#endif
                               SHELL_SUBCMD_SET_END);

// Subcommands for "sound dsp" (beat-detection parameters)
SHELL_STATIC_SUBCMD_SET_CREATE(sub_sound_dsp,
                               SHELL_CMD_ARG(params, NULL, "Print beat-detection parameters", cmd_sound_dsp_params, 0, 0),
                               SHELL_CMD_ARG(set, NULL, "Set parameter: <gamma|floor|alpha|refractory> <value>", cmd_sound_dsp_set, 3, 0),
                               SHELL_SUBCMD_SET_END);
// clang-format on

// Subcommands for "sound"
SHELL_STATIC_SUBCMD_SET_CREATE(sub_sound,
#if defined(CONFIG_VM3011)
                               SHELL_CMD(vm, &sub_sound_vm, "VM3011 Commands", NULL),
#endif
                               SHELL_CMD(mic, &sub_sound_mic, "Mic Commands", NULL),
                               SHELL_CMD(agc, &sub_sound_agc, "AGC Commands", NULL),
                               SHELL_CMD(dsp, &sub_sound_dsp, "Beat-detection DSP parameters", NULL),
                               SHELL_CMD(rms, NULL, "Print current RMS level", cmd_sound_rms),
#if defined(CONFIG_APP_AUDIO_DEBUG)
                               SHELL_CMD_ARG(dump, NULL, "Stream per-frame analysis: <frames> [buckets]", cmd_sound_dump, 2, 1),
#endif
                               SHELL_SUBCMD_SET_END);

/* Creating root (level 0) command "sound" */
SHELL_CMD_REGISTER(sound, &sub_sound, "Sound commands", NULL);