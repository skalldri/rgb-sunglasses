#include <status_led/status_led.h>
#include <status_led/status_led_math.h>

#include <core_config.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(status_led, LOG_LEVEL_INF);

#if DT_HAS_ALIAS(led_strip_2)

#include <zephyr/device.h>

#define LED_STRIP_2_NODE DT_ALIAS(led_strip_2)
#define NUM_STATUS_LEDS  DT_PROP(LED_STRIP_2_NODE, chain_length)

// Thread update interval — 20 ms gives 50 Hz, smooth enough for breathing.
#define STATUS_LED_UPDATE_INTERVAL_MS 20

// Blinking: 500 ms on, 500 ms off (1 Hz).
#define BLINK_HALF_PERIOD_TICKS (500 / STATUS_LED_UPDATE_INTERVAL_MS)

// Breathing: full sine cycle over 2000 ms (0.5 Hz, slow).
#define BREATHE_PERIOD_TICKS      (2000 / STATUS_LED_UPDATE_INTERVAL_MS)
// Fast breathing: full sine cycle over 1000 ms (1 Hz, fast).
#define FAST_BREATHE_PERIOD_TICKS (1000 / STATUS_LED_UPDATE_INTERVAL_MS)

struct StatusLedState {
    StatusIndication indication;
    StatusColor      color;
};

static StatusLedState led_state[NUM_STATUS_LEDS] = {};
static struct led_rgb led_buf[NUM_STATUS_LEDS]   = {};

K_MUTEX_DEFINE(status_led_mutex);

static void status_led_thread_func(void *, void *, void *);

K_THREAD_DEFINE(status_led_thread, 2048, status_led_thread_func, NULL, NULL, NULL, 7, 0, 0);

static void status_led_thread_func(void *, void *, void *) {
    const struct device *strip = DEVICE_DT_GET(LED_STRIP_2_NODE);

    if (!device_is_ready(strip)) {
        LOG_ERR("led_strip_2 not ready");
        return;
    }

    uint32_t tick = 0;

    while (true) {
        float brightnessScale = CoreConfig::getInstance().getStatusLedBrightnessFactor();

        k_mutex_lock(&status_led_mutex, K_FOREVER);

        for (size_t i = 0; i < NUM_STATUS_LEDS; i++) {
            struct led_rgb base = status_led_color_to_rgb(led_state[i].color);
            uint8_t brightness  = 0;

            switch (led_state[i].indication) {
                case StatusIndication::Off:
                    brightness = 0;
                    break;

                case StatusIndication::Solid:
                    brightness = 255;
                    break;

                case StatusIndication::Blinking:
                    brightness = status_led_blinking_brightness(tick, BLINK_HALF_PERIOD_TICKS);
                    break;

                case StatusIndication::Breathing:
                    brightness = status_led_breathing_brightness(tick, BREATHE_PERIOD_TICKS);
                    break;

                case StatusIndication::FastBreathing:
                    brightness = status_led_breathing_brightness(tick, FAST_BREATHE_PERIOD_TICKS);
                    break;
            }

            // Apply the global status LED brightness factor from CoreConfig.
            uint8_t scaled = static_cast<uint8_t>(brightness * brightnessScale);
            led_buf[i]     = status_led_scale_brightness(base, scaled);
        }

        k_mutex_unlock(&status_led_mutex);

        led_strip_update_rgb(strip, led_buf, NUM_STATUS_LEDS);

        tick++;
        k_msleep(STATUS_LED_UPDATE_INTERVAL_MS);
    }
}

void status_led_set(size_t led_index, StatusIndication indication, StatusColor color) {
    if (led_index >= NUM_STATUS_LEDS) {
        LOG_ERR("Invalid LED index %zu (max %d)", led_index, NUM_STATUS_LEDS - 1);
        return;
    }
    k_mutex_lock(&status_led_mutex, K_FOREVER);
    led_state[led_index].indication = indication;
    led_state[led_index].color      = color;
    k_mutex_unlock(&status_led_mutex);
}

#else  /* !DT_HAS_ALIAS(led_strip_2) */

void status_led_set(size_t led_index, StatusIndication indication, StatusColor color) {
    ARG_UNUSED(led_index);
    ARG_UNUSED(indication);
    ARG_UNUSED(color);
}

#endif /* DT_HAS_ALIAS(led_strip_2) */

/* --------------------------------------------------------------------------
 * Shell commands
 * -------------------------------------------------------------------------- */
#if defined(CONFIG_SHELL) && DT_HAS_ALIAS(led_strip_2)

#include <zephyr/shell/shell.h>

static int cmd_status_led_set(const struct shell *shell, size_t argc, char **argv) {
    if (argc != 4) {
        shell_error(shell,
                    "Usage: status_led set <led_id> "
                    "<off|solid|blink|breathe> "
                    "<red|orange|yellow|green|blue|indigo|violet>");
        return -EINVAL;
    }

    size_t led_id = static_cast<size_t>(atoi(argv[1]));

    StatusIndication indication;
    if (strcmp(argv[2], "off") == 0) {
        indication = StatusIndication::Off;
    } else if (strcmp(argv[2], "solid") == 0) {
        indication = StatusIndication::Solid;
    } else if (strcmp(argv[2], "blink") == 0) {
        indication = StatusIndication::Blinking;
    } else if (strcmp(argv[2], "breathe") == 0) {
        indication = StatusIndication::Breathing;
    } else if (strcmp(argv[2], "fast_breathe") == 0) {
        indication = StatusIndication::FastBreathing;
    } else {
        shell_error(shell, "Unknown indication '%s' (use: off, solid, blink, breathe, fast_breathe)",
                    argv[2]);
        return -EINVAL;
    }

    StatusColor color;
    if (strcmp(argv[3], "red") == 0) {
        color = StatusColor::Red;
    } else if (strcmp(argv[3], "orange") == 0) {
        color = StatusColor::Orange;
    } else if (strcmp(argv[3], "yellow") == 0) {
        color = StatusColor::Yellow;
    } else if (strcmp(argv[3], "green") == 0) {
        color = StatusColor::Green;
    } else if (strcmp(argv[3], "blue") == 0) {
        color = StatusColor::Blue;
    } else if (strcmp(argv[3], "indigo") == 0) {
        color = StatusColor::Indigo;
    } else if (strcmp(argv[3], "violet") == 0) {
        color = StatusColor::Violet;
    } else {
        shell_error(shell,
                    "Unknown color '%s' "
                    "(use: red, orange, yellow, green, blue, indigo, violet)",
                    argv[3]);
        return -EINVAL;
    }

    status_led_set(led_id, indication, color);
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_status_led,
    SHELL_CMD_ARG(set, NULL,
                  "Set LED status: <led_id> <off|solid|blink|breathe|fast_breathe> "
                  "<red|orange|yellow|green|blue|indigo|violet>",
                  cmd_status_led_set, 4, 0),
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(status_led, &sub_status_led, "Onboard status LED commands", NULL);

#endif /* CONFIG_SHELL && DT_HAS_ALIAS(led_strip_2) */
