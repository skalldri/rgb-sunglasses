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

// Fill a Fake struct's caller-owned entry and register it (registry links it by pointer,
// so the Fake struct must outlive the registration - here it's a test-local).
void registerFake(FakeUint32Entry &e) {
    e.reg = {e.key, &e, uint32_load, uint32_save, false, {}};
    persistent_value_registry_register(&e.reg);
}

void registerFake(FakeStringEntry &e) {
    e.reg = {e.key, &e, string_load, string_save, false, {}};
    persistent_value_registry_register(&e.reg);
}

void reset_test_state() {
    persistent_value_registry_reset();
    sSaveCallCount = 0;
}

void *settings_test_setup(void) {
    settings_subsys_init();
    return nullptr;
}

}  // namespace

ZTEST_SUITE(persistent_value_store_tests, NULL, settings_test_setup, NULL, NULL, NULL);

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
