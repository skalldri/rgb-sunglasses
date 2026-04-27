#pragma once

#include <cstddef>

/**
 * @brief Listener for button press events.
 *
 * Implementations are called from a system work queue thread (not from
 * the GPIO interrupt context). Register with buttons_register_listener().
 */
class ButtonEventListener
{
public:
    virtual ~ButtonEventListener() = default;

    /**
     * @param buttonId Zero-based button index (0-3 for the four main buttons,
     *                 4 for the wake button).
     */
    virtual void onButtonPressed(size_t buttonId) = 0;
};
