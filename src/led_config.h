#pragma once

#include <cstdint>
#include <cstddef>

struct LedConfig {
    const size_t displayWidth;
    const size_t displayHeight;
    const size_t ledBankWidth;
    const size_t* const ledsOnRow; // Points at an array of size displayHeight
    const size_t* const rowStartIndex; // Points at an array of size displayHeight
    const bool* const rowIsLeftToRight; // Points at an array of size displayHeight
};

/**
 * @brief Constants for the full glasses frame LED configuration
 * 
 */

// Define some constants which describe the display properties
constexpr size_t kFrameDisplayWidth = 40; // The total display width, just looking at pixels on the display
constexpr size_t kFrameDisplayHeight = 12; // The total display height, just looking at pixels on the display

constexpr size_t kFrameLedBankWidth = kFrameDisplayWidth / 2; // There are two LED banks, each is half the display width

// Number of LEDs on each row of ONE BANK
const size_t kFrameLedsOnRow[kFrameDisplayHeight] = {
    20, // Row 00 has 20 LEDs
    20, // Row 01 has 20 LEDs
    20, // Row 02 has 20 LEDs
    20, // Row 03 has 20 LEDs
    20, // Row 04 has 20 LEDs
    20, // Row 05 has 20 LEDs
    17, // Row 06 has 17 LEDs
    17, // Row 07 has 17 LEDs
    16, // Row 08 has 16 LEDs
    16, // Row 09 has 16 LEDs
    15, // Row 10 has 15 LEDs
    15, // Row 11 has 15 LEDs
};

// Start index of each row of ONE BANK
const size_t kFrameRowStartIndex[kFrameDisplayHeight] = {
    0,   // Row 00 starts at index 0
    20,  // Row 01 starts at index 20
    40,  // Row 02 starts at index 40
    60,  // Row 03 starts at index 60
    80,  // Row 04 starts at index 80
    100, // Row 05 starts at index 100
    120, // Row 06 starts at index 120
    137, // Row 07 starts at index 137
    154, // Row 08 starts at index 154
    170, // Row 09 starts at index 170
    186, // Row 10 starts at index 186
    201, // Row 11 starts at index 201
};

// Banks are symmetrical, so we can work out the index within the bank generically
const bool kFrameRowIsLeftToRight[kFrameDisplayHeight] = {
    true,   // Row 00: ->
    false,  // Row 01: <-
    true,   // Row 02: ->
    false,  // Row 03: <-
    true,   // Row 04: ->
    false,  // Row 05: <-
    true,   // Row 06: ->
    false,  // Row 07: <-
    true,   // Row 08: ->
    false,  // Row 09: <-
    true,   // Row 10: ->
    false,  // Row 11: <-
};

const LedConfig kFrameLedConfig = {
    kFrameDisplayWidth,
    kFrameDisplayHeight,
    kFrameLedBankWidth,
    kFrameLedsOnRow,
    kFrameRowStartIndex,
    kFrameRowIsLeftToRight
};

/**
 * @brief Constants for the DevKit built-in LED bank
 * 
 */

// Define some constants which describe the display properties
constexpr size_t kDevKitDisplayWidth = 8; // The total display width, just looking at pixels on the display
constexpr size_t kDevKitDisplayHeight = 2; // The total display height, just looking at pixels on the display

constexpr size_t kDevKitLedBankWidth = kDevKitDisplayWidth / 2; // There are two LED banks, each is half the display width

// Number of LEDs on each row of ONE BANK
const size_t kDevKitLedsOnRow[kDevKitDisplayHeight] = {
    4, // Row 00 has 4 LEDs
    4, // Row 01 has 4 LEDs
};

// Start index of each row of ONE BANK
const size_t kDevKitRowStartIndex[kDevKitDisplayHeight] = {
    0,   // Row 00 starts at index 0
    4,   // Row 01 starts at index 4
};

// Both rows on the devkit are right to left
const bool kDevKitRowIsLeftToRight[kDevKitDisplayHeight] = {
    true,   // Row 00: ->
    true,   // Row 01: ->
};

const LedConfig kDevKitLedConfig = {
    kDevKitDisplayWidth,
    kDevKitDisplayHeight,
    kDevKitLedBankWidth,
    kDevKitLedsOnRow,
    kDevKitRowStartIndex,
    kDevKitRowIsLeftToRight
};