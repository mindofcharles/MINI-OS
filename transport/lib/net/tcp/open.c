#include "internal.h"
#include "codec.h"

#include "../arp.h"
#include "../net_platform.h"

enum {
    TCP_SYN_INITIAL_RTO_MS = 1000,
    TCP_SYN_MAX_RTO_MS = 60000,
    TCP_SYN_TRANSMISSIONS_MAX = 6
};

static int connect_expired(const struct tcp_connection *connection)
{
    return net_timeout_expired(connection->started_ms, net_clock_now_ms(),
                               connection->config.connect_timeout_ms);
}

static int neighbor_failure(const struct tcp_connection *connection)
{
    return tcp_connection_fail_event(connect_expired(connection) ?
                        TCP_ERR_CONNECT_TIMEOUT : TCP_ERR_NEIGHBOR_TIMEOUT);
}

static int send_control(const struct tcp_connection *connection,
                        const struct net_mac_addr *mac, net_u8 flags,
                        net_u32 sequence, net_u32 acknowledgment, net_u16 window)
{
    struct tcp_segment_view segment = {
        .sequence = sequence,
        .acknowledgment = acknowledgment,
        .window = window,
        .mss = (flags & TCP_FLAG_SYN) != 0U ? TCP_IPV4_DEFAULT_MSS : 0U,
        .flags = flags
    };
    return tcp_transmit(connection, mac, &segment, 0U);
}

static int send_pending_ack(struct tcp_connection *connection)
{
    struct tcp_active_open *open = &connection->open;
    struct net_mac_addr mac;
    int result;

    result = tcp_resolve_neighbor(connection, connection->started_ms,
                                  connection->config.connect_timeout_ms, &mac);
    if (result == NET_ARP_TASK_PENDING) {
        return 0;
    }
    if (result == NET_ERR_TIMEOUT) {
        return neighbor_failure(connection);
    }
    if (result != 0) {
        return result;
    }
    if (connect_expired(connection)) {
        return tcp_connection_fail_event(TCP_ERR_CONNECT_TIMEOUT);
    }
    result = send_control(connection, &mac, TCP_FLAG_ACK,
                          open->iss + 1U, open->irs + 1U, TCP_QUEUE_CAPACITY);
    if (result != 0) {
        return result;
    }
    /* Successful transmission commits the handshake even across a PIT tick. */
    open->ack_pending = 0;
    tcp_stream_start(connection);
    connection->state = TCP_STATE_ESTABLISHED;
    return 1;
}

int tcp_open_timer(struct tcp_connection *connection, net_u32 now_ms)
{
    struct tcp_active_open *open = &connection->open;
    struct net_mac_addr mac;
    int result;

    if (open->local_port == 0U) {
        return 0;
    }
    if (open->ack_pending) {
        return send_pending_ack(connection);
    }
    if (open->syn_transmissions != 0U &&
        !net_timeout_expired(open->syn_sent_ms, now_ms, open->syn_rto_ms)) {
        return 0;
    }
    if (open->syn_transmissions >= TCP_SYN_TRANSMISSIONS_MAX) {
        return tcp_connection_fail_event(TCP_ERR_RETRIES);
    }
    result = tcp_resolve_neighbor(connection, connection->started_ms,
                                  connection->config.connect_timeout_ms, &mac);
    if (result == NET_ERR_TIMEOUT) {
        return neighbor_failure(connection);
    }
    if (result != 0) {
        return result == NET_ARP_TASK_PENDING ? 0 : result;
    }
    if (connect_expired(connection)) {
        return tcp_connection_fail_event(TCP_ERR_CONNECT_TIMEOUT);
    }
    result = send_control(connection, &mac, TCP_FLAG_SYN, open->iss, 0U, 0U);
    if (result != 0) {
        return result;
    }
    open->syn_sent_ms = net_clock_now_ms();
    open->syn_rto_ms = open->syn_transmissions == 0U ? TCP_SYN_INITIAL_RTO_MS :
                                                      open->syn_rto_ms * 2U;
    if (open->syn_rto_ms > TCP_SYN_MAX_RTO_MS) {
        open->syn_rto_ms = TCP_SYN_MAX_RTO_MS;
    }
    ++open->syn_transmissions;
    return 1;
}

static int is_syn_ack(net_u8 flags)
{
    unsigned int mask = TCP_FLAG_SYN | TCP_FLAG_ACK | TCP_FLAG_RST |
                        TCP_FLAG_FIN | TCP_FLAG_URG;
    return (flags & mask) == (TCP_FLAG_SYN | TCP_FLAG_ACK);
}

int tcp_open_is_duplicate(const struct tcp_connection *connection,
                           const struct tcp_segment_view *segment)
{
    return is_syn_ack(segment->flags) &&
           segment->sequence == connection->open.irs &&
           segment->acknowledgment == connection->open.iss + 1U;
}

int tcp_open_input(struct tcp_connection *connection,
                    const struct tcp_segment_view *segment)
{
    struct tcp_active_open *open = &connection->open;
    net_u32 next_send = open->iss + 1U;
    struct net_mac_addr mac;
    int result;

    if (connect_expired(connection)) {
        return tcp_connection_fail_event(TCP_ERR_CONNECT_TIMEOUT);
    }
    if ((segment->flags & TCP_FLAG_ACK) == 0U) {
        return 0;
    }
    if (segment->acknowledgment != next_send) {
        if ((segment->flags & TCP_FLAG_RST) != 0U) {
            return 0;
        }
        /* An unacceptable ACK must not start or replace a neighbor task. */
        result = net_arp_cache_lookup(&net_global_context, &open->next_hop, &mac);
        if (result <= 0) {
            return result;
        }
        if (connect_expired(connection)) {
            return tcp_connection_fail_event(TCP_ERR_CONNECT_TIMEOUT);
        }
        result = send_control(connection, &mac, TCP_FLAG_RST,
                              segment->acknowledgment, 0U, 0U);
        return result == 0 ? 1 : result;
    }
    if ((segment->flags & TCP_FLAG_RST) != 0U) {
        return tcp_connection_fail_event(TCP_ERR_REFUSED);
    }
    if (!is_syn_ack(segment->flags) || open->ack_pending) {
        return 0;
    }
    open->irs = segment->sequence;
    open->peer_mss = segment->mss == 0U ? TCP_IPV4_DEFAULT_MSS : segment->mss;
    open->peer_window = segment->window;
    open->ack_pending = 1;
    result = send_pending_ack(connection);
    return result == 0 ? 1 : result;
}
