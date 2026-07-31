#include "factory_reset_core.h"

namespace factory_reset_core {

bool flash_led_on(const HoldConfig& cfg, uint32_t elapsed_ms) {
    return (elapsed_ms / cfg.flash_half_period_ms) % 2 == 0;
}

namespace {

/* Emit an LED change only when the state actually differs — keeps hardware
 * writes minimal and makes the LED event stream assertable in tests (no
 * redundant transitions anywhere, including across the phase boundary where
 * the solid commit color can coincide with the last flash state). */
void set_leds_tracked(const HoldIo& io, LedState& current, LedState want) {
    if (want != current) {
        current = want;
        io.set_leds(io.ctx, want);
    }
}

/* One phase of the hold: poll until the chord is released (returns false) or
 * duration_ms has elapsed (returns true). The phase-local elapsed clock makes
 * every phase start in its ON half-cycle, so a phase transition is always a
 * visible color change. A zero duration is "held" trivially — no LED write,
 * no sleep — which collapses phase 2 into the legacy single-phase behavior
 * when CONFIG_APP_FACTORY_RESET_PHASE2_HOLD_MS is 0. */
bool run_phase(const HoldConfig& cfg, const HoldIo& io, uint32_t duration_ms,
               LedState phase_on, LedState& led) {
    if (duration_ms == 0) {
        return true;
    }

    // The LEDs start ON: flash_led_on(0) is true, and the chord is known held
    // at phase entry (the caller's precondition for phase 1, the previous
    // phase's deadline sample for phase 2).
    set_leds_tracked(io, led, phase_on);

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

        set_leds_tracked(io, led,
                         flash_led_on(cfg, elapsed_ms) ? phase_on : LedState::Off);
    }
}

}  // namespace

Decision run_hold_loop(const HoldConfig& cfg, const HoldIo& io) {
    LedState led = LedState::Off;  // matches the hardware state at entry
    Decision decision;
    if (!run_phase(cfg, io, cfg.hold_duration_ms, LedState::Phase1, led)) {
        decision = Decision::ContinueBoot;
    } else {
        // Phase 1 committed: solid Phase1 while the settings erase runs, so
        // the user sees the step happen before the phase-2 flash begins.
        set_leds_tracked(io, led, LedState::Phase1);
        if (io.commit_phase1 != nullptr) {
            io.commit_phase1(io.ctx);
        }
        decision = run_phase(cfg, io, cfg.phase2_hold_ms, LedState::Phase2, led)
                       ? Decision::FullReset
                       : Decision::SettingsReset;
    }

    // Always leave the LEDs off — the caller either resumes a normal boot,
    // reboots, or repaints them (solid Phase2) for the phase-2 erase.
    set_leds_tracked(io, led, LedState::Off);
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
