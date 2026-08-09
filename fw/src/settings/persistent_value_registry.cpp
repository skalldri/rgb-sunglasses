#include <errno.h>
#include <settings/persistent_value_registry.h>
#include <string.h>
#include <sys/types.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>

// When the feature is disabled (CONFIG_APP_PERSIST_BT_CONFIG=n, as on the legacy DK
// board on the dk-support branch), compile out the registry (and the log module that reports failures
// touching it) entirely rather than just leaving it unreferenced - every call site that
// registers an entry is itself gated by IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG), so these
// stubs are never invoked there.
#if defined(CONFIG_APP_PERSIST_BT_CONFIG)

LOG_MODULE_REGISTER(persistent_value_registry, CONFIG_LOG_DEFAULT_LEVEL);

namespace {
// Intrusive list of caller-owned PersistentValueRegistryEntry records (each links in via
// its embedded .node). No fixed capacity - append is O(1) and can never fail for lack of
// space, so registration cannot be silently dropped. Same idiom as Zephyr's own settings
// backend (settings_store.c: sys_slist_t settings_load_srcs).
//
// Locking: registration is still effectively single-threaded (static-init/boot), but
// unregister() runs at runtime on the persistence workqueue (extension DELETE purge)
// while mark_dirty() traverses the same list from arbitrary threads (BT RX via
// extension_host::setParamValue(), the shell) — an unlink concurrent with a traversal
// corrupts a bare sys_slist. sListLock covers every list-structure operation and
// traversal EXCEPT the two below that must stay lock-free because they invoke
// callbacks that block (flash I/O):
//  - save_all(): runs only on the persistence workqueue, the same queue unregister()
//    is confined to, so the list cannot change under it; mark_dirty() only flips the
//    word-sized dirty flag, which cannot corrupt the walk.
//  - dispatch_load(): boot-time only (settings_load replay + scan_slot's load_value),
//    strictly before any runtime unregister can exist.
sys_slist_t sRegistry = SYS_SLIST_STATIC_INIT(&sRegistry);
k_spinlock sListLock;

PersistentValueRegistryEntry *findRegistryEntryLocked(const char *key) {
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
    // every later traversal - refuse rather than corrupt). Logging happens after the
    // unlock — no blocking work under a spinlock.
    int ret = 0;
    k_spinlock_key_t lk = k_spin_lock(&sListLock);
    PersistentValueRegistryEntry *e;
    SYS_SLIST_FOR_EACH_CONTAINER(&sRegistry, e, node) {
        if (e == entry) {
            ret = -EALREADY;
            break;
        }
        if (strcmp(e->key, key) == 0) {
            ret = -EEXIST;
            break;
        }
    }
    if (ret == 0) {
        entry->key = key;
        entry->target = target;
        entry->load = load;
        entry->save = save;
        entry->dirty = false;
        sys_slist_append(&sRegistry, &entry->node);
    }
    k_spin_unlock(&sListLock, lk);

    if (ret == -EALREADY) {
        LOG_ERR("Entry for '%s' is already linked into the registry", key);
    } else if (ret == -EEXIST) {
        LOG_ERR("Persisted value '%s' is already registered", key);
    }
    return ret;
}

int persistent_value_registry_unregister(PersistentValueRegistryEntry *entry) {
    if (entry == nullptr) {
        return -EINVAL;
    }
    // sys_slist_find_and_remove returns whether the node was present, making
    // "never registered" (or already unregistered) a clean -ENOENT instead of
    // list corruption. sListLock guards the unlink against a concurrent
    // mark_dirty() traversal (BT RX thread); serialization against save_all()'s
    // callback-invoking walk comes from the persistence-workqueue-only calling
    // contract (see the header), not from this lock.
    int ret = 0;
    k_spinlock_key_t lk = k_spin_lock(&sListLock);
    if (sys_slist_find_and_remove(&sRegistry, &entry->node)) {
        entry->dirty = false;
    } else {
        ret = -ENOENT;
    }
    k_spin_unlock(&sListLock, lk);
    return ret;
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
    // Callable from any thread (BT RX, shell, workqueues): the walk holds
    // sListLock so a concurrent unregister() can't unlink a node out from
    // under the traversal. The walk is a handful of strcmp()s — fine under a
    // spinlock.
    K_SPINLOCK(&sListLock) {
        PersistentValueRegistryEntry *e = findRegistryEntryLocked(key);
        if (e != nullptr) {
            e->dirty = true;
        }
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

int persistent_value_registry_unregister(PersistentValueRegistryEntry *) {
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
