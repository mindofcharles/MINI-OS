#ifndef MINI_OS_TCP_INTERNAL_H
#define MINI_OS_TCP_INTERNAL_H

#include "../tcp.h"
#include "../service.h"

enum { TCP_INSTANCE_SECRET_BYTES = 32 };

struct tcp_connection {
    unsigned char tx_queue[TCP_QUEUE_CAPACITY];
    unsigned char rx_queue[TCP_QUEUE_CAPACITY];
    unsigned char secret_temporary[TCP_INSTANCE_SECRET_BYTES];
    struct tcp_endpoint endpoint;
    struct tcp_config config;
    tcp_handle handle;
    enum tcp_state state;
    int terminal_result;
    net_u32 started_ms;
    net_u32 close_started_ms;
    net_u32 transmit_started_ms;
    unsigned int tx_head;
    unsigned int tx_length;
    unsigned int tx_unsent;
    unsigned int rx_head;
    unsigned int rx_length;
    int close_started;
    int write_shutdown;
    int fin_pending;
    int peer_fin;
    int eof_observed;
    int wire_closed;
};

struct tcp_context {
    struct tcp_connection connection;
    unsigned char instance_secret[TCP_INSTANCE_SECRET_BYTES];
    net_u32 last_generation;
    int secret_ready;
    int disabled;
};

extern struct tcp_context tcp_global_context;

int tcp_config_validate(const struct tcp_config *config);
int tcp_endpoint_validate(const struct tcp_endpoint *endpoint);
struct tcp_connection *tcp_connection_lookup(tcp_handle handle);
int tcp_connection_allocate(const struct tcp_endpoint *endpoint,
                              const struct tcp_config *config,
                              net_u32 now_ms, tcp_handle *handle);
int tcp_connection_fail(int result);
int tcp_connection_complete(void);
int tcp_connection_begin_close(net_u32 now_ms);
int tcp_connection_progress_result(unsigned int accepted, int result);
int tcp_connection_timer(void *owner, net_u32 now_ms);
int tcp_connection_input(void *owner,
                          const struct net_ethernet_view *ethernet,
                          const struct net_ipv4_view *packet);
void tcp_connection_service_failure(void *owner, int result);

#endif
