#pragma once

#include <stdbool.h>

/**
 * @brief Synchronizes BLE advertising start with pre-advertising boot-time
 * work (e.g. extension discovery/registration, issue #208), so a central
 * that connects right after advertising starts can never read GATT state
 * that boot-time work hasn't finished populating yet.
 *
 * BT-free by design (same idiom as BtStateObserver/ConfigurationProvider) so
 * it can be exercised on native_sim without linking the BT stack.
 */

/**
 * @brief Block the calling thread until boot_gate_notify_ready() has been
 * called, or until timeout_ms elapses.
 *
 * @param timeout_ms How long to wait before giving up.
 * @return true if notify_ready() was (or had already been) called; false if
 * the wait timed out first.
 */
bool boot_gate_wait_ready(int timeout_ms);

/**
 * @brief Signal that pre-advertising boot-time work has finished. Call
 * exactly once. Safe to call before anyone is waiting.
 */
void boot_gate_notify_ready();
