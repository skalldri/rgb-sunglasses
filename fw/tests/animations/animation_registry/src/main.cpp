#include <animations/animation_registry.h>
#include <errno.h>
#include <zephyr/ztest.h>

namespace {
class FakeAnimation : public BaseAnimation {
   public:
    void init() override { initCount++; }

    void tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs) override {
        ARG_UNUSED(renderer);
        ARG_UNUSED(timeSinceLastTickMs);
    }

    void setActive(bool active) override { ARG_UNUSED(active); }

    size_t initCount = 0;
};

FakeAnimation sFirstAnimation;
FakeAnimation sSecondAnimation;
bool sLastActiveState = false;
size_t sSetActiveCallCount = 0;
bool sShuffleIncluded = true;
size_t sShuffleGetterCallCount = 0;

BaseAnimation *first_factory() {
    return &sFirstAnimation;
}

BaseAnimation *second_factory() {
    return &sSecondAnimation;
}

void record_active_state(bool active) {
    sLastActiveState = active;
    sSetActiveCallCount++;
}

bool report_shuffle_included(void) {
    sShuffleGetterCallCount++;
    return sShuffleIncluded;
}

void reset_test_state(void) {
    animation_registry_reset();
    sFirstAnimation.initCount = 0;
    sSecondAnimation.initCount = 0;
    sLastActiveState = false;
    sSetActiveCallCount = 0;
    sShuffleIncluded = true;
    sShuffleGetterCallCount = 0;
}
}  // namespace

ZTEST_SUITE(animation_registry_tests, NULL, NULL, NULL, NULL, NULL);

ZTEST(animation_registry_tests, test_lookup_unregistered_returns_null) {
    reset_test_state();
    BaseAnimation *animation = animation_registry_get(Animation::Text);
    zassert_is_null(animation, "Expected null for unregistered animation");
}

ZTEST(animation_registry_tests, test_register_and_lookup_animation) {
    reset_test_state();
    int ret = animation_registry_register(Animation::Text, first_factory);
    zassert_equal(ret, 0, "Failed to register animation: %d", ret);

    BaseAnimation *animation = animation_registry_get(Animation::Text);
    zassert_equal_ptr(animation, &sFirstAnimation, "Lookup returned unexpected animation pointer");
    zassert_equal(animation_registry_count(), 1, "Expected a single registry entry");
}

ZTEST(animation_registry_tests, test_register_replaces_existing_factory) {
    reset_test_state();
    int ret = animation_registry_register(Animation::Text, first_factory);
    zassert_equal(ret, 0, "Failed to register first animation: %d", ret);

    ret = animation_registry_register(Animation::Text, second_factory);
    zassert_equal(ret, 0, "Failed to replace animation factory: %d", ret);

    BaseAnimation *animation = animation_registry_get(Animation::Text);
    zassert_equal_ptr(animation, &sSecondAnimation, "Expected lookup to use replacement factory");
    zassert_equal(animation_registry_count(), 1, "Replacing factory should not change entry count");
}

ZTEST(animation_registry_tests, test_init_registered_calls_init_for_each_entry) {
    reset_test_state();
    int ret = animation_registry_register(Animation::Text, first_factory);
    zassert_equal(ret, 0, "Failed to register first animation: %d", ret);

    ret = animation_registry_register(Animation::Rainbow, second_factory);
    zassert_equal(ret, 0, "Failed to register second animation: %d", ret);

    animation_registry_init_registered();

    zassert_equal(sFirstAnimation.initCount, 1, "Expected first animation init to be called once");
    zassert_equal(sSecondAnimation.initCount, 1,
                  "Expected second animation init to be called once");
}

ZTEST(animation_registry_tests, test_register_is_active_and_dispatch) {
    reset_test_state();
    int ret = animation_registry_register(Animation::Text, first_factory);
    zassert_equal(ret, 0, "Failed to register animation: %d", ret);

    ret = animation_registry_register_is_active(Animation::Text, record_active_state);
    zassert_equal(ret, 0, "Failed to register IsActive callback: %d", ret);

    animation_registry_set_is_active(Animation::Text, true);

    zassert_equal(sSetActiveCallCount, 1, "Expected one IsActive dispatch");
    zassert_true(sLastActiveState, "Expected active state to be true");
}

ZTEST(animation_registry_tests, test_register_is_active_requires_animation_registration) {
    reset_test_state();
    int ret = animation_registry_register_is_active(Animation::Text, record_active_state);
    zassert_equal(ret, -ENOENT, "Expected -ENOENT for unregistered animation, got %d", ret);
}

ZTEST(animation_registry_tests, test_register_shuffle_include_requires_animation_registration) {
    reset_test_state();
    // Same -ENOENT contract as register_is_active (issue #243) — an ignored failure
    // here would silently leave the animation permanently included (PR #89 class).
    int ret = animation_registry_register_shuffle_include(Animation::Text,
                                                          report_shuffle_included);
    zassert_equal(ret, -ENOENT, "Expected -ENOENT for unregistered animation, got %d", ret);

    ret = animation_registry_register(Animation::Text, first_factory);
    zassert_equal(ret, 0, "Failed to register animation: %d", ret);
    ret = animation_registry_register_shuffle_include(Animation::Text,
                                                      report_shuffle_included);
    zassert_equal(ret, 0, "Expected registration to succeed after register(): %d", ret);
}

ZTEST(animation_registry_tests, test_register_shuffle_include_rejects_null_getter) {
    reset_test_state();
    animation_registry_register(Animation::Text, first_factory);
    int ret = animation_registry_register_shuffle_include(Animation::Text, NULL);
    zassert_equal(ret, -EINVAL, "Expected -EINVAL for a NULL getter, got %d", ret);
}

ZTEST(animation_registry_tests, test_shuffle_included_defaults_true) {
    reset_test_state();
    // Unknown id: default included.
    zassert_true(animation_registry_shuffle_included(Animation::Text),
                 "Unknown id must default to included");
    // Registered id with no getter: still default included.
    animation_registry_register(Animation::Text, first_factory);
    zassert_true(animation_registry_shuffle_included(Animation::Text),
                 "Registered id with no getter must default to included");
    zassert_equal(sShuffleGetterCallCount, 0, "No getter should have been consulted");
}

ZTEST(animation_registry_tests, test_shuffle_included_consults_getter) {
    reset_test_state();
    animation_registry_register(Animation::Text, first_factory);
    int ret = animation_registry_register_shuffle_include(Animation::Text,
                                                          report_shuffle_included);
    zassert_equal(ret, 0, "Failed to register shuffle-include getter: %d", ret);

    sShuffleIncluded = true;
    zassert_true(animation_registry_shuffle_included(Animation::Text),
                 "Expected getter's true to be returned");
    sShuffleIncluded = false;
    zassert_false(animation_registry_shuffle_included(Animation::Text),
                  "Expected getter's false to be returned (pulled per call, not cached)");
    zassert_equal(sShuffleGetterCallCount, 2, "Expected the getter consulted once per query");
}

ZTEST(animation_registry_tests, test_reset_after_registration_clears_entries) {
    reset_test_state();
    animation_registry_register(Animation::Text, first_factory);
    animation_registry_register(Animation::Rainbow, second_factory);
    zassert_equal(animation_registry_count(), 2, "Expected 2 entries before reset");

    animation_registry_reset();

    zassert_equal(animation_registry_count(), 0, "Expected 0 entries after reset");
    zassert_is_null(animation_registry_get(Animation::Text), "Expected null lookup after reset");
}

ZTEST(animation_registry_tests, test_init_registered_before_any_registration_is_safe) {
    reset_test_state();

    // Should not crash with zero registrations
    animation_registry_init_registered();
}

ZTEST(animation_registry_tests, test_id_at_returns_ids_in_registration_order) {
    reset_test_state();
    animation_registry_register(Animation::Text, first_factory);
    animation_registry_register(Animation::Rainbow, second_factory);

    zassert_equal((int)animation_registry_id_at(0), (int)Animation::Text,
                  "Index 0 must be the first-registered id");
    zassert_equal((int)animation_registry_id_at(1), (int)Animation::Rainbow,
                  "Index 1 must be the second-registered id");
}

ZTEST(animation_registry_tests, test_id_at_out_of_range_returns_none) {
    reset_test_state();
    zassert_equal((int)animation_registry_id_at(0), (int)Animation::None,
                  "Empty registry must return None for any index");

    animation_registry_register(Animation::Text, first_factory);
    zassert_equal((int)animation_registry_id_at(1), (int)Animation::None,
                  "Index == count must return None");
    zassert_equal((int)animation_registry_id_at(9999), (int)Animation::None,
                  "Far out-of-range index must return None");
}
