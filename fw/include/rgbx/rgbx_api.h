/**
 * @file rgbx_api.h
 * @brief The RGB Sunglasses animation-extension ABI (flat C, version 1).
 *
 * This header is the wire contract between the firmware's extension host
 * (fw/src/extensions/) and a loadable animation extension (.llext file built
 * against the LLEXT EDK). It is shipped inside the EDK, so keep it:
 *   - pure C (extensions may be C or C++; a C++ convenience wrapper lives in
 *     rgbx_animation.h and compiles down to exactly these symbols),
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
 *
 * Lifecycle: extensions are discovered and validated at boot, but only the
 * ACTIVE extension is loaded into memory. Activation loads the ELF and runs
 * rgbx_init() on the sandbox thread (a failure there is reported
 * asynchronously — the animation's BLE "Is Active" characteristic notifies
 * false); switching away unloads it. Globals therefore reset to their
 * initial values on every activation.
 */

#ifndef RGBX_API_H_
#define RGBX_API_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief ABI version this header describes. The extension stamps this into
 *  its manifest; the host refuses to run any other value. */
#define RGBX_ABI_VERSION 1u

/** @brief Fixed capacity of the per-extension parameter block. */
#define RGBX_MAX_PARAMS 16u

/** @brief Maximum number of RGBX_PARAM_STRING parameters per extension. */
#define RGBX_MAX_STRING_PARAMS 4u

/** @brief Fixed storage size of one string parameter value, INCLUDING the
 *  NUL terminator (so the longest usable string is one byte shorter). */
#define RGBX_PARAM_STRING_MAX 32u

/** @brief Number of coarse audio bands (energy + beat flag each). Matches
 *  the firmware's beat-detector band count. */
#define RGBX_AUDIO_NUM_BANDS 4u

/** @brief Number of fine-grained audio display buckets (bar-graph style). */
#define RGBX_AUDIO_NUM_DISPLAY_BUCKETS 20u

/**
 * @brief Parameter types, mapped by the host onto the same BLE
 * characteristic presentation formats (CPF) the built-in animations use.
 */
enum rgbx_param_type {
    RGBX_PARAM_UINT32 = 0, /**< plain unsigned integer (4-byte BLE value) */
    RGBX_PARAM_COLOR = 1,  /**< 0x00RRGGBB, high byte ignored (color picker
                            *   in the companion app) */
    RGBX_PARAM_BOOL = 2,   /**< 0 or 1 (toggle in the companion app; 1-byte
                            *   BLE value) */
    RGBX_PARAM_STRING = 3, /**< UTF-8 string, at most RGBX_PARAM_STRING_MAX-1
                            *   bytes (text field in the companion app) */
};

/**
 * @brief Default value for one parameter; which member is read is keyed by
 * the parameter's rgbx_param_type.
 */
union rgbx_param_default {
    uint32_t u32;     /**< RGBX_PARAM_UINT32 */
    uint32_t color;   /**< RGBX_PARAM_COLOR, 0x00RRGGBB */
    uint32_t boolean; /**< RGBX_PARAM_BOOL, 0 or 1 */
    const char *str;  /**< RGBX_PARAM_STRING; must point at a NUL-terminated
                       *   string literal inside the extension image, at most
                       *   RGBX_PARAM_STRING_MAX-1 bytes long */
};

/**
 * @brief One user-tunable parameter, surfaced as a BLE characteristic named
 * `name` on the extension's auto-generated GATT service.
 */
struct rgbx_param_desc {
    const char *name;           /**< display name (CUD string); must be a
                                 *   string literal inside the extension
                                 *   image */
    enum rgbx_param_type type;  /**< value type. NOTE: stored as the enum
                                 *   itself (int-sized, 4 bytes with the EDK
                                 *   toolchain flags both sides share) — this
                                 *   is part of the ABI */
    union rgbx_param_default default_value; /**< value before any BLE write
                                             *   arrives, keyed by `type` */
};

/**
 * @brief Extension self-description. Exported as the `rgbx_manifest` symbol
 * and validated by the host at boot (abi_version, dims, param table — see
 * fw/src/extensions/extension_manifest.cpp for the exact rules).
 */
struct rgbx_manifest {
    uint32_t abi_version; /**< must be RGBX_ABI_VERSION */
    const char *name;     /**< animation display name (BLE "Animation Name") */
    uint32_t width;       /**< framebuffer width the extension renders at;
                           *   must match the host display (40 on proto0) */
    uint32_t height;      /**< framebuffer height (12 on proto0) */
    uint32_t param_count; /**< entries in `params`, <= RGBX_MAX_PARAMS; at
                           *   most RGBX_MAX_STRING_PARAMS of them may be
                           *   RGBX_PARAM_STRING */
    const struct rgbx_param_desc *params; /**< NULL iff param_count == 0 */
};

/**
 * @brief Per-tick input snapshot.
 *
 * The host (kernel mode) fills this in before signalling a tick; the
 * extension must treat it as read-only and must not assume values persist
 * across ticks. Absent sources (no IMU, no audio, no buttons on the build)
 * read as zeros.
 *
 * String parameter values do NOT use `params[]`: the i-th STRING-typed
 * parameter (counting only string params, in manifest declaration order)
 * lives in `param_strings[i]`. `params[i]` is unspecified for string params.
 */
struct rgbx_inputs {
    uint32_t dt_ms; /**< nominal ms since the previous tick */

    uint32_t params[RGBX_MAX_PARAMS]; /**< current scalar parameter values
                                       *   (UINT32/COLOR/BOOL), in manifest
                                       *   declaration order */

    char param_strings[RGBX_MAX_STRING_PARAMS][RGBX_PARAM_STRING_MAX];
    /**< current string parameter values, always NUL-terminated; slot i =
     *   the i-th STRING-typed param in declaration order */

    float accel[3]; /**< IMU accelerometer x/y/z, m/s^2 */
    float gyro[3];  /**< IMU gyroscope x/y/z, rad/s */

    float audio_band_energy[RGBX_AUDIO_NUM_BANDS];
    /**< smoothed energy per coarse band, low frequencies first */
    uint8_t audio_beat[RGBX_AUDIO_NUM_BANDS];
    /**< 1 if a beat was detected in that band this frame, else 0 */
    float audio_display_bucket[RGBX_AUDIO_NUM_DISPLAY_BUCKETS];
    /**< fine-grained spectrum buckets for bar-graph visualisation,
     *   normalized to roughly 0..1, low frequencies first */

    uint32_t buttons_pressed;
    /**< bit i set = button i was pressed since the previous tick. proto0
     *   mapping: 0=Up, 1=Left, 2=Right, 3=Down, 4=Wake */
};

/**
 * @brief Initializer for one scalar (UINT32/COLOR/BOOL) rgbx_param_desc
 * entry. Usable from both C and C++ manifests.
 */
#define RGBX_PARAM(name_, type_, default_u32_) \
    {(name_), (type_), {.u32 = (default_u32_)}}

/**
 * @brief Initializer for one RGBX_PARAM_STRING rgbx_param_desc entry.
 * `default_str_` must be a string literal (at most RGBX_PARAM_STRING_MAX-1
 * bytes).
 */
#define RGBX_PARAM_STR(name_, default_str_) \
    {(name_), RGBX_PARAM_STRING, {.str = (default_str_)}}

/** @brief Byte offset of pixel (x, y) in `rgbx_framebuffer` for a display
 *  `w` pixels wide. Layout is row-major, 3 bytes per pixel: R, G, B. */
#define RGBX_PIXEL_INDEX(w, x, y) ((((size_t)(y) * (size_t)(w)) + (size_t)(x)) * 3u)

/*
 * === Required exports ======================================================
 * Every extension must define all five of the following symbols and mark
 * each with EXPORT_SYMBOL(<name>) (from <zephyr/llext/symbol.h>) so the host
 * can resolve them with llext_find_sym(). The declarations below let the
 * C++ wrapper (and extension code itself) reference them type-safely.
 */

/** @brief The extension's manifest (const data). */
extern const struct rgbx_manifest rgbx_manifest;

/** @brief The input block the host writes before each tick. Must be a
 *  writable global (i.e. live in the extension's .data/.bss, not .rodata). */
extern struct rgbx_inputs rgbx_inputs;

/** @brief The scratch framebuffer the extension renders into, sized exactly
 *  width * height * 3 bytes (see RGBX_PIXEL_INDEX for layout). The host
 *  copies it to the real display after each successful tick. */
extern uint8_t rgbx_framebuffer[];

/** @brief Called once, on the sandboxed thread, after every (re)load and
 *  before the first tick. Globals are freshly reinitialized at this point
 *  (the extension is unloaded whenever it is not the active animation). */
void rgbx_init(void);

/** @brief Called once per frame, on the sandboxed thread. Renders one frame
 *  into rgbx_framebuffer using the current rgbx_inputs. Must return well
 *  within the host's tick deadline or the extension is aborted and
 *  unloaded. */
void rgbx_tick(void);

/** @brief Names the host uses with llext_find_sym(); kept next to the
 *  declarations so the two can't drift apart. */
#define RGBX_SYM_MANIFEST "rgbx_manifest"
#define RGBX_SYM_INPUTS "rgbx_inputs"
#define RGBX_SYM_FRAMEBUFFER "rgbx_framebuffer"
#define RGBX_SYM_INIT "rgbx_init"
#define RGBX_SYM_TICK "rgbx_tick"

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* RGBX_API_H_ */
