#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/shell/shell.h>

#if defined(CONFIG_VM3011)
#include <zephyr/drivers/vm3011/vm3011.h>
#endif
#include <math.h> /* sqrtf */
#include <stdlib.h>
#include <zephyr/audio/dmic.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>

#include "audio_dsp.h"

/* ── Adaptive Gain Control ───────────────────────────────────────────────────
 * Adjusts PDM hardware gain to keep the RMS signal level inside a target
 * window.  Gain steps are rate-limited to prevent pumping.  After every
 * adjustment audio_dsp_reset_history() is called because the amplitude
 * discontinuity would otherwise look like a beat onset.
 *
 * Thresholds calibrated to the actual microphone output. RMS measured with
 * active music: 0.015–0.025 (sparse due to gaps between notes).
 * Target window [0.005, 0.008] keeps signal quiet to prevent FFT saturation.
 * All threshold values are tunable via shell commands. */
static float s_agc_target_low = 0.005f;  /* increase gain when quieter than this */
static float s_agc_target_high = 0.008f; /* decrease gain when louder than this  */
#define AGC_GAIN_MIN 0x00                /* −20 dB (PDM GAINL/GAINR register floor)        */
#define AGC_GAIN_MAX 0x50                /* +20 dB (PDM GAINL/GAINR register ceiling)      */
static uint8_t s_agc_rate_limit = 10;    /* minimum frames between gain steps (~320 ms) */

/* PDM gain register pointers — set once in configure_pdm(), used by AGC loop. */
static volatile uint32_t *s_gain_l;
static volatile uint32_t *s_gain_r;

static uint8_t s_agc_gain = 0x28;   /* current gain register value (0 dB) */
static int s_agc_frames_since = 0;  /* frames elapsed since last adjustment */
static float s_latest_rms = 0.0f;   /* latest instantaneous RMS */
static float s_smoothed_rms = 0.0f; /* 1-second averaged RMS for AGC decisions */
static int16_t s_latest_peak = 0;   /* latest peak sample magnitude */

/* 1-second RMS history (32 frames at 32 ms/frame) */
#define AGC_HISTORY_LEN 32
static float s_rms_history[AGC_HISTORY_LEN];
static uint8_t s_rms_history_idx = 0;

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

K_THREAD_DEFINE(audio_dsp_thread,
                8096,  // stack size
                audio_dsp_thread_func, NULL, NULL, NULL,
                -7,         // Priority
                K_FP_REGS,  // Options
                0           // Startup delay
);

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
    uint32_t seq = 0;

    while (true) {
        void *buffer = NULL;
        uint32_t size = 0;

        ret = dmic_read(pdm0, 0, &buffer, &size, READ_TIMEOUT);
        if (ret) {
            LOG_ERR("Failed to read block %d", ret);
            if (buffer != NULL) {
                k_mem_slab_free(&mem_slab, buffer);
            }
            continue;
        }

        const int16_t *pcm = static_cast<const int16_t *>(buffer);

        /* AGC: 1-second history window for stable gain control. */
        float rms = agc_compute_rms(pcm, AUDIO_FFT_SIZE);
        s_latest_rms = rms; /* Instantaneous RMS for diagnostics */

        /* Update RMS history (same structure as beat detection flux history). */
        s_rms_history[s_rms_history_idx] = rms;
        s_rms_history_idx = (s_rms_history_idx + 1) % AGC_HISTORY_LEN;

        /* Compute mean RMS over the 1-second window. */
        float sum_rms = 0.0f;
        for (int i = 0; i < AGC_HISTORY_LEN; i++) {
            sum_rms += s_rms_history[i];
        }
        s_smoothed_rms = sum_rms / (float)AGC_HISTORY_LEN;

        /* Check for gain adjustment every s_agc_rate_limit frames. */
        s_agc_frames_since++;
        if (s_agc_frames_since >= s_agc_rate_limit) {
            bool gain_changed = false;

            /* Use smoothed RMS (1-second average) for stable decisions. */
            if (s_smoothed_rms < s_agc_target_low && s_agc_gain < AGC_GAIN_MAX) {
                s_agc_gain++;
                gain_changed = true;
            } else if (s_smoothed_rms > s_agc_target_high && s_agc_gain > AGC_GAIN_MIN) {
                s_agc_gain--;
                gain_changed = true;
            }

            if (gain_changed) {
                *s_gain_l = s_agc_gain;
                *s_gain_r = s_agc_gain;
                /* Gain discontinuity invalidates the flux history. */
                audio_dsp_reset_history();
                s_agc_frames_since = 0;
                float db = (float)s_agc_gain * 0.5f - 20.0f;
                LOG_DBG("AGC: gain=0x%02x (%.1f dB) smoothed_rms=%.4f", s_agc_gain, (double)db,
                        (double)s_smoothed_rms);
            }
        }

        struct audio_analysis_result result;
        audio_dsp_process(pcm, seq++, &result);
        k_mem_slab_free(&mem_slab, buffer);

        // Log beats including noise-floor stats for threshold tuning
        for (int b = 0; b < AUDIO_NUM_BANDS; b++) {
            // Disabled output for now
            if (false && result.beat[b]) {
                LOG_INF(
                    "beat band=%d energy=%.5f mean=%.5f sigma=%.5f "
                    "threshold=%.5f seq=%u",
                    b, (double)result.band_energy[b], (double)result.band_mean[b],
                    (double)result.band_sigma[b],
                    (double)(result.band_mean[b] + 2.0f * result.band_sigma[b]), result.seq);
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

#define DEFAULT_WAV_PATH "/NAND:/sound.wav"
#define DEFAULT_RECORD_DURATION_S 10

static int cmd_sound_mic_record_wav(const struct shell *shell, size_t argc, char **argv,
                                    void *data) {
    int ret;

    // argv[1] = optional duration in seconds, argv[2] = optional output path
    uint32_t duration_s = DEFAULT_RECORD_DURATION_S;
    const char *path = DEFAULT_WAV_PATH;

    if (argc > 1) {
        duration_s = (uint32_t)strtoul(argv[1], NULL, 10);
        if (duration_s == 0) {
            shell_error(shell, "Invalid duration: %s", argv[1]);
            return -EINVAL;
        }
    }
    if (argc > 2) {
        path = argv[2];
    }

    // Total blocks needed; the slab recycles so this can exceed BLOCK_COUNT
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

    // Open (or create/truncate) the output file
    struct fs_file_t f;
    fs_file_t_init(&f);
    ret = fs_open(&f, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
    if (ret < 0) {
        shell_error(shell, "Failed to open %s: %d", path, ret);
        return ret;
    }

    // Write a placeholder header; sizes will be patched after recording
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

    shell_print(shell, "Recording %u s to %s ...", duration_s, path);

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
            shell_error(shell, "Short write on block %u (%d of %u bytes)", i, written, size);
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

// Subcommands for "sound mic"
SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_sound_mic,
    /*SHELL_CMD(record, NULL, "Record sound to console (hex)", cmd_sound_mic_record),*/
    SHELL_CMD_ARG(record_wav, NULL, "Record sound to WAV file [duration_s] [path]",
                  cmd_sound_mic_record_wav, 0, 2),
    SHELL_SUBCMD_SET_END);

static int cmd_sound_agc_status(const struct shell *shell, size_t argc, char **argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    /* Each register step = 0.5 dB; 0x00 = −20 dB, 0x28 = 0 dB, 0x50 = +20 dB. */
    float db = (float)s_agc_gain * 0.5f - 20.0f;
    float peak_norm = (float)s_latest_peak / 32768.0f;
    shell_print(shell, "AGC gain: 0x%02x (%.1f dB)", s_agc_gain, (double)db);
    shell_print(shell, "  Smoothed RMS (1s): %.4f | Instantaneous: %.4f", (double)s_smoothed_rms,
                (double)s_latest_rms);
    shell_print(shell, "  Peak: %d (%.4f norm)", s_latest_peak, (double)peak_norm);
    shell_print(shell, "  Target window: [%.4f, %.4f] | Rate limit: %d frames",
                (double)s_agc_target_low, (double)s_agc_target_high, s_agc_rate_limit);
    return 0;
}

static int cmd_sound_agc_target_low(const struct shell *shell, size_t argc, char **argv) {
    if (argc == 1) {
        shell_print(shell, "AGC target low: %.4f", (double)s_agc_target_low);
        return 0;
    }
    if (argc != 2) {
        shell_error(shell, "Usage: sound agc target-low [<value>]");
        return -EINVAL;
    }
    float val = (float)strtof(argv[1], NULL);
    if (val < 0.001f || val > 0.1f) {
        shell_error(shell, "Value must be in range [0.001, 0.1]");
        return -EINVAL;
    }
    s_agc_target_low = val;
    shell_print(shell, "AGC target low set to %.4f", (double)val);
    return 0;
}

static int cmd_sound_agc_target_high(const struct shell *shell, size_t argc, char **argv) {
    if (argc == 1) {
        shell_print(shell, "AGC target high: %.4f", (double)s_agc_target_high);
        return 0;
    }
    if (argc != 2) {
        shell_error(shell, "Usage: sound agc target-high [<value>]");
        return -EINVAL;
    }
    float val = (float)strtof(argv[1], NULL);
    if (val < 0.001f || val > 0.2f) {
        shell_error(shell, "Value must be in range [0.001, 0.2]");
        return -EINVAL;
    }
    s_agc_target_high = val;
    shell_print(shell, "AGC target high set to %.4f", (double)val);
    return 0;
}

static int cmd_sound_agc_rate(const struct shell *shell, size_t argc, char **argv) {
    if (argc == 1) {
        shell_print(shell, "AGC rate limit: %d frames (~%d ms)", s_agc_rate_limit,
                    s_agc_rate_limit * 32);
        return 0;
    }
    if (argc != 2) {
        shell_error(shell, "Usage: sound agc rate [<frames>]");
        return -EINVAL;
    }
    uint8_t val = (uint8_t)strtoul(argv[1], NULL, 10);
    if (val < 1 || val > 100) {
        shell_error(shell, "Value must be in range [1, 100] frames");
        return -EINVAL;
    }
    s_agc_rate_limit = val;
    shell_print(shell, "AGC rate limit set to %d frames (~%d ms)", val, val * 32);
    return 0;
}

static int cmd_sound_rms(const struct shell *shell, size_t argc, char **argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    float peak_norm = (float)s_latest_peak / 32768.0f;
    shell_print(shell, "Smoothed RMS (1s): %.4f", (double)s_smoothed_rms);
    shell_print(shell, "Instantaneous RMS: %.4f | Peak: %d (%.4f norm)", (double)s_latest_rms,
                s_latest_peak, (double)peak_norm);
    shell_print(shell, "Target window: [%.4f, %.4f]", (double)s_agc_target_low,
                (double)s_agc_target_high);
    return 0;
}

// Subcommands for "sound agc"
// clang-format off
SHELL_STATIC_SUBCMD_SET_CREATE(sub_sound_agc,
                               SHELL_CMD_ARG(target-low, NULL, "Get/set AGC target-low threshold", cmd_sound_agc_target_low, 0, 1),
                               SHELL_CMD_ARG(target-high, NULL, "Get/set AGC target-high threshold", cmd_sound_agc_target_high, 0, 1),
                               SHELL_CMD_ARG(status, NULL, "Show current AGC status", cmd_sound_agc_status, 0, 0),
                               SHELL_CMD_ARG(rate, NULL, "Get/set AGC rate limit (frames)", cmd_sound_agc_rate, 0, 1),
                               SHELL_SUBCMD_SET_END);
// clang-format on

// Subcommands for "sound"
SHELL_STATIC_SUBCMD_SET_CREATE(sub_sound,
#if defined(CONFIG_VM3011)
                               SHELL_CMD(vm, &sub_sound_vm, "VM3011 Commands", NULL),
#endif
                               SHELL_CMD(mic, &sub_sound_mic, "Mic Commands", NULL),
                               SHELL_CMD(agc, &sub_sound_agc, "AGC Commands", NULL),
                               SHELL_CMD(rms, NULL, "Print current RMS level", cmd_sound_rms),
                               SHELL_SUBCMD_SET_END);

/* Creating root (level 0) command "sound" */
SHELL_CMD_REGISTER(sound, &sub_sound, "Sound commands", NULL);