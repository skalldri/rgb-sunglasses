#include <animations/text_animation.h>

const char* kMessages[] = {
    "LIFE IS MADE OF LITTLE MOMENTS LIKE THIS",
    "WE ARE ALL WE NEED",
    "SO LONG AND THANKS FOR ALL THE FISH",
    "ANJUNAFAM",
    "BREAKFAST ON MARS",
    "PUSH THE BUTTON",
    "BONSOIR, NE VOUS INQUIETEZ PAS",
    "DREAMS ARE MADE OF NIGHTS LIKE THIS",
    "LIVE FROM THE GORGE AMPHITHEATER, THIS IS ABGT WEEKENDER 2023",
    "ABOVE AND BEYOND",
    "PLEASE WELCOME, ABOVE AND BEYOND",
    "THIS LOVE, KILLS ME, THIS LOVE, KILLS ME",
    "IT'S THE SAME WAY DOWN",
    "GROUP THERAPY",
    "ILAN BLUESTONE",
    "LUTTRELL",
    "YOTTO",
    "TINLICKER",
    "FLOW STATE",
    "THE GORGE",
};

void TextAnimation::init() {
    currentCycleTimeMs = 0;
    currentIndex = 0;
}

void TextAnimation::tick(const LedConfig* config, const size_t timeSinceLastTickMs, const size_t bufferId) {
    currentCycleTimeMs += timeSinceLastTickMs;

    if (currentCycleTimeMs > stepTime) {
        currentCycleTimeMs = 0;
        currentIndex++;
    }

    if (currentIndex >= (config->displayWidth * config->displayHeight)) {
        currentIndex = 0;
    }

    // Turn off all LEDs
    for (size_t x = 0; x < config->displayWidth; x++) {
        for (size_t y = 0; y < config->displayHeight; y++) {
            set_pixel_in_framebuffer(config, x, y, bufferId, 0, 0, 0);
        }
    }

    size_t y = currentIndex / config->displayWidth;
    size_t x = currentIndex % config->displayWidth;

    // Turn on just one
    set_pixel_in_framebuffer(config, x, y, bufferId, 50, 50, 50);
}