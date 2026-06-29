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
    persistent_value_registry_register(before.key, &before, uint32_load, uint32_save);

    persistent_value_registry_mark_dirty(before.key);
    persistent_value_store::request_save();
    k_sleep(K_MSEC(CONFIG_APP_SETTINGS_SAVE_DEBOUNCE_MS + 50));

    zassert_equal(sSaveCallCount, 1, "Expected exactly one save after the debounce window");

    // Simulate a reboot: a fresh registration of the same key, into a brand new in-memory
    // target defaulting to 0, should pick up what was actually written to flash.
    persistent_value_registry_reset();
    FakeUint32Entry after{"test/round_trip", 0};
    persistent_value_registry_register(after.key, &after, uint32_load, uint32_save);

    settings_load();

    zassert_equal(after.value, 1234, "Expected reloaded value to match what was saved");
}

ZTEST(persistent_value_store_tests, test_rapid_requests_coalesce_into_one_save) {
    reset_test_state();

    FakeUint32Entry entry{"test/coalesce", 0};
    persistent_value_registry_register(entry.key, &entry, uint32_load, uint32_save);

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
    persistent_value_registry_register(before.key, &before, string_load, string_save);

    persistent_value_registry_mark_dirty(before.key);
    persistent_value_store::request_save();
    k_sleep(K_MSEC(CONFIG_APP_SETTINGS_SAVE_DEBOUNCE_MS + 50));

    persistent_value_registry_reset();
    FakeStringEntry after{"test/string_round_trip", ""};
    persistent_value_registry_register(after.key, &after, string_load, string_save);

    settings_load();

    zassert_str_equal(after.value, "hello", "Expected reloaded string to match what was saved");
}
