#include "flow.h"
#include "rto.h"

#include "../net_platform.h"
#include "../tcp.h"

unsigned int tcp_flow_send_length(struct tcp_flow *flow, unsigned int smss,
                                  unsigned int flight, unsigned int unsent,
                                  unsigned int eligible, net_u32 now_ms)
{
    unsigned int half_window = ((unsigned int)flow->max_peer_window + 1U) / 2U;

    if (eligible == 0U || eligible == smss || flow->probe_sent ||
        (flight == 0U && (eligible == unsent || eligible >= half_window))) {
        return eligible;
    }
    /* Keep one deadline for the same unsent head. Only a wire send or an ACK
     * of its already-sent probe can advance that head. */
    if (!flow->short_pending) {
        flow->short_pending = 1U;
        flow->short_started_ms = now_ms;
    }
    return net_timeout_expired(flow->short_started_ms, now_ms, TCP_SHORT_OVERRIDE_MS) ?
           eligible : 0U;
}

unsigned int tcp_flow_receive_window(unsigned int free_bytes, unsigned int granted,
                                     unsigned int rmss)
{
    unsigned int threshold = rmss < TCP_QUEUE_CAPACITY / 2U ? rmss : TCP_QUEUE_CAPACITY / 2U;

    /* A read frees bytes locally; it does not move the last advertised edge. */
    return free_bytes >= granted && free_bytes - granted >= threshold ? free_bytes : granted;
}

void tcp_flow_persist_start(struct tcp_flow *flow, net_u32 now_ms, unsigned int rto_ms)
{
    if (flow->persist_wait_ms == 0U) {
        flow->persist_started_ms = now_ms;
        flow->persist_wait_ms = rto_ms;
    }
}

int tcp_flow_persist_due(const struct tcp_flow *flow, net_u32 now_ms)
{
    return flow->persist_wait_ms != 0U &&
           net_timeout_expired(flow->persist_started_ms, now_ms, flow->persist_wait_ms);
}

void tcp_flow_persist_sent(struct tcp_flow *flow, net_u32 sent_ms)
{
    flow->persist_started_ms = sent_ms;
    flow->persist_wait_ms = flow->persist_wait_ms > TCP_RTO_MAX_MS / 2U ?
                            TCP_RTO_MAX_MS : flow->persist_wait_ms * 2U;
}
