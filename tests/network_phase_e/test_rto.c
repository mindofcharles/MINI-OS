#include "../../transport/lib/net/tcp/rto.h"

#include <stdio.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "E5a RTO test failure at line %d: %s\n", __LINE__, #condition); \
    return 0; \
} } while (0)

static int estimates_and_karn(void)
{
    struct tcp_rto rto;
    net_u32 start = UINT_MAX - 249U;

    tcp_rto_init(&rto, 0);
    CHECK(rto.wait_ms == 1000U);
    tcp_rto_new_data(&rto, 2U, start, 1);
    tcp_rto_acknowledged(&rto, UINT_MAX, 3U, start + 200U);
    CHECK(rto.srtt_q3 == 0U && rto.wait_ms == 1000U);
    CHECK(rto.timer_started_ms == start + 200U && rto.timer_right == 2U);
    /* The sample covers both a sequence wrap and a millisecond clock wrap. */
    tcp_rto_acknowledged(&rto, 2U, 0U, start + 500U);
    CHECK(rto.srtt_q3 == 4000U && rto.variance_q3 == 2000U && rto.wait_ms == 1500U);

    tcp_rto_new_data(&rto, 34U, 500U, 1);
    tcp_rto_acknowledged(&rto, 34U, 0U, 1500U);
    CHECK(rto.srtt_q3 == 4500U && rto.variance_q3 == 2500U && rto.wait_ms == 1813U);

    tcp_rto_new_data(&rto, 80U, 2000U, 1);
    tcp_rto_retransmitted(&rto, 80U, 3813U);
    CHECK(rto.wait_ms == 3626U && rto.retries == 1U);
    tcp_rto_acknowledged(&rto, 50U, 30U, 3850U);
    /* A partial ACK cannot make a subsequent cumulative sample unambiguous. */
    tcp_rto_new_data(&rto, 84U, 3860U, 0);
    tcp_rto_acknowledged(&rto, 84U, 0U, 4000U);
    CHECK(rto.srtt_q3 == 4500U && rto.variance_q3 == 2500U && rto.wait_ms == 3626U);
    CHECK(rto.retries == 0U);
    tcp_rto_new_data(&rto, 90U, 4100U, 1);
    tcp_rto_acknowledged(&rto, 90U, 0U, 4600U);
    CHECK(rto.wait_ms == 1555U);
    return 1;
}

static int timing_limits(void)
{
    struct tcp_rto rto;

    tcp_rto_init(&rto, 1);
    CHECK(rto.wait_ms == 3000U);
    tcp_rto_new_data(&rto, 1U, 0U, 1);
    tcp_rto_acknowledged(&rto, 1U, 0U, 0U);
    CHECK(rto.srtt_q3 == 8U && rto.wait_ms == 1000U);

    tcp_rto_init(&rto, 0);
    tcp_rto_new_data(&rto, 1U, 0U, 1);
    tcp_rto_acknowledged(&rto, 1U, 0U, 0x80000000U);
    CHECK(rto.srtt_q3 == 0U && rto.wait_ms == 1000U);
    tcp_rto_new_data(&rto, 2U, 0U, 1);
    tcp_rto_acknowledged(&rto, 2U, 0U, 0x7fffffffU);
    CHECK(rto.srtt_q3 == 0x3fffffff8ULL && rto.wait_ms == 60000U);
    tcp_rto_new_data(&rto, 3U, 0x80000000U, 1);
    tcp_rto_retransmitted(&rto, 3U, 0x8000ea60U);
    CHECK(rto.wait_ms == 60000U && rto.retries == 1U);
    return 1;
}

int test_tcp_rto(void)
{
    if (!estimates_and_karn() || !timing_limits()) {
        return 0;
    }
    puts("E5a TCP RTT estimator, Karn and RTO bounds: PASS");
    return 1;
}
