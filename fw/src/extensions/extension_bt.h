#pragma once

#include <cstddef>

/**
 * @brief Builds and registers the runtime GATT service for extension slot
 * `slot` (issue #85 "first-class animations over BLE"): a per-extension
 * service whose UUID follows BT_ANIMATION_SERVICE_UUID(kAnimationIdBase +
 * slot), carrying the standard Animation Name and Is Active (+notify)
 * characteristics plus one read/write characteristic per manifest parameter
 * (UINT32/COLOR/BOOL/STRING, each with the matching CPF the built-ins use).
 * Unlike the built-in animations' compile-time BtGattServer templates, these
 * attribute tables are filled in at runtime from preallocated pools and
 * registered with bt_gatt_service_register() (CONFIG_BT_GATT_DYNAMIC_DB).
 *
 * These services also carry a bulk-metadata characteristic (issue #90
 * follow-up), built at runtime by extension_metadata_blob.h and byte-for-byte
 * compatible with the compile-time equivalent built-in services get (see
 * bt_service_cpp.h's MetadataBlobBuilder) — so the companion app's existing
 * fast-discovery path (app/CLAUDE.md) picks these services up automatically,
 * with no fallback to per-characteristic CUD/CPF descriptor reads needed.
 *
 * @return 0 on success, negative errno on failure (slot out of range, not
 *         loaded, already registered, or bt_gatt_service_register error).
 */
int extension_bt_register(size_t slot);

/**
 * @brief Unregisters the GATT service registered by extension_bt_register()
 * — the rollback path when a later registration step for the same slot
 * fails. No-op if the slot isn't registered.
 */
void extension_bt_unregister(size_t slot);

/**
 * @brief Binds the slot's Is Active mirror/notify setter into the animation
 * registry. Must be called AFTER extension_animation_proxy_register() — the
 * registry rejects setters for ids that have no entry yet (the proxy's
 * registration creates it).
 *
 * @return 0 on success, negative errno otherwise.
 */
int extension_bt_bind_is_active(size_t slot);
