#include <zephyr/ztest.h>

#include "bluetooth/conn_param_governor_core.h"

using namespace conn_param_governor_core;

// Mirror the production defaults (fw/Kconfig APP_BT_CONN_* symbols); the tests
// only rely on the relationships (spacing < hold < idle), not exact values.
static constexpr uint32_t kIdleMs = 20000;
static constexpr uint32_t kHoldMs = 10000;
static constexpr uint32_t kSpacingMs = 2000;

static Config test_config() {
    return Config{
        .idle_timeout_ms = kIdleMs,
        .boost_hold_ms = kHoldMs,
        .min_request_spacing_ms = kSpacingMs,
    };
}

static Inputs at(int64_t now_ms, int64_t last_activity_ms) {
    return Inputs{.now_ms = now_ms, .last_activity_ms = last_activity_ms};
}

// Drive a fresh governor to steady SLOW: connect at t=0 (activity at t=0),
// then let the idle window expire. Returns the time of the SLOW request.
static int64_t settle_to_slow(Governor &gov) {
    Decision d = gov.step(Trigger::CONNECTED, at(0, 0));
    zassert_equal(d.request, ParamSet::FAST, "connect must request FAST");
    d = gov.step(Trigger::TIMER, at(kIdleMs, 0));
    zassert_equal(d.request, ParamSet::SLOW, "idle window must downgrade to SLOW");
    return kIdleMs;
}

ZTEST(conn_param_governor, test_connect_requests_fast) {
    Governor gov(test_config());
    Decision d = gov.step(Trigger::CONNECTED, at(1000, 1000));

    zassert_equal(d.request, ParamSet::FAST);
    zassert_equal(gov.target(), ParamSet::FAST);
    zassert_not_null(d.reason);
    zassert_mem_equal(d.reason, "connect/discovery", sizeof("connect/discovery"));
    // The pending idle downgrade needs a timer.
    zassert_equal(d.next_eval_in_ms, kIdleMs);
}

ZTEST(conn_param_governor, test_discovery_activity_holds_fast) {
    Governor gov(test_config());
    gov.step(Trigger::CONNECTED, at(0, 0));

    // Discovery reads keep refreshing the idle clock: repeated steps with
    // fresh activity never emit a request (edge, not level) and keep the
    // downgrade timer pushed out.
    for (int64_t t = 1000; t <= 30000; t += 1000) {
        Decision d = gov.step(Trigger::ACTIVITY, at(t, t));
        zassert_equal(d.request, ParamSet::NONE, "steady FAST must not re-request");
        zassert_equal(d.next_eval_in_ms, kIdleMs, "fresh activity re-arms the full window");
    }
    zassert_equal(gov.target(), ParamSet::FAST);
}

ZTEST(conn_param_governor, test_idle_downgrade_to_slow) {
    Governor gov(test_config());
    gov.step(Trigger::CONNECTED, at(0, 0));

    // Timer fires before the window has elapsed: no change, timer re-armed
    // with the remaining time.
    Decision d = gov.step(Trigger::TIMER, at(kIdleMs - 5000, 0));
    zassert_equal(d.request, ParamSet::NONE);
    zassert_equal(d.next_eval_in_ms, 5000u);

    d = gov.step(Trigger::TIMER, at(kIdleMs, 0));
    zassert_equal(d.request, ParamSet::SLOW);
    zassert_mem_equal(d.reason, "idle downgrade", sizeof("idle downgrade"));
    // Steady SLOW needs no timer - only an event can change it.
    zassert_equal(d.next_eval_in_ms, 0u);
}

ZTEST(conn_param_governor, test_activity_boost_and_sliding_hold) {
    Governor gov(test_config());
    int64_t t = settle_to_slow(gov);

    // First inbound op after the downgrade boosts to MEDIUM...
    int64_t act = t + 60000;
    Decision d = gov.step(Trigger::ACTIVITY, at(act, act));
    zassert_equal(d.request, ParamSet::MEDIUM);
    zassert_mem_equal(d.reason, "activity boost", sizeof("activity boost"));
    zassert_equal(d.next_eval_in_ms, kHoldMs);

    // ...continued activity slides the hold window (no new requests)...
    for (int i = 1; i <= 5; i++) {
        int64_t tick = act + i * 3000;
        d = gov.step(Trigger::ACTIVITY, at(tick, tick));
        zassert_equal(d.request, ParamSet::NONE, "steady MEDIUM must not re-request");
        zassert_equal(d.next_eval_in_ms, kHoldMs);
    }

    // ...and a full quiet hold window drops back to SLOW.
    int64_t last_act = act + 5 * 3000;
    d = gov.step(Trigger::TIMER, at(last_act + kHoldMs, last_act));
    zassert_equal(d.request, ParamSet::SLOW);
    zassert_mem_equal(d.reason, "idle downgrade", sizeof("idle downgrade"));
}

ZTEST(conn_param_governor, test_dfu_boost_from_slow_and_recovery) {
    Governor gov(test_config());
    int64_t t = settle_to_slow(gov);

    int64_t start = t + 60000;
    Decision d = gov.step(Trigger::DFU_STARTED, at(start, t));
    zassert_equal(d.request, ParamSet::FAST);
    zassert_mem_equal(d.reason, "SMP DFU boost", sizeof("SMP DFU boost"));
    zassert_true(gov.dfuActive());
    // No downgrade timer while the DFU pins us fast.
    zassert_equal(d.next_eval_in_ms, 0u);

    // Upload chunks count as activity; none of them re-request.
    for (int i = 1; i <= 10; i++) {
        int64_t tick = start + i * 500;
        d = gov.step(Trigger::ACTIVITY, at(tick, tick));
        zassert_equal(d.request, ParamSet::NONE);
    }

    // DFU end: still FAST (no request), normal idle clock takes over.
    int64_t stop = start + 6000;
    d = gov.step(Trigger::DFU_STOPPED, at(stop, stop));
    zassert_equal(d.request, ParamSet::NONE, "FAST held across DFU end is not an edge");
    zassert_false(gov.dfuActive());
    zassert_equal(d.next_eval_in_ms, kIdleMs);

    d = gov.step(Trigger::TIMER, at(stop + kIdleMs, stop));
    zassert_equal(d.request, ParamSet::SLOW);
}

ZTEST(conn_param_governor, test_dfu_during_fast_is_not_an_edge) {
    Governor gov(test_config());
    gov.step(Trigger::CONNECTED, at(0, 0));

    Decision d = gov.step(Trigger::DFU_STARTED, at(1000, 1000));
    zassert_equal(d.request, ParamSet::NONE, "already FAST - a DFU start must not re-request");
    zassert_true(gov.dfuActive());

    // Even with the idle window long expired, DFU keeps FAST pinned.
    d = gov.step(Trigger::TIMER, at(100000, 1000));
    zassert_equal(d.request, ParamSet::NONE);
    zassert_equal(gov.target(), ParamSet::FAST);
}

ZTEST(conn_param_governor, test_spacing_defers_and_recomputes_boost) {
    Governor gov(test_config());
    int64_t slow_at = settle_to_slow(gov);

    // SLOW was requested at slow_at; activity lands inside the spacing window.
    int64_t act = slow_at + 500;
    Decision d = gov.step(Trigger::ACTIVITY, at(act, act));
    zassert_equal(d.request, ParamSet::NONE, "boost inside the spacing window must defer");
    zassert_equal(d.next_eval_in_ms, kSpacingMs - 500, "deferral timer = spacing remainder");
    zassert_equal(gov.target(), ParamSet::SLOW, "target unchanged while deferred");

    // The deferral timer lands: the boost is RE-DERIVED from live state (the
    // activity is still recent), not replayed - and now clears spacing.
    int64_t timer_at = slow_at + kSpacingMs;
    d = gov.step(Trigger::TIMER, at(timer_at, act));
    zassert_equal(d.request, ParamSet::MEDIUM, "deferred boost must survive recomputation");
    zassert_mem_equal(d.reason, "activity boost", sizeof("activity boost"));
}

ZTEST(conn_param_governor, test_spacing_deferred_boost_expires_if_activity_stale) {
    Governor gov(test_config());
    int64_t slow_at = settle_to_slow(gov);

    int64_t act = slow_at + 500;
    Decision d = gov.step(Trigger::ACTIVITY, at(act, act));
    zassert_equal(d.request, ParamSet::NONE);

    // If by the time the deferral lands the activity has gone stale past the
    // hold window (pathological, but possible if the timer is starved), the
    // recomputation correctly decides SLOW-stays-SLOW instead of boosting.
    d = gov.step(Trigger::TIMER, at(act + kHoldMs + 1, act));
    zassert_equal(d.request, ParamSet::NONE, "stale activity must not boost");
    zassert_equal(gov.target(), ParamSet::SLOW);
}

ZTEST(conn_param_governor, test_disconnect_resets_and_reconnect_requests_fresh) {
    Governor gov(test_config());
    int64_t t = settle_to_slow(gov);

    Decision d = gov.step(Trigger::DISCONNECTED, at(t + 100, t));
    zassert_equal(d.request, ParamSet::NONE);
    zassert_equal(d.next_eval_in_ms, 0u);
    zassert_false(gov.connected());
    zassert_equal(gov.target(), ParamSet::NONE);

    // Reconnect immediately (well inside what would be the spacing window):
    // the fresh connection's FAST request must never be deferred.
    d = gov.step(Trigger::CONNECTED, at(t + 200, t + 200));
    zassert_equal(d.request, ParamSet::FAST);
}

ZTEST(conn_param_governor, test_events_while_disconnected_are_ignored) {
    Governor gov(test_config());

    zassert_equal(gov.step(Trigger::ACTIVITY, at(1000, 1000)).request, ParamSet::NONE);
    zassert_equal(gov.step(Trigger::TIMER, at(2000, 1000)).request, ParamSet::NONE);
    zassert_equal(gov.step(Trigger::DFU_STARTED, at(3000, 1000)).request, ParamSet::NONE);
    zassert_equal(gov.target(), ParamSet::NONE);
}

ZTEST(conn_param_governor, test_negative_idle_clamped) {
    Governor gov(test_config());
    gov.step(Trigger::CONNECTED, at(1000, 1000));

    // last_activity ahead of now (torn read in the wiring): treated as "just
    // now", so no bogus instant downgrade.
    Decision d = gov.step(Trigger::TIMER, at(2000, 5000));
    zassert_equal(d.request, ParamSet::NONE);
    zassert_equal(gov.target(), ParamSet::FAST);
    zassert_equal(d.next_eval_in_ms, kIdleMs);
}

ZTEST_SUITE(conn_param_governor, NULL, NULL, NULL, NULL, NULL);
