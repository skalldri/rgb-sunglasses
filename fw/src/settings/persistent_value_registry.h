#pragma once

#include <zephyr/settings/settings.h>
#include <zephyr/sys/slist.h>

#include <cstddef>

/**
 * @brief BT-free registry mapping a stable settings key to a load/save target.
 *
 * Zephyr's settings subsystem dispatches one h_set() call per matching key found
 * during settings_load(), scoped to whichever subtree a SETTINGS_STATIC_HANDLER_DEFINE
 * was registered for. This registry lets many independent persisted values (one per
 * BT-settable config characteristic) share a single subtree handler: each value
 * self-registers a callback pair at static-init time, and the shared handler's h_set
 * dispatches into whichever entry's key matches.
 *
 * Storage is an intrusive singly-linked list (sys_slist_t) whose nodes are owned by the
 * *callers* - each registrant embeds a PersistentValueRegistryEntry in its own long-lived
 * object (a characteristic instance, an extension Slot, a file-scope static) and passes
 * its address. This is the same idiom Zephyr's own settings backend uses
 * (settings_store.c: sys_slist_t settings_load_srcs), and it means the registry has no
 * fixed capacity and can never drop a registration - there is no -ENOMEM path.
 */
using PersistentValueLoadFn = void (*)(void *target, const void *data, size_t len);
using PersistentValueSaveFn = void (*)(void *target);

/**
 * @brief One persisted value's registration record, owned by the caller.
 *
 * The caller fills @c key / @c target / @c load / @c save on a long-lived instance (static
 * duration, or a member of a static/singleton object) and passes its address to
 * persistent_value_registry_register(). @c dirty and @c node are registry-internal; the
 * caller does not touch them (register() zeroes @c dirty). The instance must outlive its
 * registration - it is linked into the registry by pointer, never copied.
 */
struct PersistentValueRegistryEntry {
    const char *key;
    void *target;
    PersistentValueLoadFn load;
    PersistentValueSaveFn save;
    bool dirty;
    sys_snode_t node;  // registry-internal intrusive list link
};

/**
 * @brief Registers one persisted value.
 *
 * @param entry Caller-owned, long-lived record with @c key (stable, WITHOUT the settings
 *              subtree prefix - e.g. "core/brightness", not "appcfg/core/brightness"),
 *              @c target, @c load, and @c save filled in. Linked by pointer, so it must
 *              outlive its registration.
 * @return 0 on success, -EINVAL on a null @p entry / key / load / save, -EEXIST on a
 *         duplicate key. (There is no capacity limit, so no -ENOMEM.)
 */
int persistent_value_registry_register(PersistentValueRegistryEntry *entry);

/**
 * @brief Dispatches a settings_load() callback to the matching registered entry.
 *
 * Intended to be called from the one shared SETTINGS_STATIC_HANDLER_DEFINE's h_set.
 *
 * @return 0 if a matching entry was found and loaded, -ENOENT if no entry matches
 *         @p name, -EINVAL if @p len exceeds the internal scratch buffer.
 */
int persistent_value_registry_dispatch_load(const char *name, size_t len, settings_read_cb read_cb,
                                             void *cb_arg);

/**
 * @brief Marks a registered entry as dirty so the next save_all() flushes it.
 *
 * Call this before request_save() when a value changes via a non-BLE path (e.g. a shell
 * setter) or from a custom onWrite() that bypasses BtGattPersistentCharacteristic. Does
 * nothing (and does not log) if the key is not found - safe to call speculatively.
 */
void persistent_value_registry_mark_dirty(const char *key);

/** @brief Calls the save callback for every entry that has been marked dirty, then clears
 *  the dirty flag. Entries that have not been marked dirty are skipped entirely. */
void persistent_value_registry_save_all();

/** @brief Test hook: clears the registry. */
void persistent_value_registry_reset();

/** @brief Test hook: number of currently registered entries. */
size_t persistent_value_registry_count();
