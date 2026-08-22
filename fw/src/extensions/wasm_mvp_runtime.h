#pragma once

#include <zephyr/kernel.h>

#include <array>
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

#if defined(CONFIG_APP_WASM3_V2_PROTOTYPE)
inline constexpr size_t kV2Width = 40;
inline constexpr size_t kV2Height = 12;
inline constexpr size_t kV2PixelCount = kV2Width * kV2Height;
inline constexpr size_t kV2ParameterCount = 16;

struct V2TickInputs {
    std::array<uint32_t, kV2ParameterCount> params{};
};

struct V2TickOutput {
    std::array<uint32_t, kV2PixelCount> pixels{};
    bool goodMoment = true;
    size_t arenaHighWater = 0;
    uint32_t cpuTimeUs = 0;
    uint32_t wallTimeUs = 0;
};
#endif

/** Copy, parse, link, fully compile, and resolve one embedded module in K_USER. */
Result start(const uint8_t* module, size_t moduleSize, k_timepoint_t deadline);

/** Execute one tick in K_USER and commit only its generation-matched scalar result. */
Result tick(uint32_t elapsedMs, k_timepoint_t deadline, TickOutput& output);

#if defined(CONFIG_APP_WASM3_V2_PROTOTYPE)
/** Admit a memoryless RGBX v2 module with bounded scalar and drawing imports. */
Result startV2(const uint8_t* module, size_t moduleSize, k_timepoint_t deadline);

/** Execute one RGBX v2 tick and atomically commit one complete 40 by 12 frame. */
Result tickV2(uint32_t dtMs, const V2TickInputs& inputs, k_timepoint_t deadline,
              V2TickOutput& output);
#endif

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

#if defined(CONFIG_APP_WASM3_V2_PROTOTYPE)
/** Test-only pure oracle for the palette/luma span conversion. */
uint32_t blendLumaForTest(uint32_t foreground, uint32_t background, uint32_t luma);
#endif
#endif

}  // namespace wasm_mvp_runtime
