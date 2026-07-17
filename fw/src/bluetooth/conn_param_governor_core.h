#pragma once

#include <stdint.h>

/* BT-free decision core for the LE connection-parameter governor (issue #188).
 *
 * The firmware is both the GATT server and the SMP (MCUmgr) server, so it can
 * observe every signal that should drive connection-parameter policy on its
 * own - inbound ATT traffic (the app's discovery walk and characteristic
 * reads/writes), and DFU upload start/stop - with no app<->firmware protocol.
 * This matters because iOS offers the app no central-side API at all
 * (Apple QA1931): the peripheral's bt_conn_le_param_update() request is the
 * ONLY lever there, so the policy must live firmware-side. On Android the
 * app's requestConnectionPriority() calls are optional accelerators that move
 * in the same direction; the central always has final say either way.
 *
 * This file is pure logic (events + timestamps in, "which parameter set to
 * request" out) behind the same seam pattern as coredump_manager_core, so the
 * state machine is exercised on native_sim (tests/bluetooth/conn_param_governor)
 * without the BT stack. The thin wiring to bt_conn_le_param_update(), the
 * k_work_delayable timer, and the MCUmgr mgmt hooks lives in bluetooth.cpp.
 *
 * Policy (see the parameter-set definitions in bluetooth.cpp for the actual
 * values and the Apple accessory-guideline math):
 *  - CONNECTED            -> FAST   (discovery speed, issue #41 - unchanged)
 *  - idle >= idle_timeout -> SLOW   ("idle downgrade"; FAST holds through the
 *                                    whole discovery walk because every
 *                                    discovery read refreshes the idle clock)
 *  - inbound ATT/SMP activity while SLOW -> MEDIUM ("activity boost"), held a
 *    sliding boost_hold window, then back to SLOW
 *  - DFU upload in progress -> FAST regardless ("SMP DFU boost"); after the
 *    upload the normal idle clock takes it back down
 *  - outbound notifies deliberately do NOT count as activity - device-
 *    originated traffic queues fine at slow intervals and must not hold the
 *    link fast
 *
 * Anti-ping-pong: transitions fire on trigger EDGES only (a level never
 * re-requests), and any two requests are separated by at least
 * min_request_spacing_ms - a transition that fires sooner is deferred via
 * next_eval_in_ms and recomputed (not replayed) when the timer lands. The
 * governor only ever REQUESTS; the central decides. The wiring logs a one-shot
 * warning when the granted parameters differ from the last request and does
 * not re-request until the next edge.
 */

namespace conn_param_governor_core {

enum class ParamSet : uint8_t {
    NONE = 0,  // no connection / nothing requested yet
    FAST,
    MEDIUM,
    SLOW,
};

const char *param_set_to_string(ParamSet set);

enum class Trigger : uint8_t {
    CONNECTED,     // link reached the required security level
    DISCONNECTED,  // link dropped - resets all state
    ACTIVITY,      // inbound ATT op or non-DFU SMP command observed
    DFU_STARTED,   // MGMT_EVT_OP_IMG_MGMT_DFU_STARTED (or first upload chunk)
    DFU_STOPPED,   // MGMT_EVT_OP_IMG_MGMT_DFU_STOPPED / _PENDING
    TIMER,         // scheduled re-evaluation (next_eval_in_ms elapsed)
};

struct Config {
    uint32_t idle_timeout_ms;         // FAST -> SLOW after this much quiet
    uint32_t boost_hold_ms;           // MEDIUM -> SLOW after this much quiet
    uint32_t min_request_spacing_ms;  // lower bound between two requests
};

struct Inputs {
    int64_t now_ms;            // current uptime
    int64_t last_activity_ms;  // uptime of the newest inbound ATT/SMP op
};

struct Decision {
    // Set to request from the central now, or NONE if nothing should be sent
    // on this step (steady state, or a transition deferred by spacing).
    ParamSet request;
    // Human-readable trigger for the request log line; nullptr when request
    // is NONE.
    const char *reason;
    // Re-evaluate (Trigger::TIMER) after this many ms; 0 = no timer needed,
    // the next state change can only come from an external event.
    uint32_t next_eval_in_ms;
};

class Governor {
public:
    explicit Governor(const Config &config) : config_(config) {}

    // Feed one trigger through the state machine. Not thread-safe: the wiring
    // must serialize calls (bluetooth.cpp funnels everything through one
    // k_work_delayable handler plus the BT thread's connect/disconnect path).
    Decision step(Trigger trigger, const Inputs &in);

    // Introspection for the bt_state shell command, the grant-mismatch check
    // in le_param_updated(), and the tests.
    ParamSet target() const { return target_; }
    bool dfuActive() const { return dfu_active_; }
    bool connected() const { return connected_; }

private:
    // What the state machine wants given the current inputs; pure, no side
    // effects. Recomputed on every step so a spacing-deferred transition is
    // re-derived from live state instead of replayed stale.
    ParamSet desired(Trigger trigger, const Inputs &in) const;

    Config config_;
    ParamSet target_ = ParamSet::NONE;
    bool connected_ = false;
    bool dfu_active_ = false;
    // Uptime of the last actually-emitted request; INT64_MIN so the first
    // request after a connect is never spacing-deferred.
    int64_t last_request_ms_ = INT64_MIN;
};

}  // namespace conn_param_governor_core
