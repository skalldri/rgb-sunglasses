#pragma once

#include <sys/types.h>

#include <cstddef>

struct PersistentValueRegistryEntry;

/**
 * @brief Debounced flash persistence for values registered with
 * persistent_value_registry. BT-free by design: do not give this module a
 * dependency on the Bluetooth stack or its Kconfig (see CONFIG_APP_SETTINGS_SAVE_DEBOUNCE_MS
 * in fw/Kconfig, which is intentionally separate from CONFIG_BT_SETTINGS_DELAYED_STORE_MS).
 */
namespace persistent_value_store {

/** @brief Settings subtree every persisted key lives under. */
inline constexpr const char *kSubtreeName = "appcfg";

/**
 * @brief (Re)schedules a debounced flush of every registered persistent value.
 *
 * Coalesces rapid successive calls (e.g. a BLE client editing a text string or
 * dragging a color picker) into one flash write CONFIG_APP_SETTINGS_SAVE_DEBOUNCE_MS
 * after the last call. Safe to call from any context the BT GATT write callback or
 * shell command handlers run in.
 */
void request_save();

/**
 * @brief Immediately persists one value under the "appcfg/" subtree.
 *
 * Centralizes the subtree-prefix concatenation so individual PersistentValueSaveFn
 * implementations don't each duplicate it. Called from the debounced save sweep
 * (via persistent_value_registry_save_all()), never directly from a BT write path.
 *
 * @param key Same stable key passed to persistent_value_registry_register().
 */
void save_value(const char *key, const void *data, size_t len);

/**
 * @brief Immediately, synchronously reads one persisted value's raw bytes, if present.
 *
 * Unlike persistent_value_registry's dispatch_load path (which only fires during the
 * one boot-time settings_load() replay, for keys already registered before that replay
 * runs), this needs no prior registration - a direct settings_load_one() by exact key.
 * For callers whose key isn't known until after settings_load() has already completed
 * (e.g. extension_host, which only learns an extension's identity from its manifest at
 * FAT-discovery time, on the pattern-controller thread, well after bluetooth_init()'s
 * settings_load() has run).
 *
 * @param key Same stable key passed to persistent_value_registry_register()/save_value().
 * @return Number of bytes actually read (<= bufLen) on success, 0 if @p key was never
 *         saved, or a negative errno on a storage error.
 */
ssize_t load_value(const char *key, void *buf, size_t bufLen);

/**
 * @brief Immediately deletes one persisted value's record under "appcfg/".
 *
 * Safe from the persistence workqueue (the save sweep's own thread) — used by
 * save callbacks that clear a sibling key (e.g. the last-active id/name pair)
 * and by purge_value() below. A missing record is not an error.
 */
void delete_value(const char *key);

/**
 * @brief Synchronously unregisters @p entry and deletes its persisted record,
 * executed ON the persistence workqueue.
 *
 * The workqueue hop is the point: unregistration mutates the registry list
 * that save_all() walks, and a queued debounced flush would otherwise re-save
 * the record this call just deleted — running the whole purge as one work item
 * on that queue serializes it against both. Blocks until done.
 *
 * MUST NOT be called from the persistence workqueue itself (it would deadlock
 * waiting on its own queue) — callers are runtime management paths (the SMP
 * workqueue's extension DELETE handler). @p entry may be unregistered already
 * (its record is still deleted); a null @p key skips the record deletion.
 *
 * @return 0 on success (including "was not registered"), -EINVAL on null entry.
 */
int purge_value(PersistentValueRegistryEntry *entry, const char *key);

/**
 * @brief Synchronously cancels any pending debounced save.
 *
 * Waits for an already-running save sweep to finish before returning. Two callers:
 *
 * - factory_reset before erasing the settings partition — a queued save firing after
 *   the erase would resurrect the just-erased config (or write mid-erase).
 * - Test suites whose registry entries live in test-local (stack) storage, in their
 *   per-test teardown: a zassert failure longjmps out of the test with those entries
 *   still linked, and a still-pending save firing afterwards would traverse the dead
 *   stack frames.
 */
void cancel_pending_save();

}  // namespace persistent_value_store
