#pragma once

#include <stdbool.h>
#include <stdint.h>

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
 * that include bt_service_cpp.h without linking bluetooth.cpp (governor-off
 * builds like the legacy DK board's — dk-support branch — and native_sim GATT
 * test suites like tests/bluetooth/battery_service) never need the symbol at
 * link time. */
#if defined(CONFIG_APP_BT_CONN_PARAM_GOVERNOR)
void bt_conn_activity_note(void);
#else
static inline void bt_conn_activity_note(void) {}
#endif

/* Hold the link up for a device-originated telemetry stream.
 *
 * The one sanctioned exception to the "outbound notifies are not activity" rule above,
 * and it needs its own call precisely BECAUSE of that rule: a live meter is not traffic
 * that queues fine at a slow interval, it is useless if it arrives late. Pass the send
 * rate so the governor can tell a modest stream (MEDIUM is plenty, and far cheaper on
 * radio duty) from a high-rate burst that MEDIUM's 30-45 ms interval could not carry.
 *
 * Call on EDGES only — once when a stream starts and once when it stops — never per
 * frame. The hold is released by the matching `active=false` call, by a disconnect, or
 * by the service's own watchdog; it can never expire on its own, which is exactly why
 * the watchdog exists. */
#if defined(CONFIG_APP_BT_CONN_PARAM_GOVERNOR)
void bt_conn_stream_hold(bool active, uint8_t rate_hz);
#else
static inline void bt_conn_stream_hold(bool active, uint8_t rate_hz) {
    (void)active;
    (void)rate_hz;
}
#endif
