#include <animations/wasm_mvp_module.h>
#include <extensions/wasm_mvp_runtime.h>
#include <zephyr/ztest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace {

constexpr uint32_t kExpectedCyan = 0x00ffff;
constexpr uint32_t kExpectedMagenta = 0xff00ff;

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

}  // namespace

ZTEST_SUITE(wasm_mvp_runtime, nullptr, nullptr, nullptr, nullptr, nullptr);

ZTEST(wasm_mvp_runtime, test_actual_wasm3_parse_compile_link_and_call) {
    expectGoodActivationAndTick(0, kExpectedCyan);
    expectGoodActivationAndTick(499, kExpectedCyan);
    expectGoodActivationAndTick(500, kExpectedMagenta);
    expectGoodActivationAndTick(1000, kExpectedCyan);
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
    auto infiniteLoop = moduleCopy();
    // loop { br 0 }, followed by unreachable nops to preserve the body size.
    infiniteLoop[58] = 0x03;
    infiniteLoop[59] = 0x40;
    infiniteLoop[60] = 0x0c;
    infiniteLoop[61] = 0x00;
    infiniteLoop[62] = 0x0b;
    for (size_t i = 63; i < 84; ++i) {
        infiniteLoop[i] = 0x01;
    }
    infiniteLoop[84] = 0x0b;

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
