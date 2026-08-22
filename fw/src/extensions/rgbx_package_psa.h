#pragma once

#include <extensions/rgbx_package.h>

namespace rgbx_package {

/**
 * @brief Verify an RGBX package trailer with PSA SHA-256.
 *
 * PSA Crypto must already be initialized by the platform. The callback has no
 * mutable context and can be passed directly to validate().
 */
bool verifySha256Psa(void* context, const uint8_t* covered, size_t coveredSize,
                     const uint8_t expected[kDigestSize]);

}  // namespace rgbx_package
