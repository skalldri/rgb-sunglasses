#pragma once

#include <stdint.h>

// Frame-pacing instrumentation core (issue #267).
//
// The user-visible symptom is a stutter — the panel holding a frame for 100-500 ms — which
// is a *scheduling* property, not a work-time property. led_controller.cpp's pre-existing
// timing only measured how long the display thread's own work took, so a frame delayed
// entirely by some other thread hogging the CPU looked perfectly healthy. What matters is
// the wake-to-wake interval, so that is what these track.
//
// BT-free and Zephyr-free on purpose: this is the branchy part (min/max tracking, the
// late-frame threshold, the average), so it lives here where the native_sim
// tests/led_controller suite can exercise it directly, in the same spirit as
// conn_param_governor_core and coredump_manager_core. led_controller.cpp keeps only the
// clock reads, the spinlock, and the shell printing.
namespace led_stats_core {

struct Stats {
    uint32_t frames;
    uint32_t overruns;       // frames whose work exceeded the target interval
    uint32_t intervalMinUs;  // wake-to-wake
    uint32_t intervalMaxUs;
    uint64_t intervalSumUs;
    uint32_t workMaxUs;       // the display thread's own work per frame
    uint32_t worstSegmentUs;  // longest stretch between two points the loop can yield at
    // Attribution for that worst segment, so an unattended soak stays diagnosable after
    // the rate-limited overrun warning has scrolled out of the serial backlog.
    //
    // worstSegmentCpuUs is the CPU the display thread itself consumed during that same
    // stretch. The pair is the whole point: a segment with 1.4 s wall and ~1 ms CPU was
    // not slow, it was NOT RUNNING - something preempted it. Same wall/CPU split the
    // extension sandbox uses to tell a runaway extension from a starved one.
    //
    // The label is a string literal with static storage duration (the call sites pass
    // only literals); storing a pointer is safe precisely because of that. Do not pass a
    // stack buffer.
    const char* worstSegmentLabel;
    uint32_t worstSegmentCpuUs;
    uint32_t lateFrames;      // interval > 2x target
};

// A frame is "late" past this multiple of the target interval. 2x means the panel visibly
// held a frame for a whole extra period, which is the smallest thing a viewer can notice.
inline constexpr uint32_t kLateFrameMultiple = 2;

inline void reset(Stats& s) {
    s = Stats{};
    // Sentinel so the first recorded interval always wins the min comparison.
    s.intervalMinUs = UINT32_MAX;
    s.worstSegmentLabel = "none";
}

// Record one completed frame. haveInterval is false for the very first frame after a reset,
// where there is no previous wake-up to measure against — without it the min would be
// pinned at 0 forever by a bogus first sample.
inline void recordFrame(Stats& s, bool haveInterval, uint32_t intervalUs, uint32_t workUs,
                        uint32_t segmentUs, uint32_t targetUs,
                        // Defaulted so the many existing pacing tests, which care only
                        // about interval/work bookkeeping, stay readable. The single
                        // production caller always passes both explicitly.
                        const char* segmentLabel = "none", uint32_t segmentCpuUs = 0) {
    s.frames++;

    if (workUs > s.workMaxUs) {
        s.workMaxUs = workUs;
    }
    if (segmentUs > s.worstSegmentUs) {
        s.worstSegmentUs = segmentUs;
        // Label and CPU travel WITH the max, never independently - otherwise the printed
        // triple would describe three different frames.
        s.worstSegmentLabel = (segmentLabel != nullptr) ? segmentLabel : "none";
        s.worstSegmentCpuUs = segmentCpuUs;
    }

    if (!haveInterval) {
        return;
    }

    if (intervalUs < s.intervalMinUs) {
        s.intervalMinUs = intervalUs;
    }
    if (intervalUs > s.intervalMaxUs) {
        s.intervalMaxUs = intervalUs;
    }
    s.intervalSumUs += intervalUs;
    if (targetUs != 0 && intervalUs > kLateFrameMultiple * targetUs) {
        s.lateFrames++;
    }
}

inline void recordOverrun(Stats& s) {
    s.overruns++;
}

// Flattened view for reporting: resolves the "no samples yet" cases so the caller never has
// to reason about the UINT32_MAX sentinel or divide by zero.
struct Summary {
    uint32_t frames;
    uint32_t intervalSamples;
    uint32_t intervalMinUs;
    uint32_t intervalAvgUs;
    uint32_t intervalMaxUs;
    uint32_t lateFrames;
    uint32_t workMaxUs;
    uint32_t worstSegmentUs;
    const char* worstSegmentLabel;
    uint32_t worstSegmentCpuUs;
    uint32_t overruns;
};

inline Summary summarize(const Stats& s) {
    Summary out{};
    out.frames = s.frames;
    out.lateFrames = s.lateFrames;
    out.workMaxUs = s.workMaxUs;
    out.worstSegmentUs = s.worstSegmentUs;
    out.worstSegmentLabel = (s.worstSegmentLabel != nullptr) ? s.worstSegmentLabel : "none";
    out.worstSegmentCpuUs = s.worstSegmentCpuUs;
    out.overruns = s.overruns;

    // Intervals are only recorded from the second frame onwards.
    out.intervalSamples = s.frames > 0 ? s.frames - 1 : 0;
    if (out.intervalSamples == 0) {
        return out;  // min stays 0 rather than leaking the UINT32_MAX sentinel
    }

    out.intervalMinUs = s.intervalMinUs;
    out.intervalMaxUs = s.intervalMaxUs;
    out.intervalAvgUs = static_cast<uint32_t>(s.intervalSumUs / out.intervalSamples);
    return out;
}

}  // namespace led_stats_core
