#include "deterministic_platform.h"
#include "../../transport/lib/net/net_platform.h"

#include <stdio.h>
#include <string.h>

struct cancel_state {
    unsigned int calls;
    unsigned int cancel_after;
};

static int cancel_after_calls(void *context)
{
    struct cancel_state *state = context;
    ++state->calls;
    return state->calls >= state->cancel_after;
}

static int require(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "Phase B platform test failed: %s\n", message);
        return 0;
    }
    return 1;
}

int main(void)
{
    struct cancel_state cancellation = {0U, 3U};
    unsigned char first[16];
    unsigned char second[16];
    unsigned int start;
    int status;

    if (!require(strcmp(phase_b_test_platform_marker(),
                        "MINI_OS_PHASE_B_DETERMINISTIC_PLATFORM_ONLY") == 0,
                 "test-only marker changed") ||
        !require(net_timeout_valid(0U), "zero timeout rejected") ||
        !require(net_timeout_valid(0x7FFFFFFFU), "maximum timeout rejected") ||
        !require(!net_timeout_valid(0x80000000U), "oversized timeout accepted") ||
        !require(net_elapsed_ms(0xFFFFFFF0U, 0x00000010U) == 32U,
                 "wrapping elapsed time") ||
        !require(net_timeout_expired(0xFFFFFFF0U, 0x00000010U, 32U),
                 "wrapping deadline") ||
        !require(!net_timeout_expired(0xFFFFFFF0U, 0x00000010U, 33U),
                 "premature wrapping deadline") ||
        !require(net_timeout_remaining(0xFFFFFFF0U, 0x00000010U, 40U) == 8U,
                 "wrapping remaining time") ||
        !require(net_timeout_expired(123U, 123U, 0U),
                 "zero is not nonblocking")) {
        return 1;
    }

    phase_b_test_clock_set(100U);
    start = net_clock_now_ms();
    phase_b_test_clock_advance(5000U);
    if (!require(net_timeout_expired(start, net_clock_now_ms(), 4000U),
                 "delayed polling missed expiration")) {
        return 1;
    }

    if (!require(net_random_bytes(first, sizeof(first)) == (int)sizeof(first),
                 "first deterministic random request") ||
        !require(net_random_bytes(second, sizeof(second)) == (int)sizeof(second),
                 "second deterministic random request") ||
        !require(memcmp(first, second, sizeof(first)) != 0,
                 "deterministic random state did not advance")) {
        return 1;
    }

    net_set_cancel_callback(cancel_after_calls, &cancellation);
    phase_b_test_clock_set(0U);
    start = net_clock_now_ms();
    do {
        status = net_wait_status(start, net_clock_now_ms(), 100U);
        phase_b_test_clock_advance(1U);
    } while (status == NET_WAIT_CONTINUE);
    if (!require(status == NET_WAIT_CANCELLED,
                 "long wait did not observe cancellation") ||
        !require(cancellation.calls == cancellation.cancel_after,
                 "cancellation callback polling count")) {
        return 1;
    }

    net_set_cancel_callback(0, 0);
    if (!require(net_wait_status(0U, 0U, 0U) == NET_WAIT_TIMEOUT,
                 "zero-duration wait status") ||
        !require(net_wait_status(0U, 0U, 0x80000000U) == NET_WAIT_INVALID,
                 "invalid-duration wait status")) {
        return 1;
    }

    puts("Phase B deterministic platform test: PASS");
    return 0;
}
