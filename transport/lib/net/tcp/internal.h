#ifndef MINI_OS_TCP_INTERNAL_H
#define MINI_OS_TCP_INTERNAL_H

#include "../tcp.h"
#include "../service.h"
#include "congestion.h"
#include "flow.h"
#include "rto.h"

enum {
    TCP_INSTANCE_SECRET_BYTES = 32,
    TCP_IPV4_DEFAULT_MSS = 536
};

struct tcp_active_open {
    struct net_ipv4_addr next_hop;
    net_u32 iss;
    net_u32 irs;
    net_u32 syn_sent_ms;
    unsigned int syn_rto_ms;
    unsigned int syn_transmissions;
    net_u16 local_port;
    net_u16 peer_mss;
    net_u16 peer_window;
    net_u8 ack_pending;
    net_u8 short_syn_timeout;
};

struct tcp_stream {
    struct tcp_rto rto;
    struct tcp_congestion congestion;
    struct tcp_flow flow;
    net_u32 snd_una;
    net_u32 rcv_nxt;
    net_u32 rcv_right; /* Exclusive right edge last sent on the wire. */
    net_u32 window_seq;
    net_u32 window_ack;
    net_u32 neighbor_started_ms;
    net_u32 last_challenge_ms;
    net_u16 peer_window;
    net_u8 ack_pending;
    net_u8 neighbor_pending;
    net_u8 challenge_pending;
    net_u8 challenge_limited;
    net_u8 probe_preferred; /* A due probe already yielded to a pure ACK. */
};

struct tcp_connection {
    unsigned char tx_queue[TCP_QUEUE_CAPACITY];
    unsigned char rx_queue[TCP_QUEUE_CAPACITY];
    unsigned char secret_temporary[TCP_INSTANCE_SECRET_BYTES];
    struct tcp_endpoint endpoint;
    struct tcp_config config;
    struct tcp_active_open open;
    struct tcp_stream stream;
    tcp_handle handle;
    enum tcp_state state;
    int terminal_result;
    net_u32 started_ms;
    net_u32 close_started_ms;
    net_u32 transmit_started_ms; /* Start of the current nonempty, no-ACK interval. */
    unsigned int tx_head;
    unsigned int tx_length;
    unsigned int tx_outstanding; /* Sent bytes at the front of tx_queue. */
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
struct net_tx_deadline;

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
unsigned int tcp_ring_offset(unsigned int head, unsigned int distance);
void tcp_ring_write(net_u8 *queue, unsigned int offset, const void *data,
                     unsigned int length);
void tcp_ring_read(const net_u8 *queue, unsigned int offset, void *data,
                    unsigned int length);

void tcp_finish_neighbor(const struct tcp_connection *connection);
int tcp_resolve_neighbor(const struct tcp_connection *connection,
                         net_u32 started_ms, unsigned int timeout_ms,
                         struct net_mac_addr *mac);
int tcp_transmit(const struct tcp_connection *connection,
                 const struct net_mac_addr *mac, struct tcp_segment_view *segment,
                 unsigned int tx_offset, const struct net_tx_deadline *deadline);

int tcp_open_prepare(const struct tcp_endpoint *endpoint, net_u32 now_ms,
                      struct tcp_active_open *open);
int tcp_open_is_duplicate(const struct tcp_connection *connection,
                           const struct tcp_segment_view *segment);
int tcp_open_timer(struct tcp_connection *connection, net_u32 now_ms);
int tcp_open_input(struct tcp_connection *connection,
                    const struct tcp_segment_view *segment);
void tcp_stream_start(struct tcp_connection *connection);
int tcp_stream_transmit_expired(const struct tcp_connection *connection,
                                 net_u32 now_ms);
int tcp_stream_output(struct tcp_connection *connection, net_u32 now_ms);
int tcp_stream_after_receive(const struct tcp_connection *connection);
int tcp_stream_input(struct tcp_connection *connection,
                      const struct tcp_segment_view *segment);
unsigned int tcp_stream_write(struct tcp_connection *connection,
                               const void *data, unsigned int length, net_u32 now_ms);
unsigned int tcp_stream_read(struct tcp_connection *connection,
                              void *data, unsigned int capacity);
int tcp_connection_timer(void *owner, net_u32 now_ms);
int tcp_connection_after_receive(void *owner);
int tcp_connection_input(void *owner,
                          const struct net_ethernet_view *ethernet,
                          const struct net_ipv4_view *packet);
void tcp_connection_service_failure(void *owner, int result);

#endif
