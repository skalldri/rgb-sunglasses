/*
 * plasma — a full animation extension written in C++ against the rgbx
 * wrapper (rgbx_animation.h). Classic three-wave plasma, tinted by a
 * BLE-tunable color, speed-scaled by a BLE-tunable parameter. Doubles as
 * the integration test for the C++ wrapper path: the static Plasma instance
 * below is constructed by the extension's init arrays, which the host runs
 * inside the sandbox via llext_bringup().
 *
 * (The deliberate crash/hang sandbox-recovery hooks that used to live here
 * moved to the `hello` kitchen-sink extension — plasma ships clean.)
 */

#include <rgbx/rgbx_animation.h>

namespace {

class Plasma : public rgbx::Animation {
   public:
    void tick(uint32_t dt_ms) override {
        /* speed is a percentage of nominal (50 == 1x). */
        t_ += dt_ms * paramU32(0) / 50u;

        const uint32_t color = paramColor(1);
        const uint8_t cr = (color >> 16) & 0xFF;
        const uint8_t cg = (color >> 8) & 0xFF;
        const uint8_t cb = color & 0xFF;

        for (size_t y = 0; y < height(); y++) {
            for (size_t x = 0; x < width(); x++) {
                uint32_t v = (wave8(x * 13 + t_ / 9) + wave8(y * 23 + t_ / 14) +
                              wave8((x + y) * 11 + t_ / 6)) /
                             3;
                setPixel(x, y, static_cast<uint8_t>(cr * v / 255),
                         static_cast<uint8_t>(cg * v / 255),
                         static_cast<uint8_t>(cb * v / 255));
            }
        }
    }

   private:
    /* Smooth sine-ish wave, period 256, range ~[2, 254]: parabolic humps —
     * no libm dependency inside the sandbox. */
    static uint8_t wave8(uint32_t angle) {
        const uint8_t t = angle & 0xFF;
        const uint8_t half = t & 0x7F;
        const uint32_t hump = (uint32_t)half * (127u - half) / 32u; /* 0..126 */
        return (t & 0x80) ? static_cast<uint8_t>(128u - hump)
                          : static_cast<uint8_t>(128u + hump);
    }

    uint32_t t_ = 0;
};

}  // namespace

RGBX_ANIMATION(Plasma, "Plasma", 40, 12, RGBX_PARAM("Speed", RGBX_PARAM_UINT32, 50),
               RGBX_PARAM("Color", RGBX_PARAM_COLOR, 0x00FF40FF));
