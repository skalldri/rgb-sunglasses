#pragma once

#include <zephyr/kernel.h>

#include <cstddef>
#include <cstdint>

namespace wasm_mvp_runtime {

enum class Result : uint8_t {
    Completed,
    NotReady,
    InvalidModule,
    RuntimeFailure,
    Trap,
    CpuBudgetExceeded,
    SandboxDied,
    WallBackstopExceeded,
};

struct TickOutput {
    uint32_t color = 0;
    size_t arenaHighWater = 0;
};

/** Copy, parse, link, fully compile, and resolve one embedded module in K_USER. */
Result start(const uint8_t* module, size_t moduleSize, k_timepoint_t deadline);

/** Execute one tick in K_USER and commit only its generation-matched scalar result. */
Result tick(uint32_t elapsedMs, k_timepoint_t deadline, TickOutput& output);

/** Abort and join the sandbox, invalidate all handles, then bulk-reset its arena. */
void stop();

/** Lock-free exception-context ownership check for the shared fatal handler. */
bool isCurrentSandboxThread();

/** Peak fixed-arena use observed across activations, retained after reset. */
size_t peakArenaHighWater();

const char* describe(Result result);

#if defined(CONFIG_ZTEST)
/** Test-only: make the sandbox read kernel-only memory and report its contained death. */
Result triggerMemoryFaultForTest(k_timepoint_t deadline);

/** Test-only: wait until the sandbox has accepted an execution request. */
bool waitForRequestStartForTest(k_timeout_t timeout);
#endif

}  // namespace wasm_mvp_runtime
