/*
 * rgbx_animation.h — header-only C++ convenience wrapper over the flat C
 * extension ABI in rgbx_api.h.
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
 *                    RGBX_PARAM("Speed", RGBX_PARAM_UINT32, 50));
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
 * Extension-side analog of the firmware's BaseAnimation. Subclass it and
 * instantiate with RGBX_ANIMATION() below.
 */
class Animation {
   public:
    /** Called once, on the sandboxed thread, before the first tick. */
    virtual void init() {}

    /** Called once per frame; render into the framebuffer via setPixel()/
     *  fill(). dt_ms is the nominal time since the previous tick. */
    virtual void tick(uint32_t dt_ms) = 0;

   protected:
    /* Non-virtual on purpose: instances are static and never deleted
     * polymorphically, and a virtual destructor would drag in operator
     * delete, which the sandbox does not provide. */
    ~Animation() = default;

    size_t width() const { return rgbx_manifest.width; }
    size_t height() const { return rgbx_manifest.height; }

    /** Write one pixel (out-of-range coordinates are ignored). */
    void setPixel(size_t x, size_t y, uint8_t r, uint8_t g, uint8_t b) {
        if (x >= width() || y >= height()) {
            return;
        }
        uint8_t *px = &rgbx_framebuffer[RGBX_PIXEL_INDEX(width(), x, y)];
        px[0] = r;
        px[1] = g;
        px[2] = b;
    }

    /** Fill the whole framebuffer with one color. */
    void fill(uint8_t r, uint8_t g, uint8_t b) {
        const size_t n = width() * height();
        for (size_t i = 0; i < n; i++) {
            uint8_t *px = &rgbx_framebuffer[i * 3u];
            px[0] = r;
            px[1] = g;
            px[2] = b;
        }
    }

    /** Current value of manifest parameter i (0 if out of range). */
    uint32_t param(size_t i) const {
        return (i < RGBX_MAX_PARAMS) ? rgbx_inputs.params[i] : 0u;
    }

    /* IMU snapshot accessors (zeros when the source is absent). */
    float accelX() const { return rgbx_inputs.accel[0]; }
    float accelY() const { return rgbx_inputs.accel[1]; }
    float accelZ() const { return rgbx_inputs.accel[2]; }
    float gyroX() const { return rgbx_inputs.gyro[0]; }
    float gyroY() const { return rgbx_inputs.gyro[1]; }
    float gyroZ() const { return rgbx_inputs.gyro[2]; }
};

}  // namespace rgbx

/** Parameter entry for RGBX_ANIMATION()'s variadic tail. */
#define RGBX_PARAM(name_, type_, default_) {(name_), (uint8_t)(type_), (default_)}

/**
 * Instantiates ClassName as the extension's animation and emits the five
 * required C exports (see rgbx_api.h). Use at namespace scope in exactly one
 * translation unit — which is naturally the only one, since an .llext is a
 * single object file. The variadic tail is zero or more RGBX_PARAM(...)
 * entries (a zero-length param array relies on the GNU zero-length-array
 * extension, which the EDK toolchain permits).
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
    static const struct rgbx_param_desc rgbx_wrapper_params_[] = {__VA_ARGS__};                 \
    const struct rgbx_manifest rgbx_manifest = {                                                \
        RGBX_ABI_VERSION,                                                                       \
        (DisplayName),                                                                          \
        (W),                                                                                    \
        (H),                                                                                    \
        sizeof(rgbx_wrapper_params_) / sizeof(rgbx_wrapper_params_[0]),                         \
        rgbx_wrapper_params_,                                                                   \
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
