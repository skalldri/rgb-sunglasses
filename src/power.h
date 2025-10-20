#pragma once

#include <span>
#include <cstdint>

namespace power
{
    int decompress_tps25750_patch(const std::span<uint8_t> input_buffer,
                                  std::span<uint8_t> output_buffer);
};