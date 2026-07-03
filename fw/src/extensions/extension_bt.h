#pragma once

#include <cstddef>

/**
 * Builds and registers the runtime GATT service for extension slot `slot`
 * (issue #85 "first-class animations over BLE"): a per-extension service
 * whose UUID follows BT_ANIMATION_SERVICE_UUID(0x20 + slot), carrying the
 * standard Animation Name and Is Active characteristics plus one read/write
 * characteristic per manifest parameter. Unlike the built-in animations'
 * compile-time BtGattServer templates, these attribute tables are filled in
 * at runtime from preallocated pools and registered with
 * bt_gatt_service_register() (CONFIG_BT_GATT_DYNAMIC_DB).
 *
 * These services carry no bulk-metadata characteristic; the companion app
 * automatically falls back to per-characteristic CUD/CPF descriptor reads
 * for services without one (see app/CLAUDE.md).
 */
void extension_bt_register(size_t slot);
