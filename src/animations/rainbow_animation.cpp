#include <animations/rainbow_animation.h>
#include <animations/animation_is_active_binding.h>
#include <animations/animation_is_active_characteristic.h>

#include <bluetooth/bt_service_cpp.h>

#include <zephyr/drivers/led_strip.h>

#include <cstddef>

constexpr bt_uuid_128 kRainbowConfigServiceUuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x0400, 0x56789abd0000));

BtGattPrimaryService<kRainbowConfigServiceUuid> rainbowPrimaryService;
BtGattAutoReadWriteCharacteristic<"Step Time Ms", uint32_t, 100> rainbowStepTimeMs;
BtGattAutoReadWriteCharacteristic<"Rainbow Width Pixels", uint32_t, 5> rainbowWidthPix;

using RainbowIsActiveCharacteristic = IsActiveCharacteristic<Animation::Rainbow>;
RainbowIsActiveCharacteristic rainbowIsActive;

BtGattServer rainbowConfigServer(
    rainbowPrimaryService,
    rainbowStepTimeMs,
    rainbowWidthPix,
    rainbowIsActive);
BT_GATT_SERVER_REGISTER(rainbowConfigServerStatic, rainbowConfigServer);

namespace
{
    class RainbowStepTimeSource : public AnimationUint32ParameterSource
    {
    public:
        uint32_t get() const override
        {
            return rainbowStepTimeMs;
        }
    };

    class RainbowWidthSource : public AnimationUint32ParameterSource
    {
    public:
        uint32_t get() const override
        {
            return rainbowWidthPix;
        }
    };

    RainbowStepTimeSource sDefaultStepTimeSource;
    RainbowWidthSource sDefaultWidthSource;
    RainbowAnimationDependencies sDefaultDeps(sDefaultStepTimeSource, sDefaultWidthSource);
}

// All services implement the "IsActive" service, so declare relevant BT GATT glue logic
using RainbowAnimationIsActive = AnimationIsActiveBinding<Animation::Rainbow>;

static void rainbow_set_is_active(bool active)
{
    rainbowIsActive.setActive(active);
}

struct RainbowIsActiveBindingRegistrar
{
    RainbowIsActiveBindingRegistrar()
    {
        RainbowAnimationIsActive::registerSetter(rainbow_set_is_active);
    }
};

[[maybe_unused]] RainbowIsActiveBindingRegistrar sRainbowIsActiveBindingRegistrar;

void rainbow_animation_bind_default_dependencies()
{
    RainbowAnimation::getInstance()->setDependencies(sDefaultDeps);
}

void RainbowAnimation::setDependencies(const RainbowAnimationDependencies &deps)
{
    deps_ = &deps;
}

#if defined(CONFIG_LED_STRIP_RGB_SCRATCH)
#define LED_RGB(r, g, b) {0, r, g, b}
#else
#define LED_RGB(r, g, b) {r, g, b}
#endif

// Rainbow colors: ROYGBIV
// NOTE: they have a scratch value!
const struct led_rgb rainbowColors[] = {
    LED_RGB(255, 0, 0),   // Red
    LED_RGB(255, 165, 0), // Orange
    LED_RGB(255, 255, 0), // Yellow
    LED_RGB(0, 255, 0),   // Green
    LED_RGB(0, 0, 255),   // Blue
    LED_RGB(75, 0, 255),  // Indigo-ish
    LED_RGB(143, 0, 200)  // Violet-ish
};

static constexpr size_t numRainbowColors = ARRAY_SIZE(rainbowColors);

float rainbowBrightness = 0.05f;

void RainbowAnimation::init()
{
    currentCycleTimeMs = 0;
    currentRainbowStep = 0;
}

void RainbowAnimation::tick(const LedConfig *config, const size_t timeSinceLastTickMs, const size_t bufferId)
{
    if (!deps_)
    {
        setDependencies(sDefaultDeps);
    }

    // Read BT variables
    const uint32_t rainbowColorWidth = deps_->rainbowWidthPix.get();

    // Turn off all LEDs
    for (size_t x = 0; x < config->displayWidth; x++)
    {
        size_t currentRainbowColor = ((currentRainbowStep + x) / rainbowColorWidth) % numRainbowColors;
        size_t nextRainbowColor = (currentRainbowColor + 1) % numRainbowColors;

        // Figure out the blend percentage
        // First: how far are we through the current color, in rainbow steps
        float blendPercent = ((currentRainbowStep + x) % rainbowColorWidth);

        // How far is that as a percentage?
        blendPercent /= (float)rainbowColorWidth;

        float red = ((1.0f - blendPercent) * ((float)rainbowColors[currentRainbowColor].r)) + (blendPercent * ((float)rainbowColors[nextRainbowColor].r));
        float green = ((1.0f - blendPercent) * ((float)rainbowColors[currentRainbowColor].g)) + (blendPercent * ((float)rainbowColors[nextRainbowColor].g));
        float blue = ((1.0f - blendPercent) * ((float)rainbowColors[currentRainbowColor].b)) + (blendPercent * ((float)rainbowColors[nextRainbowColor].b));

        for (size_t y = 0; y < config->displayHeight; y++)
        {
            pattern_controller_set_pixel_in_framebuffer(config, x, y, bufferId, red, green, blue);
        }
    }

    // Add the time to our counter
    currentCycleTimeMs += timeSinceLastTickMs;

    if (currentCycleTimeMs > deps_->stepTimeMs.get())
    {
        currentCycleTimeMs = 0;
        currentRainbowStep++; // Move text one pixel to the left
    }
}