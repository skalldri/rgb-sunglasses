#include <zephyr/ztest.h>

#include <animations/animation_parameter_source.h>
#include <animations/animation_renderer.h>

#define private public
#include <animations/my_eyes_animation.h>
#undef private

#include <cstring>

namespace
{
    class NullTestRenderer : public AnimationRenderer
    {
    public:
        size_t displayWidth() const override { return 40; }
        size_t displayHeight() const override { return 12; }
        void setPixel(size_t x, size_t y, uint8_t r, uint8_t g, uint8_t b) override
        {
            ARG_UNUSED(x);
            ARG_UNUSED(y);
            ARG_UNUSED(r);
            ARG_UNUSED(g);
            ARG_UNUSED(b);
        }
    };

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

    class SequenceUpNextSource : public MyEyesAnimationUpNextSource
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

    class FixedSlotSource : public MyEyesAnimationSlotSource
    {
    public:
        const char *getStringFromSlot(size_t slot) const override
        {
            if (slot == 0)
            {
                return "^^";
            }
            if (slot == 1)
            {
                return "@@";
            }

            return "00";
        }
    };
}

ZTEST_SUITE(my_eyes_animation_di_tests, NULL, NULL, NULL, NULL, NULL);

ZTEST(my_eyes_animation_di_tests, test_init_uses_injected_slot_and_upnext_sources)
{
    ConstUint32Source blinkSpeedMs(10);
    ConstUint32Source color(0xAABBCC);
    FixedSlotSource slotSource;
    SequenceUpNextSource upNextSource;

    MyEyesAnimationDependencies deps(blinkSpeedMs, color, slotSource, upNextSource);

    MyEyesAnimation *animation = MyEyesAnimation::getInstance();
    animation->setDependencies(deps);

    animation->init();
    zassert_true(strcmp(animation->currentEyes, "^^") == 0, "Expected first injected eyes to be ^^");

    animation->init();
    zassert_true(strcmp(animation->currentEyes, "@@") == 0, "Expected second injected eyes to be @@");
}

ZTEST(my_eyes_animation_di_tests, test_init_passes_slot_count_to_upnext_source)
{
    ConstUint32Source blinkSpeedMs(10);
    ConstUint32Source color(0xAABBCC);
    FixedSlotSource slotSource;
    SequenceUpNextSource upNextSource;

    MyEyesAnimationDependencies deps(blinkSpeedMs, color, slotSource, upNextSource);

    MyEyesAnimation *animation = MyEyesAnimation::getInstance();
    animation->setDependencies(deps);

    animation->init();

    zassert_equal(upNextSource.lastNumSlots, 20, "Expected up-next source to receive eye slot count");
}
