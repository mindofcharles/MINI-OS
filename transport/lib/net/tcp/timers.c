#include "internal.h"

#include "../net_platform.h"

int tcp_connection_timer(void *owner, net_u32 now_ms)
{
    struct tcp_connection *connection;

    if (owner != &tcp_global_context) {
        return NET_ERR_STATE;
    }
    connection = &tcp_global_context.connection;

    if (connection->handle == TCP_INVALID_HANDLE ||
        connection->state == TCP_STATE_FAILED ||
        connection->state == TCP_STATE_CLOSED) {
        return 0;
    }
    if (connection->close_started &&
        net_timeout_expired(connection->close_started_ms, now_ms,
                            connection->config.close_grace_ms)) {
        return tcp_connection_fail(TCP_ERR_CLOSE_TIMEOUT);
    }
    if (connection->state == TCP_STATE_CONNECTING &&
        net_timeout_expired(connection->started_ms, now_ms,
                            connection->config.connect_timeout_ms)) {
        return tcp_connection_fail(TCP_ERR_CONNECT_TIMEOUT);
    }
    return 0;
}
