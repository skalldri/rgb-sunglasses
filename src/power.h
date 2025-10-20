#pragma once

#include <span>
#include <cstdint>

namespace power
{
    /**
     * @brief
     *
     * @param input_buffer
     * @param output_buffer
     * @return int
     */
    int decompress_tps25750_patch(std::span<const char> input_buffer,
                                  std::span<char> output_buffer);
};