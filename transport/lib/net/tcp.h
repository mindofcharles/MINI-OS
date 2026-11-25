#ifndef MINI_OS_TCP_H
#define MINI_OS_TCP_H

#include "net.h"

/* A value, not a TCB pointer; generations never wrap or restart in an instance. */
typedef unsigned int tcp_handle;
#define TCP_INVALID_HANDLE 0U

/* Shared net_error values -1 through -8 are unchanged. */
enum tcp_error {
    TCP_ERR_WOULD_BLOCK = -9,
    TCP_ERR_REFUSED = -10,
    TCP_ERR_RESET = -11,
    TCP_ERR_RETRIES = -12,
    TCP_ERR_NO_MEMORY = -13,
    TCP_ERR_UNSUPPORTED = -14,
    TCP_ERR_CLOSED = -15,
    TCP_ERR_UNREAD = -16,
    TCP_ERR_CLOSE_TIMEOUT = -17,
    TCP_ERR_ABORTED = -18,
    TCP_ERR_GENERATION_EXHAUSTED = -19,
    TCP_ERR_CONNECT_TIMEOUT = -20,
    TCP_ERR_TRANSMIT_TIMEOUT = -21,
    TCP_ERR_ENTROPY = -22,
    TCP_ERR_CRYPTO = -23,
    TCP_ERR_NEIGHBOR_TIMEOUT = -24
};

enum {
    TCP_QUEUE_CAPACITY = 4096,
    TCP_CONTEXT_MAX_BYTES = 12288,
    TCP_CONNECT_TIMEOUT_DEFAULT_MS = 120000,
    TCP_CLOSE_GRACE_DEFAULT_MS = 300000,
    TCP_TRANSMIT_TIMEOUT_DEFAULT_MS = 300000
};

enum tcp_state {
    TCP_STATE_NONE = 0,
    TCP_STATE_CONNECTING = 1,
    TCP_STATE_ESTABLISHED = 2,
    TCP_STATE_FIN_WAIT_1 = 3,
    TCP_STATE_FIN_WAIT_2 = 4,
    TCP_STATE_CLOSING = 5,
    TCP_STATE_CLOSE_WAIT = 6,
    TCP_STATE_LAST_ACK = 7,
    TCP_STATE_TIME_WAIT = 8,
    TCP_STATE_DRAINING = 9,
    TCP_STATE_CLOSED = 10,
    TCP_STATE_FAILED = 11
};

enum {
    TCP_STATUS_READABLE = 1,
    TCP_STATUS_PEER_EOF = 2,
    TCP_STATUS_WRITE_SHUTDOWN = 4,
    TCP_STATUS_TERMINAL = 8
};

struct tcp_endpoint {
    struct net_ipv4_addr address;
    unsigned int port;
};

struct tcp_config {
    unsigned int connect_timeout_ms;
    unsigned int close_grace_ms;
    unsigned int transmit_timeout_ms; /* No cumulative ACK progress with data queued. */
};

struct tcp_status {
    enum tcp_state state;
    int terminal_result;
    unsigned int readable_bytes;
    unsigned int writable_bytes;
    unsigned int flags;
};

/* Copies defaults; all configured resource durations are positive and bounded. */
int tcp_config_defaults(struct tcp_config *config);

/*
 * Initialize *handle to TCP_INVALID_HANDLE before a new connect.
 * config may be null for defaults; address is IPv4 and port is in 1..65535.
 * All wait_ms values are relative call budgets in 0..NET_TIMEOUT_MAX_MS.
 * Zero is one bounded service opportunity, not an implicit abort or EOF.
 * A pending handle is resumed only for the same endpoint and configuration.
 * Ordinary wait expiry preserves it; terminal causes remain until release.
 * A successful final ACK commits the connection once the driver sends it.
 * Resolving its next hop can leave the attempt pending after SYN-ACK.
 * Returns zero when connected, TCP_ERR_WOULD_BLOCK while pending at zero wait,
 * or NET_ERR_TIMEOUT when a positive call budget expires with work pending.
 * Entropy, cryptographic and allocation failures during setup precede handle
 * publication. A terminal connection error belongs to its published handle.
 */
int tcp_connect(tcp_handle *handle, const struct tcp_endpoint *endpoint,
                 const struct tcp_config *config, unsigned int wait_ms);

/*
 * Positive send means the exact accepted prefix is copied into owned storage,
 * not acknowledged delivery; caller storage is never retained.
 * A zero-length send on a usable handle returns zero without protocol work.
 * Positive partial progress takes precedence over a newly detected failure,
 * whose cause stays observable on subsequent I/O or status calls.
 * Without progress, zero wait returns TCP_ERR_WOULD_BLOCK and a positive
 * wait expiry returns NET_ERR_TIMEOUT without discarding previously queued bytes.
 * The separate transmit timeout can end a stalled connection with
 * TCP_ERR_TRANSMIT_TIMEOUT; retry exhaustion reports TCP_ERR_RETRIES.
 * Positive stream waits check the deadline before starting a copy or a new
 * raw receive/transmit. A driver operation already started may finish later.
 */
int tcp_send(tcp_handle handle, const void *data, unsigned int length,
              unsigned int wait_ms);

/*
 * Nonzero capacity and a nonnull buffer are required; packet boundaries vanish.
 * Zero is orderly EOF only after all preceding bytes have been delivered.
 * No progress at zero wait returns TCP_ERR_WOULD_BLOCK, not EOF.
 * Ordinary wait expiry preserves the connection and buffered bytes, including
 * data received by an already started poll but not yet copied to the caller.
 * Wire-level FIN and orderly EOF are not yet implemented for active connections.
 */
int tcp_recv(tcp_handle handle, void *data, unsigned int capacity,
              unsigned int wait_ms);

/*
 * Seals new sends, schedules one logical FIN, and retains receive ownership.
 * Active connections return TCP_ERR_UNSUPPORTED until wire closing is added.
 */
int tcp_shutdown_write(tcp_handle handle);

/*
 * wait_ms never sets or resets close_grace_ms, captured on first local close.
 * Zero advances once and preserves an incomplete close; ordinary call timeout
 * also preserves it.  The fixed close-grace expiry is a terminal error.
 * Unread bytes return TCP_ERR_UNREAD without silently discarding them; drain
 * receive through EOF before retrying full close, or abort explicitly.
 * Graceful success requires protocol completion and an observed drained EOF.
 * Active connections return TCP_ERR_UNSUPPORTED until wire closing is added.
 */
int tcp_close(tcp_handle handle, unsigned int wait_ms);

/* Abort terminates immediately but does not release or replace an old cause. */
int tcp_abort(tcp_handle handle);

/* Snapshot only. writable_bytes is local queue space, not peer window credit. */
int tcp_status(tcp_handle handle, struct tcp_status *status);

/* Only terminal handles can be released; old generations then become invalid. */
int tcp_release(tcp_handle handle);

/* Requires an empty slot; clears the instance secret and permanently disables. */
int tcp_deinit(void);

#endif
