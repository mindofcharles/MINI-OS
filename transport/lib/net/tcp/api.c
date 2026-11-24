#include "internal.h"

#include "../arp.h"
#include "../net_platform.h"
#include "../../crypto/clear.h"

#include <string.h>

int tcp_config_defaults(struct tcp_config *config)
{
    if (config == 0) {
        return NET_ERR_INVALID;
    }
    config->connect_timeout_ms = TCP_CONNECT_TIMEOUT_DEFAULT_MS;
    config->close_grace_ms = TCP_CLOSE_GRACE_DEFAULT_MS;
    config->transmit_timeout_ms = TCP_TRANSMIT_TIMEOUT_DEFAULT_MS;
    return 0;
}

static int lookup(tcp_handle handle, struct tcp_connection **connection)
{
    if (net_global_context.service_active) {
        return NET_ERR_BUSY;
    }
    if (!net_global_context.initialized) {
        return NET_ERR_STATE;
    }
    *connection = tcp_connection_lookup(handle);
    return *connection == 0 ? NET_ERR_STATE : 0;
}

static int connection_result(const struct tcp_connection *connection, int result)
{
    return connection->state == TCP_STATE_FAILED ? connection->terminal_result :
                                                   result;
}

static int start_connect(tcp_handle *handle, const struct tcp_endpoint *endpoint,
                         const struct tcp_config *config, net_u32 started_ms)
{
    struct tcp_active_open open = {0};
    int result;

    if (tcp_global_context.last_generation == UINT_MAX) {
        return TCP_ERR_GENERATION_EXHAUSTED;
    }
    if (net_global_context.transport.owner != 0 &&
        net_global_context.transport.owner != &tcp_global_context) {
        return NET_ERR_BUSY;
    }
    result = net_service_acquire(&net_global_context);
    if (result != 0) {
        return result;
    }
    result = net_cancel_requested() ? NET_ERR_CANCELLED :
                                      tcp_open_prepare(endpoint, started_ms, &open);
    if (result == 0 && net_cancel_requested()) {
        result = NET_ERR_CANCELLED;
    }
    net_service_release(&net_global_context);
    if (result != 0) {
        return result;
    }
    if (net_timeout_expired(started_ms, net_clock_now_ms(),
                            config->connect_timeout_ms)) {
        return TCP_ERR_CONNECT_TIMEOUT;
    }
    result = tcp_connection_allocate(endpoint, config, started_ms, handle);
    if (result == 0) {
        tcp_global_context.connection.open = open;
    }
    return result;
}

static int wait_connect(struct tcp_connection *connection, net_u32 started_ms,
                        unsigned int wait_ms)
{
    int result = net_service_acquire(&net_global_context);

    if (result != 0) {
        return result;
    }
    for (;;) {
        net_u32 now = net_clock_now_ms();

        if (net_timeout_expired(connection->started_ms, now,
                                connection->config.connect_timeout_ms)) {
            result = tcp_connection_fail(TCP_ERR_CONNECT_TIMEOUT);
            break;
        }
        if (wait_ms != 0U && net_timeout_expired(started_ms, now, wait_ms)) {
            result = NET_ERR_TIMEOUT;
            break;
        }
        result = connection_result(connection,
            net_service_once(&net_global_context, started_ms, wait_ms));
        if (result < 0) {
            break;
        }
        if (connection->state == TCP_STATE_ESTABLISHED) {
            result = 0;
            break;
        }
        if (wait_ms == 0U) {
            result = net_timeout_expired(connection->started_ms,
                                         net_clock_now_ms(),
                                         connection->config.connect_timeout_ms) ?
                     tcp_connection_fail(TCP_ERR_CONNECT_TIMEOUT) :
                     TCP_ERR_WOULD_BLOCK;
            break;
        }
    }
    net_service_release(&net_global_context);
    return result;
}

int tcp_connect(tcp_handle *handle, const struct tcp_endpoint *endpoint,
                 const struct tcp_config *config, unsigned int wait_ms)
{
    struct tcp_config selected;
    struct tcp_connection *connection;
    net_u32 started_ms;
    int result;

    if (handle == 0 || !net_timeout_valid(wait_ms)) {
        return NET_ERR_INVALID;
    }
    started_ms = net_clock_now_ms();
    if (config == 0) {
        (void)tcp_config_defaults(&selected);
    } else {
        selected = *config;
    }
    if (tcp_config_validate(&selected) != 0) {
        return NET_ERR_INVALID;
    }
    result = tcp_endpoint_validate(endpoint);
    if (result != 0) {
        return result;
    }
    if (net_global_context.service_active) {
        return NET_ERR_BUSY;
    }
    if (*handle != TCP_INVALID_HANDLE) {
        connection = tcp_connection_lookup(*handle);
        if (connection == 0) {
            return NET_ERR_STATE;
        }
        if (connection->state == TCP_STATE_FAILED) {
            return connection->terminal_result;
        }
        if (connection->state == TCP_STATE_CLOSED) {
            return TCP_ERR_CLOSED;
        }
        if (connection->endpoint.port != endpoint->port ||
            memcmp(connection->endpoint.address.octets,
                   endpoint->address.octets, 4U) != 0 ||
            connection->config.connect_timeout_ms != selected.connect_timeout_ms ||
            connection->config.close_grace_ms != selected.close_grace_ms ||
            connection->config.transmit_timeout_ms != selected.transmit_timeout_ms) {
            return NET_ERR_INVALID;
        }
        if (connection->state == TCP_STATE_ESTABLISHED) {
            return 0;
        }
        if (connection->state != TCP_STATE_CONNECTING) {
            return TCP_ERR_CLOSED;
        }
    } else {
        if (tcp_global_context.connection.handle != TCP_INVALID_HANDLE ||
            net_global_context.pending_arp.active ||
            net_global_context.pending_echo.active) {
            return NET_ERR_BUSY;
        }
        result = start_connect(handle, endpoint, &selected, started_ms);
        if (result != 0) {
            return result;
        }
        connection = &tcp_global_context.connection;
    }
    return wait_connect(connection, started_ms, wait_ms);
}

int tcp_send(tcp_handle handle, const void *data, unsigned int length,
              unsigned int wait_ms)
{
    struct tcp_connection *connection;
    int result;

    if ((length != 0U && data == 0) || !net_timeout_valid(wait_ms)) {
        return NET_ERR_INVALID;
    }
    result = lookup(handle, &connection);
    if (result != 0) {
        return result;
    }
    if (connection->state == TCP_STATE_FAILED) {
        return connection->terminal_result;
    }
    if (connection->state == TCP_STATE_CLOSED || connection->write_shutdown) {
        return TCP_ERR_CLOSED;
    }
    if (connection->state == TCP_STATE_CONNECTING) {
        return TCP_ERR_WOULD_BLOCK;
    }
    return length == 0U ? 0 : TCP_ERR_UNSUPPORTED;
}

int tcp_recv(tcp_handle handle, void *data, unsigned int capacity,
              unsigned int wait_ms)
{
    struct tcp_connection *connection;
    int result;

    if (data == 0 || capacity == 0U || !net_timeout_valid(wait_ms)) {
        return NET_ERR_INVALID;
    }
    result = lookup(handle, &connection);
    if (result != 0) {
        return result;
    }
    if (connection->state == TCP_STATE_FAILED) {
        return connection->terminal_result;
    }
    if (connection->state == TCP_STATE_CLOSED) {
        return 0;
    }
    if (connection->state == TCP_STATE_CONNECTING) {
        return TCP_ERR_WOULD_BLOCK;
    }
    return TCP_ERR_UNSUPPORTED;
}

int tcp_status(tcp_handle handle, struct tcp_status *status)
{
    struct tcp_connection *connection;
    struct tcp_status snapshot;
    int result;

    if (status == 0) {
        return NET_ERR_INVALID;
    }
    result = lookup(handle, &connection);
    if (result != 0) {
        return result;
    }
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.state = connection->state;
    snapshot.terminal_result = connection->terminal_result;
    snapshot.readable_bytes = connection->rx_length;
    /* No public stream engine yet: never advertise send readiness. */
    if (connection->rx_length != 0U) {
        snapshot.flags |= TCP_STATUS_READABLE;
    }
    if ((connection->peer_fin && connection->rx_length == 0U) ||
        connection->state == TCP_STATE_CLOSED) {
        snapshot.flags |= TCP_STATUS_PEER_EOF;
    }
    if (connection->write_shutdown || connection->state == TCP_STATE_CLOSED) {
        snapshot.flags |= TCP_STATUS_WRITE_SHUTDOWN;
    }
    if (connection->state == TCP_STATE_CLOSED ||
        connection->state == TCP_STATE_FAILED) {
        snapshot.flags |= TCP_STATUS_TERMINAL;
    }
    *status = snapshot;
    return 0;
}

int tcp_shutdown_write(tcp_handle handle)
{
    struct tcp_connection *connection;
    net_u32 now;
    int result = lookup(handle, &connection);

    if (result != 0) {
        return result;
    }
    if (connection->open.local_port != 0U) {
        return connection->state == TCP_STATE_CONNECTING ? NET_ERR_STATE :
                                                          TCP_ERR_UNSUPPORTED;
    }
    now = net_clock_now_ms();
    result = connection_result(connection,
        tcp_connection_timer(&tcp_global_context, now));
    return result < 0 ? result : tcp_connection_begin_close(now);
}

int tcp_close(tcp_handle handle, unsigned int wait_ms)
{
    struct tcp_connection *connection;
    net_u32 started_ms;
    int result;

    if (!net_timeout_valid(wait_ms)) {
        return NET_ERR_INVALID;
    }
    result = lookup(handle, &connection);
    if (result != 0) {
        return result;
    }
    if (connection->state == TCP_STATE_FAILED ||
        connection->state == TCP_STATE_CLOSED) {
        return connection->terminal_result;
    }
    if (connection->open.local_port != 0U) {
        return connection->state == TCP_STATE_CONNECTING ? NET_ERR_STATE :
                                                          TCP_ERR_UNSUPPORTED;
    }
    started_ms = net_clock_now_ms();
    result = net_service_acquire(&net_global_context);
    if (result != 0) {
        return result;
    }
    result = connection_result(connection,
        tcp_connection_timer(&tcp_global_context, started_ms));
    if (result < 0) {
        goto done;
    }
    if (connection->rx_length != 0U) {
        result = TCP_ERR_UNREAD;
        goto done;
    }
    if (connection->wire_closed) {
        result = tcp_connection_complete();
        goto done;
    }
    result = tcp_connection_begin_close(started_ms);
    if (result != 0) {
        goto done;
    }
    for (;;) {
        result = connection_result(connection,
            net_service_once(&net_global_context, started_ms, wait_ms));
        if (result < 0) {
            goto done;
        }
        result = connection_result(connection,
            tcp_connection_timer(&tcp_global_context, net_clock_now_ms()));
        if (result < 0) {
            goto done;
        }
        if (connection->rx_length != 0U) {
            result = TCP_ERR_UNREAD;
            goto done;
        }
        if (connection->wire_closed) {
            result = tcp_connection_complete();
            goto done;
        }
        if (wait_ms == 0U) {
            result = TCP_ERR_WOULD_BLOCK;
            goto done;
        }
        if (net_timeout_expired(started_ms, net_clock_now_ms(), wait_ms)) {
            result = NET_ERR_TIMEOUT;
            goto done;
        }
    }
done:
    net_service_release(&net_global_context);
    return result;
}

int tcp_abort(tcp_handle handle)
{
    struct tcp_connection *connection;
    int result = lookup(handle, &connection);

    if (result != 0) {
        return result;
    }
    if (connection->state != TCP_STATE_FAILED &&
        connection->state != TCP_STATE_CLOSED) {
        (void)tcp_connection_fail(TCP_ERR_ABORTED);
    }
    return 0;
}

int tcp_release(tcp_handle handle)
{
    struct tcp_connection *connection;
    int result = lookup(handle, &connection);

    if (result != 0) {
        return result;
    }
    if (connection->state != TCP_STATE_FAILED &&
        connection->state != TCP_STATE_CLOSED) {
        return connection->rx_length != 0U ? TCP_ERR_UNREAD : NET_ERR_BUSY;
    }
    crypto_clear(connection, (unsigned int)sizeof(*connection));
    return 0;
}

int tcp_deinit(void)
{
    int result;

    if (net_global_context.service_active ||
        tcp_global_context.connection.handle != TCP_INVALID_HANDLE) {
        return NET_ERR_BUSY;
    }
    if (net_global_context.transport.owner == &tcp_global_context) {
        result = net_transport_unregister(&net_global_context,
                                           &tcp_global_context);
        if (result != 0) {
            return result;
        }
    }
    crypto_clear(tcp_global_context.instance_secret,
                 sizeof(tcp_global_context.instance_secret));
    tcp_global_context.secret_ready = 0;
    tcp_global_context.disabled = 1;
    return 0;
}
