#include <errno.h>
#include <settings/persistent_value_registry.h>
#include <string.h>
#include <sys/types.h>
#include <zephyr/logging/log.h>

// When the feature is disabled (e.g. CONFIG_APP_PERSIST_BT_CONFIG=n on
// rgb_sunglasses_dk), compile out the fixed-size entry table (and the log module that
// reports failures touching it) entirely rather than just leaving them unreferenced -
// every call site that populates the table is itself gated by
// IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG), so these stubs are never invoked there.
#if defined(CONFIG_APP_PERSIST_BT_CONFIG)

LOG_MODULE_REGISTER(persistent_value_registry, CONFIG_LOG_DEFAULT_LEVEL);

namespace {
struct PersistentValueRegistryEntry {
    const char *key;
    void *target;
    PersistentValueLoadFn load;
    PersistentValueSaveFn save;
    bool dirty;
};

constexpr size_t kMaxRegistryEntries = 96;
PersistentValueRegistryEntry sRegistry[kMaxRegistryEntries];
size_t sRegistryCount = 0;

ssize_t findRegistryIndex(const char *key) {
    for (size_t i = 0; i < sRegistryCount; i++) {
        if (strcmp(sRegistry[i].key, key) == 0) {
            return i;
        }
    }

    return -1;
}
}  // namespace

int persistent_value_registry_register(const char *key, void *target, PersistentValueLoadFn load,
                                       PersistentValueSaveFn save) {
    if (!key || !load || !save) {
        LOG_ERR("Refusing to register persisted value with a null key/load/save");
        return -EINVAL;
    }

    if (findRegistryIndex(key) >= 0) {
        LOG_ERR("Persisted value '%s' is already registered", key);
        return -EEXIST;
    }

    if (sRegistryCount >= kMaxRegistryEntries) {
        LOG_ERR("Persisted value table is full (max %zu) - dropping '%s'", kMaxRegistryEntries,
                key);
        return -ENOMEM;
    }

    sRegistry[sRegistryCount] = {
        .key = key,
        .target = target,
        .load = load,
        .save = save,
        .dirty = false,
    };
    sRegistryCount++;
    return 0;
}

int persistent_value_registry_dispatch_load(const char *name, size_t len, settings_read_cb read_cb,
                                            void *cb_arg) {
    for (size_t i = 0; i < sRegistryCount; i++) {
        const char *next = nullptr;
        if (!settings_name_steq(name, sRegistry[i].key, &next) || next) {
            continue;
        }

        if (len > SETTINGS_MAX_VAL_LEN) {
            return -EINVAL;
        }

        uint8_t buf[SETTINGS_MAX_VAL_LEN];
        ssize_t readLen = read_cb(cb_arg, buf, len);
        if (readLen < 0) {
            return static_cast<int>(readLen);
        }

        sRegistry[i].load(sRegistry[i].target, buf, static_cast<size_t>(readLen));
        return 0;
    }

    return -ENOENT;
}

void persistent_value_registry_mark_dirty(const char *key) {
    ssize_t i = findRegistryIndex(key);
    if (i >= 0) {
        sRegistry[i].dirty = true;
    }
}

void persistent_value_registry_save_all() {
    for (size_t i = 0; i < sRegistryCount; i++) {
        if (!sRegistry[i].dirty) {
            continue;
        }
        sRegistry[i].dirty = false;
        sRegistry[i].save(sRegistry[i].target);
    }
}

void persistent_value_registry_reset() {
    sRegistryCount = 0;
}

size_t persistent_value_registry_count() {
    return sRegistryCount;
}

#else  // !CONFIG_APP_PERSIST_BT_CONFIG

int persistent_value_registry_register(const char *, void *, PersistentValueLoadFn,
                                       PersistentValueSaveFn) {
    return -ENOSYS;
}

int persistent_value_registry_dispatch_load(const char *, size_t, settings_read_cb, void *) {
    return -ENOENT;
}

void persistent_value_registry_mark_dirty(const char *) {}

void persistent_value_registry_save_all() {}

void persistent_value_registry_reset() {}

size_t persistent_value_registry_count() {
    return 0;
}

#endif  // CONFIG_APP_PERSIST_BT_CONFIG
