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

// Builds an entry with the shared record_load/record_save callbacks. Entries are caller-
// owned and linked by pointer, so each must outlive its registration - callers keep them
// as locals living for the test, and reset_test_state() re-inits the list between tests
// (before any list access), so a prior test's now-dead entries are never dereferenced.
PersistentValueRegistryEntry makeEntry(const char *key) {
    return {key, nullptr, record_load, record_save, false, {}};
}

void reset_test_state() {
    persistent_value_registry_reset();
    sLoadedValue = 0;
    sLoadCallCount = 0;
    sSaveCallCount = 0;
}

}  // namespace

ZTEST_SUITE(persistent_value_registry_tests, NULL, NULL, NULL, NULL, NULL);

ZTEST(persistent_value_registry_tests, test_register_and_dispatch_load) {
    reset_test_state();
    PersistentValueRegistryEntry e = makeEntry("foo/bar");
    int ret = persistent_value_registry_register(&e);
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
    PersistentValueRegistryEntry e = makeEntry("foo/bar");
    int ret = persistent_value_registry_register(&e);
    zassert_equal(ret, 0, "Failed to register: %d", ret);

    uint32_t value = 1;
    FakeReadCtx ctx{&value, sizeof(value)};
    ret = persistent_value_registry_dispatch_load("other/key", sizeof(value), fake_read_cb, &ctx);
    zassert_equal(ret, -ENOENT, "Expected -ENOENT for unknown key, got %d", ret);
    zassert_equal(sLoadCallCount, 0, "Load should not have been called");
}

ZTEST(persistent_value_registry_tests, test_register_duplicate_key_returns_eexist) {
    reset_test_state();
    PersistentValueRegistryEntry e1 = makeEntry("foo/bar");
    int ret = persistent_value_registry_register(&e1);
    zassert_equal(ret, 0, "Failed to register: %d", ret);

    PersistentValueRegistryEntry e2 = makeEntry("foo/bar");
    ret = persistent_value_registry_register(&e2);
    zassert_equal(ret, -EEXIST, "Expected -EEXIST for duplicate key, got %d", ret);
    zassert_equal(persistent_value_registry_count(), 1, "Duplicate should not add an entry");
}

ZTEST(persistent_value_registry_tests, test_register_rejects_null_arguments) {
    reset_test_state();

    int ret = persistent_value_registry_register(nullptr);
    zassert_equal(ret, -EINVAL, "Expected -EINVAL for null entry, got %d", ret);

    PersistentValueRegistryEntry nullKey = {nullptr, nullptr, record_load, record_save, false, {}};
    ret = persistent_value_registry_register(&nullKey);
    zassert_equal(ret, -EINVAL, "Expected -EINVAL for null key, got %d", ret);

    PersistentValueRegistryEntry nullLoad = {"foo/bar", nullptr, nullptr, record_save, false, {}};
    ret = persistent_value_registry_register(&nullLoad);
    zassert_equal(ret, -EINVAL, "Expected -EINVAL for null load, got %d", ret);

    PersistentValueRegistryEntry nullSave = {"foo/bar", nullptr, record_load, nullptr, false, {}};
    ret = persistent_value_registry_register(&nullSave);
    zassert_equal(ret, -EINVAL, "Expected -EINVAL for null save, got %d", ret);

    zassert_equal(persistent_value_registry_count(), 0, "No rejected entry should be registered");
}

ZTEST(persistent_value_registry_tests, test_save_all_calls_every_dirty_entry) {
    reset_test_state();
    PersistentValueRegistryEntry e1 = makeEntry("foo/bar");
    PersistentValueRegistryEntry e2 = makeEntry("foo/baz");
    persistent_value_registry_register(&e1);
    persistent_value_registry_register(&e2);

    persistent_value_registry_mark_dirty("foo/bar");
    persistent_value_registry_mark_dirty("foo/baz");
    persistent_value_registry_save_all();

    zassert_equal(sSaveCallCount, 2, "Expected save to be called once per dirty entry");
}

ZTEST(persistent_value_registry_tests, test_save_all_skips_non_dirty_entries) {
    reset_test_state();
    PersistentValueRegistryEntry e1 = makeEntry("foo/bar");
    PersistentValueRegistryEntry e2 = makeEntry("foo/baz");
    persistent_value_registry_register(&e1);
    persistent_value_registry_register(&e2);

    persistent_value_registry_mark_dirty("foo/bar");
    persistent_value_registry_save_all();

    zassert_equal(sSaveCallCount, 1, "Expected only the dirty entry to be saved, not the clean one");
}

ZTEST(persistent_value_registry_tests, test_save_all_clears_dirty_flag) {
    reset_test_state();
    PersistentValueRegistryEntry e = makeEntry("foo/bar");
    persistent_value_registry_register(&e);

    persistent_value_registry_mark_dirty("foo/bar");
    persistent_value_registry_save_all();
    persistent_value_registry_save_all();  // second call — flag should be clear, no re-save

    zassert_equal(sSaveCallCount, 1, "Expected second save_all() to skip already-saved entry");
}

ZTEST(persistent_value_registry_tests, test_reset_clears_entries) {
    reset_test_state();
    PersistentValueRegistryEntry e = makeEntry("foo/bar");
    persistent_value_registry_register(&e);
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
        entries[i] = {keys[i], nullptr, record_load, record_save, false, {}};
        int ret = persistent_value_registry_register(&entries[i]);
        zassert_equal(ret, 0, "Registration %zu should succeed (no cap), got %d", i, ret);
    }

    zassert_equal(persistent_value_registry_count(), kN,
                  "Expected all %zu entries registered, got %zu", kN,
                  persistent_value_registry_count());
}
