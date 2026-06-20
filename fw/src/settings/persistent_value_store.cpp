#include <settings/persistent_value_registry.h>
#include <settings/persistent_value_store.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/printk.h>

#include <cstdio>

// When the feature is disabled (e.g. CONFIG_APP_PERSIST_BT_CONFIG=n on
// rgb_sunglasses_dk), compile out the debounce work item and shared settings handler
// entirely rather than just leaving them unused - every call site that would trigger
// them is itself gated by IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG), so the stubs below
// are never invoked there.
#if defined(CONFIG_APP_PERSIST_BT_CONFIG)

namespace {

void save_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    persistent_value_registry_save_all();
}

K_WORK_DELAYABLE_DEFINE(sSaveWork, save_work_handler);

int appcfg_handle_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    return persistent_value_registry_dispatch_load(name, len, read_cb, cb_arg);
}

// Single shared subtree handler serving every persisted key (see
// persistent_value_registry.h for why one handler dispatches to many keys instead of
// one SETTINGS_STATIC_HANDLER_DEFINE per characteristic).
SETTINGS_STATIC_HANDLER_DEFINE(appcfg, "appcfg", NULL, appcfg_handle_set, NULL, NULL);

}  // namespace

namespace persistent_value_store {

void request_save() {
    k_work_reschedule(&sSaveWork, K_MSEC(CONFIG_APP_SETTINGS_SAVE_DEBOUNCE_MS));
}

void save_value(const char *key, const void *data, size_t len) {
    char fullKey[SETTINGS_MAX_NAME_LEN + 1];
    int ret = snprintf(fullKey, sizeof(fullKey), "%s/%s", kSubtreeName, key);
    if (ret < 0 || static_cast<size_t>(ret) >= sizeof(fullKey)) {
        return;
    }

    int err = settings_save_one(fullKey, data, len);
    if (err) {
        printk("Failed to save persisted value '%s' (err: %d)\n", fullKey, err);
    }
}

}  // namespace persistent_value_store

#else  // !CONFIG_APP_PERSIST_BT_CONFIG

namespace persistent_value_store {

void request_save() {}

void save_value(const char *, const void *, size_t) {}

}  // namespace persistent_value_store

#endif  // CONFIG_APP_PERSIST_BT_CONFIG
