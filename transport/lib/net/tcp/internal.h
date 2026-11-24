#ifndef MINI_OS_TCP_INTERNAL_H
#define MINI_OS_TCP_INTERNAL_H

#include "../tcp.h"
#include "../service.h"

enum { TCP_INSTANCE_SECRET_BYTES = 32 };

struct tcp_active_open {
    struct net_ipv4_addr next_hop;
    net_u32 iss;
    net_u32 irs;
    net_u32 syn_sent_ms;
    net_u32 ack_started_ms;
    unsigned int syn_rto_ms;
    unsigned int syn_transmissions;
    net_u16 local_port;
    net_u16 peer_mss;
    net_u16 peer_window;
    net_u8 ack_pending;
};

struct tcp_connection {
    unsigned char tx_queue[TCP_QUEUE_CAPACITY];
    unsigned char rx_queue[TCP_QUEUE_CAPACITY];
    unsigned char secret_temporary[TCP_INSTANCE_SECRET_BYTES];
    struct tcp_endpoint endpoint;
    struct tcp_config config;
    struct tcp_active_open open;
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

struct tcp_segment_view;

int tcp_config_validate(const struct tcp_config *config);
int tcp_endpoint_validate(const struct tcp_endpoint *endpoint);
struct tcp_connection *tcp_connection_lookup(tcp_handle handle);
int tcp_connection_allocate(const struct tcp_endpoint *endpoint,
                              const struct tcp_config *config,
                              net_u32 now_ms, tcp_handle *handle);
int tcp_connection_fail(int result);
int tcp_connection_fail_event(int result);
int tcp_connection_complete(void);
int tcp_connection_begin_close(net_u32 now_ms);
int tcp_connection_progress_result(unsigned int accepted, int result);
int tcp_open_prepare(const struct tcp_endpoint *endpoint, net_u32 now_ms,
                      struct tcp_active_open *open);
int tcp_open_timer(struct tcp_connection *connection, net_u32 now_ms);
int tcp_open_input(struct tcp_connection *connection,
                    const struct net_ipv4_view *packet,
                    const struct tcp_segment_view *segment);
int tcp_connection_timer(void *owner, net_u32 now_ms);
int tcp_connection_input(void *owner,
                          const struct net_ethernet_view *ethernet,
                          const struct net_ipv4_view *packet);
void tcp_connection_service_failure(void *owner, int result);

#endif
