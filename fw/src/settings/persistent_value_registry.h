#pragma once

#include <zephyr/settings/settings.h>

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
 */
using PersistentValueLoadFn = void (*)(void *target, const void *data, size_t len);
using PersistentValueSaveFn = void (*)(void *target);

/**
 * @brief Registers one persisted value.
 *
 * @param key Stable key, WITHOUT the settings subtree prefix (e.g. "core/brightness",
 *            not "appcfg/core/brightness"). Must remain stable across firmware
 *            versions/declaration reordering - never derive it from positional state.
 * @param target Opaque pointer passed back into @p load / @p save.
 * @param load Called with the raw bytes read back from flash for this key.
 * @param save Called when this value should be flushed to flash.
 * @return 0 on success, -EINVAL on a null argument, -EEXIST on a duplicate key,
 *         -ENOMEM if the fixed-size table is full.
 */
int persistent_value_registry_register(const char *key, void *target, PersistentValueLoadFn load,
                                        PersistentValueSaveFn save);

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

/** @brief Calls every registered entry's save callback. */
void persistent_value_registry_save_all();

/** @brief Test hook: clears the registry. */
void persistent_value_registry_reset();

/** @brief Test hook: number of currently registered entries. */
size_t persistent_value_registry_count();
