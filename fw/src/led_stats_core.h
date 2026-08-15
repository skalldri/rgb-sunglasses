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
    // stretch. A segment with 1.4 s wall and ~1 ms CPU was not slow, it was NOT RUNNING.
    //
    // But "not running" is TWO different situations that need opposite fixes, and the
    // CPU delta alone cannot separate them: the thread may have been descheduled while
    // something else ran (starvation), or it may have blocked voluntarily inside a call
    // (led_strip_update_rgb sleeps on the SPIM completion semaphore for ~10.4 ms per
    // strip - 216 pixels x 24 bits x 8 SPI bits at 4 MHz - accruing no execution_cycles
    // at all). Reading the second as preemption sends the reader hunting a starvation
    // bug on a completely healthy frame.
    //
    // What separates them is where the wall time WENT, so the other two buckets are
    // carried alongside: worstSegmentOtherUs (some other thread ran - contention) and
    // worstSegmentIdleUs (the CPU had nothing to run, i.e. this thread was blocked
    // waiting on hardware). See classifySegment().
    //
    // The label is a string literal with static storage duration (the call sites pass
    // only literals); storing a pointer is safe precisely because of that. Do not pass a
    // stack buffer.
    const char* worstSegmentLabel;
    uint32_t worstSegmentCpuUs;
    uint32_t worstSegmentOtherUs;  // other threads ran during the segment
    uint32_t worstSegmentIdleUs;   // CPU had nothing to run during the segment
    uint32_t lateFrames;      // interval > 2x target
    // Display claims where no render had completed since the previous claim —
    // the panel re-showed the frame it already pushed. The direct observable of
    // issue #379's render/display phase slip: ~0 in steady state under the
    // frame-consumed handshake (nonzero only during genuine render overruns or
    // a render-rate divider > 1).
    uint32_t heldFrames;
};

// A frame is "late" past this multiple of the target interval. 2x means the panel visibly
// held a frame for a whole extra period, which is the smallest thing a viewer can notice.
inline constexpr uint32_t kLateFrameMultiple = 2;

// A segment counts as off-CPU when its wall time exceeds its own CPU time by this factor.
inline constexpr uint32_t kOffCpuWallToCpuRatio = 4;

// Below this wall time the verdict stays silent. The cycle clock is 32768 Hz on proto0 -
// 30.5 us per tick - and both the wall and CPU figures round independently, so a segment
// of a few ticks can only come out as "0 us cpu" whether or not it was on-CPU the whole
// time. The concrete case is PANEL_OUTPUT_OFF, where no SPI runs and the longest segment
// is a tens-of-microsecond mutex claim that is 100% on-CPU: without a floor it is reported
// as off-CPU, the exact opposite of the truth. Sized well above one tick so the ratio test
// has real resolution before it is allowed to speak.
inline constexpr uint32_t kVerdictMinWallUs = 500;

enum class SegmentVerdict {
    // Too short to resolve, or no CPU accounting available.
    Unknown,
    // The thread was running: the segment really is the cost of the work.
    OnCpu,
    // Off-CPU with the core busy elsewhere - starvation. On proto0 the display thread is
    // priority 2 and preemptible, so anything in the cooperative band (BT RX at -8, the
    // system workqueue at -1) takes the core and holds it until it blocks.
    OffCpuContended,
    // Off-CPU with the core idle - a blocking call waiting on hardware, which is the
    // NORMAL state of an led_strip_update_rgb() segment. Not a defect on its own.
    OffCpuIdle,
};

// Pure decision, kept out of led_controller.cpp so the thresholds and their edge cases are
// unit-testable rather than duplicated inline at each log site.
//
// Comparison is `wallUs / ratio > cpuUs`, never `wallUs > ratio * (cpuUs + 1)`: the latter
// is unsigned 32-bit arithmetic with no bound on cpuUs, so a large CPU figure wraps and
// INVERTS the verdict instead of suppressing it (cpuUs = UINT32_MAX makes the threshold 0,
// so every segment reads as off-CPU - the one combination the test exists to rule out).
inline SegmentVerdict classifySegment(uint32_t wallUs, uint32_t cpuUs, uint32_t otherUs,
                                      uint32_t idleUs) {
    if (wallUs < kVerdictMinWallUs) {
        return SegmentVerdict::Unknown;
    }
    if (wallUs / kOffCpuWallToCpuRatio <= cpuUs) {
        return SegmentVerdict::OnCpu;
    }
    // Neither bucket populated (no CPU accounting, or both rounded to zero): the segment
    // is off-CPU but there is nothing to say about where the time went.
    if (otherUs == 0 && idleUs == 0) {
        return SegmentVerdict::Unknown;
    }
    return (otherUs > idleUs) ? SegmentVerdict::OffCpuContended : SegmentVerdict::OffCpuIdle;
}

// Suffix for a log line, empty when there is nothing worth saying.
inline const char* verdictText(SegmentVerdict v) {
    switch (v) {
        case SegmentVerdict::OffCpuContended:
            return " <- STARVED: another thread held the CPU (not slow, not blocked)";
        case SegmentVerdict::OffCpuIdle:
            return " <- BLOCKED: waiting with the CPU idle (expected for an SPI transfer)";
        case SegmentVerdict::OnCpu:
        case SegmentVerdict::Unknown:
        default:
            return "";
    }
}

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
                        const char* segmentLabel = "none", uint32_t segmentCpuUs = 0,
                        uint32_t segmentOtherUs = 0, uint32_t segmentIdleUs = 0) {
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
        s.worstSegmentOtherUs = segmentOtherUs;
        s.worstSegmentIdleUs = segmentIdleUs;
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

inline void recordHeldFrame(Stats& s) {
    s.heldFrames++;
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
    uint32_t worstSegmentOtherUs;
    uint32_t worstSegmentIdleUs;
    uint32_t overruns;
    uint32_t heldFrames;
};

inline Summary summarize(const Stats& s) {
    Summary out{};
    out.frames = s.frames;
    out.lateFrames = s.lateFrames;
    out.workMaxUs = s.workMaxUs;
    out.worstSegmentUs = s.worstSegmentUs;
    out.worstSegmentLabel = (s.worstSegmentLabel != nullptr) ? s.worstSegmentLabel : "none";
    out.worstSegmentCpuUs = s.worstSegmentCpuUs;
    out.worstSegmentOtherUs = s.worstSegmentOtherUs;
    out.worstSegmentIdleUs = s.worstSegmentIdleUs;
    out.overruns = s.overruns;
    out.heldFrames = s.heldFrames;

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
