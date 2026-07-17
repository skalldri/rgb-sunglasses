#pragma once

#include <stdint.h>

/* Hardware-free logic for the boot-time factory reset (issue #162): the
 * button-hold detection loop and the erase-step sequencing. All side effects
 * (GPIO reads, LED writes, sleeping, flash erases) go through the HoldIo /
 * ResetOps seams so the logic runs on native_sim — same pattern as
 * coredump_manager_core::PartitionOps. The thin wiring to real GPIO / LED
 * strip / flash calls lives in factory_reset.cpp. */

namespace factory_reset_core {

enum class Decision {
    ContinueBoot,  // chord released before the hold duration elapsed
    PerformReset,  // chord held for the full duration
};

struct HoldConfig {
    uint32_t hold_duration_ms;      // how long the chord must be held (CONFIG_APP_FACTORY_RESET_HOLD_MS)
    uint32_t poll_interval_ms;      // how often to sample the chord
    uint32_t flash_half_period_ms;  // white-flash half period (on time == off time)
};

/* All side effects injected; ctx is passed through untouched. */
struct HoldIo {
    bool (*chord_held)(void* ctx);
    void (*set_leds)(void* ctx, bool on);
    void (*sleep_ms)(void* ctx, uint32_t ms);
    void* ctx;
};

/* Pure helper: is the flash "on" at a given elapsed time? On for the first
 * half period, off for the second, repeating. */
bool flash_led_on(const HoldConfig& cfg, uint32_t elapsed_ms);

/* Poll the chord until it is released (ContinueBoot) or hold_duration_ms has
 * elapsed (PerformReset). Precondition: the caller already sampled the chord
 * once and found it held. Flashes the LEDs via io.set_leds (called only on
 * state changes) and always leaves them OFF before returning. */
Decision run_hold_loop(const HoldConfig& cfg, const HoldIo& io);

/* The erase steps, in execution order. A null member means the feature is
 * absent from this build and is skipped as success. Each returns 0 on
 * success, negative errno on failure. */
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
