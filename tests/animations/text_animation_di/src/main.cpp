#include <zephyr/ztest.h>

#include <animations/animation_parameter_source.h>

#define private public
#include <animations/text_animation.h>
#undef private

#include <cstring>

namespace
{
    class ConstUint32Source : public AnimationUint32ParameterSource
    {
    public:
        explicit ConstUint32Source(uint32_t value)
            : value_(value)
        {
        }

        uint32_t get() const override
        {
            return value_;
        }

    private:
        uint32_t value_;
    };

    class SequenceUpNextSource : public TextAnimationUpNextSource
    {
    public:
        size_t consumeCurrentAndAdvance(size_t numSlots) override
        {
            lastNumSlots = numSlots;
            size_t value = sequence[index % 2];
            index++;
            return value;
        }

        size_t sequence[2] = {0, 1};
        size_t index = 0;
        size_t lastNumSlots = 0;
    };

    class FixedSlotSource : public TextAnimationSlotSource
    {
    public:
        const char *getStringFromSlot(size_t slot) const override
        {
            if (slot == 0)
            {
                return "HELLO";
            }
            if (slot == 1)
            {
                return "WORLD";
            }

            return "UNKNOWN";
        }
    };
}

int pattern_controller_set_pixel_in_framebuffer(const LedConfig *config, size_t x, size_t y, size_t bufferId, uint8_t red, uint8_t green, uint8_t blue)
{
    ARG_UNUSED(config);
    ARG_UNUSED(x);
    ARG_UNUSED(y);
    ARG_UNUSED(bufferId);
    ARG_UNUSED(red);
    ARG_UNUSED(green);
    ARG_UNUSED(blue);
    return 0;
}

int pattern_controller_change_to_animation(Animation animation)
{
    ARG_UNUSED(animation);
    return 0;
}

ZTEST_SUITE(text_animation_di_tests, NULL, NULL, NULL, NULL, NULL);

ZTEST(text_animation_di_tests, test_init_uses_injected_slot_and_upnext_sources)
{
    ConstUint32Source stepTimeMs(10);
    ConstUint32Source color(0xAABBCC);
    FixedSlotSource slotSource;
    SequenceUpNextSource upNextSource;

    TextAnimationDependencies deps(stepTimeMs, color, slotSource, upNextSource);

    TextAnimation *animation = TextAnimation::getInstance();
    animation->setDependencies(deps);

    animation->init();
    zassert_true(strcmp(animation->currentMessage, "HELLO") == 0, "Expected first injected message to be HELLO");

    animation->init();
    zassert_true(strcmp(animation->currentMessage, "WORLD") == 0, "Expected second injected message to be WORLD");
}

ZTEST(text_animation_di_tests, test_init_passes_slot_count_to_upnext_source)
{
    ConstUint32Source stepTimeMs(10);
    ConstUint32Source color(0xAABBCC);
    FixedSlotSource slotSource;
    SequenceUpNextSource upNextSource;

    TextAnimationDependencies deps(stepTimeMs, color, slotSource, upNextSource);

    TextAnimation *animation = TextAnimation::getInstance();
    animation->setDependencies(deps);

    animation->init();

    zassert_equal(upNextSource.lastNumSlots, 20, "Expected up-next source to receive text slot count");
}
