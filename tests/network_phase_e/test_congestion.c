#include "../../transport/lib/net/tcp/congestion.h"
#include "../../transport/lib/net/tcp.h"

#include <stdio.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "TCP congestion test failure at line %d: %s\n", \
            __LINE__, #condition); return 0; } } while (0)

enum { SMSS = 100 };

static int growth_and_idle_restart(void)
{
    struct tcp_congestion cc;
    net_u32 before_wrap = UINT_MAX - 500U;
    net_u32 ack = 1U;

    tcp_congestion_init(&cc, SMSS);
    CHECK(cc.cwnd == SMSS && cc.ssthresh == TCP_QUEUE_CAPACITY);
    tcp_congestion_acknowledged(&cc, SMSS, ack, 1U, SMSS - 1U);
    CHECK(cc.cwnd == SMSS + 1U);
    tcp_congestion_acknowledged(&cc, SMSS, ack += SMSS * 2U, SMSS * 2U, 0U);
    CHECK(cc.cwnd == SMSS * 2U + 1U);
    for (unsigned int index = 0U; index < TCP_QUEUE_CAPACITY / SMSS + 1U; ++index) {
        tcp_congestion_acknowledged(&cc, SMSS, ack += SMSS, SMSS, 0U);
    }
    CHECK(cc.cwnd == TCP_QUEUE_CAPACITY);

    /* Loss reduction uses the actual flight, not the larger window. */
    tcp_congestion_timeout_sent(&cc, SMSS, ack + 1000U, 1000U, SMSS, 0U);
    CHECK(cc.cwnd == SMSS && cc.ssthresh == 500U);
    for (unsigned int index = 0U; index < 4U; ++index) {
        tcp_congestion_acknowledged(&cc, SMSS, ack += SMSS, SMSS, 1000U - (index + 1U) * SMSS);
    }
    CHECK(cc.cwnd == 500U && cc.avoidance_credit == 0U);
    tcp_congestion_acknowledged(&cc, SMSS, ack += 499U, 499U, 101U);
    CHECK(cc.cwnd == 500U && cc.avoidance_credit == 499U);
    tcp_congestion_acknowledged(&cc, SMSS, ++ack, 1U, 100U);
    CHECK(cc.cwnd == 600U && cc.avoidance_credit == 0U);
    tcp_congestion_acknowledged(&cc, SMSS, ack += 100U, 100U, 0U);
    CHECK(cc.cwnd == 600U && cc.avoidance_credit == 100U);

    tcp_congestion_new_sent(&cc, before_wrap - 999U);
    tcp_congestion_restart_idle(&cc, SMSS, before_wrap, 1000U);
    CHECK(cc.cwnd == 600U && cc.last_data_sent_ms == before_wrap - 999U);
    tcp_congestion_new_sent(&cc, before_wrap);
    tcp_congestion_restart_idle(&cc, SMSS, before_wrap + 1000U, 1000U);
    CHECK(cc.cwnd == SMSS && cc.ssthresh == 500U && cc.avoidance_credit == 0U);
    CHECK(cc.last_data_sent_ms == before_wrap);
    return 1;
}

static int timeout_prefix_accounting(void)
{
    struct tcp_congestion cc;
    const net_u32 oldest = UINT_MAX - 39U, end = oldest + 500U;

    tcp_congestion_init(&cc, SMSS);
    tcp_congestion_timeout_sent(&cc, SMSS, end, 500U, 80U, 1000U);
    CHECK(cc.ssthresh == 250U && cc.timeout_prefix == 80U);
    tcp_congestion_acknowledged(&cc, SMSS, oldest + 32U, 32U, 468U);
    CHECK(cc.timeout_prefix == 48U);
    tcp_congestion_timeout_sent(&cc, SMSS, end, 468U, 24U, 3010U);
    CHECK(cc.ssthresh == 250U && cc.timeout_prefix == 48U);
    tcp_congestion_timeout_sent(&cc, SMSS, end, 468U, 60U, 7010U);
    CHECK(cc.ssthresh == 250U && cc.timeout_prefix == 60U);
    tcp_congestion_acknowledged(&cc, SMSS, oldest + 92U, 60U, 408U);
    CHECK(cc.timeout_prefix == 0U);
    tcp_congestion_timeout_sent(&cc, SMSS, end, 408U, SMSS, 15020U);
    CHECK(cc.ssthresh == 204U && cc.timeout_prefix == SMSS);
    return 1;
}

static int recovery_credit_and_boundary(void)
{
    struct tcp_congestion cc;
    const net_u32 end = 0U, oldest = end - 5U * SMSS, partial = end - 3U * SMSS;

    tcp_congestion_init(&cc, SMSS);
    CHECK(tcp_congestion_duplicate(&cc, SMSS) == 0);
    CHECK(tcp_congestion_duplicate(&cc, SMSS) == 0);
    CHECK(tcp_congestion_duplicate(&cc, SMSS) == 1);
    CHECK(tcp_congestion_fast_bytes(&cc, oldest, 5U * SMSS) == 5U * SMSS && cc.cwnd == SMSS);
    tcp_congestion_fast_sent(&cc, SMSS, end, 5U * SMSS, 100U);
    CHECK(cc.ssthresh == 250U && cc.cwnd == 550U && cc.recovery_credit == SMSS);
    CHECK(tcp_congestion_duplicate(&cc, SMSS) == 1 && cc.cwnd == 650U);
    CHECK(tcp_congestion_duplicate(&cc, SMSS) == 0 && cc.cwnd == 650U);
    CHECK(tcp_congestion_fast_bytes(&cc, oldest, 5U * SMSS) == 0U);

    /* The recovery boundary excludes newly sent data, and zero is a real edge. */
    tcp_congestion_acknowledged(&cc, SMSS, partial, 2U * SMSS, 4U * SMSS);
    CHECK(cc.cwnd == 550U && tcp_congestion_fast_bytes(&cc, partial, 4U * SMSS) == 3U * SMSS);
    tcp_congestion_fast_sent(&cc, SMSS, end + SMSS, 4U * SMSS, 200U);
    CHECK(cc.recovery_end == end);
    tcp_congestion_acknowledged(&cc, SMSS, end, 3U * SMSS, SMSS);
    CHECK(cc.cwnd == 200U && tcp_congestion_fast_bytes(&cc, end, SMSS) == 0U);
    for (unsigned int index = 0U; index < 3U; ++index) {
        CHECK(tcp_congestion_duplicate(&cc, SMSS) == 0);
    }
    tcp_congestion_acknowledged(&cc, SMSS, end + 1U, 1U, SMSS - 1U);
    CHECK(tcp_congestion_duplicate(&cc, SMSS) == 0);
    CHECK(tcp_congestion_duplicate(&cc, SMSS) == 0);
    CHECK(tcp_congestion_duplicate(&cc, SMSS) == 1);
    return 1;
}

int test_tcp_congestion(void)
{
    if (!growth_and_idle_restart() || !timeout_prefix_accounting() ||
        !recovery_credit_and_boundary()) {
        return 0;
    }
    puts("E5b congestion growth, idle restart and recovery accounting: PASS");
    return 1;
}
