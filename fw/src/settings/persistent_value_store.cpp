#include <settings/persistent_value_registry.h>
#include <settings/persistent_value_store.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <errno.h>

#include <cstdio>

// When the feature is disabled (CONFIG_APP_PERSIST_BT_CONFIG=n, as on the legacy DK
// board on the dk-support branch), compile out the debounce work item, shared settings handler, and
// the log module that reports save failures entirely rather than just leaving them
// unused - every call site that would trigger them is itself gated by
// IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG), so the stubs below are never invoked there.
#if defined(CONFIG_APP_PERSIST_BT_CONFIG)

LOG_MODULE_REGISTER(persistent_value_store, CONFIG_LOG_DEFAULT_LEVEL);

namespace {

// Kernel-only work queue: K_KERNEL_STACK_* skips the 1KB CONFIG_USERSPACE privileged
// stack; this stack can never host a K_USER thread.
K_KERNEL_STACK_DEFINE(persistent_value_store_stack, CONFIG_APP_PERSIST_WORKQ_STACK_SIZE);

// This queue does long, bursty NVS/QSPI writes; it must never outrank a rendering thread.
// The default matches CONFIG_NUM_PREEMPT_PRIORITIES - 1 (see fw/docs/threading.md).
BUILD_ASSERT(CONFIG_APP_PERSIST_WORKQ_PRIORITY >= 0 &&
                 CONFIG_APP_PERSIST_WORKQ_PRIORITY < CONFIG_NUM_PREEMPT_PRIORITIES,
             "CONFIG_APP_PERSIST_WORKQ_PRIORITY must be a valid preemptible priority");

struct k_work_q persistent_value_lowpri_workq;

void save_work_handler(struct k_work* work) {
    ARG_UNUSED(work);
    uint64_t start = k_uptime_get();
    persistent_value_registry_save_all();
    uint64_t end = k_uptime_get();
    LOG_INF("Persisted values saved in %llu ms", end - start);
}

K_WORK_DELAYABLE_DEFINE(sSaveWork, save_work_handler);

int appcfg_handle_set(const char* name, size_t len, settings_read_cb read_cb, void* cb_arg) {
    return persistent_value_registry_dispatch_load(name, len, read_cb, cb_arg);
}

// Single shared subtree handler serving every persisted key (see
// persistent_value_registry.h for why one handler dispatches to many keys instead of
// one SETTINGS_STATIC_HANDLER_DEFINE per characteristic).
SETTINGS_STATIC_HANDLER_DEFINE(appcfg, "appcfg", NULL, appcfg_handle_set, NULL, NULL);

int appcfg_settings_init() {
    k_work_queue_init(&persistent_value_lowpri_workq);
    // Named so `kernel thread list` can attribute this queue's priority and stack
    // high-water mark on device — see fw/docs/threading.md.
    static const struct k_work_queue_config cfg = {.name = "persist_wq"};
    k_work_queue_start(&persistent_value_lowpri_workq, persistent_value_store_stack,
                       K_KERNEL_STACK_SIZEOF(persistent_value_store_stack),
                       CONFIG_APP_PERSIST_WORKQ_PRIORITY, &cfg);
    return 0;
}

SYS_INIT(appcfg_settings_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

}  // namespace

namespace persistent_value_store {

void request_save() {
    k_work_reschedule_for_queue(&persistent_value_lowpri_workq, &sSaveWork,
                                K_MSEC(CONFIG_APP_SETTINGS_SAVE_DEBOUNCE_MS));
}

void save_value(const char* key, const void* data, size_t len) {
    uint64_t start = k_uptime_get();

    char fullKey[SETTINGS_MAX_NAME_LEN + 1];
    int ret = snprintf(fullKey, sizeof(fullKey), "%s/%s", kSubtreeName, key);
    if (ret < 0 || static_cast<size_t>(ret) >= sizeof(fullKey)) {
        return;
    }

    int err = settings_save_one(fullKey, data, len);
    if (err) {
        LOG_ERR("Failed to save persisted value '%s' (err: %d)", fullKey, err);
    }

    uint64_t end = k_uptime_get();
    LOG_INF("Single value Saved in %llu ms", end - start);
}

ssize_t load_value(const char* key, void* buf, size_t bufLen) {
    char fullKey[SETTINGS_MAX_NAME_LEN + 1];
    int ret = snprintf(fullKey, sizeof(fullKey), "%s/%s", kSubtreeName, key);
    if (ret < 0 || static_cast<size_t>(ret) >= sizeof(fullKey)) {
        return -EINVAL;
    }

    ssize_t len = settings_load_one(fullKey, buf, bufLen);
    if (len < 0) {
        LOG_ERR("Failed to load persisted value '%s' (err: %zd)", fullKey, len);
    }
    return len;
}

void delete_value(const char* key) {
    char fullKey[SETTINGS_MAX_NAME_LEN + 1];
    int ret = snprintf(fullKey, sizeof(fullKey), "%s/%s", kSubtreeName, key);
    if (ret < 0 || static_cast<size_t>(ret) >= sizeof(fullKey)) {
        return;
    }

    int err = settings_delete(fullKey);
    if (err) {
        LOG_ERR("Failed to delete persisted value '%s' (err: %d)", fullKey, err);
    }
}

namespace {

void purge_work_handler(struct k_work* work) {
    auto* purge = CONTAINER_OF(work, PersistentValuePurge, work);
    // -ENOENT (never registered / already unregistered) is fine: the record
    // deletion below is still wanted, e.g. for keys whose owner lost an
    // -EEXIST registration race but whose record predates this boot.
    persistent_value_registry_unregister(purge->entry);
    if (purge->key != nullptr) {
        delete_value(purge->key);
    }
}

}  // namespace

int purge_value(PersistentValuePurge* purge, PersistentValueRegistryEntry* entry,
                const char* key) {
    if (purge == nullptr || entry == nullptr) {
        return -EINVAL;
    }

    // Caller-owned storage (see the header). A purge record pairs permanently
    // with one entry/key (it lives next to the entry it purges), so a busy
    // record means this exact purge is already queued or running — skip
    // rather than k_work_init a live item, which corrupts the queue.
    if (k_work_busy_get(&purge->work) != 0) {
        return 0;
    }
    purge->entry = entry;
    purge->key = key;
    k_work_init(&purge->work, purge_work_handler);
    k_work_submit_to_queue(&persistent_value_lowpri_workq, &purge->work);
    return 0;
}

void cancel_pending_save() {
    struct k_work_sync sync;
    k_work_cancel_delayable_sync(&sSaveWork, &sync);
}

}  // namespace persistent_value_store

#else  // !CONFIG_APP_PERSIST_BT_CONFIG

namespace persistent_value_store {

void request_save() {}

void save_value(const char*, const void*, size_t) {}

void delete_value(const char*) {}

int purge_value(PersistentValuePurge*, PersistentValueRegistryEntry*, const char*) {
    return -ENOSYS;
}

void cancel_pending_save() {}

ssize_t load_value(const char*, void*, size_t) {
    return -ENOSYS;
}

}  // namespace persistent_value_store

#endif  // CONFIG_APP_PERSIST_BT_CONFIG
