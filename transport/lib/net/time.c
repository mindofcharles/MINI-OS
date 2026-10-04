#include "net_platform.h"

#include <limits.h>

_Static_assert(UINT_MAX == 0xFFFFFFFFU,
               "network time requires a 32-bit unsigned int");

int net_timeout_valid(unsigned int duration_ms)
{
    return duration_ms <= 0x7FFFFFFFU;
}

unsigned int net_elapsed_ms(unsigned int start_ms, unsigned int now_ms)
{
    return now_ms - start_ms;
}

int net_timeout_expired(unsigned int start_ms, unsigned int now_ms,
                        unsigned int duration_ms)
{
    if (!net_timeout_valid(duration_ms)) {
        return 0;
    }
    return net_elapsed_ms(start_ms, now_ms) >= duration_ms;
}

unsigned int net_timeout_remaining(unsigned int start_ms,
                                   unsigned int now_ms,
                                   unsigned int duration_ms)
{
    unsigned int elapsed;

    if (!net_timeout_valid(duration_ms)) {
        return 0U;
    }
    elapsed = net_elapsed_ms(start_ms, now_ms);
    if (elapsed >= duration_ms) {
        return 0U;
    }
    return duration_ms - elapsed;
}

int net_wait_status(unsigned int start_ms, unsigned int now_ms,
                    unsigned int duration_ms)
{
    if (!net_timeout_valid(duration_ms)) {
        return NET_WAIT_INVALID;
    }
    if (net_cancel_requested()) {
        return NET_WAIT_CANCELLED;
    }
    if (net_timeout_expired(start_ms, now_ms, duration_ms)) {
        return NET_WAIT_TIMEOUT;
    }
    return NET_WAIT_CONTINUE;
}
