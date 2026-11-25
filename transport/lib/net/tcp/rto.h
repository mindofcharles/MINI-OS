#ifndef MINI_OS_TCP_RTO_H
#define MINI_OS_TCP_RTO_H

#include "../internal.h"

enum {
    TCP_RTO_INITIAL_MS = 1000,
    TCP_RTO_SYN_FALLBACK_MS = 3000,
    TCP_RTO_MAX_MS = 60000,
    TCP_DATA_RETRY_LIMIT = 5
};

struct tcp_rto {
    unsigned long long srtt_q3;
    unsigned long long variance_q3;
    net_u32 timer_started_ms;
    net_u32 timer_right;
    net_u32 sample_started_ms;
    net_u32 sample_right;
    unsigned int wait_ms;
    net_u8 retries;
    net_u8 sample_state;
};

void tcp_rto_init(struct tcp_rto *rto, int short_syn_timeout);

/* right is the exclusive end of the transmitted flight after the send. */
void tcp_rto_new_data(struct tcp_rto *rto, net_u32 right, net_u32 sent_ms,
                      int start_timer);
void tcp_rto_retransmitted(struct tcp_rto *rto, net_u32 right, net_u32 sent_ms);
void tcp_rto_fast_retransmitted(struct tcp_rto *rto, net_u32 right, net_u32 sent_ms);
/* Flow-control pauses and probe replays are not losses or RTT measurements. */
void tcp_rto_mark_ambiguous(struct tcp_rto *rto, net_u32 right);
void tcp_rto_rearm(struct tcp_rto *rto, net_u32 right, net_u32 now_ms);

/* Called only for cumulative ACK progress, with the remaining flight size. */
void tcp_rto_acknowledged(struct tcp_rto *rto, net_u32 ack, unsigned int flight,
                          net_u32 now_ms);

#endif
