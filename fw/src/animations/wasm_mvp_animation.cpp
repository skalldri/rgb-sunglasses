#include <animations/wasm_mvp_animation.h>
#include <animations/wasm_mvp_module.h>
#include <wasm3.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(wasm_mvp_animation, LOG_LEVEL_INF);

namespace {
constexpr char kImportModule[] = "rgbx_mvp";
constexpr char kFillImport[] = "fill";
constexpr char kTickExport[] = "rgbx_tick";

const char* errorText(M3Result result) {
    return result != nullptr ? result : "unknown Wasm3 error";
}
}  // namespace

void WasmMvpAnimation::init() {
    elapsedMs_ = 0;

    if (!initializationAttempted_) {
        initializationAttempted_ = true;
        ready_ = initializeRuntime();
    }
}

bool WasmMvpAnimation::initializeRuntime() {
    environment_ = m3_NewEnvironment();
    if (environment_ == nullptr) {
        LOG_ERR("Wasm3 environment allocation failed");
        return false;
    }

    runtime_ = m3_NewRuntime(environment_, CONFIG_APP_WASM3_MVP_VALUE_STACK_SIZE, this);
    if (runtime_ == nullptr) {
        LOG_ERR("Wasm3 runtime allocation failed");
        return false;
    }

    IM3Module module = nullptr;
    M3Result result = m3_ParseModule(environment_, &module, kWasmMvpModule, sizeof(kWasmMvpModule));
    if (result != m3Err_none) {
        LOG_ERR("Wasm3 parse failed: %s", errorText(result));
        return false;
    }

    result = m3_LoadModule(runtime_, module);
    if (result != m3Err_none) {
        LOG_ERR("Wasm3 load failed: %s", errorText(result));
        m3_FreeModule(module);
        return false;
    }

    result = m3_LinkRawFunctionEx(module, kImportModule, kFillImport, "v(i)", fillHost, this);
    if (result != m3Err_none) {
        LOG_ERR("Wasm3 import link failed: %s", errorText(result));
        return false;
    }

    // Force validation and compilation of every body before the first render
    // tick. This keeps malformed bytecode out of the time-sensitive frame path.
    result = m3_CompileModule(module);
    if (result != m3Err_none) {
        LOG_ERR("Wasm3 compile failed: %s", errorText(result));
        return false;
    }

    result = m3_FindFunction(&tickFunction_, runtime_, kTickExport);
    if (result != m3Err_none) {
        LOG_ERR("Wasm3 export lookup failed: %s", errorText(result));
        return false;
    }

    LOG_INF("Wasm3 MVP ready (%u-byte module, %u-byte arena)",
            static_cast<unsigned int>(sizeof(kWasmMvpModule)),
            static_cast<unsigned int>(CONFIG_APP_WASM3_MVP_HEAP_SIZE));
    return true;
}

const void* WasmMvpAnimation::fillHost(IM3Runtime runtime, IM3ImportContext context,
                                       uint64_t* stack, void* memory) {
    uint64_t* _sp = stack;
    void* _mem = memory;
    IM3ImportContext _ctx = context;
    m3ApiGetArg(uint32_t, color);
    ARG_UNUSED(runtime);
    ARG_UNUSED(_mem);

    auto* animation = static_cast<WasmMvpAnimation*>(const_cast<void*>(_ctx->userdata));
    if (animation == nullptr || animation->activeRenderer_ == nullptr) {
        return m3Err_trapAbort;
    }

    animation->fill(*animation->activeRenderer_, color);
    m3ApiSuccess();
}

void WasmMvpAnimation::fill(AnimationRenderer& renderer, uint32_t color) {
    const uint8_t red = static_cast<uint8_t>((color >> 16) & 0xffu);
    const uint8_t green = static_cast<uint8_t>((color >> 8) & 0xffu);
    const uint8_t blue = static_cast<uint8_t>(color & 0xffu);

    for (size_t x = 0; x < renderer.displayWidth(); ++x) {
        for (size_t y = 0; y < renderer.displayHeight(); ++y) {
            renderer.setPixel(x, y, red, green, blue);
        }
    }
}

void WasmMvpAnimation::tick(AnimationRenderer& renderer, size_t timeSinceLastTickMs) {
    if (!ready_) {
        fill(renderer, 0);
        return;
    }

    elapsedMs_ += static_cast<uint32_t>(timeSinceLastTickMs);
    activeRenderer_ = &renderer;
    const M3Result result = m3_CallV(tickFunction_, elapsedMs_);
    activeRenderer_ = nullptr;

    if (result != m3Err_none) {
        LOG_ERR("Wasm3 tick trapped: %s", errorText(result));
        ready_ = false;
        fill(renderer, 0);
    }
}
