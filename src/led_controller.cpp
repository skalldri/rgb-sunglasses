#include <led_controller.h>

#include <zephyr/kernel.h>
#include <zephyr/init.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#include <zephyr/drivers/led.h>
#include <zephyr/drivers/led_strip.h>

#include <zephyr/logging/log.h>

#include <algorithm>

LOG_MODULE_REGISTER(led_controller, LOG_LEVEL_INF);

void led_display_thread_func(void* a, void* b, void* c);

K_THREAD_DEFINE(
    led_display_thread, 
    4096,
    led_display_thread_func,
    NULL,
    NULL,
    NULL,
    6,
    0,
    0
);

// Device Tree Node ID's for the LED strips
#define LED_STRIP_0_NODE_ID DT_ALIAS(led_strip_0)
#define LED_STRIP_1_NODE_ID DT_ALIAS(led_strip_1)

// String length from device tree
#define LED_STRIP_0_NUM_PIXELS DT_PROP(LED_STRIP_0_NODE_ID, chain_length)
#define LED_STRIP_1_NUM_PIXELS DT_PROP(LED_STRIP_1_NODE_ID, chain_length)

// Triple buffering to make things easy
constexpr size_t kNumDisplayBuffers = 3;

struct DisplayBufferState {
    bool inUse;
};

DisplayBufferState displayBufferState[kNumDisplayBuffers];

size_t lastRenderedDisplayBuffer = 0; // Start with an arbitrary last rendered buffer
size_t outstandingRenderBuffers = 0; // Number of buffers currently claimed for rendering: cannot exceed 1

// Double-buffered framebuffers for rendering with
static struct led_rgb led_0[kNumDisplayBuffers][LED_STRIP_0_NUM_PIXELS];
static struct led_rgb led_1[kNumDisplayBuffers][LED_STRIP_1_NUM_PIXELS];

// Hold this lock for _very_ short periods of time!
K_MUTEX_DEFINE(displayBufferMutex);

int claimBufferForRender(size_t& buffer) {
    k_mutex_lock(&displayBufferMutex, K_FOREVER);

    if (outstandingRenderBuffers >= 1) {
        k_mutex_unlock(&displayBufferMutex);

        LOG_ERR("Cannot claim another render buffer with any outstanding render buffers!");
        return -1;
    }

    // Find a not in-use buffer that is also not lastRenderedDisplayBuffer
    for (size_t i = 0; i < kNumDisplayBuffers; i++) {
        if (i == lastRenderedDisplayBuffer) {
            continue; // Skip the lastRenderedDisplayBuffer
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
int claimBufferForDisplay(size_t& buffer) {
    k_mutex_lock(&displayBufferMutex, K_FOREVER);

    // Sanity check: lastRenderedDisplayBuffer should not currently be in-use
    if (displayBufferState[lastRenderedDisplayBuffer].inUse) {
        LOG_WRN("Attempting to get lastRenderedDisplayBuffer but it is already in use?? Frame tearing may ocurr");
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
const LedConfig* currentConfig = &kFrameLedConfig;
// const LedConfig* currentConfig = &kDevKitLedConfig;

const LedConfig* get_current_led_config() {
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
int set_pixel_in_framebuffer(const LedConfig* config, size_t x, size_t y, size_t bufferId, uint8_t red, uint8_t green, uint8_t blue) {
    // The display is zero-indexed, so valid X values are 0 -> (kDisplayWidth-1),
    // valid Y values are 0 -> (kDisplayHeight-1)
    if (x >= config->displayWidth || y >= config->displayHeight) {
        LOG_ERR("Pixel at %u, %u is off the edge of the display (%u x %u)", x, y, config->displayWidth, config->displayHeight);
        return -1;
    }

    size_t bankId = 0;
    struct led_rgb* bank = led_0[bufferId];
    size_t xWithinBank = x; // Represent the x position relative to the left side of the bank

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
            LOG_DBG(
                "Pixel (%u, %u): xWithinBank %u >= ledsOnRow %u", 
                x, 
                y, 
                xWithinBank, 
                ledsOnRow);
            return -2;
        }

        // This LED is populated on this row! Calculate the correct index

        if (isLeftToRight) {
            LOG_DBG("Bank %u Row %u is left-to-right", bankId, y);
            // A Bank0, left-to-right row is very simple, just add the xWithinBank to the rowStartIndex
            // No need to include "missingLeds" in this math, since we will bail-out early above
            // if we would have moved far enough along to the row to touch a missing LED
            ledIndex = xWithinBank + rowStartIndex;
            LOG_DBG("ledIndex = %u + %u", xWithinBank, rowStartIndex);
        } else {
            LOG_DBG("Bank %u Row %u is right-to-left", bankId, y);
            // A Bank0, right-to-left row is a bit more complex
            // Say we have a right-to-left row with 6 LEDs in it:
            //
            // ledsOnRow = 6
            //
            // xWithinBank:  0  1  2  3  4  5
            // LED index:   5  4  3  2  1  0
            //              [] [] [] [] [] []   <--  Start here
            // 
            // rowStartIndex will point us to the LED marked xWithinBank:5
            // Say our desired xWithinBank = 2. To get the correct LED index, 
            // we must add 3 to the rowStartIndex. 3 = (ledsOnRow-1) - xWithinBank
            //
            // We must also account for the "missingLeds" in this math, since we are
            // starting on the side where the missingLEDs are. xWithinBank is guaranteed
            // to always be > missingLeds by the above logic. We must subtract missingLeds from
            // the result to avoid indexing too far into the row
            
            ledIndex = rowStartIndex + (((ledsOnRow - 1) - xWithinBank) - missingLeds);
            LOG_DBG("ledIndex = %u + (((%u - 1) - %u) - %u)", rowStartIndex, xWithinBank, missingLeds);
        }
    } else {
        // In Bank1, LEDs on the left side of the panel are missing
        // If the xWithinBank is too small, the LED is missing
        if (xWithinBank < missingLeds) {
            LOG_DBG(
                "Pixel (%u, %u): xWithinBank %u < missingLeds %u", 
                x, 
                y, 
                xWithinBank, 
                missingLeds);
            return -2;
        }

        // This LED is populated on this row! Calculate the correct index
        
        if (isLeftToRight) {
            LOG_DBG("Bank %u Row %u is left-to-right", bankId, y);
            // Bank1 left-to-right is relatively easy: just need to subtract missingLeds
            // from the xWithinBank value to avoid going too far, since the missing LEDs are
            // on the left-side of the display now
            ledIndex = rowStartIndex + (xWithinBank - missingLeds);
            LOG_DBG("ledIndex = %u + (%u - %u)", rowStartIndex, xWithinBank, missingLeds);
        } else {
            LOG_DBG("Bank %u Row %u is right-to-left", bankId, y);
            // A Bank1, right-to-left row is the same as a Bank0 right-to-left row
            // Say we have a right-to-left row with 6 LEDs in it:
            //
            // ledsOnRow = 6
            //
            // xWithinBank:  0  1  2  3  4  5
            // LED index:   5  4  3  2  1  0
            //              [] [] [] [] [] []   <--  Start here
            // 
            // rowStartIndex will point us to the LED marked xWithinBank:5
            // Say our desired xWithinBank = 2. To get the correct LED index, 
            // we must add 3 to the rowStartIndex. 3 = (ledsOnRow-1) - xWithinBank
            //
            // We need to account for "missingLeds" in this codepath. The xWithinBank
            // value needs to be corrected for the fact that our row actually starts with some
            // LEDs missing, so subtract that value from the xWithinBank.
            //
            // We are also guaranteed not to hit this code if xWithinBank 
            ledIndex = rowStartIndex + ((ledsOnRow - 1) - (xWithinBank - missingLeds));
            LOG_DBG("ledIndex = %u + ((%u - 1) - (%u - %u))", rowStartIndex, ledsOnRow, xWithinBank, missingLeds);
        }
    }

    LOG_DBG("Picked LED index %u in bank %u", ledIndex, bankId);

    // We have our LED index within the bank! Set the color
    bank[ledIndex].r = red;
    bank[ledIndex].g = green;
    bank[ledIndex].b = blue;

    return 0;
}

void led_display_thread_func(void* a, void* b, void* c) {
    const struct device *led_strip_0 = DEVICE_DT_GET(LED_STRIP_0_NODE_ID);
    const struct device *led_strip_1 = DEVICE_DT_GET(LED_STRIP_1_NODE_ID);

    if (!device_is_ready(led_strip_0)) {
        LOG_ERR("Device %s is not ready", led_strip_0->name);
        return;
    }

    if (!device_is_ready(led_strip_1)) {
        LOG_ERR("Device %s is not ready", led_strip_1->name);
        return;
    }

    // Set the entire LED bank to NULL before starting
    for (size_t i = 0; i < kNumDisplayBuffers; i++) {
        memset(led_0[i], 0, sizeof(struct led_rgb) * LED_STRIP_0_NUM_PIXELS);
        memset(led_1[i], 0, sizeof(struct led_rgb) * LED_STRIP_1_NUM_PIXELS);
    }

    // All display buffers start not-in-use
    for (auto& bufferState : displayBufferState) {
        bufferState.inUse = false;
    }

    LOG_INF("Starting LED display controller");

    constexpr float kTargetFrameIntervalMs = 33.3f;
    int ret;

    while (true) {
        
        // Update LED strips with current framebuffer contents
        // Monitor how long updating takes
        // Sleep appropriate amount to maintain target framerate
        int64_t startTicks = k_uptime_ticks();
        
        size_t bufferId = 0;
        ret = claimBufferForDisplay(bufferId);
        if (ret) {
            LOG_ERR("Error claiming display buffer!");
        }

        led_strip_update_rgb(led_strip_0, led_0[bufferId], LED_STRIP_0_NUM_PIXELS);
        led_strip_update_rgb(led_strip_1, led_1[bufferId], LED_STRIP_1_NUM_PIXELS);

        ret = releaseBufferFromDisplay(bufferId);
        if (ret) {
            LOG_ERR("Error releasing display buffer!");
        }

        int64_t endTicks = k_uptime_ticks();
        int64_t updateTicks = endTicks - startTicks;
        
        float updateTimeS = ((float)updateTicks) / ((float)CONFIG_SYS_CLOCK_TICKS_PER_SEC);
        float updateTimeMs = updateTimeS * 1000.0f;

        if (updateTimeMs > kTargetFrameIntervalMs) {
            LOG_WRN("Display update took >kTargetFrameIntervalMs, cannot keep framerate!");
        } else {
            // Sleep for however much time is left
            k_msleep(kTargetFrameIntervalMs - updateTimeMs);
        }
    }

    return;
}

#if defined(CONFIG_SHELL)
#include <zephyr/shell/shell.h>

static int cmd_led_test(const struct shell *shell,
                             size_t argc, char **argv, void *data)
{
    // Set the entire LED bank to NULL before the test
    memset(led_0[0], 0, sizeof(struct led_rgb) * LED_STRIP_0_NUM_PIXELS);
    memset(led_1[0], 0, sizeof(struct led_rgb) * LED_STRIP_1_NUM_PIXELS);

    int ret;

    // Set first pixel in left / right banks and check result
    ret = set_pixel_in_framebuffer(&kFrameLedConfig, 0, 0, 0 /* buffer */, 255, 0, 0);

    if (ret) {
        shell_error(shell, "Unexpected return code setting LED in buffer! %d", ret);
        return -EFAULT;
    }

    if (led_0[0][0].r != 255) {
        shell_error(shell, "Bank 0 Index 0 has wrong color!");
        return -EFAULT;
    }

    ret = set_pixel_in_framebuffer(&kFrameLedConfig, 20, 0, 0 /* buffer */, 255, 0, 0);

    if (ret) {
        shell_error(shell, "Unexpected return code setting LED in buffer! %d", ret);
        return -EFAULT;
    }

    if (led_1[0][0].r != 255) {
        shell_error(shell, "Bank 1 Index 0 has wrong color!");
        return -EFAULT;
    }

    // Try setting a pixel off the edge of the display
    ret = set_pixel_in_framebuffer(&kFrameLedConfig, 40, 0, 0 /* buffer */, 255, 0, 0);

    if (ret != -1) {
        shell_error(shell, "Unexpected return code setting off-display LED! %d", ret);
        return -EFAULT;
    }

    ret = set_pixel_in_framebuffer(&kFrameLedConfig, 0, 12, 0 /* buffer */, 255, 0, 0);

    if (ret != -1) {
        shell_error(shell, "Unexpected return code setting off-display LED! %d", ret);
        return -EFAULT;
    }

    // Try setting a pixel that isn't populated
    ret = set_pixel_in_framebuffer(&kFrameLedConfig, 16, 9, 0 /* buffer */, 255, 0, 0); // Row 9 has 16 LEDs so this should be within the nose region of Bank0

    if (ret != -2) {
        shell_error(shell, "Unexpected return code setting unpopulated LED! %d", ret);
        return -EFAULT;
    }

    ret = set_pixel_in_framebuffer(&kFrameLedConfig, 20, 11, 0 /* buffer */, 255, 0, 0); // Row 11 has 15 LEDs so this should be within the rose region of Bank1

    if (ret != -2) {
        shell_error(shell, "Unexpected return code setting unpopulated LED! %d", ret);
        return -EFAULT;
    }

    // Set the bottom-right LED on the display, which is in Bank1
    ret = set_pixel_in_framebuffer(&kFrameLedConfig, 39, 11, 0 /* buffer */, 255, 0, 0);

    if (ret) {
        shell_error(shell, "Unexpected return code setting last display LED! %d", ret);
        return -EFAULT;
    }

    if (led_1[0][201].r != 255) {
        shell_error(shell, "Bank 1 Index %u has wrong color!", 201);
        return -EFAULT;
    }

    // Set the last LED in Bank1, which is 25,11
    ret = set_pixel_in_framebuffer(&kFrameLedConfig, 25, 11, 0 /* buffer */, 255, 0, 0);

    if (ret) {
        shell_error(shell, "Unexpected return code setting last Bank1 LED! %d", ret);
        return -EFAULT;
    }

    if (led_1[0][LED_STRIP_1_NUM_PIXELS-1].r != 255) {
        shell_error(shell, "Bank 1 Index %u has wrong color!", LED_STRIP_1_NUM_PIXELS-1);
        return -EFAULT;
    }

    shell_print(shell, "All tests passed!");

    return 0;
}

static int cmd_led_config(const struct shell *shell,
                             size_t argc, char **argv, void *data)
{
    int selection = (int)data;

    if (selection == 0) {
        shell_print(shell, "Setting LED controller to frame config");
        currentConfig = &kFrameLedConfig;
    } else if (selection == 1) {
        shell_print(shell, "Setting LED controller to devkit config");
        currentConfig = &kDevKitLedConfig;
    } else {
        shell_error(shell, "Unknown config value: %d", selection);
        return -ENOEXEC;
    }

    return 0;
}

SHELL_SUBCMD_DICT_SET_CREATE(sub_led_config, cmd_led_config,
                             (frame, 0, "Set LED controller into frames mode"),
                             (devkit, 1, "Set LED controller into devkit mode"));

// Subcommands for "led"
SHELL_STATIC_SUBCMD_SET_CREATE(sub_led,
                               SHELL_CMD(config, &sub_led_config, "Pick LED configuration", NULL),
                               SHELL_CMD(test, NULL, "Run LED controller unit tests", cmd_led_test),
                               SHELL_SUBCMD_SET_END);

/* Creating root (level 0) command "led" */
SHELL_CMD_REGISTER(led, &sub_led, "LED commands", NULL);
#endif // CONFIG_SHELL