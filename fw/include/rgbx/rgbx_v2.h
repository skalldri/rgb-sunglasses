/**
 * @file rgbx_v2.h
 * @brief RGBX v2 memoryless WebAssembly guest ABI.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Stuart Alldritt
 *
 * This header describes guest imports and export annotations only. It exposes
 * no Zephyr types, native pointers, WASI, libc, filesystem, network, or BLE
 * surface. The firmware independently validates the compiled module before it
 * links any import.
 */
#pragma once

#include <stdint.h>

/** @brief RGBX WebAssembly ABI version described by this header. */
#define RGBX_V2_ABI_VERSION 2u
/** @brief Fixed display width in pixels. */
#define RGBX_V2_WIDTH 40u
/** @brief Fixed display height in pixels. */
#define RGBX_V2_HEIGHT 12u
/** @brief Total number of pixels in one complete frame. */
#define RGBX_V2_PIXEL_COUNT (RGBX_V2_WIDTH * RGBX_V2_HEIGHT)
/** @brief Number of consecutive pixels emitted by either span import. */
#define RGBX_V2_PIXELS_PER_SPAN 8u
/** @brief Maximum number of numeric parameter slots. */
#define RGBX_V2_MAX_PARAMS 16u
/** @brief Maximum number of string parameter slots. */
#define RGBX_V2_MAX_STRING_PARAMS 4u
/** @brief Storage bytes per string slot, including the NUL terminator. */
#define RGBX_V2_STRING_PARAM_SIZE 32u
/** @brief Number of coarse Q16 audio-energy bands. */
#define RGBX_V2_AUDIO_BAND_COUNT 4u
/** @brief Number of fine-grained Q16 audio display buckets. */
#define RGBX_V2_AUDIO_DISPLAY_BUCKET_COUNT 20u
/** @brief Number of axes in each IMU vector. */
#define RGBX_V2_IMU_AXIS_COUNT 3u
/** @brief Maximum numeric diagnostics emitted by one tick. */
#define RGBX_V2_DIAGNOSTIC_COUNT 4u
/** @brief Maximum Wasm payload bytes in the released memoryless profile. */
#define RGBX_V2_MODULE_MAX_BYTES 2048u

/*
 * Admission profile.
 *
 * Every structural limit the host applies to a memoryless RGBX v2 module is
 * declared once, here. The firmware admission path static_asserts its own
 * constants against these macros, the SDK packager copies them verbatim into
 * sdk-manifest.json, and the SDK's post-link gate reads them back out of that
 * manifest, so the three cannot describe different profiles without a build
 * failure or a gate failure.
 */

/** @brief Maximum functions, imported plus defined, in one module. */
#define RGBX_V2_MAX_FUNCTIONS 8u
/** @brief Maximum globals a module may define. Imported globals are refused. */
#define RGBX_V2_MAX_GLOBALS 8u
/** @brief Maximum locals declared by one function body. */
#define RGBX_V2_MAX_LOCALS_PER_FUNCTION 32u
/** @brief Fewest host imports a module may declare. */
#define RGBX_V2_MIN_IMPORTS 2u
/** @brief Most host imports a module may declare. */
#define RGBX_V2_MAX_IMPORTS 5u
/** @brief Maximum rgbx_v2_param_u32() calls admitted in one tick. */
#define RGBX_V2_MAX_PARAM_CALLS_PER_TICK 16u
/** @brief Maximum rgbx_v2_input_u32() calls admitted in one tick. */
#define RGBX_V2_MAX_INPUT_CALLS_PER_TICK 64u
/** @brief Span calls that cover one complete frame exactly once. */
#define RGBX_V2_SPAN_CALLS_PER_TICK (RGBX_V2_PIXEL_COUNT / RGBX_V2_PIXELS_PER_SPAN)

/*
 * Section-id bitmasks: bit N is set when standard WebAssembly section id N
 * belongs to the set. Admitted ids are type(1), import(2), function(3),
 * global(6), export(7) and code(10); every other id, including custom(0),
 * table(4), memory(5), start(8), element(9) and data(11), is refused. The
 * required set is the same list without the optional global section.
 */
/** @brief Bitmask of WebAssembly section ids the profile admits. */
#define RGBX_V2_SECTION_ALLOWED_MASK 0x04ceu
/** @brief Bitmask of WebAssembly section ids every module must carry. */
#define RGBX_V2_SECTION_REQUIRED_MASK 0x048eu

/** @brief Permission bit for pressed-button snapshots. */
#define RGBX_V2_CAPABILITY_BUTTONS (1u << 0)
/** @brief Permission bit for accelerometer and gyroscope snapshots. */
#define RGBX_V2_CAPABILITY_IMU (1u << 1)
/** @brief Permission bit for audio energies and beat snapshots. */
#define RGBX_V2_CAPABILITY_AUDIO (1u << 2)
/** @brief Mask of every sensor capability defined by RGBX v2. */
#define RGBX_V2_CAPABILITY_ALL \
    (RGBX_V2_CAPABILITY_BUTTONS | RGBX_V2_CAPABILITY_IMU | RGBX_V2_CAPABILITY_AUDIO)

/** @brief Selector passed to rgbx_v2_input_u32(). */
enum rgbx_v2_input_kind {
    RGBX_V2_INPUT_AUDIO_BAND_Q16 = 0,       /**< Q16 energy; index 0..3. */
    RGBX_V2_INPUT_AUDIO_DISPLAY_Q16 = 1,    /**< Q16 display energy; index 0..19. */
    RGBX_V2_INPUT_AUDIO_BEAT_MASK = 2,      /**< Beat bit mask; index must be zero. */
    RGBX_V2_INPUT_BUTTONS_PRESSED = 3,      /**< Pressed-button mask; index must be zero. */
    RGBX_V2_INPUT_ACCEL_MILLI = 4,          /**< Signed accelerometer axis; index 0..2. */
    RGBX_V2_INPUT_GYRO_MILLI = 5,           /**< Signed gyroscope axis; index 0..2. */
    RGBX_V2_INPUT_STRING_LENGTH = 6,        /**< String length; string slot 0..3. */
    RGBX_V2_INPUT_STRING_BYTE_SUM = 7,      /**< Unsigned byte sum; string slot 0..3. */
};

#if defined(__cplusplus)
/** @brief C-linkage declaration helper used by the flat guest ABI. */
#define RGBX_V2_EXTERN_C extern "C"
#else
/** @brief C-linkage declaration helper used by the flat guest ABI. */
#define RGBX_V2_EXTERN_C extern
#endif

#if defined(__wasm__)
/**
 * @brief Declare a function imported from the `rgbx_v2` host module.
 * @param name Exact host import field name.
 */
#define RGBX_V2_IMPORT(name) __attribute__((import_module("rgbx_v2"), import_name(name)))
/**
 * @brief Export a guest function under its stable RGBX v2 field name.
 * @param name Exact guest export field name.
 */
#define RGBX_V2_EXPORT(name) __attribute__((export_name(name)))
#else
/** @brief Host-parser no-op form of RGBX_V2_IMPORT(). */
#define RGBX_V2_IMPORT(name)
/** @brief Host-parser no-op form of RGBX_V2_EXPORT(). */
#define RGBX_V2_EXPORT(name)
#endif

/**
 * @brief Read one numeric parameter from the immutable tick snapshot.
 * @param id Numeric parameter slot, from zero through 15.
 * @return Parameter bits as an unsigned 32-bit value.
 */
RGBX_V2_EXTERN_C uint32_t rgbx_v2_param_u32(uint32_t id) RGBX_V2_IMPORT("param_u32");
/**
 * @brief Read one sensor, button, or bounded string-summary value.
 * @param kind One rgbx_v2_input_kind value.
 * @param index Kind-specific index documented by rgbx_v2_input_kind.
 * @return Snapshot bits; signed IMU values preserve their i32 representation.
 */
RGBX_V2_EXTERN_C uint32_t rgbx_v2_input_u32(uint32_t kind, uint32_t index)
    RGBX_V2_IMPORT("input_u32");
/**
 * @brief Publish this frame's shuffle-safe boundary signal.
 * @param value Zero or one; exactly one call is required when imported.
 */
RGBX_V2_EXTERN_C void rgbx_v2_set_good_moment(uint32_t value)
    RGBX_V2_IMPORT("set_good_moment");
/**
 * @brief Append one bounded numeric diagnostic to the current frame.
 * @param tag Guest-defined numeric tag.
 * @param value Guest-defined numeric value.
 */
RGBX_V2_EXTERN_C void rgbx_v2_debug_u32(uint32_t tag, uint32_t value)
    RGBX_V2_IMPORT("debug_u32");

/**
 * @brief Emit eight consecutive native RGB24 colors.
 * @param first_pixel Ordered linear offset: 0, 8, ..., 472.
 * @param color0 First 0x00RRGGBB color.
 * @param color1 Second 0x00RRGGBB color.
 * @param color2 Third 0x00RRGGBB color.
 * @param color3 Fourth 0x00RRGGBB color.
 * @param color4 Fifth 0x00RRGGBB color.
 * @param color5 Sixth 0x00RRGGBB color.
 * @param color6 Seventh 0x00RRGGBB color.
 * @param color7 Eighth 0x00RRGGBB color.
 */
RGBX_V2_EXTERN_C void rgbx_v2_set_span8(uint32_t first_pixel, uint32_t color0, uint32_t color1,
                                        uint32_t color2, uint32_t color3, uint32_t color4,
                                        uint32_t color5, uint32_t color6, uint32_t color7)
    RGBX_V2_IMPORT("set_span8");

/**
 * @brief Emit eight luma values interpolated between two RGB24 colors.
 * @param first_pixel Ordered linear offset: 0, 8, ..., 472.
 * @param foreground Foreground 0x00RRGGBB color selected at luma 255.
 * @param background Background 0x00RRGGBB color selected at luma zero.
 * @param luma0 First luma in the closed range 0..255.
 * @param luma1 Second luma in the closed range 0..255.
 * @param luma2 Third luma in the closed range 0..255.
 * @param luma3 Fourth luma in the closed range 0..255.
 * @param luma4 Fifth luma in the closed range 0..255.
 * @param luma5 Sixth luma in the closed range 0..255.
 * @param luma6 Seventh luma in the closed range 0..255.
 * @param luma7 Eighth luma in the closed range 0..255.
 */
RGBX_V2_EXTERN_C void rgbx_v2_set_luma_span8(uint32_t first_pixel, uint32_t foreground,
                                             uint32_t background, uint32_t luma0, uint32_t luma1,
                                             uint32_t luma2, uint32_t luma3, uint32_t luma4,
                                             uint32_t luma5, uint32_t luma6, uint32_t luma7)
    RGBX_V2_IMPORT("set_luma_span8");

/* Every guest exports exactly these signatures:
 *
 *   RGBX_V2_EXPORT("rgbx_init") void rgbx_init(void);
 *   RGBX_V2_EXPORT("rgbx_tick") void rgbx_tick(uint32_t dt_ms);
 *
 * A complete frame is 60 ordered calls to exactly one span function at
 * first_pixel offsets 0, 8, ..., 472. A guest that imports
 * set_good_moment must call it exactly once per successfully committed frame.
 */

#undef RGBX_V2_EXTERN_C
