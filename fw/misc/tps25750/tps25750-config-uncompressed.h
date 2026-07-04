#pragma once

/*
 * Serves the TPS25750 patch straight from rodata — no LZ4 code and no 14.3KB
 * RAM decompression buffer (issue #75 follow-up RAM pass).
 *
 * tps25750x_lowRegion_i2c_array in tps25750-config.c IS the uncompressed
 * patch: the tests/power/tps25750_patch_decompression suite asserts it is
 * byte-identical to the LZ4 blob's decompressed output, and asserts that the
 * size literal below matches sizeof(tps25750x_lowRegion_i2c_array) so the two
 * can never drift apart.
 *
 * The driver's uncompressed path (tps25750_get_patch() with
 * CONFIG_TPS25750_COMPRESSED_PATCH=n) expects the names `tps25750_patch` and
 * TPS25750_PATCH_UNCOMPRESSED_SIZE (see drivers/tps25750/Kconfig), which
 * tps25750-config.h does not provide — this header adapts them.
 */
#include "tps25750/tps25750-config.h"

#define TPS25750_PATCH_UNCOMPRESSED_SIZE 14592
#define tps25750_patch tps25750x_lowRegion_i2c_array
