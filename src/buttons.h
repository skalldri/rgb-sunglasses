#pragma once

#include <button_event_listener.h>

/**
 * @brief Register a listener for button press events.
 *
 * The listener is invoked from thread context (system work queue),
 * not from the GPIO ISR. Only one listener is supported at a time.
 * Must not be called from ISR context.
 */
void buttons_register_listener(ButtonEventListener *listener);
