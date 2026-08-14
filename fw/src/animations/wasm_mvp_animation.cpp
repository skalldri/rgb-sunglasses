#include <animations/wasm_mvp_animation.h>
#include <animations/wasm_mvp_module.h>
#include <extensions/wasm_mvp_runtime.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(wasm_mvp_animation, LOG_LEVEL_INF);

void WasmMvpAnimation::init() {
    elapsedMs_ = 0;
}

void WasmMvpAnimation::setActive(bool active) {
    BaseAnimationTemplate::setActive(active);
    if (!active) {
        wasm_mvp_runtime::stop();
        activationPending_ = false;
        ready_ = false;
        return;
    }

    elapsedMs_ = 0;
    activationPending_ = true;
    ready_ = false;
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
    // One wall deadline covers a lazy sandbox admission plus its first guest
    // call. Two independent deadlines would double the advertised backstop.
    const k_timepoint_t deadline =
        sys_timepoint_calc(K_MSEC(CONFIG_APP_WASM3_MVP_WALL_BACKSTOP_MS));

    if (activationPending_) {
        activationPending_ = false;
        const auto result =
            wasm_mvp_runtime::start(kWasmMvpModule, sizeof(kWasmMvpModule), deadline);
        ready_ = result == wasm_mvp_runtime::Result::Completed;
        if (!ready_) {
            LOG_ERR("Wasm3 sandbox activation failed: %s", wasm_mvp_runtime::describe(result));
        }
    }

    if (!ready_) {
        fill(renderer, 0);
        return;
    }
    elapsedMs_ += static_cast<uint32_t>(timeSinceLastTickMs);
    wasm_mvp_runtime::TickOutput output;
    const auto result = wasm_mvp_runtime::tick(elapsedMs_, deadline, output);
    if (result != wasm_mvp_runtime::Result::Completed) {
        LOG_ERR("Wasm3 sandbox tick failed: %s", wasm_mvp_runtime::describe(result));
        ready_ = false;
        fill(renderer, 0);
        return;
    }
    fill(renderer, output.color);
}
