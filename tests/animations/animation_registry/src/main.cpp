#include <zephyr/ztest.h>

#include <animations/animation_registry.h>

namespace
{
    class FakeAnimation : public BaseAnimation
    {
    public:
        void init() override
        {
            initCount++;
        }

        void tick(const LedConfig *config, const size_t timeSinceLastTickMs, const size_t bufferId) override
        {
            ARG_UNUSED(config);
            ARG_UNUSED(timeSinceLastTickMs);
            ARG_UNUSED(bufferId);
        }

        void setActive(bool active) override
        {
            ARG_UNUSED(active);
        }

        size_t initCount = 0;
    };

    FakeAnimation sFirstAnimation;
    FakeAnimation sSecondAnimation;

    BaseAnimation *first_factory()
    {
        return &sFirstAnimation;
    }

    BaseAnimation *second_factory()
    {
        return &sSecondAnimation;
    }

    void reset_test_state(void)
    {
        animation_registry_reset();
        sFirstAnimation.initCount = 0;
        sSecondAnimation.initCount = 0;
    }
}

ZTEST_SUITE(animation_registry_tests, NULL, NULL, NULL, NULL, NULL);

ZTEST(animation_registry_tests, test_lookup_unregistered_returns_null)
{
    reset_test_state();
    BaseAnimation *animation = animation_registry_get(Animation::Text);
    zassert_is_null(animation, "Expected null for unregistered animation");
}

ZTEST(animation_registry_tests, test_register_and_lookup_animation)
{
    reset_test_state();
    int ret = animation_registry_register(Animation::Text, first_factory);
    zassert_equal(ret, 0, "Failed to register animation: %d", ret);

    BaseAnimation *animation = animation_registry_get(Animation::Text);
    zassert_equal_ptr(animation, &sFirstAnimation, "Lookup returned unexpected animation pointer");
    zassert_equal(animation_registry_count(), 1, "Expected a single registry entry");
}

ZTEST(animation_registry_tests, test_register_replaces_existing_factory)
{
    reset_test_state();
    int ret = animation_registry_register(Animation::Text, first_factory);
    zassert_equal(ret, 0, "Failed to register first animation: %d", ret);

    ret = animation_registry_register(Animation::Text, second_factory);
    zassert_equal(ret, 0, "Failed to replace animation factory: %d", ret);

    BaseAnimation *animation = animation_registry_get(Animation::Text);
    zassert_equal_ptr(animation, &sSecondAnimation, "Expected lookup to use replacement factory");
    zassert_equal(animation_registry_count(), 1, "Replacing factory should not change entry count");
}

ZTEST(animation_registry_tests, test_init_registered_calls_init_for_each_entry)
{
    reset_test_state();
    int ret = animation_registry_register(Animation::Text, first_factory);
    zassert_equal(ret, 0, "Failed to register first animation: %d", ret);

    ret = animation_registry_register(Animation::Rainbow, second_factory);
    zassert_equal(ret, 0, "Failed to register second animation: %d", ret);

    animation_registry_init_registered();

    zassert_equal(sFirstAnimation.initCount, 1, "Expected first animation init to be called once");
    zassert_equal(sSecondAnimation.initCount, 1, "Expected second animation init to be called once");
}
