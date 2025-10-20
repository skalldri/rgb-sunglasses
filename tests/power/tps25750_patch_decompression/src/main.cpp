/*
 * Copyright (c) 2016 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include "power.h"
#include "tps25750/tps25750-config.h"
#include "tps25750/tps25750-config-compressed.h"

#include "lz4.h"

#include <zephyr/random/random.h>

ZTEST_SUITE(tps25750_patch_decompression_tests, NULL, NULL, NULL, NULL, NULL);

ZTEST(tps25750_patch_decompression_tests, test_decompression_valid_patch)
{
    static char decompressed_data[15 * 1024];
    std::span<char> output = std::span<char>(decompressed_data, sizeof(decompressed_data));

    std::span<const char> compressed_patch = std::span<const char>(tps25750x_lowRegion_i2c_array_lz4, gtps25750x_lowRegion_i2c_array_lz4Size);
    std::span<const char> original_patch = std::span<const char>(tps25750x_lowRegion_i2c_array, gSizeLowRegionArray);

    int ret = power::decompress_tps25750_patch(compressed_patch, output);
    zassert_true(ret > 0, "Decompression failed: %d", ret);
    zassert_equal(ret, gSizeLowRegionArray, "Decompressed size does not match expected size: %d != %d", ret, gSizeLowRegionArray);
    zassert_mem_equal(output.data(), original_patch.data(), ret, "Decompressed data does not match expected data");
}

ZTEST(tps25750_patch_decompression_tests, test_decompression_buffer_too_small)
{
    static char decompressed_data[1 * 1024];
    std::span<char> output = std::span<char>(decompressed_data, sizeof(decompressed_data));

    std::span<const char> compressed_patch = std::span<const char>(tps25750x_lowRegion_i2c_array_lz4, gtps25750x_lowRegion_i2c_array_lz4Size);
    std::span<const char> original_patch = std::span<const char>(tps25750x_lowRegion_i2c_array, gSizeLowRegionArray);

    int ret = power::decompress_tps25750_patch(compressed_patch, output);
    zassert_false(ret > 0, "Decompression succeeded unexpectedly: %d", ret);
}

ZTEST(tps25750_patch_decompression_tests, test_decompression_junk_data)
{
    static char decompressed_data[15 * 1024];
    std::span<char> output = std::span<char>(decompressed_data, sizeof(decompressed_data));

    static char junk_data[15 * 1024];
    // Fill junk_data with random bytes
    for (size_t i = 0; i < sizeof(junk_data); i++)
    {
        junk_data[i] = sys_rand8_get();
    }
    std::span<const char> compressed_patch = std::span<const char>(junk_data, gtps25750x_lowRegion_i2c_array_lz4Size);

    std::span<const char> original_patch = std::span<const char>(tps25750x_lowRegion_i2c_array, gSizeLowRegionArray);

    int ret = power::decompress_tps25750_patch(compressed_patch, output);
    zassert_false(ret > 0, "Decompression succeeded unexpectedly: %d", ret);
}