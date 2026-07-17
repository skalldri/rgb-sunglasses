#include <zephyr/ztest.h>

#include <cerrno>
#include <cstdint>
#include <vector>

#include <factory_reset/factory_reset_core.h>

using factory_reset_core::Decision;
using factory_reset_core::flash_led_on;
using factory_reset_core::HoldConfig;
using factory_reset_core::HoldIo;
using factory_reset_core::perform_reset;
using factory_reset_core::ResetOps;
using factory_reset_core::run_hold_loop;

namespace {

constexpr HoldConfig kCfg = {
    .hold_duration_ms = 10000,
    .poll_interval_ms = 20,
    .flash_half_period_ms = 100,
};

/* ---- Fake HoldIo --------------------------------------------------------
 * A virtual clock advanced by sleep_ms; the chord "releases" at a scripted
 * time; every set_leds call is logged with its timestamp. */
struct FakeHold {
    uint32_t now_ms = 0;
    uint32_t release_at_ms = UINT32_MAX;  // UINT32_MAX = held forever
    struct LedEvent {
        uint32_t at_ms;
        bool on;
    };
    std::vector<LedEvent> led_log;
};

bool fake_chord_held(void* ctx) {
    auto* f = static_cast<FakeHold*>(ctx);
    return f->now_ms < f->release_at_ms;
}

void fake_set_leds(void* ctx, bool on) {
    auto* f = static_cast<FakeHold*>(ctx);
    f->led_log.push_back({f->now_ms, on});
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

/* ---- run_hold_loop ------------------------------------------------------- */

ZTEST(factory_reset_core, test_release_at_first_poll_continues_boot) {
    FakeHold f;
    f.release_at_ms = 0;  // released before the first poll

    zassert_equal(run_hold_loop(kCfg, make_io(f)), Decision::ContinueBoot);
    // The loop should bail on the very first sample, not keep polling.
    zassert_equal(f.now_ms, kCfg.poll_interval_ms);
}

ZTEST(factory_reset_core, test_release_midway_continues_boot) {
    FakeHold f;
    f.release_at_ms = 3000;

    zassert_equal(run_hold_loop(kCfg, make_io(f)), Decision::ContinueBoot);
    zassert_true(f.now_ms >= 3000);
    zassert_true(f.now_ms < kCfg.hold_duration_ms);
}

ZTEST(factory_reset_core, test_release_just_before_deadline_continues_boot) {
    FakeHold f;
    f.release_at_ms = kCfg.hold_duration_ms - kCfg.poll_interval_ms;

    zassert_equal(run_hold_loop(kCfg, make_io(f)), Decision::ContinueBoot);
    zassert_true(f.now_ms < kCfg.hold_duration_ms + kCfg.poll_interval_ms);
}

ZTEST(factory_reset_core, test_full_hold_performs_reset) {
    FakeHold f;  // held forever

    zassert_equal(run_hold_loop(kCfg, make_io(f)), Decision::PerformReset);
    // The decision must land at the deadline, not a poll interval late.
    zassert_equal(f.now_ms, kCfg.hold_duration_ms);
}

ZTEST(factory_reset_core, test_led_flash_cadence_and_final_off) {
    FakeHold f;  // held forever: full 10 s of flashing

    run_hold_loop(kCfg, make_io(f));

    zassert_true(f.led_log.size() >= 3, "expected initial + toggles + final off");

    // Starts ON at t=0.
    zassert_equal(f.led_log.front().at_ms, 0);
    zassert_true(f.led_log.front().on);

    // Always ends OFF.
    zassert_false(f.led_log.back().on);

    // Every intermediate change lands on a half-period boundary and matches
    // the pure flash_led_on helper.
    for (size_t i = 1; i + 1 < f.led_log.size(); i++) {
        const auto& ev = f.led_log[i];
        zassert_equal(ev.at_ms % kCfg.flash_half_period_ms, 0,
                      "toggle at %u not on a half-period boundary", ev.at_ms);
        zassert_equal(ev.on, flash_led_on(kCfg, ev.at_ms));
        // Consecutive events alternate — no redundant set_leds calls.
        zassert_not_equal(ev.on, f.led_log[i - 1].on);
    }
}

ZTEST(factory_reset_core, test_leds_off_after_early_release) {
    FakeHold f;
    f.release_at_ms = 250;

    run_hold_loop(kCfg, make_io(f));
    zassert_false(f.led_log.back().on);
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
    const ResetOps ops = {
        .erase_settings = fake_erase_settings,
        .erase_coredump = nullptr,  // e.g. CONFIG_DEBUG_COREDUMP absent
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
