#include <zephyr/ztest.h>

#include <animations/animation_is_active_binding.h>
#include <animations/animation_activator.h>

// Use a dedicated animation ID for binding tests to avoid cross-test contamination
static constexpr Animation kTestAnimation = Animation::ZigZag;
using TestBinding = AnimationIsActiveBinding<kTestAnimation>;

namespace
{
    struct ActivatorCall
    {
        Animation lastId = Animation::None;
        size_t callCount = 0;
    };

    class RecordingActivator : public AnimationActivator
    {
    public:
        void changeToAnimation(Animation id) override
        {
            last.lastId = id;
            last.callCount++;
        }

        ActivatorCall last;
    };

    bool sSetterLastValue = false;
    size_t sSetterCallCount = 0;

    void recording_setter(bool active)
    {
        sSetterLastValue = active;
        sSetterCallCount++;
    }

    void reset_binding()
    {
        TestBinding::registerActivator(nullptr);
        TestBinding::registerSetter(nullptr);
        sSetterLastValue = false;
        sSetterCallCount = 0;
    }
}

ZTEST_SUITE(animation_is_active_binding_tests, NULL, NULL, NULL, NULL, NULL);

ZTEST(animation_is_active_binding_tests, test_on_remote_active_true_calls_activator)
{
    reset_binding();
    RecordingActivator activator;
    TestBinding::registerActivator(&activator);

    TestBinding::onRemoteActiveChange(true);

    zassert_equal(activator.last.callCount, 1, "Expected activator called exactly once");
    zassert_equal(activator.last.lastId, kTestAnimation, "Expected correct animation ID");
}

ZTEST(animation_is_active_binding_tests, test_on_remote_active_false_does_not_call_activator)
{
    reset_binding();
    RecordingActivator activator;
    TestBinding::registerActivator(&activator);

    TestBinding::onRemoteActiveChange(false);

    zassert_equal(activator.last.callCount, 0, "Expected activator NOT called when active==false");
}

ZTEST(animation_is_active_binding_tests, test_on_remote_active_without_activator_is_safe)
{
    reset_binding();

    // Should not crash
    TestBinding::onRemoteActiveChange(true);
}

ZTEST(animation_is_active_binding_tests, test_set_local_active_true_calls_setter)
{
    reset_binding();
    TestBinding::registerSetter(recording_setter);

    TestBinding::setLocalActiveState(true);

    zassert_equal(sSetterCallCount, 1, "Expected setter called exactly once");
    zassert_true(sSetterLastValue, "Expected setter received true");
}

ZTEST(animation_is_active_binding_tests, test_set_local_active_false_calls_setter)
{
    reset_binding();
    TestBinding::registerSetter(recording_setter);

    TestBinding::setLocalActiveState(false);

    zassert_equal(sSetterCallCount, 1, "Expected setter called exactly once");
    zassert_false(sSetterLastValue, "Expected setter received false");
}

ZTEST(animation_is_active_binding_tests, test_set_local_active_without_setter_is_safe)
{
    reset_binding();

    // Should not crash
    TestBinding::setLocalActiveState(true);
}
