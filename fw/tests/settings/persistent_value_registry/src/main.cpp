#include <errno.h>
#include <settings/persistent_value_registry.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <zephyr/ztest.h>

namespace {

struct FakeReadCtx {
    const void *data;
    size_t len;
};

ssize_t fake_read_cb(void *cb_arg, void *data, size_t len) {
    auto *ctx = static_cast<FakeReadCtx *>(cb_arg);
    size_t copyLen = len < ctx->len ? len : ctx->len;
    memcpy(data, ctx->data, copyLen);
    return static_cast<ssize_t>(copyLen);
}

uint32_t sLoadedValue = 0;
size_t sLoadCallCount = 0;
size_t sSaveCallCount = 0;

void record_load(void *target, const void *data, size_t len) {
    ARG_UNUSED(target);
    if (len == sizeof(uint32_t)) {
        memcpy(&sLoadedValue, data, sizeof(sLoadedValue));
    }
    sLoadCallCount++;
}

void record_save(void *target) {
    ARG_UNUSED(target);
    sSaveCallCount++;
}

// Registers a caller-owned entry with the shared record_load/record_save callbacks.
// Entries are linked by pointer, so each must outlive its registration - callers keep
// them as locals living for the test, and the suite's after-hook clears the list after
// every test (even one that longjmps out on a zassert failure), so a prior test's
// now-dead entries are never dereferenced.
int registerEntry(PersistentValueRegistryEntry &e, const char *key) {
    return persistent_value_registry_register(&e, key, nullptr, record_load, record_save);
}

void reset_test_state() {
    persistent_value_registry_reset();
    sLoadedValue = 0;
    sLoadCallCount = 0;
    sSaveCallCount = 0;
}

// Runs after every test, pass or fail: entries are test-frame locals linked into the
// global list, so they must be unlinked before their frames die.
void registry_test_after(void *fixture) {
    ARG_UNUSED(fixture);
    reset_test_state();
}

}  // namespace

ZTEST_SUITE(persistent_value_registry_tests, NULL, NULL, NULL, registry_test_after, NULL);

ZTEST(persistent_value_registry_tests, test_register_and_dispatch_load) {
    reset_test_state();
    PersistentValueRegistryEntry e{};
    int ret = registerEntry(e, "foo/bar");
    zassert_equal(ret, 0, "Failed to register: %d", ret);

    uint32_t value = 42;
    FakeReadCtx ctx{&value, sizeof(value)};
    ret = persistent_value_registry_dispatch_load("foo/bar", sizeof(value), fake_read_cb, &ctx);
    zassert_equal(ret, 0, "Expected dispatch to succeed: %d", ret);
    zassert_equal(sLoadCallCount, 1, "Expected load to be called once");
    zassert_equal(sLoadedValue, 42, "Expected loaded value to match");
}

ZTEST(persistent_value_registry_tests, test_dispatch_load_unknown_key_returns_enoent) {
    reset_test_state();
    PersistentValueRegistryEntry e{};
    int ret = registerEntry(e, "foo/bar");
    zassert_equal(ret, 0, "Failed to register: %d", ret);

    uint32_t value = 1;
    FakeReadCtx ctx{&value, sizeof(value)};
    ret = persistent_value_registry_dispatch_load("other/key", sizeof(value), fake_read_cb, &ctx);
    zassert_equal(ret, -ENOENT, "Expected -ENOENT for unknown key, got %d", ret);
    zassert_equal(sLoadCallCount, 0, "Load should not have been called");
}

ZTEST(persistent_value_registry_tests, test_register_duplicate_key_returns_eexist) {
    reset_test_state();
    PersistentValueRegistryEntry e1{};
    int ret = registerEntry(e1, "foo/bar");
    zassert_equal(ret, 0, "Failed to register: %d", ret);

    PersistentValueRegistryEntry e2{};
    ret = registerEntry(e2, "foo/bar");
    zassert_equal(ret, -EEXIST, "Expected -EEXIST for duplicate key, got %d", ret);
    zassert_equal(persistent_value_registry_count(), 1, "Duplicate should not add an entry");
}

ZTEST(persistent_value_registry_tests, test_register_linked_entry_returns_ealready) {
    reset_test_state();
    PersistentValueRegistryEntry e{};
    int ret = registerEntry(e, "foo/bar");
    zassert_equal(ret, 0, "Failed to register: %d", ret);

    // Re-registering a live (already-linked) entry - even under a NEW key - must be
    // refused: sys_slist_append on a linked node would self-loop or truncate the list and
    // hang every later traversal. The duplicate-key check alone cannot catch this case.
    ret = registerEntry(e, "foo/other");
    zassert_equal(ret, -EALREADY, "Expected -EALREADY for an already-linked entry, got %d", ret);
    zassert_equal(persistent_value_registry_count(), 1, "Re-registration should not add an entry");

    // The list must still be walkable and the original registration intact.
    uint32_t value = 7;
    FakeReadCtx ctx{&value, sizeof(value)};
    ret = persistent_value_registry_dispatch_load("foo/bar", sizeof(value), fake_read_cb, &ctx);
    zassert_equal(ret, 0, "Original registration should still dispatch: %d", ret);
    zassert_equal(sLoadedValue, 7, "Expected loaded value to match");
}

ZTEST(persistent_value_registry_tests, test_register_rejects_null_arguments) {
    reset_test_state();

    PersistentValueRegistryEntry e{};

    int ret = persistent_value_registry_register(nullptr, "foo/bar", nullptr, record_load,
                                                 record_save);
    zassert_equal(ret, -EINVAL, "Expected -EINVAL for null entry, got %d", ret);

    ret = persistent_value_registry_register(&e, nullptr, nullptr, record_load, record_save);
    zassert_equal(ret, -EINVAL, "Expected -EINVAL for null key, got %d", ret);

    ret = persistent_value_registry_register(&e, "foo/bar", nullptr, nullptr, record_save);
    zassert_equal(ret, -EINVAL, "Expected -EINVAL for null load, got %d", ret);

    ret = persistent_value_registry_register(&e, "foo/bar", nullptr, record_load, nullptr);
    zassert_equal(ret, -EINVAL, "Expected -EINVAL for null save, got %d", ret);

    zassert_equal(persistent_value_registry_count(), 0, "No rejected entry should be registered");
}

ZTEST(persistent_value_registry_tests, test_save_all_calls_every_dirty_entry) {
    reset_test_state();
    PersistentValueRegistryEntry e1{};
    PersistentValueRegistryEntry e2{};
    registerEntry(e1, "foo/bar");
    registerEntry(e2, "foo/baz");

    persistent_value_registry_mark_dirty("foo/bar");
    persistent_value_registry_mark_dirty("foo/baz");
    persistent_value_registry_save_all();

    zassert_equal(sSaveCallCount, 2, "Expected save to be called once per dirty entry");
}

ZTEST(persistent_value_registry_tests, test_save_all_skips_non_dirty_entries) {
    reset_test_state();
    PersistentValueRegistryEntry e1{};
    PersistentValueRegistryEntry e2{};
    registerEntry(e1, "foo/bar");
    registerEntry(e2, "foo/baz");

    persistent_value_registry_mark_dirty("foo/bar");
    persistent_value_registry_save_all();

    zassert_equal(sSaveCallCount, 1, "Expected only the dirty entry to be saved, not the clean one");
}

ZTEST(persistent_value_registry_tests, test_save_all_clears_dirty_flag) {
    reset_test_state();
    PersistentValueRegistryEntry e{};
    registerEntry(e, "foo/bar");

    persistent_value_registry_mark_dirty("foo/bar");
    persistent_value_registry_save_all();
    persistent_value_registry_save_all();  // second call — flag should be clear, no re-save

    zassert_equal(sSaveCallCount, 1, "Expected second save_all() to skip already-saved entry");
}

ZTEST(persistent_value_registry_tests, test_reset_clears_entries) {
    reset_test_state();
    PersistentValueRegistryEntry e{};
    registerEntry(e, "foo/bar");
    zassert_equal(persistent_value_registry_count(), 1, "Expected 1 entry before reset");

    persistent_value_registry_reset();

    zassert_equal(persistent_value_registry_count(), 0, "Expected 0 entries after reset");
}

ZTEST(persistent_value_registry_tests, test_registry_has_no_capacity_cap) {
    reset_test_state();

    // The registry is an intrusive sys_slist with caller-owned entries and no fixed cap
    // (this replaced the old fixed 96-entry array + -ENOMEM overflow path). Register far
    // more than the old cap and confirm every single registration succeeds. Entries + keys
    // are static so they outlive their registrations for the whole test.
    constexpr size_t kN = 500;
    static PersistentValueRegistryEntry entries[kN];
    static char keys[kN][16];

    for (size_t i = 0; i < kN; i++) {
        snprintf(keys[i], sizeof(keys[i]), "k/%zu", i);
        int ret = registerEntry(entries[i], keys[i]);
        zassert_equal(ret, 0, "Registration %zu should succeed (no cap), got %d", i, ret);
    }

    zassert_equal(persistent_value_registry_count(), kN,
                  "Expected all %zu entries registered, got %zu", kN,
                  persistent_value_registry_count());
}
