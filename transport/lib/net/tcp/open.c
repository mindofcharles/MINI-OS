#include "internal.h"
#include "codec.h"

#include "../arp.h"
#include "../net_platform.h"

#include <string.h>

enum {
    TCP_IPV4_DEFAULT_MSS = 536,
    TCP_SYN_INITIAL_RTO_MS = 1000,
    TCP_SYN_MAX_RTO_MS = 60000,
    TCP_SYN_TRANSMISSIONS_MAX = 6,
    TCP_ACK_NEIGHBOR_TIMEOUT_MS = 120000
};

static int connect_expired(const struct tcp_connection *connection)
{
    return net_timeout_expired(connection->started_ms, net_clock_now_ms(),
                               connection->config.connect_timeout_ms);
}

static void finish_neighbor(const struct tcp_connection *connection)
{
    if (net_global_context.pending_arp.active &&
        net_global_context.pending_arp.owner == connection->handle) {
        (void)net_arp_task_finish(&net_global_context, connection->handle);
    }
}

static int neighbor_failure(const struct tcp_connection *connection)
{
    return tcp_connection_fail_event(
        connection->state == TCP_STATE_CONNECTING && connect_expired(connection) ?
        TCP_ERR_CONNECT_TIMEOUT : TCP_ERR_NEIGHBOR_TIMEOUT);
}

static int resolve_neighbor(const struct tcp_connection *connection,
                            net_u32 started_ms, unsigned int timeout_ms,
                            struct net_mac_addr *mac)
{
    struct net_context *context = &net_global_context;
    int result;

    if (net_timeout_expired(started_ms, net_clock_now_ms(), timeout_ms)) {
        return NET_ERR_TIMEOUT;
    }
    result = net_arp_cache_lookup(context, &connection->open.next_hop, mac);
    if (result != 0) {
        if (result > 0) {
            finish_neighbor(connection);
            return 0;
        }
        return result;
    }
    if (context->pending_arp.active) {
        if (context->pending_arp.owner != connection->handle) {
            return NET_ARP_TASK_PENDING;
        }
        result = net_arp_task_result(context, connection->handle, 0);
        if (result != 0) {
            return result;
        }
        /* A completed task whose cache entry expired must be refreshed. */
        finish_neighbor(connection);
    }

    result = net_arp_task_start(context, &connection->open.next_hop,
                               connection->handle, started_ms, timeout_ms);
    if (result == 0) {
        result = net_arp_task_result(context, connection->handle, mac);
        finish_neighbor(connection);
    }
    return result;
}

static int send_control(const struct tcp_connection *connection,
                        const struct net_mac_addr *mac, net_u8 flags,
                        net_u32 sequence, net_u32 acknowledgment)
{
    struct tcp_segment_view segment = {
        .sequence = sequence,
        .acknowledgment = acknowledgment,
        .source_port = connection->open.local_port,
        .destination_port = (net_u16)connection->endpoint.port,
        .mss = (flags & TCP_FLAG_SYN) != 0U ? TCP_IPV4_DEFAULT_MSS : 0U,
        .flags = flags
    };
    net_u8 *ip = net_ethernet_begin_tx(&net_global_context);
    int length;

    if (ip == 0) {
        return NET_ERR_STATE;
    }
    /* The advertised window opens with the receive stream in the next stage. */
    length = tcp_segment_encode(ip + NET_IPV4_HEADER_SIZE, NET_IPV4_PAYLOAD_MAX,
                                &net_global_context.config.address,
                                &connection->endpoint.address, &segment);
    if (length < 0) {
        return length;
    }
    return net_ipv4_send(&net_global_context, &connection->endpoint.address,
                         mac, NET_IPV4_PROTOCOL_TCP, (unsigned int)length);
}

static int send_pending_ack(struct tcp_connection *connection)
{
    struct tcp_active_open *open = &connection->open;
    int connecting = connection->state == TCP_STATE_CONNECTING;
    net_u32 started_ms = connecting ? connection->started_ms : open->ack_started_ms;
    unsigned int timeout_ms = connecting ? connection->config.connect_timeout_ms :
                                           TCP_ACK_NEIGHBOR_TIMEOUT_MS;
    struct net_mac_addr mac;
    int result;

    result = resolve_neighbor(connection, started_ms, timeout_ms, &mac);
    if (result == NET_ARP_TASK_PENDING) {
        return 0;
    }
    if (result == NET_ERR_TIMEOUT) {
        return neighbor_failure(connection);
    }
    if (result != 0) {
        return result;
    }
    if (net_timeout_expired(started_ms, net_clock_now_ms(), timeout_ms)) {
        return tcp_connection_fail_event(connecting ? TCP_ERR_CONNECT_TIMEOUT :
                                                      TCP_ERR_NEIGHBOR_TIMEOUT);
    }
    result = send_control(connection, &mac, TCP_FLAG_ACK,
                          open->iss + 1U, open->irs + 1U);
    if (result != 0) {
        return result;
    }
    /* Successful transmission commits the handshake even across a PIT tick. */
    open->ack_pending = 0;
    connection->state = TCP_STATE_ESTABLISHED;
    return 1;
}

static int queue_ack(struct tcp_connection *connection)
{
    struct tcp_active_open *open = &connection->open;
    int result;

    if (open->ack_pending) {
        return 0;
    }
    open->ack_started_ms = net_clock_now_ms();
    open->ack_pending = 1;
    result = send_pending_ack(connection);
    return result == 0 ? 1 : result;
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
    if (connection->state != TCP_STATE_CONNECTING ||
        (open->syn_transmissions != 0U &&
         !net_timeout_expired(open->syn_sent_ms, now_ms, open->syn_rto_ms))) {
        return 0;
    }
    if (open->syn_transmissions >= TCP_SYN_TRANSMISSIONS_MAX) {
        return tcp_connection_fail_event(TCP_ERR_RETRIES);
    }
    result = resolve_neighbor(connection, connection->started_ms,
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
    result = send_control(connection, &mac, TCP_FLAG_SYN, open->iss, 0U);
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

int tcp_open_input(struct tcp_connection *connection,
                    const struct net_ipv4_view *packet,
                    const struct tcp_segment_view *segment)
{
    struct tcp_active_open *open = &connection->open;
    net_u32 next_send = open->iss + 1U;
    struct net_mac_addr mac;
    int result;

    if (open->syn_transmissions == 0U ||
        segment->source_port != connection->endpoint.port ||
        segment->destination_port != open->local_port ||
        memcmp(packet->source.octets, connection->endpoint.address.octets, 4U) != 0 ||
        memcmp(packet->destination.octets, net_global_context.config.address.octets,
               4U) != 0) {
        return 0;
    }
    if (connection->state == TCP_STATE_ESTABLISHED) {
        if ((segment->flags & TCP_FLAG_RST) != 0U) {
            return segment->sequence == open->irs + 1U ?
                   tcp_connection_fail_event(TCP_ERR_RESET) : 0;
        }
        if (is_syn_ack(segment->flags) && segment->sequence == open->irs &&
            segment->acknowledgment == next_send) {
            return queue_ack(connection);
        }
        /* A closed receive window consumes no data or FIN. A pure in-sequence
         * ACK needs no reply, but a window or keepalive probe needs our ACK. */
        if ((segment->flags & (TCP_FLAG_ACK | TCP_FLAG_SYN)) == TCP_FLAG_ACK &&
            (tcp_segment_sequence_length(segment) != 0U ||
             segment->sequence != open->irs + 1U)) {
            return queue_ack(connection);
        }
        return 0;
    }
    if (connection->state != TCP_STATE_CONNECTING) {
        return 0;
    }
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
                              segment->acknowledgment, 0U);
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
    return queue_ack(connection);
}
