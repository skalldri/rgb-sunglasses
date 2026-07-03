/**
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
     *  setPixel()/fill(). @param dt_ms nominal ms since the previous tick. */
    virtual void tick(uint32_t dt_ms) = 0;

   protected:
    /* Non-virtual on purpose: instances are static and never deleted
     * polymorphically, and a virtual destructor would drag in operator
     * delete, which the sandbox does not provide. */
    ~Animation() = default;

    /** @brief Framebuffer width in pixels (manifest value). */
    size_t width() const { return rgbx_manifest.width; }
    /** @brief Framebuffer height in pixels (manifest value). */
    size_t height() const { return rgbx_manifest.height; }

    /** @brief Write one pixel (out-of-range coordinates are ignored). */
    void setPixel(size_t x, size_t y, uint8_t r, uint8_t g, uint8_t b) {
        if (x >= width() || y >= height()) {
            return;
        }
        uint8_t *px = &rgbx_framebuffer[RGBX_PIXEL_INDEX(width(), x, y)];
        px[0] = r;
        px[1] = g;
        px[2] = b;
    }

    /** @brief Fill the whole framebuffer with one color. */
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

    /** @brief Value of UINT32 parameter i (0 if out of range). */
    uint32_t paramU32(size_t i) const {
        return (i < RGBX_MAX_PARAMS) ? rgbx_inputs.params[i] : 0u;
    }

    /** @brief Alias of paramU32() kept for early extensions. */
    uint32_t param(size_t i) const { return paramU32(i); }

    /** @brief Value of COLOR parameter i as 0x00RRGGBB (0 if out of range). */
    uint32_t paramColor(size_t i) const { return paramU32(i) & 0x00FFFFFFu; }

    /** @brief Value of BOOL parameter i (false if out of range). */
    bool paramBool(size_t i) const { return paramU32(i) != 0u; }

    /** @brief Value of STRING parameter i (always NUL-terminated; "" if i is
     *  out of range or not a string parameter). String values live in
     *  rgbx_inputs.param_strings, slotted by declaration order among the
     *  string-typed params — this accessor does that mapping for you. */
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

    /* --- IMU snapshot accessors (zeros when the source is absent) -------- */

    float accelX() const { return rgbx_inputs.accel[0]; } /**< m/s^2 */
    float accelY() const { return rgbx_inputs.accel[1]; } /**< m/s^2 */
    float accelZ() const { return rgbx_inputs.accel[2]; } /**< m/s^2 */
    float gyroX() const { return rgbx_inputs.gyro[0]; }   /**< rad/s */
    float gyroY() const { return rgbx_inputs.gyro[1]; }   /**< rad/s */
    float gyroZ() const { return rgbx_inputs.gyro[2]; }   /**< rad/s */

    /* --- audio snapshot accessors (zeros when the source is absent) ------ */

    /** @brief Number of coarse audio bands. */
    static constexpr size_t numBands() { return RGBX_AUDIO_NUM_BANDS; }
    /** @brief Smoothed energy of coarse band b (0 if out of range). */
    float bandEnergy(size_t b) const {
        return (b < RGBX_AUDIO_NUM_BANDS) ? rgbx_inputs.audio_band_energy[b] : 0.0f;
    }
    /** @brief True if a beat fired in band b this frame. */
    bool isBeat(size_t b) const {
        return (b < RGBX_AUDIO_NUM_BANDS) && rgbx_inputs.audio_beat[b] != 0u;
    }
    /** @brief Number of fine-grained display buckets. */
    static constexpr size_t numDisplayBuckets() { return RGBX_AUDIO_NUM_DISPLAY_BUCKETS; }
    /** @brief Energy of display bucket i, ~0..1 (0 if out of range). */
    float displayBucket(size_t i) const {
        return (i < RGBX_AUDIO_NUM_DISPLAY_BUCKETS) ? rgbx_inputs.audio_display_bucket[i]
                                                    : 0.0f;
    }

    /* --- button accessors (zeros when the source is absent) -------------- */

    /** @brief Raw pressed-since-last-tick bitmask (bit i = button i). */
    uint32_t buttonsPressed() const { return rgbx_inputs.buttons_pressed; }
    /** @brief True if button id was pressed since the previous tick.
     *  proto0 mapping: 0=Up, 1=Left, 2=Right, 3=Down, 4=Wake. */
    bool buttonWasPressed(size_t id) const {
        return id < 32 && (rgbx_inputs.buttons_pressed & (1u << id)) != 0u;
    }
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
    void rgbx_init(void) { rgbx_wrapper_instance_.init(); }                                     \
    void rgbx_tick(void) { rgbx_wrapper_instance_.tick(rgbx_inputs.dt_ms); }                    \
    EXPORT_SYMBOL(rgbx_manifest);                                                               \
    EXPORT_SYMBOL(rgbx_inputs);                                                                 \
    EXPORT_SYMBOL(rgbx_framebuffer);                                                            \
    EXPORT_SYMBOL(rgbx_init);                                                                   \
    EXPORT_SYMBOL(rgbx_tick)

#endif /* RGBX_ANIMATION_H_ */
