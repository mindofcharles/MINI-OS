#include "internal.h"

#include "../address.h"
#include "../arp.h"
#include "../ipv4.h"
#include "../net_platform.h"
#include "../../crypto/clear.h"

#include <limits.h>
#include <string.h>

_Static_assert(sizeof(struct tcp_context) <= TCP_CONTEXT_MAX_BYTES,
               "complete TCP context must fit in 12 KiB");
_Static_assert(TCP_QUEUE_CAPACITY * 2U <= sizeof(struct tcp_context),
               "TCP context owns both complete byte queues");

struct tcp_context tcp_global_context;

int tcp_config_validate(const struct tcp_config *config)
{
    return config != 0 && config->connect_timeout_ms != 0U &&
           config->close_grace_ms != 0U &&
           config->transmit_timeout_ms != 0U &&
           net_timeout_valid(config->connect_timeout_ms) &&
           net_timeout_valid(config->close_grace_ms) &&
           net_timeout_valid(config->transmit_timeout_ms) ? 0 : NET_ERR_INVALID;
}

int tcp_endpoint_validate(const struct tcp_endpoint *endpoint)
{
    struct net_ipv4_addr next_hop;

    if (endpoint == 0 || endpoint->port == 0U || endpoint->port > 65535U) {
        return NET_ERR_INVALID;
    }
    if (!net_global_context.initialized || tcp_global_context.disabled) {
        return NET_ERR_STATE;
    }
    return net_ipv4_select_next_hop(&net_global_context.config,
                                    &endpoint->address, &next_hop);
}

struct tcp_connection *tcp_connection_lookup(tcp_handle handle)
{
    if (handle == TCP_INVALID_HANDLE || tcp_global_context.disabled ||
        tcp_global_context.connection.handle != handle) {
        return 0;
    }
    return &tcp_global_context.connection;
}

int tcp_connection_allocate(const struct tcp_endpoint *endpoint,
                              const struct tcp_config *config,
                              net_u32 now_ms, tcp_handle *handle)
{
    struct net_transport_binding binding = {
        tcp_connection_input, tcp_connection_timer,
        tcp_connection_service_failure, &tcp_global_context,
        NET_IPV4_PROTOCOL_TCP, 0U
    };
    int result;

    if (handle == 0 || *handle != TCP_INVALID_HANDLE ||
        tcp_config_validate(config) != 0) {
        return NET_ERR_INVALID;
    }
    result = tcp_endpoint_validate(endpoint);
    if (result != 0) {
        return result;
    }
    if (net_global_context.service_active ||
        tcp_global_context.connection.handle != TCP_INVALID_HANDLE ||
        net_global_context.pending_arp.active ||
        net_global_context.pending_echo.active) {
        return NET_ERR_BUSY;
    }
    if (tcp_global_context.last_generation == UINT_MAX) {
        return TCP_ERR_GENERATION_EXHAUSTED;
    }
    binding.arp_owner = tcp_global_context.last_generation + 1U;
    if (net_global_context.transport.owner == 0) {
        result = net_transport_register(&net_global_context, &binding);
        if (result != 0) {
            return result;
        }
    } else if (net_global_context.transport.owner != &tcp_global_context ||
               net_global_context.transport.protocol != binding.protocol ||
               net_global_context.transport.input != binding.input ||
               net_global_context.transport.timer != binding.timer ||
               net_global_context.transport.failure != binding.failure) {
        return NET_ERR_BUSY;
    }
    memset(&tcp_global_context.connection, 0,
           sizeof(tcp_global_context.connection));
    tcp_global_context.connection.endpoint = *endpoint;
    tcp_global_context.connection.config = *config;
    tcp_global_context.connection.started_ms = now_ms;
    tcp_global_context.connection.state = TCP_STATE_CONNECTING;
    tcp_global_context.connection.handle =
        ++tcp_global_context.last_generation;
    net_global_context.transport.arp_owner =
        tcp_global_context.connection.handle;
    *handle = tcp_global_context.connection.handle;
    return 0;
}

int tcp_connection_fail(int result)
{
    struct tcp_connection *connection = &tcp_global_context.connection;
    tcp_handle handle = connection->handle;

    if (handle == TCP_INVALID_HANDLE) {
        return NET_ERR_STATE;
    }
    if (connection->state == TCP_STATE_FAILED ||
        connection->state == TCP_STATE_CLOSED) {
        return connection->terminal_result;
    }
    if (result >= 0 || result == NET_ERR_TIMEOUT ||
        result == TCP_ERR_WOULD_BLOCK || result == TCP_ERR_UNREAD) {
        return NET_ERR_INVALID;
    }
    if (net_global_context.pending_arp.active &&
        net_global_context.pending_arp.owner == handle) {
        (void)net_arp_task_finish(&net_global_context, handle);
    }
    net_global_context.transport.arp_owner = NET_ARP_OWNER_SYNC;
    crypto_clear(connection, (unsigned int)sizeof(*connection));
    connection->handle = handle;
    connection->state = TCP_STATE_FAILED;
    connection->terminal_result = result;
    return result;
}

int tcp_connection_complete(void)
{
    struct tcp_connection *connection = &tcp_global_context.connection;
    tcp_handle handle = connection->handle;

    if (handle == TCP_INVALID_HANDLE) {
        return NET_ERR_STATE;
    }
    if (connection->state == TCP_STATE_FAILED ||
        connection->state == TCP_STATE_CLOSED) {
        return connection->terminal_result;
    }
    if (!connection->wire_closed ||
        !connection->peer_fin || !connection->eof_observed ||
        connection->rx_length != 0U || connection->tx_length != 0U) {
        return TCP_ERR_UNREAD;
    }
    if (net_global_context.pending_arp.active &&
        net_global_context.pending_arp.owner == handle) {
        (void)net_arp_task_finish(&net_global_context, handle);
    }
    net_global_context.transport.arp_owner = NET_ARP_OWNER_SYNC;
    crypto_clear(connection, (unsigned int)sizeof(*connection));
    connection->handle = handle;
    connection->state = TCP_STATE_CLOSED;
    return 0;
}

int tcp_connection_begin_close(net_u32 now_ms)
{
    struct tcp_connection *connection = &tcp_global_context.connection;

    if (connection->handle == TCP_INVALID_HANDLE) {
        return NET_ERR_STATE;
    }
    if (connection->state == TCP_STATE_FAILED ||
        connection->state == TCP_STATE_CLOSED) {
        return connection->terminal_result;
    }
    if (connection->state == TCP_STATE_CONNECTING) {
        return NET_ERR_STATE;
    }
    if (!connection->close_started) {
        connection->close_started = 1;
        connection->close_started_ms = now_ms;
        connection->write_shutdown = 1;
        connection->fin_pending = 1;
        connection->state = connection->state == TCP_STATE_CLOSE_WAIT ?
                            TCP_STATE_LAST_ACK : TCP_STATE_FIN_WAIT_1;
    }
    return 0;
}

/* Prefix ordering helper shared by the future stream engine and contract tests. */
int tcp_connection_progress_result(unsigned int accepted, int result)
{
    if (tcp_global_context.connection.handle == TCP_INVALID_HANDLE) {
        return NET_ERR_STATE;
    }
    if (accepted > TCP_QUEUE_CAPACITY) {
        return NET_ERR_INVALID;
    }
    if (result < 0 && result != NET_ERR_TIMEOUT &&
        result != TCP_ERR_WOULD_BLOCK && result != TCP_ERR_UNREAD) {
        (void)tcp_connection_fail(result);
    }
    return accepted != 0U ? (int)accepted : result;
}

int tcp_connection_input(void *owner,
                          const struct net_ethernet_view *ethernet,
                          const struct net_ipv4_view *packet)
{
    (void)owner;
    (void)ethernet;
    (void)packet;
    /* No TCP codec/engine yet: consume as unsupported, never mutate a TCB. */
    ++net_global_context.counters.unsupported_frames;
    return 0;
}

void tcp_connection_service_failure(void *owner, int result)
{
    if (owner == &tcp_global_context &&
        tcp_global_context.connection.handle != TCP_INVALID_HANDLE) {
        (void)tcp_connection_fail(result);
    }
}
