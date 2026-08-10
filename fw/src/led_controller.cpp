#include <configuration_provider.h>
#include <core_config.h>
#include <led_controller.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <algorithm>
#include <cstring>

#include "led_stats_core.h"

LOG_MODULE_REGISTER(led_controller, LOG_LEVEL_INF);

static ConfigurationProvider *sLedConfigProvider = nullptr;

void led_controller_set_config_provider(ConfigurationProvider *provider) {
    sLedConfigProvider = provider;
}

static ConfigurationProvider &getLedConfig() {
    if (!sLedConfigProvider) {
        sLedConfigProvider = &CoreConfig::getInstance();
    }
    return *sLedConfigProvider;
}

// Any measured interval at or beyond this is a 32-bit cycle-counter wrap artefact
// rather than a real stall (see markSegment()). The worst genuine display stall
// measured on proto0 is ~1.5 s (issue #312), so 10 s separates them with room.
static constexpr uint32_t kImplausibleUs = 10U * 1000U * 1000U;

void led_display_thread_func(void *a, void *b, void *c);

// Kernel-only thread: K_KERNEL_* skips the 1KB CONFIG_USERSPACE privileged stack;
// this stack can never host a K_USER thread.
K_KERNEL_THREAD_DEFINE(led_display_thread, CONFIG_APP_LED_DISPLAY_THREAD_STACK_SIZE,
                       led_display_thread_func, NULL, NULL, NULL,
                       CONFIG_APP_LED_DISPLAY_THREAD_PRIORITY, 0, 0);

// The display thread carries the only hard frame deadline in the system, so it must
// outrank the thread that produces the frames (issue #267 separated them; they used to
// share priority 6 and round-robin at the 20 ms timeslice). See fw/docs/threading.md.
BUILD_ASSERT(CONFIG_APP_LED_DISPLAY_THREAD_PRIORITY < CONFIG_APP_PATTERN_CONTROLLER_THREAD_PRIORITY,
             "led_display_thread must outrank pattern_controller_thread");
// Deliberately preemptible. Making this cooperative cannot fix the issue-#267 stutter: a
// running cooperative thread is never preempted by anything (should_preempt() in
// zephyr/kernel/include/kthread.h returns false unless the *running* thread is
// preemptible), so a cooperative display thread still could not preempt the cooperative
// Bluetooth or system-workqueue threads — it would only stop the BLE radio threads from
// preempting it, which is strictly worse. The fix is to keep the blockers out of the
// cooperative band, not to join them.
BUILD_ASSERT(CONFIG_APP_LED_DISPLAY_THREAD_PRIORITY >= 0 &&
                 CONFIG_APP_LED_DISPLAY_THREAD_PRIORITY < CONFIG_NUM_PREEMPT_PRIORITIES,
             "CONFIG_APP_LED_DISPLAY_THREAD_PRIORITY must be a valid preemptible priority");

// Refuse to build without CPU accounting rather than silently degrade: display_cpu_us()
// would return 0, and a zero CPU figure against a long wall time is exactly the signature
// this code reports as "PREEMPTED" — so a missing config would manufacture a confident
// wrong verdict. Same stance extension_host.cpp takes for the sandbox budget.
#if !defined(CONFIG_SCHED_THREAD_USAGE)
#error "led_controller needs CONFIG_SCHED_THREAD_USAGE for per-segment CPU attribution"
#endif

// CPU microseconds this thread has actually been given, for telling "slow" apart from
// "not running". A segment with a long wall time but near-zero CPU delta was preempted,
// not slow — the display thread is priority 2 but PREEMPTIBLE, and the cooperative
// Bluetooth threads (BT RX, BT HCI TX) outrank it and can hold the CPU for as long as
// they like. Same wall-vs-CPU split the extension sandbox uses to tell a runaway
// extension from a starved one; see extension_tick_budget.h.
static inline uint32_t display_cpu_us(void) {
    k_thread_runtime_stats_t st;
    if (k_thread_runtime_stats_get(led_display_thread, &st) != 0) {
        return 0;
    }
    return k_cyc_to_us_near32((uint32_t)st.execution_cycles);
}

// Where the wall time went, when it did not go to this thread. For CPU stats Zephyr
// defines execution_cycles = non-idle + idle, and total_cycles = non-idle only
// (kernel/thread.h). So across any window:
//
//   thread time  = d(total_cycles)                 - some other thread ran
//   idle time    = d(execution_cycles - total)     - the CPU had nothing to run
//   unaccounted  = d(wall) - d(execution_cycles)   - no accounting window was open
//
// Those three plus this thread's own CPU account for the whole window, which turns
// "something stalled us" into a number per suspect. IDLE time would mean nobody was
// running and this thread simply was not woken - a timer/wakeup bug, not contention,
// and a completely different fix.
//
// THE THIRD BUCKET IS NOT "ISR TIME" ON THIS ARCH. Zephyr calls z_sched_usage_stop()
// on interrupt entry only on x86/arc/arm64/xtensa/sparc; the Cortex-M path does not
// (kernel/thread.c z_thread_mark_switched_out/in, driven from PendSV). So on nRF5340
// an ISR's time is billed to whichever thread it interrupted, and the only way to
// land in this bucket is the PendSV switch window itself, where usage0 == 0 - which
// interrupts can extend, since PendSV is the lowest-priority exception. Read it as
// "context-switch churn", which heavy interrupt activity inflates, NOT as a direct
// measurement of time spent in handlers. Measured baseline: 0.28% of wall over a
// whole 229 s boot, and nearly all of that inside one stall.
struct CpuSplit {
    uint32_t threadUs;  // non-idle thread time (all threads, this one included)
    uint32_t idleUs;
};
static inline CpuSplit cpu_split(void) {
    k_thread_runtime_stats_t st;
    if (k_thread_runtime_stats_cpu_get(0, &st) != 0) {
        return CpuSplit{0, 0};
    }
    return CpuSplit{k_cyc_to_us_near32((uint32_t)st.total_cycles),
                    k_cyc_to_us_near32((uint32_t)(st.execution_cycles - st.total_cycles))};
}

// DIAGNOSTIC ONLY (#312) — names the "other thread" share of a stalled segment.
//
// cpu_split() proves time went to SOME other thread but not WHICH, and the two
// candidates need opposite fixes:
//
//   * a thread that OUTRANKS this one (priority < CONFIG_APP_LED_DISPLAY_THREAD_PRIORITY,
//     i.e. the cooperative band at -1..-16) means genuine preemption;
//   * only threads that RANK BELOW this one means this thread was not READY at all —
//     it was blocked inside the call, and the lower-priority threads ran legitimately.
//
// So the priority column, not the time column, is what settles it.
#if defined(CONFIG_THREAD_MONITOR) && defined(CONFIG_THREAD_NAME)
#define LED_SEGMENT_THREAD_ATTRIBUTION 1

// Only walk the thread list for a segment long enough to be the phenomenon under
// study; a normal frame's segments are sub-millisecond and must not pay for this.
static constexpr uint32_t kThreadReportThresholdUs = 100U * 1000U;
static constexpr size_t kMaxTrackedThreads = 40;

struct ThreadSnapshot {
    const struct k_thread *tid[kMaxTrackedThreads];
    uint64_t cycles[kMaxTrackedThreads];
    size_t count;
};

static ThreadSnapshot sSegStartThreads;
static ThreadSnapshot sSegEndThreads;

static void snapshot_thread_cb(const struct k_thread *thread, void *user_data) {
    ThreadSnapshot *snap = static_cast<ThreadSnapshot *>(user_data);
    if (snap->count >= kMaxTrackedThreads) {
        return;
    }
    k_thread_runtime_stats_t st;
    if (k_thread_runtime_stats_get(const_cast<k_tid_t>(thread), &st) != 0) {
        return;
    }
    snap->tid[snap->count] = thread;
    snap->cycles[snap->count] = st.execution_cycles;
    snap->count++;
}

static void snapshot_threads(ThreadSnapshot &out) {
    out.count = 0;
    // _unlocked so the walk does not hold the scheduler spinlock across every
    // k_thread_runtime_stats_get(); the set of threads is static after boot here.
    k_thread_foreach_unlocked(snapshot_thread_cb, &out);
}

// Diffs the two snapshots and logs the biggest consumers, worst first.
static void report_thread_deltas(const char *label, uint32_t segUs) {
    struct Row {
        const struct k_thread *tid;
        uint32_t us;
    };
    Row rows[kMaxTrackedThreads];
    size_t n = 0;

    for (size_t i = 0; i < sSegEndThreads.count && n < kMaxTrackedThreads; i++) {
        const struct k_thread *tid = sSegEndThreads.tid[i];
        // A thread created during the window has no start sample; count it whole.
        uint64_t before = 0;
        for (size_t j = 0; j < sSegStartThreads.count; j++) {
            if (sSegStartThreads.tid[j] == tid) {
                before = sSegStartThreads.cycles[j];
                break;
            }
        }
        uint64_t after = sSegEndThreads.cycles[i];
        if (after <= before) {
            continue;
        }
        uint32_t us = k_cyc_to_us_near32(static_cast<uint32_t>(after - before));
        if (us == 0) {
            continue;
        }
        rows[n++] = Row{tid, us};
    }

    LOG_WRN("#312 segment '%s' %u us — thread attribution (display prio %d):", label, segUs,
            CONFIG_APP_LED_DISPLAY_THREAD_PRIORITY);
    // Selection sort of the top few; n is ~27 and this runs only on a stall.
    const size_t kTop = 8;
    for (size_t slot = 0; slot < kTop && slot < n; slot++) {
        size_t best = slot;
        for (size_t i = slot + 1; i < n; i++) {
            if (rows[i].us > rows[best].us) {
                best = i;
            }
        }
        Row tmp = rows[slot];
        rows[slot] = rows[best];
        rows[best] = tmp;

        const char *name = k_thread_name_get(const_cast<k_tid_t>(rows[slot].tid));
        int prio = k_thread_priority_get(const_cast<k_tid_t>(rows[slot].tid));
        LOG_WRN("  %-16s prio %3d  %8u us  %s", (name != nullptr && name[0] != '\0') ? name : "?",
                prio, rows[slot].us,
                prio < CONFIG_APP_LED_DISPLAY_THREAD_PRIORITY ? "<-- OUTRANKS display" : "");
    }
}
#endif  // CONFIG_THREAD_MONITOR && CONFIG_THREAD_NAME

// Device Tree Node ID's for the LED strips
#define LED_STRIP_0_NODE_ID DT_ALIAS(led_strip_0)
#define LED_STRIP_1_NODE_ID DT_ALIAS(led_strip_1)

// String length from device tree
#define LED_STRIP_0_NUM_PIXELS DT_PROP(LED_STRIP_0_NODE_ID, chain_length)
#define LED_STRIP_1_NUM_PIXELS DT_PROP(LED_STRIP_1_NODE_ID, chain_length)

// Optional third strip (proto0 onboard LEDs) — omitted when status_led module owns it.
#if DT_HAS_ALIAS(led_strip_2) && !IS_ENABLED(CONFIG_STATUS_LED)
#define LED_STRIP_2_NODE_ID DT_ALIAS(led_strip_2)
#define LED_STRIP_2_NUM_PIXELS DT_PROP(LED_STRIP_2_NODE_ID, chain_length)
#endif

// Triple buffering to make things easy
constexpr size_t kNumDisplayBuffers = 3;

struct DisplayBufferState {
    bool inUse;
};

DisplayBufferState displayBufferState[kNumDisplayBuffers];

size_t lastRenderedDisplayBuffer = 0;  // Start with an arbitrary last rendered buffer
size_t outstandingRenderBuffers =
    0;  // Number of buffers currently claimed for rendering: cannot exceed 1

// Double-buffered framebuffers for rendering with
static struct led_rgb led_0[kNumDisplayBuffers][LED_STRIP_0_NUM_PIXELS];
static struct led_rgb led_1[kNumDisplayBuffers][LED_STRIP_1_NUM_PIXELS];
#if DT_HAS_ALIAS(led_strip_2) && !IS_ENABLED(CONFIG_STATUS_LED)
static struct led_rgb led_2[kNumDisplayBuffers][LED_STRIP_2_NUM_PIXELS];
#endif

// Hold this lock for _very_ short periods of time!
K_MUTEX_DEFINE(displayBufferMutex);

int claimBufferForRender(size_t &buffer) {
    k_mutex_lock(&displayBufferMutex, K_FOREVER);

    if (outstandingRenderBuffers >= 1) {
        k_mutex_unlock(&displayBufferMutex);

        LOG_ERR("Cannot claim another render buffer with any outstanding render buffers!");
        return -1;
    }

    // Find a not in-use buffer that is also not lastRenderedDisplayBuffer
    for (size_t i = 0; i < kNumDisplayBuffers; i++) {
        if (i == lastRenderedDisplayBuffer) {
            continue;  // Skip the lastRenderedDisplayBuffer
        }

        // If this buffer is not marked as inUse, then we can render into it!
        if (displayBufferState[i].inUse == false) {
            // Mark the buffer as in-use
            // TODO: should we mark who is using the buffer?
            displayBufferState[i].inUse = true;
            // Increase the outstandingRenderBuffers
            outstandingRenderBuffers++;
            // Provide buffer index as output
            buffer = i;

            k_mutex_unlock(&displayBufferMutex);
            return 0;
        }
    }

    k_mutex_unlock(&displayBufferMutex);
    LOG_ERR("Failed to locate a free buffer to use for rendering");

    return -2;
}

// Release the previously claimed buffer now that rendering into it is complete
int releaseBufferFromRender(const size_t buffer) {
    k_mutex_lock(&displayBufferMutex, K_FOREVER);

    // Sanity check: is this buffer index within bounds
    if (buffer >= kNumDisplayBuffers) {
        k_mutex_unlock(&displayBufferMutex);
        LOG_ERR("Invalid buffer index %u", buffer);
        return -1;
    }

    // Sanity check: buffer should be in-use
    if (!displayBufferState[buffer].inUse) {
        k_mutex_unlock(&displayBufferMutex);
        LOG_ERR("Buffer index %u is not in-use!", buffer);
        return -2;
    }

    // Mark buffer as not in-use
    displayBufferState[buffer].inUse = false;

    // Update the last rendered display buffer
    lastRenderedDisplayBuffer = buffer;

    // Decrement outstanding render buffer counter
    outstandingRenderBuffers--;

    k_mutex_unlock(&displayBufferMutex);
    return 0;
}

// Claim the best buffer to display right now, locking out anyone else from writing
// content into the buffer
int claimBufferForDisplay(size_t &buffer) {
    k_mutex_lock(&displayBufferMutex, K_FOREVER);

    // Sanity check: lastRenderedDisplayBuffer should not currently be in-use
    if (displayBufferState[lastRenderedDisplayBuffer].inUse) {
        LOG_WRN(
            "Attempting to get lastRenderedDisplayBuffer but it is already in use?? Frame tearing "
            "may ocurr");
        // Don't fail here: display thread _always_ needs a buffer, even if we show frame tearing
    }

    // Mark that buffer as in-use
    displayBufferState[lastRenderedDisplayBuffer].inUse = true;

    // Return the buffer idx we want the display thread to use
    buffer = lastRenderedDisplayBuffer;

    // Unlock to allow other threads access
    k_mutex_unlock(&displayBufferMutex);
    return 0;
}

// Release the previously claimed buffer now that display is complete
int releaseBufferFromDisplay(const size_t buffer) {
    k_mutex_lock(&displayBufferMutex, K_FOREVER);

    // Sanity check: is this buffer index within bounds
    if (buffer >= kNumDisplayBuffers) {
        k_mutex_unlock(&displayBufferMutex);
        LOG_ERR("Invalid buffer index %u", buffer);

        // We are headed towards display system deadlock at this point... but not much we can do

        return -1;
    }

    // Sanity check: buffer should currently be in-use
    if (!displayBufferState[buffer].inUse) {
        k_mutex_unlock(&displayBufferMutex);
        LOG_ERR("Buffer index %u is not currently in-use", buffer);

        return -2;
    }

    // Mark the buffer as no longer in-use
    displayBufferState[buffer].inUse = false;

    k_mutex_unlock(&displayBufferMutex);
    return 0;
}

// Pick the default LED config
const LedConfig *currentConfig = &kFrameLedConfig;

const LedConfig *get_current_led_config() {
    return currentConfig;
}

// This is the function which conains all the logic that allows the rest of the program
// to consider the LED strip as a standard zig-zag / X,Y display.
// This function will convert a pixel X/Y location in global display-space into a specific index
// within one of the two led arrays, and set the correct pixel value
// If the pixel isn't populated in the display, then the pixel value is discarded

// Return 0 if LED was set successfully
// Return -1 if LED is off the edge of the panel
// Return -2 if LED is not populated
//
// TODO: consider building a LUT with this information, possibly at runtime, to speedup rendering
int set_pixel_in_framebuffer(const LedConfig *config, size_t x, size_t y, size_t bufferId,
                             uint8_t red, uint8_t green, uint8_t blue) {
    // The display is zero-indexed, so valid X values are 0 -> (kDisplayWidth-1),
    // valid Y values are 0 -> (kDisplayHeight-1)
    if (x >= config->displayWidth || y >= config->displayHeight) {
        LOG_ERR("Pixel at %u, %u is off the edge of the display (%u x %u)", x, y,
                config->displayWidth, config->displayHeight);
        return -1;
    }

    size_t bankId = 0;
    struct led_rgb *bank = led_0[bufferId];
    size_t xWithinBank = x;  // Represent the x position relative to the left side of the bank

    // If x is past the mid-line, we are in LED_1
    if (x >= config->ledBankWidth) {
        bankId = 1;
        bank = led_1[bufferId];
        xWithinBank = x - config->ledBankWidth;
    }

    // Lookup how many LEDs are actually on this row
    const size_t ledsOnRow = config->ledsOnRow[y];

    // Get the index of the first LED in this row to speed up processing
    const size_t rowStartIndex = config->rowStartIndex[y];

    // Lookup row zig-zag direction
    const bool isLeftToRight = config->rowIsLeftToRight[y];

    size_t ledIndex = 0;

    const size_t missingLeds = (config->ledBankWidth - ledsOnRow);

    // Is this pixel actually populated?
    if (bankId == 0) {
        // In Bank0, LEDs on the right side of the panel are missing
        // If the xWithinBank is too large, the LED is missing
        if (xWithinBank >= ledsOnRow) {
            LOG_DBG("Pixel (%u, %u): xWithinBank %u >= ledsOnRow %u", x, y, xWithinBank, ledsOnRow);
            return -2;
        }

        // This LED is populated on this row! Calculate the correct index

        if (isLeftToRight) {
            // LOG_DBG("Bank %u Row %u is left-to-right", bankId, y);
            //  A Bank0, left-to-right row is very simple, just add the xWithinBank to the
            //  rowStartIndex No need to include "missingLeds" in this math, since we will bail-out
            //  early above if we would have moved far enough along to the row to touch a missing
            //  LED
            ledIndex = xWithinBank + rowStartIndex;
            // LOG_DBG("ledIndex = %u + %u", xWithinBank, rowStartIndex);
        } else {
            // LOG_DBG("Bank %u Row %u is right-to-left", bankId, y);
            //  A Bank0, right-to-left row is a bit more complex
            //  Say we have a right-to-left row with 6 LEDs in it:
            //
            //  ledsOnRow = 6
            //
            //  xWithinBank:  0  1  2  3  4  5
            //  LED index:   5  4  3  2  1  0
            //               [] [] [] [] [] []   <--  Start here
            //
            //  rowStartIndex will point us to the LED marked xWithinBank:5
            //  Say our desired xWithinBank = 2. To get the correct LED index,
            //  we must add 3 to the rowStartIndex. 3 = (ledsOnRow-1) - xWithinBank
            //
            //  We must also account for the "missingLeds" in this math, since we are
            //  starting on the side where the missingLEDs are. xWithinBank is guaranteed
            //  to always be > missingLeds by the above logic. We must subtract missingLeds from
            //  the result to avoid indexing too far into the row

            ledIndex = rowStartIndex + ((ledsOnRow - 1) - xWithinBank);
            // LOG_DBG("ledIndex = %u + ((%u - 1) - %u)", rowStartIndex, xWithinBank);
        }
    } else {
        // In Bank1, LEDs on the left side of the panel are missing
        // If the xWithinBank is too small, the LED is missing
        if (xWithinBank < missingLeds) {
            // LOG_DBG(
            //     "Pixel (%u, %u): xWithinBank %u < missingLeds %u",
            //     x,
            //     y,
            //     xWithinBank,
            //     missingLeds);
            return -2;
        }

        // This LED is populated on this row! Calculate the correct index

        if (isLeftToRight) {
            // LOG_DBG("Bank %u Row %u is left-to-right", bankId, y);
            //  Bank1 left-to-right is relatively easy: just need to subtract missingLeds
            //  from the xWithinBank value to avoid going too far, since the missing LEDs are
            //  on the left-side of the display now
            ledIndex = rowStartIndex + (xWithinBank - missingLeds);
            // LOG_DBG("ledIndex = %u + (%u - %u)", rowStartIndex, xWithinBank, missingLeds);
        } else {
            // LOG_DBG("Bank %u Row %u is right-to-left", bankId, y);
            //  A Bank1, right-to-left row is the same as a Bank0 right-to-left row
            //  Say we have a right-to-left row with 6 LEDs in it:
            //
            //  ledsOnRow = 6
            //
            //  xWithinBank:  0  1  2  3  4  5
            //  LED index:   5  4  3  2  1  0
            //               [] [] [] [] [] []   <--  Start here
            //
            //  rowStartIndex will point us to the LED marked xWithinBank:5
            //  Say our desired xWithinBank = 2. To get the correct LED index,
            //  we must add 3 to the rowStartIndex. 3 = (ledsOnRow-1) - xWithinBank
            //
            //  We need to account for "missingLeds" in this codepath. The xWithinBank
            //  value needs to be corrected for the fact that our row actually starts with some
            //  LEDs missing, so subtract that value from the xWithinBank.
            //
            //  We are also guaranteed not to hit this code if xWithinBank
            ledIndex = rowStartIndex + ((ledsOnRow - 1) - (xWithinBank - missingLeds));
            // LOG_DBG("ledIndex = %u + ((%u - 1) - (%u - %u))", rowStartIndex, ledsOnRow,
            // xWithinBank, missingLeds);
        }
    }

    // LOG_DBG("Picked LED index %u in bank %u", ledIndex, bankId);

    // We have our LED index within the bank! Set the color
    bank[ledIndex].r = red;
    bank[ledIndex].g = green;
    bank[ledIndex].b = blue;

    return 0;
}

// Panel output mode (issue #172 power experiment): NORMAL clocks rendered
// frames out every display tick; BLANK keeps clocking every tick but sends
// all-black pixel data; OFF skips led_strip_update_rgb() entirely, so the
// WS2812 data lines idle low and the panel's serial shift registers stop
// clocking. Buffer claim/release runs identically in all three modes so the
// render pipeline is unaffected and the only variable is the SPI output.
enum PanelOutputMode {
    PANEL_OUTPUT_NORMAL = 0,
    PANEL_OUTPUT_BLANK = 1,
    PANEL_OUTPUT_OFF = 2,
};
static atomic_t panelOutputMode = ATOMIC_INIT(PANEL_OUTPUT_NORMAL);

// All-black frame for PANEL_OUTPUT_BLANK, sized for the longest strip.
// Zero-initialized (.bss) and never written after that (non-const only
// because led_strip_update_rgb takes a mutable pixel pointer).
constexpr size_t kMaxStripPixels = std::max<size_t>(
    {LED_STRIP_0_NUM_PIXELS, LED_STRIP_1_NUM_PIXELS,
#if DT_HAS_ALIAS(led_strip_2) && !IS_ENABLED(CONFIG_STATUS_LED)
     LED_STRIP_2_NUM_PIXELS,
#endif
     0});
static struct led_rgb blackFrame[kMaxStripPixels];

// Frame-pacing instrumentation (issue #267). The tracking logic — and the reasoning about
// why wake-to-wake interval rather than work time is the metric that matches the reported
// stutter — lives in led_stats_core.h, where the native_sim suite can test it. This file
// supplies the clock reads, the lock, and the shell output.
//
// worstSegmentUs is the safety metric for ever running this thread cooperatively: the
// longest stretch between two points where the loop can yield. NOTE it is an upper bound,
// not pure CPU time — the per-strip segments include the SPI transfer wait, during which
// the thread is blocked and other threads do run. (It measured 30.5 ms, which is one of the
// reasons the display thread stayed preemptible; see fw/docs/threading.md.)
//
// Written once per frame by led_display_thread, read by the shell. A spinlock rather than
// per-field atomics so a `led_stats` dump is a single coherent snapshot (and so the 64-bit
// sum can't tear on this 32-bit core).
struct k_spinlock sStatsLock;
led_stats_core::Stats sStats = {};

void statsReset() {
    K_SPINLOCK(&sStatsLock) {
        led_stats_core::reset(sStats);
    }
}

void led_display_thread_func(void *a, void *b, void *c) {
    const struct device *led_strip_0 = DEVICE_DT_GET(LED_STRIP_0_NODE_ID);
    const struct device *led_strip_1 = DEVICE_DT_GET(LED_STRIP_1_NODE_ID);
#if DT_HAS_ALIAS(led_strip_2) && !IS_ENABLED(CONFIG_STATUS_LED)
    const struct device *led_strip_2 = DEVICE_DT_GET(LED_STRIP_2_NODE_ID);
#endif

    if (!device_is_ready(led_strip_0)) {
        LOG_ERR("Device %s is not ready", led_strip_0->name);
        return;
    }

    if (!device_is_ready(led_strip_1)) {
        LOG_ERR("Device %s is not ready", led_strip_1->name);
        return;
    }

#if DT_HAS_ALIAS(led_strip_2) && !IS_ENABLED(CONFIG_STATUS_LED)
    if (!device_is_ready(led_strip_2)) {
        LOG_ERR("Device %s is not ready", led_strip_2->name);
        return;
    }
#endif

    // Set the entire LED bank to NULL before starting
    for (size_t i = 0; i < kNumDisplayBuffers; i++) {
        memset(led_0[i], 0, sizeof(struct led_rgb) * LED_STRIP_0_NUM_PIXELS);
        memset(led_1[i], 0, sizeof(struct led_rgb) * LED_STRIP_1_NUM_PIXELS);
#if DT_HAS_ALIAS(led_strip_2) && !IS_ENABLED(CONFIG_STATUS_LED)
        memset(led_2[i], 0, sizeof(struct led_rgb) * LED_STRIP_2_NUM_PIXELS);
#endif
    }

    // All display buffers start not-in-use
    for (auto &bufferState : displayBufferState) {
        bufferState.inUse = false;
    }

    // LOG_INF("Starting LED display controller");

    int ret;

    // Wake-to-wake tracking. 0 = no previous frame yet, so the first iteration only seeds.
    uint32_t prevWakeUs = 0;
    // Overrun logging is rate-limited: at the 33.3 ms default this fires once per frame
    // under sustained load, which is exactly the per-tick log spam PR #110 banned. Count
    // them instead and report periodically.
    int64_t lastOverrunLogMs = 0;
    uint32_t overrunsSinceLog = 0;
    // Worst overrunning frame in the CURRENT rate-limit window, reset with the
    // counter below. Tracking these per-frame instead made the log incoherent: the
    // count summarised 5 s while the label described whichever overrunning frame
    // happened to land when the timer expired, so 149 marginal SPI overruns plus one
    // 25 ms mutex stall could print the SPI label and send the reader to the wrong
    // subsystem.
    uint32_t windowWorstFrameUs = 0;
    uint32_t windowWorstSegUs = 0;
    const char *windowWorstSegLabel = "none";
    uint32_t windowWorstSegCpuUs = 0;
    uint32_t windowWorstOtherUs = 0;
    uint32_t windowWorstIdleUs = 0;
    uint32_t windowWorstIsrUs = 0;

    while (true) {
        // Update LED strips with current framebuffer contents
        // Monitor how long updating takes
        // Sleep appropriate amount to maintain target framerate
        int64_t startTicks = k_uptime_ticks();
        uint32_t wakeUs = k_cyc_to_us_near32(static_cast<uint32_t>(k_cycle_get_32()));

        float kTargetFrameIntervalMs = getLedConfig().getDisplayRateMs();

        // Longest stretch so far this frame between two points where the loop can yield.
        // The label rides along so an overrun can point at a call (issue #312): a single
        // aggregate number told us ~the whole frame sat in one segment, but not which.
        //
        // READ THE LABEL AS "WHERE THE TIME LANDED", NOT "WHAT WAS SLOW". These are
        // wall-clock deltas on a PREEMPTIBLE thread, not time spent inside the call. The
        // cooperative Bluetooth threads (BT RX at -8, HCI TX at -9) outrank this one, so
        // if they run for 20 ms mid-transfer, markSegment("strip1") bills all 20 ms to
        // strip 1 and the reader goes hunting in the WS2812 driver for a stall that was
        // really CPU starvation. Three explanations fit any label: a genuinely slow call,
        // a lock held by someone else, or preemption.
        uint32_t worstSegUs = 0;
        const char *worstSegLabel = "none";
        uint32_t worstSegCpuUs = 0;
        uint32_t worstSegOtherUs = 0;  // other threads
        uint32_t worstSegIdleUs = 0;   // CPU had nothing to run
        uint32_t worstSegIsrUs = 0;    // interrupt context
        // Seeded AFTER the preamble reads below, not from wakeUs — otherwise the first
        // segment silently includes k_uptime_ticks(), the cycle read, and the virtual
        // getDisplayRateMs() call, all billed to a label that says "claim".
        uint32_t segStartUs = k_cyc_to_us_near32(static_cast<uint32_t>(k_cycle_get_32()));
        uint32_t segStartCpuUs = display_cpu_us();
        CpuSplit segStartSplit = cpu_split();
#if defined(LED_SEGMENT_THREAD_ATTRIBUTION)
        snapshot_threads(sSegStartThreads);
#endif
        auto markSegment = [&](const char *label) {
            uint32_t now = k_cyc_to_us_near32(static_cast<uint32_t>(k_cycle_get_32()));
            uint32_t nowCpu = display_cpu_us();
            CpuSplit nowSplit = cpu_split();
            uint32_t seg = now - segStartUs;
            uint32_t segCpu = nowCpu - segStartCpuUs;
            uint32_t segThread = nowSplit.threadUs - segStartSplit.threadUs;
            uint32_t segIdle = nowSplit.idleUs - segStartSplit.idleUs;
            // Whatever is left belongs to no thread at all: interrupt context.
            uint32_t segIsr = (seg > segThread + segIdle) ? seg - segThread - segIdle : 0;
            segStartUs = now;
            segStartCpuUs = nowCpu;
            segStartSplit = nowSplit;
            // k_cycle_get_32() wraps every ~36.4 h at 32768 Hz, and k_cyc_to_us_near32()
            // truncates AFTER 64-bit math — so the derived microsecond timeline is NOT
            // linear mod 2^32 and one frame per wrap yields a garbage delta (~2.07e9 us).
            // Unfiltered that pins led_stats' all-time maxima until `led_stats reset` and
            // would print a confident, fictional segment label. Anything past this bound
            // is a wrap, not a stall — the worst real stall measured is ~1.5 s (#312).
            if (seg >= kImplausibleUs) {
                return;
            }
#if defined(LED_SEGMENT_THREAD_ATTRIBUTION)
            // The snapshot pair brackets THIS segment exactly (re-snapshotted below for
            // the next one), so the deltas cannot be contaminated by neighbouring
            // segments in the same frame.
            if (seg >= kThreadReportThresholdUs) {
                snapshot_threads(sSegEndThreads);
                report_thread_deltas(label, seg);
            }
            // Re-seed unconditionally: a cheap segment still has to advance the window,
            // or the next segment's delta would silently include it.
            snapshot_threads(sSegStartThreads);
#endif
            if (seg > worstSegUs) {
                worstSegUs = seg;
                worstSegLabel = label;
                worstSegCpuUs = segCpu;
                worstSegOtherUs = (segThread > segCpu) ? segThread - segCpu : 0;
                worstSegIdleUs = segIdle;
                worstSegIsrUs = segIsr;
            }
        };

        size_t bufferId = 0;
        ret = claimBufferForDisplay(bufferId);
        if (ret) {
            LOG_ERR("Error claiming display buffer!");
        }
        markSegment("claim");  // claim can block on displayBufferMutex

        switch (atomic_get(&panelOutputMode)) {
            case PANEL_OUTPUT_NORMAL:
                led_strip_update_rgb(led_strip_0, led_0[bufferId], LED_STRIP_0_NUM_PIXELS);
                markSegment("strip0");
                led_strip_update_rgb(led_strip_1, led_1[bufferId], LED_STRIP_1_NUM_PIXELS);
                markSegment("strip1");
#if DT_HAS_ALIAS(led_strip_2) && !IS_ENABLED(CONFIG_STATUS_LED)
                led_strip_update_rgb(led_strip_2, led_2[bufferId], LED_STRIP_2_NUM_PIXELS);
                markSegment("strip2");
#endif
                break;
            case PANEL_OUTPUT_BLANK:
                // Same clocking as NORMAL, but all-black data
                led_strip_update_rgb(led_strip_0, blackFrame, LED_STRIP_0_NUM_PIXELS);
                markSegment("strip0-blank");
                led_strip_update_rgb(led_strip_1, blackFrame, LED_STRIP_1_NUM_PIXELS);
                markSegment("strip1-blank");
#if DT_HAS_ALIAS(led_strip_2) && !IS_ENABLED(CONFIG_STATUS_LED)
                led_strip_update_rgb(led_strip_2, blackFrame, LED_STRIP_2_NUM_PIXELS);
                markSegment("strip2-blank");
#endif
                break;
            case PANEL_OUTPUT_OFF:
            default:
                // No SPI traffic at all this tick
                break;
        }

        ret = releaseBufferFromDisplay(bufferId);
        if (ret) {
            LOG_ERR("Error releasing display buffer!");
        }
        markSegment("release");

        int64_t endTicks = k_uptime_ticks();
        int64_t updateTicks = endTicks - startTicks;

        float updateTimeS = ((float)updateTicks) / ((float)CONFIG_SYS_CLOCK_TICKS_PER_SEC);
        float updateTimeMs = updateTimeS * 1000.0f;

        const uint32_t targetUs = static_cast<uint32_t>(kTargetFrameIntervalMs * 1000.0f);
        const uint32_t workUs =
            k_cyc_to_us_near32(static_cast<uint32_t>(k_cycle_get_32())) - wakeUs;

        // prevWakeUs == 0 only before the first frame, so there is no interval to record yet.
        const bool haveInterval = (prevWakeUs != 0);
        // Drop the whole frame's sample across a cycle-counter wrap rather than let a
        // ~2.07e9 us artefact become led_stats' permanent all-time maximum (it is a
        // running max, only cleared by `led_stats reset`).
        const uint32_t intervalUs = wakeUs - prevWakeUs;
        const bool sampleSane = workUs < kImplausibleUs && (!haveInterval || intervalUs < kImplausibleUs);
        if (sampleSane) {
            K_SPINLOCK(&sStatsLock) {
                led_stats_core::recordFrame(sStats, haveInterval, intervalUs, workUs, worstSegUs,
                                            targetUs, worstSegLabel, worstSegCpuUs);
            }
        }
        prevWakeUs = wakeUs;

        if (updateTimeMs > kTargetFrameIntervalMs) {
            K_SPINLOCK(&sStatsLock) {
                led_stats_core::recordOverrun(sStats);
            }
            overrunsSinceLog++;
            if (sampleSane && workUs > windowWorstFrameUs) {
                windowWorstFrameUs = workUs;
                windowWorstSegUs = worstSegUs;
                windowWorstSegLabel = worstSegLabel;
                windowWorstSegCpuUs = worstSegCpuUs;
                windowWorstOtherUs = worstSegOtherUs;
                windowWorstIdleUs = worstSegIdleUs;
                windowWorstIsrUs = worstSegIsrUs;
            }
            // Rate-limited so a sustained overrun can't bury every other log line.
            const int64_t nowMs = k_uptime_get();
            if (nowMs - lastOverrunLogMs >= 5000) {
                /* Only name a segment when it actually accounts for the frame. The marked
                 * segments do NOT tile the frame — the stats/spinlock tail after "release"
                 * is unmeasured — so a 25 ms preemption there would leave an ordinary
                 * ~1.2 ms SPI segment as "longest" while explaining 4% of the frame.
                 * "Unaccounted" is itself the useful finding: it says look outside the
                 * LED path. This also covers the all-segments-round-to-zero case (e.g.
                 * PANEL_OUTPUT_OFF, where no SPI runs at all), which would otherwise
                 * print a placeholder 'none' label.
                 *
                 * `worst frame work` is deliberately not called the compared quantity:
                 * the overrun test above is on tick-quantized updateTimeMs, while this is
                 * a cycle-clock read, and near the threshold the two can disagree. */
                const bool segmentExplainsFrame =
                    windowWorstSegUs > 0 && windowWorstSegUs >= windowWorstFrameUs / 2;
                if (segmentExplainsFrame) {
                    LOG_WRN("Display overran the frame interval %u time(s) in the last %lld ms "
                            "(worst frame work %u us vs %u us budget; longest segment '%s' "
                            "%u us wall = %u us self + %u us other-thread + %u us idle + "
                            "%u us unaccounted)%s — cannot keep framerate",
                            overrunsSinceLog, nowMs - lastOverrunLogMs, windowWorstFrameUs,
                            targetUs, windowWorstSegLabel, windowWorstSegUs,
                            windowWorstSegCpuUs, windowWorstOtherUs, windowWorstIdleUs,
                            windowWorstIsrUs,
                            // Name the dominant consumer outright; each points somewhere
                            // different. Unaccounted = context-switch churn (see the
                            // CpuSplit comment — NOT a direct ISR measurement on Cortex-M);
                            // idle means nobody ran and we simply were not woken.
                            (windowWorstIsrUs > windowWorstSegUs / 2)   ? " <- UNACCOUNTED (switch churn)"
                            : (windowWorstIdleUs > windowWorstSegUs / 2) ? " <- IDLE (not woken)"
                            : (windowWorstOtherUs > windowWorstSegUs / 2)
                                ? " <- ANOTHER THREAD"
                                : "");
                } else {
                    LOG_WRN("Display overran the frame interval %u time(s) in the last %lld ms "
                            "(worst frame work %u us vs %u us budget; NO segment accounts for "
                            "it — longest was '%s' %u us wall / %u us self, so look outside "
                            "the LED path) — cannot keep framerate",
                            overrunsSinceLog, nowMs - lastOverrunLogMs, windowWorstFrameUs,
                            targetUs, windowWorstSegLabel, windowWorstSegUs,
                            windowWorstSegCpuUs);
                }
                lastOverrunLogMs = nowMs;
                overrunsSinceLog = 0;
                windowWorstFrameUs = 0;
                windowWorstSegUs = 0;
                windowWorstSegLabel = "none";
                windowWorstSegCpuUs = 0;
                windowWorstOtherUs = 0;
                windowWorstIdleUs = 0;
                windowWorstIsrUs = 0;
            }
            // Yield unconditionally even when we blew the budget. Previously this branch
            // looped with no sleep at all, which is survivable only because this thread is
            // preemptible; the moment it is given a cooperative priority (which is the fix
            // for issue #267) a single overrunning frame would wedge the whole system,
            // since nothing can preempt a cooperative thread that never yields.
            k_msleep(1);
        } else {
            // Sleep for however much time is left
            k_msleep(kTargetFrameIntervalMs - updateTimeMs);
        }
    }

    return;
}

#if defined(CONFIG_SHELL)
#include <zephyr/shell/shell.h>

// Issue #172 power experiment: switch what the display thread clocks out to
// the panel, so panel power draw can be compared A/B/C from the serial shell
// with everything else (render thread, display tick rate) unchanged.
static int cmd_led_output_mode(const struct shell *shell, size_t argc, char **argv, void *data) {
    atomic_set(&panelOutputMode, (atomic_val_t)(intptr_t)data);
    shell_print(shell, "panel output mode: %s", argv[0]);
    return 0;
}

SHELL_SUBCMD_DICT_SET_CREATE(
    sub_led_output, cmd_led_output_mode,
    (on, PANEL_OUTPUT_NORMAL, "Clock rendered frames to the panel (normal operation)"),
    (blank, PANEL_OUTPUT_BLANK, "Keep clocking every display tick, but send all-black data"),
    (off, PANEL_OUTPUT_OFF, "Stop clocking data into the panel entirely (data lines idle)"));

SHELL_CMD_REGISTER(led_output, &sub_led_output,
                   "Panel serial-output control (issue #172 power experiment)", NULL);

// Frame-pacing counters (issue #267). `led_stats` prints, `led_stats reset` zeroes — the
// intended use is reset, drive a load (app connect + GATT discovery, ext select, a settings
// flush), then print. The interval figures are what the reported stutter actually is; the
// per-frame work figure is what the old LOG_WRN measured and is not the same thing.
static int cmd_led_stats(const struct shell *shell, size_t argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "reset") == 0) {
        statsReset();
        shell_print(shell, "led stats reset");
        return 0;
    }

    led_stats_core::Stats snapshot;
    K_SPINLOCK(&sStatsLock) {
        snapshot = sStats;
    }

    if (snapshot.frames == 0) {
        shell_print(shell, "no frames recorded yet");
        return 0;
    }

    const led_stats_core::Summary s = led_stats_core::summarize(snapshot);
    const uint32_t targetUs = static_cast<uint32_t>(getLedConfig().getDisplayRateMs() * 1000.0f);

    shell_print(shell, "frames:        %u", s.frames);
    shell_print(shell, "target:        %u us/frame", targetUs);
    shell_print(shell, "interval:      min %u us  avg %u us  max %u us", s.intervalMinUs,
                s.intervalAvgUs, s.intervalMaxUs);
    shell_print(shell, "late (>%ux):    %u frame(s)", led_stats_core::kLateFrameMultiple,
                s.lateFrames);
    shell_print(shell, "work max:      %u us", s.workMaxUs);
    // wall + cpu + which call, so an unattended soak stays diagnosable after the
    // rate-limited overrun warning has scrolled out of the serial backlog.
    shell_print(shell, "worst segment: %u us wall / %u us cpu  in '%s'%s", s.worstSegmentUs,
                s.worstSegmentCpuUs, s.worstSegmentLabel,
                (s.worstSegmentUs > 4 * (s.worstSegmentCpuUs + 1))
                    ? "  <- PREEMPTED (wall >> cpu): the thread was not running, not slow"
                    : "");
    shell_print(shell, "overruns:      %u", s.overruns);
    return 0;
}

SHELL_CMD_ARG_REGISTER(led_stats, NULL,
                       "Display frame-pacing stats (issue #267). 'led_stats reset' to zero.",
                       cmd_led_stats, 1, 1);
#endif  // CONFIG_SHELL

#if defined(CONFIG_SHELL) && 0
#include <zephyr/shell/shell.h>

static int cmd_led_test(const struct shell *shell, size_t argc, char **argv, void *data) {
    // Set the entire LED bank to NULL before the test
    memset(led_0[0], 0, sizeof(struct led_rgb) * LED_STRIP_0_NUM_PIXELS);
    memset(led_1[0], 0, sizeof(struct led_rgb) * LED_STRIP_1_NUM_PIXELS);

    int ret;

    // Set first pixel in left / right banks and check result
    ret = set_pixel_in_framebuffer(&kFrameLedConfig, 0, 0, 0 /* buffer */, 255, 0, 0);

    if (ret) {
        // shell_error(shell, "Unexpected return code setting LED in buffer! %d", ret);
        return -EFAULT;
    }

    if (led_0[0][0].r != 255) {
        // shell_error(shell, "Bank 0 Index 0 has wrong color!");
        return -EFAULT;
    }

    ret = set_pixel_in_framebuffer(&kFrameLedConfig, 20, 0, 0 /* buffer */, 255, 0, 0);

    if (ret) {
        // shell_error(shell, "Unexpected return code setting LED in buffer! %d", ret);
        return -EFAULT;
    }

    if (led_1[0][0].r != 255) {
        // shell_error(shell, "Bank 1 Index 0 has wrong color!");
        return -EFAULT;
    }

    // Try setting a pixel off the edge of the display
    ret = set_pixel_in_framebuffer(&kFrameLedConfig, 40, 0, 0 /* buffer */, 255, 0, 0);

    if (ret != -1) {
        // shell_error(shell, "Unexpected return code setting off-display LED! %d", ret);
        return -EFAULT;
    }

    ret = set_pixel_in_framebuffer(&kFrameLedConfig, 0, 12, 0 /* buffer */, 255, 0, 0);

    if (ret != -1) {
        // shell_error(shell, "Unexpected return code setting off-display LED! %d", ret);
        return -EFAULT;
    }

    // Try setting a pixel that isn't populated
    ret = set_pixel_in_framebuffer(
        &kFrameLedConfig, 16, 9, 0 /* buffer */, 255, 0,
        0);  // Row 9 has 16 LEDs so this should be within the nose region of Bank0

    if (ret != -2) {
        // shell_error(shell, "Unexpected return code setting unpopulated LED! %d", ret);
        return -EFAULT;
    }

    ret = set_pixel_in_framebuffer(
        &kFrameLedConfig, 20, 11, 0 /* buffer */, 255, 0,
        0);  // Row 11 has 15 LEDs so this should be within the rose region of Bank1

    if (ret != -2) {
        // shell_error(shell, "Unexpected return code setting unpopulated LED! %d", ret);
        return -EFAULT;
    }

    // Set the bottom-right LED on the display, which is in Bank1
    ret = set_pixel_in_framebuffer(&kFrameLedConfig, 39, 11, 0 /* buffer */, 255, 0, 0);

    if (ret) {
        // shell_error(shell, "Unexpected return code setting last display LED! %d", ret);
        return -EFAULT;
    }

    if (led_1[0][201].r != 255) {
        // shell_error(shell, "Bank 1 Index %u has wrong color!", 201);
        return -EFAULT;
    }

    // Set the last LED in Bank1, which is 25,11
    ret = set_pixel_in_framebuffer(&kFrameLedConfig, 25, 11, 0 /* buffer */, 255, 0, 0);

    if (ret) {
        // shell_error(shell, "Unexpected return code setting last Bank1 LED! %d", ret);
        return -EFAULT;
    }

    if (led_1[0][LED_STRIP_1_NUM_PIXELS - 1].r != 255) {
        // shell_error(shell, "Bank 1 Index %u has wrong color!", LED_STRIP_1_NUM_PIXELS - 1);
        return -EFAULT;
    }

    // Set the last LED in Bank0, which is 0,11
    ret =  // set_pixel_in_framebuffer(&kFrameLedConfig, 0, 11, 0 /* buffer */, 255, 0, 0);

        if (ret) {
        // shell_error(shell, "Unexpected return code setting last Bank0 LED! %d", ret);
        return -EFAULT;
    }

    if (led_0[0][LED_STRIP_0_NUM_PIXELS - 1].r != 255) {
        // shell_error(shell, "Bank 0 Index %u has wrong color!", LED_STRIP_0_NUM_PIXELS - 1);
        return -EFAULT;
    }

    // Set the last LED in the last row of bank0, which is 14,11
    ret = set_pixel_in_framebuffer(&kFrameLedConfig, 14, 11, 0 /* buffer */, 255, 0, 0);

    if (ret) {
        // shell_error(shell, "Unexpected return code setting last Bank0 LED! %d", ret);
        return -EFAULT;
    }

    if (led_0[0][201].r != 255) {
        // shell_error(shell, "Bank 0 Index %u has wrong color!", 201);
        return -EFAULT;
    }

    // shell_print(shell, "All tests passed!");

    return 0;
}

static int cmd_led_config(const struct shell *shell, size_t argc, char **argv, void *data) {
    int selection = (int)data;

    if (selection == 0) {
        // shell_print(shell, "Setting LED controller to frame config");
        currentConfig = &kFrameLedConfig;
    } else {
        // shell_error(shell, "Unknown config value: %d", selection);
        return -ENOEXEC;
    }

    return 0;
}

SHELL_SUBCMD_DICT_SET_CREATE(sub_led_config, cmd_led_config,
                             (frame, 0, "Set LED controller into frames mode"));

// Subcommands for "led"
SHELL_STATIC_SUBCMD_SET_CREATE(sub_led,
                               SHELL_CMD(config, &sub_led_config, "Pick LED configuration", NULL),
                               SHELL_CMD(test, NULL, "Run LED controller unit tests", cmd_led_test),
                               SHELL_SUBCMD_SET_END);

/* Creating root (level 0) command "led" */
SHELL_CMD_REGISTER(led, &sub_led, "LED commands", NULL);
#endif  // CONFIG_SHELL