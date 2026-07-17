#include "factory_reset_core.h"

namespace factory_reset_core {

bool flash_led_on(const HoldConfig& cfg, uint32_t elapsed_ms) {
    return (elapsed_ms / cfg.flash_half_period_ms) % 2 == 0;
}

Decision run_hold_loop(const HoldConfig& cfg, const HoldIo& io) {
    uint32_t elapsed_ms = 0;
    // The LEDs start ON: flash_led_on(0) is true, and the chord was already
    // sampled held once before entry.
    bool leds_on = true;
    io.set_leds(io.ctx, true);

    Decision decision = Decision::PerformReset;

    while (true) {
        io.sleep_ms(io.ctx, cfg.poll_interval_ms);
        elapsed_ms += cfg.poll_interval_ms;

        if (!io.chord_held(io.ctx)) {
            decision = Decision::ContinueBoot;
            break;
        }
        if (elapsed_ms >= cfg.hold_duration_ms) {
            break;  // held for the full duration: PerformReset
        }

        bool want_on = flash_led_on(cfg, elapsed_ms);
        if (want_on != leds_on) {
            leds_on = want_on;
            io.set_leds(io.ctx, want_on);
        }
    }

    // Always leave the LEDs off — the caller either resumes a normal boot or
    // repaints them (solid white) for the erase phase.
    io.set_leds(io.ctx, false);
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
