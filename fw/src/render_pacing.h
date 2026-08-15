#pragma once

#include <stdint.h>

// Render-pacing arithmetic for the frame-consumed handshake (issue #379),
// extracted from pattern_controller's render loop so the divider math is
// unit-testable on native_sim (PR #381 review: the loop itself is not compiled
// into any test binary, so its arithmetic must live in a pure seam). Same
// kernel-free testable-seam idiom as led_stats_core / extension_tick_budget;
// the ztest suite is fw/tests/render_pacing/.
namespace render_pacing {

// How many consumed display frames one render should span: CEIL(render/display),
// so the effective render interval is never SHORTER than requested —
// render_thread_rate_ms is the one knob trading frame rate against
// render/sandbox CPU, and rounding would silently snap any 1.0-1.5x setting
// back up to the full display rate (PR #381 review). An unusable display
// interval (<= 0) yields 1: the caller self-paces on that path and the divider
// is meaningless. The small epsilon keeps exact float multiples exact
// (2.0 stored as 1.999... must not gain a spurious frame).
inline uint32_t framesPerRender(float renderMs, float displayMs) {
    if (displayMs <= 0.0f) {
        return 1;
    }
    const float ratio = renderMs / displayMs;
    uint32_t frames = (uint32_t)ratio;
    if ((float)frames < ratio - 0.001f) {
        frames++;
    }
    return (frames < 1) ? 1 : frames;
}

// The true nominal inter-render interval — what tick()/shuffle receive as dt.
// N * display when the display clock is usable; the render rate itself when it
// is not (dt must not collapse to 0: animations would freeze and shuffle's
// dwell clock would stop — PR #381 review).
inline float renderIntervalMs(float renderMs, float displayMs) {
    if (displayMs <= 0.0f) {
        // Both rates are remotely writable and unclamped, and getRenderRateMs()'s
        // floor is a no-op when the display rate is 0 — so renderMs can be 0 here
        // too. dt must never reach the animations as 0 (they would freeze and
        // shuffle's dwell clock would stop), so hold a 1 ms floor (PR #381
        // review).
        return (renderMs > 1.0f) ? renderMs : 1.0f;
    }
    return (float)framesPerRender(renderMs, displayMs) * displayMs;
}

}  // namespace render_pacing
