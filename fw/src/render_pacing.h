#pragma once

#include <stdint.h>

// Render-pacing arithmetic for the frame-consumed handshake (issue #379),
// extracted from pattern_controller's render loop so the divider math is
// unit-testable on native_sim (PR #381 review: the loop itself is not compiled
// into any test binary, so its arithmetic must live in a pure seam). Same
// kernel-free testable-seam idiom as led_stats_core / extension_tick_budget;
// the ztest suite is fw/tests/render_pacing/.
namespace render_pacing {

// Divider ceiling. Both rates are remotely writable and unclamped, so the
// ratio can reach ~2^32 (render near UINT32_MAX ms over a display interval of
// 0.001 ms) — and a float-to-uint32 conversion out of range is UB, not
// saturation (PR #381 review). The cap is deliberately generous rather than
// "a handful": a legitimately slow render rate (a static display re-rendered
// every 10 s, say) yields a large N by design, and a small cap would silently
// render it far faster than requested. 1000 is ~33 s per render at the default
// display rate — beyond any sane configuration — and the wait loop's shared
// deadline (~2 x N x display, see pattern_controller.cpp) keeps even a
// capped-N misconfiguration's per-iteration stall bounded and logged.
inline constexpr uint32_t kMaxFramesPerRender = 1000;

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
    if (ratio >= (float)kMaxFramesPerRender) {
        return kMaxFramesPerRender;
    }
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
