#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Estimated 2S LiPo state-of-charge from pack voltage (BT-free).
 *
 * Piecewise-linear over a resting (open-circuit) 2S discharge curve: 7000 mV
 * = 0% up to 8400 mV = 100%. The interpolation points are sampled from the
 * companion app's fuller curve (voltageToPercent in app/services/battery.ts)
 * so firmware and app report matching percentages; if one side's curve
 * changes, change the other to match.
 *
 * ACCURACY CAVEAT: this is a REST-voltage estimate. Under load (the faceplate
 * alone pulls 1.2-1.8 W) the pack sags, so the estimate reads low while the
 * glasses are running; while charging, the charger holds the terminals high,
 * so it reads high. Acceptable for a status display / LED color. Follow-up
 * (docs/plans/power-management-overhaul.md): IBAT-based load compensation.
 *
 * Shared by the status-LED color logic (src/power.cpp) and the "Battery
 * Percent" BLE characteristic (src/bluetooth/battery_service.cpp).
 *
 * @param vbat_mv Battery voltage in millivolts.
 * @return Estimated state of charge, clamped to [0, 100].
 */
constexpr uint8_t battery_soc_percent(int32_t vbat_mv) {
    /* [mV, %] anchor points, ascending. ~8 points capture the curve's flat
     * middle and steep ends without the app's full 21-point table. */
    struct SocPoint {
        int32_t mv;
        int32_t pct;
    };
    constexpr SocPoint kCurve[] = {
        {7000, 0},  {7220, 5},  {7370, 10}, {7490, 25},
        {7670, 50}, {7970, 75}, {8160, 85}, {8400, 100},
    };
    constexpr size_t kCount = sizeof(kCurve) / sizeof(kCurve[0]);

    if (vbat_mv <= kCurve[0].mv) {
        return 0;
    }
    if (vbat_mv >= kCurve[kCount - 1].mv) {
        return 100;
    }

    /* Bounded scan (7 segments); the clamps above guarantee a hit. */
    for (size_t i = 0; i < kCount - 1; i++) {
        const SocPoint lo = kCurve[i];
        const SocPoint hi = kCurve[i + 1];
        if (vbat_mv <= hi.mv) {
            /* Linear interpolation, rounded to nearest percent (mirrors the
             * app's Math.round). Worst-case numerator ~300*25 — no overflow. */
            const int32_t span = hi.mv - lo.mv;
            const int32_t pct =
                lo.pct + ((vbat_mv - lo.mv) * (hi.pct - lo.pct) + span / 2) / span;
            return static_cast<uint8_t>(pct);
        }
    }

    return 0; /* unreachable given the clamps above */
}
