#include <errno.h>
#include <settings/persistent_value_registry.h>
#include <string.h>
#include <sys/types.h>

namespace {
struct PersistentValueRegistryEntry {
    const char *key;
    void *target;
    PersistentValueLoadFn load;
    PersistentValueSaveFn save;
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
        return -EINVAL;
    }

    if (findRegistryIndex(key) >= 0) {
        return -EEXIST;
    }

    if (sRegistryCount >= kMaxRegistryEntries) {
        return -ENOMEM;
    }

    sRegistry[sRegistryCount] = {
        .key = key,
        .target = target,
        .load = load,
        .save = save,
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

void persistent_value_registry_save_all() {
    for (size_t i = 0; i < sRegistryCount; i++) {
        sRegistry[i].save(sRegistry[i].target);
    }
}

void persistent_value_registry_reset() {
    sRegistryCount = 0;
}

size_t persistent_value_registry_count() {
    return sRegistryCount;
}
