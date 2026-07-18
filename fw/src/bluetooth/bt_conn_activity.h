#pragma once

/* Inbound-activity note for the LE connection-parameter governor (issue #188).
 *
 * Call from every handler that services an INBOUND request from the central -
 * ATT reads/writes on app characteristics (bt_service_cpp.h funnels,
 * extension_bt.cpp handlers) and SMP commands (wired via the MCUmgr mgmt
 * hooks in bluetooth.cpp). Do NOT call for outbound notifies: device-
 * originated traffic queues fine at slow intervals and must not hold the
 * link's radio duty high.
 *
 * Cheap and callable from any thread context. Defined in bluetooth.cpp when
 * the governor is compiled in; otherwise an inline no-op RIGHT HERE, so TUs
 * that include bt_service_cpp.h without linking bluetooth.cpp (the DK build
 * with the governor off, and native_sim GATT test suites like
 * tests/bluetooth/battery_service) never need the symbol at link time. */
#if defined(CONFIG_APP_BT_CONN_PARAM_GOVERNOR)
void bt_conn_activity_note(void);
#else
static inline void bt_conn_activity_note(void) {}
#endif
