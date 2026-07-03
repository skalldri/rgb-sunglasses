#pragma once

#include <cstddef>

/**
 * Registers the BaseAnimation proxy for extension slot `slot` with the
 * animation registry under id extension_host::animationId(slot). The proxy
 * forwards init/setActive/tick onto the sandboxed extension via
 * extension_host (see extension_animation_proxy.cpp).
 */
void extension_animation_proxy_register(size_t slot);
