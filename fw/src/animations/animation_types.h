#pragma once

#include <cstddef>

// Defines the ID for each animation
enum class Animation {
    None = 0,
    ZigZag = 1,
    Text = 2,
    BtAdvertising = 3,
    BtConnecting = 4,
    Rainbow = 5,
    BtPairing = 6,
    MyEyes = 7,
    Beat = 8,
    FftBars = 9,
    GlimPlayer = 10,
    MatrixCode = 11,
    Tilt = 12,

    // Sentinel: one past the last built-in animation. Not a real animation —
    // used by static_asserts (e.g. the extension animation-ID band must start
    // above every built-in). Keep this immediately after the last entry.
    Count,
};

/** Maximum animation display-name length (bytes incl. NUL). Every animation
 *  service's "Animation Name" characteristic uses BtGattString of this size,
 *  and extension display names are truncated to it. */
inline constexpr size_t kAnimationNameMaxLen = 24;