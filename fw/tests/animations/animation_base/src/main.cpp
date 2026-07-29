#include <animations/animation.h>
#include <animations/animation_active_state_observer.h>
#include <zephyr/ztest.h>

namespace {
struct ObservedCall {
    Animation id = Animation::None;
    bool active = false;
    size_t callCount = 0;
};

class RecordingObserver : public AnimationActiveStateObserver {
   public:
    void onActiveStateChanged(Animation id, bool active) override {
        last.id = id;
        last.active = active;
        last.callCount++;
    }

    ObservedCall last;
};

// Concrete animation that exercises BaseAnimationTemplate::setActive()
class FakeAnimation : public BaseAnimationTemplate<FakeAnimation, Animation::ZigZag> {
   public:
    void init() override {}
    void tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs) override {
        ARG_UNUSED(renderer);
        ARG_UNUSED(timeSinceLastTickMs);
    }
};
}  // namespace

ZTEST_SUITE(animation_base_tests, NULL, NULL, NULL, NULL, NULL);

// FakeAnimation overrides neither switch-point hook, so it stands in for every animation
// that doesn't — which includes ExtensionAnimationProxy, the reason these defaults matter:
// the rgbx ABI has no hook for either, so a sandboxed extension can never request extra
// shuffle grace and stays bounded by CONFIG_APP_SHUFFLE_GRACE_S alone. A default that
// drifted off 0 would silently widen that anti-hang guard for every extension.
ZTEST(animation_base_tests, test_default_switch_point_hooks) {
    BaseAnimation *anim = FakeAnimation::getInstance();

    zassert_true(anim->isAtGoodSwitchPoint(),
                 "an animation with no internal narrative must be switchable every frame");
    zassert_equal(anim->goodSwitchPointGraceMs(), 0u,
                  "an animation that doesn't know its next boundary must request no grace");
}

ZTEST(animation_base_tests, test_set_active_calls_observer_with_correct_id) {
    RecordingObserver observer;
    BaseAnimation::registerActiveStateObserver(&observer);

    FakeAnimation::getInstance()->setActive(true);

    zassert_equal(observer.last.callCount, 1, "Expected observer called exactly once");
    zassert_equal(observer.last.id, Animation::ZigZag, "Expected correct animation ID");
    zassert_true(observer.last.active, "Expected active == true");

    BaseAnimation::registerActiveStateObserver(nullptr);
}

ZTEST(animation_base_tests, test_set_active_passes_false_state) {
    RecordingObserver observer;
    BaseAnimation::registerActiveStateObserver(&observer);

    FakeAnimation::getInstance()->setActive(false);

    zassert_equal(observer.last.callCount, 1, "Expected observer called exactly once");
    zassert_false(observer.last.active, "Expected active == false");

    BaseAnimation::registerActiveStateObserver(nullptr);
}

ZTEST(animation_base_tests, test_set_active_safe_without_observer) {
    BaseAnimation::registerActiveStateObserver(nullptr);

    // Should not crash
    FakeAnimation::getInstance()->setActive(true);
}
