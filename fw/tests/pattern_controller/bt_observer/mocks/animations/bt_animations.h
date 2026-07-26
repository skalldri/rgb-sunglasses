#pragma once

#include <cstddef>

/*
 * Mock standing in for src/animations/bt_animations.h, selected by include-path
 * precedence (this suite's mocks/ dir comes before src/ in the include order —
 * see CMakeLists.txt).
 *
 * The real header's BtPairingAnimation derives from BaseAnimationTemplate, which
 * drags in the animation registry, renderer and font atlas — none of which the
 * observer's behavior depends on. Only the two calls the observer actually makes
 * are modelled here, and they record their arguments so the test can assert the
 * passkey is forwarded.
 */

class BtPairingAnimation {
   public:
    static BtPairingAnimation *getInstance() {
        static BtPairingAnimation instance;
        return &instance;
    }

    void init() { initCount++; }
    void setPairingCode(unsigned int code) { lastPairingCode = code; }

    unsigned int lastPairingCode = 0;
    size_t initCount = 0;
};
