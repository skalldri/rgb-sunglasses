/*
 * rgbx_api.h — the RGB Sunglasses animation-extension ABI (flat C, version 1).
 *
 * This header is the wire contract between the firmware's extension host
 * (fw/src/extensions/) and a loadable animation extension (.llext file built
 * against the LLEXT EDK). It is shipped inside the EDK, so keep it:
 *   - pure C (extensions may be C or C++; a C++ convenience wrapper lives in
 *     rgbx_animation.hpp and compiles down to exactly these symbols),
 *   - free of Zephyr/firmware includes (stdint/stddef only),
 *   - append-only: any layout change to these structs requires bumping
 *     RGBX_ABI_VERSION, and the host rejects manifests with a mismatched
 *     version rather than guessing.
 *
 * Execution model (issue #85): the extension runs entirely on a sandboxed
 * user-mode thread and owns all the memory it touches. The kernel-side host
 * writes `rgbx_inputs` before each tick and reads `rgbx_framebuffer` after
 * it; the extension never calls into the firmware. There are deliberately no
 * function imports in ABI v1 — an extension that only touches its own
 * exported globals needs no syscalls and no grants beyond its own domain.
 */

#ifndef RGBX_API_H_
#define RGBX_API_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** ABI version this header describes. The extension stamps this into its
 *  manifest; the host refuses to run any other value. */
#define RGBX_ABI_VERSION 1u

/** Fixed capacity of the per-extension parameter block. */
#define RGBX_MAX_PARAMS 8u

/** Parameter types, mapped by the host onto the same BLE characteristic
 *  presentation formats (CPF) the built-in animations use. */
enum rgbx_param_type {
    RGBX_PARAM_UINT32 = 0, /**< plain unsigned integer */
    RGBX_PARAM_COLOR = 1,  /**< 0x00RRGGBB (high byte ignored) */
};

/** One user-tunable parameter, surfaced as a BLE characteristic named
 *  `name` on the extension's auto-generated GATT service. */
struct rgbx_param_desc {
    const char *name;       /**< display name (CUD string); must be a string
                             *   literal inside the extension image */
    uint8_t type;           /**< enum rgbx_param_type */
    uint32_t default_value; /**< value before any BLE write arrives */
};

/** Extension self-description. Exported as the `rgbx_manifest` symbol and
 *  validated by the host at load time (abi_version, dims, param_count). */
struct rgbx_manifest {
    uint32_t abi_version; /**< must be RGBX_ABI_VERSION */
    const char *name;     /**< animation display name (BLE "Animation Name") */
    uint32_t width;       /**< framebuffer width the extension renders at;
                           *   must match the host display (40 on proto0) */
    uint32_t height;      /**< framebuffer height (12 on proto0) */
    uint32_t param_count; /**< entries in `params`, <= RGBX_MAX_PARAMS */
    const struct rgbx_param_desc *params; /**< NULL iff param_count == 0 */
};

/** Per-tick input snapshot. The host (kernel mode) fills this in before
 *  signalling a tick; the extension must treat it as read-only and must not
 *  assume values persist across ticks. Absent sources read as zeros. */
struct rgbx_inputs {
    uint32_t dt_ms;                  /**< nominal ms since the previous tick */
    uint32_t params[RGBX_MAX_PARAMS]; /**< current parameter values, in
                                       *   manifest declaration order */
    float accel[3]; /**< IMU accelerometer x/y/z, m/s^2 */
    float gyro[3];  /**< IMU gyroscope x/y/z, rad/s */
    /* ABI v2 candidates (do NOT add without bumping RGBX_ABI_VERSION):
     * audio band energies / beat flags. */
};

/** Byte offset of pixel (x, y) in `rgbx_framebuffer` for a display `w`
 *  pixels wide. Layout is row-major, 3 bytes per pixel: R, G, B. */
#define RGBX_PIXEL_INDEX(w, x, y) ((((size_t)(y) * (size_t)(w)) + (size_t)(x)) * 3u)

/*
 * === Required exports ======================================================
 * Every extension must define all five of the following symbols and mark
 * each with EXPORT_SYMBOL(<name>) (from <zephyr/llext/symbol.h>) so the host
 * can resolve them with llext_find_sym(). The declarations below let the
 * C++ wrapper (and extension code itself) reference them type-safely.
 */

/** The extension's manifest (const data). */
extern const struct rgbx_manifest rgbx_manifest;

/** The input block the host writes before each tick. Must be a writable
 *  global (i.e. live in the extension's .data/.bss, not .rodata). */
extern struct rgbx_inputs rgbx_inputs;

/** The scratch framebuffer the extension renders into, sized exactly
 *  width * height * 3 bytes (see RGBX_PIXEL_INDEX for layout). The host
 *  copies it to the real display after each successful tick. */
extern uint8_t rgbx_framebuffer[];

/** Called once, on the sandboxed thread, before the first tick. */
void rgbx_init(void);

/** Called once per frame, on the sandboxed thread. Renders one frame into
 *  rgbx_framebuffer using the current rgbx_inputs. Must return well within
 *  the host's tick deadline or the extension is aborted and unloaded. */
void rgbx_tick(void);

/** Names the host uses with llext_find_sym(); kept next to the declarations
 *  so the two can't drift apart. */
#define RGBX_SYM_MANIFEST "rgbx_manifest"
#define RGBX_SYM_INPUTS "rgbx_inputs"
#define RGBX_SYM_FRAMEBUFFER "rgbx_framebuffer"
#define RGBX_SYM_INIT "rgbx_init"
#define RGBX_SYM_TICK "rgbx_tick"

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* RGBX_API_H_ */
