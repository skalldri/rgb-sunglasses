#include "comm_health.h"

CommHealthEvent comm_health_tick(CommHealth &health, bool read_ok, uint32_t entry_ticks) {
    if (read_ok) {
        health.fail_ticks = 0;
        if (health.in_error) {
            health.in_error = false;
            return CommHealthEvent::Recovered;
        }
        return CommHealthEvent::None;
    }

    // Saturate rather than wrap: an outage lasting 2^32 ticks would otherwise
    // briefly read as "no failures".
    if (health.fail_ticks < UINT32_MAX) {
        health.fail_ticks++;
    }
    if (!health.in_error && health.fail_ticks >= entry_ticks) {
        health.in_error = true;
        return CommHealthEvent::EnteredError;
    }
    return CommHealthEvent::None;
}
