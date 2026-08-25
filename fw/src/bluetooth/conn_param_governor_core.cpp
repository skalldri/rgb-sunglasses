#include "conn_param_governor_core.h"

namespace conn_param_governor_core {

const char *param_set_to_string(ParamSet set) {
    switch (set) {
        case ParamSet::NONE:
            return "NONE";
        case ParamSet::FAST:
            return "FAST";
        case ParamSet::MEDIUM:
            return "MEDIUM";
        case ParamSet::SLOW:
            return "SLOW";
    }
    return "UNKNOWN";
}

ParamSet Governor::desired(Trigger trigger, const Inputs &in) const {
    (void)trigger;

    // A DFU upload gets the fast set unconditionally - transfer time at a slow
    // interval would be dreadful, and the mgmt hooks give us exact start/stop
    // edges to key off.
    if (dfu_active_) {
        return ParamSet::FAST;
    }

    // Fresh connection: fast for the app's discovery walk (issue #41).
    if (target_ == ParamSet::NONE) {
        return ParamSet::FAST;
    }

    // An active telemetry stream holds the link up for as long as it runs. This sits
    // below DFU (a firmware update still wins) and below the discovery fast path, but
    // ABOVE the idle clock - the whole point is that outbound notifies do not refresh
    // that clock, so without this a live meter would sink to SLOW and start arriving in
    // 165 ms clumps while the user is dragging a slider against it.
    if (stream_active_) {
        return stream_rate_hz_ >= kStreamFastRateHz ? ParamSet::FAST : ParamSet::MEDIUM;
    }

    // Clamp a last-activity stamp from before the connect (or a torn/unset
    // one) to "just now" rather than letting it look like ancient idleness.
    int64_t idle_ms = in.now_ms - in.last_activity_ms;
    if (idle_ms < 0) {
        idle_ms = 0;
    }

    // Activity boost is DATA-driven (recent activity while SLOW), not
    // trigger-driven: a spacing-deferred boost must survive being recomputed
    // when the deferral timer lands (desired() is recomputed, never replayed).
    // This can't fire spuriously from a stale timer because steady SLOW never
    // arms one - the only TIMER steps seen while SLOW are deferrals.
    if (target_ == ParamSet::SLOW && (uint64_t)idle_ms < config_.boost_hold_ms) {
        return ParamSet::MEDIUM;
    }

    if (target_ == ParamSet::FAST && (uint64_t)idle_ms >= config_.idle_timeout_ms) {
        return ParamSet::SLOW;
    }

    if (target_ == ParamSet::MEDIUM && (uint64_t)idle_ms >= config_.boost_hold_ms) {
        return ParamSet::SLOW;
    }

    return target_;
}

Decision Governor::step(Trigger trigger, const Inputs &in) {
    switch (trigger) {
        case Trigger::CONNECTED:
            connected_ = true;
            dfu_active_ = false;
            stream_active_ = false;
            stream_rate_hz_ = 0;
            target_ = ParamSet::NONE;
            // First request after a connect must never be spacing-deferred.
            last_request_ms_ = INT64_MIN;
            break;

        case Trigger::DISCONNECTED:
            connected_ = false;
            dfu_active_ = false;
            // A stream cannot survive the link it was streaming over. Leaving this
            // set would resurrect the hold on the next connect, pinning the radio
            // fast for a subscriber that no longer exists.
            stream_active_ = false;
            stream_rate_hz_ = 0;
            target_ = ParamSet::NONE;
            last_request_ms_ = INT64_MIN;
            return {ParamSet::NONE, nullptr, 0};

        case Trigger::DFU_STARTED:
            dfu_active_ = true;
            break;

        case Trigger::DFU_STOPPED:
            dfu_active_ = false;
            break;

        case Trigger::STREAM_STARTED:
            stream_active_ = true;
            break;

        case Trigger::STREAM_STOPPED:
            stream_active_ = false;
            stream_rate_hz_ = 0;
            break;

        case Trigger::ACTIVITY:
        case Trigger::TIMER:
            break;
    }

    if (!connected_) {
        return {ParamSet::NONE, nullptr, 0};
    }

    const ParamSet want = desired(trigger, in);

    // Timer needed after this step: while FAST/MEDIUM (and no DFU pinning us
    // fast), the pending idle downgrade needs a wake-up at the moment the
    // quiet window expires. Steady SLOW needs no timer - only an external
    // event (activity, DFU, disconnect) can change the state.
    const auto downgrade_timer_ms = [&](ParamSet from) -> uint32_t {
        if (dfu_active_ || stream_active_) {
            // Pinned by an explicit hold: the next change can only come from the
            // matching STOPPED edge (or a disconnect), never from the idle clock.
            return 0;
        }
        uint64_t window;
        if (from == ParamSet::FAST) {
            window = config_.idle_timeout_ms;
        } else if (from == ParamSet::MEDIUM) {
            window = config_.boost_hold_ms;
        } else {
            return 0;
        }
        int64_t idle_ms = in.now_ms - in.last_activity_ms;
        if (idle_ms < 0) {
            idle_ms = 0;
        }
        // desired() already downgraded if the window fully elapsed, so
        // remaining is positive here; clamp defensively anyway.
        const int64_t remaining = (int64_t)window - idle_ms;
        return remaining > 0 ? (uint32_t)remaining : 1;
    };

    if (want == target_) {
        return {ParamSet::NONE, nullptr, downgrade_timer_ms(target_)};
    }

    // Edge transition - apply request-rate bound. A too-soon transition is
    // deferred (next_eval re-derives it from live state; nothing is queued).
    // The sentinel check must come BEFORE the subtraction: now - INT64_MIN is
    // signed overflow (UB), and the sentinel is live on every fresh connect.
    if (last_request_ms_ != INT64_MIN) {
        const int64_t since_last_request = in.now_ms - last_request_ms_;
        if (since_last_request >= 0 &&
            (uint64_t)since_last_request < config_.min_request_spacing_ms) {
            return {ParamSet::NONE, nullptr,
                    (uint32_t)(config_.min_request_spacing_ms - (uint64_t)since_last_request)};
        }
    }

    const char *reason;
    if (want == ParamSet::FAST) {
        reason = dfu_active_ ? "SMP DFU boost"
                             : (stream_active_ ? "telemetry stream" : "connect/discovery");
    } else if (want == ParamSet::MEDIUM) {
        reason = stream_active_ ? "telemetry stream" : "activity boost";
    } else {
        reason = "idle downgrade";
    }

    target_ = want;
    last_request_ms_ = in.now_ms;
    return {want, reason, downgrade_timer_ms(want)};
}

}  // namespace conn_param_governor_core
