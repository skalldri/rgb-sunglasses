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

    while (true) {
        // Update LED strips with current framebuffer contents
        // Monitor how long updating takes
        // Sleep appropriate amount to maintain target framerate
        int64_t startTicks = k_uptime_ticks();
        uint32_t wakeUs = k_cyc_to_us_near32(static_cast<uint32_t>(k_cycle_get_32()));

        float kTargetFrameIntervalMs = getLedConfig().getDisplayRateMs();

        // Longest stretch so far this frame between two points where the loop can yield.
        uint32_t worstSegUs = 0;
        uint32_t segStartUs = wakeUs;
        auto markSegment = [&]() {
            uint32_t now = k_cyc_to_us_near32(static_cast<uint32_t>(k_cycle_get_32()));
            uint32_t seg = now - segStartUs;  // unsigned wraparound is correct here
            if (seg > worstSegUs) {
                worstSegUs = seg;
            }
            segStartUs = now;
        };

        size_t bufferId = 0;
        ret = claimBufferForDisplay(bufferId);
        if (ret) {
            LOG_ERR("Error claiming display buffer!");
        }
        markSegment();  // claim can block on displayBufferMutex

        switch (atomic_get(&panelOutputMode)) {
            case PANEL_OUTPUT_NORMAL:
                led_strip_update_rgb(led_strip_0, led_0[bufferId], LED_STRIP_0_NUM_PIXELS);
                markSegment();
                led_strip_update_rgb(led_strip_1, led_1[bufferId], LED_STRIP_1_NUM_PIXELS);
                markSegment();
#if DT_HAS_ALIAS(led_strip_2) && !IS_ENABLED(CONFIG_STATUS_LED)
                led_strip_update_rgb(led_strip_2, led_2[bufferId], LED_STRIP_2_NUM_PIXELS);
                markSegment();
#endif
                break;
            case PANEL_OUTPUT_BLANK:
                // Same clocking as NORMAL, but all-black data
                led_strip_update_rgb(led_strip_0, blackFrame, LED_STRIP_0_NUM_PIXELS);
                markSegment();
                led_strip_update_rgb(led_strip_1, blackFrame, LED_STRIP_1_NUM_PIXELS);
                markSegment();
#if DT_HAS_ALIAS(led_strip_2) && !IS_ENABLED(CONFIG_STATUS_LED)
                led_strip_update_rgb(led_strip_2, blackFrame, LED_STRIP_2_NUM_PIXELS);
                markSegment();
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
        markSegment();

        int64_t endTicks = k_uptime_ticks();
        int64_t updateTicks = endTicks - startTicks;

        float updateTimeS = ((float)updateTicks) / ((float)CONFIG_SYS_CLOCK_TICKS_PER_SEC);
        float updateTimeMs = updateTimeS * 1000.0f;

        const uint32_t targetUs = static_cast<uint32_t>(kTargetFrameIntervalMs * 1000.0f);
        const uint32_t workUs =
            k_cyc_to_us_near32(static_cast<uint32_t>(k_cycle_get_32())) - wakeUs;

        // prevWakeUs == 0 only before the first frame, so there is no interval to record yet.
        const bool haveInterval = (prevWakeUs != 0);
        K_SPINLOCK(&sStatsLock) {
            led_stats_core::recordFrame(sStats, haveInterval, wakeUs - prevWakeUs, workUs,
                                        worstSegUs, targetUs);
        }
        prevWakeUs = wakeUs;

        if (updateTimeMs > kTargetFrameIntervalMs) {
            K_SPINLOCK(&sStatsLock) {
                led_stats_core::recordOverrun(sStats);
            }
            overrunsSinceLog++;
            // Rate-limited so a sustained overrun can't bury every other log line.
            const int64_t nowMs = k_uptime_get();
            if (nowMs - lastOverrunLogMs >= 5000) {
                LOG_WRN("Display overran the frame interval %u time(s) in the last %lld ms "
                        "(worst work %u us vs %u us budget) — cannot keep framerate",
                        overrunsSinceLog, nowMs - lastOverrunLogMs, workUs, targetUs);
                lastOverrunLogMs = nowMs;
                overrunsSinceLog = 0;
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
    shell_print(shell, "worst segment: %u us  (upper bound - includes SPI wait)",
                s.worstSegmentUs);
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