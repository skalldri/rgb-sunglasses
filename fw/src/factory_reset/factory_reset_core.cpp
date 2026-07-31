#include "factory_reset_core.h"

namespace factory_reset_core {

bool flash_led_on(const HoldConfig& cfg, uint32_t elapsed_ms) {
    return (elapsed_ms / cfg.flash_half_period_ms) % 2 == 0;
}

namespace {

/* One phase of the hold: poll until the chord is released (returns false) or
 * duration_ms has elapsed (returns true). The phase-local elapsed clock makes
 * every phase start in its ON half-cycle, so a phase transition is always a
 * visible color change. A zero duration is "held" trivially — no LED write,
 * no sleep — which collapses phase 2 into the legacy single-phase behavior
 * when CONFIG_APP_FACTORY_RESET_PHASE2_HOLD_MS is 0. */
bool run_phase(const HoldConfig& cfg, const HoldIo& io, uint32_t duration_ms,
               LedState phase_on) {
    if (duration_ms == 0) {
        return true;
    }

    // The LEDs start ON: flash_led_on(0) is true, and the chord is known held
    // at phase entry (the caller's precondition for phase 1, the previous
    // phase's deadline sample for phase 2).
    bool leds_on = true;
    io.set_leds(io.ctx, phase_on);

    uint32_t elapsed_ms = 0;
    while (true) {
        io.sleep_ms(io.ctx, cfg.poll_interval_ms);
        elapsed_ms += cfg.poll_interval_ms;

        if (!io.chord_held(io.ctx)) {
            return false;
        }
        if (elapsed_ms >= duration_ms) {
            return true;  // held for the full phase duration
        }

        bool want_on = flash_led_on(cfg, elapsed_ms);
        if (want_on != leds_on) {
            leds_on = want_on;
            io.set_leds(io.ctx, want_on ? phase_on : LedState::Off);
        }
    }
}

}  // namespace

Decision run_hold_loop(const HoldConfig& cfg, const HoldIo& io) {
    Decision decision;
    if (!run_phase(cfg, io, cfg.hold_duration_ms, LedState::Phase1)) {
        decision = Decision::ContinueBoot;
    } else if (!run_phase(cfg, io, cfg.phase2_hold_ms, LedState::Phase2)) {
        decision = Decision::SettingsReset;
    } else {
        decision = Decision::FullReset;
    }

    // Always leave the LEDs off — the caller either resumes a normal boot or
    // repaints them (solid, in the phase color) for the erase.
    io.set_leds(io.ctx, LedState::Off);
    return decision;
}

int perform_reset(const ResetOps& ops) {
    int first_err = 0;

    const auto run_step = [&first_err](int (*step)()) {
        if (step == nullptr) {
            return;  // feature absent from this build: skipped as success
        }
        int rc = step();
        if (rc != 0 && first_err == 0) {
            first_err = rc;
        }
    };

    // Settings first — it is the essential factory-reset payload (all app
    // config + BT bonds); a later failure or hang must not leave it intact.
    run_step(ops.erase_settings);
    run_step(ops.erase_coredump);
    run_step(ops.reformat_fat);

    return first_err;
}

}  // namespace factory_reset_core
