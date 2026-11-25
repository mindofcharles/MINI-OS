#ifndef MINI_OS_TCP_FLOW_H
#define MINI_OS_TCP_FLOW_H

#include "../internal.h"

enum { TCP_SHORT_OVERRIDE_MS = 200 };

struct tcp_flow {
    net_u32 short_started_ms;
    net_u32 persist_started_ms;
    unsigned int persist_wait_ms; /* Zero when disarmed. */
    net_u16 max_peer_window;
    net_u8 short_pending;
    net_u8 probe_sent; /* One newly probed byte, outside ordinary flight. */
};

unsigned int tcp_flow_send_length(struct tcp_flow *flow, unsigned int smss,
                                  unsigned int flight, unsigned int unsent,
                                  unsigned int eligible, net_u32 now_ms);
unsigned int tcp_flow_receive_window(unsigned int free_bytes, unsigned int granted,
                                     unsigned int rmss);

void tcp_flow_persist_start(struct tcp_flow *flow, net_u32 now_ms, unsigned int rto_ms);
int tcp_flow_persist_due(const struct tcp_flow *flow, net_u32 now_ms);
void tcp_flow_persist_sent(struct tcp_flow *flow, net_u32 sent_ms);

#endif
