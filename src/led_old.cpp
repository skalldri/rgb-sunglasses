void led_strip_control_thread(void* a, void* b, void* c);
void led_strip_1_control_thread(void* a, void* b, void* c);

/*
K_THREAD_DEFINE(
    led_strip_control, 
    8096,
    led_strip_control_thread,
    NULL,
    NULL,
    NULL,
    6,
    0,
    0
);

K_THREAD_DEFINE(
    led_strip_1_control, 
    8096,
    led_strip_1_control_thread,
    NULL,
    NULL,
    NULL,
    6,
    0,
    0
);
*/

// TODO: get from device tree
#define NUM_PIXELS 8

enum strip_state {
    BREATHING,
    RAINBOW,

    STRIP_STATE_END,
};

#define MIN_BREATH_LEVEL 10
#define MAX_BREATH_LEVEL 255
#define BREATH_TIME_MS 1000
#define BREATH_STEP ((MAX_BREATH_LEVEL - MIN_BREATH_LEVEL) / (BREATH_TIME_MS / FADE_DELAY_MS))

#define MAX_BRIGHTNESS	100

#define FADE_DELAY_MS	10
#define FADE_DELAY	K_MSEC(FADE_DELAY_MS)

void _set_all_leds(uint8_t red, uint8_t green, uint8_t blue, struct led_rgb* leds, size_t numLeds) {
    for (size_t i = 0; i < numLeds; i++) {
        leds[i].r = red;
        leds[i].g = green;
        leds[i].b = blue;
    }
}

// Rainbow colors: ROYGBIV
// NOTE: they have a scratch value!
const struct led_rgb rainbow_colors[] = {
    {0, 255, 0, 0},    // Red
    {0, 255, 165, 0},  // Orange
    {0, 255, 255, 0},  // Yellow
    {0, 0, 255, 0},    // Green
    {0, 0, 0, 255},    // Blue
    {0, 75, 0, 255},   // Indigo-ish
    {0, 143, 0, 200}   // Violet-ish
};

float rainbowBrightness = 0.05f;

void control_strip(const struct device *led_strip) {
    int32_t breath_level = MIN_BREATH_LEVEL;
    bool breath_up = true;

    size_t current_rainbow_step = 0;
    const size_t rainbow_steps_per_color = (BREATH_TIME_MS / FADE_DELAY_MS);

    
    static struct led_rgb pixel_data[NUM_PIXELS];

    static strip_state curr_state = RAINBOW;
    
    // - We will execute rainbow for (BREATH_TIME_MS * ARRAY_SIZE(rainbow_colors))
    // - Total steps = (BREATH_TIME_MS * ARRAY_SIZE(rainbow_colors)) / FADE_DELAY_MS
    // - Current color = (current_step / ARRAY_SIZE(rainbow_colors))
    // - Next color = ((current_step / ARRAY_SIZE(rainbow_colors)) + 1) % ARRAY_SIZE(rainbow_colors)

    while (true) {
        switch (curr_state) {
            case BREATHING:
                _set_all_leds(breath_level, 0, 0, pixel_data, NUM_PIXELS);

                if (breath_up) {
                    breath_level += BREATH_STEP;
                } else {
                    breath_level -= BREATH_STEP;
                }

                if (breath_level > MAX_BREATH_LEVEL) {
                    breath_level = MAX_BREATH_LEVEL;
                    breath_up = false;
                } else if (breath_level < MIN_BREATH_LEVEL) {
                    breath_level = MIN_BREATH_LEVEL;
                    breath_up = true;
                }
                break;
            case RAINBOW:
                for (size_t i = 0; i < NUM_PIXELS; i++) {
                    size_t current_rainbow_color = ((current_rainbow_step / rainbow_steps_per_color) + i) % ARRAY_SIZE(rainbow_colors);
                    size_t next_rainbow_color = (current_rainbow_color + 1) % ARRAY_SIZE(rainbow_colors);

                    // Figure out the blend percentage
                    // First: how far are we through the current color, in rainbow steps
                    float blendPercent = (current_rainbow_step % rainbow_steps_per_color);

                    // How far is that as a percentage?
                    blendPercent /= (float)rainbow_steps_per_color;

                    float red = ((1.0f - blendPercent) * ((float)rainbow_colors[current_rainbow_color].r)) + (blendPercent * ((float)rainbow_colors[next_rainbow_color].r));
                    float green = ((1.0f - blendPercent) * ((float)rainbow_colors[current_rainbow_color].g)) + (blendPercent * ((float)rainbow_colors[next_rainbow_color].g));
                    float blue = ((1.0f - blendPercent) * ((float)rainbow_colors[current_rainbow_color].b)) + (blendPercent * ((float)rainbow_colors[next_rainbow_color].b));

                    red *= rainbowBrightness;
                    green *= rainbowBrightness;
                    blue *= rainbowBrightness;

                    pixel_data[i].r = std::min(red, 255.0f);
                    pixel_data[i].g = std::min(green, 255.0f);
                    pixel_data[i].b = std::min(blue, 255.0f);
                }

                current_rainbow_step++;
                break;

            case STRIP_STATE_END:
                curr_state = BREATHING; // Reset to breathing by default
        }

        led_strip_update_rgb(led_strip, pixel_data, NUM_PIXELS);
        k_sleep(FADE_DELAY);
    }
}

void led_strip_control_thread(void* a, void* b, void* c) {
    const struct device *led_strip = DEVICE_DT_GET(LED_STRIP_0_NODE_ID);
    if (!device_is_ready(led_strip)) {
        printk("Device %s is not ready\n", led_strip->name);
        return;
    }

    control_strip(led_strip);

    return;
}

void led_strip_1_control_thread(void* a, void* b, void* c) {
    const struct device *led_strip = DEVICE_DT_GET(LED_STRIP_1_NODE_ID);
    if (!device_is_ready(led_strip)) {
        printk("Device %s is not ready\n", led_strip->name);
        return;
    }

    control_strip(led_strip);

    return;
}