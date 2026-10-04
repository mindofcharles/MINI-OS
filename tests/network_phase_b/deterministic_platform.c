#include "deterministic_platform.h"
#include "../../transport/lib/net/net_platform.h"

static const char test_only_marker[] =
    "MINI_OS_PHASE_B_DETERMINISTIC_PLATFORM_ONLY";
static unsigned int fake_clock;
static unsigned int fake_random_state = 0x4D494E49U;
static net_cancel_callback active_cancel_callback;
static void *active_cancel_context;

void phase_b_test_clock_set(unsigned int milliseconds)
{
    fake_clock = milliseconds;
}

void phase_b_test_clock_advance(unsigned int milliseconds)
{
    fake_clock += milliseconds;
}

const char *phase_b_test_platform_marker(void)
{
    return test_only_marker;
}

unsigned int net_clock_now_ms(void)
{
    return fake_clock;
}

int net_random_bytes(void *buffer, unsigned int length)
{
    unsigned char *output = buffer;
    unsigned int index;

    if (length != 0U && buffer == 0) {
        return -1;
    }
    for (index = 0U; index < length; ++index) {
        fake_random_state = fake_random_state * 1664525U + 1013904223U;
        output[index] = (unsigned char)(fake_random_state >> 24);
    }
    return (int)length;
}

int net_set_cancel_callback(net_cancel_callback callback, void *context)
{
    active_cancel_callback = callback;
    active_cancel_context = context;
    return 0;
}

int net_cancel_requested(void)
{
    if (active_cancel_callback == 0) {
        return 0;
    }
    return active_cancel_callback(active_cancel_context) != 0;
}
