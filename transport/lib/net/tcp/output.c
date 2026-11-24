#include "internal.h"
#include "codec.h"

#include "../arp.h"
#include "../net_platform.h"

void tcp_finish_neighbor(const struct tcp_connection *connection)
{
    if (net_global_context.pending_arp.active &&
        net_global_context.pending_arp.owner == connection->handle) {
        (void)net_arp_task_finish(&net_global_context, connection->handle);
    }
}

int tcp_resolve_neighbor(const struct tcp_connection *connection,
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
            tcp_finish_neighbor(connection);
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
        tcp_finish_neighbor(connection);
    }

    result = net_arp_task_start(context, &connection->open.next_hop,
                               connection->handle, started_ms, timeout_ms);
    if (result == 0) {
        result = net_arp_task_result(context, connection->handle, mac);
        tcp_finish_neighbor(connection);
    }
    return result;
}

int tcp_transmit(const struct tcp_connection *connection,
                 const struct net_mac_addr *mac, struct tcp_segment_view *segment,
                 unsigned int tx_offset)
{
    struct net_context *context = &net_global_context;
    net_u8 *wire;
    int length;

    if (segment->payload_length != 0U &&
        ((segment->flags & TCP_FLAG_SYN) != 0U || segment->mss != 0U ||
         segment->payload_length > TCP_IPV4_DATA_MAX ||
         tx_offset > connection->tx_length ||
         segment->payload_length > connection->tx_length - tx_offset)) {
        return NET_ERR_INVALID;
    }
    wire = net_ethernet_begin_tx(context);
    if (wire == 0) {
        return NET_ERR_STATE;
    }
    wire += NET_IPV4_HEADER_SIZE;
    if (segment->payload_length != 0U) {
        net_u8 *data = wire + TCP_HEADER_MIN_SIZE;

        /* Both ring spans go directly to the shared Ethernet transmit frame. */
        tcp_ring_read(connection->tx_queue,
                       tcp_ring_offset(connection->tx_head, tx_offset),
                       data, segment->payload_length);
        segment->payload = data;
    }
    segment->source_port = connection->open.local_port;
    segment->destination_port = (net_u16)connection->endpoint.port;
    length = tcp_segment_encode(wire, NET_IPV4_PAYLOAD_MAX,
                                &context->config.address,
                                &connection->endpoint.address, segment);
    if (length < 0) {
        return length;
    }
    return net_ipv4_send(context, &connection->endpoint.address,
                         mac, NET_IPV4_PROTOCOL_TCP, (unsigned int)length);
}
