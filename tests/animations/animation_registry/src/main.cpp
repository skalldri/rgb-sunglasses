#include <zephyr/ztest.h>

#include <animations/animation_registry.h>

#include <errno.h>

namespace
{
    class FakeAnimation : public BaseAnimation
    {
    public:
        void init() override
        {
            initCount++;
        }

        void tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs) override
        {
            ARG_UNUSED(renderer);
            ARG_UNUSED(timeSinceLastTickMs);
        }

        void setActive(bool active) override
        {
            ARG_UNUSED(active);
        }

        size_t initCount = 0;
    };

    FakeAnimation sFirstAnimation;
    FakeAnimation sSecondAnimation;
    bool sLastActiveState = false;
    size_t sSetActiveCallCount = 0;

    BaseAnimation *first_factory()
    {
        return &sFirstAnimation;
    }

    BaseAnimation *second_factory()
    {
        return &sSecondAnimation;
    }

    void record_active_state(bool active)
    {
        sLastActiveState = active;
        sSetActiveCallCount++;
    }

    void reset_test_state(void)
    {
        animation_registry_reset();
        sFirstAnimation.initCount = 0;
        sSecondAnimation.initCount = 0;
        sLastActiveState = false;
        sSetActiveCallCount = 0;
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

ZTEST(animation_registry_tests, test_register_is_active_and_dispatch)
{
    reset_test_state();
    int ret = animation_registry_register(Animation::Text, first_factory);
    zassert_equal(ret, 0, "Failed to register animation: %d", ret);

    ret = animation_registry_register_is_active(Animation::Text, record_active_state);
    zassert_equal(ret, 0, "Failed to register IsActive callback: %d", ret);

    animation_registry_set_is_active(Animation::Text, true);

    zassert_equal(sSetActiveCallCount, 1, "Expected one IsActive dispatch");
    zassert_true(sLastActiveState, "Expected active state to be true");
}

ZTEST(animation_registry_tests, test_register_is_active_requires_animation_registration)
{
    reset_test_state();
    int ret = animation_registry_register_is_active(Animation::Text, record_active_state);
    zassert_equal(ret, -ENOENT, "Expected -ENOENT for unregistered animation, got %d", ret);
}

ZTEST(animation_registry_tests, test_reset_after_registration_clears_entries)
{
    reset_test_state();
    animation_registry_register(Animation::Text, first_factory);
    animation_registry_register(Animation::Rainbow, second_factory);
    zassert_equal(animation_registry_count(), 2, "Expected 2 entries before reset");

    animation_registry_reset();

    zassert_equal(animation_registry_count(), 0, "Expected 0 entries after reset");
    zassert_is_null(animation_registry_get(Animation::Text), "Expected null lookup after reset");
}

ZTEST(animation_registry_tests, test_init_registered_before_any_registration_is_safe)
{
    reset_test_state();

    // Should not crash with zero registrations
    animation_registry_init_registered();
}
