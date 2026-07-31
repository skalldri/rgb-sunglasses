#pragma once

#include <stdint.h>

/* Hardware-free logic for the boot-time factory reset (issues #162, #265): the
 * two-phase button-hold detection loop and the erase-step sequencing. All side
 * effects (GPIO reads, LED writes, sleeping, flash erases) go through the
 * HoldIo / ResetOps seams so the logic runs on native_sim — same pattern as
 * coredump_manager_core::PartitionOps. The thin wiring to real GPIO / LED
 * strip / flash calls lives in factory_reset.cpp.
 *
 * The settings erase runs eagerly at the phase-1/phase-2 boundary (via the
 * HoldIo::commit_phase1 seam, LEDs solid in the phase-1 color while it runs)
 * so the user sees each step commit in sequence: phase-1 flash -> solid
 * (settings erased) -> phase-2 flash -> solid (files erased). Release timing
 * still selects the same outcomes as a decide-then-erase design would. */

namespace factory_reset_core {

enum class Decision {
    ContinueBoot,   // released during phase 1: nothing erased
    SettingsReset,  // released during phase 2: settings already erased by
                    // commit_phase1 — the caller only needs to reboot
    FullReset,      // held through both phases: settings already erased by
                    // commit_phase1 — the caller erases coredump + FAT, then reboots
};

/* What the status LEDs should show. The hardware colors (Phase1 = white,
 * Phase2 = amber) live in factory_reset.cpp; the core only tracks the phase
 * so tests can assert the color *changes* without knowing the palette. */
enum class LedState : uint8_t {
    Off,
    Phase1,
    Phase2,
};

struct HoldConfig {
    uint32_t hold_duration_ms;      // phase 1: how long the chord must be held (CONFIG_APP_FACTORY_RESET_HOLD_MS)
    uint32_t phase2_hold_ms;        // phase 2 release window (CONFIG_APP_FACTORY_RESET_PHASE2_HOLD_MS); 0 = legacy single-phase full reset
    uint32_t commit_hold_ms;        // extra solid-Phase1 time after commit_phase1 returns, before the
                                    // phase-2 flash starts — pads the commit indication to a clearly
                                    // visible length (the erase itself can be under half a second)
    uint32_t poll_interval_ms;      // how often to sample the chord
    uint32_t flash_half_period_ms;  // flash half period (on time == off time), both phases
};

/* All side effects injected; ctx is passed through untouched. */
struct HoldIo {
    bool (*chord_held)(void* ctx);
    void (*set_leds)(void* ctx, LedState state);
    void (*sleep_ms)(void* ctx, uint32_t ms);
    /* The phase-1 payload (the settings erase). Called exactly once, at the
     * phase-1 deadline, with the LEDs already solid Phase1 — before the
     * phase-2 flash starts. Returns 0 on success, negative errno on failure;
     * on failure the commit_hold_ms pad is skipped so the solid dwell is a
     * genuine success confirmation, not shown for a failed erase. Null = no
     * payload (skipped as success). */
    int (*commit_phase1)(void* ctx);
    void* ctx;
};

/* Pure helper: is the flash "on" at a given phase-local elapsed time? On for
 * the first half period, off for the second, repeating. The elapsed clock
 * restarts at each phase boundary, so every phase starts in its ON half. */
bool flash_led_on(const HoldConfig& cfg, uint32_t elapsed_ms);

/* Poll the chord through up to two phases. Precondition: the caller already
 * sampled the chord once and found it held.
 *
 *  - Released during phase 1 (the first hold_duration_ms, LEDs flashing
 *    Phase1): ContinueBoot — nothing erased, commit_phase1 never called.
 *  - Phase 1 held to its deadline: LEDs go solid Phase1 and commit_phase1
 *    runs (the settings erase), then the phase-2 flash starts.
 *  - Released during phase 2 (the next phase2_hold_ms, LEDs flashing
 *    Phase2): SettingsReset — the phase-1 payload already ran.
 *  - Held through both phases: FullReset.
 *
 * A release observed on the same poll as a phase deadline takes the
 * less-destructive outcome. LEDs are driven via io.set_leds (called only on
 * state changes) and are always left Off before returning. */
Decision run_hold_loop(const HoldConfig& cfg, const HoldIo& io);

/* The erase steps, in execution order. A null member means the feature is
 * absent from this build (or excluded from this reset scope) and is skipped
 * as success. Each returns 0 on success, negative errno on failure. */
struct ResetOps {
    int (*erase_settings)();
    int (*erase_coredump)();
    int (*reformat_fat)();
};

/* Run ALL steps in order — a failed step never blocks the later ones (a
 * partial reset is still better than an aborted one, and every step is
 * independent). Returns 0 if everything succeeded, else the first error. */
int perform_reset(const ResetOps& ops);

}  // namespace factory_reset_core
