/*
 * Copyright (c) 2016 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include "power.h"
#include "tps25750/tps25750-config.h"
#include "tps25750/tps25750-config-compressed.h"
#include "tps25750/tps25750_priv.h"

#include "lz4.h"

#include <zephyr/random/random.h>

ZTEST_SUITE(tps25750_patch_decompression_tests, NULL, NULL, NULL, NULL, NULL);

ZTEST(tps25750_patch_decompression_tests, test_decompression)
{
    const char *decompressed_data;
    size_t decompressed_size;

    int ret = tps25750_get_patch(&decompressed_data, &decompressed_size);
    zassert_equal(ret, 0, "Decompression failed: %d", ret);
    zassert_equal(decompressed_size, g_tps25750_patch_UncompressedSize, "Decompressed size does not match expected size: %d != %d", ret, g_tps25750_patch_UncompressedSize);
    zassert_mem_equal(decompressed_data, tps25750x_lowRegion_i2c_array, decompressed_size, "Decompressed data does not match expected data");
}