#include <errno.h>
#include <settings/persistent_value_registry.h>
#include <settings/persistent_value_store.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/ztest.h>

namespace {

struct FakeUint32Entry {
    const char *key;
    uint32_t value;
    PersistentValueRegistryEntry reg{};  // caller-owned registry storage
};

size_t sSaveCallCount = 0;

void uint32_load(void *target, const void *data, size_t len) {
    auto *entry = static_cast<FakeUint32Entry *>(target);
    if (len == sizeof(uint32_t)) {
        memcpy(&entry->value, data, sizeof(entry->value));
    }
}

void uint32_save(void *target) {
    auto *entry = static_cast<FakeUint32Entry *>(target);
    sSaveCallCount++;
    persistent_value_store::save_value(entry->key, &entry->value, sizeof(entry->value));
}

struct FakeStringEntry {
    const char *key;
    char value[32];
    PersistentValueRegistryEntry reg{};  // caller-owned registry storage
};

void string_load(void *target, const void *data, size_t len) {
    auto *entry = static_cast<FakeStringEntry *>(target);
    size_t copyLen = len < sizeof(entry->value) - 1 ? len : sizeof(entry->value) - 1;
    memcpy(entry->value, data, copyLen);
    entry->value[copyLen] = '\0';
}

void string_save(void *target) {
    auto *entry = static_cast<FakeStringEntry *>(target);
    persistent_value_store::save_value(entry->key, entry->value, strlen(entry->value) + 1);
}

// Register a Fake struct's caller-owned entry (the registry fills it and links it by
// pointer, so the Fake struct must outlive the registration - here it's a test-local).
void registerFake(FakeUint32Entry &e) {
    persistent_value_registry_register(&e.reg, e.key, &e, uint32_load, uint32_save);
}

void registerFake(FakeStringEntry &e) {
    persistent_value_registry_register(&e.reg, e.key, &e, string_load, string_save);
}

void reset_test_state() {
    persistent_value_registry_reset();
    sSaveCallCount = 0;
}

void *settings_test_setup(void) {
    settings_subsys_init();
    return nullptr;
}

// Runs after every test, pass or fail. The Fake entries are test-frame locals linked into
// the global registry, and a zassert failure longjmps out of the test with them still
// linked and a debounced save possibly still pending - which would later walk (and call
// save function pointers read from) the dead stack frames. Cancel the save synchronously
// and unlink everything before the frames die.
void store_test_after(void *fixture) {
    ARG_UNUSED(fixture);
    persistent_value_store::cancel_pending_save();
    persistent_value_registry_reset();
}

}  // namespace

ZTEST_SUITE(persistent_value_store_tests, NULL, settings_test_setup, NULL, store_test_after, NULL);

ZTEST(persistent_value_store_tests, test_save_and_reload_round_trip) {
    reset_test_state();

    FakeUint32Entry before{"test/round_trip", 1234};
    registerFake(before);

    persistent_value_registry_mark_dirty(before.key);
    persistent_value_store::request_save();
    k_sleep(K_MSEC(CONFIG_APP_SETTINGS_SAVE_DEBOUNCE_MS + 50));

    zassert_equal(sSaveCallCount, 1, "Expected exactly one save after the debounce window");

    // Simulate a reboot: a fresh registration of the same key, into a brand new in-memory
    // target defaulting to 0, should pick up what was actually written to flash.
    persistent_value_registry_reset();
    FakeUint32Entry after{"test/round_trip", 0};
    registerFake(after);

    settings_load();

    zassert_equal(after.value, 1234, "Expected reloaded value to match what was saved");
}

ZTEST(persistent_value_store_tests, test_rapid_requests_coalesce_into_one_save) {
    reset_test_state();

    FakeUint32Entry entry{"test/coalesce", 0};
    registerFake(entry);

    entry.value = 1;
    persistent_value_registry_mark_dirty(entry.key);
    persistent_value_store::request_save();
    entry.value = 2;
    persistent_value_registry_mark_dirty(entry.key);
    persistent_value_store::request_save();
    entry.value = 3;
    persistent_value_registry_mark_dirty(entry.key);
    persistent_value_store::request_save();

    k_sleep(K_MSEC(CONFIG_APP_SETTINGS_SAVE_DEBOUNCE_MS + 50));

    zassert_equal(sSaveCallCount, 1,
                  "Expected 3 rapid request_save() calls to coalesce into a single save, got %zu",
                  sSaveCallCount);
}

ZTEST(persistent_value_store_tests, test_string_value_round_trip) {
    reset_test_state();

    FakeStringEntry before{"test/string_round_trip", "hello"};
    registerFake(before);

    persistent_value_registry_mark_dirty(before.key);
    persistent_value_store::request_save();
    k_sleep(K_MSEC(CONFIG_APP_SETTINGS_SAVE_DEBOUNCE_MS + 50));

    persistent_value_registry_reset();
    FakeStringEntry after{"test/string_round_trip", ""};
    registerFake(after);

    settings_load();

    zassert_str_equal(after.value, "hello", "Expected reloaded string to match what was saved");
}

ZTEST(persistent_value_store_tests, test_load_value_reads_saved_value) {
    reset_test_state();

    const char *key = "test/load_value_direct";
    uint32_t saved = 5678;
    persistent_value_store::save_value(key, &saved, sizeof(saved));

    uint32_t loaded = 0;
    ssize_t len = persistent_value_store::load_value(key, &loaded, sizeof(loaded));

    zassert_equal(len, static_cast<ssize_t>(sizeof(loaded)),
                  "Expected load_value to read back sizeof(uint32_t) bytes, got %zd", len);
    zassert_equal(loaded, 5678, "Expected loaded value to match what was saved");
}

ZTEST(persistent_value_store_tests, test_load_value_returns_zero_when_never_saved) {
    reset_test_state();

    uint32_t loaded = 0xDEADBEEF;
    ssize_t len =
        persistent_value_store::load_value("test/never_saved_key", &loaded, sizeof(loaded));

    zassert_equal(len, 0, "Expected load_value to return 0 for a key that was never saved, got %zd",
                  len);
}

ZTEST(persistent_value_store_tests, test_delete_value_removes_saved_record) {
    reset_test_state();
    uint32_t value = 99;
    persistent_value_store::save_value("del/one", &value, sizeof(value));

    uint32_t readBack = 0;
    zassert_equal(persistent_value_store::load_value("del/one", &readBack, sizeof(readBack)),
                  (ssize_t)sizeof(readBack), "value should exist before delete");

    persistent_value_store::delete_value("del/one");

    zassert_equal(persistent_value_store::load_value("del/one", &readBack, sizeof(readBack)), 0,
                  "deleted value must read back as never-saved");
}

ZTEST(persistent_value_store_tests, test_delete_value_missing_record_is_harmless) {
    reset_test_state();
    // A missing record is not an error — DELETE cleanup runs for keys whose
    // owner may never have flushed anything.
    persistent_value_store::delete_value("del/never_saved");
}

ZTEST(persistent_value_store_tests, test_purge_value_unregisters_and_deletes) {
    reset_test_state();
    FakeUint32Entry e{.key = "purge/one", .value = 7};
    static persistent_value_store::PersistentValuePurge purge;
    purge = {};
    registerFake(e);
    persistent_value_store::save_value(e.key, &e.value, sizeof(e.value));

    // The extension-DELETE cleanup invariant (fw/docs/extension-management.md):
    // even with a dirty flag queued, after purge_value the record is gone AND
    // stays gone — the debounced flush can no longer resurrect it because the
    // purge runs as one work item on the same workqueue as the flush.
    persistent_value_registry_mark_dirty(e.key);
    persistent_value_store::request_save();

    int ret = persistent_value_store::purge_value(&purge, &e.reg, e.key);
    zassert_equal(ret, 0, "purge_value failed: %d", ret);

    // The purge is asynchronous (fire-and-forget submit): give the work item
    // a moment to run, then let the still-queued debounced sweep fire too.
    k_sleep(K_MSEC(50));
    zassert_equal(persistent_value_registry_count(), 0, "entry should be unregistered");

    k_sleep(K_MSEC(CONFIG_APP_SETTINGS_SAVE_DEBOUNCE_MS + 50));
    uint32_t readBack = 0;
    zassert_equal(persistent_value_store::load_value(e.key, &readBack, sizeof(readBack)), 0,
                  "purged record must not be resurrected by a queued save");
    zassert_equal(sSaveCallCount, 0, "unregistered entry's save must not run");
}

ZTEST(persistent_value_store_tests, test_purge_value_tolerates_unregistered_entry) {
    reset_test_state();
    // Entry never registered (e.g. a display-name-collision loser), but a
    // record under the key may predate this boot: purge still deletes it.
    FakeUint32Entry e{.key = "purge/loser", .value = 3};
    static persistent_value_store::PersistentValuePurge purge;
    purge = {};
    persistent_value_store::save_value(e.key, &e.value, sizeof(e.value));

    int ret = persistent_value_store::purge_value(&purge, &e.reg, e.key);
    zassert_equal(ret, 0, "purge of an unregistered entry should succeed, got %d", ret);

    k_sleep(K_MSEC(50));
    uint32_t readBack = 0;
    zassert_equal(persistent_value_store::load_value(e.key, &readBack, sizeof(readBack)), 0,
                  "record should be deleted even for an unregistered entry");

    ret = persistent_value_store::purge_value(&purge, nullptr, "purge/loser");
    zassert_equal(ret, -EINVAL, "null entry must be -EINVAL, got %d", ret);
    ret = persistent_value_store::purge_value(nullptr, &e.reg, "purge/loser");
    zassert_equal(ret, -EINVAL, "null purge storage must be -EINVAL, got %d", ret);
}
