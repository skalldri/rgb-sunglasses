#include <extensions/rgbx_package_psa.h>
#include <psa/crypto.h>

namespace rgbx_package {

bool verifySha256Psa(void* context, const uint8_t* covered, size_t coveredSize,
                     const uint8_t expected[kDigestSize]) {
    static_cast<void>(context);
    if (covered == nullptr || expected == nullptr) {
        return false;
    }

    return psa_hash_compare(PSA_ALG_SHA_256, covered, coveredSize, expected, kDigestSize) ==
           PSA_SUCCESS;
}

}  // namespace rgbx_package
