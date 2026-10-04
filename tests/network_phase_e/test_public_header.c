#include <net/tcp.h>
#include <net/tcp.h>
#include <net/net_platform.h>

typedef char tcp_handle_size[sizeof(tcp_handle) == 4U ? 1 : -1];
typedef char tcp_endpoint_size[sizeof(struct tcp_endpoint) == 8U ? 1 : -1];
typedef char tcp_config_size[sizeof(struct tcp_config) == 12U ? 1 : -1];
typedef char tcp_status_size[sizeof(struct tcp_status) == 20U ? 1 : -1];
typedef char tcp_old_errors[
    NET_ERR_INVALID == -1 && NET_ERR_STATE == -2 &&
    NET_ERR_UNAVAILABLE == -3 && NET_ERR_DEVICE == -4 &&
    NET_ERR_TIMEOUT == -5 && NET_ERR_CANCELLED == -6 &&
    NET_ERR_NO_ROUTE == -7 && NET_ERR_BUSY == -8 ? 1 : -1
];
typedef char tcp_new_errors[
    TCP_ERR_WOULD_BLOCK == -9 && TCP_ERR_REFUSED == -10 &&
    TCP_ERR_RESET == -11 && TCP_ERR_RETRIES == -12 &&
    TCP_ERR_NO_MEMORY == -13 && TCP_ERR_UNSUPPORTED == -14 &&
    TCP_ERR_CLOSED == -15 && TCP_ERR_UNREAD == -16 &&
    TCP_ERR_CLOSE_TIMEOUT == -17 && TCP_ERR_ABORTED == -18 &&
    TCP_ERR_GENERATION_EXHAUSTED == -19 && TCP_ERR_CONNECT_TIMEOUT == -20 &&
    TCP_ERR_TRANSMIT_TIMEOUT == -21 ? 1 : -1
];

int phase_e_header_probe(void)
{
    int (*defaults_fn)(struct tcp_config *);
    int (*connect_fn)(tcp_handle *, const struct tcp_endpoint *,
                       const struct tcp_config *, unsigned int);
    int (*send_fn)(tcp_handle, const void *, unsigned int, unsigned int);
    int (*recv_fn)(tcp_handle, void *, unsigned int, unsigned int);
    int (*shutdown_fn)(tcp_handle);
    int (*close_fn)(tcp_handle, unsigned int);
    int (*abort_fn)(tcp_handle);
    int (*status_fn)(tcp_handle, struct tcp_status *);
    int (*release_fn)(tcp_handle);
    int (*deinit_fn)(void);
    int (*idle_fn)(unsigned int);

    defaults_fn = tcp_config_defaults;
    connect_fn = tcp_connect;
    send_fn = tcp_send;
    recv_fn = tcp_recv;
    shutdown_fn = tcp_shutdown_write;
    close_fn = tcp_close;
    abort_fn = tcp_abort;
    status_fn = tcp_status;
    release_fn = tcp_release;
    deinit_fn = tcp_deinit;
    idle_fn = net_idle;
    return defaults_fn == 0 || connect_fn == 0 || send_fn == 0 || recv_fn == 0 ||
           shutdown_fn == 0 || close_fn == 0 || abort_fn == 0 || status_fn == 0 ||
           release_fn == 0 || deinit_fn == 0 || idle_fn == 0;
}
