#include "net_platform.h"

#include "../platform.h"

static int keyboard_cancel(void *context)
{
    (void)context;
    return kbd_poll_key() > 0;
}

static net_cancel_callback active_cancel_callback = keyboard_cancel;
static void *active_cancel_context;

unsigned int net_clock_now_ms(void)
{
    return clock_monotonic_ms();
}

int net_random_bytes(void *buffer, unsigned int length)
{
    return get_random(buffer, length);
}

int net_set_cancel_callback(net_cancel_callback callback, void *context)
{
    if (callback == 0) {
        active_cancel_callback = keyboard_cancel;
        active_cancel_context = 0;
    }
    else {
        active_cancel_callback = callback;
        active_cancel_context = context;
    }
    return 0;
}

int net_cancel_requested(void)
{
    return active_cancel_callback(active_cancel_context) != 0;
}
