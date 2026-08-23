#include <animations/wasm_mvp_module.h>
#include <animations/plasma_v2_module.h>
#include <extensions/wasm_mvp_runtime.h>
#include <zephyr/ztest.h>

#include "wasm3_security_modules.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
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

uint8_t colorChannel(uint32_t color, uint32_t shift) {
    return static_cast<uint8_t>((color >> shift) & 0xffu);
}

uint8_t plasmaLerp8(uint8_t from, uint8_t to, uint32_t amount) {
    const int32_t delta = static_cast<int32_t>(to) - static_cast<int32_t>(from);
    return static_cast<uint8_t>(static_cast<int32_t>(from) +
                                delta * static_cast<int32_t>(amount) / 255);
}

uint32_t plasmaReferencePixel(uint32_t timeMs, uint32_t foreground, bool invert,
                              uint32_t background, size_t x, size_t y) {
    constexpr float kTau = 6.2831853f;
    const float time = static_cast<float>(timeMs) * 0.001f;
    const float fx = static_cast<float>(x) / static_cast<float>(wasm_mvp_runtime::kV2Width);
    const float fy = static_cast<float>(y) / static_cast<float>(wasm_mvp_runtime::kV2Height);
    const float wave = std::sin(fx * kTau * 1.5f + time * 1.1f) +
                       std::sin(fy * kTau + time * 0.7f) + std::sin((fx + fy) * kTau + time * 1.7f);
    float value = (wave + 3.0f) * (255.0f / 6.0f);
    if (invert) {
        value = 255.0f - value;
    }
    const uint32_t amount = static_cast<uint32_t>(value);
    return (static_cast<uint32_t>(
                plasmaLerp8(colorChannel(background, 16), colorChannel(foreground, 16), amount))
            << 16u) |
           (static_cast<uint32_t>(
                plasmaLerp8(colorChannel(background, 8), colorChannel(foreground, 8), amount))
            << 8u) |
           static_cast<uint32_t>(
               plasmaLerp8(colorChannel(background, 0), colorChannel(foreground, 0), amount));
}

void expectPlasmaFrame(const wasm_mvp_runtime::V2TickOutput& output, uint32_t timeMs,
                       const wasm_mvp_runtime::V2TickInputs& inputs) {
    for (size_t y = 0; y < wasm_mvp_runtime::kV2Height; ++y) {
        for (size_t x = 0; x < wasm_mvp_runtime::kV2Width; ++x) {
            const size_t pixel = y * wasm_mvp_runtime::kV2Width + x;
            const uint32_t expected = plasmaReferencePixel(
                timeMs, inputs.params[1], inputs.params[2] != 0, inputs.params[3], x, y);
            for (uint32_t shift : {0u, 8u, 16u}) {
                const int32_t actualChannel = colorChannel(output.pixels[pixel], shift);
                const int32_t expectedChannel = colorChannel(expected, shift);
                const int32_t difference = std::abs(actualChannel - expectedChannel);
                zassert_true(difference <= 1, "pixel %zu channel %u differs by %d", pixel, shift,
                             difference);
            }
        }
    }
    zassert_true(output.goodMoment);
    zassert_true(output.arenaHighWater > 0);
    zassert_true(output.arenaHighWater <= CONFIG_APP_WASM3_MVP_HEAP_SIZE);
    zassert_true(output.cpuTimeUs <= CONFIG_APP_WASM3_MVP_CPU_BUDGET_MS * 1000u);
    zassert_true(output.wallTimeUs <= CONFIG_APP_WASM3_MVP_WALL_BACKSTOP_MS * 1000u);
    TC_PRINT("plasma v2 tick: cpu %u us, wall %u us\n", output.cpuTimeUs, output.wallTimeUs);
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

void expectGoodActivationAndTick(uint32_t elapsedMs, uint32_t expectedColor);

void expectV2TrapWithoutCommit(const wasm_mvp_runtime::V2TickInputs& inputs) {
    sV2Output.pixels.fill(0xdeadbeefu);
    sV2Output.goodMoment = false;
    sV2Output.diagnosticCount = 3;
    zassert_equal(wasm_mvp_runtime::tickV2(0, inputs, deadline(), sV2Output),
                  wasm_mvp_runtime::Result::Trap);
    for (uint32_t pixel : sV2Output.pixels) {
        zassert_equal(pixel, 0xdeadbeefu, "rejected signal call committed output");
    }
    zassert_false(sV2Output.goodMoment);
    zassert_equal(sV2Output.diagnosticCount, 3, "rejected generation changed diagnostics");
    expectGoodActivationAndTick(0, kExpectedCyan);
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

void appendLebU32(std::vector<uint8_t>& output, uint32_t value) {
    do {
        uint8_t byte = value & 0x7fu;
        value >>= 7u;
        if (value != 0) {
            byte |= 0x80u;
        }
        output.push_back(byte);
    } while (value != 0);
}

template <size_t Capacity>
struct FixedBytes {
    std::array<uint8_t, Capacity> bytes{};
    size_t size = 0;

    void push(uint8_t value) {
        zassert_true(size < Capacity, "fixed Wasm builder overflow");
        bytes[size++] = value;
    }

    void append(std::initializer_list<uint8_t> values) {
        for (uint8_t value : values) {
            push(value);
        }
    }

    template <size_t OtherCapacity>
    void append(const FixedBytes<OtherCapacity>& other) {
        for (size_t i = 0; i < other.size; ++i) {
            push(other.bytes[i]);
        }
    }
};

template <size_t Capacity>
void appendLebU32(FixedBytes<Capacity>& output, uint32_t value) {
    do {
        uint8_t byte = value & 0x7fu;
        value >>= 7u;
        if (value != 0) {
            byte |= 0x80u;
        }
        output.push(byte);
    } while (value != 0);
}

template <size_t Capacity>
void appendPositiveI32Const(FixedBytes<Capacity>& output, uint32_t value) {
    output.push(0x41);
    while (true) {
        uint8_t byte = value & 0x7fu;
        value >>= 7u;
        const bool done = value == 0 && (byte & 0x40u) == 0;
        if (!done) {
            byte |= 0x80u;
        }
        output.push(byte);
        if (done) {
            return;
        }
    }
}

template <size_t Capacity>
void appendName(FixedBytes<Capacity>& output, const char* name) {
    const size_t length = std::strlen(name);
    appendLebU32(output, length);
    for (size_t i = 0; i < length; ++i) {
        output.push(static_cast<uint8_t>(name[i]));
    }
}

template <size_t ModuleCapacity, size_t PayloadCapacity>
void appendSection(FixedBytes<ModuleCapacity>& module, uint8_t id,
                   const FixedBytes<PayloadCapacity>& payload) {
    module.push(id);
    appendLebU32(module, payload.size);
    module.append(payload);
}

FixedBytes<2048> sLumaSpanModule;
FixedBytes<2048> sInputSignalsModule;

void buildLumaSpanV2Module(FixedBytes<2048>& module, bool invalidLuma) {
    module.bytes.fill(0);
    module.size = 0;
    module.append({0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00});

    FixedBytes<64> types;
    types.append({0x04, 0x60, 0x01, 0x7f, 0x01, 0x7f, 0x60, 0x0b});
    for (size_t i = 0; i < 11; ++i) {
        types.push(0x7f);
    }
    types.append({0x00, 0x60, 0x00, 0x00, 0x60, 0x01, 0x7f, 0x00});
    appendSection(module, 0x01, types);

    FixedBytes<96> imports;
    imports.push(0x02);
    appendName(imports, "rgbx_v2");
    appendName(imports, "param_u32");
    imports.append({0x00, 0x00});
    appendName(imports, "rgbx_v2");
    appendName(imports, "set_luma_span8");
    imports.append({0x00, 0x01});
    appendSection(module, 0x02, imports);

    FixedBytes<8> functions;
    functions.append({0x02, 0x02, 0x03});
    appendSection(module, 0x03, functions);

    FixedBytes<48> exports;
    exports.push(0x02);
    appendName(exports, "rgbx_init");
    exports.append({0x00, 0x02});
    appendName(exports, "rgbx_tick");
    exports.append({0x00, 0x03});
    appendSection(module, 0x07, exports);

    FixedBytes<8> initBody;
    initBody.append({0x00, 0x0b});
    FixedBytes<128> tickBody;
    tickBody.append({0x01, 0x01, 0x7f});  // one i32 local at index 1
    appendPositiveI32Const(tickBody, 0);
    tickBody.append({0x21, 0x01, 0x03, 0x40});  // local.set 1; loop
    tickBody.append({0x20, 0x01});              // local.get 1: first pixel
    appendPositiveI32Const(tickBody, 0x00204060u);
    appendPositiveI32Const(tickBody, 0x00020406u);
    for (uint32_t pixel = 0; pixel < 8; ++pixel) {
        const uint32_t luma = invalidLuma && pixel == 0 ? 256u : pixel * 36u;
        appendPositiveI32Const(tickBody, luma);
    }
    tickBody.append({0x10, 0x01,              // call set_luma_span8
                     0x20, 0x01});            // local.get 1
    appendPositiveI32Const(tickBody, 8);
    tickBody.append({0x6a, 0x22, 0x01});      // i32.add; local.tee 1
    appendPositiveI32Const(tickBody, 480);
    tickBody.append({0x49, 0x0d, 0x00,        // i32.lt_u; br_if loop
                     0x0b, 0x0b});            // end loop; end function

    FixedBytes<192> code;
    code.push(0x02);
    appendLebU32(code, initBody.size);
    code.append(initBody);
    appendLebU32(code, tickBody.size);
    code.append(tickBody);
    appendSection(module, 0x0a, code);
}

void buildInputSignalsV2Module(FixedBytes<2048>& module, uint32_t firstInputKind = 0,
                               uint32_t goodMomentCalls = 1, uint32_t extraInputCalls = 0,
                               uint32_t diagnosticCalls = 4, bool wrongInputSignature = false,
                               bool wrongGoodMomentSignature = false,
                               bool wrongDebugSignature = false, uint32_t firstInputIndex = 0) {
    module.bytes.fill(0);
    module.size = 0;
    module.append({0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00});

    static FixedBytes<96> types;
    types.bytes.fill(0);
    types.size = 0;
    types.append({0x07, 0x60, 0x01, 0x7f, 0x01, 0x7f,  // param_u32: i(i)
                  0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7f,  // input_u32: i(ii)
                  0x60, 0x01, 0x7f, 0x00,              // set_good_moment: v(i)
                  0x60, 0x02, 0x7f, 0x7f, 0x00,        // debug_u32: v(ii)
                  0x60, 0x09});                        // set_span8: v(9*i32)
    for (size_t i = 0; i < 9; ++i) {
        types.push(0x7f);
    }
    types.append({0x00, 0x60, 0x00, 0x00, 0x60, 0x01, 0x7f, 0x00});
    appendSection(module, 0x01, types);

    static FixedBytes<192> imports;
    imports.bytes.fill(0);
    imports.size = 0;
    imports.push(0x05);
    appendName(imports, "rgbx_v2");
    appendName(imports, "param_u32");
    imports.append({0x00, 0x00});
    appendName(imports, "rgbx_v2");
    appendName(imports, "input_u32");
    imports.append({0x00, static_cast<uint8_t>(wrongInputSignature ? 0 : 1)});
    appendName(imports, "rgbx_v2");
    appendName(imports, "set_good_moment");
    imports.append({0x00, static_cast<uint8_t>(wrongGoodMomentSignature ? 0 : 2)});
    appendName(imports, "rgbx_v2");
    appendName(imports, "debug_u32");
    imports.append({0x00, static_cast<uint8_t>(wrongDebugSignature ? 0 : 3)});
    appendName(imports, "rgbx_v2");
    appendName(imports, "set_span8");
    imports.append({0x00, 0x04});
    appendSection(module, 0x02, imports);

    static FixedBytes<8> functions;
    functions.bytes.fill(0);
    functions.size = 0;
    functions.append({0x02, 0x05, 0x06});
    appendSection(module, 0x03, functions);

    static FixedBytes<48> exports;
    exports.bytes.fill(0);
    exports.size = 0;
    exports.push(0x02);
    appendName(exports, "rgbx_init");
    exports.append({0x00, 0x05});
    appendName(exports, "rgbx_tick");
    exports.append({0x00, 0x06});
    appendSection(module, 0x07, exports);

    static FixedBytes<8> initBody;
    initBody.bytes.fill(0);
    initBody.size = 0;
    initBody.append({0x00, 0x0b});
    static FixedBytes<1024> tickBody;
    tickBody.bytes.fill(0);
    tickBody.size = 0;
    tickBody.append({0x01, 0x09, 0x7f});  // nine i32 locals at indices 1..9
    constexpr uint32_t kinds[] = {0, 1, 2, 3, 4, 5, 6, 7};
    constexpr uint32_t indices[] = {0, 19, 0, 0, 0, 2, 0, 0};
    for (uint32_t i = 0; i < 8; ++i) {
        appendPositiveI32Const(tickBody, i == 0 ? firstInputKind : kinds[i]);
        if (!wrongInputSignature) {
            appendPositiveI32Const(tickBody, i == 0 ? firstInputIndex : indices[i]);
        }
        tickBody.append({0x10, 0x01, static_cast<uint8_t>(0x21), static_cast<uint8_t>(i + 1)});
    }
    for (uint32_t i = 0; i < extraInputCalls; ++i) {
        appendPositiveI32Const(tickBody, 2);
        if (!wrongInputSignature) {
            appendPositiveI32Const(tickBody, 0);
        }
        tickBody.append({0x10, 0x01, 0x1a});
    }
    for (uint32_t i = 0; i < goodMomentCalls; ++i) {
        appendPositiveI32Const(tickBody, 0);
        tickBody.append({0x10, 0x00, 0x10, 0x02});
    }
    constexpr uint8_t diagnosticLocals[] = {3, 5, 7, 8, 1};
    for (uint32_t i = 0; i < diagnosticCalls; ++i) {
        appendPositiveI32Const(tickBody, 10 + i);
        tickBody.append({0x20, diagnosticLocals[i % 5], 0x10, 0x03});
    }
    appendPositiveI32Const(tickBody, 0);
    tickBody.append({0x21, 0x09, 0x03, 0x40});  // local.set 9; loop
    tickBody.append({0x20, 0x09});              // first pixel
    for (uint8_t local = 1; local <= 8; ++local) {
        tickBody.append({0x20, local});
    }
    tickBody.append({0x10, 0x04, 0x20, 0x09});
    appendPositiveI32Const(tickBody, 8);
    tickBody.append({0x6a, 0x22, 0x09});
    appendPositiveI32Const(tickBody, 480);
    tickBody.append({0x49, 0x0d, 0x00, 0x0b, 0x0b});

    static FixedBytes<1100> code;
    code.bytes.fill(0);
    code.size = 0;
    code.push(0x02);
    appendLebU32(code, initBody.size);
    code.append(initBody);
    appendLebU32(code, tickBody.size);
    code.append(tickBody);
    appendSection(module, 0x0a, code);
}

std::vector<uint8_t> minimalV2Module(const std::vector<uint8_t>& tickInstructions,
                                     const std::vector<uint8_t>& initInstructions = {}) {
    std::vector<uint8_t> module = {
        0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x19, 0x04, 0x60, 0x01, 0x7f, 0x01, 0x7f,
        0x60, 0x09, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x00,
        0x60, 0x00, 0x00, 0x60, 0x01, 0x7f, 0x00,
        0x02, 0x29, 0x02, 0x07, 0x72, 0x67, 0x62, 0x78, 0x5f, 0x76, 0x32,
        0x09, 0x70, 0x61, 0x72, 0x61, 0x6d, 0x5f, 0x75, 0x33, 0x32, 0x00, 0x00,
        0x07, 0x72, 0x67, 0x62, 0x78, 0x5f, 0x76, 0x32,
        0x09, 0x73, 0x65, 0x74, 0x5f, 0x73, 0x70, 0x61, 0x6e, 0x38, 0x00, 0x01,
        0x03, 0x03, 0x02, 0x02, 0x03,
        0x07, 0x19, 0x02, 0x09, 0x72, 0x67, 0x62, 0x78, 0x5f, 0x69, 0x6e, 0x69, 0x74,
        0x00, 0x02, 0x09, 0x72, 0x67, 0x62, 0x78, 0x5f, 0x74, 0x69, 0x63, 0x6b, 0x00, 0x03,
    };

    std::vector<uint8_t> initBody = {0x00};
    initBody.insert(initBody.end(), initInstructions.begin(), initInstructions.end());
    initBody.push_back(0x0b);
    std::vector<uint8_t> tickBody = {0x00};
    tickBody.insert(tickBody.end(), tickInstructions.begin(), tickInstructions.end());
    tickBody.push_back(0x0b);

    std::vector<uint8_t> codePayload = {0x02};
    appendLebU32(codePayload, initBody.size());
    codePayload.insert(codePayload.end(), initBody.begin(), initBody.end());
    appendLebU32(codePayload, tickBody.size());
    codePayload.insert(codePayload.end(), tickBody.begin(), tickBody.end());
    module.push_back(0x0a);
    appendLebU32(module, codePayload.size());
    module.insert(module.end(), codePayload.begin(), codePayload.end());
    return module;
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

ZTEST(wasm_mvp_runtime, test_plasma_v2_matches_registry_effect_with_bounded_math) {
    zassert_equal(wasm_mvp_runtime::startV2(kPlasmaV2Module, sizeof(kPlasmaV2Module), deadline()),
                  wasm_mvp_runtime::Result::Completed);

    wasm_mvp_runtime::V2TickInputs inputs;
    inputs.params[0] = 50;
    inputs.params[1] = 0x00ff40ff;
    inputs.params[2] = 0;
    inputs.params[3] = 0;

    uint32_t timeMs = 0;
    zassert_equal(wasm_mvp_runtime::tickV2(17, inputs, deadline(), sV2Output),
                  wasm_mvp_runtime::Result::Completed);
    timeMs += 17;
    expectPlasmaFrame(sV2Output, timeMs, inputs);

    inputs.params[0] = 100;
    inputs.params[1] = 0x00102080;
    inputs.params[3] = 0x00ffffff;
    zassert_equal(wasm_mvp_runtime::tickV2(25, inputs, deadline(), sV2Output),
                  wasm_mvp_runtime::Result::Completed);
    timeMs += 50;
    expectPlasmaFrame(sV2Output, timeMs, inputs);

    inputs.params[2] = 1;
    zassert_equal(wasm_mvp_runtime::tickV2(0, inputs, deadline(), sV2Output),
                  wasm_mvp_runtime::Result::Completed);
    expectPlasmaFrame(sV2Output, timeMs, inputs);

    // Cross the 62,832 ms accumulator boundary exactly: 67 + 62,766 -> 1.
    inputs.params[0] = 285300u;
    inputs.params[2] = 0;
    constexpr uint32_t kDtMs = 11;
    constexpr uint32_t kPeriodMs = 62832;
    uint32_t step =
        static_cast<uint32_t>((static_cast<uint64_t>(kDtMs) * inputs.params[0] / 50u) % kPeriodMs);
    zassert_equal(step, 62766u);
    timeMs = (timeMs + step) % kPeriodMs;
    zassert_equal(timeMs, 1u);
    zassert_equal(wasm_mvp_runtime::tickV2(kDtMs, inputs, deadline(), sV2Output),
                  wasm_mvp_runtime::Result::Completed);
    expectPlasmaFrame(sV2Output, timeMs, inputs);

    // Separately prove the guest keeps the dt*speed product in 64 bits.
    inputs.params[0] = 390451573u;
    inputs.params[1] = 0x00f02040;
    inputs.params[3] = 0x00102030;
    step =
        static_cast<uint32_t>((static_cast<uint64_t>(kDtMs) * inputs.params[0] / 50u) % kPeriodMs);
    timeMs = (timeMs + step) % kPeriodMs;
    zassert_equal(wasm_mvp_runtime::tickV2(kDtMs, inputs, deadline(), sV2Output),
                  wasm_mvp_runtime::Result::Completed);
    expectPlasmaFrame(sV2Output, timeMs, inputs);

    wasm_mvp_runtime::stop();
}

ZTEST(wasm_mvp_runtime, test_v2_input_and_lifecycle_signals_commit_atomically) {
    buildInputSignalsV2Module(sInputSignalsModule);
    zassert_true(sInputSignalsModule.size <= CONFIG_APP_WASM3_MVP_MODULE_MAX_SIZE);
    zassert_equal(wasm_mvp_runtime::startV2(sInputSignalsModule.bytes.data(),
                                            sInputSignalsModule.size, deadline()),
                  wasm_mvp_runtime::Result::Completed);

    wasm_mvp_runtime::V2TickInputs inputs;
    inputs.params[0] = 0;
    inputs.capabilities = RGBX_V2_CAPABILITY_ALL;
    inputs.audioBandQ16[0] = 0x00112233u;
    inputs.audioDisplayQ16[19] = 0x00445566u;
    inputs.audioBeatMask = 0x0fu;
    inputs.buttonsPressed = 0x1bu;
    inputs.accelMilli[0] = -1234;
    inputs.gyroMilli[2] = 5678;
    inputs.paramStrings[0][0] = 'A';
    inputs.paramStrings[0][1] = 'B';

    zassert_equal(wasm_mvp_runtime::tickV2(17, inputs, deadline(), sV2Output),
                  wasm_mvp_runtime::Result::Completed);
    const uint32_t expected[] = {inputs.audioBandQ16[0],
                                 inputs.audioDisplayQ16[19],
                                 inputs.audioBeatMask,
                                 inputs.buttonsPressed,
                                 static_cast<uint32_t>(inputs.accelMilli[0]),
                                 static_cast<uint32_t>(inputs.gyroMilli[2]),
                                 2u,
                                 static_cast<uint32_t>('A' + 'B')};
    for (size_t pixel = 0; pixel < sV2Output.pixels.size(); ++pixel) {
        zassert_equal(sV2Output.pixels[pixel], expected[pixel % 8] & 0x00ffffffu,
                      "input pixel %zu differs", pixel);
    }
    zassert_false(sV2Output.goodMoment);
    zassert_equal(sV2Output.diagnosticCount, 4);
    zassert_equal(sV2Output.diagnostics[0].tag, 10);
    zassert_equal(sV2Output.diagnostics[0].value, inputs.audioBeatMask);
    zassert_equal(sV2Output.diagnostics[1].tag, 11);
    zassert_equal(sV2Output.diagnostics[1].value, static_cast<uint32_t>(inputs.accelMilli[0]));
    zassert_equal(sV2Output.diagnostics[2].tag, 12);
    zassert_equal(sV2Output.diagnostics[2].value, 2);
    zassert_equal(sV2Output.diagnostics[3].tag, 13);
    zassert_equal(sV2Output.diagnostics[3].value, static_cast<uint32_t>('A' + 'B'));

    inputs.params[0] = 1;
    zassert_equal(wasm_mvp_runtime::tickV2(17, inputs, deadline(), sV2Output),
                  wasm_mvp_runtime::Result::Completed);
    zassert_true(sV2Output.goodMoment);
    wasm_mvp_runtime::stop();
}

ZTEST(wasm_mvp_runtime, test_v2_optional_signal_imports_fail_closed) {
    struct RejectedModule {
        uint32_t firstInputKind;
        uint32_t goodMomentCalls;
        uint32_t extraInputCalls;
        uint32_t diagnosticCalls;
    };
    constexpr RejectedModule cases[] = {
        {8, 1, 0, 4},   // unknown input kind
        {0, 0, 0, 4},   // imported good-moment signal not emitted
        {0, 2, 0, 4},   // duplicate good-moment signal
        {0, 1, 57, 4},  // 8 baseline + 57 exceeds the 64-call input quota
        {0, 1, 0, 5},   // diagnostics exceed the four-record mailbox
    };
    for (const auto& rejected : cases) {
        buildInputSignalsV2Module(sInputSignalsModule, rejected.firstInputKind,
                                  rejected.goodMomentCalls, rejected.extraInputCalls,
                                  rejected.diagnosticCalls);
        zassert_equal(wasm_mvp_runtime::startV2(sInputSignalsModule.bytes.data(),
                                                sInputSignalsModule.size, deadline()),
                      wasm_mvp_runtime::Result::Completed);
        wasm_mvp_runtime::V2TickInputs inputs;
        inputs.params[0] = 1;
        inputs.capabilities = RGBX_V2_CAPABILITY_ALL;
        expectV2TrapWithoutCommit(inputs);
    }
}

ZTEST(wasm_mvp_runtime, test_v2_optional_import_signatures_are_exact) {
    for (const auto& signatures : {
             std::array<bool, 3>{true, false, false},
             std::array<bool, 3>{false, true, false},
             std::array<bool, 3>{false, false, true},
         }) {
        buildInputSignalsV2Module(sInputSignalsModule, 0, 1, 0, 4, signatures[0], signatures[1],
                                  signatures[2]);
        zassert_equal(wasm_mvp_runtime::startV2(sInputSignalsModule.bytes.data(),
                                                sInputSignalsModule.size, deadline()),
                      wasm_mvp_runtime::Result::InvalidModule);
    }
    expectGoodActivationAndTick(0, kExpectedCyan);
}

ZTEST(wasm_mvp_runtime, test_v2_input_indices_are_exactly_bounded) {
    struct InvalidSelector {
        uint32_t kind;
        uint32_t index;
    };
    constexpr InvalidSelector selectors[] = {
        {RGBX_V2_INPUT_AUDIO_BAND_Q16, RGBX_V2_AUDIO_BAND_COUNT},
        {RGBX_V2_INPUT_AUDIO_DISPLAY_Q16, RGBX_V2_AUDIO_DISPLAY_BUCKET_COUNT},
        {RGBX_V2_INPUT_AUDIO_BEAT_MASK, 1},
        {RGBX_V2_INPUT_BUTTONS_PRESSED, 1},
        {RGBX_V2_INPUT_ACCEL_MILLI, RGBX_V2_IMU_AXIS_COUNT},
        {RGBX_V2_INPUT_GYRO_MILLI, RGBX_V2_IMU_AXIS_COUNT},
        {RGBX_V2_INPUT_STRING_LENGTH, RGBX_V2_MAX_STRING_PARAMS},
        {RGBX_V2_INPUT_STRING_BYTE_SUM, RGBX_V2_MAX_STRING_PARAMS},
    };
    for (const auto& selector : selectors) {
        buildInputSignalsV2Module(sInputSignalsModule, selector.kind, 1, 0, 4, false, false, false,
                                  selector.index);
        zassert_equal(wasm_mvp_runtime::startV2(sInputSignalsModule.bytes.data(),
                                                sInputSignalsModule.size, deadline()),
                      wasm_mvp_runtime::Result::Completed);
        wasm_mvp_runtime::V2TickInputs inputs;
        inputs.params[0] = 1;
        inputs.capabilities = RGBX_V2_CAPABILITY_ALL;
        expectV2TrapWithoutCommit(inputs);
    }
}

ZTEST(wasm_mvp_runtime, test_v2_input_kinds_require_manifest_capabilities) {
    struct MissingCapability {
        uint32_t kind;
        uint32_t capabilities;
    };
    constexpr MissingCapability cases[] = {
        {RGBX_V2_INPUT_AUDIO_BAND_Q16, RGBX_V2_CAPABILITY_BUTTONS | RGBX_V2_CAPABILITY_IMU},
        {RGBX_V2_INPUT_BUTTONS_PRESSED, RGBX_V2_CAPABILITY_IMU | RGBX_V2_CAPABILITY_AUDIO},
        {RGBX_V2_INPUT_ACCEL_MILLI, RGBX_V2_CAPABILITY_BUTTONS | RGBX_V2_CAPABILITY_AUDIO},
    };
    for (const auto& missing : cases) {
        buildInputSignalsV2Module(sInputSignalsModule, missing.kind);
        zassert_equal(wasm_mvp_runtime::startV2(sInputSignalsModule.bytes.data(),
                                                sInputSignalsModule.size, deadline()),
                      wasm_mvp_runtime::Result::Completed);
        wasm_mvp_runtime::V2TickInputs inputs;
        inputs.params[0] = 1;
        inputs.capabilities = missing.capabilities;
        expectV2TrapWithoutCommit(inputs);
    }
}

ZTEST(wasm_mvp_runtime, test_v2_and_mvp_profiles_reject_each_others_modules) {
    zassert_equal(wasm_mvp_runtime::start(kCppTestV2Module, sizeof(kCppTestV2Module), deadline()),
                  wasm_mvp_runtime::Result::InvalidModule);
    zassert_equal(wasm_mvp_runtime::startV2(kWasmMvpModule, sizeof(kWasmMvpModule), deadline()),
                  wasm_mvp_runtime::Result::InvalidModule);
    expectGoodActivationAndTick(0, kExpectedCyan);
}

ZTEST(wasm_mvp_runtime, test_v2_rejects_imported_globals) {
    std::array<uint8_t, sizeof(kCppTestV2Module) + 7> importedGlobal{};
    std::copy_n(kCppTestV2Module, 78, importedGlobal.begin());
    const std::array<uint8_t, 7> globalImport = {0x01, 'x', 0x01, 'g', 0x03, 0x7f, 0x00};
    std::copy(globalImport.begin(), globalImport.end(), importedGlobal.begin() + 78);
    std::copy(kCppTestV2Module + 78, kCppTestV2Module + sizeof(kCppTestV2Module),
              importedGlobal.begin() + 85);
    importedGlobal[36] = 0x30;  // import section grows from 41 to 48 bytes
    importedGlobal[37] = 0x03;  // two function imports plus one global import
    zassert_equal(wasm_mvp_runtime::startV2(importedGlobal.data(), importedGlobal.size(), deadline()),
                  wasm_mvp_runtime::Result::InvalidModule);
    expectGoodActivationAndTick(0, kExpectedCyan);
}

ZTEST(wasm_mvp_runtime, test_v2_rejects_sections_outside_exact_profile) {
    const auto base = minimalV2Module({});
    const auto expectRejected = [](const std::vector<uint8_t>& module) {
        zassert_equal(wasm_mvp_runtime::startV2(module.data(), module.size(), deadline()),
                      wasm_mvp_runtime::Result::InvalidModule);
    };

    auto module = base;
    module.insert(module.begin() + 8, {0x00, 0x01, 0x00});
    expectRejected(module);

    for (uint8_t section : {0x0c, 0x0d, 0x0e}) {
        module = base;
        module.insert(module.end(), {section, 0x01, 0x00});
        expectRejected(module);
    }

    module = base;
    module.insert(module.end(), {0x06, 0x01, 0x00});
    expectRejected(module);

    module = base;
    module.insert(module.end(), {0x06, 0x80});
    expectRejected(module);

    module = base;
    module[9] |= 0x80u;
    module.insert(module.begin() + 10, 0x00);
    expectRejected(module);
    expectGoodActivationAndTick(0, kExpectedCyan);
}

ZTEST(wasm_mvp_runtime, test_v2_requires_init_export) {
    std::array<uint8_t, sizeof(kCppTestV2Module)> missingInit{};
    std::copy_n(kCppTestV2Module, missingInit.size(), missingInit.begin());
    missingInit[101] = 'x';  // rgbx_init -> rgbx_inix
    zassert_equal(wasm_mvp_runtime::startV2(missingInit.data(), missingInit.size(), deadline()),
                  wasm_mvp_runtime::Result::InvalidModule);
    expectGoodActivationAndTick(0, kExpectedCyan);
}

ZTEST(wasm_mvp_runtime, test_v2_checks_init_signature_before_call) {
    auto badInitSignature = minimalV2Module({});
    badInitSignature[81] = 0x03;  // rgbx_init changes from ()->() to (i32)->()
    zassert_equal(wasm_mvp_runtime::startV2(badInitSignature.data(), badInitSignature.size(),
                                            deadline()),
                  wasm_mvp_runtime::Result::InvalidModule);
    expectGoodActivationAndTick(0, kExpectedCyan);
}

ZTEST(wasm_mvp_runtime, test_v2_rejects_wrong_import_signatures) {
    std::array<uint8_t, sizeof(kCppTestV2Module)> wrongParamSignature{};
    std::copy_n(kCppTestV2Module, wrongParamSignature.size(), wrongParamSignature.begin());
    wrongParamSignature[57] = 0x03;  // param_u32 changes from (i32)->i32 to (i32)->void
    zassert_equal(wasm_mvp_runtime::startV2(wrongParamSignature.data(),
                                            wrongParamSignature.size(), deadline()),
                  wasm_mvp_runtime::Result::InvalidModule);
    expectGoodActivationAndTick(0, kExpectedCyan);
}

ZTEST(wasm_mvp_runtime, test_v2_admission_budget_aborts_infinite_init) {
    const auto infiniteInit = minimalV2Module({}, {0x03, 0x40, 0x0c, 0x00, 0x0b});
    const auto result =
        wasm_mvp_runtime::startV2(infiniteInit.data(), infiniteInit.size(), deadline());
    zassert_true(result == wasm_mvp_runtime::Result::CpuBudgetExceeded ||
                     result == wasm_mvp_runtime::Result::WallBackstopExceeded,
                 "infinite init returned unexpected result %u", static_cast<unsigned int>(result));
    expectGoodActivationAndTick(0, kExpectedCyan);
}

ZTEST(wasm_mvp_runtime, test_v2_host_import_limits_trap_without_committing_output) {
    std::vector<uint8_t> badSpanInstructions;
    for (uint32_t argument = 0; argument < 9; ++argument) {
        badSpanInstructions.push_back(0x41);  // i32.const
        badSpanInstructions.push_back(argument == 0 ? 0x08 : 0x00);
    }
    badSpanInstructions.insert(badSpanInstructions.end(), {0x10, 0x01});  // call set_span8

    auto partialFrameInstructions = badSpanInstructions;
    partialFrameInstructions[1] = 0x00;  // one valid first span, then an incomplete return

    std::vector<uint8_t> excessParamInstructions;
    for (size_t call = 0; call < 17; ++call) {
        excessParamInstructions.insert(excessParamInstructions.end(),
                                       {0x41, 0x00, 0x10, 0x00, 0x1a});
    }

    for (const auto& module : {minimalV2Module(badSpanInstructions),
                               minimalV2Module(partialFrameInstructions),
                               minimalV2Module(excessParamInstructions)}) {
        zassert_equal(wasm_mvp_runtime::startV2(module.data(), module.size(), deadline()),
                      wasm_mvp_runtime::Result::Completed);
        wasm_mvp_runtime::V2TickInputs inputs;
        wasm_mvp_runtime::V2TickOutput output;
        output.pixels.fill(0xdeadbeef);
        output.goodMoment = false;
        zassert_equal(wasm_mvp_runtime::tickV2(0, inputs, deadline(), output),
                      wasm_mvp_runtime::Result::Trap);
        for (uint32_t pixel : output.pixels) {
            zassert_equal(pixel, 0xdeadbeef, "rejected host call committed output");
        }
        zassert_false(output.goodMoment);
        expectGoodActivationAndTick(0, kExpectedCyan);
    }
}

ZTEST(wasm_mvp_runtime, test_v2_palette_luma_span_commits_only_complete_valid_frame) {
    zassert_equal(wasm_mvp_runtime::blendLumaForTest(0x00102030u, 0x00f0d0b0u, 0),
                  0x00f0d0b0u);
    zassert_equal(wasm_mvp_runtime::blendLumaForTest(0x00102030u, 0x00f0d0b0u, 85),
                  0x00a69686u);
    zassert_equal(wasm_mvp_runtime::blendLumaForTest(0x00000000u, 0x00ffffffu, 128),
                  0x007f7f7fu);
    zassert_equal(wasm_mvp_runtime::blendLumaForTest(0x00102030u, 0x00f0d0b0u, 255),
                  0x00102030u);

    buildLumaSpanV2Module(sLumaSpanModule, false);
    zassert_true(sLumaSpanModule.size <= CONFIG_APP_WASM3_MVP_MODULE_MAX_SIZE);
    zassert_equal(wasm_mvp_runtime::startV2(sLumaSpanModule.bytes.data(), sLumaSpanModule.size,
                                            deadline()),
                  wasm_mvp_runtime::Result::Completed);
    wasm_mvp_runtime::V2TickInputs inputs;
    zassert_equal(wasm_mvp_runtime::tickV2(0, inputs, deadline(), sV2Output),
                  wasm_mvp_runtime::Result::Completed);
    for (size_t pixel = 0; pixel < sV2Output.pixels.size(); ++pixel) {
        zassert_equal(sV2Output.pixels[pixel],
                      wasm_mvp_runtime::blendLumaForTest(0x00204060u, 0x00020406u,
                                                        (pixel % 8u) * 36u),
                      "palette/luma pixel %zu differs", pixel);
    }
    wasm_mvp_runtime::stop();

    buildLumaSpanV2Module(sLumaSpanModule, true);
    zassert_equal(wasm_mvp_runtime::startV2(sLumaSpanModule.bytes.data(), sLumaSpanModule.size,
                                            deadline()),
                  wasm_mvp_runtime::Result::Completed);
    sV2Output.pixels.fill(0xdeadbeefu);
    sV2Output.goodMoment = false;
    zassert_equal(wasm_mvp_runtime::tickV2(0, inputs, deadline(), sV2Output),
                  wasm_mvp_runtime::Result::Trap);
    for (uint32_t color : sV2Output.pixels) {
        zassert_equal(color, 0xdeadbeefu, "invalid luma committed output");
    }
    zassert_false(sV2Output.goodMoment);
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
