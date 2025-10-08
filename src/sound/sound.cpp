#include <zephyr/shell/shell.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#include <zephyr/drivers/vm3011/vm3011.h>
#include <zephyr/audio/dmic.h>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(sound);

const struct device *vm3011 = DEVICE_DT_GET(DT_NODELABEL(vm3011));
const struct device *pdm0 = DEVICE_DT_GET(DT_NODELABEL(pdm0));

// Number of PCM samples the driver will generate in 1s
#define SAMPLE_RATE_HZ 16000

// Sample size. Nordic PDM / DMIC / PCM pipeline only supports 16-bit samples
#define SAMPLE_BIT_WIDTH 16

// We will store the sample as an int16_t
#define BYTES_PER_SAMPLE sizeof(int16_t)

// Milliseconds to wait for a block to be read by the driver
#define READ_TIMEOUT 1000

// How much time (in ms) is captured in each block?
#define BLOCK_CAPTURE_TIME_MS 100
static_assert(MSEC_PER_SEC % BLOCK_CAPTURE_TIME_MS == 0, "Block capture time must cleanly divide into 1s");

// How many audio channels are we capturing? Nordic supports 1 or 2
// We only have 1 mic, so 1
#define NUM_AUDIO_CHANNELS 2

#define BLOCK_SIZE_HELPER(_sample_rate_hz, _number_of_channels, _block_time_ms) \
    (BYTES_PER_SAMPLE * (_sample_rate_hz / (MSEC_PER_SEC / _block_time_ms)) * _number_of_channels)

// Size of a block required to capture the specified amount of time of PCM samples
#define BLOCK_SIZE BLOCK_SIZE_HELPER(SAMPLE_RATE_HZ, NUM_AUDIO_CHANNELS, BLOCK_CAPTURE_TIME_MS)

// Number of blocks available to the driver
// MCU must keep up with the PCM system: reading block contents
// and freeing them so the driver can continue grabing more
#define BLOCK_COUNT DT_PROP(DT_NODELABEL(pdm0), queue_size)

// Alignment of the entries in the memory slab. Must be a power of 2, and the data size
// of the memory slab must also be a multiple of N
#define MEM_SLAB_ALIGNMENT BYTES_PER_SAMPLE
static_assert(BLOCK_SIZE % MEM_SLAB_ALIGNMENT == 0, "Block size must be a multiple of the alignment");

// Define the memory slab that the driver will grab blocks from to fill
K_MEM_SLAB_DEFINE_STATIC(
    mem_slab,
    BLOCK_SIZE,
    BLOCK_COUNT + 1, // Add an extra block to keep the driver happy
    MEM_SLAB_ALIGNMENT);

static struct pcm_stream_cfg stream;
static struct dmic_cfg cfg;

void configure_pdm()
{
    if (!device_is_ready(pdm0))
    {
        LOG_ERR("%s is not ready", pdm0->name);
        return;
    }

    // Information about the PCM stream we want the driver to create
    stream = {
        .pcm_rate = SAMPLE_RATE_HZ,
        .pcm_width = SAMPLE_BIT_WIDTH,
        .block_size = BLOCK_SIZE,
        .mem_slab = &mem_slab,
    };

    cfg = {
        .io = {
            /* These fields can be used to limit the PDM clock
             * configurations that the driver is allowed to use
             * to those supported by the microphone.
             */
            .min_pdm_clk_freq = 1100000, // 1.1Mhz
            .max_pdm_clk_freq = 3500000, // 3.5Mhz
            .min_pdm_clk_dc = 40,
            .max_pdm_clk_dc = 60,
        },
        .streams = &stream,
        .channel = {
            .req_num_streams = 1, // Nordic driver only supports a single PCM stream
        },
    };

    cfg.channel.req_num_chan = NUM_AUDIO_CHANNELS; // Requested number of audio channels
    cfg.channel.req_chan_map_lo = dmic_build_channel_map(
        0,              // Channel number
        0,              // PDM hardware controller number, always 0 on this board
        PDM_CHAN_LEFT); // microphone is configured as a left channel which
                        // means it will emit data on the FALLING edge. We want the PDM circuitry to read data
                        // on the RISING edge of the clock, so we must tell it we are a RIGHT microphone

    cfg.channel.req_chan_map_lo |= dmic_build_channel_map(
        1, // Channel number
        0, // PDM hardware controller number, always 0 on this board
        PDM_CHAN_RIGHT);

    // Calculate the total recording buffer time
    LOG_INF("DMIC Configuration: sample rate: %u hz, sample bit width: %u", SAMPLE_RATE_HZ, SAMPLE_BIT_WIDTH);
    LOG_INF("DMIC Configuration: block size: %u bytes, num blocks: %u", BLOCK_SIZE, BLOCK_COUNT);
    LOG_INF("DMIC Configuration: total recording buffer %u ms", BLOCK_COUNT * BLOCK_CAPTURE_TIME_MS);
}

static int cmd_sound_mic_record(const struct shell *shell,
                                size_t argc, char **argv, void *data)
{
    int ret;

    if (!device_is_ready(pdm0))
    {
        shell_error(shell, "%s is not ready", pdm0->name);
        return -ENOEXEC;
    }

    ret = dmic_configure(pdm0, &cfg);
    if (ret < 0)
    {
        shell_error(shell, "Failed to configure the driver: %d", ret);
        return ret;
    }

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

        k_mem_slab_free(&mem_slab, &buffer);
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

static int cmd_sound_vm_dump(const struct shell *shell,
                             size_t argc, char **argv, void *data)
{
    vm3011_dump(vm3011);
    return 0;
}

static int cmd_sound_vm_clear(const struct shell *shell,
                              size_t argc, char **argv, void *data)
{
    vm3011_clear_dout(vm3011);
    return 0;
}

// Subcommands for "sound vm"
SHELL_STATIC_SUBCMD_SET_CREATE(sub_sound_vm,
                               SHELL_CMD(dump, NULL, "Dump VM3011 Registers to console", cmd_sound_vm_dump),
                               SHELL_CMD(clear, NULL, "Clear VM3011 DOUT pin", cmd_sound_vm_clear),
                               SHELL_SUBCMD_SET_END);

// Subcommands for "sound mic"
SHELL_STATIC_SUBCMD_SET_CREATE(sub_sound_mic,
                               SHELL_CMD(record, NULL, "Record sound to buffer", cmd_sound_mic_record),
                               SHELL_SUBCMD_SET_END);

// Subcommands for "sound"
SHELL_STATIC_SUBCMD_SET_CREATE(sub_sound,
                               SHELL_CMD(vm, &sub_sound_vm, "VM3011 Commands", NULL),
                               SHELL_CMD(mic, &sub_sound_mic, "Mic Commands", NULL),
                               SHELL_SUBCMD_SET_END);

/* Creating root (level 0) command "sound" */
SHELL_CMD_REGISTER(sound, &sub_sound, "Sound commands", NULL);

static int init_microphone(void)
{
    configure_pdm();

    return 0;
}

SYS_INIT(init_microphone, APPLICATION, 2);