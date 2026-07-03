#pragma once

#include <cstddef>

/**
 * @brief Registers the BaseAnimation proxy for extension slot `slot` with
 * the animation registry under id extension_host::animationId(slot). The
 * proxy forwards init/setActive/tick onto the sandboxed extension via
 * extension_host (see extension_animation_proxy.cpp) and renders a scrolling
 * fault message when the extension is dead.
 *
 * @return 0 on success, negative errno if the slot is out of range or the
 *         animation registry rejected the entry (full / duplicate id).
 */
int extension_animation_proxy_register(size_t slot);
