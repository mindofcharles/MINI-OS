#ifndef MINI_OS_TCP_CONGESTION_H
#define MINI_OS_TCP_CONGESTION_H

#include "../internal.h"

struct tcp_congestion {
    net_u32 recovery_end; /* Exclusive sequence at entry or timeout. */
    net_u32 last_data_sent_ms;
    net_u16 cwnd;
    net_u16 ssthresh;
    net_u16 avoidance_credit;
    net_u16 recovery_credit; /* Remaining artificial inflation, in bytes. */
    net_u16 timeout_prefix; /* Unacknowledged bytes at SND.UNA already resent by RTO. */
    net_u8 duplicate_acks;
    net_u8 state;
};

void tcp_congestion_init(struct tcp_congestion *congestion, unsigned int smss);
void tcp_congestion_acknowledged(struct tcp_congestion *congestion, unsigned int smss,
                                 net_u32 ack, unsigned int acknowledged, unsigned int flight);
/* Only qualified, non-advancing ACKs with data in flight reach this hook. */
int tcp_congestion_duplicate(struct tcp_congestion *congestion, unsigned int smss);
void tcp_congestion_break_duplicate_run(struct tcp_congestion *congestion);
unsigned int tcp_congestion_fast_bytes(const struct tcp_congestion *congestion,
                                      net_u32 snd_una, unsigned int flight);
/* Apply before admitting new data, without changing the last-send time. */
void tcp_congestion_restart_idle(struct tcp_congestion *congestion, unsigned int smss,
                                 net_u32 now_ms, unsigned int rto_ms);

/* These hooks commit send effects, never a pending or deferred wire operation. */
void tcp_congestion_new_sent(struct tcp_congestion *congestion, net_u32 sent_ms);
void tcp_congestion_fast_sent(struct tcp_congestion *congestion, unsigned int smss,
                              net_u32 right, unsigned int flight, net_u32 sent_ms);
void tcp_congestion_timeout_sent(struct tcp_congestion *congestion, unsigned int smss,
                                 net_u32 right, unsigned int flight, unsigned int retransmitted,
                                 net_u32 sent_ms);

#endif
