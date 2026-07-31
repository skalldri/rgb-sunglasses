#include <zephyr/ztest.h>

#include <cerrno>
#include <cstdint>
#include <vector>

#include <factory_reset/factory_reset_core.h>

using factory_reset_core::Decision;
using factory_reset_core::flash_led_on;
using factory_reset_core::HoldConfig;
using factory_reset_core::HoldIo;
using factory_reset_core::LedState;
using factory_reset_core::perform_reset;
using factory_reset_core::ResetOps;
using factory_reset_core::run_hold_loop;

namespace {

constexpr HoldConfig kCfg = {
    .hold_duration_ms = 10000,
    .phase2_hold_ms = 10000,
    .poll_interval_ms = 20,
    .flash_half_period_ms = 100,
};

constexpr uint32_t kPhase1DeadlineMs = kCfg.hold_duration_ms;
constexpr uint32_t kPhase2DeadlineMs = kCfg.hold_duration_ms + kCfg.phase2_hold_ms;

/* ---- Fake HoldIo --------------------------------------------------------
 * A virtual clock advanced by sleep_ms; the chord "releases" at a scripted
 * time; every set_leds call is logged with its timestamp. */
struct FakeHold {
    uint32_t now_ms = 0;
    uint32_t release_at_ms = UINT32_MAX;  // UINT32_MAX = held forever
    struct LedEvent {
        uint32_t at_ms;
        LedState state;
    };
    std::vector<LedEvent> led_log;
};

bool fake_chord_held(void* ctx) {
    auto* f = static_cast<FakeHold*>(ctx);
    return f->now_ms < f->release_at_ms;
}

void fake_set_leds(void* ctx, LedState state) {
    auto* f = static_cast<FakeHold*>(ctx);
    f->led_log.push_back({f->now_ms, state});
}

void fake_sleep_ms(void* ctx, uint32_t ms) {
    static_cast<FakeHold*>(ctx)->now_ms += ms;
}

HoldIo make_io(FakeHold& f) {
    return HoldIo{
        .chord_held = fake_chord_held,
        .set_leds = fake_set_leds,
        .sleep_ms = fake_sleep_ms,
        .ctx = &f,
    };
}

/* Which flash state (phase color or Off) the loop should have commanded at a
 * phase-local elapsed time — mirrors run_phase's LED cadence. */
LedState expected_flash_state(uint32_t phase_elapsed_ms, LedState phase_on) {
    return flash_led_on(kCfg, phase_elapsed_ms) ? phase_on : LedState::Off;
}

/* ---- Fake ResetOps ------------------------------------------------------
 * ResetOps function pointers take no context, so the fake's state is static;
 * reset in the suite's before() hook (same idiom as tests/debug/coredump_manager). */
struct FakeReset {
    std::vector<char> call_order;  // 's' = settings, 'c' = coredump, 'f' = fat
    int settings_rc = 0;
    int coredump_rc = 0;
    int fat_rc = 0;
};
FakeReset sReset;

int fake_erase_settings() {
    sReset.call_order.push_back('s');
    return sReset.settings_rc;
}

int fake_erase_coredump() {
    sReset.call_order.push_back('c');
    return sReset.coredump_rc;
}

int fake_reformat_fat() {
    sReset.call_order.push_back('f');
    return sReset.fat_rc;
}

constexpr ResetOps kAllOps = {
    .erase_settings = fake_erase_settings,
    .erase_coredump = fake_erase_coredump,
    .reformat_fat = fake_reformat_fat,
};

}  // namespace

static void before_hook(void*) {
    sReset = FakeReset{};
}

ZTEST_SUITE(factory_reset_core, NULL, NULL, before_hook, NULL, NULL);

/* ---- flash_led_on -------------------------------------------------------- */

ZTEST(factory_reset_core, test_flash_led_on_boundaries) {
    zassert_true(flash_led_on(kCfg, 0));
    zassert_true(flash_led_on(kCfg, 99));
    zassert_false(flash_led_on(kCfg, 100));
    zassert_false(flash_led_on(kCfg, 199));
    zassert_true(flash_led_on(kCfg, 200));
}

/* ---- run_hold_loop: phase 1 (release = cancel) --------------------------- */

ZTEST(factory_reset_core, test_release_at_first_poll_continues_boot) {
    FakeHold f;
    f.release_at_ms = 0;  // released before the first poll

    zassert_equal(run_hold_loop(kCfg, make_io(f)), Decision::ContinueBoot);
    // The loop should bail on the very first sample, not keep polling.
    zassert_equal(f.now_ms, kCfg.poll_interval_ms);
}

ZTEST(factory_reset_core, test_release_midway_phase1_continues_boot) {
    FakeHold f;
    f.release_at_ms = 3000;

    zassert_equal(run_hold_loop(kCfg, make_io(f)), Decision::ContinueBoot);
    zassert_true(f.now_ms >= 3000);
    zassert_true(f.now_ms < kPhase1DeadlineMs);
}

ZTEST(factory_reset_core, test_release_just_before_phase1_deadline_continues_boot) {
    FakeHold f;
    f.release_at_ms = kPhase1DeadlineMs - kCfg.poll_interval_ms;

    zassert_equal(run_hold_loop(kCfg, make_io(f)), Decision::ContinueBoot);
    zassert_true(f.now_ms < kPhase1DeadlineMs + kCfg.poll_interval_ms);
}

ZTEST(factory_reset_core, test_release_exactly_at_phase1_deadline_continues_boot) {
    FakeHold f;
    // A release observed on the same poll as the phase-1 deadline must take
    // the less-destructive outcome: cancel, not SettingsReset.
    f.release_at_ms = kPhase1DeadlineMs;

    zassert_equal(run_hold_loop(kCfg, make_io(f)), Decision::ContinueBoot);
    zassert_equal(f.now_ms, kPhase1DeadlineMs);
}

/* ---- run_hold_loop: phase 2 (release = settings-only reset) -------------- */

ZTEST(factory_reset_core, test_release_at_first_phase2_poll_settings_reset) {
    FakeHold f;
    f.release_at_ms = kPhase1DeadlineMs + kCfg.poll_interval_ms;

    zassert_equal(run_hold_loop(kCfg, make_io(f)), Decision::SettingsReset);
    zassert_equal(f.now_ms, kPhase1DeadlineMs + kCfg.poll_interval_ms);
}

ZTEST(factory_reset_core, test_release_midway_phase2_settings_reset) {
    FakeHold f;
    f.release_at_ms = kPhase1DeadlineMs + kCfg.phase2_hold_ms / 2;

    zassert_equal(run_hold_loop(kCfg, make_io(f)), Decision::SettingsReset);
    zassert_true(f.now_ms >= f.release_at_ms);
    zassert_true(f.now_ms < kPhase2DeadlineMs);
}

ZTEST(factory_reset_core, test_release_exactly_at_phase2_deadline_settings_reset) {
    FakeHold f;
    // Same boundary rule as phase 1: release on the deadline poll wins.
    f.release_at_ms = kPhase2DeadlineMs;

    zassert_equal(run_hold_loop(kCfg, make_io(f)), Decision::SettingsReset);
    zassert_equal(f.now_ms, kPhase2DeadlineMs);
}

ZTEST(factory_reset_core, test_full_hold_performs_full_reset) {
    FakeHold f;  // held forever

    zassert_equal(run_hold_loop(kCfg, make_io(f)), Decision::FullReset);
    // The decision must land at the phase-2 deadline, not a poll interval late.
    zassert_equal(f.now_ms, kPhase2DeadlineMs);
}

ZTEST(factory_reset_core, test_zero_phase2_hold_is_legacy_single_phase) {
    // phase2_hold_ms == 0: the full reset fires at the phase-1 deadline and
    // Phase2 never appears in the LED log — the legacy one-phase behavior.
    HoldConfig cfg = kCfg;
    cfg.phase2_hold_ms = 0;
    FakeHold f;  // held forever

    zassert_equal(run_hold_loop(cfg, make_io(f)), Decision::FullReset);
    zassert_equal(f.now_ms, cfg.hold_duration_ms);
    for (const auto& ev : f.led_log) {
        zassert_not_equal(ev.state, LedState::Phase2,
                          "Phase2 LED event at %u with zero phase-2 window", ev.at_ms);
    }
}

/* ---- run_hold_loop: LED behavior ----------------------------------------- */

ZTEST(factory_reset_core, test_led_flash_cadence_both_phases_and_final_off) {
    FakeHold f;  // held forever: full 20 s of flashing across both phases

    run_hold_loop(kCfg, make_io(f));

    zassert_true(f.led_log.size() >= 4, "expected initial + toggles + boundary + final off");

    // Phase 1 starts ON in the phase-1 color at t=0.
    zassert_equal(f.led_log.front().at_ms, 0);
    zassert_equal(f.led_log.front().state, LedState::Phase1);

    // Always ends OFF.
    zassert_equal(f.led_log.back().state, LedState::Off);

    // Phase 2 starts ON in the phase-2 color exactly at the phase boundary —
    // the flash clock restarts, so the color change is always visible.
    bool boundary_seen = false;
    for (const auto& ev : f.led_log) {
        if (ev.at_ms == kPhase1DeadlineMs) {
            zassert_equal(ev.state, LedState::Phase2);
            boundary_seen = true;
        }
    }
    zassert_true(boundary_seen, "no LED event at the phase-1/phase-2 boundary");

    // Every intermediate event lands on a half-period boundary of its phase's
    // local clock and matches the pure flash_led_on helper, in that phase's
    // color; no Phase1 events after the boundary, no Phase2 before it.
    for (size_t i = 1; i + 1 < f.led_log.size(); i++) {
        const auto& ev = f.led_log[i];
        const bool in_phase2 = ev.at_ms >= kPhase1DeadlineMs;
        const uint32_t phase_elapsed =
            in_phase2 ? ev.at_ms - kPhase1DeadlineMs : ev.at_ms;
        const LedState phase_on = in_phase2 ? LedState::Phase2 : LedState::Phase1;

        zassert_equal(phase_elapsed % kCfg.flash_half_period_ms, 0,
                      "event at %u not on a phase-local half-period boundary", ev.at_ms);
        zassert_equal(ev.state, expected_flash_state(phase_elapsed, phase_on),
                      "wrong LED state at %u", ev.at_ms);
        // Consecutive events differ — no redundant set_leds calls.
        zassert_not_equal(ev.state, f.led_log[i - 1].state,
                          "redundant LED event at %u", ev.at_ms);
    }
}

ZTEST(factory_reset_core, test_leds_off_after_early_release) {
    FakeHold f;
    f.release_at_ms = 250;

    run_hold_loop(kCfg, make_io(f));
    zassert_equal(f.led_log.back().state, LedState::Off);
}

ZTEST(factory_reset_core, test_leds_off_after_phase2_release) {
    FakeHold f;
    f.release_at_ms = kPhase1DeadlineMs + 250;

    run_hold_loop(kCfg, make_io(f));
    zassert_equal(f.led_log.back().state, LedState::Off);
}

/* ---- perform_reset ------------------------------------------------------- */

ZTEST(factory_reset_core, test_reset_runs_all_ops_in_order) {
    zassert_equal(perform_reset(kAllOps), 0);
    zassert_equal(sReset.call_order.size(), 3u);
    zassert_equal(sReset.call_order[0], 's');
    zassert_equal(sReset.call_order[1], 'c');
    zassert_equal(sReset.call_order[2], 'f');
}

ZTEST(factory_reset_core, test_reset_failure_does_not_block_later_ops) {
    sReset.settings_rc = -EIO;

    zassert_equal(perform_reset(kAllOps), -EIO);
    // Coredump and FAT still ran despite the settings failure.
    zassert_equal(sReset.call_order.size(), 3u);
}

ZTEST(factory_reset_core, test_reset_returns_first_error) {
    sReset.coredump_rc = -EIO;
    sReset.fat_rc = -ENOSPC;

    zassert_equal(perform_reset(kAllOps), -EIO);
    zassert_equal(sReset.call_order.size(), 3u);
}

ZTEST(factory_reset_core, test_reset_null_ops_skipped_as_success) {
    // This ops table — settings erase only — is exactly what the glue passes
    // for a SettingsReset (soft reset), as well as the shape of a build with
    // coredump/FAT support absent.
    const ResetOps ops = {
        .erase_settings = fake_erase_settings,
        .erase_coredump = nullptr,
        .reformat_fat = nullptr,
    };

    zassert_equal(perform_reset(ops), 0);
    zassert_equal(sReset.call_order.size(), 1u);
    zassert_equal(sReset.call_order[0], 's');
}

ZTEST(factory_reset_core, test_reset_all_null_is_success) {
    const ResetOps ops = {nullptr, nullptr, nullptr};

    zassert_equal(perform_reset(ops), 0);
    zassert_true(sReset.call_order.empty());
}
