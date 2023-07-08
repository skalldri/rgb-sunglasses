#include <zephyr/shell/shell.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#include <zephyr/drivers/vm3011/vm3011.h>
#include <zephyr/audio/dmic.h>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(sound);

const struct device *vm3011 = DEVICE_DT_GET(DT_NODELABEL(vm3011));
const struct device *pdm0 = DEVICE_DT_GET(DT_NODELABEL(pdm0));

#define MAX_SAMPLE_RATE  16000
#define SAMPLE_BIT_WIDTH 16
#define BYTES_PER_SAMPLE sizeof(int16_t)

/* Milliseconds to wait for a block to be read. */
#define READ_TIMEOUT     1000

/* Size of a block for 100 ms of audio data. */
#define BLOCK_SIZE(_sample_rate, _number_of_channels) \
	(BYTES_PER_SAMPLE * (_sample_rate / 10) * _number_of_channels)

/* Driver will allocate blocks from this slab to receive audio data into them.
 * Application, after getting a given block from the driver and processing its
 * data, needs to free that block.
 */
#define MAX_BLOCK_SIZE   BLOCK_SIZE(MAX_SAMPLE_RATE, 2)
#define BLOCK_COUNT      4
K_MEM_SLAB_DEFINE_STATIC(
    mem_slab, 
    MAX_BLOCK_SIZE, 
    BLOCK_COUNT, 
    4 /* alignment */);

void configure_pdm() {
    if (!device_is_ready(pdm0)) {
		LOG_ERR("%s is not ready", pdm0->name);
		return;
	}

    struct pcm_stream_cfg stream = {
		.pcm_width = SAMPLE_BIT_WIDTH,
		.mem_slab  = &mem_slab,
	};

    struct dmic_cfg cfg = {
		.io = {
			/* These fields can be used to limit the PDM clock
			 * configurations that the driver is allowed to use
			 * to those supported by the microphone.
			 */
			.min_pdm_clk_freq = 1000000,
			.max_pdm_clk_freq = 3500000,
			.min_pdm_clk_dc   = 10,
			.max_pdm_clk_dc   = 90,
		},
		.streams = &stream,
		.channel = {
			.req_num_streams = 1,
		},
	};

    cfg.channel.req_num_chan = 1;
	cfg.channel.req_chan_map_lo =
		dmic_build_channel_map(0, 0, PDM_CHAN_LEFT);
	cfg.streams[0].pcm_rate = MAX_SAMPLE_RATE;
	cfg.streams[0].block_size =
		BLOCK_SIZE(cfg.streams[0].pcm_rate, cfg.channel.req_num_chan);
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

SHELL_STATIC_SUBCMD_SET_CREATE(sub_sound_vm,
                               SHELL_CMD(dump, NULL, "Dump VM3011 Registers to console", cmd_sound_vm_dump),
                               SHELL_CMD(clear, NULL, "Clera VM3011 DOUT pin", cmd_sound_vm_clear),
                               SHELL_SUBCMD_SET_END);

// Subcommands for "sound"
SHELL_STATIC_SUBCMD_SET_CREATE(sub_sound,
                               SHELL_CMD(vm, &sub_sound_vm, "VM3011 Commands", NULL),
                               SHELL_SUBCMD_SET_END);

/* Creating root (level 0) command "sound" */
SHELL_CMD_REGISTER(sound, &sub_sound, "Sound commands", NULL);