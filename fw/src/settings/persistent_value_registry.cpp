#include <errno.h>
#include <settings/persistent_value_registry.h>
#include <string.h>
#include <sys/types.h>
#include <zephyr/logging/log.h>

// When the feature is disabled (e.g. CONFIG_APP_PERSIST_BT_CONFIG=n on
// rgb_sunglasses_dk), compile out the registry (and the log module that reports failures
// touching it) entirely rather than just leaving it unreferenced - every call site that
// registers an entry is itself gated by IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG), so these
// stubs are never invoked there.
#if defined(CONFIG_APP_PERSIST_BT_CONFIG)

LOG_MODULE_REGISTER(persistent_value_registry, CONFIG_LOG_DEFAULT_LEVEL);

namespace {
// Intrusive list of caller-owned PersistentValueRegistryEntry records (each links in via
// its embedded .node). No fixed capacity - append is O(1) and can never fail for lack of
// space, so registration cannot be silently dropped. Same idiom as Zephyr's own settings
// backend (settings_store.c: sys_slist_t settings_load_srcs). Registration is single-
// threaded at static-init/boot, so no locking is needed (unchanged invariant).
sys_slist_t sRegistry = SYS_SLIST_STATIC_INIT(&sRegistry);

PersistentValueRegistryEntry *findRegistryEntry(const char *key) {
    PersistentValueRegistryEntry *e;
    SYS_SLIST_FOR_EACH_CONTAINER(&sRegistry, e, node) {
        if (strcmp(e->key, key) == 0) {
            return e;
        }
    }
    return nullptr;
}
}  // namespace

int persistent_value_registry_register(PersistentValueRegistryEntry *entry, const char *key,
                                       void *target, PersistentValueLoadFn load,
                                       PersistentValueSaveFn save) {
    if (!entry || !key || !load || !save) {
        LOG_ERR("Refusing to register persisted value with a null entry/key/load/save");
        return -EINVAL;
    }

    // One walk checks both hazards: a duplicate key, and this exact record already being
    // linked (sys_slist_append on a linked node self-loops or truncates the list, hanging
    // every later traversal - refuse rather than corrupt).
    PersistentValueRegistryEntry *e;
    SYS_SLIST_FOR_EACH_CONTAINER(&sRegistry, e, node) {
        if (e == entry) {
            LOG_ERR("Entry for '%s' is already linked into the registry (as '%s')", key, e->key);
            return -EALREADY;
        }
        if (strcmp(e->key, key) == 0) {
            LOG_ERR("Persisted value '%s' is already registered", key);
            return -EEXIST;
        }
    }

    entry->key = key;
    entry->target = target;
    entry->load = load;
    entry->save = save;
    entry->dirty = false;
    sys_slist_append(&sRegistry, &entry->node);
    return 0;
}

int persistent_value_registry_dispatch_load(const char *name, size_t len, settings_read_cb read_cb,
                                            void *cb_arg) {
    PersistentValueRegistryEntry *e;
    SYS_SLIST_FOR_EACH_CONTAINER(&sRegistry, e, node) {
        const char *next = nullptr;
        if (!settings_name_steq(name, e->key, &next) || next) {
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

        e->load(e->target, buf, static_cast<size_t>(readLen));
        return 0;
    }

    return -ENOENT;
}

void persistent_value_registry_mark_dirty(const char *key) {
    PersistentValueRegistryEntry *e = findRegistryEntry(key);
    if (e != nullptr) {
        e->dirty = true;
    }
}

void persistent_value_registry_save_all() {
    PersistentValueRegistryEntry *e;
    SYS_SLIST_FOR_EACH_CONTAINER(&sRegistry, e, node) {
        if (!e->dirty) {
            continue;
        }
        e->dirty = false;
        e->save(e->target);
    }
}

void persistent_value_registry_reset() {
    // Caller-owned nodes; just drop them all from the list (their storage is untouched).
    sys_slist_init(&sRegistry);
}

size_t persistent_value_registry_count() {
    return sys_slist_len(&sRegistry);
}

#else  // !CONFIG_APP_PERSIST_BT_CONFIG

int persistent_value_registry_register(PersistentValueRegistryEntry *, const char *, void *,
                                       PersistentValueLoadFn, PersistentValueSaveFn) {
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
