#include "rto.h"
#include "sequence.h"

#include "../net_platform.h"

enum {
    TCP_RTT_NONE,
    TCP_RTT_CANDIDATE,
    TCP_RTT_AMBIGUOUS
};

static void arm(struct tcp_rto *rto, net_u32 right, net_u32 now_ms)
{
    rto->timer_started_ms = now_ms;
    /* Only bytes already sent at the time of this arm are eligible at expiry. */
    rto->timer_right = right;
}

static void measure(struct tcp_rto *rto, unsigned int elapsed_ms)
{
    unsigned long long sample, spread, estimate;

    if (!net_timeout_valid(elapsed_ms)) {
        return;
    }
    /* Keep fractional milliseconds for the filters. Wide intermediates cover
     * the full valid clock interval; constant shifts avoid i386 division. */
    sample = (unsigned long long)(elapsed_ms != 0U ? elapsed_ms : 1U) << 3;
    if (rto->srtt_q3 == 0U) {
        rto->srtt_q3 = sample;
        rto->variance_q3 = sample >> 1;
    } else {
        unsigned long long error = rto->srtt_q3 < sample ?
                                   sample - rto->srtt_q3 : rto->srtt_q3 - sample;

        rto->variance_q3 = (3U * rto->variance_q3 + error + 3U) >> 2;
        rto->srtt_q3 = (7U * rto->srtt_q3 + sample + 7U) >> 3;
    }
    spread = rto->variance_q3 << 2;
    estimate = (rto->srtt_q3 + (spread < 8U ? 8U : spread) + 7U) >> 3;
    rto->wait_ms = estimate > TCP_RTO_MAX_MS ? TCP_RTO_MAX_MS :
                   estimate < TCP_RTO_INITIAL_MS ? TCP_RTO_INITIAL_MS :
                   (unsigned int)estimate;
}

void tcp_rto_init(struct tcp_rto *rto, int short_syn_timeout)
{
    *rto = (struct tcp_rto){
        .wait_ms = short_syn_timeout ? TCP_RTO_SYN_FALLBACK_MS : TCP_RTO_INITIAL_MS
    };
}

void tcp_rto_new_data(struct tcp_rto *rto, net_u32 right, net_u32 sent_ms,
                      int start_timer)
{
    if (start_timer) {
        arm(rto, right, sent_ms);
    }
    if (rto->sample_state == TCP_RTT_NONE) {
        rto->sample_state = TCP_RTT_CANDIDATE;
        rto->sample_started_ms = sent_ms;
        rto->sample_right = right;
    }
}

void tcp_rto_retransmitted(struct tcp_rto *rto, net_u32 right, net_u32 sent_ms)
{
    /* An ACK covering this flight might have been prompted by the retry.
     * Wait for fresh data after it before timing another RTT. */
    rto->sample_state = TCP_RTT_AMBIGUOUS;
    rto->sample_right = right;
    ++rto->retries;
    rto->wait_ms = rto->wait_ms > TCP_RTO_MAX_MS / 2U ?
                   TCP_RTO_MAX_MS : rto->wait_ms * 2U;
    arm(rto, right, sent_ms);
}

void tcp_rto_acknowledged(struct tcp_rto *rto, net_u32 ack, unsigned int flight,
                          net_u32 now_ms)
{
    if (rto->sample_state != TCP_RTT_NONE) {
        enum tcp_seq_order order = tcp_seq_compare(ack, rto->sample_right);

        if (order == TCP_SEQ_EQUAL || order == TCP_SEQ_AFTER) {
            if (rto->sample_state == TCP_RTT_CANDIDATE) {
                measure(rto, net_elapsed_ms(rto->sample_started_ms, now_ms));
            }
            rto->sample_state = TCP_RTT_NONE;
        }
    }
    rto->retries = 0U;
    /* A zero flight disarms the timer; the next new send arms it again. */
    arm(rto, ack + flight, now_ms);
}
