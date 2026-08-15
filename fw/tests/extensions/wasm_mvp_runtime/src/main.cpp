#include <animations/wasm_mvp_module.h>
#include <extensions/wasm_mvp_runtime.h>
#include <zephyr/ztest.h>

#include "wasm3_security_modules.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "cpptest_v2_module.h"

namespace {

constexpr uint32_t kExpectedCyan = 0x00ffff;
constexpr uint32_t kExpectedMagenta = 0xff00ff;

uint32_t wave8(uint32_t angle) {
    const uint8_t t = angle & 0xffu;
    const uint8_t half = t & 0x7fu;
    const uint32_t hump = static_cast<uint32_t>(half) * (127u - half) / 32u;
    return (t & 0x80u) ? 128u - hump : 128u + hump;
}

uint32_t cpptestReferencePixel(uint32_t timeMs, uint32_t color, bool invert, size_t x, size_t y) {
    const uint32_t red = (color >> 16u) & 0xffu;
    const uint32_t green = (color >> 8u) & 0xffu;
    const uint32_t blue = color & 0xffu;
    uint32_t value = (wave8(x * 13u + timeMs / 9u) + wave8(y * 23u + timeMs / 14u) +
                      wave8((x + y) * 11u + timeMs / 6u)) /
                     3u;
    if (invert) {
        value = 255u - value;
    }
    return ((red * value / 255u) << 16u) | ((green * value / 255u) << 8u) | (blue * value / 255u);
}

void expectV2Frame(const wasm_mvp_runtime::V2TickOutput& output, uint32_t timeMs, uint32_t color,
                   bool invert) {
    for (size_t y = 0; y < wasm_mvp_runtime::kV2Height; ++y) {
        for (size_t x = 0; x < wasm_mvp_runtime::kV2Width; ++x) {
            const size_t pixel = y * wasm_mvp_runtime::kV2Width + x;
            zassert_equal(output.pixels[pixel], cpptestReferencePixel(timeMs, color, invert, x, y),
                          "pixel %zu differs", pixel);
        }
    }
    zassert_true(output.goodMoment);
    zassert_true(output.arenaHighWater > 0);
    zassert_true(output.arenaHighWater <= CONFIG_APP_WASM3_MVP_HEAP_SIZE);
    zassert_true(output.cpuTimeUs <= CONFIG_APP_WASM3_MVP_CPU_BUDGET_MS * 1000u);
    zassert_true(output.wallTimeUs <= CONFIG_APP_WASM3_MVP_WALL_BACKSTOP_MS * 1000u);
    TC_PRINT("cpptest v2 tick: cpu %u us, wall %u us\n", output.cpuTimeUs, output.wallTimeUs);
}

K_THREAD_STACK_DEFINE(sConcurrentTickStack, 2048);
struct k_thread sConcurrentTickThread;

struct ConcurrentTickState {
    wasm_mvp_runtime::Result result = wasm_mvp_runtime::Result::RuntimeFailure;
    wasm_mvp_runtime::TickOutput output{0xdeadbeef, 0};
};

wasm_mvp_runtime::V2TickOutput sV2Output;

k_timepoint_t deadline() {
    return sys_timepoint_calc(K_MSEC(CONFIG_APP_WASM3_MVP_WALL_BACKSTOP_MS));
}

void expectGoodActivationAndTick(uint32_t elapsedMs, uint32_t expectedColor) {
    zassert_equal(wasm_mvp_runtime::start(kWasmMvpModule, sizeof(kWasmMvpModule), deadline()),
                  wasm_mvp_runtime::Result::Completed);
    wasm_mvp_runtime::TickOutput output;
    zassert_equal(wasm_mvp_runtime::tick(elapsedMs, deadline(), output),
                  wasm_mvp_runtime::Result::Completed);
    zassert_equal(output.color, expectedColor);
    zassert_true(output.arenaHighWater > 0);
    zassert_true(output.arenaHighWater <= CONFIG_APP_WASM3_MVP_HEAP_SIZE);
    wasm_mvp_runtime::stop();
}

std::array<uint8_t, sizeof(kWasmMvpModule)> moduleCopy() {
    std::array<uint8_t, sizeof(kWasmMvpModule)> copy{};
    for (size_t i = 0; i < copy.size(); ++i) {
        copy[i] = kWasmMvpModule[i];
    }
    return copy;
}

std::vector<uint8_t> moduleVector() {
    return std::vector<uint8_t>(kWasmMvpModule, kWasmMvpModule + sizeof(kWasmMvpModule));
}

void insertBytes(std::vector<uint8_t>& module, size_t offset,
                 std::initializer_list<uint8_t> bytes) {
    module.insert(module.begin() + static_cast<std::ptrdiff_t>(offset), bytes);
}

std::array<uint8_t, sizeof(kWasmMvpModule)> infiniteLoopModule() {
    auto module = moduleCopy();
    // loop { br 0 }, followed by unreachable nops to preserve the body size.
    module[58] = 0x03;
    module[59] = 0x40;
    module[60] = 0x0c;
    module[61] = 0x00;
    module[62] = 0x0b;
    for (size_t i = 63; i < 84; ++i) {
        module[i] = 0x01;
    }
    module[84] = 0x0b;
    return module;
}

void concurrentTickEntry(void* p1, void* p2, void* p3) {
    auto* state = static_cast<ConcurrentTickState*>(p1);
    (void)p2;
    (void)p3;
    state->result = wasm_mvp_runtime::tick(0, deadline(), state->output);
}

}  // namespace

ZTEST_SUITE(wasm_mvp_runtime, nullptr, nullptr, nullptr, nullptr, nullptr);

ZTEST(wasm_mvp_runtime, test_actual_wasm3_parse_compile_link_and_call) {
    expectGoodActivationAndTick(0, kExpectedCyan);
    expectGoodActivationAndTick(499, kExpectedCyan);
    expectGoodActivationAndTick(500, kExpectedMagenta);
    expectGoodActivationAndTick(1000, kExpectedCyan);
    expectGoodActivationAndTick(UINT32_MAX, kExpectedCyan);
}

ZTEST(wasm_mvp_runtime, test_cpptest_v2_matches_legacy_effect_across_state_and_parameters) {
    zassert_equal(wasm_mvp_runtime::startV2(kCppTestV2Module, sizeof(kCppTestV2Module), deadline()),
                  wasm_mvp_runtime::Result::Completed);

    wasm_mvp_runtime::V2TickInputs inputs;
    inputs.params[0] = 50;
    inputs.params[1] = 0x00ff40ff;
    inputs.params[2] = 0;

    uint32_t timeMs = 0;
    zassert_equal(wasm_mvp_runtime::tickV2(17, inputs, deadline(), sV2Output),
                  wasm_mvp_runtime::Result::Completed);
    timeMs += 17;
    expectV2Frame(sV2Output, timeMs, inputs.params[1], false);

    inputs.params[0] = 100;
    inputs.params[1] = 0x00102080;
    zassert_equal(wasm_mvp_runtime::tickV2(25, inputs, deadline(), sV2Output),
                  wasm_mvp_runtime::Result::Completed);
    timeMs += 50;
    expectV2Frame(sV2Output, timeMs, inputs.params[1], false);

    inputs.params[2] = 1;
    zassert_equal(wasm_mvp_runtime::tickV2(0, inputs, deadline(), sV2Output),
                  wasm_mvp_runtime::Result::Completed);
    expectV2Frame(sV2Output, timeMs, inputs.params[1], true);

    wasm_mvp_runtime::stop();
}

ZTEST(wasm_mvp_runtime, test_v2_and_mvp_profiles_reject_each_others_modules) {
    zassert_equal(wasm_mvp_runtime::start(kCppTestV2Module, sizeof(kCppTestV2Module), deadline()),
                  wasm_mvp_runtime::Result::InvalidModule);
    zassert_equal(wasm_mvp_runtime::startV2(kWasmMvpModule, sizeof(kWasmMvpModule), deadline()),
                  wasm_mvp_runtime::Result::InvalidModule);
    expectGoodActivationAndTick(0, kExpectedCyan);
}

ZTEST(wasm_mvp_runtime, test_malformed_module_fails_then_good_module_recovers) {
    auto malformed = moduleCopy();
    malformed[0] = 0xff;
    zassert_equal(wasm_mvp_runtime::start(malformed.data(), malformed.size(), deadline()),
                  wasm_mvp_runtime::Result::InvalidModule);
    expectGoodActivationAndTick(500, kExpectedMagenta);
}

ZTEST(wasm_mvp_runtime, test_missing_import_fails_then_good_module_recovers) {
    auto missingImport = moduleCopy();
    // "fill" begins at byte 28. Rename it to "fall" without changing shape.
    missingImport[29] = 'a';
    zassert_equal(wasm_mvp_runtime::start(missingImport.data(), missingImport.size(), deadline()),
                  wasm_mvp_runtime::Result::InvalidModule);
    expectGoodActivationAndTick(0, kExpectedCyan);
}

ZTEST(wasm_mvp_runtime, test_extra_import_is_rejected_before_load) {
    auto extraImport = moduleVector();
    // Grow the import payload from 17 to 34 bytes, change its count to two,
    // and append rgbx_mvp.other with the same function type.
    extraImport[16] = 0x22;
    extraImport[17] = 0x02;
    insertBytes(
        extraImport, 34,
        {0x08, 'r', 'g', 'b', 'x', '_', 'm', 'v', 'p', 0x05, 'o', 't', 'h', 'e', 'r', 0x00, 0x00});
    zassert_equal(wasm_mvp_runtime::start(extraImport.data(), extraImport.size(), deadline()),
                  wasm_mvp_runtime::Result::InvalidModule);
    expectGoodActivationAndTick(0, kExpectedCyan);
}

ZTEST(wasm_mvp_runtime, test_imported_global_is_rejected_before_load) {
    auto importedGlobal = moduleVector();
    importedGlobal[16] = 0x18;  // import payload grows from 17 to 24 bytes
    importedGlobal[17] = 0x02;  // fill plus one imported global
    insertBytes(importedGlobal, 34, {0x01, 'x', 0x01, 'g', 0x03, 0x7f, 0x00});
    zassert_equal(wasm_mvp_runtime::start(importedGlobal.data(), importedGlobal.size(), deadline()),
                  wasm_mvp_runtime::Result::InvalidModule);
    expectGoodActivationAndTick(0, kExpectedCyan);
}

ZTEST(wasm_mvp_runtime, test_tick_export_signature_is_checked_before_first_call) {
    auto twoArgumentTick = moduleVector();
    twoArgumentTick[9] = 0x0a;   // type payload grows from 5 to 10 bytes
    twoArgumentTick[10] = 0x02;  // fill type plus tick type
    insertBytes(twoArgumentTick, 15, {0x60, 0x02, 0x7f, 0x7f, 0x00});
    twoArgumentTick[42] = 0x01;  // defined rgbx_tick uses the second type
    zassert_equal(
        wasm_mvp_runtime::start(twoArgumentTick.data(), twoArgumentTick.size(), deadline()),
        wasm_mvp_runtime::Result::InvalidModule);
    expectGoodActivationAndTick(0, kExpectedCyan);
}

ZTEST(wasm_mvp_runtime, test_start_function_is_rejected_before_load) {
    auto startFunction = moduleVector();
    // Section 8 names function index 1 as the start function. Insert it before
    // the original code section at byte 53 so section ordering stays valid.
    insertBytes(startFunction, 53, {0x08, 0x01, 0x01});
    zassert_equal(wasm_mvp_runtime::start(startFunction.data(), startFunction.size(), deadline()),
                  wasm_mvp_runtime::Result::InvalidModule);
    expectGoodActivationAndTick(0, kExpectedCyan);
}

ZTEST(wasm_mvp_runtime, test_memory_and_table_sections_are_rejected) {
    auto memory = moduleVector();
    // One memory with minimum one 64 KiB page, inserted before export section.
    insertBytes(memory, 38, {0x05, 0x03, 0x01, 0x00, 0x01});
    zassert_equal(wasm_mvp_runtime::start(memory.data(), memory.size(), deadline()),
                  wasm_mvp_runtime::Result::InvalidModule);

    auto table = moduleVector();
    // One funcref table with minimum one entry, also before export section.
    insertBytes(table, 38, {0x04, 0x04, 0x01, 0x70, 0x00, 0x01});
    zassert_equal(wasm_mvp_runtime::start(table.data(), table.size(), deadline()),
                  wasm_mvp_runtime::Result::InvalidModule);
    expectGoodActivationAndTick(0, kExpectedCyan);
}

ZTEST(wasm_mvp_runtime, test_excessive_locals_are_rejected_after_full_compile) {
    auto excessiveLocals = moduleVector();
    // Replace the zero local-group count with one group of 33 i32 locals.
    excessiveLocals[54] = 0x20;
    excessiveLocals[56] = 0x1e;
    excessiveLocals[57] = 0x01;
    insertBytes(excessiveLocals, 58, {0x21, 0x7f});
    zassert_equal(
        wasm_mvp_runtime::start(excessiveLocals.data(), excessiveLocals.size(), deadline()),
        wasm_mvp_runtime::Result::InvalidModule);
    expectGoodActivationAndTick(0, kExpectedCyan);
}

ZTEST(wasm_mvp_runtime, test_malformed_data_segment_size_is_rejected_then_recovers) {
    zassert_equal(wasm_mvp_runtime::start(kWasm3DataSegmentOverflowModule,
                                         sizeof(kWasm3DataSegmentOverflowModule), deadline()),
                  wasm_mvp_runtime::Result::InvalidModule);
    expectGoodActivationAndTick(0, kExpectedCyan);
}

ZTEST(wasm_mvp_runtime, test_overflowing_branch_table_count_is_rejected_then_recovers) {
    for (uint8_t lowByte : {0xfau, 0xfbu, 0xfcu}) {
        auto module = std::array<uint8_t, sizeof(kWasm3BranchTableOverflowModule)>{};
        std::copy_n(kWasm3BranchTableOverflowModule, module.size(), module.begin());
        module[module.size() - 7] = lowByte;
        zassert_equal(wasm_mvp_runtime::start(module.data(), module.size(), deadline()),
                      wasm_mvp_runtime::Result::InvalidModule);
    }
    expectGoodActivationAndTick(0, kExpectedCyan);
}

ZTEST(wasm_mvp_runtime, test_recursive_argument_copy_traps_then_recovers) {
    zassert_equal(wasm_mvp_runtime::start(kWasm3RecursiveArgumentsModule,
                                         sizeof(kWasm3RecursiveArgumentsModule), deadline()),
                  wasm_mvp_runtime::Result::Completed);
    wasm_mvp_runtime::TickOutput output{0xdeadbeef, 0};
    zassert_equal(wasm_mvp_runtime::tick(0, deadline(), output),
                  wasm_mvp_runtime::Result::Trap);
    zassert_equal(output.color, 0xdeadbeef,
                  "stack overflow must not commit partial output");
    expectGoodActivationAndTick(0, kExpectedCyan);
}

ZTEST(wasm_mvp_runtime, test_guest_trap_discards_output_then_good_module_recovers) {
    auto trap = moduleCopy();
    // Keep the original 28-byte function body valid, but replace its first
    // instruction with unreachable and pad the remainder with nops.
    trap[58] = 0x00;
    for (size_t i = 59; i < 84; ++i) {
        trap[i] = 0x01;
    }
    trap[84] = 0x0b;

    zassert_equal(wasm_mvp_runtime::start(trap.data(), trap.size(), deadline()),
                  wasm_mvp_runtime::Result::Completed);
    wasm_mvp_runtime::TickOutput output{0xdeadbeef, 0};
    zassert_equal(wasm_mvp_runtime::tick(0, deadline(), output), wasm_mvp_runtime::Result::Trap);
    zassert_equal(output.color, 0xdeadbeef, "a trapped generation must not commit partial output");
    expectGoodActivationAndTick(0, kExpectedCyan);
}

ZTEST(wasm_mvp_runtime, test_infinite_loop_is_aborted_then_good_module_recovers) {
    auto infiniteLoop = infiniteLoopModule();

    zassert_equal(wasm_mvp_runtime::start(infiniteLoop.data(), infiniteLoop.size(), deadline()),
                  wasm_mvp_runtime::Result::Completed);
    wasm_mvp_runtime::TickOutput output{0xdeadbeef, 0};
    const auto result = wasm_mvp_runtime::tick(0, deadline(), output);
    zassert_true(result == wasm_mvp_runtime::Result::CpuBudgetExceeded ||
                     result == wasm_mvp_runtime::Result::WallBackstopExceeded,
                 "infinite guest returned unexpected result %u", static_cast<unsigned int>(result));
    zassert_equal(output.color, 0xdeadbeef, "an aborted generation must not commit partial output");
    expectGoodActivationAndTick(0, kExpectedCyan);
}

ZTEST(wasm_mvp_runtime, test_concurrent_stop_discards_in_flight_output_then_recovers) {
    auto infiniteLoop = infiniteLoopModule();
    zassert_equal(wasm_mvp_runtime::start(infiniteLoop.data(), infiniteLoop.size(), deadline()),
                  wasm_mvp_runtime::Result::Completed);

    ConcurrentTickState state;
    k_tid_t tid = k_thread_create(&sConcurrentTickThread, sConcurrentTickStack,
                                  K_THREAD_STACK_SIZEOF(sConcurrentTickStack), concurrentTickEntry,
                                  &state, nullptr, nullptr, 4, 0, K_NO_WAIT);
    zassert_not_null(tid);
    zassert_true(wasm_mvp_runtime::waitForRequestStartForTest(K_SECONDS(1)));

    wasm_mvp_runtime::stop();
    zassert_equal(k_thread_join(tid, K_SECONDS(1)), 0);
    zassert_true(state.result == wasm_mvp_runtime::Result::CpuBudgetExceeded ||
                     state.result == wasm_mvp_runtime::Result::WallBackstopExceeded,
                 "in-flight guest returned unexpected result %u",
                 static_cast<unsigned int>(state.result));
    zassert_equal(state.output.color, 0xdeadbeef,
                  "concurrent stop must not expose an aborted generation's output");
    expectGoodActivationAndTick(0, kExpectedCyan);
}

ZTEST(wasm_mvp_runtime, test_mpu_fault_kills_only_sandbox_then_good_module_recovers) {
    zassert_equal(wasm_mvp_runtime::start(kWasmMvpModule, sizeof(kWasmMvpModule), deadline()),
                  wasm_mvp_runtime::Result::Completed);
    zassert_equal(wasm_mvp_runtime::triggerMemoryFaultForTest(deadline()),
                  wasm_mvp_runtime::Result::SandboxDied);
    expectGoodActivationAndTick(0, kExpectedCyan);
}

ZTEST(wasm_mvp_runtime, test_one_thousand_bulk_reset_recreate_cycles_do_not_drift) {
    size_t firstHighWater = 0;
    for (size_t i = 0; i < 1000; ++i) {
        zassert_equal(wasm_mvp_runtime::start(kWasmMvpModule, sizeof(kWasmMvpModule), deadline()),
                      wasm_mvp_runtime::Result::Completed, "activation %zu failed", i);
        wasm_mvp_runtime::TickOutput output;
        zassert_equal(wasm_mvp_runtime::tick(static_cast<uint32_t>(i), deadline(), output),
                      wasm_mvp_runtime::Result::Completed, "tick %zu failed", i);
        if (i == 0) {
            firstHighWater = output.arenaHighWater;
        }
        zassert_equal(output.arenaHighWater, firstHighWater, "fixed-arena use drifted at cycle %zu",
                      i);
        wasm_mvp_runtime::stop();
    }
}
