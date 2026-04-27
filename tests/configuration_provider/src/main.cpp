#include <zephyr/ztest.h>
#include <configuration_provider.h>

// ---------------------------------------------------------------------------
// Configurable test double
// ---------------------------------------------------------------------------

class FakeConfigurationProvider : public ConfigurationProvider
{
public:
    float brightness = 1.0f;
    float displayRateMs = 33.0f;
    float renderRateMs = 11.0f;

    float getBrightnessFactor() override { return brightness; }
    float getDisplayRateMs() override    { return displayRateMs; }
    float getRenderRateMs() override     { return renderRateMs; }
};

// ---------------------------------------------------------------------------
// Tests — exercise the interface contract directly
// (pattern_controller and led_controller use this interface to read config)
// ---------------------------------------------------------------------------

ZTEST_SUITE(configuration_provider_tests, NULL, NULL, NULL, NULL, NULL);

ZTEST(configuration_provider_tests, test_fake_returns_injected_brightness)
{
    FakeConfigurationProvider fake;
    fake.brightness = 0.5f;
    ConfigurationProvider &provider = fake;

    zassert_within(provider.getBrightnessFactor(), 0.5f, 0.001f,
                   "Expected injected brightness factor");
}

ZTEST(configuration_provider_tests, test_fake_returns_injected_display_rate)
{
    FakeConfigurationProvider fake;
    fake.displayRateMs = 16.67f;
    ConfigurationProvider &provider = fake;

    zassert_within(provider.getDisplayRateMs(), 16.67f, 0.01f,
                   "Expected injected display rate");
}

ZTEST(configuration_provider_tests, test_fake_returns_injected_render_rate)
{
    FakeConfigurationProvider fake;
    fake.renderRateMs = 8.0f;
    ConfigurationProvider &provider = fake;

    zassert_within(provider.getRenderRateMs(), 8.0f, 0.001f,
                   "Expected injected render rate");
}

ZTEST(configuration_provider_tests, test_interface_allows_zero_brightness)
{
    FakeConfigurationProvider fake;
    fake.brightness = 0.0f;
    ConfigurationProvider &provider = fake;

    zassert_within(provider.getBrightnessFactor(), 0.0f, 0.001f,
                   "Expected brightness of zero to be expressible");
}

ZTEST(configuration_provider_tests, test_interface_allows_full_brightness)
{
    FakeConfigurationProvider fake;
    fake.brightness = 1.0f;
    ConfigurationProvider &provider = fake;

    zassert_within(provider.getBrightnessFactor(), 1.0f, 0.001f,
                   "Expected brightness of one to be expressible");
}
