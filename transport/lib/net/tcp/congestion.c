#include "congestion.h"
#include "sequence.h"

#include "../net_platform.h"
#include "../tcp.h"

enum {
    TCP_CC_OPEN,
    TCP_CC_FAST_PENDING,
    TCP_CC_RECOVERY,
    TCP_CC_RECOVERY_PENDING,
    TCP_CC_GUARDED
};

enum { TCP_CC_DUP_ACK_LIMIT = 3 };

_Static_assert(TCP_QUEUE_CAPACITY <= USHRT_MAX, "congestion credit fits in 16 bits");

static unsigned int smaller(unsigned int left, unsigned int right)
{
    return left < right ? left : right;
}

static net_u16 bounded_window(unsigned int bytes)
{
    return (net_u16)smaller(bytes, TCP_QUEUE_CAPACITY);
}

static net_u16 loss_threshold(unsigned int flight, unsigned int smss)
{
    unsigned int half_flight = flight / 2U;
    unsigned int floor = 2U * smss;

    return bounded_window(half_flight > floor ? half_flight : floor);
}

void tcp_congestion_init(struct tcp_congestion *congestion, unsigned int smss)
{
    *congestion = (struct tcp_congestion){
        .cwnd = bounded_window(smss), .ssthresh = TCP_QUEUE_CAPACITY
    };
}

void tcp_congestion_acknowledged(struct tcp_congestion *congestion, unsigned int smss,
                                 net_u32 ack, unsigned int acknowledged, unsigned int flight)
{
    congestion->duplicate_acks = 0U;
    congestion->timeout_prefix -= (net_u16)smaller(acknowledged, congestion->timeout_prefix);

    if (congestion->state == TCP_CC_RECOVERY || congestion->state == TCP_CC_RECOVERY_PENDING) {
        enum tcp_seq_order order = tcp_seq_compare(ack, congestion->recovery_end);

        if (order == TCP_SEQ_EQUAL || order == TCP_SEQ_AFTER) {
            /* Deflate without releasing a full new-window burst on this ACK. */
            congestion->cwnd = (net_u16)smaller(congestion->ssthresh,
                                              (flight > smss ? flight : smss) + smss);
            congestion->recovery_credit = 0U;
            congestion->state = order == TCP_SEQ_AFTER ? TCP_CC_OPEN : TCP_CC_GUARDED;
        } else {
            unsigned int deflated = acknowledged < congestion->cwnd ?
                                    congestion->cwnd - acknowledged : 0U;

            congestion->cwnd = bounded_window(deflated + (acknowledged >= smss ? smss : 0U));
            congestion->state = TCP_CC_RECOVERY_PENDING;
        }
        /* Recovery ACKs do not also grow the normal congestion window. */
        return;
    }
    if (congestion->state == TCP_CC_FAST_PENDING ||
        (congestion->state == TCP_CC_GUARDED &&
         tcp_seq_compare(ack, congestion->recovery_end) == TCP_SEQ_AFTER)) {
        /* Advancing ACK invalidates an unsent initial fast retry. After a
         * committed recovery/timeout, only progress beyond its flight reopens it. */
        congestion->state = TCP_CC_OPEN;
    }
    if (congestion->cwnd < congestion->ssthresh) {
        congestion->cwnd = (net_u16)smaller(congestion->ssthresh,
                                          congestion->cwnd + smaller(acknowledged, smss));
    } else if (congestion->cwnd < TCP_QUEUE_CAPACITY) {
        unsigned int credit = congestion->avoidance_credit + acknowledged;

        if (credit >= congestion->cwnd) {
            credit -= congestion->cwnd;
            congestion->cwnd = bounded_window(congestion->cwnd + smss);
        }
        congestion->avoidance_credit = (net_u16)smaller(credit, congestion->cwnd - 1U);
    }
}

int tcp_congestion_duplicate(struct tcp_congestion *congestion, unsigned int smss)
{
    if (congestion->state == TCP_CC_OPEN) {
        if (++congestion->duplicate_acks == TCP_CC_DUP_ACK_LIMIT) {
            congestion->state = TCP_CC_FAST_PENDING;
            return 1;
        }
    } else if ((congestion->state == TCP_CC_RECOVERY ||
                congestion->state == TCP_CC_RECOVERY_PENDING) &&
               congestion->recovery_credit != 0U) {
        unsigned int previous = congestion->cwnd;
        unsigned int increase = smaller(congestion->recovery_credit, smss);

        congestion->cwnd = bounded_window(previous + increase);
        congestion->recovery_credit = (net_u16)(congestion->recovery_credit - increase);
        return congestion->cwnd != previous;
    }
    return 0;
}

void tcp_congestion_break_duplicate_run(struct tcp_congestion *congestion)
{
    congestion->duplicate_acks = 0U;
}

unsigned int tcp_congestion_fast_bytes(const struct tcp_congestion *congestion,
                                      net_u32 snd_una, unsigned int flight)
{
    if (congestion->state == TCP_CC_FAST_PENDING) {
        return flight;
    }
    if (congestion->state == TCP_CC_RECOVERY_PENDING) {
        /* A partial ACK cannot include the right edge of the recovery flight. */
        return smaller(congestion->recovery_end - snd_una, flight);
    }
    return 0U;
}

void tcp_congestion_restart_idle(struct tcp_congestion *congestion, unsigned int smss,
                                 net_u32 now_ms, unsigned int rto_ms)
{
    if (congestion->cwnd > smss &&
        net_timeout_expired(congestion->last_data_sent_ms, now_ms, rto_ms)) {
        congestion->cwnd = (net_u16)smss;
        congestion->avoidance_credit = 0U;
    }
}

void tcp_congestion_new_sent(struct tcp_congestion *congestion, net_u32 sent_ms)
{
    congestion->last_data_sent_ms = sent_ms;
}

void tcp_congestion_fast_sent(struct tcp_congestion *congestion, unsigned int smss,
                              net_u32 right, unsigned int flight, net_u32 sent_ms)
{
    if (congestion->state == TCP_CC_FAST_PENDING) {
        unsigned int following = flight > smss ? flight - smss : 0U;
        unsigned int initial = smaller(following, TCP_CC_DUP_ACK_LIMIT * smss);

        congestion->ssthresh = loss_threshold(flight, smss);
        congestion->cwnd = bounded_window(congestion->ssthresh + initial);
        /* Duplicate ACKs cannot inflate without bound. Infer at most the
         * original flight behind the oldest full-sized segment. */
        congestion->recovery_credit = (net_u16)(following - initial);
        congestion->avoidance_credit = 0U;
        congestion->recovery_end = right;
    }
    congestion->state = TCP_CC_RECOVERY;
    congestion->last_data_sent_ms = sent_ms;
}

void tcp_congestion_timeout_sent(struct tcp_congestion *congestion, unsigned int smss,
                                 net_u32 right, unsigned int flight, unsigned int retransmitted,
                                 net_u32 sent_ms)
{
    /* Timeout sends always start at SND.UNA. A partial ACK can refresh the
     * retry budget without making the remaining prefix a first timeout. */
    if (congestion->timeout_prefix == 0U) {
        congestion->ssthresh = loss_threshold(flight, smss);
    }
    if (retransmitted > congestion->timeout_prefix) {
        congestion->timeout_prefix = (net_u16)retransmitted;
    }
    congestion->cwnd = (net_u16)smss;
    congestion->avoidance_credit = 0U;
    congestion->recovery_credit = 0U;
    tcp_congestion_break_duplicate_run(congestion);
    congestion->state = TCP_CC_GUARDED;
    congestion->recovery_end = right;
    congestion->last_data_sent_ms = sent_ms;
}
