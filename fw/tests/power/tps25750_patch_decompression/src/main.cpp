/*
 * Copyright (c) 2016 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/random/random.h>
#include <zephyr/ztest.h>

#include "lz4.h"
#include "power.h"
#include "tps25750/tps25750-config-compressed.h"
#include "tps25750/tps25750-config.h"
#include "tps25750/tps25750_priv.h"
// Included LAST: this header #defines tps25750_patch to alias
// tps25750x_lowRegion_i2c_array, which would rewrite the compressed header's
// extern declaration if it came first. Its TPS25750_PATCH_UNCOMPRESSED_SIZE
// macro is an identical redefinition of the compressed header's (both 14592).
#include "tps25750/tps25750-config-uncompressed.h"

ZTEST_SUITE(tps25750_patch_decompression_tests, NULL, NULL, NULL, NULL, NULL);

// The uncompressed adapter header (used by proto0 to serve the patch straight
// from rodata) hardcodes TPS25750_PATCH_UNCOMPRESSED_SIZE rather than using
// sizeof, since the array is defined in a separate translation unit. Pin the
// literal to the real array size so the two can never drift apart.
ZTEST(tps25750_patch_decompression_tests, test_uncompressed_header_size) {
    zassert_equal((size_t)gSizeLowRegionArray, (size_t)TPS25750_PATCH_UNCOMPRESSED_SIZE,
                  "tps25750-config-uncompressed.h size literal (%d) != "
                  "sizeof(tps25750x_lowRegion_i2c_array) (%d)",
                  TPS25750_PATCH_UNCOMPRESSED_SIZE, gSizeLowRegionArray);
}

ZTEST(tps25750_patch_decompression_tests, test_decompression) {
    const char *decompressed_data;
    size_t decompressed_size;

    int ret = tps25750_get_patch(&decompressed_data, &decompressed_size);
    zassert_equal(ret, 0, "Decompression failed: %d", ret);
    zassert_equal(decompressed_size, g_tps25750_patch_UncompressedSize,
                  "Decompressed size does not match expected size: %d != %d", ret,
                  g_tps25750_patch_UncompressedSize);
    zassert_mem_equal(decompressed_data, tps25750x_lowRegion_i2c_array, decompressed_size,
                      "Decompressed data does not match expected data");
}