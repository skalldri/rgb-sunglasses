#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/shell/shell.h>

#if defined(CONFIG_VM3011)
#include <zephyr/drivers/vm3011/vm3011.h>
#endif
#include <math.h> /* sqrtf */
#include <sound/audio_param_table.h>
#if defined(CONFIG_APP_AUDIO_TELEMETRY)
#include <sound/audio_telemetry.h>
#endif
#include <stdio.h> /* snprintf (fmt_fixed4) */
#include <stdlib.h>
#include <string.h> /* memcpy (audio tap) */
#include <zephyr/audio/dmic.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>

#include <algorithm>

#include "audio_dsp.h"
#if defined(CONFIG_APP_AUDIO_DEBUG) || defined(CONFIG_APP_CAPTURE_AUDIO_SIDECAR)
#include "audio_tap_format.h"
#endif
#include "sound.h"

/* Both frame-dump producers — the CONFIG_APP_AUDIO_DEBUG rich tap and the
 * capture path's analysis sidecar — share one formatter and one #PARAMS
 * snapshot, so this spells the union once instead of repeating the pair of
 * symbols at every shared piece below. */
#if defined(CONFIG_APP_AUDIO_DEBUG) || defined(CONFIG_APP_CAPTURE_AUDIO_SIDECAR)
#define APP_AUDIO_TAP_FORMAT_USED 1
#endif

/* ── Adaptive Gain Control ───────────────────────────────────────────────────
 * Decision policy lives in AgcController (agc_controller.{h,cpp}): asymmetric
 * attack/release with a near-clip fast path, a noise gate that suppresses beat
 * output in silence, and a silence-park drift back to 0 dB. This file applies
 * the controller's decisions to the PDM GAINL/GAINR registers and carries the
 * beat detector's state across each step (audio_dsp_compensate_gain_change —
 * an amplitude discontinuity would otherwise look like a beat onset).
 *
 * Historical note: the original symmetric [0.005, 0.008] target window sat
 * BELOW real music levels ("prevent FFT saturation" was a misdiagnosis — the
 * FFT is float; only int16 capture clips, which the peak path now handles), so
 * the AGC stepped every few hundred ms during music. Current targets are
 * derived from real captures — see DefaultAgcConfigProvider below.
 * All values are tunable via shell commands and (see audio_config.cpp) BT. */
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
    void setTargetLow(float value) override {
        targetLow_ = audioParamClampF<kAudioParamAgcTargetLow>(value);
    }

    float getTargetHigh() override { return targetHigh_; }
    /* The raised 0.02 floor is deliberate settings migration for Phase 2's semantic change
     * (attack compares INSTANTANEOUS RMS now) — rationale in audio_param_table.h, which now
     * owns the bound itself. */
    void setTargetHigh(float value) override {
        targetHigh_ = audioParamClampF<kAudioParamAgcTargetHigh>(value);
    }

    uint32_t getRateLimitFrames() override { return rateLimitFrames_; }
    void setRateLimitFrames(uint32_t value) override {
        rateLimitFrames_ = audioParamClampU<kAudioParamAgcRateLimitFrames>(value);
    }

    uint32_t getAttackFrames() override { return attackFrames_; }
    void setAttackFrames(uint32_t value) override {
        attackFrames_ = audioParamClampU<kAudioParamAgcAttackFrames>(value);
    }

    uint32_t getReleaseFrames() override { return releaseFrames_; }
    void setReleaseFrames(uint32_t value) override {
        releaseFrames_ = audioParamClampU<kAudioParamAgcReleaseFrames>(value);
    }

    float getNoiseGateRms() override { return noiseGateRms_; }
    void setNoiseGateRms(float value) override {
        noiseGateRms_ = audioParamClampF<kAudioParamNoiseGateRms>(value);
    }

   private:
    /* Defaults and clamp ranges both come from audio_param_table.h, which is also where the
     * ABGT 250 baseline derivation of the targets and the field-report retune of the noise
     * gate are written down. This provider is what native_sim tests and the pre-binding boot
     * window see; AudioConfig (audio_config.cpp) reads the same table. */
    float targetLow_ = audioParamDefaultF<kAudioParamAgcTargetLow>();
    float targetHigh_ = audioParamDefaultF<kAudioParamAgcTargetHigh>();
    uint32_t rateLimitFrames_ = audioParamDefaultU<kAudioParamAgcRateLimitFrames>();
    uint32_t attackFrames_ = audioParamDefaultU<kAudioParamAgcAttackFrames>();
    uint32_t releaseFrames_ = audioParamDefaultU<kAudioParamAgcReleaseFrames>();
    float noiseGateRms_ = audioParamDefaultF<kAudioParamNoiseGateRms>();
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

static uint8_t s_agc_gain = 0x28; /* current gain register value (0 dB) */
static bool s_agc_frozen = false; /* debug: "sound agc freeze" halts gain adjustment */
static float s_latest_rms = 0.0f; /* latest instantaneous RMS */
static int16_t s_latest_peak = 0; /* latest peak sample magnitude */
static bool s_agc_silent = false; /* latest noise-gate state (for status + beat gating) */

/* Decision logic (RMS window, attack/release/gate/park policy) lives in the
 * BT-free AgcController so the native_sim suite and the WAV-replay harness run
 * the identical code — see agc_controller.h. */
static AgcController s_agc_controller;

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
 * audio_dsp_compensate_gain_change().
 *
 * Takes an int and clamps INSIDE the lock: callers compute relative targets
 * (s_agc_gain + steps) from an unsynchronized read of s_agc_gain, and a
 * concurrent manual gain change could otherwise wrap the arithmetic past the
 * register range (e.g. 0x00 + (-1) as uint8_t = 0xFF written to GAINL/GAINR). */
static void agc_apply_gain(int requested_gain) {
    k_mutex_lock(&s_agc_apply_mutex, K_FOREVER);
    uint8_t new_gain = (uint8_t)std::clamp(requested_gain, (int)AGC_GAIN_MIN, (int)AGC_GAIN_MAX);
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
    /* The controller's RMS window is in the old gain domain — single rescale
     * path for AGC-decided and manual gain changes alike (the |steps| > 4
     * flush rule lives inside notifyGainChange, mirroring the detector side). */
    s_agc_controller.notifyGainChange(steps);
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

K_MSGQ_DEFINE(audio_result_q, sizeof(struct audio_analysis_result), AUDIO_RESULT_QUEUE_DEPTH, 4);

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

/* Sector-aligned WAV prologue. Needed by BOTH capture paths: PCM that starts
 * at the canonical 44-byte offset makes every subsequent 4096-byte write
 * straddle a sector boundary, and each one then pays the FF_FS_TINY
 * shared-window read-modify-write penalty. Measured: that alone dropped 34 of
 * 250 blocks on an 8 s capture, and a deeper queue did NOT help because the
 * deficit is continuous rather than bursty.
 */
#define WAV_DATA_OFFSET 4096
#define WAV_JUNK_PAD (WAV_DATA_OFFSET - 12 - 24 - 8 - 8)

#if defined(CONFIG_APP_CAPTURE)
/* Lean capture tap: PCM only.
 *
 * The rich tap above carries the analysis result alongside every block and feeds
 * a per-frame CSV. That is most of what makes CONFIG_APP_AUDIO_DEBUG cost ~33 KB
 * of appcore RAM: an 18 KB queue of 1128-byte frames, an 8 KB WAV batch and a
 * 4.6 KB CSV batch. A scenario capture needs none of the analysis — the simulator
 * replays the WAV through the real DSP and re-derives the features itself — so
 * this queue carries bare PCM blocks and nothing else.
 *
 * Depth is a Kconfig symbol because it is a real tuning knob against FAT stalls,
 * not an arbitrary number: the rich path needed 16 entries because its drain loop
 * also wrote the CSV, and 8 dropped frames on every recording. This one writes
 * only the WAV, in single-sector batches, so it does far less work per frame —
 * but a flash sector erase still stalls the writer for hundreds of ms, and
 * s_capture_dropped is how you find out the depth is wrong rather than guessing. */
struct capture_pcm_block {
    int16_t pcm[AUDIO_FFT_SIZE];
};
K_MSGQ_DEFINE(capture_pcm_q, sizeof(struct capture_pcm_block),
              CONFIG_APP_CAPTURE_QUEUE_FRAMES, 4);
static atomic_t s_capture_armed;
static atomic_t s_capture_dropped;

#if defined(CONFIG_APP_CAPTURE_AUDIO_SIDECAR)
/* Analysis for the same blocks, carried in a SECOND queue rather than as extra
 * members of capture_pcm_block. The drain loop gets each PCM block by handing
 * k_msgq_get() the address of its slot inside the sector-aligned WAV batch — no
 * staging copy at all — and a wider entry would land the analysis bytes on top
 * of the next block's slot. Splitting the streams keeps that loop and its 4 KB
 * batch exactly as they were; the cost is one small queue (~164 B x
 * CONFIG_APP_CAPTURE_QUEUE_FRAMES) instead of a second 1 KB staging block.
 *
 * The two queues stay index-aligned because the producer only publishes here
 * after the PCM put succeeded, so a queue-full drop drops both halves of a
 * frame. Rows still carry the DSP's seq, so a host can see the gap regardless.
 *
 * ONE DEEPER THAN THE PCM QUEUE, and that is not slack — it is what keeps the
 * pairing sound. The consumer takes the PCM block first and its analysis a few
 * instructions later, inside audio_sidecar_drain(); the producer runs at a
 * higher priority and can publish an entire PCM+analysis pair inside even that
 * short window. At equal depths it would then land the PCM put in the slot just
 * freed while the analysis queue was still full, dropping one row for a block
 * that IS in the WAV — after which row k describes block k+1 for the rest of
 * the recording, with sc->frames still equal to blocks_captured so the
 * close-time cross-check never fires. One extra slot makes "analysis full while
 * PCM had room" unreachable.
 *
 * (The window used to be far wider — imu_sidecar_drain() sat between the two
 * gets and could stall on a 4096 B FatFs flush — but the IMU rows moved into
 * audio_sidecar_drain() when the two CSVs merged. Narrower, not gone.) */
struct capture_analysis_block {
    struct audio_analysis_result result;
    float rms;
    uint8_t gain; /* pre-step: the gain this block was captured at */
};
#define CAPTURE_ANALYSIS_QUEUE_FRAMES (CONFIG_APP_CAPTURE_QUEUE_FRAMES + 1)
K_MSGQ_DEFINE(capture_analysis_q, sizeof(struct capture_analysis_block),
              CAPTURE_ANALYSIS_QUEUE_FRAMES, 4);

/* The invariant the comment above spells out, in a form the compiler enforces:
 * trimming this back to the PCM depth to "save" 160 B reintroduces a
 * silently misaligned sidecar that no runtime check can catch. */
BUILD_ASSERT(CAPTURE_ANALYSIS_QUEUE_FRAMES > CONFIG_APP_CAPTURE_QUEUE_FRAMES,
             "analysis queue must outrun the PCM queue by at least one slot - the consumer "
             "takes the PCM block before its analysis, so the producer can refill the freed "
             "PCM slot while the analysis entry is still in flight");

/* Analysis rows lost, counted apart from s_capture_dropped because they mean
 * something completely different: a dropped PCM block is a gap in the audio and
 * the operator's fix is a deeper tap, while a lost row means the sidecar no
 * longer lines up with a WAV that is otherwise perfect. Should be unreachable
 * given the depth above; kept so that if it ever isn't, the capture says so
 * instead of shipping a quietly misaligned file. */
static atomic_t s_capture_analysis_dropped;
#endif /* CONFIG_APP_CAPTURE_AUDIO_SIDECAR */
#endif /* CONFIG_APP_CAPTURE */

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
// BLOCK_CAPTURE_TIME_MS now lives in sound.h (PR #378 review round 9): the FFT
// bars proration depends on it numerically, so it is exported and
// BUILD_ASSERTed against the animation's mirror in the audio adapter.

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
                /* The restart gap is an amplitude discontinuity like a gain step;
                 * the controller's RMS window spans the dead time too. */
                audio_dsp_reset_history();
                s_agc_controller.reset();
            }
            continue;
        }
        consecutive_failures = 0;

        const int16_t *pcm = static_cast<const int16_t *>(buffer);

        /* AGC: the controller ingests this block's levels and DECIDES here, but
         * the decision is APPLIED only after audio_dsp_process() below — this
         * block was captured at the CURRENT gain, and applying the step first
         * would feed an old-domain block against new-domain detector state (a
         * false flux of ~0.115/step, i.e. a spurious beat per AGC step — PR
         * #277 review). "sound agc freeze" (debug) sets allow_adjust=false so
         * recordings can be made at a known fixed gain while levels/gate stay
         * live for status. */
        float rms = agc_compute_rms(pcm, AUDIO_FFT_SIZE);
        s_latest_rms = rms; /* Instantaneous RMS for diagnostics */

        AgcDecision agc =
            s_agc_controller.update(*sAgcProvider, rms, s_latest_peak, s_agc_gain, !s_agc_frozen);
        if (agc.silent != s_agc_silent) {
            LOG_DBG("AGC noise gate %s", agc.silent ? "closed (silence)" : "open");
            s_agc_silent = agc.silent;
        }

        struct audio_analysis_result result;
        audio_dsp_process(pcm, seq++, &result);

        /* Noise gate: in silence the detector could only fire on amplified
         * mic/room noise — suppress beat output entirely (issue #264's
         * quiet-room complaint). Skipped while frozen so debug captures see the
         * raw detector output (device-vs-host replay comparison depends on it). */
        if (s_agc_silent && !s_agc_frozen) {
            for (int b = 0; b < AUDIO_NUM_BANDS; b++) {
                result.beat[b] = false;
            }
        }

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

#if defined(CONFIG_APP_CAPTURE)
        /* No intermediate copy: pcm is already exactly one block, so the msgq
         * copies straight out of it. Only runs while a capture is armed, so an
         * idle device pays one atomic read per block. */
        if (atomic_get(&s_capture_armed)) {
            if (k_msgq_put(&capture_pcm_q, pcm, K_NO_WAIT) != 0) {
                atomic_inc(&s_capture_dropped);
            }
#if defined(CONFIG_APP_CAPTURE_AUDIO_SIDECAR)
            else {
                /* Only after the PCM landed — see the queue's comment. The
                 * analysis queue is one deeper than the PCM one precisely so
                 * this put cannot fail while the PCM put succeeded; the
                 * separate counter exists to catch it if that ever stops
                 * holding, and is deliberately NOT s_capture_dropped, which
                 * means "audio blocks lost".
                 *
                 * Static, not stack, for the same reason as the rich tap's
                 * frame above: this thread runs on 2 KB and the neighbouring
                 * code already treats that as tight. Safe because this thread
                 * is the only producer. */
                static struct capture_analysis_block a;
                a.result = result;
                a.rms = rms;
                a.gain = s_agc_gain;
                if (k_msgq_put(&capture_analysis_q, &a, K_NO_WAIT) != 0) {
                    atomic_inc(&s_capture_analysis_dropped);
                }
            }
#endif
        }
#endif

        k_mem_slab_free(&mem_slab, buffer);

        /* Apply the gain decision AFTER the old-domain block was processed (see
         * the ordering note above). The register write lands mid-capture of the
         * next DMA block, so that one transitional block is a bounded old/new
         * mix (≤ 0.5 dB across it) — far below the full-step error this
         * ordering removes. */
        if (agc.gain_steps != 0) {
            agc_apply_gain((int)s_agc_gain + agc.gain_steps);
            int db10 = agc_gain_db10(s_agc_gain);
            char rms_buf[16];
            LOG_DBG("AGC: gain=0x%02x (%s%d.%u dB)%s smoothed_rms=%s", s_agc_gain,
                    db10 < 0 ? "-" : "", abs(db10) / 10, (unsigned)(abs(db10) % 10),
                    agc.clipped ? " [near-clip]" : "",
                    fmt_fixed4(s_agc_controller.smoothedRms(), rms_buf, sizeof(rms_buf)));
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

#if defined(CONFIG_APP_AUDIO_TELEMETRY)
        /* Live telemetry for the app's Audio Tuning meters.
         *
         * Placed HERE deliberately: after the noise gate has suppressed beats and after
         * agc_apply_gain() has landed, so what the phone sees is what the animations see
         * and the gain reported is the one now in the register. Reporting the pre-gate
         * beats would show the user a detector firing merrily while their glasses sat
         * dark, which is the exact confusion this screen exists to end.
         *
         * The is_active() gate belongs HERE, not only inside publish(): every argument
         * below costs real work on a thread with a 32 ms deadline — three virtual config
         * getters with clamps, and formerly a second run of the 40-iteration gain-ratio
         * loop. Paying that every frame forever on a device nobody is streaming from is
         * precisely what "cheap when idle" promised not to do. Idle now costs one atomic
         * read and nothing else. */
        if (audio_telemetry_is_active()) {
            AudioDspConfigProvider *dsp_cfg = audio_dsp_get_config_provider();
            audio_telemetry_publish(&result, s_agc_controller.inputReferredRms(), s_latest_rms,
                                    (float)s_latest_peak / 32768.0f, s_agc_controller.noiseFloor(),
                                    (int8_t)((int)s_agc_gain - (int)AgcController::kGainPark),
                                    s_agc_controller.framesSinceStep(), s_agc_silent, agc.clipped,
                                    s_agc_frozen, dsp_cfg->getThresholdMode(),
                                    dsp_cfg->getBeatAlpha(), dsp_cfg->getBeatFluxFloor());
        }
#endif

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
#endif /* CONFIG_APP_AUDIO_DEBUG */

#define DEFAULT_WAV_PATH "/NAND:/sound.wav"
#define DEFAULT_RECORD_DURATION_S 10

/* Set by sound_record_request_stop() from another thread; polled by both capture
 * loops. Distinct from the io_error abort below: that one means the capture
 * FAILED and returns -EIO, this one means the operator ended it and the result is
 * a shorter but perfectly good recording. */
static atomic_t s_record_stop_requested;

/* One-at-a-time guard for the direct (no-DSP) capture path, mirroring
 * s_tap_armed / s_capture_armed on the other two. */
static atomic_t s_direct_armed;

void sound_record_request_stop(void) {
    atomic_set(&s_record_stop_requested, 1);
}

void sound_record_arm(void) {
    atomic_set(&s_record_stop_requested, 0);
}

static bool record_stop_requested(void) {
    return atomic_get(&s_record_stop_requested) != 0;
}

#if defined(CONFIG_IMU)
#include <imu/imu.h>

/* IMU sidecar: "<wav path>.imu.csv", written by the SAME capture loop as the WAV
 * and timestamped from the same t0, so the two streams share a timebase by
 * construction. That is the whole reason this lives inside record_wav rather than
 * as its own `imu record` command: the Zephyr shell is single-threaded, so two
 * commands cannot run concurrently, and anything else would need the host to
 * align two independently-started recordings after the fact.
 *
 * Deliberately a SEPARATE file rather than extra columns on the analysis CSV:
 * fw/sim/node/dline.ts accepts exactly 21 or 41 comma-separated fields per D-line,
 * so widening those rows would break every existing beat_lab consumer.
 *
 * Batched in sector-aligned 4096-byte chunks for the hardware-measured reason
 * given for s_csv_batch above — a misaligned flush pays the FF_FS_TINY shared-
 * window RMW penalty and stalls the drain loop long enough to DROP AUDIO FRAMES.
 * A 512-byte buffer would have flushed ~4x per second, misaligned, and quietly
 * degraded the very recording it is part of. */
#define IMU_CSV_CHUNK 4096
#define IMU_CSV_LINE_MAX 96
static char s_imu_batch[IMU_CSV_CHUNK + IMU_CSV_LINE_MAX];

/* Values are scaled integers, not floats: CONFIG_CBPRINTF_FP_SUPPORT is off (see
 * fw/CLAUDE.md), so a %f here would print the literal specifier. The scale is
 * stated in the file header so the host converter does not have to assume it. */
#define IMU_CSV_SCALE 1000

struct imu_sidecar {
    struct fs_file_t file;
    bool open;
    int64_t t0_ms;
    size_t len;
    uint32_t samples;
};

static void imu_sidecar_flush(struct imu_sidecar *sc, bool final) {
    while (sc->len >= IMU_CSV_CHUNK || (final && sc->len > 0)) {
        size_t take = (sc->len >= IMU_CSV_CHUNK) ? IMU_CSV_CHUNK : sc->len;
        /* A SHORT write is a failure here, not a partial success: on a nearly
         * full volume FatFs returns FR_OK with fewer bytes than asked, which
         * arrives as a non-negative count. Treating that as written would drop
         * the tail and splice the next CSV line onto a truncated one — and the
         * host converter skips malformed rows silently, so the scenario would
         * just be missing a span of IMU keyframes with nothing to notice.
         * Matches the `written != size` check the two WAV writers use. */
        if (fs_write(&sc->file, s_imu_batch, take) != (ssize_t)take) {
            /* Give the handle back immediately. FatFs allows only
             * CONFIG_FS_FATFS_NUM_FILES (4) open files device-wide, and leaving
             * it open would starve GLIM loads, extension loads, coredump writes
             * and MCUmgr DFU staging alike — every fs_open would return -ENOMEM
             * until reboot. imu_sidecar_close() early-returns on !open, so this
             * is the only place that can release it. */
            fs_close(&sc->file);
            sc->open = false; /* stop trying; the WAV is the primary artifact */
            return;
        }
        sc->len -= take;
        if (sc->len > 0) {
            memmove(s_imu_batch, &s_imu_batch[take], sc->len);
        }
    }
}

static int imu_sidecar_open(struct imu_sidecar *sc, const char *wav_path, int64_t t0_ms) {
    char path[96];
    (void)snprintf(path, sizeof(path), "%s.imu.csv", wav_path);

    fs_file_t_init(&sc->file);
    int ret = fs_open(&sc->file, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
    if (ret < 0) {
        return ret;
    }
    sc->open = true;
    sc->t0_ms = t0_ms;
    sc->len = 0;
    sc->samples = 0;

    /* Header padded to a full sector, so every subsequent chunk lands aligned —
     * same trick as the analysis CSV's #PARAMS line. */
    memset(s_imu_batch, ' ', IMU_CSV_CHUNK);
    int n = snprintf(s_imu_batch, IMU_CSV_CHUNK,
                     "#IMU scale=%d cols=ms,seq,ax,ay,az,gx,gy,gz units=mm/s2,mrad/s",
                     IMU_CSV_SCALE);
    if (n > 0 && (size_t)n < IMU_CSV_CHUNK) {
        s_imu_batch[n] = ' ';
    }
    s_imu_batch[IMU_CSV_CHUNK - 1] = '\n';
    if (fs_write(&sc->file, s_imu_batch, IMU_CSV_CHUNK) < 0) {
        fs_close(&sc->file);
        sc->open = false;
        return -EIO;
    }
    k_msgq_purge(&imu_tap_q);
    return 0;
}

/* Called once per audio frame from the capture loop; drains whatever the IMU
 * thread has produced since the last call. */
static void imu_sidecar_drain(struct imu_sidecar *sc) {
    /* Deliberately NO function-level `if (!sc->open) return;`. This is the only
     * reader of imu_tap_q on these paths, so bailing out when the sidecar never
     * opened (FatFs already at CONFIG_FS_FATFS_NUM_FILES, -ENOSPC, ...) leaves
     * the 8-deep tap to fill and the IMU thread dropping samples for the rest of
     * the recording — a sidecar that failed to open would degrade the IMU
     * pipeline itself. The loop below drains and discards instead. Same rule as
     * audio_sidecar_drain(); it had this fixed first, and leaving the guard here
     * made the pair inconsistent in exactly the case that matters. */
    /* `open` gates every iteration, not just entry: a flush failing mid-loop
     * clears it WITHOUT consuming the buffer, so len stays >= IMU_CSV_CHUNK and
     * the next line would be formatted at &s_imu_batch[len] — past the end of a
     * buffer that is only IMU_CSV_CHUNK + one line long, growing by a line for
     * every sample still queued. Its .bss neighbours are the capture batch and
     * the audio-tap statics. A mid-capture write failure is all it takes, and
     * the free-space clamp cannot rule that out: it works from a cached figure,
     * and a host can add files over USB during a recording. */
    struct imu_tap_sample s;
    while (k_msgq_get(&imu_tap_q, &s, K_NO_WAIT) == 0) {
        if (!sc->open) {
            continue; /* drained to keep the tap moving; nowhere to write it */
        }
        const struct imu_analysis_result r = s.result;
        int n = snprintf(&s_imu_batch[sc->len], IMU_CSV_LINE_MAX,
                         "I,%d,%u,%d,%d,%d,%d,%d,%d\n",
                         (int)((int64_t)s.uptime_ms - sc->t0_ms), r.seq,
                         (int)(r.accel_x * IMU_CSV_SCALE), (int)(r.accel_y * IMU_CSV_SCALE),
                         (int)(r.accel_z * IMU_CSV_SCALE), (int)(r.gyro_x * IMU_CSV_SCALE),
                         (int)(r.gyro_y * IMU_CSV_SCALE), (int)(r.gyro_z * IMU_CSV_SCALE));
        /* snprintf returns what it WOULD have written, so a truncated line would
         * advance len past the bytes actually stored and shift every following
         * line — silently corrupting the sidecar rather than overflowing (the
         * size argument still bounds the write). A sane sample cannot reach 96
         * bytes; a garbage one from a sensor glitch could, so drop it instead of
         * writing a half-line. Matches the guard imu_sidecar_open() already uses. */
        if (n > 0 && n < IMU_CSV_LINE_MAX) {
            sc->len += (size_t)n;
            sc->samples++;
        }
        if (sc->len >= IMU_CSV_CHUNK) {
            imu_sidecar_flush(sc, false);
        }
    }
}

static void imu_sidecar_close(struct imu_sidecar *sc, const struct shell *shell) {
    if (!sc->open) {
        return;
    }
    imu_sidecar_flush(sc, true);
    fs_close(&sc->file);
    sc->open = false;
    shell_print(shell, "IMU sidecar: %u samples", sc->samples);
}
#endif /* CONFIG_IMU */


#if defined(APP_AUDIO_TAP_FORMAT_USED)
/* Tap frame text format: field order lives in audio_tap_format.h (shared with
 * the host replay harness so the producers can never drift; decoder in
 * fw/tools/beat_lab/frames.py). The wrappers here just bind the firmware's
 * data sources (config providers, AGC state).
 *
 * Renders the #PARAMS snapshot line (no trailing newline); returns its length.
 * Params can be changed at runtime (BLE/shell) — captures assume they stay
 * stable for the duration; this snapshot is what the replay harness replays with. */
static size_t tap_params_format(char *buf, size_t cap) {
    AudioDspConfigProvider *dsp = audio_dsp_get_config_provider();
    return audio_tap_format_params(dsp->getFluxGamma(), dsp->getBeatAlpha(),
                                   dsp->getBeatFluxFloor(), dsp->getBeatRefractoryFrames(),
                                   s_agc_frozen, s_agc_gain, sAgcProvider->getTargetLow(),
                                   sAgcProvider->getTargetHigh(),
                                   sAgcProvider->getRateLimitFrames(),
                                   sAgcProvider->getAttackFrames(),
                                   sAgcProvider->getReleaseFrames(),
                                   sAgcProvider->getNoiseGateRms(), dsp->getSfDelta(),
                                   dsp->getThresholdMode(), buf, cap);
}
#endif /* APP_AUDIO_TAP_FORMAT_USED */

#if defined(CONFIG_APP_CAPTURE) || defined(CONFIG_APP_AUDIO_DEBUG)
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
#endif /* CONFIG_APP_CAPTURE || CONFIG_APP_AUDIO_DEBUG */

#if defined(CONFIG_APP_CAPTURE_AUDIO_SIDECAR)
/* Combined capture sidecar: "<wav path>.csv", written by the capture loop
 * from capture_analysis_q — the analysis the DSP thread computed for the very
 * blocks going into the WAV.
 *
 * This is what makes an app-triggered capture usable. That path deliberately
 * does not freeze the AGC (a field recording has to cope with whatever it
 * meets), so its gain steps mid-capture and the recorded samples already
 * contain those steps. Re-deriving the features on the host from the WAV alone
 * therefore cannot reproduce what the device saw; the per-frame gain column is
 * the missing piece that makes the replay harness applicable at all.
 *
 * Same wire format as "sound dump" and the CONFIG_APP_AUDIO_DEBUG sidecar
 * (audio_tap_format.h), buckets included — 41 fields, an arity that
 * fw/tools/beat_lab/frames.py and fw/sim/node/dline.ts already accept, so this
 * file needs no new decoder anywhere.
 *
 * Batched in sector-aligned 4096-byte chunks for the hardware-measured reason
 * given for s_csv_batch below: a misaligned flush pays the FF_FS_TINY shared-
 * window RMW penalty and stalls the drain loop long enough to DROP AUDIO
 * FRAMES. Writes go through tap_write_at_retry() like the WAV's, so a
 * transient -EIO is ridden out rather than retiring the file; only a failure
 * that survives close + reopen + reseek gives up. (This comment used to say
 * the opposite — the sidecar predated the retry being hoisted out of
 * CONFIG_APP_AUDIO_DEBUG.) */
#define AUDIO_CSV_CHUNK 4096
#define AUDIO_CSV_LINE_MAX 512
static char s_audio_batch[AUDIO_CSV_CHUNK + AUDIO_CSV_LINE_MAX];

/* Longest D-line the format can emit: "D," + a 10-digit seq + the gain and
 * beatmask fields, then one 9-byte ",%08x" per float (rms, four bands x four
 * statistics, and every display bucket), plus the newline. Tied to the field
 * counts rather than to a measured number so adding a band or a bucket fails
 * the build here instead of silently truncating rows on the device. */
/* Derivation, since an off-by-one here defeats the whole point: the widest
 * line is CAPTURE_D_LINE_MAX_CHARS characters; snprintf needs one more for its
 * NUL; and audio_sidecar_drain() hands it AUDIO_CSV_LINE_MAX - 1 as the cap,
 * so the buffer must carry two beyond the characters themselves. Asserting
 * only chars+1 would let a future trim to exactly that bound pass while every
 * row silently lost its last character. */
#define CAPTURE_D_LINE_MAX_CHARS \
    (2 + 10 + 3 + 2 + 9 * (1 + 4 * AUDIO_NUM_BANDS + AUDIO_NUM_DISPLAY_BUCKETS))
#if defined(CONFIG_IMU)
/* The I-rows share s_audio_batch with the D-rows but are bounded by a constant
 * defined ~440 lines away for a DIFFERENT buffer (s_imu_batch). 96 <= 512
 * today, so nothing overruns — but the coupling is invisible from either end,
 * and the D-row path right below is asserted for exactly this reason. */
BUILD_ASSERT(IMU_CSV_LINE_MAX <= AUDIO_CSV_LINE_MAX,
             "I-rows are written into s_audio_batch, so their line bound cannot exceed the "
             "headroom that buffer is sized with");
#endif
BUILD_ASSERT(AUDIO_CSV_LINE_MAX >= CAPTURE_D_LINE_MAX_CHARS + 2,
             "AUDIO_CSV_LINE_MAX too small for one D-line with buckets (needs the NUL and "
             "the -1 the drain call site passes as cap)");

/* ONE file for both streams, not one per stream — this is a correctness
 * constraint, not tidiness. Zephyr hardcodes `FF_FS_TINY 1` in
 * zephyr_fatfs_config.h (in the block commented "no Kconfig options", and the
 * NCS tree is off-limits per fw/CLAUDE.md), so every open FIL shares ONE sector
 * window in the FATFS object instead of owning a buffer. Writing WAV + IMU +
 * analysis meant three handles thrashing that single window on a volume with a
 * single FAT copy and no mirror to recover from. Folding the two CSVs into one
 * file puts the capture back at two open handles — exactly what it used before
 * the analysis sidecar existed, which is the configuration this path has always
 * been proven at.
 *
 * It does NOT hand back the two-buffers-become-one RAM saving an earlier
 * version of this comment claimed. Verified on the map for the shipping
 * config: s_imu_batch (4,192 B) and s_audio_batch (4,608 B) are BOTH linked,
 * because s_imu_batch is guarded by CONFIG_IMU alone and record_wav_direct()
 * still uses it for the DSP-not-streaming fallback. The two are never live at
 * once, so sharing one buffer between them would genuinely recover ~4.2 KB —
 * that is unclaimed work, not something this change did.
 *
 * The combined stream needs no new host parser: fw/tools/beat_lab/frames.py
 * consumes only `D,`/`#PARAMS`/`#DONE` and skips everything else, and
 * capture_to_scenario.py's parse_imu_csv() consumes only `I,` (reading `scale=`
 * off `#IMU`) and skips everything else. Both already tolerate a mixed stream
 * on purpose, so each reads this file correctly while ignoring the other's
 * rows. */
struct audio_sidecar {
    struct fs_file_t file;
    bool open;
    /* Opened, then given up on mid-recording. `open` alone cannot say so — it
     * is false both for a sidecar that never opened (nothing to report) and one
     * abandoned at block 300 of 5000 (a truncated file the operator must be
     * told about), and those two need opposite treatment at close. */
    bool retired;
    size_t len;
    uint32_t frames;   /* analysis rows, one per WAV block */
    /* Both needed to survive a transient write error: FatFS latches a sticky
     * error on the FIL, so recovery means close + reopen (by path) and seek
     * back to where we were (the handle's own position is lost). */
    char path[96];
    off_t pos;
    uint32_t *io_retries;
#if defined(CONFIG_IMU)
    int64_t t0_ms;     /* same t0 the WAV starts at, so I-rows share its timebase */
    uint32_t samples;  /* IMU rows */
#endif
};

static void audio_sidecar_flush(struct audio_sidecar *sc, bool final) {
    while (sc->len >= AUDIO_CSV_CHUNK || (final && sc->len > 0)) {
        size_t take = (sc->len >= AUDIO_CSV_CHUNK) ? AUDIO_CSV_CHUNK : sc->len;
        /* A SHORT write is a failure, not a partial success — same reasoning as
         * imu_sidecar_flush(): FatFs returns FR_OK with a short count on a
         * nearly full volume, and treating that as written would splice the
         * next row onto a truncated one. Give the handle back immediately; a
         * capture holds two of the four FatFs slots and leaking one starves
         * GLIM loads, extension loads and DFU staging until reboot. */
        /* Retry rather than retiring the file on the first -EIO; giving up on
         * it is what left a capture with a CSV shorter than its own directory
         * entry claims. Only a failure that survives close + reopen + reseek
         * retires the sidecar.
         *
         * This is now a backstop, not the main event. While this branch was
         * being written the -EIO looked like "a recoverable fact of sustained
         * recording on this flash" — a hardware hiccup. Issue #380 root-caused
         * it as software: FatFS has one shared sector window per volume and
         * Zephyr's fs.c took no lock, so coredump_wq's unconditional 60 s
         * fs_stat("/NAND:") raced this writer and manufactured -EIO with the
         * flashdisk failure counters reading zero (PR #382's instrumented
         * build caught one exactly on the 60 s tick). CONFIG_FS_FATFS_REENTRANT
         * (PR #383) serializes the FatFS entry points and is the actual fix; a
         * missing upstream flashdisk write-error backport (PR #382) was the
         * other half. Expect 0 io retries on a healthy build now — a nonzero
         * count is worth investigating, not shrugging at. Kept because USB MSC
         * raw disk_access traffic is deliberately outside that lock and genuine
         * media errors still exist. */
        if (tap_write_at_retry(&sc->file, sc->path, sc->pos, s_audio_batch, take,
                               sc->io_retries) != 0) {
            fs_close(&sc->file);
            sc->open = false; /* stop trying; the WAV is the primary artifact */
            sc->retired = true;
            return;
        }
        sc->pos += (off_t)take;
        sc->len -= take;
        if (sc->len > 0) {
            memmove(s_audio_batch, &s_audio_batch[take], sc->len);
        }
    }
}

static int audio_sidecar_open(struct audio_sidecar *sc, const char *wav_path, int64_t t0_ms,
                              uint32_t *io_retries) {
    int path_len = snprintf(sc->path, sizeof(sc->path), "%s.csv", wav_path);
    if (path_len < 0 || (size_t)path_len >= sizeof(sc->path)) {
        return -EINVAL; /* truncated: reopen-on-retry would target the wrong file */
    }

    fs_file_t_init(&sc->file);
    int ret = fs_open(&sc->file, sc->path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
    if (ret < 0) {
        return ret;
    }
    sc->open = true;
    sc->retired = false; /* every field reset here, so a reused struct cannot
                          * inherit a previous capture's verdict */
    sc->len = 0;
    sc->frames = 0;
    sc->pos = 0;
    sc->io_retries = io_retries;
#if defined(CONFIG_IMU)
    sc->t0_ms = t0_ms;
    sc->samples = 0;
#else
    (void)t0_ms;
#endif

    /* Both header lines, then space padding out to a full sector so every
     * subsequent chunk write lands aligned (a misaligned flush pays the
     * FF_FS_TINY shared-window RMW penalty and stalls the drain loop long
     * enough to drop audio frames). One '\n' separates them and one terminates
     * the block, so each header is its own line to the parsers that want it. */
    memset(s_audio_batch, ' ', AUDIO_CSV_CHUNK);
    size_t n = tap_params_format(s_audio_batch, AUDIO_CSV_CHUNK - 1);
    if (n < AUDIO_CSV_CHUNK - 1) {
        s_audio_batch[n] = '\n';
        n++;
    }
#if defined(CONFIG_IMU)
    int m = snprintf(&s_audio_batch[n], AUDIO_CSV_CHUNK - 1 - n,
                     "#IMU scale=%d cols=ms,seq,ax,ay,az,gx,gy,gz units=mm/s2,mrad/s",
                     IMU_CSV_SCALE);
    if (m > 0 && (size_t)m < AUDIO_CSV_CHUNK - 1 - n) {
        s_audio_batch[n + m] = ' '; /* overwrite the NUL snprintf left behind */
    }
#endif
    s_audio_batch[AUDIO_CSV_CHUNK - 1] = '\n';
    if (tap_write_at_retry(&sc->file, sc->path, 0, s_audio_batch, AUDIO_CSV_CHUNK,
                           sc->io_retries) != 0) {
        fs_close(&sc->file);
        sc->open = false;
        /* fs_open() already created and truncated the file, so failing here
         * would leave a headerless orphan — no #PARAMS, no rows, no #DONE, and
         * frames.py yields empty arrays rather than an error. Remove it rather
         * than arming `retired`: the caller reports every open failure as
         * "recording audio only" and carries on, so a sticky flag would have
         * close() contradict that ~20 s later and fail a perfectly good WAV.
         * No file on the volume is the honest outcome, and it matches the
         * sibling fs_open() failure exactly. */
        (void)fs_unlink(sc->path);
        return -EIO;
    }
    sc->pos = AUDIO_CSV_CHUNK;
#if defined(CONFIG_IMU)
    /* Drop whatever the 25 Hz IMU tap queued before this capture existed.
     * Those samples predate t0, so without this they are written with NEGATIVE
     * millisecond stamps and stretch the track back in time — measured before
     * this line was restored: a 15 s capture spanned -10.71..15.22 s and its
     * apparent rate read 14.7 Hz instead of 25. The analysis tap is purged by
     * the caller instead (see capture_taps_purge); this one has no such
     * pairing constraint, so it is purged here where t0 is set. */
    k_msgq_purge(&imu_tap_q);
#endif
    /* Deliberately does NOT purge capture_analysis_q — the caller purges it
     * together with capture_pcm_q, which is the only way to start the two
     * streams in step (see capture_taps_purge). */
    return 0;
}

/* Writes the row for ONE captured block; called once per block by the capture
 * loop, immediately after that block was drained from the PCM tap.
 *
 * Exactly one row per call, NOT "drain whatever is queued" the way the IMU
 * sidecar does. Hardware-measured: a greedy drain runs ahead of the WAV,
 * because analysis is queued for blocks still sitting in capture_pcm_q — a 20 s
 * capture produced 627 rows for 625 blocks, the last two describing audio the
 * loop stopped before ever writing. IMU rows carry a timestamp and survive
 * that; these are paired POSITIONALLY (device row k <-> WAV block k, which is
 * what fw/tools/beat_lab/compare.py assumes), so the pairing has to be exact by
 * construction. One get per block makes it so.
 *
 * The get cannot come up empty: the producer publishes the analysis right after
 * the PCM block, without blocking in between, and it runs at a higher priority
 * than this loop — so a block reaching us means its analysis is already queued.
 * audio_sidecar_close() re-checks the count against the block count rather than
 * trusting that argument. */
static void audio_sidecar_drain(struct audio_sidecar *sc) {
    /* Consume BEFORE the open check, not after. This is the queue's only
     * reader, while the producer gates on s_capture_armed alone and keeps
     * publishing whether or not the sidecar survived — so returning early on a
     * retired sidecar leaves the queue permanently full, and from the ninth
     * frame on every analysis put fails. Those failures used to land on
     * s_capture_dropped, which reports as lost AUDIO: a capture whose WAV is
     * perfect would print "625 blocks, 617 dropped" and send the operator off
     * to raise CONFIG_APP_CAPTURE_QUEUE_FRAMES over a sidecar that simply
     * could not be opened (FatFs at its four-file limit, say). Draining and
     * discarding keeps the drop counter meaning only what it says. */
    struct capture_analysis_block a;
    bool have_row = k_msgq_get(&capture_analysis_q, &a, K_NO_WAIT) == 0;
    /* Falls through to the IMU drain either way. Since the two sidecars merged
     * this is imu_tap_q's ONLY reader on this path, so returning early here —
     * on a missing analysis entry or a retired sidecar — left that 8-deep tap
     * to fill and the IMU thread dropping samples for the rest of the
     * recording. */
    if (have_row && sc->open) {
        size_t n = audio_tap_format_frame(&a.result, a.rms, a.gain, /*buckets=*/true,
                                          &s_audio_batch[sc->len], AUDIO_CSV_LINE_MAX - 1);
        s_audio_batch[sc->len + n] = '\n';
        sc->len += n + 1;
        sc->frames++;
        if (sc->len >= AUDIO_CSV_CHUNK) {
            audio_sidecar_flush(sc, false);
        }
    }

#if defined(CONFIG_IMU)
    /* IMU rows go into the SAME file, interleaved between the analysis rows.
     * Greedy here, unlike the analysis row above: I-rows carry their own
     * millisecond stamp so they need no positional pairing, and draining
     * whatever the 25 Hz producer has queued is what keeps its 8-deep tap from
     * overflowing. `open` is re-checked every iteration because a flush failing
     * mid-loop clears it WITHOUT consuming the buffer, after which the next row
     * would be formatted past the end of a buffer only one line longer than the
     * chunk. */
    struct imu_tap_sample s;
    while (k_msgq_get(&imu_tap_q, &s, K_NO_WAIT) == 0) {
        if (!sc->open) {
            continue; /* drained to keep the tap moving; nowhere to write it */
        }
        const struct imu_analysis_result r = s.result;
        int len = snprintf(&s_audio_batch[sc->len], IMU_CSV_LINE_MAX,
                           "I,%d,%u,%d,%d,%d,%d,%d,%d\n",
                           (int)((int64_t)s.uptime_ms - sc->t0_ms), r.seq,
                           (int)(r.accel_x * IMU_CSV_SCALE), (int)(r.accel_y * IMU_CSV_SCALE),
                           (int)(r.accel_z * IMU_CSV_SCALE), (int)(r.gyro_x * IMU_CSV_SCALE),
                           (int)(r.gyro_y * IMU_CSV_SCALE), (int)(r.gyro_z * IMU_CSV_SCALE));
        /* snprintf returns what it WOULD have written, so a truncated line
         * would advance len past the bytes actually stored and shift every
         * following line — silently corrupting the file rather than
         * overflowing. A sane sample cannot reach 96 bytes; a garbage one from
         * a sensor glitch could, so drop it instead of writing a half-line. */
        if (len > 0 && len < IMU_CSV_LINE_MAX) {
            sc->len += (size_t)len;
            sc->samples++;
        }
        if (sc->len >= AUDIO_CSV_CHUNK) {
            audio_sidecar_flush(sc, false);
        }
    }
#endif
}

/* Returns 0 only if the file on the volume is one a consumer may trust. The
 * caller keeps this apart from io_error and prints its own verdict, but does
 * NOT fail the capture on it: a damaged sidecar leaves the WAV whole, and the
 * only app-visible consequence of a non-zero return would be a "Failed" badge
 * that a complete recording does not deserve (last_error never leaves the
 * UART — see the sidecar_unusable comment in record_wav_capture()).
 * A sidecar that never opened is NOT a failure either: nothing was promised,
 * the warning at open time said so, and the WAV stands on its own. A file that
 * exists but cannot be trusted is the case worth reporting. */
static int audio_sidecar_close(struct audio_sidecar *sc, const struct shell *shell,
                               uint32_t blocks, uint32_t dropped) {
    if (!sc->open) {
        /* A sidecar abandoned mid-recording used to leave here in silence: the
         * capture then ran to completion and printed a clean WAV summary, while
         * the volume kept a CSV holding only the rows written before the write
         * failed, with no #DONE trailer and possibly a torn final row. Exactly
         * the quietly-unusable artifact the MISALIGNED branch below exists to
         * prevent, reached by a different route. Worded so the MCP success
         * regex cannot match it. */
        if (sc->retired) {
            uint32_t lost = (uint32_t)atomic_get(&s_capture_analysis_dropped);
            /* Rows FORMATTED, which overstates what reached the file: a flush
             * failure leaves sc->len bytes unwritten by design, up to a full
             * chunk (~11 D-rows plus interleaved I-rows). sc->pos is exactly
             * how many bytes made it, so report both rather than one number
             * the operator would act on as if it were the file's length. */
            shell_warn(shell,
                       "Capture CSV ABANDONED - %u rows formatted, only %u bytes written "
                       "(%u rows lost); truncated, no #DONE trailer, do NOT pair with the WAV",
                       sc->frames, (uint32_t)sc->pos, lost);
            return -EIO;
        }
        return 0;
    }
    audio_sidecar_flush(sc, true);
    /* The trailer only means anything if everything before it got out, and the
     * flush above clears `open` (having already closed the handle) when it
     * didn't. Report that case as a warning rather than the usual line: the
     * MCP plugin keys on "Audio sidecar: N frames" to decide the file is there
     * and worth parsing, so a truncated one must not produce it. */
    if (!sc->open) {
        shell_warn(shell, "Capture CSV write failed - the file is incomplete");
        return -EIO;
    }
    /* Computed BEFORE the trailer write so the verdict can go INTO the file.
     * A shell_warn is the wrong and only channel for it on the path this
     * feature exists for: an app-started capture has no console, the return is
     * deliberately 0, and last_error never leaves the UART — so a misaligned
     * CSV would reach the laptop with a well-formed #PARAMS, well-formed D,
     * rows and a valid #DONE, i.e. nothing in the file itself saying it must
     * not be paired with the WAV. frames.py would parse it happily and
     * capture_to_scenario.py would pick it, making the misaligned capture the
     * one that gets tuned against. The ABANDONED case is already
     * self-describing (no #DONE); this makes MISALIGNED equally so.
     *
     * blocks=/lost= go into #DONE unconditionally, not just on the bad path:
     * they let a host redo the cross-check itself rather than trust that the
     * device would have flagged it. #DONE is parsed as key=value tokens
     * (frames.py::parse), so older files without them still load. */
    uint32_t lost = (uint32_t)atomic_get(&s_capture_analysis_dropped);
    bool misaligned = (sc->frames != blocks || lost != 0);

    char trailer[256];
    int n = snprintf(trailer, sizeof(trailer),
                     "%s#DONE frames=%u dropped=%u blocks=%u lost=%u\n",
                     misaligned ? "#MISALIGNED do NOT pair this file with the WAV\n" : "",
                     sc->frames, dropped, blocks, lost);
    /* Bounded like every other snprintf-then-write in this file: the return is
     * what it WOULD have written, so an outgrown format would hand a length
     * past the end of the buffer to the writer. Unreachable at ~44 B today. */
    bool trailer_ok = n > 0 && (size_t)n < sizeof(trailer) &&
                      tap_write_at_retry(&sc->file, sc->path, sc->pos, trailer, (size_t)n,
                                         sc->io_retries) == 0;
    if (trailer_ok) {
        sc->pos += n;
    }
    fs_close(&sc->file);
    sc->open = false;
    /* No trailer means frames.py leaves frames_reported/dropped unset, so the
     * row-count cross-check the trailer exists for is silently unavailable.
     * Must not fall through to the success line the MCP plugin keys on —
     * record_wav_tap() fails its capture for exactly this, and the branches
     * below are worded to dodge that regex for exactly this reason. */
    if (!trailer_ok) {
        shell_warn(shell, "Capture CSV lost its #DONE trailer - row count unverifiable");
        return -EIO;
    }
    /* Row count MUST equal the number of blocks that reached the WAV — the two
     * files are paired by position, so any drift makes every row after the
     * drift point describe the wrong audio. Nothing downstream can detect that
     * on its own (a D-line's seq counts produced frames, not written ones), so
     * say it here rather than let a quietly misaligned capture be tuned
     * against. A lost analysis row misaligns the file just as surely without
     * changing the count, so it fails the same check.
     *
     * Must NOT lead with "Audio sidecar: <n> frames": the MCP plugin decides
     * the file is worth parsing with a re.search for exactly that prefix, so
     * wording this like the success line would hand every downstream consumer
     * the file this branch exists to reject. */
    if (misaligned) {
        shell_warn(shell,
                   "Capture CSV MISALIGNED: %u rows for %u blocks written (%u rows lost) - "
                   "do NOT pair with the WAV",
                   sc->frames, blocks, lost);
        return -EIO;
    }
#if defined(CONFIG_IMU)
    shell_print(shell, "Audio sidecar: %u frames, %u IMU samples", sc->frames, sc->samples);
#else
    shell_print(shell, "Audio sidecar: %u frames", sc->frames);
#endif
    return 0;
}
#endif /* CONFIG_APP_CAPTURE_AUDIO_SIDECAR */

#if defined(CONFIG_APP_AUDIO_DEBUG)
/* Binds the shared formatter to the rich tap's frame struct. */
static size_t tap_frame_format(const struct audio_tap_frame *f, bool buckets, char *buf,
                               size_t cap) {
    return audio_tap_format_frame(&f->result, f->rms, f->gain, buckets, buf, cap);
}

/* Shell-side drain buffers, shared by record_wav and dump: too big for the shell
 * thread's stack, and safe as statics because only one shell command runs at a time. */
static struct audio_tap_frame s_tap_drain;
static char s_tap_line[512];


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
        uint64_t needed = (uint64_t)total_frames * (BLOCK_SIZE + CAPTURE_TAP_ANALYSIS_BYTES_PER_FRAME) + WAV_DATA_OFFSET +
                          TAP_CSV_CHUNK + 64 * 1024;
        if (needed > free_bytes) {
            /* Multiply by the block time rather than dividing by a frame rate,
             * for the reason record_wav_capture()'s pre-flight spells out:
             * MSEC_PER_SEC / BLOCK_CAPTURE_TIME_MS truncates 31.25 to 31, which
             * understates the per-second cost and so overstates this advice by
             * ~0.8% — enough that re-running at exactly the printed figure fails
             * again with -ENOSPC and the operator has to guess downward. */
            uint32_t max_s =
                (uint32_t)((free_bytes > WAV_DATA_OFFSET + TAP_CSV_CHUNK + 64 * 1024)
                               ? ((free_bytes - WAV_DATA_OFFSET - TAP_CSV_CHUNK - 64 * 1024) *
                                  BLOCK_CAPTURE_TIME_MS) /
                                     ((uint64_t)(BLOCK_SIZE + CAPTURE_TAP_ANALYSIS_BYTES_PER_FRAME) * MSEC_PER_SEC)
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

#if defined(CONFIG_IMU)
    /* Opened here so its t0 is the same instant the WAV's first frame is timed
     * from. A failure is non-fatal: the WAV is the primary artifact and a capture
     * without the sidecar is still useful. */
    struct imu_sidecar imu_sc = {};
    if (imu_sidecar_open(&imu_sc, path, k_uptime_get()) < 0) {
        shell_warn(shell, "IMU sidecar could not be opened; recording audio only");
    }
#endif

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
    /* CSV faults that happen AFTER the capture loop has run to completion, so
     * the WAV is whole and only its sidecar is damaged. Kept apart from
     * io_error so the abort line below cannot claim "capture incomplete - 937
     * of 937 frames". Not the same call as record_wav_capture()'s, which
     * reports the equivalent and returns 0: there the consumer is the phone,
     * which never sees an errno and cannot act on a sidecar. Here the consumer
     * is the MCP capture_scenario handler, which keys on this path's
     * "Wrote ... (N frames, ...)" line as proof the CSV is worth parsing and
     * would hand a damaged one straight to beat_lab. Suppressing that line is
     * what protects it — the handler bails on `"Wrote" not in output`
     * (rgb_sunglasses.py:365,407) — NOT the return value, which it never
     * inspects.
     *
     * Mid-loop CSV failures stay io_error: they break out of the capture, so
     * the WAV is truncated too and the abort line is accurate. */
    bool csv_unusable = false;

    bool stopped_early = false;
    while (frames_captured < total_frames) {
        if (record_stop_requested()) {
            stopped_early = true;
            break;
        }
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
#if defined(CONFIG_IMU)
        imu_sidecar_drain(&imu_sc);
#endif
    }

    atomic_set(&s_tap_armed, 0);
    uint32_t dropped = (uint32_t)atomic_get(&s_tap_dropped);

    /* Flush partial batches (frames that didn't fill a whole sector). */
    if (!io_error && wav_batched > 0) {
        size_t tail = wav_batched * sizeof(s_tap_drain.pcm);
        if (tap_write_at_retry(&f, path, wav_pos, s_wav_batch, tail, &io_retries) == 0) {
            total_bytes += tail;
        } else {
            /* Same contract as record_wav_capture()'s: a short file must not
             * return success. Beyond the shell, the MCP plugin treats this
             * path's "Wrote ... (N frames, ...)" line as proof the analysis CSV
             * is worth parsing, so falling through here hands a truncated
             * artifact to beat_lab and capture_to_scenario.py. */
            shell_error(shell, "WAV write failed on final flush");
            io_error = true;
        }
    }
    /* Deliberately NOT gated on io_error: the two artifacts have independent
     * failure paths, and a WAV tail failure says nothing about the CSV handle.
     * Gating it discarded up to TAP_CSV_CHUNK of already-captured rows and then
     * wrote #DONE at a stale offset — undercutting the promise a few lines
     * below that what WAS captured stays parseable. */
    if (csv_pos > 0) {
        if (tap_write_at_retry(&fcsv, csv_path, csv_file_pos, s_csv_batch, csv_pos,
                               &io_retries) != 0) {
            shell_error(shell, "CSV write failed on final flush");
            csv_unusable = true; /* loses up to a chunk of trailing D-rows */
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
        /* Same reasoning, and worse consequences: without these two patches the
         * file keeps file_size=0/data_size=0 from the prologue, so it is not a
         * WAV at all — wav_duration_ms() reads 0 ms and no sample loads. The
         * console line is invisible to anyone on a phone, and capture.cpp would
         * otherwise report CAPTURE_IDLE with last_error=0 and bump the capture
         * count. The sidecar already refuses to look successful when it is
         * unusable (see the MISALIGNED path); the primary artifact gets the
         * same treatment. */
        shell_error(shell, "WAV header patch failed - the file will not parse as WAV");
        io_error = true;
    }
    fs_close(&f);

    /* Bounded like its sibling in audio_sidecar_close(): a negative snprintf
     * return casts to a huge size_t and would be handed to the writer as a
     * length. Unreachable at ~44 B into 512, but two identical trailer writes
     * with opposite treatment is the configuration most likely to drift back. */
    int trailer_len = snprintf(s_tap_line, sizeof(s_tap_line), "#DONE frames=%u dropped=%u\n",
                               frames_captured, dropped);
    len = (trailer_len > 0 && (size_t)trailer_len < sizeof(s_tap_line)) ? (size_t)trailer_len : 0;
    if (len == 0 ||
        tap_write_at_retry(&fcsv, csv_path, csv_file_pos, s_tap_line, len, &io_retries) != 0) {
        shell_error(shell, "CSV #DONE trailer write failed");
        csv_unusable = true;
    }
    fs_close(&fcsv);
#if defined(CONFIG_IMU)
    imu_sidecar_close(&imu_sc, shell);
#endif

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
    if (csv_unusable) {
        /* Returning before the "Wrote ..." line is the whole mechanism: that
         * string is what the MCP handler treats as proof the analysis CSV is
         * parseable, and it bails on `"Wrote" not in output`. Wording chosen so
         * neither success gate can match it either, same rule as
         * audio_sidecar_close()'s failures (pinned by
         * fw/tools/tests/test_capture_csv_contract.py).
         *
         * Return 0 all the same, for the reason record_wav_capture()'s
         * sidecar_unusable comment gives. This is NOT a debug-only path:
         * sound_record_wav() routes here whenever APP_AUDIO_DEBUG=y and the DSP
         * is streaming, ahead of the APP_CAPTURE branch — including for
         * capture.cpp's worker, i.e. a phone-triggered capture. A non-zero
         * return there becomes CAPTURE_FAILED, so a complete, playable WAV
         * would render a "Failed" badge on an APP_AUDIO_DEBUG=y + APP_CAPTURE=y
         * build, with last_error never leaving the UART to explain it. The
         * automated consumer is already protected by the suppressed line above,
         * so the errno bought nothing and cost exactly the regression the
         * sibling function was just fixed for. */
        shell_error(shell,
                    "Analysis CSV unusable - the WAV is COMPLETE (%u of %u frames, %u dropped) "
                    "but its sidecar is not; keep %s, discard %s",
                    frames_captured, total_frames, dropped, path, csv_path);
        return 0;
    }
    if (stopped_early) {
        shell_print(shell, "Stopped early at %u of %u frames", frames_captured, total_frames);
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

    /* Same one-at-a-time guard the tap and capture paths carry. Without it a
     * shell `sound mic record_wav` and a BLE-triggered capture can both land
     * here — two callers driving one PDM stream into two files. */
    if (!atomic_cas(&s_direct_armed, 0, 1)) {
        shell_error(shell, "A capture is already running");
        return -EBUSY;
    }

    if (!device_is_ready(pdm0)) {
        shell_error(shell, "%s is not ready", pdm0->name);
        atomic_set(&s_direct_armed, 0);
        return -ENOEXEC;
    }
    ret = configure_pdm();
    if (ret < 0) {
        shell_error(shell, "Failed to configure the driver: %d", ret);
        atomic_set(&s_direct_armed, 0);
        return ret;
    }

    struct fs_file_t f;
    fs_file_t_init(&f);
    ret = fs_open(&f, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
    if (ret < 0) {
        shell_error(shell, "Failed to open %s: %d", path, ret);
        atomic_set(&s_direct_armed, 0);
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
        atomic_set(&s_direct_armed, 0);
        return -EIO;
    }

    ret = dmic_trigger(pdm0, DMIC_TRIGGER_START);
    if (ret < 0) {
        shell_error(shell, "START trigger failed: %d", ret);
        fs_close(&f);
        atomic_set(&s_direct_armed, 0);
        return ret;
    }

#if defined(CONFIG_IMU)
    /* The IMU sidecar is written on BOTH capture paths on purpose. This one runs
     * whenever CONFIG_APP_AUDIO_DEBUG is off — which is the default, because the
     * analysis tap costs 33 KB of appcore RAM. Logging IMU only on the tap path
     * would have made recording a scenario require a special build and a reflash,
     * for data the IMU thread produces regardless of the audio configuration. */
    struct imu_sidecar imu_sc = {};
    if (imu_sidecar_open(&imu_sc, path, k_uptime_get()) < 0) {
        shell_warn(shell, "IMU sidecar could not be opened; recording audio only");
    }
#endif

    shell_print(shell, "Recording %u s to %s (direct capture, no analysis sidecar) ...",
                duration_s, path);

    uint32_t total_bytes = 0;
    bool stopped_early = false;
    for (uint32_t i = 0; i < total_blocks; i++) {
        if (record_stop_requested()) {
            stopped_early = true;
            break;
        }
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

#if defined(CONFIG_IMU)
        imu_sidecar_drain(&imu_sc);
#endif
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

#if defined(CONFIG_IMU)
    imu_sidecar_close(&imu_sc, shell);
#endif
    atomic_set(&s_direct_armed, 0);
    if (stopped_early) {
        shell_print(shell, "Stopped early at %u of %u blocks", total_bytes / BLOCK_SIZE,
                    total_blocks);
    }
    shell_print(shell, "Wrote %u bytes of PCM to %s", total_bytes, path);
    return 0;
}

/* Shared by the shell command and the background capture worker.
 *
 * Clearing the stop flag HERE rather than in the worker means a stop requested
 * while idle cannot leak into the next capture and end it instantly. */
#if defined(CONFIG_APP_CAPTURE)
/* Batched straight into one FAT sector: 4 blocks x 1024 B is exactly 4096, so
 * every PCM write lands sector-aligned by arithmetic.
 *
 * (The alignment still needs the JUNK-padded prologue below — this path writes
 * a full WAV_DATA_OFFSET header, not the canonical 44 bytes — and a CSV does
 * share the FF_FS_TINY window for the whole recording. An earlier version of
 * this comment claimed both the opposite; it described a design that predates
 * the prologue and the sidecar alike.) */
/* The budget in sound.h mirrors these; drift here is what makes the clamp and
 * the pre-flight disagree, so it fails the build instead. */
BUILD_ASSERT(CAPTURE_BLOCK_TIME_MS == BLOCK_CAPTURE_TIME_MS,
             "capture budget block time must track BLOCK_CAPTURE_TIME_MS");
BUILD_ASSERT(CAPTURE_WAV_BYTES_PER_FRAME == BLOCK_SIZE,
             "capture budget WAV bytes/frame must track BLOCK_SIZE");
BUILD_ASSERT(CAPTURE_SECTOR_BYTES == WAV_DATA_OFFSET,
             "capture budget sector size must track the WAV prologue");
#if defined(CONFIG_APP_CAPTURE_AUDIO_SIDECAR)
BUILD_ASSERT(CAPTURE_SECTOR_BYTES == AUDIO_CSV_CHUNK,
             "capture budget sector size must track the CSV header sector");
#endif
#if defined(CONFIG_IMU)
BUILD_ASSERT(CAPTURE_SECTOR_BYTES == IMU_CSV_CHUNK,
             "capture budget sector size must track the IMU sidecar header sector - "
             "CAPTURE_OVERHEAD_BYTES charges one sector whenever CONFIG_IMU is set");
#endif

#define CAPTURE_BATCH_BLOCKS 4
static int16_t s_capture_batch[CAPTURE_BATCH_BLOCKS * AUDIO_FFT_SIZE];

/* Empty both capture taps, leaving them in step.
 *
 * A plain purge of each in turn is not enough. The DSP thread runs at a higher
 * priority than this one and publishes the PCM block and its analysis as two
 * separate puts, so a purge landing between them keeps one half of a frame and
 * discards the other — after which the WAV and the sidecar are offset by one
 * block for the whole recording, silently, because a PCM block carries nothing
 * to align against.
 *
 * The re-check converges: the only interleaving that can leave the queues
 * unequal is a purge splitting a pair, and that always leaves one queue
 * non-empty, which the loop condition catches. Observing both empty while a
 * put is in flight is harmless in the other direction — the producer always
 * publishes PCM first, so a PCM already counted means its analysis is on the
 * way and the pair arrives intact. */
static void capture_taps_purge(void) {
#if defined(CONFIG_APP_CAPTURE_AUDIO_SIDECAR)
    do {
        k_msgq_purge(&capture_pcm_q);
        k_msgq_purge(&capture_analysis_q);
    } while (k_msgq_num_used_get(&capture_pcm_q) != 0 ||
             k_msgq_num_used_get(&capture_analysis_q) != 0);
#else
    k_msgq_purge(&capture_pcm_q);
#endif
}

/* Capture from the lean PCM tap: the DSP thread stays the sole dmic_read()
 * consumer (so beat-reactive animations keep running through a recording) and
 * this drains its copy, plus — when CONFIG_APP_CAPTURE_AUDIO_SIDECAR is set —
 * the matching per-frame analysis into "<wav>.csv". */
static int record_wav_capture(const struct shell *shell, uint32_t duration_s, const char *path) {
    const uint32_t total_blocks = (duration_s * MSEC_PER_SEC) / BLOCK_CAPTURE_TIME_MS;

    if (!atomic_cas(&s_capture_armed, 0, 1)) {
        shell_error(shell, "A capture is already draining the tap");
        return -EBUSY;
    }

    /* Free-space pre-flight, same contract as record_wav_tap()'s — this path
     * REPLACES that one on a default build, so without it `sound mic record_wav
     * 180 ...` on a near-full volume fills the disk to zero and aborts partway,
     * after which unrelated writes and DFU staging fail with -ENOSPC. The BLE
     * front end clamps against its own cached figure before getting here; this
     * covers the shell caller, and re-checks with a live figure for both.
     * WAV (1024 B/frame) + IMU sidecar (~56 B/frame at 25 Hz) + analysis
     * sidecar (~360 B/frame, one D-line per block) + the prologues + 64 KB of
     * slack for FAT metadata and whatever else writes meanwhile. Keep the
     * per-frame figures in step with kBytesPerSecond in capture.cpp, which is
     * what the app's Remaining S readout and the limit clamp are derived from. */
    struct fs_statvfs vfs;
    if (fs_statvfs("/NAND:", &vfs) == 0) {
        /* One CSV carries both streams now, so there is one padded header
         * sector to account for, not two. */
        uint64_t per_frame = CAPTURE_WAV_BYTES_PER_FRAME;
        uint64_t overhead = CAPTURE_OVERHEAD_BYTES;
#if defined(CONFIG_IMU)
        per_frame += CAPTURE_IMU_BYTES_PER_FRAME;
#endif
#if defined(CONFIG_APP_CAPTURE_AUDIO_SIDECAR)
        per_frame += CAPTURE_ANALYSIS_BYTES_PER_FRAME;
#endif
        uint64_t free_bytes = (uint64_t)vfs.f_bfree * vfs.f_frsize;
        uint64_t needed = (uint64_t)total_blocks * per_frame + overhead;
        if (needed > free_bytes) {
            /* Multiply by the block time rather than dividing by a frame
             * rate: MSEC_PER_SEC / BLOCK_CAPTURE_TIME_MS truncates 31.25 to
             * 31, which understates the cost per second and so overstates the
             * advice by ~0.8 % — enough that retrying at the printed figure
             * fails again and the operator has to guess downward. This matches
             * kBytesPerSecond in capture.cpp, which already computes it this
             * way. */
            uint32_t max_s =
                (uint32_t)((free_bytes > overhead) ? ((free_bytes - overhead) *
                                                      BLOCK_CAPTURE_TIME_MS) /
                                                         ((uint64_t)per_frame * MSEC_PER_SEC)
                                                   : 0);
            shell_error(shell, "Not enough free space for %u s (max ~%u s free)", duration_s,
                        max_s);
            atomic_set(&s_capture_armed, 0);
            return -ENOSPC;
        }
    }

    /* Shared by the WAV and the CSV so one figure reports how much transient
     * flash trouble the whole capture rode out. */
    uint32_t io_retries = 0;

    struct fs_file_t f;
    fs_file_t_init(&f);
    int ret = fs_open(&f, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
    if (ret < 0) {
        shell_error(shell, "Failed to open %s: %d", path, ret);
        atomic_set(&s_capture_armed, 0);
        return ret;
    }

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
    /* One full sector: RIFF+fmt, a JUNK pad, then the data chunk header, so PCM
     * begins exactly at WAV_DATA_OFFSET and every batch write below is
     * sector-aligned. s_capture_batch doubles as the scratch for it — it is
     * exactly one sector and the drain loop has not started yet. */
    memset(s_capture_batch, 0, sizeof(s_capture_batch));
    uint8_t *prologue = (uint8_t *)s_capture_batch;
    memcpy(&prologue[0], &hdr, 36); /* RIFF(12) + fmt(24), without the data header */
    const uint32_t junk_size = WAV_JUNK_PAD;
    memcpy(&prologue[36], "JUNK", 4);
    memcpy(&prologue[40], &junk_size, 4);
    memcpy(&prologue[WAV_DATA_OFFSET - 8], "data", 4);
    if (fs_write(&f, prologue, WAV_DATA_OFFSET) != (ssize_t)WAV_DATA_OFFSET) {
        shell_error(shell, "Failed to write WAV header");
        fs_close(&f);
        atomic_set(&s_capture_armed, 0);
        return -EIO;
    }

#if defined(CONFIG_APP_CAPTURE_AUDIO_SIDECAR)
    /* ONE sidecar carrying both the analysis and the IMU rows, so a capture
     * holds two FatFs handles rather than three — see the struct's comment for
     * why that matters under FF_FS_TINY. Non-fatal if it cannot be opened: the
     * WAV is the primary artifact and a capture without it is still a capture.
     * Opened before the purge so its #PARAMS header describes the parameters in
     * force at t=0, and given the same t0 the WAV starts at so the I-rows share
     * its timebase by construction. */
    struct audio_sidecar audio_sc = {};
    if (audio_sidecar_open(&audio_sc, path, k_uptime_get(), &io_retries) < 0) {
        shell_warn(shell, "Capture CSV could not be opened; recording audio only");
    }
#elif defined(CONFIG_IMU)
    struct imu_sidecar imu_sc = {};
    if (imu_sidecar_open(&imu_sc, path, k_uptime_get()) < 0) {
        shell_warn(shell, "IMU sidecar could not be opened; recording audio only");
    }
#endif

    /* Purge AFTER arming and opening: anything queued while the files were being
     * created predates t=0 and would desynchronise the WAV from the sidecar. */
    capture_taps_purge();
    atomic_set(&s_capture_dropped, 0);
#if defined(CONFIG_APP_CAPTURE_AUDIO_SIDECAR)
    atomic_set(&s_capture_analysis_dropped, 0);
#endif
    shell_print(shell, "Recording %u s (%u blocks) to %s ...", duration_s, total_blocks, path);

    uint32_t blocks_captured = 0;
    uint32_t batched = 0;
    uint32_t total_bytes = 0;
    uint32_t timeouts = 0;
    bool io_error = false;
    bool stopped_early = false;
    /* Absolute offset, because recovering from a transient -EIO means closing
     * and reopening the file, which loses the handle's own position. */
    off_t wav_pos = WAV_DATA_OFFSET;

    while (blocks_captured < total_blocks) {
        if (record_stop_requested()) {
            stopped_early = true;
            break;
        }
        /* Drained straight into its slot in the batch — no separate 1 KB staging
         * buffer, which is a saving worth having on a path whose whole point is
         * being lean. */
        ret = k_msgq_get(&capture_pcm_q, &s_capture_batch[batched * AUDIO_FFT_SIZE],
                         K_MSEC(1000));
        if (ret != 0) {
            if (++timeouts >= 5) {
                shell_error(shell, "Tap produced nothing for 5 s - aborting at block %u",
                            blocks_captured);
                io_error = true;
                break;
            }
            continue;
        }
        timeouts = 0;
        blocks_captured++;
        batched++;

#if defined(CONFIG_APP_CAPTURE_AUDIO_SIDECAR)
        audio_sidecar_drain(&audio_sc); /* drains the IMU tap too */
#elif defined(CONFIG_IMU)
        imu_sidecar_drain(&imu_sc);
#endif

        if (batched == CAPTURE_BATCH_BLOCKS) {
            /* A single -EIO here used to abort the whole capture. It is a
             * transient QSPI hiccup that close + reopen + reseek clears — the
             * rich tap has ridden it out this way since it was first observed;
             * this path simply never inherited the helper. */
            if (tap_write_at_retry(&f, path, wav_pos, s_capture_batch, sizeof(s_capture_batch),
                                   &io_retries) != 0) {
                shell_error(shell, "PCM write failed at block %u (after retries)",
                            blocks_captured);
                io_error = true;
                break;
            }
            wav_pos += (off_t)sizeof(s_capture_batch);
            total_bytes += (uint32_t)sizeof(s_capture_batch);
            batched = 0;
        }
    }

    /* Flush the partial batch so an early stop keeps every block it captured. */
    if (batched > 0 && !io_error) {
        size_t tail = batched * AUDIO_FFT_SIZE * sizeof(int16_t);
        if (tap_write_at_retry(&f, path, wav_pos, s_capture_batch, tail, &io_retries) == 0) {
            wav_pos += (off_t)tail;
            total_bytes += (uint32_t)tail;
        } else {
            /* Short WAV. Must fail the capture, not just log: capture.cpp keys
             * the whole app-facing state off this function's return value, so
             * returning 0 here notifies Ready with last_error=0 for a file that
             * is missing its tail. */
            shell_error(shell, "PCM write failed on final flush");
            io_error = true;
        }
    }

    atomic_set(&s_capture_armed, 0);

    /* Patch the two sizes in place rather than rewriting the sector: file_size at
     * offset 4, data_size in the data-chunk header at WAV_DATA_OFFSET - 4. */
    uint32_t file_size = total_bytes + WAV_DATA_OFFSET - 8;
    bool patch_ok =
        tap_write_at_retry(&f, path, 4, &file_size, sizeof(file_size), &io_retries) == 0;
    patch_ok = tap_write_at_retry(&f, path, WAV_DATA_OFFSET - 4, &total_bytes,
                                  sizeof(total_bytes), &io_retries) == 0 &&
               patch_ok;
    if (!patch_ok) {
        /* Same reasoning, and worse consequences: without these two patches the
         * file keeps file_size=0/data_size=0 from the prologue, so it is not a
         * WAV at all — wav_duration_ms() reads 0 ms and no sample loads. The
         * console line is invisible to anyone on a phone, and capture.cpp would
         * otherwise report CAPTURE_IDLE with last_error=0 and bump the capture
         * count. The sidecar already refuses to look successful when it is
         * unusable (see the MISALIGNED path); the primary artifact gets the
         * same treatment. */
        shell_error(shell, "WAV header patch failed - the file will not parse as WAV");
        io_error = true;
    }
    fs_close(&f);

    uint32_t dropped = (uint32_t)atomic_get(&s_capture_dropped);

#if !defined(CONFIG_APP_CAPTURE_AUDIO_SIDECAR) && defined(CONFIG_IMU)
    imu_sidecar_close(&imu_sc, shell);
#endif
#if defined(CONFIG_APP_CAPTURE_AUDIO_SIDECAR)
    /* Whatever is still queued stays there: those blocks were produced after
     * the last one this loop consumed, so they are not in the WAV and their
     * rows would run past the end of the audio.
     *
     * Checked against the blocks that actually REACHED the file, derived from
     * total_bytes, not against blocks_captured. They diverge on the abort path:
     * a failed batch write leaves up to CAPTURE_BATCH_BLOCKS blocks counted as
     * captured, with their rows already written, while the partial-batch flush
     * below the loop is skipped — so the sidecar overruns the audio and
     * blocks_captured would report them equal. On a clean capture the two are
     * identical, so this costs nothing there. */
    /* Tracked apart from io_error on purpose: folding it in made the operator
     * see "ABORTED: 625 of 625 blocks saved" for a WAV that is complete and
     * playable, contradicting the invariant this design rests on
     * (audio_sidecar_flush(): "the WAV is the primary artifact").
     *
     * It does NOT change the return value. An earlier version of this returned
     * -EBADMSG "so the app can tell the two apart", which was wrong on its own
     * premise: last_error is never published over BLE. capture_service.cpp
     * exposes six characteristics and on_capture_state() copies only state,
     * elapsed_s, remaining_s and captures — last_error reaches the UART
     * (`capture status`) and the log, nowhere else. So the ONLY app-visible
     * effect of any non-zero return here is capture.cpp's
     * `state = CAPTURE_FAILED`, which renders as a "Failed" badge that is
     * indistinguishable from a truncated WAV — the exact confusion the split
     * was meant to remove, made worse: a field user would see a good 60 s
     * recording reported as failed while the capture count bumped anyway.
     * Returning 0 also restores what shipped before this PR, when
     * imu_sidecar_close() was void and a CSV fault never touched the return. */
    bool sidecar_unusable =
        audio_sidecar_close(&audio_sc, shell, total_bytes / BLOCK_SIZE, dropped) != 0;
#endif

    if (io_error) {
        shell_error(shell, "ABORTED: %u of %u blocks saved to %s (%u io retries)",
                    blocks_captured, total_blocks, path, io_retries);
        return -EIO;
    }
#if defined(CONFIG_APP_CAPTURE_AUDIO_SIDECAR)
    if (sidecar_unusable) {
        /* Reported, not returned — see the comment on sidecar_unusable above.
         * The console is the only surface that can distinguish this, and it is
         * also the only one whose audience can act on it: nothing a phone user
         * can do fixes a sidecar, and the WAV they came for is intact.
         *
         * Unlike record_wav_tap()'s equivalent, this cannot feed a bad CSV to
         * an automated consumer: that path's success line is what the MCP
         * capture_scenario handler keys on, whereas this one says "blocks"
         * rather than "frames" and is matched by neither gate (pinned by
         * test_capture_path_summary_is_not_read_as_an_analysis_file). The
         * handler cannot reach this function anyway — it requires
         * APP_AUDIO_DEBUG, which APP_CAPTURE_AUDIO_SIDECAR depends on being
         * off. */
        shell_error(shell,
                    "Capture CSV unusable - the WAV is COMPLETE (%u blocks, %u dropped) but "
                    "its sidecar is not; keep %s, discard the .csv (see the warning above)",
                    blocks_captured, dropped, path);
    }
#endif
    if (stopped_early) {
        shell_print(shell, "Stopped early at %u of %u blocks", blocks_captured, total_blocks);
    }
    shell_print(shell, "Wrote %u bytes of PCM to %s (%u blocks, %u dropped, %u io retries)",
                total_bytes, path, blocks_captured, dropped, io_retries);
    if (dropped > 0) {
        shell_warn(shell, "%u blocks dropped - raise CONFIG_APP_CAPTURE_QUEUE_FRAMES", dropped);
    }
    return 0;
}
#endif /* CONFIG_APP_CAPTURE */

int sound_record_wav(const struct shell *shell, uint32_t duration_s, const char *path) {
    /* Deliberately does NOT clear the stop flag — sound_record_arm() does, at the
     * moment the capture is REQUESTED. Clearing here loses a stop pressed during
     * the pre-roll: the capture manager publishes RECORDING (so the app's Stop
     * button is live) before the worker has finished its free-space check and
     * directory scan, and a stop landing in that window was ACKed, wiped here,
     * and then never honoured — the device recorded the full limit while the UI
     * showed a press that did nothing. */
#if defined(CONFIG_APP_AUDIO_DEBUG)
    if (atomic_get(&s_dsp_running)) {
        return record_wav_tap(shell, duration_s, path);
    }
    shell_warn(shell,
               "DSP thread not streaming - falling back to direct capture (no analysis "
               "sidecar)");
#endif
#if defined(CONFIG_APP_CAPTURE)
    if (atomic_get(&s_dsp_running)) {
        /* The DSP owns the PDM stream, so the direct path cannot have the mic.
         * Before this existed that was simply an error on a stock build, which
         * made field capture impossible without the 33 KB analysis tap. */
        return record_wav_capture(shell, duration_s, path);
    }
#else
    if (atomic_get(&s_dsp_running)) {
        shell_error(shell, "DSP thread owns the PDM stream and no capture tap is compiled in");
        return -ENOTSUP;
    }
#endif
    return record_wav_direct(shell, duration_s, path);
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

    /* This path has no pre-roll — the request and the loop are the same call —
     * but it still has to clear any stale flag, since sound_record_wav() no
     * longer does. See sound_record_arm(). */
    sound_record_arm();
    return sound_record_wav(shell, duration_s, path);
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
                fmt_fixed4(s_agc_controller.smoothedRms(), b1, sizeof(b1)),
                fmt_fixed4(s_latest_rms, b2, sizeof(b2)));
    shell_print(shell, "  Peak: %d (%s norm)", s_latest_peak,
                fmt_fixed4(peak_norm, b1, sizeof(b1)));
    shell_print(shell, "  Target window: [%s, %s] | Rate limit: %u frames",
                fmt_fixed4(sAgcProvider->getTargetLow(), b1, sizeof(b1)),
                fmt_fixed4(sAgcProvider->getTargetHigh(), b2, sizeof(b2)),
                sAgcProvider->getRateLimitFrames());
    /* The gate operates on the INPUT-REFERRED level (smoothed RMS normalized to
     * 0 dB gain) — print that value too so operators tune against the number
     * the controller actually compares, not the output-domain smoothed RMS. */
    float input_ref = s_agc_controller.inputReferredRms();
    char b3[16];
    shell_print(shell, "  Attack: %u frames | Release: %u frames | Gate: %s input-referred "
                       "(now %s) -> %s",
                sAgcProvider->getAttackFrames(), sAgcProvider->getReleaseFrames(),
                fmt_fixed4(sAgcProvider->getNoiseGateRms(), b1, sizeof(b1)),
                fmt_fixed4(input_ref, b3, sizeof(b3)),
                s_agc_silent ? "SILENT - beats gated" : "open");
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
    /* Range matches the Phase 2 clamp incl. the migration floor (see
     * AudioConfig::setTargetHigh). */
    if (!parse_finite_float(argv[1], &val) || val < 0.02f || val > 0.5f) {
        shell_error(shell, "Value must be a number in range [0.02, 0.5]");
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
    agc_apply_gain((int)v);
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
    char b1[16], b2[16], b3[16], b4[16];
    uint32_t mode = p->getThresholdMode();
    shell_print(shell, "gamma: %s | floor: %s | alpha: %s | refractory: %u frames",
                fmt_fixed4(p->getFluxGamma(), b1, sizeof(b1)),
                fmt_fixed4(p->getBeatFluxFloor(), b2, sizeof(b2)),
                fmt_fixed4(p->getBeatAlpha(), b3, sizeof(b3)), p->getBeatRefractoryFrames());
    shell_print(shell, "threshold mode: %u (%s) | sf_delta: %s", mode,
                mode == AUDIO_THRESHOLD_MODE_MEDIAN_DELTA ? "median + sf_delta"
                                                          : "mean + alpha*sigma",
                fmt_fixed4(p->getSfDelta(), b4, sizeof(b4)));
    return 0;
}

static int cmd_sound_dsp_set(const struct shell *shell, size_t argc, char **argv) {
    if (argc != 3) {
        shell_error(shell,
                    "Usage: sound dsp set <gamma|floor|alpha|refractory|sf_delta|mode> <value>");
        return -EINVAL;
    }
    AudioDspConfigProvider *p = audio_dsp_get_config_provider();
    const char *name = argv[1];
    char buf[16];

    if (strcmp(name, "refractory") == 0 || strcmp(name, "mode") == 0) {
        char *end = nullptr;
        unsigned long v = strtoul(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0') {
            shell_error(shell, "Invalid value: %s", argv[2]);
            return -EINVAL;
        }
        /* Both read back through the getter so the printed value reflects clamping. */
        if (strcmp(name, "mode") == 0) {
            p->setThresholdMode((uint32_t)v);
            uint32_t mode = p->getThresholdMode();
            shell_print(shell, "threshold mode set to %u (%s)", mode,
                        mode == AUDIO_THRESHOLD_MODE_MEDIAN_DELTA ? "median + sf_delta"
                                                                  : "mean + alpha*sigma");
            return 0;
        }
        p->setBeatRefractoryFrames((uint32_t)v);
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
    } else if (strcmp(name, "sf_delta") == 0) {
        p->setSfDelta(v);
        shell_print(shell, "sf_delta set to %s", fmt_fixed4(p->getSfDelta(), buf, sizeof(buf)));
    } else {
        shell_error(shell, "Unknown parameter '%s' (gamma|floor|alpha|refractory|sf_delta|mode)",
                    name);
        return -EINVAL;
    }
    return 0;
}

static int cmd_sound_agc_attack(const struct shell *shell, size_t argc, char **argv) {
    if (argc == 1) {
        shell_print(shell, "AGC attack: %u frames (~%u ms over target-high before -1 step)",
                    sAgcProvider->getAttackFrames(), sAgcProvider->getAttackFrames() * 32);
        return 0;
    }
    uint32_t val = (uint32_t)strtoul(argv[1], NULL, 10);
    if (val < 1 || val > 20) {
        shell_error(shell, "Value must be in range [1, 20] frames");
        return -EINVAL;
    }
    sAgcProvider->setAttackFrames(val);
    shell_print(shell, "AGC attack set to %u frames", val);
    return 0;
}

static int cmd_sound_agc_release(const struct shell *shell, size_t argc, char **argv) {
    if (argc == 1) {
        shell_print(shell, "AGC release: %u frames (~%u ms under target-low before +1 step)",
                    sAgcProvider->getReleaseFrames(), sAgcProvider->getReleaseFrames() * 32);
        return 0;
    }
    uint32_t val = (uint32_t)strtoul(argv[1], NULL, 10);
    if (val < 1 || val > 100) {
        shell_error(shell, "Value must be in range [1, 100] frames");
        return -EINVAL;
    }
    sAgcProvider->setReleaseFrames(val);
    shell_print(shell, "AGC release set to %u frames", val);
    return 0;
}

static int cmd_sound_agc_gate(const struct shell *shell, size_t argc, char **argv) {
    char buf[16];
    if (argc == 1) {
        shell_print(shell,
                    "AGC noise gate: INPUT-REFERRED RMS (normalized to 0 dB gain) < %s = "
                    "silence (hold/park gain, no beats). 0 disables the gate entirely, "
                    "restoring the pre-Phase-2 chase-quiet-sources behavior.",
                    fmt_fixed4(sAgcProvider->getNoiseGateRms(), buf, sizeof(buf)));
        return 0;
    }
    float val;
    if (!parse_finite_float(argv[1], &val) || val < 0.0f || val > 0.02f) {
        shell_error(shell, "Value must be a number in range [0, 0.02]");
        return -EINVAL;
    }
    sAgcProvider->setNoiseGateRms(val);
    shell_print(shell, "AGC noise gate set to %s (input-referred)",
                fmt_fixed4(val, buf, sizeof(buf)));
    return 0;
}

static int cmd_sound_rms(const struct shell *shell, size_t argc, char **argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    float peak_norm = (float)s_latest_peak / 32768.0f;
    char b1[16], b2[16];
    shell_print(shell, "Smoothed RMS (1s): %s",
                fmt_fixed4(s_agc_controller.smoothedRms(), b1, sizeof(b1)));
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
                               SHELL_CMD_ARG(attack, NULL, "Get/set AGC attack frames", cmd_sound_agc_attack, 0, 1),
                               SHELL_CMD_ARG(release, NULL, "Get/set AGC release frames", cmd_sound_agc_release, 0, 1),
                               SHELL_CMD_ARG(gate, NULL, "Get/set AGC noise-gate RMS", cmd_sound_agc_gate, 0, 1),
#if defined(CONFIG_APP_AUDIO_DEBUG)
                               SHELL_CMD_ARG(freeze, NULL, "Get/set AGC freeze (halt gain adjustment)", cmd_sound_agc_freeze, 0, 1),
                               SHELL_CMD_ARG(gain, NULL, "Get/set PDM gain register directly (implies freeze)", cmd_sound_agc_gain, 0, 1),
#endif
                               SHELL_SUBCMD_SET_END);

#if defined(CONFIG_APP_AUDIO_TELEMETRY)
/* On-device oracle for the BLE telemetry stream.
 *
 * Exists because everything the app shows is quantised and arrives over a link that can
 * silently degrade: without a serial-side view there is no way to tell "the meters are
 * wrong" from "the meters are right and the room really is like that". This reads the same
 * accumulator the notifier does, so a mismatch between this and the app is a transport
 * problem, and agreement means the transport is fine and the DSP is what to look at.
 *
 * Note it CONSUMES a frame (take() clears the window), so running it while the app is
 * streaming steals that frame from the phone. That is fine for debugging and would be
 * confusing to discover by accident, hence this comment. */
static int cmd_sound_telemetry_status(const struct shell *shell, size_t argc, char **argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    shell_print(shell, "Telemetry: %s", audio_telemetry_is_active() ? "streaming" : "idle");

    struct audio_telemetry_frame f;
    const bool fresh = audio_telemetry_take(&f);

    char b1[16], b2[16], b3[16], b4[16];
    shell_print(shell, "  seq %u | %s | dropped %u | clips %u", f.seq,
                fresh ? "fresh" : "STALE (nothing new since last take)", f.dropped,
                f.clip_count);
    shell_print(shell, "  gain %+d steps (%s%d.%u dB) | settled %u frames", f.gain_steps,
                f.gain_steps < 0 ? "-" : "", abs(f.gain_steps) / 2,
                (unsigned)((abs(f.gain_steps) % 2) * 5), f.frames_since_step);
    shell_print(shell, "  rms in-ref %s | inst %s | peak %s | noise floor %s",
                fmt_fixed4(f.rms_input_referred, b1, sizeof(b1)),
                fmt_fixed4(f.rms_instant, b2, sizeof(b2)),
                fmt_fixed4(f.peak, b3, sizeof(b3)),
                fmt_fixed4(f.noise_floor, b4, sizeof(b4)));
    shell_print(shell, "  flags:%s%s%s | mode %u | beats 0x%x",
                f.silent ? " SILENT" : "", f.clipped ? " CLIP" : "",
                f.agc_frozen ? " FROZEN" : "", f.threshold_mode, f.beat_mask);

    for (int b = 0; b < AUDIO_NUM_BANDS; b++) {
        char fb[16], tb[16];
        shell_print(shell, "  band %d: flux %s | fires at %s", b,
                    fmt_fixed4(f.flux[b], fb, sizeof(fb)),
                    fmt_fixed4(f.threshold[b], tb, sizeof(tb)));
    }
    return 0;
}

/* Drive the accumulator with no phone attached, so the pack path can be exercised without
 * needing a subscriber. */
static int cmd_sound_telemetry_sim(const struct shell *shell, size_t argc, char **argv) {
    ARG_UNUSED(argc);
    const bool on = (strcmp(argv[1], "on") == 0);
    if (on) {
        audio_telemetry_reset();
    }
    audio_telemetry_set_active(on);
    shell_print(shell, "Telemetry accumulation %s", on ? "forced ON" : "OFF");
    shell_print(shell,
                "  (gates the accumulator only; the BLE stream still needs a control write)");
    return 0;
}
#endif /* CONFIG_APP_AUDIO_TELEMETRY */

// Subcommands for "sound dsp" (beat-detection parameters)
SHELL_STATIC_SUBCMD_SET_CREATE(sub_sound_dsp,
                               SHELL_CMD_ARG(params, NULL, "Print beat-detection parameters", cmd_sound_dsp_params, 0, 0),
                               SHELL_CMD_ARG(set, NULL, "Set parameter: <gamma|floor|alpha|refractory|sf_delta|mode> <value>", cmd_sound_dsp_set, 3, 0),
                               SHELL_SUBCMD_SET_END);
// clang-format on

#if defined(CONFIG_APP_AUDIO_TELEMETRY)
SHELL_STATIC_SUBCMD_SET_CREATE(sub_sound_telemetry,
                               SHELL_CMD_ARG(status, NULL, "Print the latest telemetry frame",
                                             cmd_sound_telemetry_status, 0, 0),
                               SHELL_CMD_ARG(sim, NULL, "Force accumulation on/off: <on|off>",
                                             cmd_sound_telemetry_sim, 2, 0),
                               SHELL_SUBCMD_SET_END);
#endif

// Subcommands for "sound"
SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_sound,
#if defined(CONFIG_VM3011)
    SHELL_CMD(vm, &sub_sound_vm, "VM3011 Commands", NULL),
#endif
    SHELL_CMD(mic, &sub_sound_mic, "Mic Commands", NULL),
    SHELL_CMD(agc, &sub_sound_agc, "AGC Commands", NULL),
    SHELL_CMD(dsp, &sub_sound_dsp, "Beat-detection DSP parameters", NULL),
    SHELL_CMD(rms, NULL, "Print current RMS level", cmd_sound_rms),
#if defined(CONFIG_APP_AUDIO_TELEMETRY)
    SHELL_CMD(telemetry, &sub_sound_telemetry, "Live BLE telemetry stream", NULL),
#endif
#if defined(CONFIG_APP_AUDIO_DEBUG)
    SHELL_CMD_ARG(dump, NULL, "Stream per-frame analysis: <frames> [buckets]", cmd_sound_dump, 2,
                  1),
#endif
    SHELL_SUBCMD_SET_END);

/* Creating root (level 0) command "sound" */
SHELL_CMD_REGISTER(sound, &sub_sound, "Sound commands", NULL);