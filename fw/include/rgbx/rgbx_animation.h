/**
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Stuart Alldritt
 *
 * @file rgbx_animation.h
 * @brief Header-only C++ convenience wrapper over the flat C extension ABI
 *        in rgbx_api.h.
 *
 * Lets an extension author write a class instead of raw exports:
 *
 *     #include <rgbx/rgbx_animation.h>
 *
 *     class Plasma : public rgbx::Animation {
 *        public:
 *         void tick(uint32_t dt_ms) override {
 *             fill(0, 0, 0);
 *             setPixel(x_, 0, 255, 0, 0);
 *             x_ = (x_ + 1) % width();
 *         }
 *        private:
 *         size_t x_ = 0;
 *     };
 *
 *     RGBX_ANIMATION(Plasma, "Plasma", 40, 12,
 *                    RGBX_PARAM("Speed", RGBX_PARAM_UINT32, 50),
 *                    RGBX_PARAM_STR("Label", "HI"));
 *
 * Everything here compiles *inside* the extension: no vtables or C++ objects
 * cross the host boundary — the RGBX_ANIMATION macro emits exactly the five
 * flat C symbols rgbx_api.h requires, so C and C++ extensions are
 * indistinguishable to the firmware.
 *
 * Constraints (enforced where possible):
 *  - The animation class must be trivially destructible: the single static
 *    instance is never destroyed, and a non-trivial destructor would make
 *    the compiler emit an __aeabi_atexit registration the sandbox does not
 *    provide. (static_assert in the macro.)
 *  - No heap, no exceptions, no RTTI — the sandbox provides none of them.
 */

#ifndef RGBX_ANIMATION_H_
#define RGBX_ANIMATION_H_

#ifndef __cplusplus
#error "rgbx_animation.h is the C++ wrapper; C extensions should use rgbx_api.h directly"
#endif

#include <rgbx/rgbx_api.h>
#include <zephyr/llext/symbol.h>

#include <type_traits>

namespace rgbx {

/**
 * @brief Extension-side analog of the firmware's BaseAnimation. Subclass it
 * and instantiate with RGBX_ANIMATION() below.
 */
class Animation {
   public:
    /** @brief Called once, on the sandboxed thread, after every (re)load and
     *  before the first tick. */
    virtual void init() {}

    /** @brief Called once per frame; render into the framebuffer via
     *  setPixel()/fill().
     *
     *  Must return well within the host's per-tick CPU budget or the
     *  extension is aborted and unloaded.
     *
     *  @param dt_ms Nominal milliseconds since the previous tick. */
    virtual void tick(uint32_t dt_ms) = 0;

    /** @brief Queried after every tick(): return true when the frame just rendered
     *  ended at a natural switch boundary (end of a scroll/clip/cycle), so shuffle
     *  mode (issue #121) may switch away without visual jarring. Default: every
     *  frame is a good moment — matching built-in animations with no override.
     *  Backed by the optional rgbx_good_moment export, which RGBX_ANIMATION()
     *  always emits for wrapper-based extensions.
     *
     *  @return true if this is a natural point to switch animations. */
    virtual bool goodMoment() const { return true; }

   protected:
    /* Non-virtual on purpose: instances are static and never deleted
     * polymorphically, and a virtual destructor would drag in operator
     * delete, which the sandbox does not provide. */
    ~Animation() = default;

    /** @brief Framebuffer width in pixels (manifest value).
     *  @return Width in pixels, as declared to RGBX_ANIMATION(). */
    size_t width() const { return rgbx_manifest.width; }
    /** @brief Framebuffer height in pixels (manifest value).
     *  @return Height in pixels, as declared to RGBX_ANIMATION(). */
    size_t height() const { return rgbx_manifest.height; }

    /** @brief Write one pixel (out-of-range coordinates are ignored).
     *
     *  @param x Column, 0 .. width()-1. Out-of-range values are a no-op.
     *  @param y Row, 0 .. height()-1. Out-of-range values are a no-op.
     *  @param r Red channel, 0-255.
     *  @param g Green channel, 0-255.
     *  @param b Blue channel, 0-255.
     *
     *  @note Render near full scale. The host multiplies every pixel by the
     *  global brightness factor (default 0.02), so an animation that dims
     *  itself to e.g. 32/255 is invisible on the panel. */
    void setPixel(size_t x, size_t y, uint8_t r, uint8_t g, uint8_t b) {
        if (x >= width() || y >= height()) {
            return;
        }
        uint8_t *px = &rgbx_framebuffer[RGBX_PIXEL_INDEX(width(), x, y)];
        px[0] = r;
        px[1] = g;
        px[2] = b;
    }

    /** @brief Fill the whole framebuffer with one color.
     *
     *  @param r Red channel, 0-255.
     *  @param g Green channel, 0-255.
     *  @param b Blue channel, 0-255. */
    void fill(uint8_t r, uint8_t g, uint8_t b) {
        const size_t n = width() * height();
        for (size_t i = 0; i < n; i++) {
            uint8_t *px = &rgbx_framebuffer[i * 3u];
            px[0] = r;
            px[1] = g;
            px[2] = b;
        }
    }

    /* --- typed parameter accessors (manifest declaration order) ---------- */

    /** @brief Value of UINT32 parameter i.
     *  @param i Parameter index in manifest declaration order.
     *  @return The current value, or 0 if i is out of range. */
    uint32_t paramU32(size_t i) const {
        return (i < RGBX_MAX_PARAMS) ? rgbx_inputs.params[i] : 0u;
    }

    /** @brief Alias of paramU32() kept for early extensions.
     *  @param i Parameter index in manifest declaration order.
     *  @return The current value, or 0 if i is out of range. */
    uint32_t param(size_t i) const { return paramU32(i); }

    /** @brief Value of COLOR parameter i.
     *  @param i Parameter index in manifest declaration order.
     *  @return The color as 0x00RRGGBB, or 0 if i is out of range. */
    uint32_t paramColor(size_t i) const { return paramU32(i) & 0x00FFFFFFu; }

    /** @brief Value of BOOL parameter i.
     *  @param i Parameter index in manifest declaration order.
     *  @return The current value, or false if i is out of range. */
    bool paramBool(size_t i) const { return paramU32(i) != 0u; }

    /** @brief Value of STRING parameter i.
     *
     *  String values live in rgbx_inputs::param_strings, slotted by
     *  declaration order among the string-typed params only — this accessor
     *  does that mapping for you, so `i` is the plain manifest index.
     *
     *  @param i Parameter index in manifest declaration order.
     *  @return A NUL-terminated string, or "" if i is out of range or does
     *          not name an RGBX_PARAM_STRING parameter. Never NULL. */
    const char *paramString(size_t i) const {
        if (i >= rgbx_manifest.param_count ||
            rgbx_manifest.params[i].type != RGBX_PARAM_STRING) {
            return "";
        }
        size_t slot = 0;
        for (size_t p = 0; p < i; p++) {
            if (rgbx_manifest.params[p].type == RGBX_PARAM_STRING) {
                slot++;
            }
        }
        return (slot < RGBX_MAX_STRING_PARAMS) ? rgbx_inputs.param_strings[slot] : "";
    }

    /**
     * @name IMU snapshot accessors
     * All read zero on a build or board with no IMU, so an animation can use
     * them unconditionally.
     * @{
     */

    /** @brief Accelerometer X for this tick. @return Acceleration in m/s^2. */
    float accelX() const { return rgbx_inputs.accel[0]; }
    /** @brief Accelerometer Y for this tick. @return Acceleration in m/s^2. */
    float accelY() const { return rgbx_inputs.accel[1]; }
    /** @brief Accelerometer Z for this tick. @return Acceleration in m/s^2. */
    float accelZ() const { return rgbx_inputs.accel[2]; }
    /** @brief Gyroscope X for this tick. @return Angular rate in rad/s. */
    float gyroX() const { return rgbx_inputs.gyro[0]; }
    /** @brief Gyroscope Y for this tick. @return Angular rate in rad/s. */
    float gyroY() const { return rgbx_inputs.gyro[1]; }
    /** @brief Gyroscope Z for this tick. @return Angular rate in rad/s. */
    float gyroZ() const { return rgbx_inputs.gyro[2]; }

    /** @} */

    /**
     * @name Audio snapshot accessors
     * All read zero when audio is absent or silent, so an animation can use
     * them unconditionally. Bands are ordered low frequencies first.
     * @{
     */

    /** @brief Number of coarse audio bands.
     *  @return RGBX_AUDIO_NUM_BANDS. */
    static constexpr size_t numBands() { return RGBX_AUDIO_NUM_BANDS; }
    /** @brief Smoothed energy of one coarse band.
     *  @param b Band index, 0 .. numBands()-1.
     *  @return Smoothed band energy, or 0 if b is out of range. */
    float bandEnergy(size_t b) const {
        return (b < RGBX_AUDIO_NUM_BANDS) ? rgbx_inputs.audio_band_energy[b] : 0.0f;
    }
    /** @brief Whether a beat fired in one band this frame.
     *  @param b Band index, 0 .. numBands()-1.
     *  @return true if a beat was detected, false if not or b is out of range. */
    bool isBeat(size_t b) const {
        return (b < RGBX_AUDIO_NUM_BANDS) && rgbx_inputs.audio_beat[b] != 0u;
    }
    /** @brief Number of fine-grained display buckets.
     *  @return RGBX_AUDIO_NUM_DISPLAY_BUCKETS. */
    static constexpr size_t numDisplayBuckets() { return RGBX_AUDIO_NUM_DISPLAY_BUCKETS; }
    /** @brief Energy of one fine-grained spectrum bucket, for bar-graph style
     *  visualisation.
     *  @param i Bucket index, 0 .. numDisplayBuckets()-1.
     *  @return Bucket energy normalized to roughly 0..1, or 0 if i is out of
     *          range. */
    float displayBucket(size_t i) const {
        return (i < RGBX_AUDIO_NUM_DISPLAY_BUCKETS) ? rgbx_inputs.audio_display_bucket[i]
                                                    : 0.0f;
    }

    /** @} */

    /**
     * @name Button accessors
     * Both read zero when no buttons are present. proto0 mapping: 0=Up,
     * 1=Left, 2=Right, 3=Down, 4=Wake.
     * @{
     */

    /** @brief Raw pressed-since-last-tick bitmask.
     *  @return Bitmask where bit i is set if button i was pressed since the
     *          previous tick. */
    uint32_t buttonsPressed() const { return rgbx_inputs.buttons_pressed; }
    /** @brief Whether one button was pressed since the previous tick.
     *  @param id Button id (proto0: 0=Up, 1=Left, 2=Right, 3=Down, 4=Wake).
     *  @return true if that button was pressed, false if not or id is >= 32. */
    bool buttonWasPressed(size_t id) const {
        return id < 32 && (rgbx_inputs.buttons_pressed & (1u << id)) != 0u;
    }

    /** @} */
};

}  // namespace rgbx

/**
 * @brief Instantiates ClassName as the extension's animation and emits the
 * five required C exports (see rgbx_api.h).
 *
 * Use at namespace scope in exactly one translation unit — which is
 * naturally the only one, since an .llext is a single object file. The
 * variadic tail is zero or more RGBX_PARAM(...) / RGBX_PARAM_STR(...)
 * entries. With an empty tail the manifest gets param_count == 0 and
 * params == NULL, exactly as the ABI contract requires (`NULL ? NULL : x`
 * below evaluates to NULL when __VA_OPT__ emits nothing and to the params
 * array when it doesn't).
 *
 * __cxa_pure_virtual is defined here because the vtable of any class with a
 * pure-virtual member references it, and the sandbox links no C++ runtime;
 * it can never actually be reached on a fully-constructed static instance.
 *
 * @param ClassName   The rgbx::Animation subclass to instantiate. Must be
 *                    trivially destructible (static_assert'd here).
 * @param DisplayName String literal shown as the animation's name in the
 *                    companion app.
 * @param W           Framebuffer width in pixels; must match the host display
 *                    (40 on proto0).
 * @param H           Framebuffer height in pixels (12 on proto0).
 * @param ...         Zero or more RGBX_PARAM() / RGBX_PARAM_STR() entries, at
 *                    most RGBX_MAX_PARAMS of them.
 */
#define RGBX_ANIMATION(ClassName, DisplayName, W, H, ...)                                       \
    static_assert(std::is_trivially_destructible_v<ClassName>,                                  \
                  "extension animation classes must be trivially destructible (no atexit in "  \
                  "the sandbox)");                                                              \
    extern "C" void __cxa_pure_virtual(void) {                                                  \
        while (true) {                                                                          \
        }                                                                                       \
    }                                                                                           \
    /* Definitions below pick up C language linkage from the declarations in rgbx_api.h. */    \
    struct rgbx_inputs rgbx_inputs;                                                             \
    uint8_t rgbx_framebuffer[(size_t)(W) * (size_t)(H) * 3u];                                   \
    __VA_OPT__(static const struct rgbx_param_desc rgbx_wrapper_params_[] = {__VA_ARGS__};)     \
    const struct rgbx_manifest rgbx_manifest = {                                                \
        RGBX_ABI_VERSION,                                                                       \
        (DisplayName),                                                                          \
        (W),                                                                                    \
        (H),                                                                                    \
        0u __VA_OPT__(+sizeof(rgbx_wrapper_params_) / sizeof(rgbx_wrapper_params_[0])),         \
        NULL __VA_OPT__(? NULL : rgbx_wrapper_params_),                                         \
    };                                                                                          \
    static ClassName rgbx_wrapper_instance_;                                                    \
    uint8_t rgbx_good_moment;                                                                   \
    void rgbx_init(void) { rgbx_wrapper_instance_.init(); }                                     \
    void rgbx_tick(void) {                                                                      \
        rgbx_wrapper_instance_.tick(rgbx_inputs.dt_ms);                                         \
        rgbx_good_moment = rgbx_wrapper_instance_.goodMoment() ? 1u : 0u;                       \
    }                                                                                           \
    EXPORT_SYMBOL(rgbx_manifest);                                                               \
    EXPORT_SYMBOL(rgbx_inputs);                                                                 \
    EXPORT_SYMBOL(rgbx_framebuffer);                                                            \
    EXPORT_SYMBOL(rgbx_init);                                                                   \
    EXPORT_SYMBOL(rgbx_tick);                                                                   \
    EXPORT_SYMBOL(rgbx_good_moment)

#endif /* RGBX_ANIMATION_H_ */
