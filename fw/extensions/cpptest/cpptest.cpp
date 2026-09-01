/*
 * cpptest — the in-repo integration-test fixture for the C++ wrapper path
 * (rgbx_animation.h): the static instance below is constructed by the
 * extension's init arrays, which the host runs inside the sandbox via
 * llext_bringup(), and the `ld -r` COMDAT handling is exercised end to end.
 * A dev/debug tool like `hello` — built and provisioned on dev boards,
 * never shipped on releases.
 *
 * Visually it is the classic three-wave plasma. The production Plasma
 * extension now lives in its own registry-shipped repo
 * (https://github.com/skalldri/rgbx-plasma), modernized to real sinf();
 * this fixture keeps the integer wave8() form deliberately — it documents
 * the cheap-integer-math pattern AND keeps the fixture's ARM import
 * surface empty, which sdk-ci asserts.
 */

#include <rgbx/rgbx_animation.h>

namespace {

class CppTest : public rgbx::Animation {
   public:
    void tick(uint32_t dt_ms) override {
        /* speed is a percentage of nominal (50 == 1x). */
        t_ += dt_ms * paramU32(0) / 50u;

        const uint32_t color = paramColor(1);
        const uint8_t cr = (color >> 16) & 0xFF;
        const uint8_t cg = (color >> 8) & 0xFF;
        const uint8_t cb = color & 0xFF;

        /* Invert flips the plasma's brightness gradient (light<->dark),
         * effectively inverting the palette while keeping the same tint. */
        const bool invert = paramBool(2);

        for (size_t y = 0; y < height(); y++) {
            for (size_t x = 0; x < width(); x++) {
                uint32_t v = (wave8(x * 13 + t_ / 9) + wave8(y * 23 + t_ / 14) +
                              wave8((x + y) * 11 + t_ / 6)) /
                             3;
                if (invert) {
                    v = 255u - v;
                }
                /* Gain exercises the FLOAT param path end to end — the value
                 * arrives as an IEEE-754 bit pattern in the shared u32 slot
                 * and paramF32() bit-casts it back. Clamped so a wild BLE
                 * write can only wash the plasma out, not overflow the cast. */
                float gain = paramF32(3);
                if (gain < 0.0f) {
                    gain = 0.0f;
                } else if (gain > 4.0f) {
                    gain = 4.0f;
                }
                const uint32_t scaled = static_cast<uint32_t>(static_cast<float>(v) * gain);
                v = (scaled > 255u) ? 255u : scaled;
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

RGBX_ANIMATION(CppTest, "C++ Test", 40, 12, RGBX_PARAM("Speed", RGBX_PARAM_UINT32, 50),
               RGBX_PARAM("Color", RGBX_PARAM_COLOR, 0x00FF40FF),
               RGBX_PARAM("Invert", RGBX_PARAM_BOOL, 0),
               RGBX_PARAM_F32("Gain", 1.0f));
