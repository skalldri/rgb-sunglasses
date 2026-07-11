#pragma once

#include <sys/types.h>

#include <cstddef>

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

}  // namespace persistent_value_store
